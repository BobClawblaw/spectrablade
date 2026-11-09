#include "PitchEditor.h"
#include "Platform.h"
#include "Theme.h"

namespace
{
    constexpr int barHeight = 32, labelWidth = 40;

    juce::String noteName (int note)
    {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((note % 12) + 12) % 12]) + juce::String (note / 12 - 1);
    }

    bool isBlackKey (int note)
    {
        const auto step = ((note % 12) + 12) % 12;
        return step == 1 || step == 3 || step == 6 || step == 8 || step == 10;
    }
}

PitchEditor::PitchEditor()
{
    for (auto* b : { &analyseButton, &snapButton, &resetButton, &applyButton })
        addAndMakeVisible (b);

    addAndMakeVisible (status);
    status.setColour (juce::Label::textColourId, Theme::dimText);
    status.setText ("Select up to 30 s of one voice or instrument, then Analyse.", juce::dontSendNotification);

    analyseButton.onClick = [this] { analyse(); };
    snapButton.onClick = [this]
    {
        notes = PitchCorrection::snapped (std::move (notes));
        repaint();
    };
    resetButton.onClick = [this]
    {
        for (auto& n : notes)
        {
            n.shiftCents = 0.0;
            n.moveFrames = 0;
        }

        refreshTimes();
        repaint();
    };
    applyButton.onClick = [this]
    {
        if (! source.has_value() || apply == nullptr)
            return;

        if (readVersion != nullptr && readVersion() != source->version)
        {
            clear ("The audio changed since it was analysed: Analyse again.");
            return;
        }

        apply (*source, track, notes);
    };

    refreshButtons();
}

void PitchEditor::refreshTimes()
{
    const auto map = source.has_value() ? PitchCorrection::timeMapFor (track, source->start, source->end, notes) : std::nullopt;
    times = map.value_or (PitchCorrection::TimeMap {});
}

void PitchEditor::clear (const juce::String& message)
{
    source.reset();
    track = {};
    notes.clear();
    times = {};
    dragging = -1;
    status.setText (message, juce::dontSendNotification);
    refreshButtons();
    repaint();
}

void PitchEditor::analyse()
{
    const auto got = readSource != nullptr ? readSource() : std::nullopt;

    if (! got.has_value() || got->audio == nullptr || got->end <= got->start)
    {
        clear ("Open a file and select what to analyse, or select a clip in a session (unlocked).");
        return;
    }

    if ((double) (got->end - got->start) / got->sampleRate > longestSeconds)
    {
        clear ("Select 30 s or less to analyse (of a longer clip, select a time range across it).");
        return;
    }

    track = PitchCorrection::analyse (*got->audio, got->start, got->end, got->sampleRate);
    notes = PitchCorrection::findNotes (track);
    source = got;
    dragging = -1;
    times = {};

    if (notes.empty())
    {
        clear ("No notes found: what was analysed has no clear pitch.");
        return;
    }

    auto lowest = notes.front().pitch, highest = lowest;

    for (auto& n : notes)
    {
        lowest = juce::jmin (lowest, n.pitch);
        highest = juce::jmax (highest, n.pitch);
    }

    lowNote = std::floor (lowest) - 4.0;
    highNote = std::ceil (highest) + 4.0;
    status.setText (juce::String ((int) notes.size()) + (notes.size() == 1 ? " note" : " notes")
                        + Platform::keys (". Drag a note up or down (Option: in cents) or sideways in time; double-click to snap it, Cmd-click to split it, right-click to join."),
                    juce::dontSendNotification);
    refreshButtons();
    repaint();
}

void PitchEditor::refreshButtons()
{
    const auto have = source.has_value() && ! notes.empty();
    snapButton.setEnabled (have);
    resetButton.setEnabled (have);
    applyButton.setEnabled (have);
}

void PitchEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (barHeight).reduced (4, 4);

    for (auto* b : { &analyseButton, &snapButton, &resetButton, &applyButton })
    {
        b->setBounds (bar.removeFromLeft (b == &snapButton ? 110 : 76));
        bar.removeFromLeft (4);
    }

    status.setBounds (bar);
}

juce::Rectangle<int> PitchEditor::rollArea() const
{
    return getLocalBounds().withTrimmedTop (barHeight).withTrimmedLeft (labelWidth).reduced (0, 2);
}

float PitchEditor::xOf (double frame) const
{
    const auto area = rollArea();
    const auto frames = juce::jmax (1.0, (double) track.hz.size());
    return (float) area.getX() + (float) (frame / frames) * (float) area.getWidth();
}

float PitchEditor::yOf (double note) const
{
    const auto area = rollArea();
    return (float) area.getBottom() - (float) ((note - lowNote) / (highNote - lowNote)) * (float) area.getHeight();
}

