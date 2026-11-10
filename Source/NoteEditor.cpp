#include "NoteEditor.h"
#include "Instrument.h"
#include "Theme.h"

namespace
{
    constexpr int divisions[] = { 1, 2, 4, 8, 16, 32 };

    bool isBlackKey (int pitch)
    {
        const auto n = pitch % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
}

NoteEditor::NoteEditor()
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (gridLabel);
    gridLabel.setText ("Grid", juce::dontSendNotification);
    Theme::follow (gridLabel, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (gridBox);

    for (int i = 0; i < (int) std::size (divisions); ++i)
        gridBox.addItem (divisions[i] == 1 ? juce::String ("Beat") : "1/" + juce::String (divisions[i]) + " beat", i + 1);

    gridBox.addItem ("Off", (int) std::size (divisions) + 1);
    gridBox.setSelectedId (3, juce::dontSendNotification);
    gridBox.onChange = [this]
    {
        const auto id = gridBox.getSelectedId();
        gridDivision = juce::isPositiveAndBelow (id - 1, (int) std::size (divisions)) ? divisions[id - 1] : 0;
        repaint();
    };

    addAndMakeVisible (velocityLabel);
    velocityLabel.setText ("Velocity", juce::dontSendNotification);
    Theme::follow (velocityLabel, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (velocitySlider);
    velocitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    velocitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    velocitySlider.setRange (1.0, 127.0, 1.0);
    velocitySlider.setValue (100.0, juce::dontSendNotification);
    velocitySlider.onValueChange = [this]
    {
        if (! shown.has_value() || selected.empty())
            return;

        for (auto i : selected)
            if (juce::isPositiveAndBelow (i, (int) shown->clip.notes.size()))
                shown->clip.notes[(size_t) i].velocity = getVelocity();

        repaint();
    };
    velocitySlider.onDragEnd = [this]
    {
        if (shown.has_value() && ! selected.empty())
            commit ("Note Velocity");
    };

    addAndMakeVisible (status);
    status.setFont (juce::Font (juce::FontOptions (12.0f)));
    Theme::follow (status, juce::Label::textColourId, Theme::dimText);

    refreshControls();
    startTimer (100);
}

NoteEditor::~NoteEditor()
{
    stopTimer();
}

void NoteEditor::timerCallback()
{
    if (drag == Drag::none)
        refresh();
}

void NoteEditor::refresh()
{
    auto next = readClip != nullptr ? readClip() : std::nullopt;

    if (next == shown)
        return;

    const auto other = ! next.has_value() || ! shown.has_value() || next->clip.id != shown->clip.id;
    shown = std::move (next);

    if (shown.has_value())
        for (auto it = selected.begin(); it != selected.end();)
            it = *it < (int) shown->clip.notes.size() ? std::next (it) : selected.erase (it);
    else
        selected.clear();

    if (other)
    {
        selected.clear();
        scrollToNotes();
    }

    refreshControls();
    repaint();
}

void NoteEditor::refreshControls()
{
    const auto on = shown.has_value();
    gridBox.setEnabled (on);
    velocitySlider.setEnabled (on);

    if (! on)
    {
        status.setText ("Select a note clip in the multitrack (Add Notes... in a track's menu makes one).", juce::dontSendNotification);
        return;
    }

    const auto& c = shown->clip;
    juce::String text = shown->trackName + ": " + c.name + ", " + juce::String ((int) c.notes.size()) + (c.notes.size() == 1 ? " note" : " notes");

    if (c.loopLength > 0 && c.loopLength < c.length)
        text << ", repeating every " << juce::String ((double) c.loopLength / shown->sampleRate, 2) << " s";

    if (! selected.empty())
        text << "; " << juce::String ((int) selected.size()) << " selected";

    if (! shown->hasInstrument)
        text << ". The track has no instrument: Add Instrument in its FX menu.";

    status.setText (text, juce::dontSendNotification);
}

void NoteEditor::setGridDivision (int division)
{
    const auto found = std::find (std::begin (divisions), std::end (divisions), division);
    gridBox.setSelectedId (found == std::end (divisions) ? (int) std::size (divisions) + 1 : (int) (found - std::begin (divisions)) + 1, juce::sendNotificationSync);
}

juce::int64 NoteEditor::gridStepFrames() const noexcept
{
    if (! shown.has_value() || gridDivision <= 0)
        return 0;

    return juce::jmax ((juce::int64) 1, (juce::int64) std::llround (shown->beatSeconds * shown->sampleRate / gridDivision));
}

void NoteEditor::setVelocity (float velocity)
{
    velocitySlider.setValue (juce::jlimit (1.0, 127.0, (double) std::round (velocity * 127.0f)), juce::sendNotificationSync);

    if (shown.has_value() && ! selected.empty())
        commit ("Note Velocity");
}

juce::int64 NoteEditor::snapped (juce::int64 frame) const
{
    const auto step = gridStepFrames();
    return step > 0 ? (frame / step) * step : frame;
}

//==============================================================================
juce::Rectangle<int> NoteEditor::rollArea() const
{
    return getLocalBounds().withTrimmedTop (topBar).withTrimmedLeft (keyboardWidth);
}

float NoteEditor::yOf (int pitch) const
{
    return (float) rollArea().getBottom() - (float) (pitch - lowPitch + 1) * rowHeight;
}

int NoteEditor::pitchAt (float y) const
{
    return juce::jlimit (0, 127, lowPitch + (int) std::floor (((float) rollArea().getBottom() - y) / rowHeight));
}

float NoteEditor::xOf (juce::int64 frame) const
{
    const auto area = rollArea();
    const auto length = shown.has_value() ? juce::jmax ((juce::int64) 1, shown->clip.length) : (juce::int64) 1;
    return (float) area.getX() + (float) frame * (float) (area.getWidth() - 1) / (float) length;
}

juce::int64 NoteEditor::frameAt (float x) const
{
    const auto area = rollArea();
    const auto length = shown.has_value() ? juce::jmax ((juce::int64) 1, shown->clip.length) : (juce::int64) 1;
    return juce::jlimit ((juce::int64) 0, length, (juce::int64) std::llround ((x - (float) area.getX()) * (double) length / (double) juce::jmax (1, area.getWidth() - 1)));
}

juce::Rectangle<float> NoteEditor::noteBounds (const ClipNote& n) const
{
    const auto x0 = xOf (n.start), x1 = xOf (n.start + n.length);
    return { x0, yOf (n.pitch) + 1.0f, juce::jmax (3.0f, x1 - x0), (float) rowHeight - 2.0f };
}

int NoteEditor::noteAt (juce::Point<float> p) const
{
    if (! shown.has_value() || ! rollArea().toFloat().contains (p))
        return -1;

    for (auto i = (int) shown->clip.notes.size() - 1; i >= 0; --i)
        if (noteBounds (shown->clip.notes[(size_t) i]).contains (p))
            return i;

    return -1;
}

void NoteEditor::scrollToNotes()
{
    if (! shown.has_value())
        return;

    const auto rows = juce::jmax (1, rollArea().getHeight() / rowHeight);
    int low = 128, high = -1;

    for (auto& n : shown->clip.notes)
    {
        low = juce::jmin (low, n.pitch);
        high = juce::jmax (high, n.pitch);
    }

    if (high < 0)
    {
        low = 60;
        high = 60;
    }

    lowPitch = juce::jlimit (0, juce::jmax (0, 128 - rows), (low + high) / 2 - rows / 2);
}

//==============================================================================
void NoteEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
    const auto area = rollArea();
    const auto keys = juce::Rectangle<int> (0, area.getY(), keyboardWidth, area.getHeight());
    g.setColour (Theme::canvas);
    g.fillRect (area);

    if (! shown.has_value())
    {
        g.setColour (Theme::surface);
        g.fillRect (keys);
        return;
    }

    const auto& c = shown->clip;
    const auto rows = area.getHeight() / rowHeight + 1;

    // Rows: the black keys' rows darker, a line at each C.
    for (int row = 0; row < rows; ++row)
    {
        const auto pitch = lowPitch + row;

        if (pitch > 127)
            break;

        const auto y = yOf (pitch);
        const juce::Rectangle<float> r ((float) area.getX(), y, (float) area.getWidth(), (float) rowHeight);
        g.setColour (isBlackKey (pitch) ? Theme::laneAlternate : Theme::lane);
        g.fillRect (r.getIntersection (area.toFloat()));

        if (pitch % 12 == 0)
        {
            g.setColour (Theme::lineStrong);
            g.drawHorizontalLine ((int) (y + rowHeight), (float) area.getX(), (float) area.getRight());
        }

        // The keyboard: white keys light, black keys dark, each C named.
        const juce::Rectangle<float> key (0.0f, y, (float) keyboardWidth - 1.0f, (float) rowHeight);
        g.setColour (isBlackKey (pitch) ? Theme::inset : Theme::raised);
        g.fillRect (key.getIntersection (keys.toFloat()));

        if (pitch % 12 == 0 && key.getY() >= (float) area.getY())
        {
            g.setColour (Theme::text);
            g.setFont (10.0f);
            g.drawText (Instrument::pitchName (pitch), key.toNearestInt().withTrimmedLeft (3), juce::Justification::centredLeft);
        }
    }

    g.setColour (Theme::line);
    g.drawVerticalLine (keyboardWidth - 1, (float) area.getY(), (float) area.getBottom());

    // The grid: a line each step, stronger each beat; the repeats of the pattern.
    const auto step = gridStepFrames();
    const auto beat = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (shown->beatSeconds * shown->sampleRate));
    const auto period = c.loopLength > 0 ? juce::jmin (c.loopLength, c.length) : c.length;

