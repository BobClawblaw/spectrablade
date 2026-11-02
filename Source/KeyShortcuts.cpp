#include "KeyShortcuts.h"

namespace KeyShortcuts
{
void store (juce::KeyPressMappingSet& mappings, juce::PropertiesFile& settings)
{
    const auto xml = mappings.createXml (true);

    if (xml == nullptr || xml->getNumChildElements() == 0)
        settings.removeValue (settingsKey);
    else
        settings.setValue (settingsKey, xml.get());

    settings.saveIfNeeded();
}

bool restore (juce::KeyPressMappingSet& mappings, juce::PropertiesFile& settings)
{
    const auto xml = settings.getXmlValue (settingsKey);
    return xml != nullptr && xml->getNumChildElements() > 0 && mappings.restoreFromXml (*xml);
}

namespace
{
    bool sameKeys (juce::Array<juce::KeyPress> a, juce::Array<juce::KeyPress> b)
    {
        if (a.size() != b.size())
            return false;

        for (auto& k : a)
            if (! b.contains (k))
                return false;

        return true;
    }
}

juce::String describe (const juce::KeyPress& k)
{
    return k.getTextDescriptionWithIcons();
}

std::vector<Row> rows (juce::ApplicationCommandManager& commands, const juce::String& query, bool changedOnly)
{
    auto& mappings = *commands.getKeyMappings();
    juce::StringArray words;
    words.addTokens (query.toLowerCase(), " ", "\"");
    words.removeEmptyStrings();
    std::vector<Row> result;

    for (int i = 0; i < commands.getNumCommands(); ++i)
    {
        const auto* info = commands.getCommandForIndex (i);

        if (info == nullptr || (info->flags & juce::ApplicationCommandInfo::hiddenFromKeyEditor) != 0)
            continue;

        Row r;
        r.id = info->commandID;
        r.name = info->shortName;
        r.menu = info->categoryName;
        r.description = info->description;
        r.keys = mappings.getKeyPressesAssignedToCommand (r.id);
        r.changed = ! sameKeys (r.keys, info->defaultKeypresses);
        r.readOnly = (info->flags & juce::ApplicationCommandInfo::readOnlyInKeyEditor) != 0;

        if (changedOnly && ! r.changed)
            continue;

        auto text = (r.name + " " + r.menu + " " + r.description).toLowerCase();

        for (auto& k : r.keys)
            text << " " << k.getTextDescription().toLowerCase() << " " << describe (k).toLowerCase();

        auto all = true;

        for (auto& w : words)
            all = all && text.contains (w);

        if (all)
            result.push_back (std::move (r));
    }

    std::stable_sort (result.begin(), result.end(), [] (const Row& a, const Row& b)
    {
        const auto m = a.menu.compareNatural (b.menu);
        return m != 0 ? m < 0 : a.name.compareNatural (b.name) < 0;
    });

    return result;
}

juce::StringArray applySet (juce::ApplicationCommandManager& commands, const KeySet& set)
{
    auto& mappings = *commands.getKeyMappings();
    mappings.resetToDefaultMappings();
    juce::StringArray taken;

    for (auto& [id, key] : set.keys)
        if (commands.getCommandForID (id) != nullptr)
            if (const auto from = assign (mappings, id, key); from != 0)
                taken.add (describe (key) + " (from " + commands.getNameOfCommand (from) + ")");

    return taken;
}

juce::CommandID assign (juce::KeyPressMappingSet& mappings, juce::CommandID id, const juce::KeyPress& key, int replaceIndex)
{
    if (! key.isValid())
        return 0;

    const auto previous = mappings.findCommandForKeyPress (key);
    auto taken = previous != 0 && previous != id ? previous : 0;

    if (previous == id)
    {
        // Already this command's: moving it into the replaced slot just drops the old key there.
        const auto keys = mappings.getKeyPressesAssignedToCommand (id);

        if (replaceIndex >= 0 && replaceIndex < keys.size() && keys[replaceIndex] != key)
            mappings.removeKeyPress (id, replaceIndex);

        return 0;
    }

    if (taken != 0)
        mappings.removeKeyPress (key);

    const auto keys = mappings.getKeyPressesAssignedToCommand (id);

    if (replaceIndex >= 0 && replaceIndex < keys.size())
    {
        mappings.removeKeyPress (id, replaceIndex);
        mappings.addKeyPress (id, key, replaceIndex);
    }
    else
    {
        mappings.addKeyPress (id, key);
    }

    return taken;
}

juce::Array<juce::KeyPress> resetCommand (juce::ApplicationCommandManager& commands, juce::CommandID id)
{
    juce::Array<juce::KeyPress> keptElsewhere;
    auto& mappings = *commands.getKeyMappings();
    const auto* info = commands.getCommandForID (id);

    if (info == nullptr)
        return keptElsewhere;

    mappings.clearAllKeyPresses (id);

    for (auto& k : info->defaultKeypresses)
    {
        const auto other = mappings.findCommandForKeyPress (k);

        if (other != 0 && other != id)
            keptElsewhere.add (k);
        else
            mappings.addKeyPress (id, k);
    }

    return keptElsewhere;
}

juce::Result exportTo (juce::KeyPressMappingSet& mappings, const juce::File& file)
{
    const auto xml = mappings.createXml (true);

    if (xml == nullptr || ! xml->writeTo (file))
        return juce::Result::fail ("The shortcuts couldn't be written to " + file.getFullPathName() + ".");

    return juce::Result::ok();
}

juce::Result importFrom (juce::KeyPressMappingSet& mappings, const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);

