#pragma once

#include <JuceHeader.h>

/** Opening and writing the formats SpectraBlade doesn't read or write itself, through an ffmpeg
    the user has installed (it is not part of SpectraBlade). */
namespace Ffmpeg
{
    /** The ffmpeg program: the first found of Homebrew's (/opt/homebrew/bin, /usr/local/bin),
        MacPorts' (/opt/local/bin) and the folders on the PATH. Nothing if there is none. */
    juce::File find();

    /** File types offered to open through ffmpeg, besides those read directly. Video files give
        their first audio stream. */
    const juce::StringArray& importExtensions();

    /** Decodes the first audio stream of source into a 32-bit float WAV (RF64 when over 4 GB), at
        its own sample rate and channels. keepGoing (if given) is asked every 50 ms; false stops
        ffmpeg. Returns an error message, or empty on success. */
    juce::String decode (const juce::File& ffmpeg, const juce::File& source, const juce::File& wav,
                         const std::function<bool()>& keepGoing = {});

    struct Target
    {
        juce::String name, extension, codec;
        bool hasBitRate = false;   // lossy: takes a bit rate in kbps
        juce::Array<int> bitRates; // offered, highest first
        int defaultBitRate = 0;
    };

    /** The formats offered to export through ffmpeg. */
    const std::vector<Target>& targets();

    /** Encodes a WAV into target with ffmpeg (kbps for the lossy ones), through a temporary file
        beside the target. Returns an error message, or empty on success. */
    juce::String encode (const juce::File& ffmpeg, const juce::File& wav, const juce::File& target, const Target&, int kbps,
                         const std::function<bool()>& keepGoing = {});
}