    if (step > 0 && xOf (step) - xOf (0) >= 3.0f)
        for (juce::int64 f = 0; f <= c.length; f += step)
        {
            g.setColour (f % beat == 0 ? Theme::lineStrong : Theme::line);
            g.drawVerticalLine ((int) xOf (f), (float) area.getY(), (float) area.getBottom());
        }

    if (period > 0 && period < c.length)
        for (auto f = period; f < c.length; f += period)
        {
            g.setColour (Theme::accent.withAlpha (0.6f));
            g.drawVerticalLine ((int) xOf (f), (float) area.getY(), (float) area.getBottom());
        }

    // Past the pattern the notes don't play: washed out.
    if (period > 0 && period < c.length)
    {
        g.setColour (Theme::overlay.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (xOf (period), (float) area.getY(), xOf (c.length) - xOf (period), (float) area.getHeight()));
    }

    // The notes: velocity as their depth of colour, the selected outlined.
    for (size_t i = 0; i < c.notes.size(); ++i)
    {
        const auto& n = c.notes[i];
        const auto r = noteBounds (n);

        if (r.getBottom() < (float) area.getY() || r.getY() > (float) area.getBottom())
            continue;

        const auto playing = n.start < period && shown->hasInstrument;
        g.setColour ((playing ? Theme::accent : Theme::dimText).withAlpha (0.45f + 0.55f * juce::jlimit (0.0f, 1.0f, n.velocity)));
        g.fillRoundedRectangle (r.getIntersection (area.toFloat()), 2.0f);

        if (selected.count ((int) i) > 0)
        {
            g.setColour (Theme::textStrong);
            g.drawRoundedRectangle (r.getIntersection (area.toFloat()), 2.0f, 1.5f);
        }
    }

