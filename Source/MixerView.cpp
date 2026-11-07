#include "MixerView.h"
#include "MultitrackView.h"
#include "SurroundPad.h"
#include "Surround.h"
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
        const float both[] { left, right };
        push (both, 2);
    }

    /** Takes in the newest peaks of its channels (2, a 5.1 master's 6 or a 7.1 master's 8). */
    void push (const float* peaks, int count)
    {
        // Falls by about 4 dB a tick at 30 a second: from full scale to silence in half a second.
        constexpr float fall = 0.63f;

        for (int ch = 0; ch < juce::jmin (count, channels); ++ch)
        {
            shown[ch] = juce::jmax (peaks[ch], shown[ch] * fall);
            clipped = clipped || peaks[ch] > 1.0f;
        }

        repaint();
    }

    /** Two bars, or six or eight named ones (Surround::channelName) for a 5.1 or 7.1 master. */
    void setChannels (int count)
    {
        count = count == 6 || count == 8 ? count : 2;

        if (count == channels)
            return;

        channels = count;
        std::fill (std::begin (shown), std::end (shown), 0.0f);
        repaint();
    }

    int getChannels() const noexcept                { return channels; }
    float getShown (int channel) const              { return shown[juce::jlimit (0, channels - 1, channel)]; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::inset);
        const auto named = channels > 2;
        const auto labels = named ? 12 : 0;
        const auto height = getHeight() - labels;
        const auto barWidth = (getWidth() - 1 - channels) / channels;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto x = (float) (1 + ch * (barWidth + 1));
            const auto level = (float) height * meterProportion (shown[ch]);
            g.setGradientFill (juce::ColourGradient (Theme::meterLow, 0.0f, (float) height,
                                                     Theme::meterMid, 0.0f, (float) height * 0.1f, false));
            g.fillRect (juce::Rectangle<float> (x, (float) height - level, (float) barWidth, level));

            if (named)
            {
                g.setColour (Theme::dimText);
                g.setFont (9.0f);
                g.drawText (Surround::channelName (channels, ch),
                            juce::Rectangle<float> (x - 2.0f, (float) height, (float) barWidth + 4.0f, (float) labels), juce::Justification::centred, false);
            }
        }

        // 0 dB, and a red mark once anything went over it (click to clear).
        const auto zero = (float) height * (1.0f - meterProportion (1.0f));
        g.setColour (clipped ? Theme::danger : Theme::dimText);
        g.fillRect (0.0f, zero - (clipped ? 2.0f : 0.5f), (float) getWidth(), clipped ? 3.0f : 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clipped = false;
        repaint();
    }

