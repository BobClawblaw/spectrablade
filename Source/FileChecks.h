#pragma once

#include "AudioFileIO.h"

/** Work on files that may sit on a slow or absent volume (a network share that doesn't answer),
    done away from the message thread so the window never waits for it. Each piece of work runs
    on its own thread, which is left to finish on its own: a volume that never answers holds up
    only the work on its own files. */
class FileChecks
{
public:
    using Checker = std::function<bool (const juce::File&)>;

    /** Results older than maxAgeMs are checked again the next time they are asked for, so a
        volume that comes back (or goes away) shows within a few seconds. */
    explicit FileChecks (Checker = [] (const juce::File& f) { return f.existsAsFile(); }, int maxAgeMs = 5000);
    ~FileChecks();

    enum class Known { unknown, exists, missing };

    /** What is known of the file now; an unknown or old result starts a check (one at a time per
        file). Message thread. */
    Known check (const juce::File&);

    /** Called on the message thread when a check finds something new. */
    std::function<void()> onChange;

    /** Checks still running. */
    int getNumRunning() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared;
    Checker checker;
    int maxAge;
};

/** Reads an audio file on a thread of its own, then hands the result to the message thread. */
namespace BackgroundLoad
{
    using Loader = std::function<AudioFileIO::Loaded (const juce::File&)>;

    /** The file read with the basic formats, as the engine reads them, or an empty result with no
        error when the file isn't there. */
    AudioFileIO::Loaded readIfThere (const juce::File&);

    /** Starts the load and returns at once; onLoaded runs on the message thread when it is done.
        Anything onLoaded uses must check it is still alive. */
    void start (const juce::File&, std::function<void (AudioFileIO::Loaded)> onLoaded, Loader = readIfThere);
}
