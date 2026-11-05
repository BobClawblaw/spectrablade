#include "OpenFiles.h"
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

juce::String OpenFiles::nameAt (int index) const
{
    const auto file = documentAt (index).getFile();
    return file == juce::File() ? juce::String ("Untitled") : file.getFileName();
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
    g.setColour (isActive ? juce::Colours::white : Theme::text);
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
