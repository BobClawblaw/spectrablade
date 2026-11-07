// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/PitchCorrection.h"
#include "../Source/AudioEdits.h"
#include "../Source/PitchEditor.h"
#include "TestUtils.h"
#include "PitchCorrectionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int second = (int) rate;

    /** A voice-like buzz: 30 harmonics falling 6 dB an octave, shaped by a formant at 800 Hz, at
        the pitch hz (i) gives for each frame, at about -12 dBFS peak; both channels alike. */
    AudioDocument::Snapshot buzz (int frames, const std::function<double (int)>& hz)
    {
        juce::AudioBuffer<float> b (2, frames);
        double phase = 0.0;

        for (int i = 0; i < frames; ++i)
        {
            const auto f0 = hz (i);
            double v = 0.0;

            for (int h = 1; h <= 30 && h * f0 < 0.45 * rate; ++h)
            {
                const auto formant = 1.0 / std::sqrt (1.0 + std::pow ((h * f0 - 800.0) / 300.0, 2.0));
                v += std::sin (h * phase) / h * (0.3 + formant);
            }

            b.setSample (0, i, (float) (0.12 * v));
            b.setSample (1, i, (float) (0.12 * v));
            phase += juce::MathConstants<double>::twoPi * f0 / rate;
        }

        return AudioEdits::fromBuffer (std::move (b));
    }

    double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }

    /** The median pitch over [from, to) frames, from the detector; 0 if mostly without one. */
    double medianPitch (const AudioSnapshot& s, juce::int64 from, juce::int64 to)
    {
        const auto track = PitchCorrection::detect (s, from, to, rate);
        std::vector<float> voiced;

        for (auto hz : track.hz)
            if (hz > 0.0f)
                voiced.push_back (hz);

        if (voiced.size() * 2 < track.hz.size())
            return 0.0;

        std::nth_element (voiced.begin(), voiced.begin() + (long) voiced.size() / 2, voiced.end());
        return voiced[voiced.size() / 2];
    }

    double rms (const AudioSnapshot& s, juce::int64 from, juce::int64 to)
    {
        juce::AudioBuffer<float> b (1, (int) (to - from));
        s.read (b, 0, from, b.getNumSamples());
        return b.getRMSLevel (0, 0, b.getNumSamples());
    }

    void testDetect()
    {
        juce::StringArray seen;
        auto worst = 0.0;

        for (auto hz : { 98.0, 220.0, 440.5, 830.0 })
        {
            const auto s = buzz (second, [hz] (int) { return hz; });
            const auto found = medianPitch (*s, 0, second);
            worst = juce::jmax (worst, found > 0.0 ? std::abs (cents (found, hz)) : 1200.0);
            seen.add (juce::String (hz, 1) + " as " + juce::String (found, 2));
        }

        // Noise and silence: no pitch.
        juce::AudioBuffer<float> noise (1, second);
        juce::Random random (4);

        for (int i = 0; i < second; ++i)
            noise.setSample (0, i, 0.2f * (2.0f * random.nextFloat() - 1.0f));

        const auto noisy = AudioEdits::fromBuffer (std::move (noise));
        const auto noiseTrack = PitchCorrection::detect (*noisy, 0, second, rate);
        const auto voicedInNoise = std::count_if (noiseTrack.hz.begin(), noiseTrack.hz.end(), [] (float h) { return h > 0.0f; });
        juce::AudioBuffer<float> quiet (1, second);
        quiet.clear();
        const auto silent = PitchCorrection::detect (*AudioEdits::fromBuffer (std::move (quiet)), 0, second, rate);
        const auto voicedInSilence = std::count_if (silent.hz.begin(), silent.hz.end(), [] (float h) { return h > 0.0f; });

        check (worst < 1.0 && voicedInNoise * 20 < (long) noiseTrack.hz.size() && voicedInSilence == 0 && noiseTrack.hz.size() == 200,
               "pitch correction: the pitch of a buzz is found within 1 cent (" + seen.joinIntoString (", ") + " Hz; worst "
                   + juce::String (worst, 2) + " cents); white noise has a pitch in " + juce::String ((int) voicedInNoise) + " of "
                   + juce::String ((int) noiseTrack.hz.size()) + " frames, silence in none");
    }

    void testScales()
    {
        // 228 Hz: 38 cents below A#3 (233.08 Hz), 62 above A3 (220). Chromatic, it goes to A#; in C
        // major, with no A#, to A; at half strength, half way to A#.
        const auto in = buzz (2 * second, [] (int) { return 228.0; });
        const auto run = [&] (PitchCorrection::Settings s)
        {
            const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, s);
            return out != nullptr ? medianPitch (*out, second / 2, 3 * second / 2) : 0.0;
        };

        PitchCorrection::Settings chromatic;
        PitchCorrection::Settings major;
        major.scale = PitchCorrection::Scale::major;
        PitchCorrection::Settings half;
        half.strengthPercent = 50.0f;
        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto toChromatic = cents (run (chromatic), aSharp), toMajor = cents (run (major), 220.0);
        const auto halfWay = cents (run (half), 228.0), wanted = 0.5 * cents (aSharp, 228.0);
        const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, chromatic);
        const auto levelChange = out != nullptr ? juce::Decibels::gainToDecibels (rms (*out, second / 2, 3 * second / 2) / rms (*in, second / 2, 3 * second / 2)) : 99.0f;

        check (std::abs (toChromatic) < 3.0 && std::abs (toMajor) < 3.0 && std::abs (halfWay - wanted) < 3.0 && std::abs (levelChange) < 0.5,
               "pitch correction: a voice at 228 Hz goes to A#3 in the chromatic scale (" + juce::String (toChromatic, 2) + " cents off), to A3 in "
               "C major (" + juce::String (toMajor, 2) + " cents off), half way at half strength (" + juce::String (halfWay, 1) + " cents for "
                   + juce::String (wanted, 1) + "), its level kept (" + juce::String (levelChange, 2) + " dB)");
    }

    void testGlideAndSpeed()
    {
        // A glide from 200 to 260 Hz over 3 s, corrected at once: every moment on a note (all but
        // the moments a note changes).
        const auto glideIn = buzz (3 * second, [] (int i) { return 200.0 * std::pow (260.0 / 200.0, (double) i / (3.0 * rate)); });
        PitchCorrection::Settings now;
        now.speedMs = 0.0f;
        const auto glideOut = PitchCorrection::correct (*glideIn, 0, 3 * second, rate, now);
        auto onNote = 0, voiced = 0;

        if (glideOut != nullptr)
            for (auto hz : PitchCorrection::detect (*glideOut, second / 10, 3 * second - second / 10, rate).hz)
                if (hz > 0.0f)
                {
                    ++voiced;
                    const auto note = 69.0 + 12.0 * std::log2 (hz / 440.0);
                    onNote += std::abs (note - std::round (note)) < 0.05 ? 1 : 0;
                }

        // Slowly (200 ms): a note at 228 Hz is still near where it was 30 ms in, and on A# by 1.5 s.
        const auto in = buzz (2 * second, [] (int) { return 228.0; });
        PitchCorrection::Settings slow;
        slow.speedMs = 200.0f;
        const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, slow);
        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto early = out != nullptr ? cents (medianPitch (*out, (int) (0.02 * rate), (int) (0.04 * rate)), 228.0) : 99.0;
        const auto late = out != nullptr ? cents (medianPitch (*out, (int) (1.4 * rate), (int) (1.6 * rate)), aSharp) : 99.0;

        check (voiced > 400 && onNote * 100 >= voiced * 95 && std::abs (early) < 10.0 && std::abs (late) < 3.0,
               "pitch correction: a glide from 200 to 260 Hz corrected at once is within 5 cents of a note in " + juce::String (onNote) + " of "
                   + juce::String (voiced) + " frames; corrected over 200 ms, a note 38 cents flat is " + juce::String (early, 1)
                   + " cents from where it was 30 ms in and " + juce::String (late, 2) + " cents from A# by 1.5 s");
    }

    void testKeptAndRange()
    {
        // In tune: kept in tune. Noise (no pitch): passes as it was, to float rounding.
        const auto tuned = buzz (2 * second, [] (int) { return 220.0; });
        const auto tunedOut = PitchCorrection::correct (*tuned, 0, 2 * second, rate, {});
        const auto stays = tunedOut != nullptr ? cents (medianPitch (*tunedOut, second / 2, 3 * second / 2), 220.0) : 99.0;

        juce::AudioBuffer<float> noise (2, second);
        juce::Random random (9);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < second; ++i)
                noise.setSample (ch, i, 0.2f * (2.0f * random.nextFloat() - 1.0f));

        const auto noisy = AudioEdits::fromBuffer (juce::AudioBuffer<float> (noise));
        const auto noiseOut = PitchCorrection::correct (*noisy, 0, second, rate, {});
        auto largest = noiseOut != nullptr ? 0.0f : 1.0f;

        if (noiseOut != nullptr)
        {
            juce::AudioBuffer<float> b (2, second);
            noiseOut->read (b, 0, 0, second);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < second; ++i)
                    largest = juce::jmax (largest, std::abs (b.getSample (ch, i) - noise.getSample (ch, i)));
        }

        // A range: the audio outside it bit for bit, and the pitch in it moved.
        const auto in = buzz (3 * second, [] (int) { return 228.0; });
        const juce::int64 start = second, end = 2 * second;
        const auto out = PitchCorrection::correct (*in, start, end, rate, {});
        auto outsideSame = out != nullptr;

        if (out != nullptr)
        {
            juce::AudioBuffer<float> a (2, 3 * second), b (2, 3 * second);
            in->read (a, 0, 0, a.getNumSamples());
            out->read (b, 0, 0, b.getNumSamples());

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 3 * second; ++i)
                    if (i < start || i >= end)
                        outsideSame = outsideSame && a.getSample (ch, i) == b.getSample (ch, i);
        }

        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto inside = out != nullptr ? cents (medianPitch (*out, start + second / 4, end - second / 4), aSharp) : 99.0;
        int calls = 0;
        const auto stopped = PitchCorrection::correct (*in, 0, 3 * second, rate, {}, [&] (float) { return ++calls < 2; });
        PitchCorrection::Settings badKey, badSpeed, badStrength;
        badKey.key = 12;
        badSpeed.speedMs = 501.0f;
        badStrength.strengthPercent = -1.0f;

        check (std::abs (stays) < 1.0 && largest < 1.0e-6f && outsideSame && std::abs (inside) < 3.0 && stopped == nullptr && calls == 2
                   && PitchCorrection::correct (*in, 10, 10, rate, {}) == nullptr && ! PitchCorrection::isValid (badKey)
                   && ! PitchCorrection::isValid (badSpeed) && ! PitchCorrection::isValid (badStrength),
               "pitch correction: a voice in tune stays in tune (" + juce::String (stays, 2) + " cents), noise passes as it was (within "
                   + juce::String (largest, 8) + "), a range moves the pitch in it (" + juce::String (inside, 2) + " cents from A#) and leaves "
                   "the audio outside it bit for bit; an empty range and settings out of range are refused, and it can be stopped");
    }
}

