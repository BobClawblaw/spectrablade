#include "MultitrackView.h"

namespace
{
    const juce::Colour background (0xff15181d);
    const juce::Colour laneColour (0xff191c22);
    const juce::Colour laneAlternate (0xff1c2027);
    const juce::Colour laneLine (0xff2a2f37);
    const juce::Colour cursorColour (0xffffd34d);
    const juce::Colour playColour (0xffffffff);

    constexpr int edgeGrab = 6;   // pixels from a clip's edge that trim instead of move

    /** A raised-cosine fade's curve, from silence at silentX (on the bottom) to full level at
        fullX (on the top). */
    juce::Path fadeCurve (float silentX, float fullX, float top, float bottom)
    {
        juce::Path curve;

        for (int i = 0; i <= 32; ++i)
        {
            const auto t = (float) i / 32.0f;
            const auto level = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * t);
            const auto point = juce::Point<float> (silentX + (fullX - silentX) * t, bottom - level * (bottom - top));

            if (i == 0)
                curve.startNewSubPath (point);
            else
                curve.lineTo (point);
        }

        return curve;
    }

    juce::Colour trackColour (int index)
    {
        static const juce::uint32 colours[] = { 0xff4fc3a1, 0xff58a6ff, 0xffd98cff, 0xffffa657, 0xff7ee787, 0xffff7b72 };
        return juce::Colour (colours[(size_t) index % std::size (colours)]);
    }

    juce::String formatTime (double seconds, double secondsPerTick)
    {
        const auto mins = (int) (seconds / 60.0);
        const auto secs = seconds - mins * 60.0;
        const auto decimals = secondsPerTick >= 1.0 ? 0 : secondsPerTick >= 0.1 ? 1 : secondsPerTick >= 0.01 ? 2 : 3;
        return juce::String (mins) + ":" + (secs < 10.0 ? "0" : "") + juce::String (secs, decimals);
    }
}

//==============================================================================
class MultitrackView::TrackHeader final : public juce::Component
{
public:
    TrackHeader (MultitrackView& v, juce::uint32 id) : trackId (id), view (v)
    {
        name.setEditable (false, true);
        name.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        name.onTextChange = [this]
        {
            const auto text = name.getText().trim();
            view.editTrack (trackId, "Rename Track", false, [text] (SessionTrack& t) { if (text.isNotEmpty()) t.name = text; });
        };

        arm.setClickingTogglesState (true);
        arm.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0453a));
        arm.setTooltip ("Arm for recording: Record puts the take on this track");
        arm.onClick = [this] { view.setArmedTrack (arm.getToggleState() ? trackId : 0); };

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, b == &mute ? juce::Colour (0xffd0453a) : juce::Colour (0xffd8b43a));
        }

        sendsButton.setTooltip ("Send this track to buses");
        sendsButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3a7bd8));
        sendsButton.onClick = [this] { view.showSends (trackId, sendsButton); };

        effectsButton.setTooltip ("This track's effects");
        effectsButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3a7bd8));
        effectsButton.onClick = [this] { view.showEffectsMenu (trackId, effectsButton); };

        automationButton.setTooltip ("Volume and pan envelopes");
        automationButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3a7bd8));
        automationButton.onClick = [this] { view.showAutomationMenu (trackId, automationButton); };

        mute.setTooltip ("Mute");
        solo.setTooltip ("Solo: play only soloed tracks");
        mute.onClick = [this] { const auto on = mute.getToggleState(); view.editTrack (trackId, on ? "Mute" : "Unmute", false, [on] (SessionTrack& t) { t.mute = on; }); };
        solo.onClick = [this] { const auto on = solo.getToggleState(); view.editTrack (trackId, on ? "Solo" : "Unsolo", false, [on] (SessionTrack& t) { t.solo = on; }); };

        gain.setRange (-60.0, 12.0, 0.1);
        gain.setSkewFactorFromMidPoint (-12.0);
        gain.setDoubleClickReturnValue (true, 0.0);
        gain.setTextValueSuffix (" dB");
        gain.setTooltip ("Track volume");
        gain.onValueChange = [this]
        {
            const auto db = (float) gain.getValue();

            if (! controlChanged (AutomationParameter::volume, db))
                view.editTrack (trackId, "Track Volume", true, [db] (SessionTrack& t) { t.gainDb = db; });
        };

        pan.setRange (-100.0, 100.0, 1.0);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setTooltip ("Pan");
        pan.textFromValueFunction = [] (double value)
        {
            return value == 0.0 ? juce::String ("C") : juce::String (std::abs ((int) value)) + (value < 0.0 ? " L" : " R");
        };
        pan.updateText();
        pan.onValueChange = [this]
        {
            const auto p = (float) pan.getValue() / 100.0f;

            if (! controlChanged (AutomationParameter::pan, p))
                view.editTrack (trackId, "Track Pan", true, [p] (SessionTrack& t) { t.pan = p; });
        };

        for (auto* s : { &gain, &pan })
        {
            s->setSliderStyle (juce::Slider::LinearBar);
            s->setColour (juce::Slider::trackColourId, juce::Colour (0xff2c3542));
            s->onDragEnd = [this]
            {
                view.finishAutomationPass();
                view.rebuildHeaders();   // an automated control that wrote nothing goes back to its envelope
            };
        }

        for (auto* c : std::initializer_list<juce::Component*> { &name, &effectsButton, &arm, &mute, &solo, &gain, &pan, &sendsButton, &automationButton })
            addAndMakeVisible (c);
    }

    /** A control moved. While automation is being written and the session plays, it records
        into the envelope; an automated control does nothing otherwise. False to edit the setting. */
    bool controlChanged (AutomationParameter p, float value)
    {
        auto& player = view.engine.getPlayer();
        const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session;

        if (view.isAutomationPassUnderWay (trackId, p))
        {
            view.continueAutomationPass (player.getPositionFrames(), value);
            return true;
        }

        if (view.isWritingAutomation() && playing)
        {
            view.startAutomationPass (trackId, p, player.getPositionFrames(), value);
            return true;
        }

        const auto* t = view.shown().findTrack (trackId);
        return t != nullptr && ! t->envelope (p).isEmpty();
    }

    void update (const SessionTrack& t, int index, bool selected)
    {
        colour = t.isBus ? juce::Colour (0xff9aa3ad) : trackColour (index);
        isSelected = selected;
        isBus = t.isBus;
        sendsButton.setVisible (! isBus);
        arm.setVisible (! isBus);
        arm.setToggleState (view.armedTrackId == trackId, juce::dontSendNotification);
        sendsButton.setButtonText (t.sends.empty() ? juce::String ("Sends") : "Sends " + juce::String ((int) t.sends.size()));
        sendsButton.setToggleState (! t.sends.empty(), juce::dontSendNotification);
        effectsButton.setTooltip (isBus ? "This bus's effects" : "This track's effects");
        resized();

        if (! name.isBeingEdited())
            name.setText (t.name, juce::dontSendNotification);

        mute.setToggleState (t.mute, juce::dontSendNotification);
        solo.setToggleState (t.solo, juce::dontSendNotification);

        const auto& effects = view.engine.getTrackEffects();
        const auto slots = effects.getNumSlots (trackId);
        effectsButton.setButtonText (slots > 0 ? "FX " + juce::String (slots) : juce::String ("FX"));
        auto anyOn = false;

        for (int i = 0; i < slots; ++i)
            anyOn = anyOn || ! effects.isBypassed (trackId, i);

        effectsButton.setToggleState (anyOn, juce::dontSendNotification);

        // An automated volume or pan follows its envelope; the control shows its value at the cursor.
        const auto volumeAutomated = ! t.volumeEnvelope.isEmpty(), panAutomated = ! t.panEnvelope.isEmpty();
        automationButton.setToggleState (volumeAutomated || panAutomated, juce::dontSendNotification);
        const auto writing = view.isWritingAutomation();
        gain.setEnabled (! volumeAutomated || writing);
        pan.setEnabled (! panAutomated || writing);
        const juce::String automatedTip = writing ? " follows its envelope; drag while playing to write it" : " follows its envelope (A to edit it)";
        gain.setTooltip (volumeAutomated ? "Volume" + automatedTip : writing ? "Track volume; drag while playing to write automation" : "Track volume");
        pan.setTooltip (panAutomated ? "Pan" + automatedTip : writing ? "Pan; drag while playing to write automation" : "Pan");

        const auto frame = view.controlFrame();

        if (! gain.isMouseButtonDown())
            gain.setValue (t.valueAt (AutomationParameter::volume, frame), juce::dontSendNotification);

        if (! pan.isMouseButtonDown())
            pan.setValue (std::round (t.valueAt (AutomationParameter::pan, frame) * 100.0f), juce::dontSendNotification);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? juce::Colour (0xff232a33) : juce::Colour (isBus ? 0xff16191d : 0xff1b1e23));
        g.setColour (colour);
        g.fillRect (0, 0, isBus ? 8 : 4, getHeight());
        g.setColour (laneLine);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (10).reduced (4, 6);
        auto top = r.removeFromTop (24);
        solo.setBounds (top.removeFromRight (26));
        top.removeFromRight (4);
        mute.setBounds (top.removeFromRight (26));
        top.removeFromRight (4);

        if (! isBus)
        {
            arm.setBounds (top.removeFromRight (26));
            top.removeFromRight (4);
        }

        effectsButton.setBounds (top.removeFromRight (40));
        name.setBounds (top);
        r.removeFromTop (8);
        gain.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto bottom = r.removeFromTop (22);

        automationButton.setBounds (bottom.removeFromRight (26));
        bottom.removeFromRight (4);

        if (! isBus)
        {
            sendsButton.setBounds (bottom.removeFromRight (64));
            bottom.removeFromRight (4);
        }

        pan.setBounds (bottom);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (const auto& tracks = view.shown().tracks; ! tracks.empty())
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].id == trackId)
                    view.selectTrack ((int) i);
    }

    const juce::uint32 trackId;

