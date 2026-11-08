#pragma once

#include "AudioDocument.h"

/** Dialogue isolation: speech kept and everything else lowered. The audio is analysed in
    4,096-point frames at 75 % overlap (square-root Hann windows, as the noise reduction). Each frame
    is judged for speech by its pitch (pitchOf below): how well the frame repeats at some period
    from 70 to 400 Hz (the autocorrelation of its 100 Hz to 1.5 kHz, the frame's own taper taken
    out), the shortest period within 15 % of the best repeat taken, as long as the frame does not
    repeat as well at half of it (a lone tone does) and at least three of its first twelve harmonics
    (under 4 kHz) stand twice over the gaps between them (a lone tone or a chance repeat in noise
    has fewer). Where the shortest period fails that last test, the longer ones up to four times it
    are tried, asked for five harmonics with one among the first three, since a low voice with a
    strong formant repeats at a third of its period nearly as well as at the period. A frame with
    voicing over the sensitivity's threshold is voiced speech, as is a frame within 80 ms after one
    whose voicing falls short of the threshold by less than 0.15 (a voice's breathier moments and
    its ends), and in it the bins within a quarter of the pitch of each harmonic (up to 4 kHz) and
    the bins from 4 to 8 kHz that stand over the background are kept. A frame whose power from
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
        repeat in noise (fewer than three harmonics standing over the gaps between them). A low
        voice whose shortest repeat is a harmonic's (a strong formant) is read at its own period
        when that holds five harmonics, one among the first three. */
    struct Pitch { double frequency = 0.0; double voicing = 0.0; };
    Pitch pitchOf (const juce::AudioBuffer<float>& audio, int at, double sampleRate);

    /** Isolates speech over frames [start, end). progress gets 0..1 and returns false to cancel;
        then this returns nullptr. Nullptr too for settings that aren't valid or an empty range. */
    AudioDocument::Snapshot isolate (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, Settings,
                                     const std::function<bool (float)>& progress = {});
}