    if (xml == nullptr || ! xml->hasTagName ("KEYMAPPINGS"))
        return juce::Result::fail (file.getFileName() + " isn't a set of keyboard shortcuts.");

    // A set always counts from the defaults, so what it doesn't change comes out as the defaults.
    auto fromDefaults = *xml;
    fromDefaults.setAttribute ("basedOnDefaults", true);

    if (! mappings.restoreFromXml (fromDefaults))
        return juce::Result::fail (file.getFileName() + " couldn't be read.");

    return juce::Result::ok();
}

//==============================================================================
/** Asks for a key: shows what was pressed and which command has it now, until Assign or Cancel. */
class Editor::KeyEntry final : public juce::AlertWindow
{
public:
    KeyEntry (juce::KeyPressMappingSet& m, juce::CommandID forCommand, const juce::String& commandName, juce::Component* over)
        : AlertWindow ("Set Key", "Press the keys for " + commandName.quoted() + ".", juce::MessageBoxIconType::NoIcon, over),
          mappings (m), id (forCommand), name (commandName)
    {
        // Buttons without keys of their own, so Return, Escape and the rest can be chosen too.
        addButton ("Assign", 1);
        addButton ("Cancel", 0);
        getButton ("Assign")->setEnabled (false);
        setWantsKeyboardFocus (true);
        grabKeyboardFocus();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        pressed = key;
        auto message = "Press the keys for " + name.quoted() + ".\n\n" + describe (key);
        const auto owner = mappings.findCommandForKeyPress (key);

        if (owner == id)
            message << " is already its key.";
        else if (owner != 0)
            message << " is used by " << mappings.getCommandManager().getNameOfCommand (owner).quoted() << ", which would lose it.";

        setMessage (message);
        getButton ("Assign")->setEnabled (owner != id);
        return true;
    }

    juce::KeyPress pressed;

private:
    juce::KeyPressMappingSet& mappings;
    juce::CommandID id;
    juce::String name;
};