private:
    MultitrackView& view;
    juce::Label name;
    juce::TextButton effectsButton { "FX" }, arm { "R" }, mute { "M" }, solo { "S" }, sendsButton { "Sends" }, automationButton { "A" };
    juce::Slider gain, pan;
    juce::Colour colour;
    bool isSelected = false, isBus = false;
};

//==============================================================================
/** A track's sends: one row per bus (on/off, level, before or after the track's volume), and a
    button that makes a new bus with a send to it. Follows the session, so undo shows here too. */
class MultitrackView::SendsPanel final : public juce::Component,
                                         private juce::ChangeListener
{
public:
    SendsPanel (AudioEngine& e, juce::uint32 id) : engine (e), trackId (id)
    {
        newBus.onClick = [this]
        {
            auto next = *engine.session.getState();
            const auto busId = next.addBus().id;
            SessionEdits::setSend (next, trackId, { busId, 0.0f, false });
            engine.session.apply (std::move (next), "Add Bus and Send");
        };

        addAndMakeVisible (newBus);
        addChildComponent (empty);
        empty.setText ("No buses yet.", juce::dontSendNotification);
        empty.setColour (juce::Label::textColourId, juce::Colours::grey);
        engine.session.addChangeListener (this);
        refresh();
    }

    ~SendsPanel() override
    {
        engine.session.removeChangeListener (this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        for (auto* row : rows)
        {
            auto line = r.removeFromTop (rowHeight).reduced (0, 2);
            row->on.setBounds (line.removeFromLeft (110));
            row->pre.setBounds (line.removeFromRight (44));
            line.removeFromRight (4);
            row->level.setBounds (line);
        }

        if (rows.isEmpty())
            empty.setBounds (r.removeFromTop (rowHeight));

        r.removeFromTop (6);
        newBus.setBounds (r.removeFromTop (24).removeFromLeft (120));
    }

private:
    struct Row
    {
        juce::uint32 busId = 0;
        juce::ToggleButton on;
        juce::Slider level;
        juce::TextButton pre { "Pre" };
    };

    static constexpr int rowHeight = 28;

    void changeListenerCallback (juce::ChangeBroadcaster*) override  { refresh(); }

    /** The send as the row shows it now. */
    SessionSend sendFrom (const Row& row) const
    {
        return { row.busId, (float) row.level.getValue(), row.pre.getToggleState() };
    }

    void apply (const Row& row, const juce::String& description, bool merge)
    {
        auto next = *engine.session.getState();

        if (row.on.getToggleState())
            SessionEdits::setSend (next, trackId, sendFrom (row));
        else
            SessionEdits::removeSend (next, trackId, row.busId);

        engine.session.apply (std::move (next), description, merge);
    }

    void refresh()
    {
        const auto current = engine.session.getState();
        const auto* track = current->findTrack (trackId);
        std::vector<const SessionTrack*> buses;

        for (auto& t : current->tracks)
            if (t.isBus)
                buses.push_back (&t);

        auto same = track != nullptr && (int) buses.size() == rows.size();

        for (size_t i = 0; same && i < buses.size(); ++i)
            same = rows[(int) i]->busId == buses[i]->id;

        if (! same)
        {
            rows.clear();

            for (auto* bus : buses)
            {
                auto* row = rows.add (new Row());
                row->busId = bus->id;
                row->level.setSliderStyle (juce::Slider::LinearBar);
                row->level.setRange (-60.0, 12.0, 0.1);
                row->level.setSkewFactorFromMidPoint (-12.0);
                row->level.setDoubleClickReturnValue (true, 0.0);
                row->level.setTextValueSuffix (" dB");
                row->level.setColour (juce::Slider::trackColourId, juce::Colour (0xff2c3542));
                row->pre.setClickingTogglesState (true);
                row->pre.setTooltip ("Send before the track's volume");
                row->pre.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3a7bd8));

                // Turning a level or Pre on a send that is off turns it on.
                row->on.onClick = [this, row] { apply (*row, row->on.getToggleState() ? "Add Send" : "Remove Send", false); };
                row->level.onValueChange = [this, row] { row->on.setToggleState (true, juce::dontSendNotification); apply (*row, "Send Level", true); };
                row->pre.onClick = [this, row] { row->on.setToggleState (true, juce::dontSendNotification); apply (*row, "Send Before or After Volume", false); };

                for (auto* c : std::initializer_list<juce::Component*> { &row->on, &row->level, &row->pre })
                    addAndMakeVisible (c);
            }

            empty.setVisible (rows.isEmpty());
            setSize (340, 16 + juce::jmax (1, rows.size()) * rowHeight + 30);
            resized();
        }

        for (auto* row : rows)
        {
            const auto* bus = current->findTrack (row->busId);
            const auto* send = track != nullptr ? track->findSend (row->busId) : nullptr;
            row->on.setButtonText (bus != nullptr ? bus->name : juce::String());
            row->on.setToggleState (send != nullptr, juce::dontSendNotification);
            row->pre.setToggleState (send != nullptr && send->preFader, juce::dontSendNotification);

            if (! row->level.isMouseButtonDown())
                row->level.setValue (send != nullptr ? send->gainDb : 0.0, juce::dontSendNotification);

            row->level.setAlpha (send != nullptr ? 1.0f : 0.5f);
        }
    }

    AudioEngine& engine;
    const juce::uint32 trackId;
    juce::OwnedArray<Row> rows;
    juce::Label empty;
    juce::TextButton newBus { "New Bus" };
};

