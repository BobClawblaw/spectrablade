#pragma once

#include "AudioDocument.h"

/** Writing MP3 files (with LAME) and AAC in MPEG-4 files (.m4a, with the system's encoder). */
namespace LossyFiles
{
    enum class Kind { mp3, aac };

    struct Settings
    {
        Kind kind = Kind::mp3;
        int kbps = 192;            // the constant bit rate (MP3), or the bit rate aimed at (AAC)
        bool variable = false;     // MP3 only: a variable bit rate, set by vbrQuality instead of kbps
        int vbrQuality = 2;        // 0 (best, largest) to 9

        bool isValid() const;
    };

    /** The bit rates offered, in kbps, highest first. */
    const juce::Array<int>& bitRates();

    /** True for the file types written here (.mp3 and .m4a). */
    bool isLossy (const juce::File&);
    Kind kindFor (const juce::File&);

    /** The sample rate a file is written at: the audio's own rate when the formats hold it (8, 11.025,
        12, 16, 22.05, 24, 32, 44.1 and 48 kHz); above 48 kHz, 44.1 kHz for the 44.1 kHz family and 48 kHz
        otherwise; any other rate goes up to the next one they hold. */
    double outputRate (double sampleRate);

    /** Writes the audio through a temporary file, then replaces the target. One or two channels only.
        progress (if given) is called between blocks with the fraction done, and stops the writing by
        returning false. Returns an error message, or empty on success. */
    juce::String write (const AudioSnapshot&, double sampleRate, const juce::File& target, const Settings&,
                        const std::function<bool (float)>& progress = {});
}
