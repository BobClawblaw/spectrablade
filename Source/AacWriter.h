#pragma once

#include <functional>
#include <string>

/** Writes AAC in an MPEG-4 file (.m4a) with the system's encoder. Kept apart from JUCE, whose names
    clash with the system's audio headers. */
namespace AacWriter
{
    /** Fills each channel with up to maxFrames frames and returns how many; 0 ends the file, and a
        negative count stops the writing. */
    using Pull = std::function<int (float* const* channels, int maxFrames)>;

    /** Writes the file at path, encoding audio at inputRate as outputRate (the encoder resamples) at the
        average bit rate it takes nearest at or below kbps. seconds is the audio's length, for which room
        is kept at the start of the file for its index of packets. Returns an error message, "Stopped." if pull stopped
        it, or empty on success. */
    std::string write (const char* path, double inputRate, double outputRate, unsigned channels, int kbps, double seconds, const Pull& pull);
}
