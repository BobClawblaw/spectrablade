// The muted player's volume is checked to be exactly 0.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioDocument.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/Ffmpeg.h"
#include "../Source/Video.h"
#include "../Source/PictureMap.h"
#include "../Source/Session.h"
#include "../Source/OfflineRender.h"
#include "TestUtils.h"
#include "VideoTests.h"

namespace
{
    using TestUtils::check;

    /** Runs a program to the end; what it wrote (stdout and stderr), or "" when it failed. */
    juce::String runTool (const juce::StringArray& command)
    {
        juce::ChildProcess p;

        if (! p.start (command))
            return {};

        const auto out = p.readAllProcessOutput();
        return p.getExitCode() == 0 ? out : juce::String();
    }

    /** The MD5 of each picture stream's packets, as copied (not decoded). */
    juce::String pictureHash (const juce::File& ffmpeg, const juce::File& video)
    {
        return runTool ({ ffmpeg.getFullPathName(), "-v", "error", "-i", video.getFullPathName(), "-map", "0:v", "-c", "copy",
                          "-f", "streamhash", "-hash", "md5", "-" }).trim();
    }

    /** Each stream's type and codec, as "video:h264 audio:aac". */
    juce::String streams (const juce::File& ffprobe, const juce::File& video)
    {
        auto lines = juce::StringArray::fromLines (runTool ({ ffprobe.getFullPathName(), "-v", "error", "-show_entries", "stream=codec_type,codec_name",
                                                              "-of", "csv=p=0", video.getFullPathName() }).trim());
        juce::StringArray out;

        for (auto& l : lines)
            if (l.contains (","))
                out.add (l.fromFirstOccurrenceOf (",", false, false) + ":" + l.upToFirstOccurrenceOf (",", false, false));

        return out.joinIntoString (" ");
    }
}

namespace
{
    /** A session's video: where its picture is at a session time, the sound it takes, and how it
        is kept in the session file and undone. */
    void testSessionVideo()
    {
        const auto before = Video::sessionPicture (1.0, 2.5), at = Video::sessionPicture (3.25, 2.5), from0 = Video::sessionPicture (0.75, 0.0);
        const auto range = Video::sessionRange (2.5, 2.0, 48000.0);
        check (before.blank && ! at.blank && std::abs (at.picture - 0.75) < 1.0e-9 && ! from0.blank && std::abs (from0.picture - 0.75) < 1.0e-9
                   && range == juce::Range<juce::int64> (120000, 216000),
               "session video: a video starting 2.5 s in shows black at 1 s and its 0.75 s at 3.25 s, and takes the session's frames "
               "120000 to 216000 for its 2 s");

        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        juce::TemporaryFile folderTemp;
        const auto folder = folderTemp.getFile();
        const auto moved = folder.getChildFile ("Moved");
        moved.createDirectory();
        const auto video = folder.getChildFile ("Film.mp4");
        video.replaceWithText ("not read when a session is saved or opened");

        SessionState state;
        state.video = video;
        state.videoStart = 2.5;
        const auto sessionFile = folder.getChildFile ("With Video.sbsession");
        const auto saveError = SessionFile::save (state, sessionFile);
        const auto loaded = SessionFile::load (sessionFile, formats);

        // Moved with the session to another folder, the original gone: found by its relative path.
        sessionFile.copyFileTo (moved.getChildFile (sessionFile.getFileName()));
        video.copyFileTo (moved.getChildFile (video.getFileName()));
        video.deleteFile();
        const auto movedLoaded = SessionFile::load (moved.getChildFile (sessionFile.getFileName()), formats);

        const auto plainFile = folder.getChildFile ("Without.sbsession");
        SessionFile::save (SessionState(), plainFile);
        const auto plain = SessionFile::load (plainFile, formats);
        const auto xml = juce::parseXML (plainFile);

        check (saveError.isEmpty() && loaded.error.isEmpty() && loaded.state.video == video && std::abs (loaded.state.videoStart - 2.5) < 1.0e-9
                   && movedLoaded.state.video == moved.getChildFile ("Film.mp4") && plain.state.video == juce::File()
                   && xml != nullptr && xml->getChildByName ("VIDEO") == nullptr,
               "session video: kept in the session file with its start (2.5 s), found beside the session when both are moved, "
               "and a session without one has none");

        Session session;
        auto next = *session.getState();
        next.video = video;
        next.videoStart = 1.0;
        session.apply (std::move (next), "Attach Video");
        const auto attached = session.getState()->video == video;
        session.undo();
        check (attached && session.getState()->video == juce::File() && juce::exactlyEqual (session.getState()->videoStart, 0.0),
               "session video: attaching one is a step to undo");

        folder.deleteRecursively();
    }
}