namespace
{
    /** A melody: A3, B3 with a vibrato of 30 cents at 5 Hz, C4, each 0.5 s and joined; 0.2 s of
        silence; then 228 Hz for 0.5 s. */
    double melody (int i)
    {
        const auto t = i / rate;

        if (t < 0.5)  return 220.0;
        if (t < 1.0)  return 246.94 * std::pow (2.0, 0.3 / 12.0 * std::sin (juce::MathConstants<double>::twoPi * 5.0 * t));
        if (t < 1.5)  return 261.63;
        if (t < 1.7)  return 0.0;
        return 228.0;
    }

    AudioDocument::Snapshot melodySnapshot()
    {
        const auto all = buzz ((int) (2.2 * rate), [] (int i) { const auto hz = melody (i); return hz > 0.0 ? hz : 100.0; });
        juce::AudioBuffer<float> b (2, (int) all->getNumFrames());
        all->read (b, 0, 0, b.getNumSamples());
        b.clear ((int) (1.5 * rate), (int) (0.2 * rate));
        return AudioEdits::fromBuffer (std::move (b));
    }

    void testNotes()
    {
        const auto in = melodySnapshot();
        const auto length = in->getNumFrames();
        const auto track = PitchCorrection::analyse (*in, 0, length, rate);
        const auto notes = PitchCorrection::findNotes (track);
        juce::StringArray seen;
        const double wanted[] { 57.0, 59.0, 60.0, 69.0 + 12.0 * std::log2 (228.0 / 440.0) };
        auto right = notes.size() == 4;

        for (size_t k = 0; k < notes.size(); ++k)
        {
            seen.add (juce::String (notes[k].pitch, 2) + " from " + juce::String ((double) (track.first + (juce::int64) notes[k].first * track.hop) / rate, 2) + " s");
            right = right && k < 4 && std::abs (notes[k].pitch - wanted[k]) < (k == 1 ? 0.15 : 0.05);
        }

        check (right, "pitch correction by hand: a melody of A3, B3 with vibrato, C4, a pause and a note at 228 Hz is found as four notes, "
                      "within 5 cents of each (15 for the one with vibrato, whose median over its 2.5 swings isn't its centre: "
                          + seen.joinIntoString ("; ") + ")");

        if (notes.size() != 4)
            return;

        // B3 moved up a semitone and the last note snapped to its nearest (A#3): those move, the others stay.
        auto moved = PitchCorrection::snapped (notes);
        moved[0].shiftCents = moved[2].shiftCents = 0.0;
        moved[1].shiftCents = 100.0;
        const auto out = PitchCorrection::moveNotes (*in, 0, length, rate, track, moved);
        const auto pitchOf = [&] (const AudioSnapshot& s, double from, double to) { return medianPitch (s, (juce::int64) (from * rate), (juce::int64) (to * rate)); };
        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;

        if (out == nullptr)
        {
            check (false, "pitch correction by hand: notes are moved");
            return;
        }

        const auto first = cents (pitchOf (*out, 0.1, 0.4), 220.0), raised = cents (pitchOf (*out, 0.6, 0.9), pitchOf (*in, 0.6, 0.9)) - 100.0;
        const auto third = cents (pitchOf (*out, 1.1, 1.4), 261.63), last = cents (pitchOf (*out, 1.8, 2.1), aSharp);
        const auto snapCents = moved[3].shiftCents, snapWanted = cents (aSharp, 228.0);

        check (std::abs (first) < 1.0 && std::abs (raised) < 1.0 && std::abs (third) < 1.0 && std::abs (last) < 1.0 && std::abs (snapCents - snapWanted) < 1.0,
               "pitch correction by hand: B3 (with vibrato) moved up a semitone sounds a semitone higher (" + juce::String (raised, 2) + " cents off), the "
               "note at 228 Hz snapped to the nearest semitone moves " + juce::String (snapCents, 1) + " cents to A#3 (" + juce::String (last, 2)
                   + " cents off), and the notes left alone stay (" + juce::String (first, 2) + " and " + juce::String (third, 2) + " cents)");

        // Shifts ease from note to note; a track that doesn't reach the range is refused; nothing
        // moved, a note sounds as it was.
        const auto shifts = PitchCorrection::shiftsFor (track, moved);
        auto eased = true;

        for (size_t f = 1; f < shifts.size(); ++f)
            if (track.hz[f] > 0.0f && track.hz[f - 1] > 0.0f)
                eased = eased && std::abs (shifts[f] - shifts[f - 1]) <= 100.0 / 5.0 + 1.0e-9;

        const auto still = PitchCorrection::moveNotes (*in, 0, length, rate, track, notes);
        const auto stillPitch = still != nullptr ? cents (pitchOf (*still, 1.1, 1.4), 261.63) : 99.0;
        const auto shortTrack = PitchCorrection::analyse (*in, 0, length / 2, rate);

        check (eased && std::abs (stillPitch) < 1.0 && PitchCorrection::moveNotes (*in, 0, length, rate, shortTrack, moved) == nullptr
                   && PitchCorrection::moveNotes (*in, 10, 10, rate, track, moved) == nullptr,
               "pitch correction by hand: from one note to the next the shift changes by at most a fifth of the step a frame (over 25 ms); "
               "with nothing moved a note stays (" + juce::String (stillPitch, 2) + " cents); a pitch track that doesn't reach the range, "
               "and an empty range, are refused");

        // 30 s of audio is analysed quickly enough to wait for in the panel.
        const auto longOne = buzz (30 * second, [] (int) { return 196.0; });
        const auto began = juce::Time::getMillisecondCounterHiRes();
        const auto longTrack = PitchCorrection::analyse (*longOne, 0, 30 * second, rate);
        const auto longNotes = PitchCorrection::findNotes (longTrack);
        const auto took = (juce::Time::getMillisecondCounterHiRes() - began) / 1000.0;
        check (took < 3.0 && longNotes.size() == 1,
               "pitch correction by hand: 30 s of a held note is analysed and found as one note in " + juce::String (took, 2) + " s");
    }
}

