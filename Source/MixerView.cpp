#include "MixerView.h"
#include "Theme.h"
#include "TrackColours.h"

namespace
{
    // The theme's colours, read as they are when painting.
    const juce::Colour& background = Theme::canvas;
    const juce::Colour& stripColour = Theme::surface;
    const juce::Colour& busColour = Theme::base;
    const juce::Colour& folderColour = Theme::raised;

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
        g.fillAll (Theme::inset);
        const auto barWidth = (getWidth() - 3) / 2;

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto height = (float) getHeight() * meterProportion (shown[ch]);
            const auto bar = juce::Rectangle<float> ((float) (1 + ch * (barWidth + 1)), (float) getHeight() - height, (float) barWidth, height);
            g.setGradientFill (juce::ColourGradient (Theme::meterLow, 0.0f, (float) getHeight(),
                                                     Theme::meterMid, 0.0f, (float) getHeight() * 0.1f, false));
            g.fillRect (bar);
        }

        // 0 dB, and a red mark once anything went over it (click to clear).
        const auto zero = (float) getHeight() * (1.0f - meterProportion (1.0f));
        g.setColour (clipped ? Theme::danger : Theme::dimText);
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
        name.setInterceptsMouseClicks (false, false);
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
        Theme::follow (pan, juce::Slider::trackColourId, Theme::lineStrong);
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
            Theme::follow (*b, juce::TextButton::buttonOnColourId, b == &mute ? Theme::danger : Theme::warn);
        }

        polarity.setClickingTogglesState (true);
        Theme::follow (polarity, juce::TextButton::buttonOnColourId, Theme::warn);
        polarity.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        polarity.setTooltip ("Invert polarity: turn the track's output upside down, after its effects");
        polarity.onClick = [this]
        {
            const auto on = polarity.getToggleState();
            edit (on ? "Invert Polarity" : "Restore Polarity", false, [on] (SessionTrack& t) { t.invertPolarity = on; });
        };

        mute.setTooltip ("Mute");
        solo.setTooltip ("Solo: play only soloed tracks");
        mute.onClick = [this] { const auto on = mute.getToggleState(); edit (on ? "Mute" : "Unmute", false, [on] (SessionTrack& t) { t.mute = on; }); };
        solo.onClick = [this] { const auto on = solo.getToggleState(); edit (on ? "Solo" : "Unsolo", false, [on] (SessionTrack& t) { t.solo = on; }); };

        for (auto* c : std::initializer_list<juce::Component*> { &fader, &pan, &mute, &polarity, &solo })
            addAndMakeVisible (c);
    }

    /** Shows the track as it is; the frame is where an automated volume or pan is read. */
    void update (const SessionTrack& t, int index, juce::int64 frame)
    {
        isBus = t.isBus;

        // A folder's strip: its volume, mute and solo, which act on what is in it; no audio to meter.
        if (std::exchange (isFolder, t.isFolder) != t.isFolder)
        {
            pan.setVisible (! isFolder);
            meter.setVisible (! isFolder);
            polarity.setVisible (! isFolder);
        }

        colour = TrackColours::of (t, index);
        name.setText (t.name, juce::dontSendNotification);
        mute.setToggleState (t.mute, juce::dontSendNotification);
        solo.setToggleState (t.solo, juce::dontSendNotification);
        polarity.setToggleState (t.invertPolarity, juce::dontSendNotification);
        solo.setTooltip (t.soloSafe ? "Solo: play only soloed tracks (this one is solo safe: it plays anyway)" : "Solo: play only soloed tracks");
        soloSafe = t.soloSafe;

        const auto volumeAutomated = ! t.volumeEnvelope.isEmpty(), panAutomated = ! t.panEnvelope.isEmpty();
        fader.setEnabled (! volumeAutomated);
        pan.setEnabled (! panAutomated);
        fader.setTooltip (isFolder ? "Folder volume: added to the volume of each track and bus in it"
                                   : volumeAutomated ? "Volume follows its envelope" : "Volume");
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
        g.fillAll (trackId == 0 ? Theme::hover : isBus ? busColour : isFolder ? folderColour : stripColour);

        if (trackId != 0)
        {
            g.setColour (colour);
            g.fillRect (0, 0, getWidth() - 1, 3);
        }

        // Solo safe: a bar under the solo button, as in the track headers.
        if (soloSafe && trackId != 0)
        {
            g.setColour (Theme::warn);
            g.fillRect (solo.getX() + 3, solo.getBottom() + 1, solo.getWidth() - 6, 2);
        }

        g.setColour (Theme::line);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    /** Right-click: solo safe and the colour. */
    juce::PopupMenu createMenu()
    {
        juce::PopupMenu menu;
        const auto* t = view.engine.session.getState()->findTrack (trackId);

        if (t == nullptr)
            return menu;

        menu.addItem ("Solo Safe", true, t->soloSafe, [this, safe = t->soloSafe]
        {
            edit (safe ? "Not Solo Safe" : "Solo Safe", false, [safe] (SessionTrack& track) { track.soloSafe = ! safe; });
        });

        juce::PopupMenu colours;
        colours.addItem ("Automatic", true, t->colour < 0, [this] { edit ("Track Colour", false, [] (SessionTrack& track) { track.colour = -1; }); });

        for (int c = 0; c < (int) TrackColours::palette.size(); ++c)
        {
            juce::PopupMenu::Item item (TrackColours::palette[(size_t) c].name);
            item.setTicked (t->colour == c);
            item.setColour (juce::Colour (TrackColours::palette[(size_t) c].argb));
            item.setAction ([this, c] { edit ("Track Colour", false, [c] (SessionTrack& track) { track.colour = c; }); });
            colours.addItem (std::move (item));
        }

        menu.addSubMenu ("Colour", colours);
        return menu;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (trackId != 0 && e.mods.isPopupMenu())
            createMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
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
        const auto each = (buttons.getWidth() - 4) / 3;
        mute.setBounds (buttons.removeFromLeft (each));
        buttons.removeFromLeft (2);
        polarity.setBounds (buttons.removeFromLeft (each));
        buttons.removeFromLeft (2);
        solo.setBounds (buttons);
        r.removeFromTop (6);
        meter.setBounds (r.removeFromRight (16));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

    const juce::uint32 trackId;
    Meter meter;
    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" }, polarity { juce::String (juce::CharPointer_UTF8 ("\xc3\x98")) };

private:
    void edit (const juce::String& description, bool merge, const std::function<void (SessionTrack&)>& change)
    {
        auto next = *view.engine.session.getState();

        for (auto& t : next.tracks)
        {
            if (t.id == trackId)
            {
                const auto before = t;
                change (t);
                SessionEdits::followGroup (next, before);
                break;
            }
        }

        view.engine.session.apply (std::move (next), description, merge);
    }

    MixerView& view;
    juce::Colour colour;
    juce::Label name, readout;
    bool isBus = false, isFolder = false, soloSafe = false;
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

    // A strip for each track, bus and folder, in their order: strip i is track i.
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
        strips[i]->update (state->tracks[i], (int) i, frame);
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
juce::Button* MixerView::findPolarity (juce::uint32 trackId) const { auto* s = findStrip (trackId); return s != nullptr ? &s->polarity : nullptr; }

juce::PopupMenu MixerView::getStripMenu (juce::uint32 trackId) const
{
    auto* s = findStrip (trackId);
    return s != nullptr ? s->createMenu() : juce::PopupMenu();
}

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
                strips[i]->update (state->tracks[i], (int) i, frame);
    }
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (background);

    if (strips.empty())
    {
        g.setColour (Theme::dimText);
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
