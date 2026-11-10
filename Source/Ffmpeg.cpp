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
            if (keepGoing != nullptr && ! keepGoing())
            {
                process.kill();
                return "Stopped.";
            }

            const auto n = process.readProcessOutput (buffer, sizeof (buffer));

            if (n > 0)
            {
                output.write (buffer, (size_t) n);
                continue;
            }

            if (! process.isRunning())
                break;

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
   #if JUCE_WINDOWS
    const auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); };
    juce::StringArray folders;
    folders.addTokens (env ("PATH"), ";", "\"");
    folders.add (env ("LOCALAPPDATA") + "/Microsoft/WinGet/Links");
    folders.add (env ("USERPROFILE") + "/scoop/shims");
    folders.add (env ("ProgramData") + "/chocolatey/bin");
    const juce::String program ("ffmpeg.exe");
   #else
    juce::StringArray folders { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin" };
    folders.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {});
    const juce::String program ("ffmpeg");
   #endif

    for (auto folder : folders)
    {
        folder = folder.trim().unquoted();

        if (! juce::File::isAbsolutePath (folder))
            continue;

        if (const auto f = juce::File (folder).getChildFile (program); f.existsAsFile())
            return f;
    }

    return {};
}

const juce::StringArray& importExtensions()
{
    static const juce::StringArray extensions { "opus", "wma", "aac", "ac3", "eac3", "wv", "ape", "tta", "mka", "mp2", "amr", "dts",
                                                "mp4", "m4v", "m4b", "mov", "mkv", "webm", "avi",
                                               #if JUCE_WINDOWS
                                                "aifc", "caf",          // Core Audio reads these on the Mac
                                               #elif ! JUCE_MAC
                                                "aifc", "m4a", "caf",
                                               #endif
                                              };
    return extensions;
}

juce::String decode (const juce::File& ffmpeg, const juce::File& source, const juce::File& wav, const std::function<bool()>& keepGoing)
{
    // The tags go into the WAV's INFO chunk. Ogg files (Opus among them) keep them on the audio stream
    // rather than on the file.
    const auto tagsFrom = source.hasFileExtension ("opus;ogg;oga;spx") ? "0:s:a:0" : "0";
    juce::StringArray arguments { "-i", source.getFullPathName(), "-map", "0:a:0", "-map_metadata", tagsFrom, "-vn" };

   #if ! JUCE_MAC
    // AC-3 starts 256 frames late, which Core Audio leaves out on the Mac (keeping whole 1,536-frame
    // blocks) and ffmpeg doesn't: the same here, so a file reads alike on every system.
    if (source.hasFileExtension ("ac3;eac3"))
        arguments.addArray ({ "-af", "atrim=start_sample=256,apad=pad_len=256" });
   #endif

    arguments.addArray ({ "-c:a", "pcm_f32le", "-rf64", "auto", "-f", "wav", wav.getFullPathName() });
    return run (ffmpeg, arguments, keepGoing);
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
                     const std::function<bool()>& keepGoing, const FileInfo& info)
{
    juce::TemporaryFile temp (target);
    juce::StringArray arguments { "-i", wav.getFullPathName(), "-c:a", t.codec };

    if (t.hasBitRate)
        arguments.addArray ({ "-b:a", juce::String (kbps) + "k" });

    arguments.addArray (FileInfoIO::ffmpegArguments (info));

    // Name the container rather than leave ffmpeg to guess it from the file name.
    arguments.addArray ({ "-f", t.extension == "m4a" ? "ipod" : t.extension, temp.getFile().getFullPathName() });

    if (const auto error = run (ffmpeg, arguments, keepGoing); error.isNotEmpty())
        return error;

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}

