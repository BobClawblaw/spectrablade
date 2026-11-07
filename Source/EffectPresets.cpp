#include "EffectPresets.h"

#include <sstream>

namespace EffectPresets
{
namespace
{
    /** 17 significant digits, which reads back as the same double. */
    juce::String exactly (double v)
    {
        std::ostringstream s;
        s.imbue (std::locale::classic());
        s.precision (17);
        s << v;
        return s.str();
    }

    double parse (const juce::String& text)
    {
        std::istringstream s (text.toStdString());
        s.imbue (std::locale::classic());
        double v = 0.0;
        s >> v;
        return v;
    }
}

Store::Store (juce::File f) : folder (std::move (f)) {}

juce::File Store::fileFor (const juce::String& effectKey) const
{
    // "spectrablade:compressor" keeps its presets in compressor.xml.
    return folder.getChildFile (juce::File::createLegalFileName (effectKey.fromLastOccurrenceOf (":", false, false)) + ".xml");
}

std::unique_ptr<juce::XmlElement> Store::read (const juce::String& effectKey) const
{
    auto xml = juce::XmlDocument::parse (fileFor (effectKey));

    if (xml == nullptr || ! xml->hasTagName ("SPECTRABLADE_PRESETS") || xml->getStringAttribute ("effect") != effectKey)
        return std::make_unique<juce::XmlElement> ("SPECTRABLADE_PRESETS");

    return xml;
}

juce::StringArray Store::getNames (const juce::String& effectKey) const
{
    juce::StringArray names;
    const auto xml = read (effectKey);

    for (auto* p : xml->getChildWithTagNameIterator ("PRESET"))
        names.addIfNotAlreadyThere (p->getStringAttribute ("name"));

    names.removeEmptyStrings();
    names.sortNatural();
    return names;
}

std::optional<Values> Store::load (const juce::String& effectKey, const juce::String& name) const
{
    const auto xml = read (effectKey);

    for (auto* p : xml->getChildWithTagNameIterator ("PRESET"))
    {
        if (p->getStringAttribute ("name") != name)
            continue;

        Values v;

        for (auto* value : p->getChildWithTagNameIterator ("VALUE"))
            v[value->getStringAttribute ("id")] = parse (value->getStringAttribute ("value"));

        return v;
    }

    return std::nullopt;
}

bool Store::save (const juce::String& effectKey, const juce::String& name, const Values& values)
{
    if (name.trim().isEmpty())
        return false;

    auto xml = read (effectKey);
    xml->setAttribute ("effect", effectKey);

    if (auto* old = xml->getChildByAttribute ("name", name))
        xml->removeChildElement (old, true);

    auto* p = xml->createNewChildElement ("PRESET");
    p->setAttribute ("name", name);

    for (auto& [id, v] : values)
    {
        auto* e = p->createNewChildElement ("VALUE");
        e->setAttribute ("id", id);
        e->setAttribute ("value", exactly (v));
    }

    return folder.createDirectory().wasOk() && xml->writeTo (fileFor (effectKey));
}

bool Store::remove (const juce::String& effectKey, const juce::String& name)
{
    auto xml = read (effectKey);
    auto* old = xml->getChildByAttribute ("name", name);

    if (old == nullptr)
        return false;

    xml->removeChildElement (old, true);
    return xml->writeTo (fileFor (effectKey));
}

Store& userStore()
{
    static Store store (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("SpectraBlade").getChildFile ("Presets"));
    return store;
}

//==============================================================================
Bar::Bar (Store& s, juce::String effectKey, std::function<Values()> current, std::function<void (const Values&)> apply)
    : store (s), key (std::move (effectKey)), currentValues (std::move (current)), applyValues (std::move (apply))
{
    list.setTextWhenNothingSelected ("Presets");
    list.setTextWhenNoChoicesAvailable ("No presets saved");
    list.onChange = [this]
    {
        const auto id = list.getSelectedId();
        remove.setEnabled (id > userBase);

        if (id > userBase)
        {
            if (const auto v = store.load (key, list.getText()))
                applyValues (*v);
        }
        else if (id > 0)
        {
            const auto& made = factory (key);

            if (juce::isPositiveAndNotGreaterThan (id, (int) made.size()))
                applyValues (made[(size_t) id - 1].values);
        }
    };
    save.onClick = [this] { saveAs(); };
    remove.onClick = [this] { removeChosen(); };
    addAndMakeVisible (list);
    addAndMakeVisible (save);
    addAndMakeVisible (remove);
    refresh();
    setSize (320, 28);
}

Bar::~Bar() = default;

void Bar::refresh()
{
    const auto chosenId = list.getSelectedId();
    const auto chosen = list.getText();
    list.clear (juce::dontSendNotification);
    const auto& made = factory (key);
    const auto names = store.getNames (key);

    if (! made.empty())
    {
        list.addSectionHeading ("Factory");

        for (size_t i = 0; i < made.size(); ++i)
            list.addItem (made[i].name, (int) i + 1);
    }

    if (! names.isEmpty())
    {
        if (! made.empty())
            list.addSectionHeading ("Yours");

        for (int i = 0; i < names.size(); ++i)
            list.addItem (names[i], userBase + i + 1);
    }

    // The same one chosen again: a factory preset by its place, one of the user's by its name.
    if (chosenId > 0 && chosenId <= userBase && chosenId <= (int) made.size())
        list.setSelectedId (chosenId, juce::dontSendNotification);
    else if (names.contains (chosen))
        list.setSelectedId (userBase + names.indexOf (chosen) + 1, juce::dontSendNotification);

    remove.setEnabled (list.getSelectedId() > userBase);
}

void Bar::saveAs()
{
    auto* box = new juce::AlertWindow ("Save Preset", "A name for these settings (an existing name is replaced).", juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("name", list.getText(), "Name");
    box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Bar> (this), box] (int result)
    {
        const auto name = box->getTextEditorContents ("name").trim();

        if (safe == nullptr || result != 1 || name.isEmpty())
            return;

        if (! safe->store.save (safe->key, name, safe->currentValues()))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save Preset",
                                                    "The preset couldn't be written to " + safe->store.getFolder().getFullPathName() + ".");
            return;
        }

        safe->list.setSelectedId (0, juce::dontSendNotification);
        safe->refresh();
        safe->list.setSelectedId (userBase + safe->store.getNames (safe->key).indexOf (name) + 1, juce::dontSendNotification);
        safe->remove.setEnabled (true);
    }), true);
}

void Bar::removeChosen()
{
    const auto name = list.getText();

    if (list.getSelectedId() <= userBase || ! store.getNames (key).contains (name))
        return;

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete Preset", "Delete the preset " + name.quoted() + "?",
                                        "Delete", "Cancel", this,
                                        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Bar> (this), name] (int result)
    {
        if (safe == nullptr || result != 1)
            return;

        safe->store.remove (safe->key, name);
        safe->list.setSelectedId (0, juce::dontSendNotification);
        safe->refresh();
    }));
}

void Bar::resized()
{
    auto r = getLocalBounds();
    remove.setBounds (r.removeFromRight (64));
    r.removeFromRight (4);
    save.setBounds (r.removeFromRight (70));
    r.removeFromRight (6);
    list.setBounds (r);
}
}
