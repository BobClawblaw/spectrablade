#include "OpenFiles.h"
#include "SampleRateConverter.h"
#include "Theme.h"

OpenFiles::OpenFiles (AudioEngine& e) : engine (e)
{
    entries.push_back ({ std::make_unique<AudioDocument>(), {} });
    engine.document.addChangeListener (this);
}

OpenFiles::~OpenFiles()
{
    engine.document.removeChangeListener (this);
}

const AudioDocument& OpenFiles::documentAt (int index) const
{
    jassert (juce::isPositiveAndBelow (index, size()));
    return index == active ? engine.document : *entries[(size_t) index].aside;
}

AudioDocument& OpenFiles::documentAt (int index)
{
    jassert (juce::isPositiveAndBelow (index, size()));
    return index == active ? engine.document : *entries[(size_t) index].aside;
}

void OpenFiles::closeAll()
{
    while (size() > 1)
        close (active == size() - 1 ? 0 : size() - 1);

    close (0);
}

juce::String OpenFiles::nameAt (int index) const
{
    const auto file = documentAt (index).getFile();
    return file == juce::File() ? juce::String ("Untitled") : file.getFileName();
}

juce::uint32 OpenFiles::addToSession (SessionState& s, int index, int track, juce::int64 position) const
{
    if (! juce::isPositiveAndBelow (index, size()) || ! documentAt (index).hasAudio())
        return 0;

    const auto& doc = documentAt (index);
    auto audio = doc.getSnapshot();

    if (s.getLength() == 0)
        s.sampleRate = doc.getSampleRate();
    else if (! juce::exactlyEqual (doc.getSampleRate(), s.sampleRate))
        audio = SampleRateConverter::convert (*audio, doc.getSampleRate(), s.sampleRate);

    const auto id = SessionEdits::addClip (s, track, doc.getFile(), std::move (audio), position);

    if (const auto where = s.findClip (id))
        s.tracks[(size_t) where->first].clips[(size_t) where->second].name = doc.getFile() != juce::File() ? doc.getFile().getFileNameWithoutExtension()
                                                                                                             : nameAt (index);

    return id;
}

int OpenFiles::indexOf (const juce::File& file) const
{
    if (file == juce::File())
        return -1;

    for (int i = 0; i < size(); ++i)
        if (documentAt (i).getFile() == file)
            return i;

    return -1;
}

bool OpenFiles::isBlank (const AudioDocument& doc) const
{
    return ! doc.hasAudio() && doc.getFile() == juce::File() && ! doc.isDirty();
}

void OpenFiles::prepareForAnother (const FilePlace& place)
{
    if (isBlank (engine.document))
        return;

    entries[(size_t) active].place = place;
    engine.swapDocument (*entries[(size_t) active].aside);   // the open file goes aside; an empty one comes in
    entries.insert (entries.begin() + active + 1, Entry { std::make_unique<AudioDocument>(), {} });
    ++active;
    sendChangeMessage();
}

std::optional<FilePlace> OpenFiles::activate (int index, const FilePlace& place)
{
    if (index == active || ! juce::isPositiveAndBelow (index, size()))
        return std::nullopt;

    entries[(size_t) active].place = place;
    engine.swapDocument (*entries[(size_t) index].aside);

    // The file that left is now in the arriving one's spare document; the leaving entry takes it,
    // and the arriving one takes the leaving entry's empty spare.
    std::swap (entries[(size_t) active].aside, entries[(size_t) index].aside);
    active = index;
    sendChangeMessage();
    return entries[(size_t) index].place;
}

std::optional<FilePlace> OpenFiles::close (int index)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return std::nullopt;

    if (index != active)
    {
        entries.erase (entries.begin() + index);

        if (index < active)
            --active;

        sendChangeMessage();
        return std::nullopt;
    }

    if (size() == 1)
    {
        AudioDocument closed;
        engine.swapDocument (closed);   // the editor is left empty; the closed file goes with this
        entries[0].place = {};
        sendChangeMessage();
        return FilePlace {};
    }

    const auto neighbour = index + 1 < size() ? index + 1 : index - 1;
    auto place = activate (neighbour, {});
    entries.erase (entries.begin() + index);

    if (index < active)
        --active;

    sendChangeMessage();
    return place;
}

juce::Array<int> OpenFiles::unsaved() const
{
    juce::Array<int> out;

    for (int i = 0; i < size(); ++i)
        if (documentAt (i).isDirty() && documentAt (i).hasAudio())
            out.add (i);

    return out;
}

//==============================================================================
namespace
{
    juce::String lengthText (juce::int64 frames, double rate)
    {
        const auto ms = rate > 0.0 ? (juce::int64) std::llround ((double) frames * 1000.0 / rate) : 0;
        return juce::String::formatted ("%d:%02d.%03d", (int) (ms / 60000), (int) ((ms / 1000) % 60), (int) (ms % 1000));
    }

