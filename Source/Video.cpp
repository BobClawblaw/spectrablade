#include "Video.h"
#include "Theme.h"
#include "Ffmpeg.h"
#include "OfflineRender.h"
#include "AudioFileIO.h"
#include "AudioEdits.h"

namespace Video
{
const juce::StringArray& extensions()
{
    static const juce::StringArray list { "mp4", "m4v", "mov", "mkv", "webm", "avi" };
    return list;
}

bool isVideoFile (const juce::File& f)
{
    return extensions().contains (f.getFileExtension().substring (1).toLowerCase());
}

SessionPicture sessionPicture (double sessionSeconds, double videoStart)
{
    const auto at = sessionSeconds - videoStart;
    return at < 0.0 ? SessionPicture { 0.0, true } : SessionPicture { at, false };
}

juce::Range<juce::int64> sessionRange (double videoStart, double videoSeconds, double sampleRate)
{
    const auto start = (juce::int64) std::llround (juce::jmax (0.0, videoStart) * sampleRate);
    return { start, start + (juce::int64) std::llround (juce::jmax (0.0, videoSeconds) * sampleRate) };
}

juce::String saveMixInto (const SessionState& state, SessionMixer::Inserts* inserts, const juce::File& ffmpeg, const juce::File& target,
                          juce::AudioFormatManager& formats, const std::function<bool (float)>& progress)
{
    const auto keepGoing = [&progress] (float p) { return progress == nullptr || progress (p); };

    if (! state.video.existsAsFile())
        return "The session's video " + state.video.getFullPathName() + " was not found.";

    if (target == state.video)
        return "The new video needs a name of its own: the original is read while it is written.";

    const auto picture = Ffmpeg::probePicture (ffmpeg, state.video);

    if (picture.duration <= 0.0)
        return "The length of " + state.video.getFileName() + " could not be read.";

    juce::AudioBuffer<float> mix;

    if (! OfflineRender::mixSession (state, sessionRange (state.videoStart, picture.duration, state.sampleRate), mix, inserts,
                                     [&keepGoing] (float p) { return keepGoing (p * 0.6f); }))
        return "Stopped.";

    juce::TemporaryFile wav (".wav");

    if (auto error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), state.sampleRate, wav.getFile(), 32, formats); error.isNotEmpty())
        return error;

    if (! keepGoing (0.6f))
        return "Stopped.";

    return Ffmpeg::replaceAudio (ffmpeg, state.video, wav.getFile(), target, [&keepGoing] { return keepGoing (0.6f); });
}

bool needsPreview (const juce::File& f)
{
    return f.hasFileExtension ("mkv;webm");
}

namespace
{
    juce::File& previewFolderSetting()
    {
        static juce::File folder;
        return folder;
    }
}

juce::File previewFolder()
{
    if (previewFolderSetting() != juce::File())
        return previewFolderSetting();

    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Caches/SpectraBlade/Video Previews");
}

void setPreviewFolder (const juce::File& folder)
{
    previewFolderSetting() = folder;
}

juce::File previewFile (const juce::File& video)
{
    const auto key = video.getFullPathName() + "|" + juce::String (video.getSize()) + "|" + juce::String (video.getLastModificationTime().toMilliseconds());
    return previewFolder().getChildFile (video.getFileNameWithoutExtension() + " " + juce::String::toHexString (key.hashCode64()) + ".mp4");
}

Follow Follower::next (double audio, bool audioPlaying, double picture, bool picturePlaying, double duration)
{
    const auto end = juce::jmax (0.0, duration);
    const auto target = juce::jlimit (0.0, end, audio);
    const auto changed = ! juce::exactlyEqual (audio, lastAudio);
    lastAudio = audio;
    Follow f;

    if (audioPlaying && audio < end)
    {
        const auto aim = [&]
        {
            seekedTo = juce::jmin (end, target + lead);
            settling = true;
            settleStart = audio;
            return seekedTo;
        };

        if (! picturePlaying)
            return { true, aim(), true, false };

        // Just told where to go: judged once it moves on from there (a seek takes a moment, so it
        // may still be where it was). If it doesn't within half a second, told again.
        if (settling)
        {
            const auto moved = picture > seekedTo + 0.001 && picture < seekedTo + 0.5;

            if (! moved)
                return audio - settleStart < 0.5 ? f : Follow { true, aim(), false, false };
        }

        settling = false;
        const auto drift = picture - target;

        if (std::abs (drift) > driftAllowed)
        {
            lead = juce::jlimit (0.0, 0.5, lead - drift);
            return { true, aim(), false, false };
        }

        return f;
    }

    settling = false;
    f.stop = picturePlaying;

    if (changed || f.stop)
    {
        f.seek = true;
        f.to = target;
    }

    return f;
}

//==============================================================================
/** Makes a video's preview with ffmpeg on a thread of its own, then hands it to the panel. */
class Panel::PreviewMaker final : public juce::Thread
{
public:
    PreviewMaker (Panel& p, juce::File ff, juce::File v, juce::File t)
        : juce::Thread ("Video preview"), panel (&p), ffmpeg (std::move (ff)), video (std::move (v)), target (std::move (t))
    {
        startThread();
    }

    ~PreviewMaker() override   { stopThread (10000); }

    void run() override
    {
        const auto error = Ffmpeg::makePreview (ffmpeg, video, target, [this] { return ! threadShouldExit(); });

        if (! threadShouldExit())
            juce::MessageManager::callAsync ([safe = panel, v = video, t = target, error]
            {
                if (safe != nullptr)
                    safe->previewMade (v, t, error);
            });
    }

private:
    juce::Component::SafePointer<Panel> panel;
    juce::File ffmpeg, video, target;
};

