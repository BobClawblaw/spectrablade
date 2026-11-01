#include "MixerView.h"

namespace
{
    const juce::Colour background (0xff15181d);
    const juce::Colour stripColour (0xff1b1e23);
    const juce::Colour busColour (0xff16191d);

    /** How far up a meter a level (gain) reaches: -60 dB at the bottom, +6 dB at the top. */
    float meterProportion (float gain)
    {
        const auto db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    }
}

//==============================================================================
/** Two bars, left and right, holding each peak and letting it fall back. */
class MixerView::Meter final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    void push (float left, float right)
    {
        // Falls by about 4 dB a tick at 30 a second: from full scale to silence in half a second.
        constexpr float fall = 0.63f;
        shown[0] = juce::jmax (left, shown[0] * fall);
        shown[1] = juce::jmax (right, shown[1] * fall);
        clipped = clipped || left > 1.0f || right > 1.0f;
        repaint();
    }

    float getShown (int channel) const              { return shown[juce::jlimit (0, 1, channel)]; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0e1013));
        const auto barWidth = (getWidth() - 3) / 2;

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto height = (float) getHeight() * meterProportion (shown[ch]);
            const auto bar = juce::Rectangle<float> ((float) (1 + ch * (barWidth + 1)), (float) getHeight() - height, (float) barWidth, height);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4fc3a1), 0.0f, (float) getHeight(),
                                                     juce::Colour (0xffffd34d), 0.0f, (float) getHeight() * 0.1f, false));
            g.fillRect (bar);
        }

        // 0 dB, and a red mark once anything went over it (click to clear).
        const auto zero = (float) getHeight() * (1.0f - meterProportion (1.0f));
        g.setColour (clipped ? juce::Colour (0xffd0453a) : juce::Colours::grey);
        g.fillRect (0.0f, zero - (clipped ? 2.0f : 0.5f), (float) getWidth(), clipped ? 3.0f : 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clipped = false;
        repaint();
    }

private:
    float shown[2] {};
    bool clipped = false;
};

//==============================================================================
class MixerView::Strip final : public juce::Component
{
public:
    /** A track's or bus's strip, or the master's (id 0). */
    Strip (MixerView& v, juce::uint32 id) : trackId (id), view (v)
    {
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (name);
        addAndMakeVisible (meter);
        addAndMakeVisible (readout);
        readout.setJustificationType (juce::Justification::centred);
        readout.setFont (juce::FontOptions (11.0f));

        if (trackId == 0)
        {
            name.setText ("Master", juce::dontSendNotification);
            meter.setTooltip ("What goes to the outputs, after the effects rack");
            return;
        }

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        fader.setRange (-60.0, 12.0, 0.1);
        fader.setSkewFactorFromMidPoint (-12.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.onValueChange = [this]
        {
            const auto db = (float) fader.getValue();
            readout.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);
            edit ("Track Volume", true, [db] (SessionTrack& t) { t.gainDb = db; });
        };

        pan.setSliderStyle (juce::Slider::LinearBar);
        pan.setRange (-100.0, 100.0, 1.0);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setColour (juce::Slider::trackColourId, juce::Colour (0xff2c3542));
        pan.textFromValueFunction = [] (double value)
        {
            return value == 0.0 ? juce::String ("C") : juce::String (std::abs ((int) value)) + (value < 0.0 ? " L" : " R");
        };
        pan.onValueChange = [this]
        {
            const auto p = (float) pan.getValue() / 100.0f;
            edit ("Track Pan", true, [p] (SessionTrack& t) { t.pan = p; });
        };

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, b == &mute ? juce::Colour (0xffd0453a) : juce::Colour (0xffd8b43a));
        }

        mute.setTooltip ("Mute");
        solo.setTooltip ("Solo: play only soloed tracks");
        mute.onClick = [this] { const auto on = mute.getToggleState(); edit (on ? "Mute" : "Unmute", false, [on] (SessionTrack& t) { t.mute = on; }); };
        solo.onClick = [this] { const auto on = solo.getToggleState(); edit (on ? "Solo" : "Unsolo", false, [on] (SessionTrack& t) { t.solo = on; }); };

        for (auto* c : std::initializer_list<juce::Component*> { &fader, &pan, &mute, &solo })
            addAndMakeVisible (c);
    }

    /** Shows the track as it is; the frame is where an automated volume or pan is read. */
    void update (const SessionTrack& t, juce::int64 frame)
    {
        isBus = t.isBus;
        name.setText (t.name, juce::dontSendNotification);
        mute.setToggleState (t.mute, juce::dontSendNotification);
        solo.setToggleState (t.solo, juce::dontSendNotification);

        const auto volumeAutomated = ! t.volumeEnvelope.isEmpty(), panAutomated = ! t.panEnvelope.isEmpty();
        fader.setEnabled (! volumeAutomated);
        pan.setEnabled (! panAutomated);
        fader.setTooltip (volumeAutomated ? "Volume follows its envelope" : "Volume");
        pan.setTooltip (panAutomated ? "Pan follows its envelope" : "Pan");

        const auto db = t.valueAt (AutomationParameter::volume, frame);

        if (! fader.isMouseButtonDown())
            fader.setValue (db, juce::dontSendNotification);

        if (! pan.isMouseButtonDown())
            pan.setValue (std::round (t.valueAt (AutomationParameter::pan, frame) * 100.0f), juce::dontSendNotification);

        readout.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (trackId == 0 ? juce::Colour (0xff20262e) : isBus ? busColour : stripColour);
        g.setColour (juce::Colour (0xff2a2f37));
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        name.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        readout.setBounds (r.removeFromBottom (18));

        if (trackId == 0)
        {
            meter.setBounds (r.withSizeKeepingCentre (24, r.getHeight()));
            return;
        }

        pan.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        auto buttons = r.removeFromTop (22);
        mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        solo.setBounds (buttons.removeFromRight (buttons.getWidth() - 4));
        r.removeFromTop (6);
        meter.setBounds (r.removeFromRight (16));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

    const juce::uint32 trackId;
    Meter meter;
    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" };

private:
    void edit (const juce::String& description, bool merge, const std::function<void (SessionTrack&)>& change)
    {
        auto next = *view.engine.session.getState();

        for (auto& t : next.tracks)
            if (t.id == trackId)
                change (t);

        view.engine.session.apply (std::move (next), description, merge);
    }

    MixerView& view;
    juce::Label name, readout;
    bool isBus = false;
};