namespace
{
    juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, juce::Point<float> down, juce::ModifierKeys mods = {}, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        return { juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                 juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                 juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, clicks, at != down };
    }

    void testPitchEditor()
    {
        const auto audio = melodySnapshot();
        juce::uint32 version = 7;
        std::vector<PitchCorrection::Note> handed;
        PitchEditor editor;
        editor.setSize (900, 320);
        editor.readSource = [&] { return std::optional<PitchEditor::Source> ({ audio, 0, audio->getNumFrames(), rate, 7 }); };
        editor.readVersion = [&] { return version; };
        editor.apply = [&] (const PitchEditor::Source&, const PitchCorrection::Track&, const std::vector<PitchCorrection::Note>& notes) { handed = notes; };
        editor.analyse();
        const auto found = editor.getNotes().size();

        const auto drag = [&] (int note, float dy, juce::ModifierKeys mods)
        {
            const auto from = editor.centreOf (note);
            editor.mouseDown (mouseAt (editor, from, from, mods));
            editor.mouseDrag (mouseAt (editor, from.translated (0.0f, dy), from, mods));
            editor.mouseUp (mouseAt (editor, from.translated (0.0f, dy), from, mods));
            return editor.getNotes()[(size_t) note];
        };

        // Up about one and a half semitones' worth: to a whole semitone; with Option, by cents.
        const auto rowHeight = found == 4 ? (editor.centreOf (0).y - editor.centreOf (2).y) / (float) (editor.getNotes()[2].pitch - editor.getNotes()[0].pitch) : 1.0f;
        const auto whole = found == 4 ? drag (1, -1.4f * rowHeight, {}) : PitchCorrection::Note {};
        const auto landed = whole.pitch + whole.shiftCents / 100.0;
        const auto free = found == 4 ? drag (0, -0.37f * rowHeight, juce::ModifierKeys::altModifier) : PitchCorrection::Note {};

        // Double-clicking the last note snaps it to the nearest semitone.
        if (found == 4)
            editor.mouseDoubleClick (mouseAt (editor, editor.centreOf (3), editor.centreOf (3), {}, 2));

        const auto snappedLast = found == 4 ? editor.getNotes()[3].shiftCents : 0.0;

        // Apply hands the notes over; once the audio has changed, it asks for a new analysis instead.
        for (auto* child : editor.getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (child); b != nullptr && b->getButtonText() == "Apply")
                b->onClick();

        const auto handedOver = handed.size() == found && found == 4 && juce::exactlyEqual (handed[1].shiftCents, whole.shiftCents);
        handed.clear();
        version = 8;

        for (auto* child : editor.getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (child); b != nullptr && b->getButtonText() == "Apply")
                b->onClick();

        const auto refusedStale = handed.empty() && editor.getNotes().empty() && editor.getStatus().contains ("Analyse again");

        check (found == 4 && std::abs (landed - std::round (landed)) < 1.0e-9 && whole.shiftCents > 50.0 && whole.shiftCents < 250.0
                   && std::abs (free.shiftCents - 37.0) < 3.0 && std::abs (snappedLast - cents (440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0, 228.0)) < 1.0
                   && handedOver && refusedStale,
               "pitch editor: analysed, a melody shows its four notes; dragging a note lands it on a whole semitone ("
                   + juce::String (whole.shiftCents, 1) + " cents), with Option it moves by cents (" + juce::String (free.shiftCents, 1)
                   + " for 0.37 of a semitone's height), double-clicking snaps one (" + juce::String (snappedLast, 1) + " cents); Apply hands "
                   "the notes over, and once the audio has changed asks for a new analysis instead");
    }
}