//==============================================================================
MultitrackView::MultitrackView (AudioEngine& e) : engine (e)
{
    state = engine.session.getState();
    addAndMakeVisible (headerArea);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    engine.session.addChangeListener (this);
    rebuildHeaders();
    startTimerHz (30);
}

MultitrackView::~MultitrackView()
{
    engine.session.removeChangeListener (this);
}

//==============================================================================
void MultitrackView::showSends (juce::uint32 trackId, juce::Component& target)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<SendsPanel> (engine, trackId), target.getScreenBounds(), nullptr);
}

void MultitrackView::showAutomationMenu (juce::uint32 trackId, juce::Component& target)
{
    const auto* track = shown().findTrack (trackId);

    if (track == nullptr)
        return;

    enum { showVolume = 1, showPan, hide, clearVolume, clearPan };
    const auto showing = getShownEnvelope (trackId);
    juce::PopupMenu menu;
    menu.addItem (showVolume, "Show Volume Envelope", true, showing == AutomationParameter::volume);
    menu.addItem (showPan, "Show Pan Envelope", true, showing == AutomationParameter::pan);
    menu.addItem (hide, "Hide Envelope", showing.has_value());
    menu.addSeparator();
    menu.addItem (clearVolume, "Clear Volume Envelope", ! track->volumeEnvelope.isEmpty());
    menu.addItem (clearPan, "Clear Pan Envelope", ! track->panEnvelope.isEmpty());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this, trackId, safe = juce::Component::SafePointer<MultitrackView> (this)] (int result)
        {
            if (safe == nullptr)
                return;

            switch (result)
            {
                case showVolume: showEnvelope (trackId, AutomationParameter::volume); break;
                case showPan:    showEnvelope (trackId, AutomationParameter::pan); break;
                case hide:       showEnvelope (trackId, std::nullopt); break;
                case clearVolume:
                case clearPan:
                {
                    const auto parameter = result == clearVolume ? AutomationParameter::volume : AutomationParameter::pan;
                    auto next = *engine.session.getState();

                    if (SessionEdits::setEnvelope (next, trackId, parameter, {}))
                        engine.session.apply (std::move (next), result == clearVolume ? "Clear Volume Envelope" : "Clear Pan Envelope");

                    break;
                }
                default: break;
            }
        });
}

std::optional<AutomationParameter> MultitrackView::getShownEnvelope (juce::uint32 trackId) const
{
    if (const auto found = shownEnvelopes.find (trackId); found != shownEnvelopes.end())
        return found->second;

    return std::nullopt;
}

void MultitrackView::showEnvelope (juce::uint32 trackId, std::optional<AutomationParameter> parameter)
{
    if (parameter.has_value())
        shownEnvelopes[trackId] = *parameter;
    else
        shownEnvelopes.erase (trackId);

    repaint();
}

void MultitrackView::showEffectsMenu (juce::uint32 trackId, juce::Component& target)
{
    auto& effects = engine.getTrackEffects();
    const auto slots = effects.getNumSlots (trackId);

    juce::Array<juce::PluginDescription> types;

    for (auto& type : engine.knownPlugins.getTypes())
        if (! type.isInstrument)
            types.add (type);

    // Plug-in choices take ids from 1 (KnownPluginList); each slot's actions sit above them.
    enum { slotBase = 1000000, editSlot = 0, bypassSlot, moveSlotUp, moveSlotDown, removeSlot, actions };
    juce::PopupMenu menu;

    for (int i = 0; i < slots; ++i)
    {
        const auto* plugin = effects.getPlugin (trackId, i);
        const auto bypassed = effects.isBypassed (trackId, i);
        const auto id = slotBase + i * actions;
        juce::PopupMenu slot;
        slot.addItem (id + editSlot, "Edit...");
        slot.addItem (id + bypassSlot, "Bypass", true, bypassed);
        slot.addItem (id + moveSlotUp, "Move Up", i > 0);
        slot.addItem (id + moveSlotDown, "Move Down", i + 1 < slots);
        slot.addSeparator();
        slot.addItem (id + removeSlot, "Remove");
        menu.addSubMenu (juce::String (i + 1) + ". " + plugin->getName() + (bypassed ? " (bypassed)" : ""), slot);
    }

    if (const auto latency = effects.getChainLatency (trackId); latency > 0)
        menu.addItem (-1, "Latency " + juce::String (latency) + " frames, compensated", false);

    if (slots > 0)
        menu.addSeparator();

    if (types.isEmpty())
    {
        menu.addItem (-2, "No effects: scan with Plug-ins... first", false);
    }
    else
    {
        juce::PopupMenu add;
        juce::KnownPluginList::addToMenu (add, types, juce::KnownPluginList::sortByManufacturer);
        menu.addSubMenu ("Add Effect", add);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this, trackId, types, safe = juce::Component::SafePointer<MultitrackView> (this)] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result >= slotBase)
            {
                const auto slot = (result - slotBase) / actions;

                switch ((result - slotBase) % actions)
                {
                    case editSlot:
                        if (onEditPlugin != nullptr)
                            if (auto* plugin = engine.getTrackEffects().getPlugin (trackId, slot))
                                onEditPlugin (*plugin);
                        break;
                    case bypassSlot:   engine.setTrackPluginBypassed (trackId, slot, ! engine.getTrackEffects().isBypassed (trackId, slot)); break;
                    case moveSlotUp:   engine.moveTrackPlugin (trackId, slot, slot - 1); break;
                    case moveSlotDown: engine.moveTrackPlugin (trackId, slot, slot + 1); break;
                    case removeSlot:   engine.removeTrackPlugin (trackId, slot); break;
                    default: break;
                }

                return;
            }

            const auto chosen = juce::KnownPluginList::getIndexChosenByMenu (types, result);

            if (! juce::isPositiveAndBelow (chosen, types.size()))
                return;

            const auto name = types[chosen].name;
            engine.addTrackPlugin (trackId, types[chosen], [name] (const juce::String& error)
            {
                if (error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load " + name, error);
            });
        });
}