double PitchEditor::pitchAt (float y) const
{
    const auto area = rollArea();
    return lowNote + (double) ((float) area.getBottom() - y) / (double) area.getHeight() * (highNote - lowNote);
}

juce::Point<float> PitchEditor::centreOf (int note) const
{
    if (note < 0 || note >= (int) notes.size())
        return {};

    const auto& n = notes[(size_t) note];
    return { 0.5f * (xOf (times.toOutput (n.first)) + xOf (times.toOutput (n.end))), yOf (n.pitch + n.shiftCents / 100.0) };
}

int PitchEditor::noteAt (juce::Point<float> p) const
{
    const auto half = 0.5f * (yOf (0.0) - yOf (1.0));

    for (int k = 0; k < (int) notes.size(); ++k)
    {
        const auto& n = notes[(size_t) k];
        const auto y = yOf (n.pitch + n.shiftCents / 100.0);

        if (p.x >= xOf (times.toOutput (n.first)) && p.x < xOf (times.toOutput (n.end)) && std::abs (p.y - y) <= juce::jmax (4.0f, half))
            return k;
    }

    return -1;
}

void PitchEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
    const auto area = rollArea();
    g.setColour (Theme::canvas);
    g.fillRect (area);

    // The semitones: black keys darker, a line under each C; every note named where the rows
    // are tall enough, else each C.
    const auto rowHeight = yOf (0.0) - yOf (1.0);
    g.saveState();
    g.reduceClipRegion (getLocalBounds().withTrimmedTop (area.getY()).withBottom (area.getBottom()));

    for (auto note = (int) std::ceil (lowNote); note <= (int) std::floor (highNote); ++note)
    {
        const auto top = yOf (note + 0.5), bottom = yOf (note - 0.5);

        if (isBlackKey (note))
        {
            g.setColour (Theme::laneAlternate);
            g.fillRect (juce::Rectangle<float> ((float) area.getX(), top, (float) area.getWidth(), bottom - top).getIntersection (area.toFloat()));
        }

        if (note % 12 == 0)
        {
            g.setColour (Theme::line);
            g.drawHorizontalLine ((int) bottom, (float) area.getX(), (float) area.getRight());
        }

        if (note % 12 == 0 || rowHeight >= 13.0f)
        {
            g.setColour (note % 12 == 0 ? Theme::text : Theme::dimText);
            g.setFont (juce::jmin (11.0f, rowHeight - 1.0f));
            g.drawText (noteName (note), 2, (int) top, labelWidth - 6, (int) (bottom - top), juce::Justification::centredRight);
        }
    }

    g.restoreState();

    if (! source.has_value())
        return;

    g.saveState();
    g.reduceClipRegion (area);

    // The pitch as it is (faint) and as it will be.
    const auto shifts = PitchCorrection::shiftsFor (track, notes);

    for (auto moved : { false, true })
    {
        juce::Path path;
        auto drawing = false;

        for (int f = 0; f < (int) track.hz.size(); ++f)
        {
            const auto hz = (double) track.hz[(size_t) f];

            if (hz <= 0.0)
            {
                drawing = false;
                continue;
            }

            const auto note = 69.0 + 12.0 * std::log2 (hz / 440.0) + (moved ? shifts[(size_t) f] / 100.0 : 0.0);
            const juce::Point<float> p (xOf (moved ? times.toOutput (f + 0.5) : f + 0.5), yOf (note));

            if (drawing)
                path.lineTo (p);
            else
                path.startNewSubPath (p);

            drawing = true;
        }

        g.setColour (moved ? Theme::accentLight : Theme::faintText);
        g.strokePath (path, juce::PathStrokeType (moved ? 1.6f : 1.0f));
    }

    // The notes, where they will be.
    for (int k = 0; k < (int) notes.size(); ++k)
    {
        const auto& n = notes[(size_t) k];
        const auto y = yOf (n.pitch + n.shiftCents / 100.0);
        const auto h = juce::jmax (6.0f, 0.7f * (yOf (0.0) - yOf (1.0)));
        const auto left = xOf (times.toOutput (n.first)), right = xOf (times.toOutput (n.end));
        const juce::Rectangle<float> box (left, y - 0.5f * h, juce::jmax (2.0f, right - left), h);
        g.setColour ((k == dragging ? Theme::accent : Theme::accentDeep).withAlpha (0.55f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        if (std::abs (n.shiftCents) >= 0.5 && box.getWidth() > 40.0f)
        {
            g.setColour (Theme::textStrong);
            g.setFont (10.0f);
            g.drawText ((n.shiftCents > 0.0 ? "+" : "") + juce::String (juce::roundToInt (n.shiftCents)), box.toNearestInt(), juce::Justification::centred);
        }
    }

    g.restoreState();
}

int PitchEditor::frameAt (float x) const
{
    const auto area = rollArea();
    const auto frames = juce::jmax (1.0, (double) track.hz.size());
    return (int) std::lround ((double) (x - (float) area.getX()) / (double) juce::jmax (1, area.getWidth()) * frames);
}

bool PitchEditor::splitAt (juce::Point<float> p)
{
    const auto k = noteAt (p);

    if (k < 0)
        return false;

    // Cut where it is shown; the second piece is moved to start there, as it is drawn now.
    const auto at = (int) std::lround (times.toInput (frameAt (p.x)));
    const auto before = notes.size();
    notes = PitchCorrection::splitNote (track, std::move (notes), k, at);

    if (notes.size() == before)
        return false;

    notes[(size_t) k + 1].moveFrames = (int) std::lround (times.toOutput (at)) - at;
    refreshTimes();
    repaint();
    return true;
}

bool PitchEditor::joinWithNext (int index)
{
    if (! PitchCorrection::canJoin (notes, index))
        return false;

    const auto kept = notes;
    notes = PitchCorrection::joinNotes (track, std::move (notes), index);

    // A join that would move the second's end where the time map can't put it isn't made.
    if (source.has_value() && ! PitchCorrection::timeMapFor (track, source->start, source->end, notes).has_value())
    {
        notes = kept;
        return false;
    }

    refreshTimes();
    repaint();
    return true;
}

juce::PopupMenu PitchEditor::menuAt (juce::Point<float> p)
{
    juce::PopupMenu menu;
    const auto k = noteAt (p);

    if (k < 0)
        return menu;

    const auto at = (int) std::lround (times.toInput (frameAt (p.x)));
    const auto& n = notes[(size_t) k];
    const auto splits = at - n.first >= PitchCorrection::shortestNote && n.end - at >= PitchCorrection::shortestNote;
    menu.addItem ("Split Here", splits, false, [this, p] { splitAt (p); });
    menu.addItem ("Join with Previous Note", PitchCorrection::canJoin (notes, k - 1), false, [this, k] { joinWithNext (k - 1); });
    menu.addItem ("Join with Next Note", PitchCorrection::canJoin (notes, k), false, [this, k] { joinWithNext (k); });
    return menu;
}

void PitchEditor::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        dragging = -1;
        auto menu = menuAt (e.position);

        if (menu.getNumItems() > 0)
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());

        return;
    }

    if (e.mods.isCommandDown())
    {
        dragging = -1;
        splitAt (e.position);
        return;
    }

    dragging = noteAt (e.position);
    axis = Axis::undecided;

    if (dragging >= 0)
    {
        dragShift = notes[(size_t) dragging].shiftCents;
        dragMove = notes[(size_t) dragging].moveFrames;
    }

    repaint();
}

void PitchEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    // The way it first goes, 4 points from where it was pressed, is the way it moves.
    if (axis == Axis::undecided)
    {
        const auto offset = e.position - e.mouseDownPosition;

        if (offset.getDistanceFromOrigin() < 4.0f)
            return;

        axis = std::abs (offset.x) > std::abs (offset.y) ? Axis::time : Axis::pitch;
    }

    if (axis == Axis::time)
    {
        // As far as the mouse has gone, or back to the furthest the time map allows on the way.
        const auto wanted = dragMove + frameAt (e.position.x) - frameAt (e.mouseDownPosition.x);
        const auto step = wanted > dragMove ? -1 : 1;

        for (auto move = wanted; ; move += step)
        {
            notes[(size_t) dragging].moveFrames = move;

            if (move == dragMove || (source.has_value() && PitchCorrection::timeMapFor (track, source->start, source->end, notes).has_value()))
                break;
        }

        refreshTimes();
        repaint();
        return;
    }

    auto& n = notes[(size_t) dragging];
    const auto moved = pitchAt (e.position.y) - pitchAt ((float) e.getMouseDownY());
    auto shift = dragShift + moved * 100.0;

    // To whole semitones unless Option is held.
    if (! e.mods.isAltDown())
        shift = (std::round (n.pitch + shift / 100.0) - n.pitch) * 100.0;

    n.shiftCents = juce::jlimit (-1200.0, 1200.0, shift);
    repaint();
}

void PitchEditor::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
    repaint();
}

void PitchEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto k = noteAt (e.position); k >= 0)
    {
        auto& n = notes[(size_t) k];
        n.shiftCents = (std::round (n.pitch) - n.pitch) * 100.0;
        repaint();
    }
}