void runVideoTests()
{
    testSessionVideo();

    // Following the playhead, against a player that starts 0.1 s after it is told to (as AVPlayer
    // was measured to, about 0.107 s), looked at 30 times a second.
    {
        struct FakePlayer
        {
            double latency = 0.1, from = 0.0, startAt = 0.0;
            bool playing = false;
            double at (double now) const      { return playing && now > startAt ? from + (now - startAt) : from; }
        };

        Video::Follower follower;
        FakePlayer fake;
        auto seeks = 0;

        // Plays the sound from `start` for `seconds`; the worst drift over its last second.
        const auto play = [&] (double& now, double start, double seconds)
        {
            auto worst = 0.0;

            for (double t = 0.0; t < seconds; t += 1.0 / 30.0, now += 1.0 / 30.0)
            {
                const auto audio = start + t;
                const auto f = follower.next (audio, true, fake.at (now), fake.playing, 10.0);

                if (f.seek)
                {
                    ++seeks;
                    fake.from = f.to;
                    fake.startAt = now + fake.latency;
                }

                if (f.play)
                {
                    fake.from = fake.at (now);
                    fake.playing = true;
                    fake.startAt = now + fake.latency;
                }

                if (t > seconds - 1.0)
                    worst = juce::jmax (worst, std::abs (fake.at (now) - audio));
            }

            return worst;
        };

        auto now = 0.0;
        const auto first = play (now, 1.0, 3.0);
        const auto firstSeeks = seeks;
        const auto learned = follower.getLead();

        const auto stopped = follower.next (4.0, false, fake.at (now), true, 10.0);
        fake.playing = false;
        fake.from = stopped.to;
        seeks = 0;
        const auto second = play (now, 2.0, 1.5);

        Video::Follower fresh;
        const auto moved = fresh.next (2.0, false, 1.0, false, 10.0);
        const auto still = fresh.next (2.0, false, 2.0, false, 10.0);
        const auto pastEnd = fresh.next (12.0, true, 9.9, true, 10.0);

        check (first <= Video::driftAllowed && firstSeeks <= 3 && std::abs (learned - 0.1) < 0.034 && stopped.stop && stopped.seek && stopped.to == 4.0
                   && second <= Video::driftAllowed && seeks == 1 && moved.seek && moved.to == 2.0 && ! moved.play && ! still.seek && ! still.stop
                   && pastEnd.stop && pastEnd.seek && pastEnd.to == 10.0,
               "video: with a player starting 0.1 s late, the picture learns to aim " + juce::String (learned, 3) + " s ahead in "
               + juce::String (firstSeeks) + " seeks and stays within " + juce::String (first * 1000.0, 1) + " ms of the sound; played "
               "again, it is within " + juce::String (second * 1000.0, 1) + " ms with one seek; stopped, it shows the frame at the "
               "sound's place when that changes; past its end it stops on its last frame");
    }

    // The picture following edits that change the sound's length.
    {
        const auto rate = 1000.0;   // a frame a millisecond, to read the times easily
        juce::AudioBuffer<float> b (1, 10000);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float) std::sin (0.01 * i));

        AudioDocument doc;
        doc.reset (AudioEdits::fromBuffer (std::move (b)), rate, {}, 24);
        const auto describe = [] (const PictureMap& m)
        {
            if (m.isIdentity())
                return juce::String ("identity");

            juce::StringArray parts;

            for (auto& p : m.getPieces())
                parts.add (juce::String (p.start, 1) + "+" + juce::String (p.length, 1) + (p.picture < 0.0 ? juce::String (" black") : "@" + juce::String (p.picture, 1)));

            return parts.joinIntoString (" ");
        };

        juce::StringArray steps;
        const auto note = [&] { steps.add (describe (doc.getPictureMap())); };

        doc.apply (AudioEdits::gain (*doc.getSnapshot(), 1000, 2000, -6.0f), "Gain");                 // in place
        note();
        doc.apply (AudioEdits::remove (*doc.getSnapshot(), 2000, 3000), "Delete");                    // 2-3 s cut
        note();
        doc.apply (AudioEdits::insertSilence (*doc.getSnapshot(), 500, 500), "Insert Silence");        // 0.5 s of nothing at 0.5
        note();
        const auto clip = AudioEdits::copy (*doc.getSnapshot(), 8000, 9000, rate);                    // the picture from 8.5 s
        doc.apply (AudioEdits::replaceWithClip (*doc.getSnapshot(), 0, 0, clip), "Paste");            // pasted at the start
        note();
        doc.apply (AudioEdits::fadeOut (*doc.getSnapshot(), 9000, 10500), "Fade Out");                 // in place again
        note();
        doc.undo();
        doc.undo();
        note();
        doc.redo();
        note();
        doc.applyNewRate (AudioEdits::remove (*doc.getSnapshot(), 0, 0), 2000.0, doc.getMarkers(), "Convert");   // the same time at another rate
        note();
        const auto saved = PictureMap::fromString (doc.getPictureMap().toString());

        const auto map = doc.getPictureMap();
        const auto at = [&] (double t) { const auto p = map.pictureAt (t); return p.has_value() ? juce::String (*p, 2) : juce::String ("black"); };

        check (steps.joinIntoString (" | ") == "identity | 0.0+2.0@0.0 2.0+7.0@3.0 | 0.0+0.5@0.0 0.5+0.5 black 1.0+1.5@0.5 2.5+7.0@3.0"
                                                " | 0.0+1.0@8.5 1.0+0.5@0.0 1.5+0.5 black 2.0+1.5@0.5 3.5+7.0@3.0"
                                                " | 0.0+1.0@8.5 1.0+0.5@0.0 1.5+0.5 black 2.0+1.5@0.5 3.5+7.0@3.0"
                                                " | 0.0+0.5@0.0 0.5+0.5 black 1.0+1.5@0.5 2.5+7.0@3.0"
                                                " | 0.0+1.0@8.5 1.0+0.5@0.0 1.5+0.5 black 2.0+1.5@0.5 3.5+7.0@3.0"
                                                " | 0.0+1.0@8.5 1.0+0.5@0.0 1.5+0.5 black 2.0+1.5@0.5 3.5+7.0@3.0"
                   && saved == map && at (0.25) == "8.75" && at (1.25) == "0.25" && at (1.75) == "black" && at (4.0) == "3.50" && at (12.0) == "11.50",
               "video: the picture follows edits that change the sound's length: a cut jumps it, inserted silence has none (black), audio "
               "pasted from elsewhere brings its picture, edits in place and a rate conversion leave it, undo and redo take it back and "
               "forth, and it is saved and read back as text (steps: " + steps.joinIntoString (" | ") + ")");
    }

    // The video a file remembers.
    {
        const auto movie = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Scene.MOV");
        const auto sound = movie.withFileExtension ("wav");
        const auto other = movie.withFileExtension ("mp4");
        auto one = AudioEdits::fromBuffer (juce::AudioBuffer<float> (2, 10));
        AudioDocument a, b;
        a.reset (one, 48000.0, movie, 24);
        b.reset (one, 48000.0, sound, 24);
        const auto fromVideo = a.getVideo() == movie && b.getVideo() == juce::File();
        b.setVideo (other);
        a.markSaved (sound, 24);
        const auto keptOnSave = a.getVideo() == movie;
        a.swapWith (b);
        const auto swapped = a.getVideo() == other && b.getVideo() == movie;
        b.reset (one, 48000.0, sound, 24);

        check (Video::isVideoFile (movie) && Video::isVideoFile (other) && ! Video::isVideoFile (sound) && ! Video::isVideoFile (movie.withFileExtension ("m4a"))
                   && fromVideo && keptOnSave && swapped && b.getVideo() == juce::File(),
               "video: a file opened from a video (MP4, M4V, MOV, MKV, WebM, AVI, any case) remembers it, through Save As and switching "
               "files; one attached is kept the same way; opening another file forgets it");
    }

    const auto ffmpeg = Ffmpeg::find();
    const auto ffprobe = ffmpeg.getSiblingFile ("ffprobe" + ffmpeg.getFileExtension());   // ffprobe.exe on Windows

    if (ffmpeg == juce::File() || ! ffprobe.existsAsFile())
    {
        std::cout << "NOTE  video tests with ffmpeg skipped: no ffmpeg and ffprobe installed" << std::endl;
        return;
    }

    juce::AudioFormatManager formats;
    AudioFileIO::registerFormats (formats);
    juce::TemporaryFile folderTemp;
    const auto folder = folderTemp.getFile();
    folder.createDirectory();

    // The new sound: 2 s at 48 kHz, a different tone on each side, made here.
    const auto rate = 48000.0;
    const int n = 96000;
    juce::AudioBuffer<float> fresh (2, n);

    for (int i = 0; i < n; ++i)
    {
        fresh.setSample (0, i, (float) (0.4 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate)));
        fresh.setSample (1, i, (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 660.0 * i / rate)));
    }

    const auto freshWav = folder.getChildFile ("fresh.wav");
    AudioFileIO::save (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (fresh)), rate, freshWav, 32, formats);

    // A test video of each type: 2 s of a moving test picture, 320 x 240 at 25 frames a second,
    // with a 440 Hz tone as its sound.
    struct Kind { juce::String extension; juce::StringArray codecs; juce::String picture, sound; };
    const std::vector<Kind> kinds { { "mp4", { "-c:v", "libx264", "-c:a", "aac" }, "h264", "aac" },
                                    { "m4v", { "-c:v", "libx264", "-c:a", "aac", "-f", "mp4" }, "h264", "aac" },
                                    { "mov", { "-c:v", "libx264", "-c:a", "aac" }, "h264", "pcm_s24le" },
                                    { "mkv", { "-c:v", "libx264", "-c:a", "aac" }, "h264", "flac" },
                                    { "webm", { "-c:v", "libvpx-vp9", "-c:a", "libopus" }, "vp9", "opus" },
                                    { "avi", { "-c:v", "mpeg4", "-c:a", "pcm_s16le" }, "mpeg4", "pcm_s24le" } };
    std::map<juce::String, juce::File> made;

    for (auto& k : kinds)
    {
        const auto video = folder.getChildFile ("Clip." + k.extension);
        juce::StringArray command { ffmpeg.getFullPathName(), "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=2",
                                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2", "-ac", "2", "-pix_fmt", "yuv420p" };
        command.addArray (k.codecs);
        command.add (video.getFullPathName());
        runTool (command);

        if (! video.existsAsFile())
        {
            check (false, "video: ffmpeg made a test ." + k.extension + " video");
            continue;
        }

        made[k.extension] = video;

        // Opened: its sound. Then new sound put into a copy.
        const auto opened = AudioFileIO::load (video, formats);
        const auto target = folder.getChildFile ("Clip (new sound)." + k.extension);
        const auto error = Ffmpeg::replaceAudio (ffmpeg, video, freshWav, target);
        const auto back = AudioFileIO::load (target, formats);
        const auto expected = "video:" + k.picture + " audio:" + k.sound;
        const auto got = streams (ffprobe, target);

        // How close the sound read back is to what went in: the worst sample error, and the
        // signal-to-error ratio over the middle second (clear of the encoders' edges).
        auto worst = 0.0, signal = 0.0, noise = 0.0;
        auto frames = 0;

        if (back.snapshot != nullptr)
        {
            frames = (int) back.snapshot->getNumFrames();
            juce::AudioBuffer<float> b (back.snapshot->getNumChannels(), frames);
            back.snapshot->read (b, 0, 0, frames);

            for (int ch = 0; ch < 2 && ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < juce::jmin (n, frames); ++i)
                {
                    const auto e = (double) b.getSample (ch, i) - fresh.getSample (ch, i);
                    worst = juce::jmax (worst, std::abs (e));

                    if (i >= 24000 && i < 72000)
                    {
                        signal += (double) fresh.getSample (ch, i) * fresh.getSample (ch, i);
                        noise += e * e;
                    }
                }
        }

        const auto snr = noise > 0.0 ? 10.0 * std::log10 (signal / noise) : 999.0;
        const auto lossless = Ffmpeg::audioForVideo (video).kbps == 0;
        const auto soundOk = back.snapshot != nullptr && back.snapshot->getNumChannels() == 2 && std::abs (back.sampleRate - rate) < 0.5
                             && (lossless ? frames == n && worst <= 1.0 / 8388608.0 : std::abs (frames - n) <= 2048 && snr > 20.0);

        check (opened.snapshot != nullptr && std::abs (opened.snapshot->getNumFrames() - n) <= 2048 && error.isEmpty() && got == expected
                   && pictureHash (ffmpeg, target).isNotEmpty() && pictureHash (ffmpeg, target) == pictureHash (ffmpeg, video) && soundOk,
               "video ." + k.extension + ": its sound opens, and the new sound goes into a copy with the picture's packets unchanged "
               "(same MD5) and one sound track, " + got + ", " + (lossless ? "within 1 step of 24 bits (worst " + juce::String (worst * 8388608.0, 2) + " steps)"
                                                                           : juce::String (snr, 1) + " dB signal to error over its middle second")
               + ", " + juce::String (frames) + " frames back of " + juce::String (n) + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
    }

    // Cut to follow edits: a 4 s, 25 frame-a-second video whose frame N is a flat grey of 30 + 2N (up to 228, inside video's 16-235),
    // so each frame written can be told by its brightness (black is 16), to within a third of a step.
    {
        const auto video = folder.getChildFile ("Counted.mp4");
        runTool ({ ffmpeg.getFullPathName(), "-v", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=16x16:r=25:d=4",
                   "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=4", "-ac", "2",
                   "-vf", "geq=lum='30+2*N':cb=128:cr=128", "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", "-c:a", "aac", video.getFullPathName() });

        AudioDocument doc;
        auto opened = AudioFileIO::load (video, formats);
        const auto soundRate = opened.sampleRate;
        doc.reset (opened.snapshot, soundRate, video, 32);
        const auto f = [&] (double s) { return (juce::int64) std::llround (s * soundRate); };
        doc.apply (AudioEdits::remove (*doc.getSnapshot(), f (1.0), f (2.0)), "Delete");          // 1-2 s cut
        doc.apply (AudioEdits::insertSilence (*doc.getSnapshot(), f (0.5), f (0.4)), "Silence");  // 0.4 s of black at 0.5 s
        const auto map = doc.getPictureMap();
        const auto seconds = (double) doc.getNumFrames() / soundRate;

        const auto wav = folder.getChildFile ("counted.wav");
        AudioFileIO::save (*doc.getSnapshot(), soundRate, wav, 32, formats);
        const auto target = folder.getChildFile ("Counted (new sound).mp4");
        const auto error = Ffmpeg::replaceAudio (ffmpeg, video, wav, target, {}, map, seconds);

        // Each frame written, as its mean brightness, against what the map says it should show.
        juce::ChildProcess decode;
        juce::MemoryOutputStream raw;

        if (decode.start (juce::StringArray { ffmpeg.getFullPathName(), "-v", "error", "-i", target.getFullPathName(), "-map", "0:v",
                                              "-f", "rawvideo", "-pix_fmt", "gray", "-" }, juce::ChildProcess::wantStdOut))
        {
            char buffer[8192];

            for (int got; (got = decode.readProcessOutput (buffer, sizeof (buffer))) > 0;)
                raw.write (buffer, (size_t) got);
        }

        const auto frames = (int) (raw.getDataSize() / 256);
        auto wrong = 0;
        juce::StringArray firstWrong;

        for (int k = 0; k < frames; ++k)
        {
            const auto* px = static_cast<const juce::uint8*> (raw.getData()) + k * 256;
            auto sum = 0;

            for (int i = 0; i < 256; ++i)
                sum += px[i];

            const auto mean = sum / 256.0;
            // The piece this frame is in (each starts at the frame nearest its time), and the source frame
            // it should show: the one nearest the piece's picture time, counted on from there.
            const auto frameOf = [] (double s) { return (int) std::llround (s * 25.0); };
            const PictureMap::Piece* piece = nullptr;

            for (auto& p : map.getPieces())
                if (frameOf (p.start) <= k)
                    piece = &p;

            const auto source = piece == nullptr ? k : (piece->picture < 0.0 ? -1 : frameOf (piece->picture) + k - frameOf (piece->start));

            // Video brightness 16-235 read back as full-range grey: (Y - 16) * 255 / 219, 2.33 a frame.
            const auto luma = source >= 0 ? 30.0 + 2.0 * source : 16.0;
            const auto expected = (luma - 16.0) * 255.0 / 219.0;

            if (std::abs (mean - expected) > 1.0)
            {
                ++wrong;

                if (firstWrong.size() < 4)
                    firstWrong.add (juce::String (k) + ":" + juce::String (mean, 1) + "/" + juce::String (expected, 1));
            }
        }

        check (error.isEmpty() && frames == 85 && wrong == 0 && streams (ffprobe, target) == "video:h264 audio:aac",
               "video: with 1 s of sound cut and 0.4 s of silence inserted, the picture is cut to follow on whole frames: " + juce::String (frames)
               + " frames for 3.4 s, each one the source frame the map gives, or black (" + juce::String (wrong) + " wrong, read by brightness), "
               "encoded again as H.264 with the sound" + (firstWrong.isEmpty() ? juce::String() : " (" + firstWrong.joinIntoString (" ") + ")")
               + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
    }

    // Writing over the video itself is refused, and the video is left as it was.
    if (made.count ("mp4") > 0)
    {
        const auto video = made["mp4"];
        const auto before = video.getSize();
        const auto error = Ffmpeg::replaceAudio (ffmpeg, video, freshWav, video);
        check (error.isNotEmpty() && video.getSize() == before, "video: putting new sound into the video itself, in place, is refused");
    }

    // The Video panel: shows a video and follows the playhead. Previews go to a folder of the test's own.
    Video::setPreviewFolder (folder.getChildFile ("Previews"));

    {
        Video::Panel panel;
        panel.setSize (400, 320);

       #if JUCE_WINDOWS
        // DirectShow draws into a window of its own inside the panel's, so the panel has to be on the
        // screen (in the app it is in the main window).
        panel.setVisible (true);
        panel.addToDesktop (juce::ComponentPeer::windowIsTemporary);
       #endif
        auto audio = 1.0;
        auto playing = false;
        panel.readPlayhead = [&] { return std::optional<Video::Panel::Playhead> ({ audio, playing, false }); };

        // DirectShow, which plays video on Windows, has no MP4 or QuickTime reader: only AVI is shown as it is there.
       #if JUCE_WINDOWS
        const juce::StringArray previewed { "mp4", "m4v", "mov", "mkv", "webm" };
        const juce::String expectedShown ("mp4 (preview),m4v (preview),mov (preview),mkv (preview),webm (preview),avi");
       #else
        const juce::StringArray previewed { "mkv", "webm" };
        const juce::String expectedShown ("mp4,m4v,mov,mkv (preview),webm (preview),avi");
       #endif
        juce::StringArray shown, refused;
        auto refusalSaid = true, previewsOk = true;
        juce::String previewSeen;   // a preview that wasn't right: its length and size
        juce::Array<juce::int64> previewTimes;
        auto reopenMs = 0.0;

        for (auto& k : kinds)
        {
            if (made.count (k.extension) == 0)
                continue;

            panel.setFile (made[k.extension]);
            TestUtils::runUntil ([&] { panel.update(); return panel.isReady() || panel.getMessage().contains ("can't be shown"); }, 30000);
            (panel.isReady() ? shown : refused).add (k.extension + (panel.isShowingPreview() ? " (preview)" : ""));
            refusalSaid = refusalSaid && (panel.isReady() || panel.getMessage().contains ("can't be shown"));

            if (panel.isShowingPreview() && panel.isReady())
            {
                // The player can still be reading the preview's length and size when the panel is
                // ready (seen on a busy machine): given a few seconds to have them.
                auto* pp = panel.getPlayer();
                const auto previewRight = [pp] { return std::abs (pp->getVideoDuration() - 2.0) < 0.05 && pp->getVideoNativeSize() == juce::Rectangle<int> (320, 240); };
                TestUtils::runUntil (previewRight, 5000);
                previewsOk = previewsOk && previewRight() && Video::previewFile (made[k.extension]).existsAsFile();

                if (! previewRight())
                    previewSeen << k.extension << ": " << juce::String (pp->getVideoDuration(), 3) << " s, " << pp->getVideoNativeSize().toString() << "; ";
                previewTimes.add (Video::previewFile (made[k.extension]).getLastModificationTime().toMilliseconds());
            }
        }

        // Shown again: the preview kept is used, not made again.
        juce::Array<juce::int64> timesAgain;

        for (auto e : previewed)
            if (made.count (e) > 0)
            {
                panel.setFile ({});
                const auto start = juce::Time::getMillisecondCounterHiRes();
                panel.setFile (made[e]);
                TestUtils::runUntil ([&] { panel.update(); return panel.isReady(); }, 8000);
                reopenMs = juce::jmax (reopenMs, juce::Time::getMillisecondCounterHiRes() - start);
                timesAgain.add (Video::previewFile (made[e]).getLastModificationTime().toMilliseconds());
            }

        panel.setFile (made.count ("mp4") > 0 ? made["mp4"] : juce::File());
        TestUtils::runUntil ([&] { panel.update(); return panel.isReady(); }, 8000);
        auto* p = panel.getPlayer();
        TestUtils::runUntil ([p] { return p == nullptr || (std::abs (p->getVideoDuration() - 2.0) < 0.05 && ! p->getVideoNativeSize().isEmpty()); }, 5000);
        const auto opened = p != nullptr && panel.isReady() && std::abs (p->getVideoDuration() - 2.0) < 0.05
                            && p->getVideoNativeSize() == juce::Rectangle<int> (320, 240)
                            && (p->getAudioVolume() == 0.0f || panel.isShowingPreview());   // a preview has no sound to turn down
        const auto openedSeen = p == nullptr ? juce::String ("opened (no player)")
                                             : "opened (" + juce::String (p->getVideoDuration(), 3) + " s, " + p->getVideoNativeSize().toString()
                                                   + ", volume " + juce::String (p->getAudioVolume(), 2) + ")";

        audio = 1.24;
        panel.update();
        TestUtils::runUntil ([&] { return p == nullptr || std::abs (p->getPlayPosition() - 1.24) < 0.001; }, 8000);
        const auto stoppedAt = p != nullptr ? p->getPlayPosition() : -1.0;

        // Playing: the picture plays along; how far off it is after 1.5 s (having learned how late
        // the player starts).
        playing = true;
        audio = 0.2;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        panel.update();
        TestUtils::runUntil ([&]
        {
            audio = 0.2 + (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
            panel.update();
            return juce::Time::getMillisecondCounterHiRes() - start > 1500.0;
        }, 3000);
        const auto drift = p != nullptr ? std::abs (p->getPlayPosition() - audio) : 99.0;
        const auto wasPlaying = p != nullptr && p->isPlaying();

        playing = false;
        audio = 0.2;
        panel.update();
        TestUtils::runUntil ([&] { return p == nullptr || (! p->isPlaying() && std::abs (p->getPlayPosition() - 0.2) < 0.001); }, 8000);
        const auto stoppedAgain = p != nullptr && ! p->isPlaying() && std::abs (p->getPlayPosition() - 0.2) < 0.001;

        panel.setFile ({});
        const auto none = panel.getPlayer() == nullptr && panel.getMessage().isNotEmpty();

        // Named when they fail, so a failure on a busy machine says which part.
        juce::StringArray failed;

        for (auto [ok, part] : std::initializer_list<std::pair<bool, juce::String>> { { shown.joinIntoString (",") == expectedShown && refused.isEmpty(), "shown" },
                                 { refusalSaid, "refusal" }, { previewsOk, previewSeen.isEmpty() ? juce::String ("previews") : "previews (" + previewSeen.trimEnd() + ")" },
                                 { previewTimes == timesAgain && previewTimes.size() == previewed.size(), "previews kept" }, { opened, openedSeen },
                                 { std::abs (stoppedAt - 1.24) < 0.001, "stopped" }, { wasPlaying, "playing" },
                                 { drift <= Video::driftAllowed, "drift" }, { stoppedAgain, "stopped again" }, { none, "none" } })
            if (! ok)
                failed.add (part);

        check (failed.isEmpty(),
               "video panel: shows MP4, M4V, MOV and an MPEG-4 AVI itself, and MKV and WebM through a preview ffmpeg makes (2 s long, "
               "320 x 240), kept and used again without being made again (shown again within " + juce::String (juce::roundToInt (reopenMs)) + " ms) (shown: "
               + shown.joinIntoString (", ") + "; not: " + refused.joinIntoString (", ") + "); "
               "320 x 240 and 2 s long, silent; stopped, it shows the frame at the playhead (" + juce::String (stoppedAt, 3) + " s for 1.24); "
               "playing, it plays along, " + juce::String (drift * 1000.0, 1) + " ms from the sound after 1.5 s; stopped again, back to "
               "the playhead; with no video, a message" + (failed.isEmpty() ? juce::String() : " (failed: " + failed.joinIntoString (", ") + ")"));
    }

    // A session's mix into its video: from the video's start for its length. A stereo tone from
    // 1 s in, the video starting at 0.5 s, so the tone comes in half a second into the new video.
    if (made.count ("mov") > 0)
    {
        SessionState state;
        state.sampleRate = rate;
        juce::AudioBuffer<float> tone (2, 3 * 48000);

        for (int i = 0; i < tone.getNumSamples(); ++i)
        {
            tone.setSample (0, i, (float) (0.4 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate)));
            tone.setSample (1, i, (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 550.0 * i / rate)));
        }

        SessionClip clip;
        clip.id = state.nextId++;
        clip.name = "tone";
        clip.audio = AudioEdits::fromBuffer (std::move (tone));
        clip.position = 48000;
        clip.length = clip.audio->getNumFrames();
        state.addTrack().clips.push_back (clip);
        state.video = made["mov"];
        state.videoStart = 0.5;

        const auto target = folder.getChildFile ("Clip (mix).mov");
        const auto error = Video::saveMixInto (state, nullptr, ffmpeg, target, formats);
        const auto back = AudioFileIO::load (target, formats);
        juce::AudioBuffer<float> expected;
        OfflineRender::mixSession (state, { 24000, 24000 + n }, expected, nullptr, {});

        auto worst = 1.0;
        auto firstSound = -1;

        if (back.snapshot != nullptr && back.snapshot->getNumFrames() == n && expected.getNumSamples() == n)
        {
            worst = 0.0;
            juce::AudioBuffer<float> b (2, n);
            back.snapshot->read (b, 0, 0, n);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    worst = juce::jmax (worst, std::abs ((double) b.getSample (ch, i) - expected.getSample (ch, i)));

                    if (firstSound < 0 && std::abs (b.getSample (ch, i)) > 1.0e-4f)
                        firstSound = i;
                }
        }

        check (error.isEmpty() && worst <= 1.0 / 8388608.0 && firstSound >= 24000 && firstSound < 24000 + 48
                   && pictureHash (ffmpeg, target).isNotEmpty() && pictureHash (ffmpeg, target) == pictureHash (ffmpeg, made["mov"]),
               "session video: the mix from the video's start (0.5 s) for its 2 s goes into a copy with the picture's packets unchanged "
               "(same MD5), the session's own mix of that time within 1 step of 24 bits (worst " + juce::String (worst * 8388608.0, 2)
                   + " steps), the tone coming in at frame " + juce::String (firstSound) + " (24000 is 0.5 s)"
                   + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));

        const auto stoppedTarget = folder.getChildFile ("Clip (stopped).mov");
        const auto stopped = Video::saveMixInto (state, nullptr, ffmpeg, stoppedTarget, formats, [] (float) { return false; });
        auto missing = state;
        missing.video = folder.getChildFile ("Gone.mov");
        const auto sameName = Video::saveMixInto (state, nullptr, ffmpeg, made["mov"], formats);
        check (stopped == "Stopped." && ! stoppedTarget.exists() && Video::saveMixInto (missing, nullptr, ffmpeg, target, formats).isNotEmpty()
                   && sameName.isNotEmpty() && pictureHash (ffmpeg, made["mov"]).isNotEmpty(),
               "session video: stopping writes nothing, and a missing video or writing over the video itself is refused");
    }

    Video::setPreviewFolder ({});
    folder.deleteRecursively();
}
