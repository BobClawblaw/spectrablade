#pragma once

#include "AudioEngine.h"

#include <optional>

class MultitrackView;
class SurroundPad;

/** The session as a mixing desk: a strip for each track and bus, in the session's order, with its
    meter, volume fader, pan, mute, polarity switch and solo, and a master strip metering what goes
    to the outputs. In a 5.1 master each strip is wider, with a surround pad, width and LFE beside its fader.

    The controls edit the session as the track headers do (each change an undo step, a fader's
    drag one step); an automated volume or pan follows its envelope and can't be moved here.
    The surround pad, width and LFE likewise follow their envelopes, and with automation being
    written (setAutomationWriter) they stay live and record into them while the session plays.
    Right-click a strip to make it solo safe or change its colour.
    Meters show peaks after each track's volume and pan, falling back over about half a second. */
class MixerView final : public juce::Component,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit MixerView (AudioEngine&);
    ~MixerView() override;

    static constexpr int stripWidth = 84;
    static constexpr int surroundStripWidth = 132;   // a strip's in a 5.1 master, with its surround pad beside the fader

    /** Strips for tracks and buses (the master strip not counted). */
    int getNumStrips() const noexcept               { return (int) strips.size(); }

    /** A strip's controls, for the track or bus with this id (null when there is none). */
    juce::Slider* findFader (juce::uint32 trackId) const;
    juce::Label* findReadout (juce::uint32 trackId) const;

    /** A volume typed into a strip's readout: a number from -60 to +12, with or without "dB", rounded to
        0.1 dB as the fader sets it. Anything else gives nothing. */
    static std::optional<float> parseVolume (const juce::String&);
    juce::Slider* findPan (juce::uint32 trackId) const;

    /** A strip's surround controls, shown in a 5.1 master: the point, the width (0 to 100) and
        the LFE (-61 for off, then -60 to +10 dB). */
    SurroundPad* findSurroundPad (juce::uint32 trackId) const;
    juce::Slider* findSurroundWidth (juce::uint32 trackId) const;
    juce::Slider* findLfe (juce::uint32 trackId) const;
    juce::Button* findMute (juce::uint32 trackId) const;
    juce::Button* findSolo (juce::uint32 trackId) const;
    juce::Button* findPolarity (juce::uint32 trackId) const;

    /** The menu a strip shows when right-clicked: Solo Safe, and a Colour submenu (each an undo
        step); empty for an unknown id. */
    juce::PopupMenu getStripMenu (juce::uint32 trackId) const;

    /** The level a meter shows (gain, not dB): a track's or bus's, or with id 0 the master's
        (channels 0 to 5, L R C LFE Ls Rs, in a 5.1 master). */
    float getShownLevel (juce::uint32 trackId, int channel) const;

    /** How many channels the master's meter shows: 2, or 6 in a 5.1 master. */
    int getMasterMeterChannels() const;

    /** Takes the latest peaks into the meters (the view's timer does this 30 times a second). */
    void updateMeters();

    /** The multitrack view whose automation writing the surround controls follow: with writing on
        and the session playing, moving them records a pass into their envelopes (one undo step,
        "Write Surround Automation"). nullptr (as made) for none. */
    void setAutomationWriter (MultitrackView*);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Meter;
    class Strip;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override                   { updateMeters(); }
    void rebuild();
    Strip* findStrip (juce::uint32 trackId) const;

    AudioEngine& engine;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Strip>> strips;
    std::unique_ptr<Strip> master;
    std::vector<std::pair<juce::uint32, bool>> shownTracks;   // id and bus-ness of each strip, to see when to rebuild
    bool shownSurround = false;                               // the strips laid out for a 5.1 master
    MultitrackView* writer = nullptr;
    bool shownWriting = false;                                // the controls set up for writing automation

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
