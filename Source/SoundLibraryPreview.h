#pragma once

#include <JuceHeader.h>

/** The Sound Library's preview: the selected sound's whole waveform, a playhead crossing it as it
    plays, a strip along its top to scrub in, and a part of it to select by dragging across it.

    - Clicking the waveform plays from there (and clears a selection); dragging across it selects
      a part, which then plays (and loops, with Loop on) alone.
    - Dragging along the strip at the top scrubs: the sound plays from wherever the mouse is.
    - Dragging from inside the selected part carries only that part out, written to a file of its
      own (a WAV at the sound's rate, channels and bit depth) in the parts folder.
    - Play/Stop (and the space bar in the window), Loop, Play on Select and a volume slider.

    The player reads the file through a buffer filled on a thread of its own, so the audio thread
    never waits for the disk, and resamples only when the sound's rate isn't the device's. Mono
    plays in both channels and 5.1 or 7.1 is folded down to stereo. */
class SoundLibraryPreview final : public juce::Component,
                                  private juce::Timer,
                                  private juce::ChangeListener
{
public:
    /** The player, an AudioSource for a device callback (or to be pulled directly). */
    class Player final : public juce::AudioSource
    {
    public:
        Player();
        ~Player() override;

        /** Opens a sound, stopped at its start with no part selected; false when it can't be read. */
        bool load (const juce::File&);
        void unload();
        juce::File getFile() const;
        juce::int64 getLength() const noexcept           { return length.load(); }
        double getSampleRate() const noexcept           { return fileRate.load(); }

        void play();                                    // from where it is, or the start of the part when outside it or at its end
        void playFrom (juce::int64 frame);
        void stop()                                     { playing = false; }
        bool isPlaying() const noexcept                 { return playing.load(); }
        juce::int64 getPosition() const noexcept        { return position.load(); }
        void setPosition (juce::int64 frame);

        /** The part to play (and loop); empty for the whole sound. */
        void setRange (juce::Range<juce::int64>);
        juce::Range<juce::int64> getRange() const;

        void setLooping (bool shouldLoop) noexcept      { looping = shouldLoop; }
        bool isLooping() const noexcept                 { return looping.load(); }
        void setGainDb (float db) noexcept;
        float getGainDb() const noexcept                { return gainDb.load(); }

        /** How long a block may wait for the disk: 0 (the default) gives silence instead of waiting. */
        void setReadTimeout (int milliseconds);

        void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
        void releaseResources() override;
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    private:
        /** The sound at its own rate, in two channels. */
        struct Source final : public juce::AudioSource
        {
            explicit Source (Player& p) : player (p) {}
            void prepareToPlay (int, double) override  {}
            void releaseResources() override           {}
            void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;
            Player& player;
        };

        juce::TimeSliceThread readAhead { "Sound Library preview" };
        juce::AudioFormatManager formats;
        juce::CriticalSection lock;
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::File file;
        juce::Range<juce::int64> range;
        juce::AudioBuffer<float> scratch;
        int fileChannels = 0, readTimeout = 0;
        std::atomic<juce::int64> length { 0 }, position { 0 };
        std::atomic<double> fileRate { 0.0 }, deviceRate { 0.0 };
        std::atomic<bool> playing { false }, looping { false };
        std::atomic<bool> flush { false };   // the resampler still holds the last sound's tail
        std::atomic<float> gainDb { 0.0f }, gain { 1.0f };
        Source source { *this };
        juce::ResamplingAudioSource resampler { &source, false, 2 };

        JUCE_DECLARE_NON_COPYABLE (Player)
    };

    /** Writes [range) of a sound as a WAV at its rate, channels and bit depth (32-bit float when it
        is float), named after it and the part's times, in the folder; the same part asked for again
        gives the file already there. None when the sound can't be read or the range is empty. */
    static juce::File writePart (const juce::File& sound, juce::Range<juce::int64>, const juce::File& folder);

    explicit SoundLibraryPreview (const juce::File& partsFolder);
    ~SoundLibraryPreview() override;

    Player& getPlayer() noexcept                        { return player; }

    /** Shows a sound (none: an empty preview), playing it at once with Play on Select on. */
    void show (const juce::File&);
    bool isWaveformLoaded() const                       { return thumbnail.isFullyLoaded() && thumbnail.getTotalLength() > 0.0; }

    void togglePlay();
    void setPlayOnSelect (bool);
    bool getPlayOnSelect() const                        { return autoButton.getToggleState(); }

    /** The selected part, in frames; empty for none. */
    juce::Range<juce::int64> getSelection() const       { return selection; }
    void setSelection (juce::Range<juce::int64>);

    /** The area the waveform is drawn in, and the strip along its top to scrub in. */
    juce::Rectangle<int> getWaveArea() const            { return wave; }
    static constexpr int scrubStripHeight = 12;
    juce::int64 frameAt (float x) const;
    float xOf (juce::int64 frame) const;

    /** The folder the parts dragged or inserted are written into. */
    const juce::File& getPartsFolder() const noexcept   { return partsFolder; }

    /** Called with the file of the part being dragged out. */
    std::function<void (const juce::StringArray&)> onDragOut;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override  { repaint(); }
    void updateButtons();

    enum class Gesture { none, scrub, select, dragPart };

    Player player;
    juce::File partsFolder;
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache cache { 16 };
    juce::AudioThumbnail thumbnail { 256, formats, cache };
    juce::Range<juce::int64> selection;
    juce::int64 anchor = 0;
    Gesture gesture = Gesture::none;
    bool moved = false;
    juce::Rectangle<int> wave;

    juce::TextButton playButton { "Play" };
    juce::ToggleButton loopButton { "Loop" }, autoButton { "Play on Select" };
    juce::Slider volume { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label time;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundLibraryPreview)
};
