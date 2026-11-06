#include "Ffmpeg.h"

namespace Ffmpeg
{
namespace
{
    /** Runs ffmpeg to the end; its last error line on failure, "Stopped." if keepGoing said so. */
    juce::String run (const juce::File& ffmpeg, const juce::StringArray& arguments, const std::function<bool()>& keepGoing)
    {
        juce::StringArray command { ffmpeg.getFullPathName(), "-nostdin", "-hide_banner", "-v", "error", "-y" };
        command.addArray (arguments);
        juce::ChildProcess process;

        if (! process.start (command, juce::ChildProcess::wantStdErr | juce::ChildProcess::wantStdOut))
            return "ffmpeg could not be started: " + ffmpeg.getFullPathName();

        juce::MemoryOutputStream output;
        char buffer[4096];

        // Read as it goes, so a chatty ffmpeg never blocks on a full pipe.
        for (;;)
        {
            const auto n = process.readProcessOutput (buffer, sizeof (buffer));

            if (n > 0)
            {
                output.write (buffer, (size_t) n);
                continue;
            }

            if (! process.isRunning())
                break;

            if (keepGoing != nullptr && ! keepGoing())
            {
                process.kill();
                return "Stopped.";
            }

            juce::Thread::sleep (50);
        }

        if (process.getExitCode() == 0)
            return {};

        auto lines = juce::StringArray::fromLines (output.toString().trim());
        lines.removeEmptyStrings();
        return "ffmpeg failed" + (lines.isEmpty() ? juce::String (".") : ": " + lines[lines.size() - 1]);
    }
}

juce::File find()
{
    juce::StringArray folders { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin" };
    folders.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {});

    for (const auto& folder : folders)
        if (const auto f = juce::File (folder).getChildFile ("ffmpeg"); folder.startsWithChar ('/') && f.existsAsFile())
            return f;

    return {};
}

const juce::StringArray& importExtensions()
{
    static const juce::StringArray extensions { "opus", "wma", "aac", "ac3", "eac3", "wv", "ape", "tta", "mka", "mp2", "amr", "dts",
                                                "mp4", "m4b", "mov", "mkv", "webm", "avi" };
    return extensions;
}

juce::String decode (const juce::File& ffmpeg, const juce::File& source, const juce::File& wav, const std::function<bool()>& keepGoing)
{
    return run (ffmpeg, { "-i", source.getFullPathName(), "-map", "0:a:0", "-vn", "-c:a", "pcm_f32le", "-rf64", "auto", "-f", "wav",
                          wav.getFullPathName() }, keepGoing);
}

const std::vector<Target>& targets()
{
    static const std::vector<Target> list {
        { "Opus", "opus", "libopus", true, { 256, 192, 160, 128, 96, 64 }, 128 },
        { "Apple Lossless", "m4a", "alac", false, {}, 0 },
        { "WavPack", "wv", "wavpack", false, {}, 0 },
        { "AC-3", "ac3", "ac3", true, { 640, 448, 384, 320, 256, 192 }, 384 },
    };
    return list;
}

juce::String encode (const juce::File& ffmpeg, const juce::File& wav, const juce::File& target, const Target& t, int kbps,
                     const std::function<bool()>& keepGoing)
{
    juce::TemporaryFile temp (target);
    juce::StringArray arguments { "-i", wav.getFullPathName(), "-c:a", t.codec };

    if (t.hasBitRate)
        arguments.addArray ({ "-b:a", juce::String (kbps) + "k" });

    // Name the container rather than leave ffmpeg to guess it from the file name.
    arguments.addArray ({ "-f", t.extension == "m4a" ? "ipod" : t.extension, temp.getFile().getFullPathName() });

    if (const auto error = run (ffmpeg, arguments, keepGoing); error.isNotEmpty())
        return error;

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}
}