private:
    float shown[8] {};
    int channels = 2;
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
        // Double-click the readout to type a volume.
        readout.setEditable (false, true, false);
        readout.setTooltip ("Double-click to type a volume, -60 to +12 dB");
        readout.onEditorShow = [this]
        {
            if (auto* e = readout.getCurrentTextEditor())
            {
                e->setText (juce::String (fader.getValue(), 1), false);
                e->selectAll();
                e->setJustification (juce::Justification::centred);
            }
        };
        readout.onTextChange = [this]
        {
            if (const auto typed = parseVolume (readout.getText()); typed.has_value() && fader.isEnabled())
            {
                fader.setValue (*typed, juce::dontSendNotification);
                edit ("Track Volume", false, [volume = *typed] (SessionTrack& t) { t.gainDb = volume; });
            }

            readout.setText (juce::String (fader.getValue(), 1) + " dB", juce::dontSendNotification);
        };

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
        pan.updateText();   // the text box was made before textFromValueFunction: show "C", not "0"

        // A 5.1 or 7.1 master's controls: the point, the width and the LFE's share.
        surroundPad.onChange = [this] (const SurroundPan& p, bool dragging)
        {
            if (! record ({ AutomationParameter::surroundX, AutomationParameter::surroundY }, { p.x, p.y }))
                edit ("Surround Pan", dragging, [x = p.x, y = p.y] (SessionTrack& t) { t.surroundPan.x = x; t.surroundPan.y = y; });
        };
        surroundPad.onRelease = [this] { finishRecording (AutomationParameter::surroundX); };

        for (auto* s : { &width, &lfe })
        {
            s->setSliderStyle (juce::Slider::LinearBar);
            Theme::follow (*s, juce::Slider::trackColourId, Theme::lineStrong);
        }

        width.setRange (0.0, 100.0, 1.0);
        width.setDoubleClickReturnValue (true, 100.0);
        width.setTooltip ("Surround width: how far apart its left and right channels play (0: together, as mono)");
        width.textFromValueFunction = [] (double value) { return "W " + juce::String ((int) value) + "%"; };
        width.onValueChange = [this]
        {
            const auto w = (float) width.getValue() / 100.0f;

            if (! record ({ AutomationParameter::surroundWidth }, { w }))
                edit ("Surround Width", true, [w] (SessionTrack& t) { t.surroundPan.width = w; });
        };
        width.onDragEnd = [this] { finishRecording (AutomationParameter::surroundWidth); };

        // The far left is off; otherwise -60 to +10 dB.
        lfe.setRange (-61.0, (double) SurroundPan::maxLfeDb, 1.0);
        lfe.setDoubleClickReturnValue (true, -61.0);
        lfe.setTooltip ("How much also goes to the LFE (far left: none)");
        lfe.textFromValueFunction = [] (double value)
        {
            return value < -60.0 ? juce::String ("LFE off") : "LFE " + juce::String ((int) value) + " dB";
        };
        lfe.onValueChange = [this]
        {
            const auto db = lfe.getValue() < -60.0 ? SurroundPan::lfeOff : (float) lfe.getValue();

            if (! record ({ AutomationParameter::lfe }, { juce::jmax (-60.0f, db) }))
                edit ("LFE Level", true, [db] (SessionTrack& t) { t.surroundPan.lfeDb = db; });
        };
        lfe.onDragEnd = [this] { finishRecording (AutomationParameter::lfe); };

        width.updateText();
        lfe.updateText();

        for (auto* c : std::initializer_list<juce::Component*> { &surroundPad, &width, &lfe })
            addChildComponent (c);

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
        readout.setEditable (false, trackId != 0 && ! volumeAutomated, false);
        pan.setEnabled (! panAutomated);
        fader.setTooltip (isFolder ? "Folder volume: added to the volume of each track and bus in it"
                                   : volumeAutomated ? "Volume follows its envelope" : "Volume");
        pan.setTooltip (panAutomated ? "Pan follows its envelope" : "Pan");

        const auto db = t.valueAt (AutomationParameter::volume, frame);

        if (! fader.isMouseButtonDown())
            fader.setValue (db, juce::dontSendNotification);

        if (! pan.isMouseButtonDown())
            pan.setValue (std::round (t.valueAt (AutomationParameter::pan, frame) * 100.0f), juce::dontSendNotification);

        if (! readout.isBeingEdited())
            readout.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);

        // The surround controls, in a 5.1 master; a 5.1 track has neither them nor a pan.
        const auto showSurround = view.engine.session.getState()->surround && ! isFolder && ! t.fiveOne;
        pan.setEnabled (! panAutomated && ! t.fiveOne);

        if (t.fiveOne)
            pan.setTooltip ("A 5.1 track plays its channels to their own speakers: it has no pan");

        if (surroundPad.isVisible() != showSurround)
        {
            for (auto* c : std::initializer_list<juce::Component*> { &surroundPad, &width, &lfe })
                c->setVisible (showSurround);

            resized();
        }

        // Each part that follows an envelope shows its value at the playhead and can't be set.
        const auto& envelopes = t.surroundEnvelopes;
        const auto pointAutomated = ! envelopes[0].isEmpty() || ! envelopes[1].isEmpty();
        const auto widthAutomated = ! envelopes[2].isEmpty(), lfeAutomated = ! envelopes[3].isEmpty();
        // While automation is being written they stay live, to record into their envelopes.
        const auto now = t.surroundPanAt (frame);
        const auto writing = view.writer != nullptr && view.writer->isWritingAutomation();
        const juce::String writeTip = writing ? "; move it while playing to write automation" : "";
        surroundPad.setEnabled (! pointAutomated || writing);
        width.setEnabled (! widthAutomated || writing);
        lfe.setEnabled (! lfeAutomated || writing);
        surroundPad.setTooltip ((pointAutomated && ! writing ? "Where it plays in the surround master follows its envelopes"
                                                             : "Where it plays in the surround master: drag the point, double-click to put it back in front")
                                + writeTip);
        width.setTooltip ((widthAutomated && ! writing ? "Surround width follows its envelope"
                                                       : "Surround width: how far apart its left and right channels play (0: together, as mono)")
                          + writeTip);
        lfe.setTooltip ((lfeAutomated && ! writing ? "The LFE's share follows its envelope" : "How much also goes to the LFE (far left: none)") + writeTip);

        surroundPad.setSevenOne (view.engine.session.getState()->sevenOne);

        if (! surroundPad.isMouseButtonDown())
            surroundPad.setPan (now);

        if (! width.isMouseButtonDown())
            width.setValue (std::round (now.width * 100.0f), juce::dontSendNotification);

        if (! lfe.isMouseButtonDown())
            lfe.setValue (now.lfeDb > SurroundPan::lfeOff ? std::round (now.lfeDb) : -61.0, juce::dontSendNotification);

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
        const auto surround = surroundPad.isVisible();

        if (! surround)
            readout.setBounds (r.removeFromBottom (18));

        if (trackId == 0)
        {
            meter.setBounds (r.withSizeKeepingCentre (meter.getChannels() == 6 ? juce::jmin (r.getWidth(), 6 * 15 + 7) : 24, r.getHeight()));
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

        // A 5.1 or 7.1 master's (wider) strip: the fader, meter and readout on the right, and on the left
        // the surround pad, as large a square as fits over the width and LFE sliders.
        if (surround)
        {
            auto right = r.removeFromRight (44);
            r.removeFromRight (6);
            readout.setBounds (right.removeFromBottom (18));
            right.removeFromBottom (4);
            meter.setBounds (right.removeFromRight (16));
            right.removeFromRight (4);
            fader.setBounds (right);

            lfe.setBounds (r.removeFromBottom (18));
            r.removeFromBottom (2);
            width.setBounds (r.removeFromBottom (18));
            r.removeFromBottom (4);
            const auto side = juce::jmin (r.getWidth(), r.getHeight());
            surroundPad.setBounds (r.withSizeKeepingCentre (side, side).withY (r.getY()));
            return;
        }

        meter.setBounds (r.removeFromRight (16));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

    const juce::uint32 trackId;
    Meter meter;
    juce::Slider fader, pan;
    SurroundPad surroundPad;
    juce::Slider width, lfe;
    juce::TextButton mute { "M" }, solo { "S" }, polarity { juce::String (juce::CharPointer_UTF8 ("\xc3\x98")) };
    juce::Label readout;

private:
    /** A surround control moved: while automation is written and the session plays, it records
        into its envelopes (starting a pass, or going on with this control's); an automated control
        does nothing otherwise. False to edit the setting. */
    bool record (const std::vector<AutomationTarget>& parameters, const std::vector<float>& values)
    {
        auto& player = view.engine.getPlayer();
        const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session;

        if (auto* w = view.writer; w != nullptr)
        {
            if (w->isAutomationPassUnderWay (trackId, parameters.front()))
            {
                w->continueAutomationPass (player.getPositionFrames(), values);
                return true;
            }

            if (w->isWritingAutomation() && playing)
            {
                w->startAutomationPass (trackId, parameters, player.getPositionFrames(), values);
                return true;
            }
        }

        const auto* t = view.engine.session.getState()->findTrack (trackId);
        return t != nullptr && std::any_of (parameters.begin(), parameters.end(), [t] (auto& p) { return ! t->envelope (p).isEmpty(); });
    }

    /** The control was let go: its pass, if one is under way, ends. */
    void finishRecording (AutomationParameter p)
    {
        if (view.writer != nullptr && view.writer->isAutomationPassUnderWay (trackId, p))
            view.writer->finishAutomationPass();
    }

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
    juce::Label name;
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

    // A 5.1 or 7.1 master's strips are wider, and its meter has six or eight channels.
    if (std::exchange (shownChannels, state->masterChannels()) != state->masterChannels())
    {
        master->meter.setChannels (state->masterChannels());
        master->meter.setTooltip (state->sevenOne ? "The 7.1 master's eight channels, L R C LFE Lrs Rrs Lss Rss, as mixed: before the effects rack and any fold-down"
                                  : state->surround ? "The 5.1 master's six channels, L R C LFE Ls Rs, as mixed: before the effects rack and any fold-down"
                                                    : "What goes to the outputs, after the effects rack");
        resized();
        master->resized();
    }
}

