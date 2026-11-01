#pragma once

#include "AudioEngine.h"

/** The session as a mixing desk: a strip for each track and bus, in the session's order, with its
    meter, volume fader, pan, mute and solo, and a master strip metering what goes to the outputs.

    The controls edit the session as the track headers do (each change an undo step, a fader's
    drag one step); an automated volume or pan follows its envelope and can't be moved here.
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

    /** Strips for tracks and buses (the master strip not counted). */
    int getNumStrips() const noexcept               { return (int) strips.size(); }

    /** A strip's controls, for the track or bus with this id (null when there is none). */
    juce::Slider* findFader (juce::uint32 trackId) const;
    juce::Slider* findPan (juce::uint32 trackId) const;
    juce::Button* findMute (juce::uint32 trackId) const;
    juce::Button* findSolo (juce::uint32 trackId) const;

    /** The menu a strip shows when right-clicked: Solo Safe, and a Colour submenu (each an undo
        step); empty for an unknown id. */
    juce::PopupMenu getStripMenu (juce::uint32 trackId) const;

    /** The level a meter shows (gain, not dB): a track's or bus's, or with id 0 the master's. */
    float getShownLevel (juce::uint32 trackId, int channel) const;

    /** Takes the latest peaks into the meters (the view's timer does this 30 times a second). */
    void updateMeters();

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
