#pragma once

#include <JuceHeader.h>
#include "SessionMixer.h"

/** Video beside the audio: a file opened from a video (or given one with Attach Video) shows its
    picture in the Video panel, following the editor's playhead, and its edited sound can be put back
    into a copy of the video (Ffmpeg::replaceAudio). The audio's time 0 is the video's time 0.

    A multitrack session can have a video too, starting at a time in the session of its own: the
    panel follows the session's playhead, and the mix from that time can be put into a copy. */
namespace Video
{
    /** The video types opened (their sound, through ffmpeg) and shown: MP4, M4V, MOV, MKV, WebM, AVI. */
    const juce::StringArray& extensions();
    bool isVideoFile (const juce::File&);

    /** Videos whose picture macOS can't show (MKV, WebM): the panel shows a preview made by ffmpeg. */
    bool needsPreview (const juce::File&);

    /** Where previews are kept, unless set (for tests): ~/Library/Caches/SpectraBlade/Video Previews on macOS,
        SpectraBlade/Video Previews in the local application data folder on Windows. */
    juce::File previewFolder();
    void setPreviewFolder (const juce::File&);

    /** A video's preview there, named for the video's path, size and time it was changed (so a changed
        video gets a new one). */
    juce::File previewFile (const juce::File& video);

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

    /** Where in a session's video the session's time is: its time less the video's start, or
        blank (the first frame, shown black) before the video starts. */
    struct SessionPicture
    {
        double picture = 0.0;
        bool blank = false;
    };

    SessionPicture sessionPicture (double sessionSeconds, double videoStart);

    /** The session frames a video's length covers from its start: the sound put into it. */
    juce::Range<juce::int64> sessionRange (double videoStart, double videoSeconds, double sampleRate);

    /** Writes target: a copy of the session's video with the session's mix in place of its sound,
        from the video's start for the video's length, so its picture streams are copied as they
        are (Ffmpeg::replaceAudio). inserts: the tracks' effects, or null for none. progress gets
        0..1 and returns false to stop (then "Stopped."). Returns an error message, or empty. */
    juce::String saveMixInto (const SessionState&, SessionMixer::Inserts* inserts, const juce::File& ffmpeg, const juce::File& target,
                              juce::AudioFormatManager&, const std::function<bool (float)>& progress = {});

    /** The Video panel: the picture, kept in step with the playhead each 30th of a second, without
        its own sound. */
    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        Panel();
        ~Panel() override;

        /** Where in the video the playhead is (the sound's place through its PictureMap), whether it
            plays, and whether the sound there has no picture (shown black). */
        struct Playhead
        {
            double picture = 0.0;
            bool playing = false, blank = false;
        };

        /** The playhead now (the editor's, or the session's while it is shown); nothing while the picture shouldn't follow. */
        std::function<std::optional<Playhead>()> readPlayhead;

        /** Whether the panel is black because the sound there has no picture. */
        bool isBlank() const noexcept                  { return blank; }

        /** The video to show, asked as it follows (nothing: none). */
        std::function<juce::File()> readVideo;

        /** Shows this video (nothing: none). The same file again leaves it as it is. */
        void setFile (const juce::File&);
        juce::File getFile() const                     { return file; }

        /** The picture's player, while a video is open (for tests). */
        juce::VideoComponent* getPlayer() const        { return player.get(); }

        /** Whether the video is ready to show (macOS has read it), and follows the playhead. */
        bool isReady() const;

        /** Whether the picture shown is a preview made by ffmpeg, not the video itself. */
        bool isShowingPreview() const noexcept         { return usingPreview; }

        /** What the panel says instead of a picture, or empty when it shows one. */
        juce::String getMessage() const                { return message; }

        /** Follows the playhead once, as the timer does. */
        void update();

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override                  { update(); }
        void failed (const juce::String& why);
        void load (const juce::File& played);
        void startPreview();
        void previewMade (const juce::File& video, const juce::File& preview, const juce::String& error);

        class PreviewMaker;
        std::unique_ptr<PreviewMaker> maker;
        bool usingPreview = false, blank = false;

        juce::File file;
        juce::File waitingForWindow;   // Windows: loaded once the panel is on the screen (DirectShow draws in a window of its own)
        std::unique_ptr<juce::VideoComponent> player;
        juce::String message, caption;
        Follower follower;
        int ticksWaiting = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