//==============================================================================
Editor::Editor (juce::ApplicationCommandManager& c, std::vector<KeySet> s) : commands (c), sets (std::move (s))
{
    search.setTextToShowWhenEmpty ("Search commands or keys", juce::Colours::grey);
    search.onTextChange = [this] { refresh(); };
    search.onEscapeKey = [this] { search.clear(); refresh(); };
    addAndMakeVisible (search);

    changedOnly.onClick = [this] { refresh(); };
    addAndMakeVisible (changedOnly);

    list.setRowHeight (24);
    addAndMakeVisible (list);

    title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    description.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    description.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);
    addAndMakeVisible (description);

    addKey.onClick = [this] { askForKey (-1); };
    resetOne.onClick = [this]
    {
        const auto kept = resetCommand (commands, chosen);

        if (! kept.isEmpty())
        {
            juce::StringArray names;

            for (auto& k : kept)
                names.add (describe (k) + " (" + commands.getNameOfCommand (commands.getKeyMappings()->findCommandForKeyPress (k)) + ")");

            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Reset",
                                                    "These default keys are used by other commands now and stay with them: "
                                                        + names.joinIntoString (", ") + ".");
        }
    };
    importButton.onClick = [this] { importSet(); };
    exportButton.onClick = [this] { exportSet(); };
    resetAll.onClick = [this]
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Reset All Shortcuts",
                                            "Put every command's keys back to the defaults?", "Reset All", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Editor> (this)] (int result)
        {
            if (safe != nullptr && result == 1)
                safe->commands.getKeyMappings()->resetToDefaultMappings();
        }));
    };

    for (auto* b : { &addKey, &resetOne, &importButton, &exportButton, &resetAll })
        addAndMakeVisible (b);

    setsButton.onClick = [this] { chooseSet(); };
    addChildComponent (setsButton);
    setsButton.setVisible (! sets.empty());

    commands.getKeyMappings()->addChangeListener (this);
    refresh();
    setSize (640, 600);
}

Editor::~Editor()
{
    commands.getKeyMappings()->removeChangeListener (this);
}

void Editor::refresh()
{
    shown = rows (commands, search.getText(), changedOnly.getToggleState());
    list.updateContent();
    auto row = -1;

    for (size_t i = 0; i < shown.size(); ++i)
        if (shown[i].id == chosen)
            row = (int) i;

    if (row >= 0)
        list.selectRow (row, true, true);
    else
        list.deselectAllRows();

    list.repaint();
    showDetails();
}

void Editor::choose (juce::CommandID id)
{
    chosen = id;
    refresh();
}

void Editor::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) shown.size())
        return;

    const auto& r = shown[(size_t) row];
    auto& lf = getLookAndFeel();

    if (selected)
        g.fillAll (lf.findColour (juce::TextEditor::highlightColourId));
    else if (row % 2 == 1)
        g.fillAll (lf.findColour (juce::ListBox::backgroundColourId).brighter (0.04f));

    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (8, 0);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (juce::Colours::grey);
    g.drawText (r.menu, area.removeFromLeft (90), juce::Justification::centredLeft, true);

    juce::StringArray keys;

    for (auto& k : r.keys)
        keys.add (describe (k));

    const auto keyText = keys.isEmpty() ? juce::String ("no key") : keys.joinIntoString ("   ");
    auto keyArea = area.removeFromRight (juce::jmin (220, width / 3));
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (keys.isEmpty() ? juce::Colours::grey : (r.changed ? juce::Colour (0xffffc857) : lf.findColour (juce::ListBox::textColourId)));
    g.drawText (keyText, keyArea, juce::Justification::centredRight, true);

    g.setColour (lf.findColour (juce::ListBox::textColourId));
    g.drawText (r.name + (r.changed ? "  *" : ""), area, juce::Justification::centredLeft, true);
}

void Editor::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected < (int) shown.size())
        chosen = shown[(size_t) lastRowSelected].id;

    showDetails();
}

void Editor::showDetails()
{
    keyButtons.clear();
    const auto* info = commands.getCommandForID (chosen);
    const auto readOnly = info == nullptr || (info->flags & juce::ApplicationCommandInfo::readOnlyInKeyEditor) != 0;

    title.setText (info != nullptr ? info->shortName + "  (" + info->categoryName + ")" : "Choose a command to change its keys.", juce::dontSendNotification);
    description.setText (info != nullptr ? info->description : juce::String(), juce::dontSendNotification);

    if (info != nullptr)
    {
        const auto keys = commands.getKeyMappings()->getKeyPressesAssignedToCommand (chosen);

        for (int i = 0; i < keys.size(); ++i)
        {
            auto* b = keyButtons.add (new juce::TextButton (describe (keys[i])));
            b->setTooltip ("Change or remove this key");
            b->setEnabled (! readOnly);
            b->onClick = [this, i, b]
            {
                juce::PopupMenu m;
                m.addItem ("Change...", [this, i] { askForKey (i); });
                m.addItem ("Remove", [this, i]
                {
                    commands.getKeyMappings()->removeKeyPress (chosen, i);
                });
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (b));
            };
            addAndMakeVisible (b);
        }
    }

    addKey.setEnabled (! readOnly);
    resetOne.setEnabled (! readOnly && std::any_of (shown.begin(), shown.end(), [this] (const Row& r) { return r.id == chosen && r.changed; }));
    resized();
}