void MultitrackView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    state = engine.session.getState();

    if (! state->findClip (selectedClip).has_value())
        selectedClip = 0;

    if (state->findTrack (armedTrackId) == nullptr)
        armedTrackId = 0;

    for (auto it = shownEnvelopes.begin(); it != shownEnvelopes.end();)
        it = state->findTrack (it->first) == nullptr ? shownEnvelopes.erase (it) : std::next (it);

    rebuildHeaders();
    updateScrollBars();
    repaint();
}

void MultitrackView::timerCallback()
{
    engine.session.releaseUnusedStates();

    if (engine.getPlayer().isPlaying() && engine.getPlayer().getMode() == DocumentPlayerProcessor::Mode::session)
    {
        // Keep the play position in view.
        const auto x = xForFrame (engine.getPlayer().getPositionFrames());
        const auto area = timelineArea();

        if (x > (float) area.getRight() - 20.0f)
            setView ((double) engine.getPlayer().getPositionFrames() - 20.0 * framesPerPixel, framesPerPixel);

        // A control held still keeps writing its value as the play position moves on.
        if (pass.has_value() && ! pass->points.empty())
            continueAutomationPass (engine.getPlayer().getPositionFrames(), pass->points.back().value);

        // Automated controls show their envelope's value as it plays.
        controlsFollowPlay = std::any_of (shown().tracks.begin(), shown().tracks.end(),
                                          [] (auto& t) { return ! t.volumeEnvelope.isEmpty() || ! t.panEnvelope.isEmpty(); });

        if (controlsFollowPlay)
            rebuildHeaders();

        repaint();
    }
    else
    {
        finishAutomationPass();

        if (std::exchange (controlsFollowPlay, false))
            rebuildHeaders();   // back to the cursor's values
    }
}

juce::int64 MultitrackView::controlFrame() const
{
    auto& player = engine.getPlayer();
    return player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session ? player.getPositionFrames() : cursor;
}

void MultitrackView::setWritingAutomation (bool on)
{
    if (! on)
        finishAutomationPass();

    writingAutomation = on;
    rebuildHeaders();
}

void MultitrackView::startAutomationPass (juce::uint32 trackId, AutomationParameter p, juce::int64 frame, float value)
{
    finishAutomationPass();

    const auto current = engine.session.getState();

    if (current->findTrack (trackId) == nullptr)
        return;

    pass = AutomationPass { trackId, p, *current, { { juce::jmax ((juce::int64) 0, frame), value } } };
    applyAutomationPass (true);
}

void MultitrackView::continueAutomationPass (juce::int64 frame, float value)
{
    if (! pass.has_value())
        return;

    auto& points = pass->points;

    if (frame < points.back().frame)
    {
        finishAutomationPass();
        return;
    }

    const auto held = points.size() >= 2 && juce::exactlyEqual (points[points.size() - 2].value, points.back().value)
                      && juce::exactlyEqual (points.back().value, value);

    if (frame == points.back().frame || held)
    {
        // The same frame, or a value held still: the last point moves rather than adding one.
        if (frame == points.back().frame && juce::exactlyEqual (points.back().value, value))
            return;

        points.back() = { frame, value };
    }
    else
    {
        points.push_back ({ frame, value });
    }

    applyAutomationPass (false);
}

void MultitrackView::finishAutomationPass()
{
    pass.reset();
}

void MultitrackView::applyAutomationPass (bool first)
{
    auto next = pass->base;
    const auto returnFrames = (juce::int64) std::llround (returnSeconds * next.sampleRate);
    SessionEdits::writeAutomation (next, pass->trackId, pass->parameter, pass->points, returnFrames);

    // The first write is a step of its own; the rest of the pass merges into it.
    engine.session.apply (std::move (next), pass->parameter == AutomationParameter::volume ? "Write Volume Automation"
                                                                                           : "Write Pan Automation", ! first);
}

void MultitrackView::rebuildHeaders()
{
    const auto& tracks = shown().tracks;
    auto same = headers.size() == tracks.size();

    for (size_t i = 0; same && i < tracks.size(); ++i)
        same = headers[i]->trackId == tracks[i].id;

    if (! same)
    {
        headers.clear();

        for (auto& t : tracks)
            headerArea.addAndMakeVisible (*headers.emplace_back (std::make_unique<TrackHeader> (*this, t.id)));

        layoutHeaders();
    }

    for (size_t i = 0; i < tracks.size(); ++i)
        headers[i]->update (tracks[i], (int) i, tracks[i].id == selectedTrackId);
}

void MultitrackView::layoutHeaders()
{
    for (size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, (int) i * trackHeight - scrollY, headerArea.getWidth(), trackHeight);
}

void MultitrackView::resized()
{
    auto r = getLocalBounds();
    hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (headerWidth));
    vScroll.setBounds (r.removeFromRight (12).withTrimmedTop (rulerHeight));
    headerArea.setBounds (r.removeFromLeft (headerWidth).withTrimmedTop (rulerHeight));
    layoutHeaders();
    updateScrollBars();
}

//==============================================================================
juce::Rectangle<int> MultitrackView::timelineArea() const
{
    return getLocalBounds().withTrimmedLeft (headerWidth).withTrimmedRight (12).withTrimmedBottom (12).withTrimmedTop (rulerHeight);
}

int MultitrackView::laneTop (int track) const
{
    return rulerHeight + track * trackHeight - scrollY;
}

int MultitrackView::trackAt (int y) const
{
    if (y < rulerHeight)
        return -1;

    return (y - rulerHeight + scrollY) / trackHeight;
}

float MultitrackView::xForFrame (juce::int64 frame) const
{
    return (float) headerWidth + (float) (((double) frame - viewStart) / framesPerPixel);
}

juce::int64 MultitrackView::frameForX (float x) const
{
    return juce::jmax ((juce::int64) 0, (juce::int64) std::llround (viewStart + (double) (x - (float) headerWidth) * framesPerPixel));
}

juce::Rectangle<int> MultitrackView::clipBounds (int track, const SessionClip& c) const
{
    const auto x0 = xForFrame (c.position), x1 = xForFrame (c.getEnd());
    return juce::Rectangle<float> (x0, (float) laneTop (track) + 3.0f, juce::jmax (2.0f, x1 - x0), (float) trackHeight - 6.0f).toNearestInt();
}

std::optional<std::pair<int, int>> MultitrackView::clipAt (juce::Point<int> p) const
{
    const auto& tracks = shown().tracks;
    const auto t = trackAt (p.y);

    if (! juce::isPositiveAndBelow (t, (int) tracks.size()) || p.x < headerWidth)
        return std::nullopt;

    // The topmost (last drawn) clip wins.
    for (auto c = (int) tracks[(size_t) t].clips.size() - 1; c >= 0; --c)
        if (clipBounds (t, tracks[(size_t) t].clips[(size_t) c]).expanded (2, 0).contains (p))
            return std::pair<int, int> { t, c };

    return std::nullopt;
}

