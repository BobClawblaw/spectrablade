#pragma once

#include <JuceHeader.h>

/** Video beside the audio: a file opened from a video (or given one with Attach Video) shows its
    picture in the Video panel, following the editor's playhead, and its edited sound can be put back
    into a copy of the video (Ffmpeg::replaceAudio). The audio's time 0 is the video's time 0. */
namespace Video
{
    /** The video types opened (their sound, through ffmpeg) and shown: MP4, M4V, MOV, MKV, WebM, AVI. */
    const juce::StringArray& extensions();
    bool isVideoFile (const juce::File&);

    /** While both play, how far off the picture may be before it is put right, in seconds. */
    constexpr double driftAllowed = 0.04;

    /** What the picture does next. */
    struct Follow
    {
        bool seek = false;
        double to = 0.0;
        bool play = false, stop = false;
    };

    /** Keeps the picture with the sound. Stopped, the picture shows the frame at the sound's place
        each time that changes. Playing, it plays at normal speed (changing a player's speed makes
        it stall); a player starts a little after it is told to, so a seek while playing aims ahead
        of the sound by `lead`, learned from how far off the picture is once it moves (and kept for
        the next time). Past the video's end it stops on its last frame. */
    class Follower
    {
    public:
        Follow next (double audio, bool audioPlaying, double picture, bool picturePlaying, double duration);

        /** How far ahead a seek while playing aims (seconds). */
        double getLead() const noexcept           { return lead; }

        /** A new video: everything forgotten but the lead. */
        void restart()                            { lastAudio = -1.0; seekedTo = -1.0; settling = false; }

    private:
        double lead = 0.0, lastAudio = -1.0, seekedTo = -1.0, settleStart = 0.0;
        bool settling = false;
    };

    /** The Video panel: the picture, kept in step with the playhead each 30th of a second, without
        its own sound. */
    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        Panel();
        ~Panel() override;

        /** Where the sound is, in seconds, and whether it plays; nothing while the picture shouldn't
            follow (the multitrack session is shown). */
        std::function<std::optional<std::pair<double, bool>>()> readPlayhead;

        /** The video to show, asked as it follows (nothing: none). */
        std::function<juce::File()> readVideo;

        /** Shows this video (nothing: none). The same file again leaves it as it is. */
        void setFile (const juce::File&);
        juce::File getFile() const                     { return file; }

        /** The picture's player, while a video is open (for tests). */
        juce::VideoComponent* getPlayer() const        { return player.get(); }

        /** Whether the video is ready to show (macOS has read it), and follows the playhead. */
        bool isReady() const;

        /** What the panel says instead of a picture, or empty when it shows one. */
        juce::String getMessage() const                { return message; }

        /** Follows the playhead once, as the timer does. */
        void update();

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override                  { update(); }
        void failed (const juce::String& why);

        juce::File file;
        std::unique_ptr<juce::VideoComponent> player;
        juce::String message, caption;
        Follower follower;
        int ticksWaiting = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