juce::String makePreview (const juce::File& ffmpeg, const juce::File& video, const juce::File& target, const std::function<bool()>& keepGoing)
{
    if (! target.getParentDirectory().createDirectory())
        return "Could not make the folder " + target.getParentDirectory().getFullPathName();

    juce::TemporaryFile temp (target);
    juce::String error;

   #if JUCE_WINDOWS
    // Windows Media Video, which DirectShow plays without anything installed (it can't play H.264 in MP4).
    // A key frame every 12 frames, for following the playhead.
    error = run (ffmpeg, { "-i", video.getFullPathName(), "-map", "0:v:0", "-an", "-sn", "-dn",
                           "-vf", "scale='min(1280,iw)':-2", "-pix_fmt", "yuv420p", "-g", "12", "-c:v", "wmv2", "-b:v", "6M",
                           "-f", "asf", temp.getFile().getFullPathName() }, keepGoing);
   #else
    for (const auto* encoder : { "libx264", "h264_videotoolbox" })
    {
        juce::StringArray arguments { "-i", video.getFullPathName(), "-map", "0:v:0", "-an", "-sn", "-dn",
                                      "-vf", "scale='min(1280,iw)':-2", "-pix_fmt", "yuv420p", "-g", "12", "-c:v", encoder };

        if (juce::String (encoder) == "libx264")
            arguments.addArray ({ "-preset", "veryfast", "-crf", "23" });
        else
            arguments.addArray ({ "-b:v", "6M" });

        arguments.addArray ({ "-movflags", "+faststart", "-f", "mp4", temp.getFile().getFullPathName() });
        error = run (ffmpeg, arguments, keepGoing);

        if (error.isEmpty() || error == "Stopped.")
            break;
    }
   #endif

    if (error.isNotEmpty())
        return error;

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not write " + target.getFullPathName();

    return {};
}

VideoAudio audioForVideo (const juce::File& video)
{
    if (video.hasFileExtension ("mov;avi"))  return { "pcm_s24le", "24-bit PCM", 0 };
    if (video.hasFileExtension ("mkv"))      return { "flac", "24-bit FLAC", 0 };
    if (video.hasFileExtension ("webm"))     return { "libopus", "Opus", 256 };
    return { "aac", "AAC", 320 };
}

Picture probePicture (const juce::File& ffmpeg, const juce::File& video)
{
    // ffmpeg -i with no output describes the file on stderr (and exits with an error).
    juce::ChildProcess process;
    Picture p;

    if (! process.start (juce::StringArray { ffmpeg.getFullPathName(), "-hide_banner", "-nostdin", "-i", video.getFullPathName() }))
        return p;

    const auto text = process.readAllProcessOutput();

    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();

        if (line.startsWith ("Duration:") && p.duration <= 0.0)
        {
            const auto t = juce::StringArray::fromTokens (line.fromFirstOccurrenceOf ("Duration:", false, false).upToFirstOccurrenceOf (",", false, false).trim(), ":", {});

            if (t.size() == 3)
                p.duration = t[0].getDoubleValue() * 3600.0 + t[1].getDoubleValue() * 60.0 + t[2].getDoubleValue();
        }

        if (line.startsWith ("Stream #") && line.contains ("Video:") && p.width == 0)
        {
            for (auto token : juce::StringArray::fromTokens (line.fromFirstOccurrenceOf ("Video:", false, false), ", ", {}))
            {
                if (p.width == 0 && token.containsOnly ("0123456789x") && token.contains ("x") && ! token.startsWith ("0x"))
                {
                    p.width = token.upToFirstOccurrenceOf ("x", false, false).getIntValue();
                    p.height = token.fromFirstOccurrenceOf ("x", false, false).getIntValue();
                }
            }

            const auto beforeFps = line.upToFirstOccurrenceOf (" fps", false, false);

            if (beforeFps != line)
                p.frameRate = beforeFps.fromLastOccurrenceOf (",", false, false).trim().getDoubleValue();
        }
    }

    return p;
}