//==============================================================================
void MultitrackView::setView (double start, double fpp)
{
    const auto width = (double) juce::jmax (1, timelineArea().getWidth());
    framesPerPixel = juce::jlimit (0.05, 1.0e6, fpp);
    const auto length = (double) juce::jmax (shown().getLength(), (juce::int64) (shown().sampleRate * 60.0));
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - width * framesPerPixel * 0.5), start);
    updateScrollBars();
    repaint();
}

void MultitrackView::zoomAround (float x, double factor)
{
    const auto anchor = viewStart + (double) (x - (float) headerWidth) * framesPerPixel;
    const auto fpp = framesPerPixel * factor;
    setView (anchor - (double) (x - (float) headerWidth) * fpp, fpp);
}

void MultitrackView::zoomIn()     { zoomAround ((float) xForFrame (cursor), 0.5); }
void MultitrackView::zoomOut()    { zoomAround ((float) xForFrame (cursor), 2.0); }

void MultitrackView::zoomToFit()
{
    const auto length = (double) juce::jmax (shown().getLength(), (juce::int64) (shown().sampleRate * 10.0));
    setView (0.0, length * 1.05 / juce::jmax (1, timelineArea().getWidth()));
}

void MultitrackView::updateScrollBars()
{
    const auto width = (double) juce::jmax (1, timelineArea().getWidth());
    const auto length = (double) juce::jmax (shown().getLength(), (juce::int64) (shown().sampleRate * 60.0));
    hScroll.setRangeLimits (0.0, juce::jmax (length, viewStart + width * framesPerPixel), juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, width * framesPerPixel, juce::dontSendNotification);

    const auto visible = juce::jmax (1, timelineArea().getHeight());
    const auto total = ((int) shown().tracks.size() + 1) * trackHeight;
    scrollY = juce::jlimit (0, juce::jmax (0, total - visible), scrollY);
    vScroll.setRangeLimits (0.0, (double) juce::jmax (total, visible), juce::dontSendNotification);
    vScroll.setCurrentRange ((double) scrollY, (double) visible, juce::dontSendNotification);
    layoutHeaders();
}

void MultitrackView::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &hScroll)
    {
        setView (newRangeStart, framesPerPixel);
    }
    else
    {
        scrollY = (int) newRangeStart;
        layoutHeaders();
        repaint();
    }
}

//==============================================================================
juce::Rectangle<float> MultitrackView::fadeHandle (int track, const SessionClip& c, bool fadeOut) const
{
    const auto r = clipBounds (track, c).toFloat();
    const auto x = fadeOut ? xForFrame (c.getEnd() - c.fadeOut) : xForFrame (c.position + c.fadeIn);
    return juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ juce::jlimit (r.getX() + 4.0f, r.getRight() - 4.0f, x), r.getY() + 8.0f });
}

MultitrackView::Drag MultitrackView::fadeHandleAt (juce::Point<int> p, int track, const SessionClip& c) const
{
    if (clipBounds (track, c).getWidth() <= 3 * edgeGrab)
        return Drag::none;

    const auto in = fadeHandle (track, c, false), out = fadeHandle (track, c, true);
    const auto point = p.toFloat();
    const auto onIn = in.expanded (3.0f).contains (point), onOut = out.expanded (3.0f).contains (point);

    // Where the handles meet, the nearer one.
    if (onIn && onOut)
        return point.getDistanceFrom (in.getCentre()) <= point.getDistanceFrom (out.getCentre()) ? Drag::fadeIn : Drag::fadeOut;

    return onIn ? Drag::fadeIn : onOut ? Drag::fadeOut : Drag::none;
}

namespace
{
    /** Volume is laid out like the track's fader, with -12 dB half way up. */
    juce::NormalisableRange<float> envelopeScale (AutomationParameter p)
    {
        const auto range = automationRange (p);
        juce::NormalisableRange<float> scale (range.getStart(), range.getEnd());

        if (p == AutomationParameter::volume)
            scale.setSkewForCentre (-12.0f);

        return scale;
    }

    // Envelope lines stay this far inside their lane.
    constexpr float envelopeMargin = 8.0f;
}

float MultitrackView::yForValue (int track, AutomationParameter p, float value) const
{
    const auto top = (float) laneTop (track) + envelopeMargin, bottom = (float) (laneTop (track) + trackHeight) - envelopeMargin;
    auto proportion = envelopeScale (p).convertTo0to1 (automationRange (p).clipValue (value));

    // Pan: left at the top.
    if (p == AutomationParameter::pan)
        proportion = 1.0f - proportion;

    return bottom - proportion * (bottom - top);
}

float MultitrackView::valueForY (int track, AutomationParameter p, float y) const
{
    const auto top = (float) laneTop (track) + envelopeMargin, bottom = (float) (laneTop (track) + trackHeight) - envelopeMargin;
    auto proportion = juce::jlimit (0.0f, 1.0f, (bottom - y) / (bottom - top));

    if (p == AutomationParameter::pan)
        proportion = 1.0f - proportion;

    return envelopeScale (p).convertFrom0to1 (proportion);
}

std::optional<MultitrackView::EnvelopeHit> MultitrackView::envelopeAt (juce::Point<int> p) const
{
    const auto& tracks = shown().tracks;
    const auto t = trackAt (p.y);

    if (! juce::isPositiveAndBelow (t, (int) tracks.size()) || p.x < headerWidth)
        return std::nullopt;

    const auto& track = tracks[(size_t) t];
    const auto parameter = getShownEnvelope (track.id);

    if (! parameter.has_value())
        return std::nullopt;

    const auto& points = track.envelope (*parameter).points;
    EnvelopeHit hit { t, *parameter, -1 };
    auto nearest = 7.0f;

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto distance = p.toFloat().getDistanceFrom ({ xForFrame (points[i].frame), yForValue (t, *parameter, points[i].value) });

        if (distance < nearest)
        {
            nearest = distance;
            hit.point = (int) i;
        }
    }

    if (hit.point >= 0)
        return hit;

    const auto lineY = yForValue (t, *parameter, track.valueAt (*parameter, frameForX ((float) p.x)));
    return std::abs ((float) p.y - lineY) <= 5.0f ? std::optional<EnvelopeHit> (hit) : std::nullopt;
}

