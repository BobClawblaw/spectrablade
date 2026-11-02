#pragma once

#include <JuceHeader.h>

/** The strip under an open panel's file list for auditioning audio files before opening them: a
    Preview checkbox (play each file as it is selected), a Play/Stop button for the selected file,
    and a line describing it (format, rate, channels, depth, length). The sound itself is played
    by the caller's functions, so the panel needs nothing from the engine. */
class AuditionPreview final : public juce::FilePreviewComponent,
                              private juce::Timer
{
public:
    struct Player
    {
        std::function<bool (const juce::File&)> play;   // from the start; false when it can't; an empty File stops
        std::function<bool()> isPlaying;
    };

    AuditionPreview (juce::AudioFormatManager&, Player, bool previewOn);
    ~AuditionPreview() override;

    void selectedFileChanged (const juce::File&) override;

    bool isPreviewOn() const                      { return previewToggle.getToggleState(); }
    void setPreviewOn (bool);   // as if clicked: plays or stops the selected file, calls onPreviewChanged

    /** Plays the selected file, or stops it when it is playing (the Play/Stop button). */
    void playOrStop();

    juce::String getDescription() const           { return description.getText(); }
    juce::File getSelectedFile() const            { return selected; }

    std::function<void (bool)> onPreviewChanged;

    /** A file's format, rate, channels, depth and length ("WAV, 48 kHz, stereo, 24-bit, 0:03.5"),
        or empty when it isn't audio the formats can read. */
    static juce::String describe (juce::AudioFormatManager&, const juce::File&);

    void resized() override;

private:
    void timerCallback() override;
    void updateButton();

    juce::AudioFormatManager& formats;
    Player player;
    juce::File selected;
    bool readable = false;

    juce::ToggleButton previewToggle { "Preview" };
    juce::TextButton playButton { "Play" };
    juce::Label description;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuditionPreview)
};