juce::String recutFilter (const PictureMap& map, double soundSeconds, const Picture& picture)
{
    // Cut on whole frames: each piece starts at the frame nearest its time, and takes the source's
    // frames from the one nearest its picture's time, so no rounding builds up from piece to piece.
    const auto rate = picture.frameRate;
    const auto rateText = juce::String (rate, 6);
    const auto frameOf = [rate] (double seconds) { return (juce::int64) std::llround (seconds * rate); };
    const auto sourceFrames = (juce::int64) std::floor (picture.duration * rate + 1.0e-6);
    juce::StringArray chains, labels;

    const auto addPicture = [&] (juce::int64 from, juce::int64 count)
    {
        const auto label = "[p" + juce::String (labels.size()) + "]";
        chains.add ("[0:v]fps=" + rateText + ",trim=start_frame=" + juce::String (from) + ":end_frame=" + juce::String (from + count)
                    + ",setpts=PTS-STARTPTS,scale=" + juce::String (picture.width) + ":" + juce::String (picture.height) + ",setsar=1,format=yuv420p" + label);
        labels.add (label);
    };

    const auto addBlack = [&] (juce::int64 count)
    {
        const auto label = "[p" + juce::String (labels.size()) + "]";
        chains.add ("color=c=black:s=" + juce::String (picture.width) + "x" + juce::String (picture.height) + ":r=" + rateText
                    + ",trim=end_frame=" + juce::String (count) + ",setsar=1,format=yuv420p" + label);
        labels.add (label);
    };

    auto pieces = map.getPieces();

    if (pieces.empty())
        pieces.push_back ({ 0.0, soundSeconds, 0.0 });

    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& p = pieces[i];
        const auto end = juce::jmin (soundSeconds, i + 1 < pieces.size() ? pieces[i + 1].start : soundSeconds);   // the last runs to the sound's end
        const auto count = frameOf (end) - frameOf (p.start);

        if (count <= 0)
            continue;

        // The picture there, then black for what runs past the video's end.
        const auto from = p.picture < 0.0 ? sourceFrames : frameOf (p.picture);
        const auto shown = juce::jlimit ((juce::int64) 0, count, sourceFrames - from);

        if (shown > 0)
            addPicture (from, shown);

        if (count > shown)
            addBlack (count - shown);
    }

    chains.add (labels.joinIntoString ("") + "concat=n=" + juce::String (labels.size()) + ":v=1:a=0[v]");
    return chains.joinIntoString (";");
}

juce::String replaceAudio (const juce::File& ffmpeg, const juce::File& video, const juce::File& wav, const juce::File& target,
                           const std::function<bool()>& keepGoing, const PictureMap& map, double soundSeconds)
{
    if (target == video)
        return "The new video needs a name of its own: the original is read while it is written.";

    const auto audio = audioForVideo (video);
    const auto extension = video.getFileExtension().substring (1).toLowerCase();
    juce::TemporaryFile temp (target);
    juce::StringArray arguments { "-i", video.getFullPathName(), "-i", wav.getFullPathName() };

    if (map.isIdentity())
    {
        arguments.addArray ({ "-map", "0:v", "-map", "1:a:0", "-map_metadata", "0", "-map_chapters", "0", "-c:v", "copy" });
    }
    else
    {
        const auto picture = probePicture (ffmpeg, video);

        if (picture.width <= 0 || picture.height <= 0 || picture.frameRate <= 0.0 || picture.duration <= 0.0)
            return "The picture of " + video.getFileName() + " could not be read.";

        arguments.addArray ({ "-filter_complex", recutFilter (map, soundSeconds, picture), "-map", "[v]", "-map", "1:a:0",
                              "-map_metadata", "0", "-map_chapters", "-1" });

        if (extension == "webm")
            arguments.addArray ({ "-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "0" });
        else if (extension == "avi")
            arguments.addArray ({ "-c:v", "mpeg4", "-q:v", "2" });
        else
            arguments.addArray ({ "-c:v", "libx264", "-crf", "18", "-preset", "medium", "-pix_fmt", "yuv420p" });
    }

    arguments.addArray ({ "-c:a", audio.codec });

    if (audio.kbps > 0)
        arguments.addArray ({ "-b:a", juce::String (audio.kbps) + "k" });

    if (audio.codec == "flac")
        arguments.addArray ({ "-sample_fmt", "s32", "-bits_per_raw_sample", "24" });

    arguments.addArray ({ "-f", extension == "mkv" ? "matroska" : extension == "m4v" ? "mp4" : extension, temp.getFile().getFullPathName() });

    if (const auto error = run (ffmpeg, arguments, keepGoing); error.isNotEmpty())
        return error;

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not write " + target.getFullPathName();

    return {};
}
}
