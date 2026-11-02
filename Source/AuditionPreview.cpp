#include "AuditionPreview.h"

AuditionPreview::AuditionPreview (juce::AudioFormatManager& f, Player p, bool previewOn)
    : formats (f), player (std::move (p))
{
    previewToggle.setToggleState (previewOn, juce::dontSendNotification);
    previewToggle.setTooltip ("Play each audio file as you select it");
    previewToggle.onClick = [this] { setPreviewOn (previewToggle.getToggleState()); };
    addAndMakeVisible (previewToggle);

    playButton.onClick = [this] { playOrStop(); };
    playButton.setEnabled (false);
    addAndMakeVisible (playButton);

    description.setFont (juce::FontOptions (12.0f));
    description.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (description);

    setSize (520, 36);
}

AuditionPreview::~AuditionPreview()
{
    stopTimer();
}

juce::String AuditionPreview::describe (juce::AudioFormatManager& formatManager, const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        return {};

    const auto ext = file.getFileExtension().trimCharactersAtStart (".").toUpperCase();
    const auto channels = reader->numChannels == 1 ? juce::String ("mono")
                        : reader->numChannels == 2 ? juce::String ("stereo")
                                                   : juce::String ((int) reader->numChannels) + " ch";
    const auto depth = reader->usesFloatingPointData ? juce::String (reader->bitsPerSample) + "-bit float"
                                                     : juce::String (reader->bitsPerSample) + "-bit";
    const auto seconds = (double) reader->lengthInSamples / reader->sampleRate;
    const auto minutes = (int) (seconds / 60.0);
    const auto rest = seconds - 60.0 * minutes;
    const auto length = juce::String (minutes) + ":" + (rest < 10.0 ? "0" : "") + juce::String (rest, 1);

    return ext + ", " + juce::String (reader->sampleRate / 1000.0, 1) + " kHz, " + channels + (reader->bitsPerSample > 0 ? ", " + depth : juce::String()) + ", " + length;
}

void AuditionPreview::selectedFileChanged (const juce::File& file)
{
    selected = file;
    const auto text = describe (formats, file);
    readable = text.isNotEmpty();
    description.setText (text, juce::dontSendNotification);

    if (isPreviewOn() && readable)
        player.play (file);
    else
        player.play ({});

    updateButton();
}

void AuditionPreview::setPreviewOn (bool on)
{
    previewToggle.setToggleState (on, juce::dontSendNotification);
    player.play (on && readable ? selected : juce::File());
    updateButton();

    if (onPreviewChanged != nullptr)
        onPreviewChanged (on);
}

void AuditionPreview::playOrStop()
{
    if (player.isPlaying())
        player.play ({});
    else if (readable)
        player.play (selected);

    updateButton();
}

void AuditionPreview::updateButton()
{
    const auto playing = player.isPlaying();
    playButton.setButtonText (playing ? "Stop" : "Play");
    playButton.setEnabled (readable || playing);

    if (playing)
        startTimerHz (10);
    else
        stopTimer();
}

void AuditionPreview::timerCallback()
{
    updateButton();   // the sound ends by itself
}

void AuditionPreview::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    previewToggle.setBounds (r.removeFromLeft (84));
    playButton.setBounds (r.removeFromLeft (60));
    r.removeFromLeft (10);
    description.setBounds (r);
}