    juce::String rateText (double rate)
    {
        return juce::String (rate / 1000.0, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz";
    }
}

FilesPanel::FilesPanel (OpenFiles& f) : files (f)
{
    addAndMakeVisible (openButton);
    openButton.setTooltip ("Open another file (it opens beside these)");
    openButton.onClick = [this] { if (onOpen != nullptr) onOpen(); };
    list.setRowHeight (rowHeight);
    list.setRowSelectedOnMouseDown (false);   // a click switches on mouse up, so a drag does not switch
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness (0);
    addAndMakeVisible (list);
    files.addChangeListener (this);
}

FilesPanel::~FilesPanel()
{
    files.removeChangeListener (this);
}

juce::String FilesPanel::describeRow (int row) const
{
    if (! juce::isPositiveAndBelow (row, files.size()))
        return {};

    const auto& doc = files.documentAt (row);
    auto text = files.nameAt (row) + (doc.isDirty() && doc.hasAudio() ? " *" : "");

    if (! doc.hasAudio())
        return text + " - empty";

    const auto channels = doc.getNumChannels();
    return text + " - " + lengthText (doc.getNumFrames(), doc.getSampleRate()) + ", " + rateText (doc.getSampleRate()) + ", "
           + (channels == 1 ? juce::String ("mono") : channels == 2 ? juce::String ("stereo") : juce::String (channels) + " ch")
           + ", " + juce::String (doc.getSourceBitDepth()) + "-bit";
}

void FilesPanel::clickRow (int row, bool close)
{
    if (! juce::isPositiveAndBelow (row, files.size()))
        return;

    if (close)
    {
        if (onClose != nullptr)
            onClose (row);
    }
    else if (onChoose != nullptr)
    {
        onChoose (row);
    }
}

juce::var FilesPanel::dragDescription (int row)
{
    return "openfile:" + juce::String (row);
}

int FilesPanel::rowOfDrag (const juce::var& description)
{
    const auto text = description.toString();

    if (! description.isString() || ! text.startsWith ("openfile:"))
        return -1;

    const auto rest = text.fromFirstOccurrenceOf ("openfile:", false, false);
    return rest.containsOnly ("0123456789") && rest.isNotEmpty() ? rest.getIntValue() : -1;
}

juce::var FilesPanel::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (rows.size() != 1 || ! juce::isPositiveAndBelow (rows[0], files.size()) || ! files.documentAt (rows[0]).hasAudio())
        return {};

    return dragDescription (rows[0]);
}

void FilesPanel::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    openButton.setBounds (r.removeFromTop (26).removeFromLeft (90));
    r.removeFromTop (6);
    list.setBounds (r);
}

void FilesPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);

    if (files.size() == 1 && ! files.documentAt (0).hasAudio())
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("No files open. Open some, or drop them on the editor.", list.getBounds().reduced (6), juce::Justification::centredTop, 3);
    }
}

void FilesPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, files.size()) || (files.size() == 1 && ! files.documentAt (0).hasAudio()))
        return;

    const auto isActive = row == files.getActiveIndex();
    const auto& doc = files.documentAt (row);
    auto r = juce::Rectangle<int> (width, height).reduced (0, 2);

    g.setColour (isActive ? Theme::accentDeep : Theme::raised);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);

    if (isActive)
    {
        g.setColour (Theme::accent);
        g.fillRect (r.removeFromLeft (3));
    }
    else
    {
        r.removeFromLeft (3);
    }

    r.removeFromLeft (8);
    r.removeFromRight (30);
    const auto unsaved = doc.isDirty() && doc.hasAudio();
    g.setColour (isActive ? Theme::textStrong : Theme::text);
    g.setFont (juce::FontOptions (14.0f, isActive ? juce::Font::bold : juce::Font::plain));
    g.drawText (files.nameAt (row) + (unsaved ? juce::String (" " + juce::String::charToString (0x2022)) : juce::String()),
                r.removeFromTop (r.getHeight() / 2 + 1), juce::Justification::bottomLeft, true);

    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (describeRow (row).fromFirstOccurrenceOf (" - ", false, false), r, juce::Justification::topLeft, true);

    const auto c = closeBounds (width).toFloat().reduced (5.0f);
    g.setColour (Theme::dimText);
    g.drawLine ({ c.getTopLeft(), c.getBottomRight() }, 1.4f);
    g.drawLine ({ c.getTopRight(), c.getBottomLeft() }, 1.4f);
}

void FilesPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem ("Show in Editor", [this, row] { clickRow (row, false); });
        menu.addItem ("Close " + files.nameAt (row), [this, row] { clickRow (row, true); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }

    clickRow (row, closeBounds (list.getVisibleRowWidth()).contains (e.getPosition()));
}

void FilesPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    list.updateContent();
    list.repaint();
    repaint();
}

