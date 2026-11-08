#pragma once

#include "AudioDocument.h"

/** Dialogue isolation: speech kept and everything else lowered. The audio is analysed in
    4,096-point frames at 75 % overlap (square-root Hann windows, as the noise reduction). Each frame
    is judged for speech: a pitch is looked for from 70 to 400 Hz by comparing, for each candidate,
    the power per bin within a quarter of it round its first twelve harmonics with the power per bin
    in the gaps between them (from 80 Hz to 4 kHz); a candidate counts only with at least three
    harmonics, and two thirds of those looked for, standing four times over the gaps (a lone tone
    is periodic but not speech; a candidate an octave under the pitch finds only every other one),
    the higher of candidates within 2 % wins, and the best one's voicing is that ratio over one plus itself (0 for nothing periodic, near 1 for a
    clean buzz); a frame with voicing over the sensitivity's threshold is voiced speech, and in it the bins within a quarter of the pitch of each harmonic (up to 4 kHz)
    and the bins from 4 to 8 kHz that stand over the background are kept. A frame whose power from
    2 to 8 kHz stands 6 dB over the background where the rest does not is a fricative (an s or an f)
    and keeps that band. The background is the running minimum of each bin's smoothed power over
    the last second and a half (what is always there). Everything not kept is lowered by the
    reduction; the kept and lowered shares ease over a few frames so nothing warbles. Mono or
    stereo (the channels judged on their mix, each shaped alike). */
namespace DialogueIsolation
{
    struct Settings
    {
        float reductionDb = 12.0f;    // 0 to 40: how far everything that isn't speech is lowered
        float sensitivity = 50.0f;    // 0 to 100: how readily a frame counts as speech (the voicing threshold from 0.85 down to 0.6)
    };

    bool isValid (const Settings&);

    /** The voicing threshold the sensitivity sets. */
    float thresholdFor (float sensitivity);

    /** The pitch of the 4,096 frames of audio from `at` (the channels mixed; frames outside the
        buffer count as silence), in Hz, and its voicing (0 to 1): how well the frame repeats at
        that period, the frame's own taper taken out, so a steady voice reads near 1 and noise
        under about 0.4. 0 Hz and 0 for nothing periodic enough, and for a lone tone or a chance
        repeat in noise (fewer than three harmonics standing over the gaps between them). */
    struct Pitch { double frequency = 0.0; double voicing = 0.0; };
    Pitch pitchOf (const juce::AudioBuffer<float>& audio, int at, double sampleRate);

    /** Isolates speech over frames [start, end). progress gets 0..1 and returns false to cancel;
        then this returns nullptr. Nullptr too for settings that aren't valid or an empty range. */
    AudioDocument::Snapshot isolate (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, Settings,
                                     const std::function<bool (float)>& progress = {});
}