    if (drag == Drag::box)
    {
        g.setColour (Theme::selection.withAlpha (0.2f));
        g.fillRect (box);
        g.setColour (Theme::selection);
        g.drawRect (box, 1.0f);
    }

    g.setColour (Theme::line);
    g.drawRect (area);
}

void NoteEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (topBar).reduced (6, 4);
    gridLabel.setBounds (bar.removeFromLeft (34));
    gridBox.setBounds (bar.removeFromLeft (96));
    bar.removeFromLeft (12);
    velocityLabel.setBounds (bar.removeFromLeft (56));
    velocitySlider.setBounds (bar.removeFromLeft (juce::jmin (180, bar.getWidth())));
    bar.removeFromLeft (12);
    status.setBounds (bar);
}

//==============================================================================
void NoteEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::none;
    dragged = false;

    if (! shown.has_value() || ! rollArea().contains (e.getPosition()))
        return;

    const auto p = e.position;
    const auto hit = noteAt (p);
    base = shown;
    dragStart = p;

    if (hit >= 0)
    {
        if (e.mods.isAltDown())
        {
            shown->clip.notes.erase (shown->clip.notes.begin() + hit);
            selected.clear();
            commit ("Remove Note");
            drag = Drag::nothing;
            return;
        }

        if (e.mods.isShiftDown())
        {
            if (selected.count (hit) > 0)
                selected.erase (hit);
            else
                selected.insert (hit);

            drag = Drag::nothing;
            refreshControls();
            repaint();
            return;
        }

        if (selected.count (hit) == 0)
            selected = { hit };

        const auto& n = shown->clip.notes[(size_t) hit];
        const auto r = noteBounds (n);
        drag = p.x >= r.getRight() - juce::jmin (6.0f, r.getWidth() / 3.0f) ? Drag::resize : Drag::move;
        dragNote = hit;
        dragFrame = frameAt (p.x);
        dragPitch = pitchAt (p.y);
        velocitySlider.setValue (std::round (n.velocity * 127.0f), juce::dontSendNotification);
        refreshControls();
        repaint();
        return;
    }

    if (e.mods.isAltDown() || e.mods.isPopupMenu())
    {
        drag = Drag::nothing;
        return;
    }

    // Empty grid: a note here, or with Shift (or a drag) a box of notes.
    if (e.mods.isShiftDown())
    {
        drag = Drag::box;
        box = { p, p };
        return;
    }

    const auto step = gridStepFrames();
    ClipNote n;
    n.pitch = pitchAt (p.y);
    n.start = snapped (frameAt (p.x));
    n.length = step > 0 ? step : lastLength > 0 ? lastLength : juce::jmax ((juce::int64) 1, (juce::int64) std::llround (0.25 * shown->sampleRate));
    n.velocity = getVelocity();

    if (n.start >= shown->clip.length)
        return;

    n.length = juce::jmin (n.length, shown->clip.length - n.start);
    shown->clip.notes.push_back (n);
    selected = { (int) shown->clip.notes.size() - 1 };
    dragNote = (int) shown->clip.notes.size() - 1;
    dragFrame = n.start;
    dragPitch = n.pitch;
    drag = Drag::resize;   // dragging on sets its length
    base = shown;
    commit ("Add Note");
    base = shown;
}

void NoteEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! shown.has_value() || ! base.has_value() || drag == Drag::none || drag == Drag::nothing)
        return;

    const auto p = e.position;

    if (drag == Drag::box)
    {
        box = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (dragStart.x, p.x), juce::jmin (dragStart.y, p.y), juce::jmax (dragStart.x, p.x), juce::jmax (dragStart.y, p.y));
        selected.clear();

        for (size_t i = 0; i < shown->clip.notes.size(); ++i)
            if (noteBounds (shown->clip.notes[i]).intersects (box))
                selected.insert ((int) i);

        dragged = true;
        refreshControls();
        repaint();
        return;
    }

    if (! juce::isPositiveAndBelow (dragNote, (int) base->clip.notes.size()))
        return;

    dragged = true;
    shown->clip.notes = base->clip.notes;
    const auto length = shown->clip.length;

    if (drag == Drag::resize)
    {
        // The dragged note's end goes to the mouse (snapped); the other selected notes' lengths change by as much.
        const auto& was = base->clip.notes[(size_t) dragNote];
        const auto end = juce::jmax (was.start + 1, juce::jmin (length, snapped (frameAt (p.x) + (gridStepFrames() > 0 ? gridStepFrames() : 0))));
        const auto delta = (end - was.start) - was.length;

        for (auto i : selected)
            if (juce::isPositiveAndBelow (i, (int) shown->clip.notes.size()))
            {
                auto& n = shown->clip.notes[(size_t) i];
                n.length = juce::jlimit ((juce::int64) 1, length - n.start, n.length + delta);
            }

        lastLength = shown->clip.notes[(size_t) dragNote].length;
    }
    else
    {
        const auto semitones = pitchAt (p.y) - dragPitch;
        auto frames = snapped (frameAt (p.x)) - snapped (dragFrame);

        // Nothing moves before the clip's start or past its end.
        for (auto i : selected)
            if (juce::isPositiveAndBelow (i, (int) base->clip.notes.size()))
            {
                const auto& n = base->clip.notes[(size_t) i];
                frames = juce::jmax (frames, -n.start);
                frames = juce::jmin (frames, length - 1 - n.start);
            }

        for (auto i : selected)
            if (juce::isPositiveAndBelow (i, (int) shown->clip.notes.size()))
            {
                auto& n = shown->clip.notes[(size_t) i];
                n.pitch = juce::jlimit (0, 127, n.pitch + semitones);
                n.start += frames;
                n.length = juce::jmin (n.length, length - n.start);
            }
    }

    repaint();
}

