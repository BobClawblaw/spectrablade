#pragma once

#include "MultitrackView.h"

/** A strip above the lanes showing the whole session in miniature: every shown track's clips
    in their colours, the cursor, and a frame around the part the lanes show. Clicking or
    dragging in it scrolls the lanes to centre there, keeping the zoom. */
class SessionOverview final : public juce::Component,
                              private juce::ChangeListener,
                              private juce::Timer
{
public:
    SessionOverview (AudioEngine&, MultitrackView&);
    ~SessionOverview() override;

    static constexpr int height = 36;

    /** Session frames across the strip: from 0 to the session's end (at least a minute, and as
        far as the lanes show). */
    double getTotalFrames() const;
    juce::int64 frameAt (float x) const;
    float xFor (double frame) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void centreOn (float x);

    AudioEngine& engine;
    MultitrackView& view;
    juce::Range<double> lastShown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionOverview)
};
