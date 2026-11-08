#pragma once

#include "AudioDocument.h"
#include "LossyFiles.h"

/** Codec preview: the sound as it will come back from an MP3 or an AAC file at a bit rate. The
    range is written through the real encoder (as Export as MP3 or AAC writes it) to a temporary
    file, read back, lined up with the original (the encoder's delay found by cross-correlation
    over the first second, within 8,192 frames), brought back to the audio's rate where the file's
    differs, trimmed to the range's length and put in its place, so what plays is the codec's own
    output. Applying it keeps that output in the file, for an A/B or to work on what a listener
    will hear. Mono or stereo only, as the encoders are. */
namespace CodecPreview
{
    struct Result
    {
        AudioDocument::Snapshot snapshot;
        int delayFrames = 0;      // the encoder's delay found and taken out
        double bytesPerSecond = 0.0;
    };

    /** The round trip over [start, end). Nothing (an error) for settings that aren't valid, more
        than two channels, an empty range, a file that couldn't be written or read back, or when
        cancelled. */
    std::optional<Result> roundTrip (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const LossyFiles::Settings&,
                                     juce::String& error, const std::function<bool (float)>& progress = {});
}