//==============================================================================
Panel::Panel()
{
    message = "No video. Open a video file, or attach one to this file with File > Attach Video...";
    startTimerHz (30);
}

Panel::~Panel()
{
    stopTimer();
    maker.reset();
}

void Panel::setFile (const juce::File& f)
{
    if (f == file)
        return;

    file = f;
    caption.clear();
    follower.restart();
    ticksWaiting = 0;
    usingPreview = false;
    maker.reset();

    if (player != nullptr)
    {
        removeChildComponent (player.get());
        player.reset();
    }

    if (file == juce::File())
    {
        message = "No video. Open a video file, or attach one to this file with File > Attach Video...";
        repaint();
        return;
    }

    if (! file.existsAsFile())
    {
        message = file.getFileName() + " isn't there any more.";
        repaint();
        return;
    }

    if (needsPreview (file))
        startPreview();
    else
        load (file);
}

void Panel::load (const juce::File& played)
{
    ticksWaiting = 0;
    follower.restart();
    player = std::make_unique<juce::VideoComponent> (false);
    addAndMakeVisible (*player);
    resized();
    // Not from inside the player's own call: failing deletes it.
    player->onErrorOccurred = [safe = juce::Component::SafePointer<Panel> (this)] (const juce::String& error)
    {
        juce::MessageManager::callAsync ([safe, error] { if (safe != nullptr) safe->failed (error); });
    };

    if (const auto result = player->load (played); result.failed())
    {
        failed (result.getErrorMessage());
        return;
    }

    player->setAudioVolume (0.0f);
    message = "Opening " + file.getFileName() + "...";
    repaint();
}

void Panel::startPreview()
{
    usingPreview = true;

    if (const auto preview = previewFile (file); preview.existsAsFile())
    {
        load (preview);
        return;
    }

    const auto ffmpeg = Ffmpeg::find();

    if (ffmpeg == juce::File())
    {
        failed ("showing it needs ffmpeg, which isn't installed");
        return;
    }

    message = "Making a preview of " + file.getFileName() + "'s picture, which macOS can't show itself...";
    repaint();
    maker = std::make_unique<PreviewMaker> (*this, ffmpeg, file, previewFile (file));
}

void Panel::previewMade (const juce::File& video, const juce::File& preview, const juce::String& error)
{
    if (video != file)
        return;   // another video since

    maker.reset();

    if (error.isNotEmpty())
        failed ("its preview could not be made: " + error);
    else
        load (preview);
}

void Panel::failed (const juce::String& why)
{
    if (player != nullptr)
    {
        removeChildComponent (player.get());
        player.reset();
    }

    // macOS couldn't play it: a preview made by ffmpeg, if one hasn't been tried.
    if (! usingPreview && file.existsAsFile() && Ffmpeg::find() != juce::File())
    {
        startPreview();
        return;
    }

    message = "The picture of " + file.getFileName() + " can't be shown" + (why.isNotEmpty() ? ": " + why : juce::String (".")) + "\n\n"
              "Its sound can still be edited and put back into it.";
    caption.clear();
    repaint();
}

bool Panel::isReady() const
{
    if (player == nullptr || ! player->isVideoOpen())
        return false;

    const auto duration = player->getVideoDuration();
    return std::isfinite (duration) && duration > 0.0;
}

void Panel::update()
{
    if (readVideo != nullptr)
        setFile (readVideo());

    if (player == nullptr || ! player->isVideoOpen())
        return;

    // Until macOS has read the video, nothing to follow; one it can't read never gets ready.
    if (! isReady())
    {
        if (++ticksWaiting > 150)
            failed ({});

        return;
    }

    if (message.isNotEmpty())
    {
        const auto size = player->getVideoNativeSize();
        caption = file.getFileName() + "   " + (usingPreview ? "preview " : "") + juce::String (size.getWidth()) + " x " + juce::String (size.getHeight());
        message.clear();
        resized();
        repaint();
    }

    if (readPlayhead == nullptr)
        return;

    const auto playhead = readPlayhead();

    if (! playhead.has_value())
    {
        if (player->isPlaying())
            player->stop();

        return;
    }

    // Sound with no picture: black, the picture stopped until there is one again.
    if (playhead->blank != blank)
    {
        blank = playhead->blank;
        player->setVisible (! blank);
        follower.restart();
        repaint();
    }

    if (blank)
    {
        if (player->isPlaying())
            player->stop();

        return;
    }

    const auto audio = playhead->picture;
    const auto playing = playhead->playing;
    const auto f = follower.next (audio, playing, player->getPlayPosition(), player->isPlaying(), player->getVideoDuration());

    if (f.stop)
        player->stop();

    if (f.seek)
        player->setPlayPosition (f.to);

    if (f.play)
        player->play();

}

void Panel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::canvas);

    if (blank && player != nullptr)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (player->getBounds());
    }

    if (message.isNotEmpty())
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (message, getLocalBounds().reduced (16), juce::Justification::centred, 6);
    }
    else if (caption.isNotEmpty())
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (caption, getLocalBounds().removeFromBottom (20).reduced (8, 0), juce::Justification::centredLeft, true);
    }
}

void Panel::resized()
{
    if (player != nullptr)
        player->setBounds (getLocalBounds().withTrimmedBottom (20));
}
}