void MultitrackView::drawEnvelope (juce::Graphics& g, int track, AutomationParameter p) const
{
    const auto& t = shown().tracks[(size_t) track];
    const auto& points = t.envelope (p).points;
    const auto area = timelineArea();
    const auto colour = p == AutomationParameter::volume ? juce::Colour (0xff7ee0ff) : juce::Colour (0xffff9de0);
    const auto left = (float) area.getX(), right = (float) area.getRight();

    g.setColour (colour.withAlpha (0.8f));
    g.setFont (11.0f);
    g.drawText (juce::String (p == AutomationParameter::volume ? "Volume" : "Pan (left at the top)")
                    + (points.empty() ? ": click the line to add a point" : ""),
                juce::Rectangle<float> (left + 6.0f, (float) laneTop (track) + (float) trackHeight - 20.0f, 300.0f, 16.0f),
                juce::Justification::centredLeft, false);

    if (points.empty())
    {
        const auto y = yForValue (track, p, t.valueAt (p, 0));
        const float dashes[] = { 4.0f, 4.0f };
        g.drawDashedLine ({ left, y, right, y }, dashes, 2, 1.5f);
        return;
    }

    juce::Path line;
    line.startNewSubPath (left, yForValue (track, p, points.front().value));

    for (auto& point : points)
        line.lineTo (xForFrame (point.frame), yForValue (track, p, point.value));

    line.lineTo (right, yForValue (track, p, points.back().value));
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (1.5f));

    for (auto& point : points)
    {
        const auto x = xForFrame (point.frame);

        if (x < left - 5.0f || x > right + 5.0f)
            continue;

        const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, yForValue (track, p, point.value) });
        g.fillEllipse (dot);
        g.setColour (juce::Colours::black);
        g.drawEllipse (dot, 1.0f);
        g.setColour (colour);
    }
}

double MultitrackView::rulerTick() const
{
    // The finest spacing that leaves at least 100 pixels between marks.
    const auto secondsPerPixel = framesPerPixel / shown().sampleRate;
    double tick = 0.001;

    for (auto candidate : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
    {
        tick = candidate;

        if (candidate / secondsPerPixel >= 100.0)
            break;
    }

    return tick;
}

SessionSnap::Targets MultitrackView::snapTargets (juce::uint32 ignoreClip, bool toCursorAndRange) const
{
    SessionSnap::Targets targets;
    targets.ignoreClip = ignoreClip;
    targets.gridStep = (juce::int64) std::llround (rulerTick() * shown().sampleRate);

    if (toCursorAndRange)
    {
        targets.extra.push_back (cursor);

        if (! timeSelection.isEmpty())
        {
            targets.extra.push_back (timeSelection.getStart());
            targets.extra.push_back (timeSelection.getEnd());
        }
    }

    return targets;
}

juce::int64 MultitrackView::snapTolerance() const
{
    return (juce::int64) std::llround (8.0 * framesPerPixel);
}

juce::int64 MultitrackView::snapped (juce::int64 frame, juce::uint32 ignoreClip, bool toCursorAndRange) const
{
    if (! snapping)
        return frame;

    // Snap against the session as it was before the drag, so a dragged clip can't snap to itself.
    return SessionSnap::snap (*state, frame, snapTolerance(), snapTargets (ignoreClip, toCursorAndRange));
}

void MultitrackView::drawRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (juce::Colour (0xff1b1e23));
    g.fillRect (area);

    const auto rate = shown().sampleRate;
    const auto secondsPerPixel = framesPerPixel / rate;
    const auto tick = rulerTick();

    g.setFont (11.0f);
    const auto startSeconds = viewStart / rate;
    const auto endSeconds = startSeconds + area.getWidth() * secondsPerPixel;

    for (auto t = std::floor (startSeconds / tick) * tick; t <= endSeconds; t += tick)
    {
        const auto x = xForFrame ((juce::int64) std::llround (t * rate));
        g.setColour (juce::Colours::grey);
        g.drawVerticalLine ((int) x, (float) area.getBottom() - 6.0f, (float) area.getBottom());
        g.setColour (juce::Colours::lightgrey);
        g.drawText (formatTime (juce::jmax (0.0, t), tick), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 80.0f, (float) area.getHeight() - 4.0f),
                    juce::Justification::centredLeft, false);
    }
}

void MultitrackView::drawClip (juce::Graphics& g, int track, const SessionClip& c) const
{
    const auto r = clipBounds (track, c);
    const auto colour = trackColour (track);
    const auto selected = c.id == selectedClip;

    g.setColour (colour.withAlpha (0.18f));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);

    auto body = r.reduced (1);
    auto title = body.removeFromTop (16);
    g.setColour (colour.withAlpha (0.45f));
    g.fillRect (title);
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);
    g.drawText (c.name, title.withTrimmedLeft (12).withTrimmedRight (10), juce::Justification::centredLeft, true);   // clear of the fade handles

    // Waveform: one peak per pixel column, each channel in its own band.
    if (c.audio != nullptr && body.getHeight() > 4)
    {
        const auto channels = juce::jmin (2, c.audio->getNumChannels());
        const auto clipGain = juce::Decibels::decibelsToGain (c.gainDb);
        const auto left = juce::jmax (body.getX(), headerWidth);
        const auto right = juce::jmin (body.getRight(), timelineArea().getRight());
        g.setColour (colour.withAlpha (0.9f));

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto bandHeight = (float) body.getHeight() / (float) channels;
            const auto mid = (float) body.getY() + bandHeight * ((float) ch + 0.5f);

            for (int x = left; x < right; ++x)
            {
                const auto f0 = juce::jlimit (c.position, c.getEnd(), frameForX ((float) x));
                const auto f1 = juce::jlimit (c.position, c.getEnd(), frameForX ((float) x + 1.0f));
                const auto a0 = c.offset + (f0 - c.position);
                const auto a1 = juce::jmax (a0 + 1, c.offset + (f1 - c.position));
                const auto peak = c.audio->getPeak (ch, a0, juce::jmin (a1, c.audio->getNumFrames()));
                const auto top = mid - juce::jlimit (0.0f, 1.0f, peak.getEnd() * clipGain) * bandHeight * 0.5f;
                const auto bottom = mid - juce::jlimit (-1.0f, 0.0f, peak.getStart() * clipGain) * bandHeight * 0.5f;
                g.drawVerticalLine (x, top, juce::jmax (top + 1.0f, bottom));
            }
        }
    }

    // Fades as the raised-cosine curves they play, from silence up to full level.
    const auto drawFade = [&] (float silentX, float fullX)
    {
        const auto top = (float) r.getY() + 16.0f, bottom = (float) r.getBottom();
        const auto curve = fadeCurve (silentX, fullX, top, bottom);
        auto shade = curve;
        shade.lineTo (silentX, top);
        shade.closeSubPath();
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillPath (shade);
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.strokePath (curve, juce::PathStrokeType (1.0f));
    };

    if (c.fadeIn > 0)
        drawFade ((float) r.getX(), xForFrame (c.position + c.fadeIn));

    if (c.fadeOut > 0)
        drawFade ((float) r.getRight(), xForFrame (c.getEnd() - c.fadeOut));

    if (r.getWidth() > 3 * edgeGrab)
    {
        g.setColour (juce::Colours::white.withAlpha (selected ? 0.95f : 0.6f));

        for (auto fadeOut : { false, true })
            g.fillRect (fadeHandle (track, c, fadeOut));
    }

    g.setColour (selected ? cursorColour : colour.withAlpha (0.7f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, selected ? 2.0f : 1.0f);
}

