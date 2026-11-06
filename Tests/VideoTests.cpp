// The muted player's volume is checked to be exactly 0.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioDocument.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/Ffmpeg.h"
#include "../Source/Video.h"
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

void runVideoTests()
{
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
    const auto ffprobe = ffmpeg.getSiblingFile ("ffprobe");

    if (ffmpeg == juce::File() || ! ffprobe.existsAsFile())
    {
        std::cout << "NOTE  video tests with ffmpeg skipped: no ffmpeg and ffprobe installed" << std::endl;
        return;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
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
        auto audio = 1.0;
        auto playing = false;
        panel.readPlayhead = [&] { return std::optional<std::pair<double, bool>> ({ audio, playing }); };

        juce::StringArray shown, refused;
        auto refusalSaid = true, previewsOk = true;
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
                auto* pp = panel.getPlayer();
                previewsOk = previewsOk && std::abs (pp->getVideoDuration() - 2.0) < 0.05 && pp->getVideoNativeSize() == juce::Rectangle<int> (320, 240)
                          && Video::previewFile (made[k.extension]).existsAsFile();
                previewTimes.add (Video::previewFile (made[k.extension]).getLastModificationTime().toMilliseconds());
            }
        }

        // Shown again: the preview kept is used, not made again.
        juce::Array<juce::int64> timesAgain;

        for (auto e : { "mkv", "webm" })
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
        const auto opened = p != nullptr && panel.isReady() && std::abs (p->getVideoDuration() - 2.0) < 0.05
                            && p->getVideoNativeSize() == juce::Rectangle<int> (320, 240) && p->getAudioVolume() == 0.0f;

        audio = 1.24;
        panel.update();
        TestUtils::runUntil ([&] { return p == nullptr || std::abs (p->getPlayPosition() - 1.24) < 0.001; }, 3000);
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
        TestUtils::runUntil ([&] { return p == nullptr || (! p->isPlaying() && std::abs (p->getPlayPosition() - 0.2) < 0.001); }, 3000);
        const auto stoppedAgain = p != nullptr && ! p->isPlaying() && std::abs (p->getPlayPosition() - 0.2) < 0.001;

        panel.setFile ({});
        const auto none = panel.getPlayer() == nullptr && panel.getMessage().isNotEmpty();

        check (shown.joinIntoString (",") == "mp4,m4v,mov,mkv (preview),webm (preview),avi" && refused.isEmpty() && refusalSaid
                   && previewsOk && previewTimes == timesAgain && previewTimes.size() == 2
                   && opened && std::abs (stoppedAt - 1.24) < 0.001 && wasPlaying && drift <= Video::driftAllowed && stoppedAgain && none,
               "video panel: shows MP4, M4V, MOV and an MPEG-4 AVI itself, and MKV and WebM through a preview ffmpeg makes (2 s long, "
               "320 x 240), kept and used again without being made again (shown again within " + juce::String (juce::roundToInt (reopenMs)) + " ms) (shown: "
               + shown.joinIntoString (", ") + "; not: " + refused.joinIntoString (", ") + "); "
               "320 x 240 and 2 s long, silent; stopped, it shows the frame at the playhead (" + juce::String (stoppedAt, 3) + " s for 1.24); "
               "playing, it plays along, " + juce::String (drift * 1000.0, 1) + " ms from the sound after 1.5 s; stopped again, back to "
               "the playhead; with no video, a message");
    }

    Video::setPreviewFolder ({});
    folder.deleteRecursively();
}