namespace
{
    void testSplitAndJoin()
    {
        const auto in = melodySnapshot();
        const auto length = in->getNumFrames();
        const auto track = PitchCorrection::analyse (*in, 0, length, rate);
        const auto notes = PitchCorrection::findNotes (track);

        if (notes.size() != 4)
        {
            check (false, "pitch editing, splitting and joining: the melody's four notes are found");
            return;
        }

        // B3 (with vibrato) cut in the middle: two notes, each with its own frames' pitch and B3's shift.
        auto shifted = notes;
        shifted[1].shiftCents = 30.0;
        const auto b3 = shifted[1];
        const auto middle = (b3.first + b3.end) / 2;
        const auto split = PitchCorrection::splitNote (track, shifted, 1, middle);
        const auto halvesRight = split.size() == 5 && split[1].first == b3.first && split[1].end == middle && split[2].first == middle
                                 && split[2].end == b3.end && juce::exactlyEqual (split[1].shiftCents, 30.0) && juce::exactlyEqual (split[2].shiftCents, 30.0)
                                 && juce::exactlyEqual (split[1].pitch, PitchCorrection::pitchOf (track, b3.first, middle))
                                 && std::abs (split[1].pitch - 59.0) < 0.3 && std::abs (split[2].pitch - 59.0) < 0.3;
        const auto nearEnd = PitchCorrection::splitNote (track, shifted, 1, b3.first + 1).size() == 4
                             && PitchCorrection::splitNote (track, shifted, 1, b3.end - 1).size() == 4
                             && PitchCorrection::splitNote (track, shifted, 1, b3.first + PitchCorrection::shortestNote).size() == 5;

        // Joined again: B3 as it was found, pitch and all, with the shift of the longer half.
        auto unequal = split;
        unequal[2].shiftCents = 80.0;
        const auto rejoined = PitchCorrection::joinNotes (track, split, 1);
        const auto longerWins = PitchCorrection::joinNotes (track, unequal, 1)[1].shiftCents;
        const auto backAsFound = rejoined.size() == 4 && rejoined[1].first == b3.first && rejoined[1].end == b3.end
                                 && juce::exactlyEqual (rejoined[1].pitch, notes[1].pitch) && juce::exactlyEqual (rejoined[1].shiftCents, 30.0);
        const auto halfLength = [&] (const PitchCorrection::Note& n) { return n.end - n.first; };
        const auto wantShift = halfLength (unequal[2]) > halfLength (unequal[1]) ? 80.0 : 30.0;

        // Notes with a pause between them don't join.
        auto apart = -1;

        for (int k = 0; k + 1 < (int) notes.size(); ++k)
            if (notes[(size_t) k].end < notes[(size_t) k + 1].first)
                apart = k;

        const auto refused = apart >= 0 && ! PitchCorrection::canJoin (notes, apart) && PitchCorrection::joinNotes (track, notes, apart).size() == 4
                             && ! PitchCorrection::canJoin (notes, 3) && ! PitchCorrection::canJoin (notes, -1);

        check (halvesRight && nearEnd && backAsFound && juce::exactlyEqual (longerWins, wantShift) && refused,
               "pitch editing, splitting and joining: B3 (with vibrato) split in the middle is two notes, each with its own frames' pitch ("
                   + juce::String (split.size() == 5 ? split[1].pitch : 0.0, 2) + " and " + juce::String (split.size() == 5 ? split[2].pitch : 0.0, 2)
                   + ") and the note's shift; not within 10 ms of an end; joined again it is the note as found, with the longer half's shift; "
                   "notes with a pause between them don't join");

        // The second half moved up a semitone, the first left: only the second half sounds higher.
        if (split.size() != 5)
            return;

        auto moved = split;

        for (auto& n : moved)
            n.shiftCents = 0.0;

        moved[2].shiftCents = 100.0;
        const auto out = PitchCorrection::moveNotes (*in, 0, length, rate, track, moved);
        const auto secondsOf = [&] (int frame) { return (double) (track.first + (juce::int64) frame * track.hop) / rate; };
        const auto pitchOver = [&] (const AudioSnapshot& a, double from, double to) { return medianPitch (a, (juce::int64) (from * rate), (juce::int64) (to * rate)); };

        // Each half away from the 25 ms where the shift eases between them.
        const auto firstFrom = secondsOf (b3.first) + 0.03, firstTo = secondsOf (middle) - 0.03;
        const auto secondFrom = secondsOf (middle) + 0.03, secondTo = secondsOf (b3.end) - 0.03;
        const auto kept = out != nullptr ? cents (pitchOver (*out, firstFrom, firstTo), pitchOver (*in, firstFrom, firstTo)) : 99.0;
        const auto raised = out != nullptr ? cents (pitchOver (*out, secondFrom, secondTo), pitchOver (*in, secondFrom, secondTo)) - 100.0 : 99.0;

        check (std::abs (kept) < 1.0 && std::abs (raised) < 1.0,
               "pitch editing, splitting and joining: with B3's second half moved up a semitone and its first left, the first half "
               "sounds as it was (" + juce::String (kept, 2) + " cents) and the second a semitone higher (" + juce::String (raised, 2) + " cents off)");
    }