MixerView::Strip* MixerView::findStrip (juce::uint32 trackId) const
{
    for (auto& s : strips)
        if (s->trackId == trackId)
            return s.get();

    return nullptr;
}

juce::Slider* MixerView::findFader (juce::uint32 trackId) const   { auto* s = findStrip (trackId); return s != nullptr ? &s->fader : nullptr; }
juce::Label* MixerView::findReadout (juce::uint32 trackId) const  { auto* s = findStrip (trackId); return s != nullptr && trackId != 0 ? &s->readout : nullptr; }

std::optional<float> MixerView::parseVolume (const juce::String& text)
{
    auto t = text.trim();

    if (t.endsWithIgnoreCase ("db"))
        t = t.dropLastCharacters (2).trimEnd();

    if (t.startsWithChar ('+'))
        t = t.substring (1);

    // Digits, one point and a leading minus only: no stray letters or empty text.
    if (t.isEmpty() || ! t.containsOnly ("-.0123456789") || t.lastIndexOfChar ('-') > 0 || t.indexOfChar ('.') != t.lastIndexOfChar ('.')
        || ! t.containsAnyOf ("0123456789"))
        return std::nullopt;

    const auto value = std::round (t.getDoubleValue() * 10.0) / 10.0;

    if (value < -60.0 || value > 12.0)
        return std::nullopt;

    return (float) value;
}

