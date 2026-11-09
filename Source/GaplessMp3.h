#pragma once

#include <JuceHeader.h>

/** MP3 read from its first sample to its last, as the encoder was given it. LAME (and ffmpeg's
    libmp3lame) writes the encoder's delay and padding into the tag in the first frame; Core Audio
    leaves them out when reading, which JUCE's own MP3 reader doesn't. This one does, so files read
    the same on every system. A file with no such tag reads as JUCE's reader gives it. */
class GaplessMp3Format final : public juce::MP3AudioFormat
{
public:
    /** The samples a decoder of this kind adds in front of the encoder's delay (LAME's documented 528 + 1). */
    static constexpr int decoderDelay = 529;

    struct Gap
    {
        int delay = 0, padding = 0;
    };

    /** The encoder's delay and padding from the LAME tag of the MP3 data at the stream's position, which
        is left where it was. Nothing when the first frame holds no such tag. */
    static std::optional<Gap> readGap (juce::InputStream&);

    juce::AudioFormatReader* createReaderFor (juce::InputStream*, bool deleteStreamIfOpeningFails) override;
};
