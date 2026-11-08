#pragma once

#include "AudioDocument.h"

/** Music rebalance: the voice, the bass, the drums and the rest of a mix each turned up or down.
    The mix is analysed in 4,096-point frames at 75 % overlap (square-root Hann windows) and each
    bin of each frame shared among four stems by soft masks that add up to one, so with every stem
    at 0 dB the mix comes back as it was. Drums are what is percussive: a bin whose magnitude in its
    own channel stands out across frequency (the median over 17 bins) more than across time (the
    median over 17 frames), the two medians' squares sharing it. Of the rest (the harmonic share), the bass is the
    part below the bass corner (a soft edge over half an octave); the voice is the part of the mid
    (what the channels share) within a quarter of the pitch of each harmonic in a frame judged
    voiced, as Dialogue Isolation judges it, from 80 Hz to 4 kHz, plus the mid's highs from 4 to
    8 kHz in such a frame; and the rest is what is left, the sides included. The stems are a
    reading of the mix, not a separation: a steady chord in the mid reads as voice, a kick drum's
    body as bass, a plucked bass as partly drums. */
namespace Rebalance
{
    struct Settings
    {
        double voiceDb = 0.0, bassDb = 0.0, drumsDb = 0.0, restDb = 0.0;   // -40 to +12 each
        double bassCornerHz = 150.0;                                         // 60 to 300
    };

    bool isValid (const Settings&);

    /** Rebalances [start, end). progress gets 0..1 and returns false to cancel; then this returns
        nullptr. Nullptr too for settings that aren't valid or a range under a window. */
    AudioDocument::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                   const std::function<bool (float)>& progress = {});
}