void NoteEditor::mouseUp (const juce::MouseEvent&)
{
    const auto was = std::exchange (drag, Drag::none);

    if (was == Drag::box)
    {
        refreshControls();
        repaint();
        return;
    }

    if ((was == Drag::move || was == Drag::resize) && dragged && shown.has_value() && base.has_value() && ! (shown->clip == base->clip))
        commit (was == Drag::move ? "Move Notes" : "Note Length");

    base.reset();
    refreshControls();
    repaint();
}

void NoteEditor::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto rows = juce::jmax (1, rollArea().getHeight() / rowHeight);
    lowPitch = juce::jlimit (0, juce::jmax (0, 128 - rows), lowPitch + (int) std::round (wheel.deltaY * 12.0f));
    repaint();
}

bool NoteEditor::keyPressed (const juce::KeyPress& key)
{
    if (! shown.has_value())
        return false;

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        removeSelected();
        return true;
    }

    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
    {
        selectAll();
        return true;
    }

    const auto step = gridStepFrames() > 0 ? gridStepFrames() : juce::jmax ((juce::int64) 1, (juce::int64) std::llround (0.05 * shown->sampleRate));
    const auto shift = key.getModifiers().isShiftDown();

    if (key.getKeyCode() == juce::KeyPress::upKey)         moveSelected (shift ? 12 : 1, 0);
    else if (key.getKeyCode() == juce::KeyPress::downKey)  moveSelected (shift ? -12 : -1, 0);
    else if (key.getKeyCode() == juce::KeyPress::leftKey)  moveSelected (0, shift ? -juce::jmax ((juce::int64) 1, step / 4) : -step);
    else if (key.getKeyCode() == juce::KeyPress::rightKey) moveSelected (0, shift ? juce::jmax ((juce::int64) 1, step / 4) : step);
    else
        return false;

    return true;
}

void NoteEditor::moveSelected (int semitones, juce::int64 frames)
{
    if (! shown.has_value() || selected.empty())
        return;

    const auto length = shown->clip.length;

    for (auto i : selected)
        if (juce::isPositiveAndBelow (i, (int) shown->clip.notes.size()))
        {
            const auto& n = shown->clip.notes[(size_t) i];
            frames = juce::jmax (frames, -n.start);
            frames = juce::jmin (frames, length - 1 - n.start);
        }

    for (auto i : selected)
        if (juce::isPositiveAndBelow (i, (int) shown->clip.notes.size()))
        {
            auto& n = shown->clip.notes[(size_t) i];
            n.pitch = juce::jlimit (0, 127, n.pitch + semitones);
            n.start += frames;
            n.length = juce::jmin (n.length, length - n.start);
        }

    commit ("Move Notes");
}

void NoteEditor::selectAll()
{
    if (! shown.has_value())
        return;

    selected.clear();

    for (size_t i = 0; i < shown->clip.notes.size(); ++i)
        selected.insert ((int) i);

    refreshControls();
    repaint();
}

void NoteEditor::removeSelected()
{
    if (! shown.has_value() || selected.empty())
        return;

    std::vector<ClipNote> kept;

    for (size_t i = 0; i < shown->clip.notes.size(); ++i)
        if (selected.count ((int) i) == 0)
            kept.push_back (shown->clip.notes[i]);

    const auto count = (int) shown->clip.notes.size() - (int) kept.size();
    shown->clip.notes = std::move (kept);
    selected.clear();
    commit (count > 1 ? "Remove " + juce::String (count) + " Notes" : juce::String ("Remove Note"));
}

void NoteEditor::commit (const juce::String& description)
{
    if (shown.has_value() && apply != nullptr)
        apply (shown->clip, description);

    refreshControls();
    repaint();
}