//==============================================================================
FileTabs::FileTabs (OpenFiles& f) : files (f)
{
    files.addChangeListener (this);
}

FileTabs::~FileTabs()
{
    files.removeChangeListener (this);
}

bool FileTabs::hasTabs() const
{
    return files.size() > 1 || files.documentAt (0).hasAudio() || files.documentAt (0).getFile() != juce::File();
}

int FileTabs::tabWidth (int index) const
{
    // Each wants room for its name and its ×; when they don't all fit, they share the width.
    const auto wanted = [this] (int i)
    {
        const auto name = files.nameAt (i) + " *";
        return juce::jlimit (narrowest, widest,
                             juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (12.0f)), name) + 38);
    };

    auto total = 0;

    for (int i = 0; i < files.size(); ++i)
        total += wanted (i);

    if (total <= getWidth())
        return wanted (index);

    return juce::jmax (narrowest / 2, getWidth() / juce::jmax (1, files.size()));
}

juce::Rectangle<int> FileTabs::getTabBounds (int index) const
{
    if (! juce::isPositiveAndBelow (index, files.size()))
        return {};

    auto x = 0;

    for (int i = 0; i < index; ++i)
        x += tabWidth (i);

    return { x, 0, tabWidth (index), getHeight() };
}

juce::Rectangle<int> FileTabs::getCloseBounds (int index) const
{
    const auto tab = getTabBounds (index);
    return tab.isEmpty() ? tab : juce::Rectangle<int> (tab.getRight() - 20, tab.getCentreY() - 7, 14, 14);
}

int FileTabs::tabAt (juce::Point<int> p) const
{
    for (int i = 0; i < files.size(); ++i)
        if (getTabBounds (i).contains (p))
            return i;

    return -1;
}

void FileTabs::paint (juce::Graphics& g)
{
    g.fillAll (Theme::surface);

    if (! hasTabs())
        return;

    for (int i = 0; i < files.size(); ++i)
    {
        const auto tab = getTabBounds (i);
        const auto active = i == files.getActiveIndex();
        const auto& doc = files.documentAt (i);
        const auto unsaved = doc.isDirty() && doc.hasAudio();

        g.setColour (active ? Theme::base : i == hovered ? Theme::hover : Theme::raised);
        g.fillRect (tab.withTrimmedRight (1));

        if (active)
        {
            g.setColour (Theme::accent);
            g.fillRect (tab.withHeight (2).withTrimmedRight (1));
        }

        auto text = tab.withTrimmedLeft (10).withTrimmedRight (24);
        g.setColour (active ? Theme::textStrong : Theme::dimText);
        g.setFont (juce::FontOptions (12.0f, active ? juce::Font::bold : juce::Font::plain));
        g.drawText (files.nameAt (i), text, juce::Justification::centredLeft, true);

        // The ×, or a dot in its place while unsaved and not under the mouse.
        const auto c = getCloseBounds (i).toFloat();

        if (unsaved && i != hovered)
        {
            g.setColour (active ? Theme::text : Theme::dimText);
            g.fillEllipse (c.withSizeKeepingCentre (7.0f, 7.0f));
        }
        else if (active || i == hovered)
        {
            const auto x = c.reduced (4.0f);
            g.setColour (Theme::dimText);
            g.drawLine ({ x.getTopLeft(), x.getBottomRight() }, 1.4f);
            g.drawLine ({ x.getTopRight(), x.getBottomLeft() }, 1.4f);
        }
    }

    g.setColour (Theme::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void FileTabs::mouseDown (const juce::MouseEvent& e)
{
    pressed = tabAt (e.getPosition());
    pressedClose = pressed >= 0 && getCloseBounds (pressed).contains (e.getPosition());

    if (pressed < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        const auto index = std::exchange (pressed, -1);
        juce::PopupMenu menu;
        menu.addItem ("Close " + files.nameAt (index), [this, index] { if (onClose != nullptr) onClose (index); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }

    // A tab is shown as it is pressed; its × closes it when released over it.
    if (! pressedClose && onChoose != nullptr)
        onChoose (pressed);
}

void FileTabs::mouseUp (const juce::MouseEvent& e)
{
    const auto index = std::exchange (pressed, -1);

    if (index >= 0 && pressedClose && getCloseBounds (index).contains (e.getPosition()) && onClose != nullptr)
        onClose (index);
}

void FileTabs::mouseMove (const juce::MouseEvent& e)
{
    if (const auto over = tabAt (e.getPosition()); over != hovered)
    {
        hovered = over;
        setTooltip (over >= 0 ? files.documentAt (over).getFile().getFullPathName() : juce::String());
        repaint();
    }
}

void FileTabs::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

void FileTabs::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();

    if (auto* parent = getParentComponent())
        parent->resized();   // the tabs come and go with the files
}