void Editor::askForKey (int replaceIndex)
{
    if (commands.getCommandForID (chosen) == nullptr)
        return;

    auto* box = new KeyEntry (*commands.getKeyMappings(), chosen, commands.getNameOfCommand (chosen), this);
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Editor> (this), box, replaceIndex,
                                                                      id = chosen] (int result)
    {
        if (safe == nullptr || result != 1 || ! box->pressed.isValid())
            return;

        assign (*safe->commands.getKeyMappings(), id, box->pressed, replaceIndex);
    }), true);
}

void Editor::importSet()
{
    chooser = std::make_unique<juce::FileChooser> ("Import Shortcuts", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   juce::String ("*") + fileSuffix);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<Editor> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        if (const auto r = importFrom (*safe->commands.getKeyMappings(), fc.getResult()); r.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import Shortcuts", r.getErrorMessage());
    });
}

void Editor::exportSet()
{
    chooser = std::make_unique<juce::FileChooser> ("Export Shortcuts",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (juce::String ("Shortcuts") + fileSuffix),
                                                   juce::String ("*") + fileSuffix);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<Editor> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        if (const auto r = exportTo (*safe->commands.getKeyMappings(), fc.getResult().withFileExtension (fileSuffix)); r.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export Shortcuts", r.getErrorMessage());
    });
}

void Editor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void Editor::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (28);
    changedOnly.setBounds (top.removeFromRight (120));
    top.removeFromRight (8);
    search.setBounds (top);
    r.removeFromTop (8);

    auto bottom = r.removeFromBottom (28);
    resetAll.setBounds (bottom.removeFromRight (100));
    bottom.removeFromRight (6);
    exportButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    importButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    setsButton.setBounds (bottom.removeFromRight (100));
    r.removeFromBottom (10);

    auto details = r.removeFromBottom (92);
    r.removeFromBottom (8);
    list.setBounds (r);

    title.setBounds (details.removeFromTop (24));
    description.setBounds (details.removeFromTop (22));
    details.removeFromTop (6);
    auto keysRow = details.removeFromTop (28);
    resetOne.setBounds (keysRow.removeFromRight (70));
    keysRow.removeFromRight (6);
    addKey.setBounds (keysRow.removeFromRight (90));
    keysRow.removeFromRight (10);

    for (auto* b : keyButtons)
    {
        const auto w = juce::jmin (keysRow.getWidth(), b->getBestWidthForHeight (28) + 16);
        b->setBounds (keysRow.removeFromLeft (w));
        keysRow.removeFromLeft (6);
    }
}

void Editor::chooseSet()
{
    juce::PopupMenu menu;

    for (size_t i = 0; i < sets.size(); ++i)
        menu.addItem ((int) i + 1, sets[i].name + "...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (setsButton),
                        [safe = juce::Component::SafePointer<Editor> (this)] (int picked)
                        {
                            if (safe != nullptr && picked > 0)
                                safe->offerSet ((size_t) picked - 1);
                        });
}

void Editor::offerSet (size_t index)
{
    if (index >= sets.size())
        return;

    const auto& set = sets[index];
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, set.name,
                                        set.about + "\n\nThis puts every command's keys back to the defaults first, replacing your own changes "
                                                    "(export them first to keep them).",
                                        "Use " + set.name, "Cancel", this,
                                        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Editor> (this), index] (int result)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto taken = applySet (safe->commands, safe->sets[index]);

        if (! taken.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, safe->sets[index].name,
                                                    "These keys were moved from other commands: " + taken.joinIntoString (", ") + ".");
    }));
}
}