void MultitrackView::paint (juce::Graphics& g)
{
    g.fillAll (background);
    const auto area = timelineArea();
    const auto& s = shown();

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area);

        for (int t = 0; t < (int) s.tracks.size(); ++t)
        {
            const auto lane = juce::Rectangle<int> (area.getX(), laneTop (t), area.getWidth(), trackHeight);
            g.setColour (s.tracks[(size_t) t].id == selectedTrackId ? juce::Colour (0xff20262e) : (t % 2 == 0 ? laneColour : laneAlternate));
            g.fillRect (lane);
            g.setColour (laneLine);
            g.drawHorizontalLine (lane.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());

            const auto& track = s.tracks[(size_t) t];

            if (track.isBus)
            {
                juce::StringArray feeders;

                for (auto& other : s.tracks)
                    if (other.findSend (track.id) != nullptr)
                        feeders.add (other.name);

                g.setColour (juce::Colour (0x0affffff));
                g.fillRect (lane.reduced (0, 3));
                g.setColour (juce::Colours::grey);
                g.setFont (13.0f);
                g.drawText (feeders.isEmpty() ? "Bus: nothing sends here yet. Use a track's Sends button."
                                              : "Bus: from " + feeders.joinIntoString (", "),
                            lane.reduced (12, 0), juce::Justification::centredLeft);
            }

            for (auto& c : track.clips)
                drawClip (g, t, c);

            // Where clips overlap, a later clip covers the fade out under it: draw the fade outs
            // again on top, so a crossfade shows both its curves.
            for (auto& c : track.clips)
            {
                if (c.fadeOut <= 0)
                    continue;

                const auto r = clipBounds (t, c);
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.strokePath (fadeCurve ((float) r.getRight(), xForFrame (c.getEnd() - c.fadeOut), (float) r.getY() + 16.0f, (float) r.getBottom()),
                              juce::PathStrokeType (1.0f));
            }

            if (const auto parameter = getShownEnvelope (track.id))
                drawEnvelope (g, t, *parameter);
        }

        if (s.tracks.empty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (15.0f);
            g.drawText ("Drop audio files here, or choose Multitrack > Import Audio", area, juce::Justification::centred);
        }

        if (! timeSelection.isEmpty())
        {
            const auto x0 = xForFrame (timeSelection.getStart()), x1 = xForFrame (timeSelection.getEnd());
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()));
        }

        if (! recording.isEmpty())
        {
            auto lane = (int) s.tracks.size();

            for (int t = 0; t < (int) s.tracks.size(); ++t)
                if (s.tracks[(size_t) t].id == armedTrackId)
                    lane = t;

            const auto x0 = xForFrame (recording.getStart()), x1 = xForFrame (recording.getEnd());
            const auto r = juce::Rectangle<float> (x0, (float) laneTop (lane) + 2.0f, juce::jmax (1.0f, x1 - x0), (float) trackHeight - 4.0f);
            g.setColour (juce::Colour (0xffd0453a).withAlpha (0.3f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colour (0xffd0453a));
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        }

        g.setColour (cursorColour);
        g.drawVerticalLine ((int) xForFrame (cursor), (float) area.getY(), (float) area.getBottom());

        auto& player = engine.getPlayer();

        if (player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session)
        {
            g.setColour (playColour);
            g.drawVerticalLine ((int) xForFrame (player.getPositionFrames()), (float) area.getY(), (float) area.getBottom());
        }
    }

    drawRuler (g, { area.getX(), 0, area.getWidth(), rulerHeight });

    if (! timeSelection.isEmpty())
    {
        const auto x0 = juce::jmax ((float) area.getX(), xForFrame (timeSelection.getStart()));
        const auto x1 = juce::jmin ((float) area.getRight(), xForFrame (timeSelection.getEnd()));
        g.setColour (cursorColour.withAlpha (0.35f));

        if (x1 > x0)
            g.fillRect (juce::Rectangle<float> (x0, (float) rulerHeight - 5.0f, x1 - x0, 5.0f));
    }
    g.setColour (juce::Colour (0xff1b1e23));
    g.fillRect (0, 0, headerWidth, rulerHeight);
    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    g.drawText (juce::String (s.sampleRate / 1000.0, 1) + " kHz session", juce::Rectangle<int> (8, 0, headerWidth - 8, rulerHeight),
                juce::Justification::centredLeft);
}

//==============================================================================
void MultitrackView::setCursor (juce::int64 frame)
{
    cursor = juce::jmax ((juce::int64) 0, frame);

    // Automated controls show their envelope's value at the cursor.
    if (std::any_of (headers.begin(), headers.end(), [this] (auto& h)
                     {
                         const auto* t = shown().findTrack (h->trackId);
                         return t != nullptr && (! t->volumeEnvelope.isEmpty() || ! t->panEnvelope.isEmpty());
                     }))
        rebuildHeaders();

    repaint();
}

void MultitrackView::setTimeSelection (juce::Range<juce::int64> range)
{
    timeSelection = range.getStart() < 0 ? range.movedToStartAt (0) : range;
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void MultitrackView::setArmedTrack (juce::uint32 trackId)
{
    const auto* t = shown().findTrack (trackId);
    armedTrackId = t != nullptr && ! t->isBus ? trackId : 0;
    rebuildHeaders();
    repaint();
}

void MultitrackView::showRecording (juce::Range<juce::int64> range)
{
    if (range != recording)
    {
        recording = range;
        repaint();
    }
}

int MultitrackView::getSelectedTrack() const
{
    const auto& tracks = shown().tracks;

    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == selectedTrackId)
            return (int) i;

    return -1;
}

