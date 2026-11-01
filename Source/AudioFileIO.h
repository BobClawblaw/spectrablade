#pragma once

#include "AudioDocument.h"

namespace AudioFileIO
{
    struct Loaded
    {
        AudioDocument::Snapshot snapshot;
        double sampleRate = 0.0;
        int bitDepth = 0;              // 32 means 32-bit float
        juce::String error;
        Markers markers;               // WAV cue points and regions, AIFF markers
    };

    /** Reads the whole file into memory, in blocks of a few seconds each. */
    Loaded load (const juce::File&, juce::AudioFormatManager&);

    /** Bit depths offered when saving to this file type (32 = float), best first. */
    juce::Array<int> bitDepthsFor (const juce::File&, juce::AudioFormatManager&);

    /** True for the file types that can hold markers (WAV, and AIFF without regions). */
    bool canStoreMarkers (const juce::File&);

    /** Writes the snapshot (and markers, where the format holds them) through a temporary file,
        then replaces the target. Integer formats get TPDF dither. Returns an error message, or
        empty on success. */
    juce::String save (const AudioSnapshot&, double sampleRate, const juce::File& target, int bitDepth,
                       juce::AudioFormatManager&, const Markers& = {});

    /** Saves only [start, end), with the markers in it moved so start becomes frame 0, as Crop keeps them. */
    juce::String saveRange (const AudioSnapshot&, double sampleRate, juce::int64 start, juce::int64 end, const juce::File& target,
                            int bitDepth, juce::AudioFormatManager&, const Markers& = {});
}
