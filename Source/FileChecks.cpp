#include "FileChecks.h"
#include "AudioFileIO.h"

struct FileChecks::Shared
{
    struct Result
    {
        Known known = Known::unknown;
        juce::uint32 checkedAt = 0;
        bool running = false;
    };

    std::mutex lock;
    std::map<juce::String, Result> results;
    FileChecks* owner = nullptr;   // nullptr once the owner is gone; message thread only
};

FileChecks::FileChecks (Checker c, int maxAgeMs)
    : shared (std::make_shared<Shared>()), checker (std::move (c)), maxAge (maxAgeMs)
{
    shared->owner = this;
}

FileChecks::~FileChecks()
{
    shared->owner = nullptr;
}

FileChecks::Known FileChecks::check (const juce::File& file)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto key = file.getFullPathName();
    const auto now = juce::Time::getMillisecondCounter();
    Known known;

    {
        const std::lock_guard<std::mutex> l (shared->lock);
        auto& r = shared->results[key];
        known = r.known;

        if (r.running || (r.known != Known::unknown && now - r.checkedAt < (juce::uint32) maxAge))
            return known;

        r.running = true;
    }

    std::thread ([s = shared, c = checker, file, key]
    {
        const auto found = c (file) ? Known::exists : Known::missing;
        bool changed;

        {
            const std::lock_guard<std::mutex> l (s->lock);
            auto& r = s->results[key];
            changed = r.known != found;
            r = { found, juce::Time::getMillisecondCounter(), false };
        }

        if (changed)
            juce::MessageManager::callAsync ([s]
            {
                if (s->owner != nullptr && s->owner->onChange != nullptr)
                    s->owner->onChange();
            });
    }).detach();

    return known;
}

int FileChecks::getNumRunning() const
{
    const std::lock_guard<std::mutex> l (shared->lock);
    return (int) std::count_if (shared->results.begin(), shared->results.end(), [] (auto& r) { return r.second.running; });
}

//==============================================================================
namespace BackgroundLoad
{
AudioFileIO::Loaded readIfThere (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    juce::AudioFormatManager formats;
    AudioFileIO::registerFormats (formats);
    return AudioFileIO::load (file, formats);
}

void start (const juce::File& file, std::function<void (AudioFileIO::Loaded)> onLoaded, Loader loader)
{
    std::thread ([file, done = std::move (onLoaded), load = std::move (loader)]
    {
        auto loaded = std::make_shared<AudioFileIO::Loaded> (load (file));
        juce::MessageManager::callAsync ([done, loaded] { done (std::move (*loaded)); });
    }).detach();
}
}