void MultitrackView::selectTrack (int track)
{
    const auto& tracks = shown().tracks;
    selectedTrackId = juce::isPositiveAndBelow (track, (int) tracks.size()) ? tracks[(size_t) track].id : 0;
    rebuildHeaders();
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void MultitrackView::editTrack (juce::uint32 trackId, const juce::String& description, bool merge,
                                const std::function<void (SessionTrack&)>& change)
{
    auto next = *engine.session.getState();

    for (auto& t : next.tracks)
        if (t.id == trackId)
            change (t);

    engine.session.apply (std::move (next), description, merge);
}

void MultitrackView::deleteSelectedClip()
{
    auto next = *engine.session.getState();

    if (SessionEdits::removeClip (next, selectedClip))
    {
        selectedClip = 0;
        engine.session.apply (std::move (next), "Delete Clip");
    }
}

void MultitrackView::splitSelectedClip()
{
    auto next = *engine.session.getState();

    if (SessionEdits::splitClip (next, selectedClip, cursor))
        engine.session.apply (std::move (next), "Split Clip");
}

void MultitrackView::deleteSelectedTrack()
{
    auto next = *engine.session.getState();

    if (SessionEdits::removeTrack (next, getSelectedTrack()))
    {
        selectedTrackId = 0;
        engine.session.apply (std::move (next), "Delete Track");
    }
}

//==============================================================================
void MultitrackView::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    drag = Drag::none;
    preview.reset();

    if (p.x < headerWidth)
        return;

    if (p.y < rulerHeight)
    {
        drag = Drag::range;
        dragFrame = snapped (frameForX ((float) p.x), 0, false);
        setCursor (dragFrame);
        setTimeSelection ({});
        return;
    }

    if (const auto hit = envelopeAt (p))
    {
        const auto& track = shown().tracks[(size_t) hit->track];
        auto envelope = track.envelope (hit->parameter);
        selectTrack (hit->track);

        if (hit->point >= 0 && e.mods.isAltDown())
        {
            envelope.remove (hit->point);
            auto next = *engine.session.getState();
            SessionEdits::setEnvelope (next, track.id, hit->parameter, std::move (envelope));
            engine.session.apply (std::move (next), "Delete Envelope Point");
            return;
        }

        dragEnvelope = *hit;
        dragDescription = "Move Envelope Point";

        // A click on the line adds a point there, at the value the line has, and drags it.
        if (hit->point < 0)
        {
            const auto frame = snapped (frameForX ((float) p.x));
            dragEnvelope.point = envelope.add (frame, track.valueAt (hit->parameter, frame));
            dragDescription = "Add Envelope Point";

            auto next = *engine.session.getState();
            SessionEdits::setEnvelope (next, track.id, hit->parameter, envelope);
            preview = std::move (next);
        }

        dragEnvelopeBase = std::move (envelope);
        drag = Drag::envelopePoint;
        repaint();
        return;
    }

    if (const auto hit = clipAt (p))
    {
        const auto& clip = shown().tracks[(size_t) hit->first].clips[(size_t) hit->second];
        const auto r = clipBounds (hit->first, clip);
        selectedClip = clip.id;
        dragClip = clip;
        dragTrack = hit->first;
        dragFrame = frameForX ((float) p.x);
        drag = fadeHandleAt (p, hit->first, clip);

        if (drag == Drag::none)
            drag = p.x <= r.getX() + edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimStart
                 : p.x >= r.getRight() - edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimEnd
                 : Drag::move;

        dragDescription = drag == Drag::move ? "Move Clip" : drag == Drag::fadeIn ? "Clip Fade In"
                        : drag == Drag::fadeOut ? "Clip Fade Out" : "Trim Clip";
        selectTrack (hit->first);
        return;
    }

    selectedClip = 0;
    drag = Drag::range;
    dragFrame = snapped (frameForX ((float) p.x), 0, false);
    setCursor (dragFrame);
    setTimeSelection ({});
    selectTrack (trackAt (p.y));
}

void MultitrackView::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (drag == Drag::range)
    {
        // Dragging selects the range between where the mouse went down and where it is now; the
        // cursor follows the range's start.
        const auto frame = snapped (frameForX ((float) juce::jmax (headerWidth, p.x)), 0, false);
        const auto range = juce::Range<juce::int64>::between (dragFrame, frame);
        setCursor (range.getStart());
        setTimeSelection (range);
        return;
    }

    if (drag == Drag::none || e.mouseWasDraggedSinceMouseDown() == false)
        return;

    auto next = *engine.session.getState();
    const auto delta = frameForX ((float) p.x) - dragFrame;

    switch (drag)
    {
        case Drag::move:
        {
            const auto target = juce::jlimit (0, (int) next.tracks.size(), trackAt (juce::jmax (rulerHeight, p.y)));
            auto position = juce::jmax ((juce::int64) 0, dragClip.position + delta);

            if (snapping)
                position += SessionSnap::snapSpan (*state, { position, position + dragClip.length }, snapTolerance(),
                                                   snapTargets (dragClip.id, true));

            SessionEdits::moveClip (next, dragClip.id, target, position);
            break;
        }
        case Drag::trimStart:
            SessionEdits::trimClip (next, dragClip.id, snapped (dragClip.position + delta, dragClip.id), dragClip.getEnd());
            break;
        case Drag::trimEnd:
            SessionEdits::trimClip (next, dragClip.id, dragClip.position, snapped (dragClip.getEnd() + delta, dragClip.id));
            break;
        case Drag::fadeIn:
            SessionEdits::setClipFades (next, dragClip.id, snapped (frameForX ((float) p.x), dragClip.id) - dragClip.position, dragClip.fadeOut);
            break;
        case Drag::fadeOut:
            SessionEdits::setClipFades (next, dragClip.id, dragClip.fadeIn, dragClip.getEnd() - snapped (frameForX ((float) p.x), dragClip.id));
            break;
        case Drag::envelopePoint:
        {
            auto envelope = dragEnvelopeBase;
            envelope.move (dragEnvelope.point, snapped (frameForX ((float) p.x)),
                           valueForY (dragEnvelope.track, dragEnvelope.parameter, (float) p.y));
            SessionEdits::setEnvelope (next, next.tracks[(size_t) dragEnvelope.track].id, dragEnvelope.parameter, std::move (envelope));
            break;
        }
        case Drag::none:
        case Drag::range:
            break;
    }

    if (crossfading && (drag == Drag::move || drag == Drag::trimStart || drag == Drag::trimEnd))
        SessionEdits::crossfade (next, dragClip.id);

    preview = std::move (next);
    rebuildHeaders();
    repaint();
}

void MultitrackView::mouseUp (const juce::MouseEvent&)
{
    if (preview.has_value())
    {
        auto next = std::move (*preview);
        preview.reset();
        engine.session.apply (std::move (next), dragDescription);
    }

    drag = Drag::none;
}

void MultitrackView::mouseMove (const juce::MouseEvent& e)
{
    auto cursorShape = juce::MouseCursor::NormalCursor;
    const auto p = e.getPosition();

    if (const auto envelope = envelopeAt (p))
    {
        cursorShape = envelope->point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor;
    }
    else if (const auto hit = clipAt (p))
    {
        const auto& clip = shown().tracks[(size_t) hit->first].clips[(size_t) hit->second];
        const auto r = clipBounds (hit->first, clip);
        cursorShape = fadeHandleAt (p, hit->first, clip) != Drag::none
                          || (r.getWidth() > 3 * edgeGrab && (p.x <= r.getX() + edgeGrab || p.x >= r.getRight() - edgeGrab))
                          ? juce::MouseCursor::LeftRightResizeCursor
                          : juce::MouseCursor::DraggingHandCursor;
    }

    setMouseCursor (cursorShape);
}

void MultitrackView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (envelopeAt (p).has_value())
        return;

    if (const auto hit = clipAt (p); hit.has_value() && onOpenInEditor != nullptr)
    {
        const auto& clip = shown().tracks[(size_t) hit->first].clips[(size_t) hit->second];

        if (fadeHandleAt (p, hit->first, clip) == Drag::none)
            onOpenInEditor (clip.source);
    }
}

void MultitrackView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown())
        zoomAround (e.position.x, std::pow (2.0, (double) -wheel.deltaY * 2.0));
    else if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        setView (viewStart - (double) wheel.deltaX * timelineArea().getWidth() * framesPerPixel * 0.5, framesPerPixel);
    else
        vScroll.setCurrentRangeStart ((double) scrollY - (double) wheel.deltaY * 200.0);   // calls scrollBarMoved
}

void MultitrackView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (scaleFactor > 0.0f)
        zoomAround (e.position.x, 1.0 / (double) scaleFactor);
}

//==============================================================================
bool MultitrackView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& path : files)
        if (engine.formatManager.findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            return true;

    return false;
}

void MultitrackView::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (onFilesDropped != nullptr)
        onFilesDropped (files, trackAt (y), x < headerWidth ? 0 : frameForX ((float) x));
}