juce::Slider* MixerView::findPan (juce::uint32 trackId) const     { auto* s = findStrip (trackId); return s != nullptr ? &s->pan : nullptr; }
SurroundPad* MixerView::findSurroundPad (juce::uint32 trackId) const { auto* s = findStrip (trackId); return s != nullptr && trackId != 0 ? &s->surroundPad : nullptr; }
juce::Slider* MixerView::findSurroundWidth (juce::uint32 trackId) const { auto* s = findStrip (trackId); return s != nullptr && trackId != 0 ? &s->width : nullptr; }
juce::Slider* MixerView::findLfe (juce::uint32 trackId) const     { auto* s = findStrip (trackId); return s != nullptr && trackId != 0 ? &s->lfe : nullptr; }
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

int MixerView::getMasterMeterChannels() const   { return master->meter.getChannels(); }

void MixerView::setAutomationWriter (MultitrackView* w)
{
    writer = w;
    shownWriting = writer != nullptr && writer->isWritingAutomation();
    rebuild();
}

void MixerView::updateMeters()
{
    auto& levels = engine.getPlayer().getLevels();

    for (size_t i = 0; i < strips.size(); ++i)
    {
        const auto [left, right] = levels.take ((int) i);
        strips[i]->meter.push (left, right);
    }

    // Writing automation turned on or off: the surround controls come alive, or follow again.
    if (const auto writing = writer != nullptr && writer->isWritingAutomation(); writing != shownWriting)
    {
        shownWriting = writing;
        rebuild();
    }

    // A 5.1 or 7.1 master: its channels as mixed; otherwise what went to the outputs.
    const auto [left, right] = engine.takeMasterPeaks();
    const auto wide = levels.takeMaster();

    if (master->meter.getChannels() > 2)
        master->meter.push (wide.data(), master->meter.getChannels());
    else
        master->meter.push (left, right);

    // An automated volume, pan or surround pan moves as the session plays.
    if (engine.getPlayer().isPlaying())
    {
        const auto state = engine.session.getState();
        const auto frame = engine.getPlayer().getPositionFrames();

        for (size_t i = 0; i < strips.size() && i < state->tracks.size(); ++i)
            if (! state->tracks[i].volumeEnvelope.isEmpty() || ! state->tracks[i].panEnvelope.isEmpty() || state->tracks[i].hasSurroundAutomation())
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
    master->setBounds (r.removeFromRight ((shownChannels > 2 ? surroundStripWidth : stripWidth) + 12));
    viewport.setBounds (r);
    const auto each = shownChannels > 2 ? surroundStripWidth : stripWidth;
    content.setSize (juce::jmax (r.getWidth(), (int) strips.size() * each), juce::jmax (0, r.getHeight() - viewport.getScrollBarThickness()));

    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * each, 0, each, content.getHeight());
}
