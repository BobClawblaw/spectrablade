#include "Video.h"
#include "Theme.h"

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
Panel::Panel()
{
    message = "No video. Open a video file, or attach one to this file with File > Attach Video...";
    startTimerHz (30);
}

Panel::~Panel()
{
    stopTimer();
}

void Panel::setFile (const juce::File& f)
{
    if (f == file)
        return;

    file = f;
    caption.clear();
    follower.restart();
    ticksWaiting = 0;

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

    player = std::make_unique<juce::VideoComponent> (false);
    addAndMakeVisible (*player);
    resized();
    player->onErrorOccurred = [this] (const juce::String& error) { failed (error); };

    if (const auto result = player->load (file); result.failed())
    {
        failed (result.getErrorMessage());
        return;
    }

    player->setAudioVolume (0.0f);
    message = "Opening " + file.getFileName() + "...";
    repaint();
}

void Panel::failed (const juce::String& why)
{
    if (player != nullptr)
    {
        removeChildComponent (player.get());
        player.reset();
    }

    message = "The picture of " + file.getFileName() + " can't be shown" + (why.isNotEmpty() ? ": " + why : juce::String (".")) + "\n\n"
              "The picture is shown by macOS (AVFoundation), which plays MP4, M4V and MOV video (and some AVI), not MKV or WebM; "
              "its sound can still be edited and put back into it.";
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
        caption = file.getFileName() + "   " + juce::String (size.getWidth()) + " x " + juce::String (size.getHeight());
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

    const auto [audio, playing] = *playhead;
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
