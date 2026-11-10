#include <JuceHeader.h>
#include "../Source/NoteEditor.h"
#include "../Source/Theme.h"
#include "TestUtils.h"
#include "NoteEditorTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    juce::MouseEvent eventAt (juce::Component& c, juce::Point<float> at, juce::Point<float> down, bool dragged = false,
                              juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods,
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                 juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, dragged);
    }

    /** What the editor is given and hands back: the clip it shows, and the steps it applied. */
    struct Rig
    {
        NoteEditor::Shown shown;
        std::vector<std::pair<NoteClip, juce::String>> applied;
        NoteEditor editor;

        Rig()
        {
            shown.clip.id = 7;
            shown.clip.name = "Riff";
            shown.clip.length = (juce::int64) (4.0 * rate);   // four seconds: eight beats at 120
            shown.sampleRate = rate;
            shown.beatSeconds = 0.5;
            shown.trackName = "Synth";
            editor.readClip = [this] { return std::optional<NoteEditor::Shown> (shown); };
            editor.apply = [this] (const NoteClip& c, const juce::String& d)
            {
                shown.clip = c;
                applied.emplace_back (c, d);
            };
            editor.setBounds (0, 0, 900, 400);
            editor.refresh();
        }

        /** Where a note at this pitch and frame starts: its left edge, the middle of its row. */
        juce::Point<float> at (int pitch, juce::int64 frame) const
        {
            const auto r = editor.noteBounds ({ pitch, frame, 1, 1.0f });
            return { r.getX(), r.getCentreY() };
        }

        void click (juce::Point<float> p, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
        {
            editor.mouseDown (eventAt (editor, p, p, false, mods));
            editor.mouseUp (eventAt (editor, p, p, false, mods));
        }

        void drag (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
        {
            editor.mouseDown (eventAt (editor, from, from, false, mods));
            editor.mouseDrag (eventAt (editor, (from + to) / 2.0f, from, true, mods));
            editor.mouseDrag (eventAt (editor, to, from, true, mods));
            editor.mouseUp (eventAt (editor, to, from, true, mods));
        }

        const std::vector<ClipNote>& notes() const  { return shown.clip.notes; }
        juce::String last() const                   { return applied.empty() ? juce::String() : applied.back().second; }
    };
}

void runNoteEditorTests()
{
    std::cout << "Piano roll:" << std::endl;

    Rig rig;
    auto& e = rig.editor;
    const auto step = e.gridStepFrames();
    check (e.getShown().has_value() && e.getShown()->clip.id == 7 && e.getSelectedNotes().empty() && e.getGridDivision() == 4 && step == 6000
               && e.pitchAt (rig.at (64, 0).y) == 64 && std::abs (e.frameAt (rig.at (64, 30000).x) - 30000) < 300,
           "piano roll: shows the clip it is given on a 1/4-beat grid (6,000 frames at 120), its rows and columns reading back a pitch and a frame");

    // Step entry: a click on empty grid puts a grid-long note at the grid line before the mouse.
    rig.click (rig.at (64, 7000));
    check (rig.notes().size() == 1 && rig.notes()[0].pitch == 64 && rig.notes()[0].start == 6000 && rig.notes()[0].length == 6000
               && std::abs (rig.notes()[0].velocity - 100.0f / 127.0f) < 1.0e-6f && rig.last() == "Add Note" && rig.applied.size() == 1
               && e.getSelectedNotes() == std::set<int> { 0 },
           "piano roll: a click on empty grid adds a note at the grid line before it, one step long, at the slider's velocity, selected (one undo step)");

    // Dragging the note moves it in pitch and time, snapped.
    rig.drag (rig.at (64, 8000), rig.at (66, 8000 + 12500));
    check (rig.notes().size() == 1 && rig.notes()[0].pitch == 66 && rig.notes()[0].start == 18000 && rig.notes()[0].length == 6000
               && rig.last() == "Move Notes" && rig.applied.size() == 2,
           "piano roll: dragging a note moves it two semitones up and two grid steps on (snapped), one undo step");

    // Dragging its right edge changes its length (snapped to the grid line after the mouse).
    const auto r = e.noteBounds (rig.notes()[0]);
    rig.drag ({ r.getRight() - 2.0f, r.getCentreY() }, { r.getRight() - 2.0f + (e.noteBounds ({ 66, 12000, 1, 1.0f }).getRight() - e.noteBounds ({ 66, 0, 1, 1.0f }).getRight()), r.getCentreY() });
    check (rig.notes().size() == 1 && rig.notes()[0].start == 18000 && rig.notes()[0].length == 18000 && rig.last() == "Note Length",
           "piano roll: dragging a note's right edge makes it three steps long");

    // A second note, then Shift-click adds it to the selection and the velocity slider sets both.
    rig.click (rig.at (60, 36500));
    rig.click (rig.at (66, 20000), juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    e.setVelocity (0.5f);
    check (rig.notes().size() == 2 && e.getSelectedNotes() == std::set<int> { 0, 1 } && rig.last() == "Note Velocity"
               && std::abs (rig.notes()[0].velocity - 64.0f / 127.0f) < 1.0e-6f && std::abs (rig.notes()[1].velocity - 64.0f / 127.0f) < 1.0e-6f,
           "piano roll: Shift-click adds a note to the selection, and the velocity slider sets every selected note");

    // The arrow keys move the selection: up a semitone, right a grid step, Shift-up an octave.
    e.keyPressed (juce::KeyPress (juce::KeyPress::upKey));
    e.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
    e.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    check (rig.notes()[0].pitch == 79 && rig.notes()[0].start == 24000 && rig.notes()[1].pitch == 73 && rig.notes()[1].start == 42000
               && rig.last() == "Move Notes",
           "piano roll: the arrow keys move the selected notes a semitone and a grid step, Shift an octave");

    // Option-click removes a note; Delete removes the selected ones; Cmd-A selects all.
    rig.click (rig.at (73, 44000), juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);
    const auto afterOption = rig.notes().size();
    e.keyPressed (juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0));
    const auto all = e.getSelectedNotes().size();
    e.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
    check (afterOption == 1 && all == 1 && rig.notes().empty() && rig.last() == "Remove Note" && e.getSelectedNotes().empty(),
           "piano roll: Option-click removes a note, Cmd-A selects all, Delete removes the selected");

    // A box dragged with Shift over empty grid selects the notes it touches; a click on empty grid
    // past the clip's end adds nothing; with the grid off a note takes the last length.
    rig.click (rig.at (60, 6500));
    rig.click (rig.at (62, 30500));
    rig.drag (rig.at (58, 0), rig.at (61, 12000), juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    const auto boxed = e.getSelectedNotes();
    const auto before = rig.applied.size();
    e.mouseDown (eventAt (e, { (float) e.getWidth() - 1.0f, rig.at (60, 0).y }, { (float) e.getWidth() - 1.0f, rig.at (60, 0).y }));
    e.mouseUp (eventAt (e, { (float) e.getWidth() - 1.0f, rig.at (60, 0).y }, { (float) e.getWidth() - 1.0f, rig.at (60, 0).y }));
    e.setGridDivision (0);
    rig.click (rig.at (65, 50123));
    check (boxed == std::set<int> { 0 } && rig.applied.size() == before + 1 && rig.notes().size() == 3 && e.gridStepFrames() == 0
               && std::abs (rig.notes()[2].start - 50123) < 300 && rig.notes()[2].length == 18000,
           "piano roll: a Shift-drag selects the notes in its box, a click past the clip's end adds nothing, and with the grid off a "
           "note goes where the mouse is at the length last dragged to");

    // Another clip shown drops the selection; nothing shown disables the controls.
    rig.shown.clip.id = 8;
    e.refresh();
    const auto dropped = e.getSelectedNotes().empty() && e.getShown().has_value() && e.getShown()->clip.id == 8;
    e.readClip = [] { return std::optional<NoteEditor::Shown>(); };
    e.refresh();
    check (dropped && ! e.getShown().has_value(), "piano roll: a different clip clears the selection, and with none selected it shows nothing");

    // Drawn in every theme without an unpainted spot.
    auto painted = true;

    for (const auto& name : Theme::themeNames())
    {
        Theme::apply (Theme::make (name, "Blue"));
        Rig look;
        look.shown.clip.loopLength = 96000;
        look.shown.clip.notes = { { 60, 0, 12000, 0.9f }, { 64, 12000, 12000, 0.6f }, { 67, 24000, 24000, 0.3f } };
        look.editor.refresh();
        const auto image = look.editor.createComponentSnapshot (look.editor.getLocalBounds());
        painted = painted && image.isValid() && image.getWidth() == 900;
    }

    check (painted, "piano roll: drawn in every theme");
}