    void testEditorSplitAndJoin()
    {
        const auto audio = melodySnapshot();
        PitchEditor editor;
        editor.setSize (900, 320);
        editor.readSource = [&] { return std::optional<PitchEditor::Source> ({ audio, 0, audio->getNumFrames(), rate, 7 }); };
        editor.analyse();

        if (editor.getNotes().size() != 4)
        {
            check (false, "pitch editor, splitting and joining: the melody's four notes are shown");
            return;
        }

        // Cmd-click in the middle of B3 splits it there.
        const auto b3 = editor.getNotes()[1];
        const auto at = editor.centreOf (1);
        editor.mouseDown (mouseAt (editor, at, at, juce::ModifierKeys::commandModifier));
        editor.mouseUp (mouseAt (editor, at, at, juce::ModifierKeys::commandModifier));
        const auto afterSplit = editor.getNotes().size();
        const auto halves = afterSplit == 5 ? editor.getNotes()[2].first - editor.getNotes()[1].first : 0;

        // The right half's menu: Split Here (it's long enough), Join with Previous (the left half
        // touches it); Join with Next only if the next note touches it. Its Join with Previous
        // puts B3 back.
        const auto menu = editor.menuAt (editor.centreOf (2));
        std::map<juce::String, std::pair<bool, std::function<void()>>> items;

        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            items[it.getItem().text] = { it.getItem().isEnabled, it.getItem().action };

        const auto offered = items.size() == 3 && items["Split Here"].first && items["Join with Previous Note"].first;

        if (items["Join with Previous Note"].second != nullptr)
            items["Join with Previous Note"].second();

        const auto joined = editor.getNotes().size() == 4 && editor.getNotes()[1].first == b3.first && editor.getNotes()[1].end == b3.end
                            && juce::exactlyEqual (editor.getNotes()[1].pitch, b3.pitch);
        const auto offNotes = editor.menuAt ({ 5.0f, 5.0f }).getNumItems() == 0;

        check (afterSplit == 5 && halves > 0 && offered && joined && offNotes && editor.getStatus().contains ("Cmd-click to split"),
               "pitch editor, splitting and joining: Cmd-clicking a note splits it where it is clicked (4 notes become 5); right-clicking "
               "a half offers Split Here and Join with Previous Note, which makes them one again; off the notes, no menu");
    }
}

void runPitchCorrectionTests()
{
    testSplitAndJoin();
    testEditorSplitAndJoin();
    testPitchEditor();
    testNotes();
    testDetect();
    testScales();
    testGlideAndSpeed();
    testKeptAndRange();
}