//==============================================================================
MixerView::MixerView (AudioEngine& e) : engine (e)
{
    master = std::make_unique<Strip> (*this, 0);
    addAndMakeVisible (*master);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    engine.session.addChangeListener (this);
    rebuild();
    startTimerHz (30);
}

MixerView::~MixerView()
{
    engine.session.removeChangeListener (this);
}

void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuild();
}

void MixerView::rebuild()
{
    const auto state = engine.session.getState();
    std::vector<std::pair<juce::uint32, bool>> tracks;

    for (auto& t : state->tracks)
        tracks.emplace_back (t.id, t.isBus);

    // Strips are made again only when tracks come, go or move; otherwise they are just updated.
    if (tracks != shownTracks)
    {
        shownTracks = tracks;
        strips.clear();

        for (auto& [id, bus] : tracks)
        {
            strips.push_back (std::make_unique<Strip> (*this, id));
            content.addAndMakeVisible (*strips.back());
        }

        resized();
    }

    const auto frame = engine.getPlayer().getPositionFrames();

    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->update (state->tracks[i], frame);
}

MixerView::Strip* MixerView::findStrip (juce::uint32 trackId) const
{
    for (auto& s : strips)
        if (s->trackId == trackId)
            return s.get();

    return nullptr;
}

juce::Slider* MixerView::findFader (juce::uint32 trackId) const   { auto* s = findStrip (trackId); return s != nullptr ? &s->fader : nullptr; }
juce::Slider* MixerView::findPan (juce::uint32 trackId) const     { auto* s = findStrip (trackId); return s != nullptr ? &s->pan : nullptr; }
juce::Button* MixerView::findMute (juce::uint32 trackId) const    { auto* s = findStrip (trackId); return s != nullptr ? &s->mute : nullptr; }
juce::Button* MixerView::findSolo (juce::uint32 trackId) const    { auto* s = findStrip (trackId); return s != nullptr ? &s->solo : nullptr; }

float MixerView::getShownLevel (juce::uint32 trackId, int channel) const
{
    if (trackId == 0)
        return master->meter.getShown (channel);

    auto* s = findStrip (trackId);
    return s != nullptr ? s->meter.getShown (channel) : 0.0f;
}

void MixerView::updateMeters()
{
    auto& levels = engine.getPlayer().getLevels();

    for (size_t i = 0; i < strips.size(); ++i)
    {
        const auto [left, right] = levels.take ((int) i);
        strips[i]->meter.push (left, right);
    }

    const auto [left, right] = engine.takeMasterPeaks();
    master->meter.push (left, right);

    // An automated volume or pan moves as the session plays.
    if (engine.getPlayer().isPlaying())
    {
        const auto state = engine.session.getState();
        const auto frame = engine.getPlayer().getPositionFrames();

        for (size_t i = 0; i < strips.size() && i < state->tracks.size(); ++i)
            if (! state->tracks[i].volumeEnvelope.isEmpty() || ! state->tracks[i].panEnvelope.isEmpty())
                strips[i]->update (state->tracks[i], frame);
    }
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (background);

    if (strips.empty())
    {
        g.setColour (juce::Colours::grey);
        g.drawText ("No tracks yet", getLocalBounds().withTrimmedRight (stripWidth + 12), juce::Justification::centred);
    }
}

void MixerView::resized()
{
    auto r = getLocalBounds();
    master->setBounds (r.removeFromRight (stripWidth + 12));
    viewport.setBounds (r);
    content.setSize (juce::jmax (r.getWidth(), (int) strips.size() * stripWidth), juce::jmax (0, r.getHeight() - viewport.getScrollBarThickness()));

    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * stripWidth, 0, stripWidth, content.getHeight());
}
