#include "MultitrackView.h"
#include "Metronome.h"
#include "TrackColours.h"

namespace
{
    const juce::Colour background (0xff15181d);
    const juce::Colour laneColour (0xff191c22);
    const juce::Colour laneAlternate (0xff1c2027);
    const juce::Colour laneLine (0xff2a2f37);
    const juce::Colour cursorColour (0xffffd34d);
    const juce::Colour playColour (0xffffffff);
    const juce::Colour markerColour (0xffff8a3d);
    const juce::Colour regionColour (0xff58a6ff);

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
        arm.setTooltip ("Arm for recording: Record puts a take on each armed track, from its input");
        arm.onClick = [this] { view.setArmed (trackId, arm.getToggleState()); };

        inputButton.setTooltip ("The input this track records from");
        inputButton.onClick = [this] { view.showInputMenu (trackId, inputButton); };

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

        for (auto* c : std::initializer_list<juce::Component*> { &name, &effectsButton, &arm, &mute, &solo, &gain, &pan, &sendsButton, &inputButton, &automationButton })
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
        colour = TrackColours::of (t, index);
        name.setTooltip (t.note);
        group = t.group;
        isSelected = selected;
        isBus = t.isBus;
        sendsButton.setVisible (! isBus);
        arm.setVisible (! isBus);
        arm.setToggleState (view.isArmed (trackId), juce::dontSendNotification);
        inputButton.setVisible (! isBus);
        inputButton.setButtonText (t.input.isDefault() ? juce::String ("In")
                                   : t.input.channels == 2 ? juce::String (t.input.first + 1) + "/" + juce::String (t.input.first + 2)
                                                           : juce::String (t.input.first + 1));
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
        automationButton.setToggleState (t.isAutomated(), juce::dontSendNotification);
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

        if (group != 0)
        {
            g.setColour (juce::Colours::lightgrey);
            g.setFont (11.0f);
            g.drawText ("G" + juce::String (group), juce::Rectangle<int> (getWidth() - 30, getHeight() - 15, 26, 12), juce::Justification::centredRight);
        }

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
            sendsButton.setBounds (bottom.removeFromRight (56));
            bottom.removeFromRight (4);
            inputButton.setBounds (bottom.removeFromRight (40));
            bottom.removeFromRight (4);
        }

        pan.setBounds (bottom);

        // In a short lane, the rows that don't fit (or get squeezed) are left out.
        for (auto* c : std::initializer_list<juce::Component*> { &gain, &pan, &automationButton, &sendsButton, &inputButton })
            if (c->getHeight() < 20 || c->getBottom() > getHeight() - 2)
                c->setBounds ({});
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        view.selectTrack (index());

        if (e.mods.isPopupMenu())
            showGroupMenu();
    }

    void showGroupMenu()
    {
        const auto* t = view.engine.session.getState()->findTrack (trackId);

        if (t == nullptr)
            return;

        // Groups link tracks' volume, mute and solo.
        juce::PopupMenu groups;
        groups.addItem ("None", true, t->group == 0, [this] { view.setTrackGroup (trackId, 0); });

        for (int g = 1; g <= SessionTrack::maxGroups; ++g)
            groups.addItem ("Group " + juce::String (g), true, t->group == g, [this, g] { view.setTrackGroup (trackId, g); });

        juce::PopupMenu colours;
        colours.addItem ("Automatic", true, t->colour < 0, [this] { view.setTrackColour (trackId, -1); });

        for (int c = 0; c < (int) TrackColours::palette.size(); ++c)
        {
            juce::PopupMenu::Item item (TrackColours::palette[(size_t) c].name);
            item.setTicked (t->colour == c);
            item.setColour (juce::Colour (TrackColours::palette[(size_t) c].argb));
            item.setAction ([this, c] { view.setTrackColour (trackId, c); });
            colours.addItem (std::move (item));
        }

        juce::PopupMenu menu;
        menu.addSubMenu ("Group", groups);
        menu.addSubMenu ("Colour", colours);
        menu.addItem ("Note...", [this] { view.askForTrackNote (trackId); });
        menu.addItem ("Hide", [this] { view.setTrackHidden (trackId, true); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // Dragged by its background, the header moves its track: the view shows where it would go.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStartY() == 0 && view.trackDropIndex < 0)
            return;

        const auto y = e.getEventRelativeTo (&view).position.y;
        view.trackDropIndex = juce::jlimit (0, juce::jmax (0, (int) view.shown().tracks.size() - 1), view.trackAt ((int) y));
        view.repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto to = std::exchange (view.trackDropIndex, -1);
        view.repaint();

        if (to >= 0)
            view.moveTrack (index(), to);
    }

    const juce::uint32 trackId;

private:
    int index() const
    {
        const auto& tracks = view.shown().tracks;

        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == trackId)
                return (int) i;

        return -1;
    }

    MultitrackView& view;
    int group = 0;
    juce::Label name;
    juce::TextButton effectsButton { "FX" }, arm { "R" }, mute { "M" }, solo { "S" }, sendsButton { "Sends" }, inputButton { "In" }, automationButton { "A" };
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
    engine.setTrackEffectsListener ([this] (const TrackEffects::ParameterTouch& touch)
    {
        auto& player = engine.getPlayer();
        const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session;
        effectParameterTouched (touch, playing ? std::optional<juce::int64> (player.getPositionFrames()) : std::nullopt);
    });
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
    engine.setTrackEffectsListener (nullptr);
    finishAutomationPass();
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

    enum { showVolume = 1, showPan, hide, clearVolume, clearPan, clearEffect, firstEffect = 1000 };
    const auto showing = getShownEnvelope (trackId);
    const auto showingEffect = showing.has_value() && showing->parameter == AutomationParameter::effect;
    juce::PopupMenu menu;
    menu.addItem (showVolume, "Show Volume Envelope", true, showing == AutomationParameter::volume);
    menu.addItem (showPan, "Show Pan Envelope", true, showing == AutomationParameter::pan);

    // Each effect's parameters, in the plug-in's order; automated ones say so.
    std::vector<AutomationTarget> effectTargets;
    const auto& effects = engine.getTrackEffects();

    for (int slot = 0; slot < effects.getNumSlots (trackId); ++slot)
    {
        auto* plugin = effects.getPlugin (trackId, slot);
        const auto slotId = effects.getSlotId (trackId, slot);
        std::vector<std::pair<int, juce::String>> byIndex;

        for (auto& [id, parameter] : TrackEffects::parametersById (*plugin))
            byIndex.emplace_back (parameter->getParameterIndex(), id);

        std::sort (byIndex.begin(), byIndex.end());
        juce::PopupMenu parameters;

        for (auto& [index, id] : byIndex)
        {
            const auto effectTarget = AutomationTarget::effect (slotId, id);
            const auto* parameter = plugin->getParameters()[index];
            parameters.addItem (firstEffect + (int) effectTargets.size(),
                                parameter->getName (60) + (track->envelope (effectTarget).isEmpty() ? "" : " (automated)"),
                                true, showing == effectTarget);
            effectTargets.push_back (effectTarget);
        }

        menu.addSubMenu ("Show " + juce::String (slot + 1) + ". " + plugin->getName() + " Envelope", parameters, ! byIndex.empty());
    }

    menu.addItem (hide, "Hide Envelope", showing.has_value());
    menu.addSeparator();
    menu.addItem (clearVolume, "Clear Volume Envelope", ! track->volumeEnvelope.isEmpty());
    menu.addItem (clearPan, "Clear Pan Envelope", ! track->panEnvelope.isEmpty());
    menu.addItem (clearEffect, "Clear Shown Effect Envelope", showingEffect && ! track->envelope (*showing).isEmpty());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this, trackId, showing, effectTargets, safe = juce::Component::SafePointer<MultitrackView> (this)] (int result)
        {
            if (safe == nullptr)
                return;

            if (result >= firstEffect && result - firstEffect < (int) effectTargets.size())
            {
                showEnvelope (trackId, effectTargets[(size_t) (result - firstEffect)]);
                return;
            }

            switch (result)
            {
                case showVolume: showEnvelope (trackId, AutomationParameter::volume); break;
                case showPan:    showEnvelope (trackId, AutomationParameter::pan); break;
                case hide:       showEnvelope (trackId, std::nullopt); break;
                case clearEffect:
                {
                    auto next = *engine.session.getState();

                    if (showing.has_value() && SessionEdits::setEnvelope (next, trackId, *showing, {}))
                        engine.session.apply (std::move (next), "Clear Effect Envelope");

                    break;
                }
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

void MultitrackView::showClipMenu (juce::uint32 clipId)
{
    const auto where = shown().findClip (clipId);

    if (! where.has_value())
        return;

    const auto& clip = shown().tracks[(size_t) where->first].clips[(size_t) where->second];
    const juce::Component::SafePointer<MultitrackView> safe (this);
    juce::PopupMenu menu;

    if (clip.takes.size() > 1)
    {
        for (int i = 0; i < (int) clip.takes.size(); ++i)
            menu.addItem ("Take " + juce::String (i + 1), ! clip.locked, i == clip.activeTake,
                          [safe, clipId, i] { if (safe != nullptr) safe->chooseTake (clipId, i); });

        menu.addSeparator();
    }

    juce::PopupMenu colours;
    colours.addItem ("Track's", true, clip.colour < 0, [safe] { if (safe != nullptr) safe->setSelectedClipsColour (-1); });

    for (int c = 0; c < (int) TrackColours::palette.size(); ++c)
    {
        juce::PopupMenu::Item item (TrackColours::palette[(size_t) c].name);
        item.setTicked (clip.colour == c);
        item.setColour (juce::Colour (TrackColours::palette[(size_t) c].argb));
        item.setAction ([safe, c] { if (safe != nullptr) safe->setSelectedClipsColour (c); });
        colours.addItem (std::move (item));
    }

    menu.addSubMenu ("Colour", colours);
    menu.addItem ("Lock", true, areSelectedClipsLocked(), [safe] { if (safe != nullptr) safe->setSelectedClipsLocked (! safe->areSelectedClipsLocked()); });
    menu.addItem ("Mute", true, areSelectedClipsMuted(), [safe] { if (safe != nullptr) safe->setSelectedClipsMuted (! safe->areSelectedClipsMuted()); });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void MultitrackView::chooseTake (juce::uint32 clipId, int index)
{
    auto next = *engine.session.getState();

    if (SessionEdits::chooseTake (next, clipId, index))
        engine.session.apply (std::move (next), "Choose Take " + juce::String (index + 1));
}

std::optional<AutomationTarget> MultitrackView::getShownEnvelope (juce::uint32 trackId) const
{
    if (const auto found = shownEnvelopes.find (trackId); found != shownEnvelopes.end())
        return found->second;

    return std::nullopt;
}

void MultitrackView::showEnvelope (juce::uint32 trackId, std::optional<AutomationTarget> parameter)
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

    // Clips that went, or whose track was hidden, leave the selection.
    for (auto it = selectedClips.begin(); it != selectedClips.end();)
    {
        const auto where = state->findClip (*it);
        it = where.has_value() && ! state->tracks[(size_t) where->first].hidden ? std::next (it) : selectedClips.erase (it);
    }

    if (const auto* t = state->findTrack (selectedTrackId); t != nullptr && t->hidden)
        selectedTrackId = 0;

    if (const auto where = state->findClip (selectedClip); ! where.has_value() || state->tracks[(size_t) where->first].hidden)
        selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

    for (auto it = armed.begin(); it != armed.end();)
        it = state->findTrack (*it) == nullptr ? armed.erase (it) : std::next (it);

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

void MultitrackView::startAutomationPass (juce::uint32 trackId, const AutomationTarget& p, juce::int64 frame, float value)
{
    finishAutomationPass();

    const auto current = engine.session.getState();

    if (current->findTrack (trackId) == nullptr)
        return;

    pass = AutomationPass { trackId, p, *current, { { juce::jmax ((juce::int64) 0, frame), value } } };

    // An effect parameter being written is held off its envelope, and its envelope shown.
    if (p.parameter == AutomationParameter::effect)
    {
        engine.setTrackEffectTouched (trackId, p.slotId, p.parameterId, true);
        showEnvelope (trackId, p);
    }

    applyAutomationPass (true);
}

void MultitrackView::effectParameterTouched (const TrackEffects::ParameterTouch& touch, std::optional<juce::int64> playFrame)
{
    using Kind = TrackEffects::ParameterTouch::Kind;
    const auto target = AutomationTarget::effect (touch.slotId, touch.parameterId);
    const auto underWay = isAutomationPassUnderWay (touch.trackId, target);

    if (touch.kind == Kind::begin && writingAutomation && playFrame.has_value())
        startAutomationPass (touch.trackId, target, *playFrame, touch.value);
    else if (touch.kind == Kind::change && underWay && playFrame.has_value())
        continueAutomationPass (*playFrame, touch.value);
    else if (touch.kind == Kind::end && underWay)
        finishAutomationPass();
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
    if (pass.has_value() && pass->parameter.parameter == AutomationParameter::effect)
        engine.setTrackEffectTouched (pass->trackId, pass->parameter.slotId, pass->parameter.parameterId, false);

    pass.reset();
}

void MultitrackView::applyAutomationPass (bool first)
{
    auto next = pass->base;
    const auto returnFrames = (juce::int64) std::llround (returnSeconds * next.sampleRate);
    SessionEdits::writeAutomation (next, pass->trackId, pass->parameter, pass->points, returnFrames);

    // The first write is a step of its own; the rest of the pass merges into it.
    const auto kind = pass->parameter.parameter;
    engine.session.apply (std::move (next), kind == AutomationParameter::volume ? "Write Volume Automation"
                                          : kind == AutomationParameter::pan ? "Write Pan Automation" : "Write Effect Automation", ! first);
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
    const auto& tracks = shown().tracks;

    for (size_t i = 0; i < headers.size(); ++i)
    {
        const auto hidden = i < tracks.size() && tracks[i].hidden;
        headers[i]->setVisible (! hidden);
        headers[i]->setBounds (0, rowOf ((int) i) * laneHeight - scrollY, headerArea.getWidth(), hidden ? 0 : laneHeight);
    }
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

int MultitrackView::rowOf (int track) const
{
    // Past the last track, rows go on as they would for new tracks.
    const auto& tracks = shown().tracks;
    auto row = 0;

    for (int t = 0; t < juce::jmin (track, (int) tracks.size()); ++t)
        row += tracks[(size_t) t].hidden ? 0 : 1;

    return row + juce::jmax (0, track - (int) tracks.size());
}

int MultitrackView::laneTop (int track) const
{
    return rulerHeight + rowOf (track) * laneHeight - scrollY;
}

int MultitrackView::trackAt (int y) const
{
    if (y < rulerHeight)
        return -1;

    // The track shown in that row; past the shown ones, the places new tracks would take.
    const auto& tracks = shown().tracks;
    auto row = (y - rulerHeight + scrollY) / laneHeight;

    for (int t = 0; t < (int) tracks.size(); ++t)
        if (! tracks[(size_t) t].hidden && row-- == 0)
            return t;

    return (int) tracks.size() + row;
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
    return juce::Rectangle<float> (x0, (float) laneTop (track) + 3.0f, juce::jmax (2.0f, x1 - x0), (float) laneHeight - 6.0f).toNearestInt();
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

void MultitrackView::setLaneHeight (int height)
{
    const auto limited = juce::jlimit (minLaneHeight, maxLaneHeight, height);

    if (limited == laneHeight)
        return;

    laneHeight = limited;
    layoutHeaders();
    updateScrollBars();
    repaint();
}

void MultitrackView::zoomToRange (juce::Range<juce::int64> range)
{
    if (range.isEmpty())
        return;

    const auto margin = (double) range.getLength() * 0.025;
    setView ((double) range.getStart() - margin, ((double) range.getLength() + 2.0 * margin) / juce::jmax (1, timelineArea().getWidth()));
}

juce::Range<double> MultitrackView::getVisibleFrames() const
{
    return { viewStart, viewStart + framesPerPixel * timelineArea().getWidth() };
}

void MultitrackView::updateScrollBars()
{
    const auto width = (double) juce::jmax (1, timelineArea().getWidth());
    const auto length = (double) juce::jmax (shown().getLength(), (juce::int64) (shown().sampleRate * 60.0));
    hScroll.setRangeLimits (0.0, juce::jmax (length, viewStart + width * framesPerPixel), juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, width * framesPerPixel, juce::dontSendNotification);

    const auto visible = juce::jmax (1, timelineArea().getHeight());
    const auto total = (rowOf ((int) shown().tracks.size()) + 1) * laneHeight;
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

float MultitrackView::yForValue (int track, const AutomationTarget& p, float value) const
{
    const auto top = (float) laneTop (track) + envelopeMargin, bottom = (float) (laneTop (track) + laneHeight) - envelopeMargin;
    auto proportion = envelopeScale (p.parameter).convertTo0to1 (automationRange (p.parameter).clipValue (value));

    // Pan: left at the top.
    if (p.parameter == AutomationParameter::pan)
        proportion = 1.0f - proportion;

    return bottom - proportion * (bottom - top);
}

float MultitrackView::valueForY (int track, const AutomationTarget& p, float y) const
{
    const auto top = (float) laneTop (track) + envelopeMargin, bottom = (float) (laneTop (track) + laneHeight) - envelopeMargin;
    auto proportion = juce::jlimit (0.0f, 1.0f, (bottom - y) / (bottom - top));

    if (p.parameter == AutomationParameter::pan)
        proportion = 1.0f - proportion;

    return envelopeScale (p.parameter).convertFrom0to1 (proportion);
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

    const auto lineY = yForValue (t, *parameter, lineValueAt (track, *parameter, frameForX ((float) p.x)));
    return std::abs ((float) p.y - lineY) <= 5.0f ? std::optional<EnvelopeHit> (hit) : std::nullopt;
}

juce::AudioProcessorParameter* MultitrackView::findEffectParameter (juce::uint32 trackId, const AutomationTarget& target) const
{
    if (target.parameter != AutomationParameter::effect)
        return nullptr;

    auto* plugin = engine.getTrackEffects().findSlot (trackId, target.slotId);

    if (plugin == nullptr)
        return nullptr;

    const auto parameters = TrackEffects::parametersById (*plugin);
    const auto found = parameters.find (target.parameterId);
    return found != parameters.end() ? found->second : nullptr;
}

float MultitrackView::lineValueAt (const SessionTrack& track, const AutomationTarget& target, juce::int64 frame) const
{
    if (target.parameter == AutomationParameter::effect && track.envelope (target).isEmpty())
    {
        const auto* parameter = findEffectParameter (track.id, target);
        return parameter != nullptr ? parameter->getValue() : 0.0f;
    }

    return track.valueAt (target, frame);
}

void MultitrackView::drawEnvelope (juce::Graphics& g, int track, const AutomationTarget& p) const
{
    const auto& t = shown().tracks[(size_t) track];
    const auto& points = t.envelope (p).points;
    const auto area = timelineArea();
    const auto colour = p.parameter == AutomationParameter::volume ? juce::Colour (0xff7ee0ff)
                      : p.parameter == AutomationParameter::pan ? juce::Colour (0xffff9de0) : juce::Colour (0xffb8f07a);
    const auto left = (float) area.getX(), right = (float) area.getRight();
    juce::String label = p.parameter == AutomationParameter::volume ? "Volume" : "Pan (left at the top)";

    if (p.parameter == AutomationParameter::effect)
    {
        const auto* plugin = engine.getTrackEffects().findSlot (t.id, p.slotId);
        const auto* parameter = findEffectParameter (t.id, p);
        label = plugin == nullptr || parameter == nullptr ? juce::String ("An effect no longer on this track")
              : plugin->getName() + ": " + parameter->getName (40) + " "
                    + parameter->getText (lineValueAt (t, p, controlFrame()), 40) + parameter->getLabel();
    }

    g.setColour (colour.withAlpha (0.8f));
    g.setFont (11.0f);
    g.drawText (label + (points.empty() ? ": click the line to add a point" : ""),
                juce::Rectangle<float> (left + 6.0f, (float) laneTop (track) + (float) laneHeight - 20.0f, 400.0f, 16.0f),
                juce::Justification::centredLeft, false);

    if (points.empty())
    {
        const auto y = yForValue (track, p, lineValueAt (t, p, 0));
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

    if (showBars)
        targets.gridBeats = rulerBeats().second;
    else
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

std::pair<int, int> MultitrackView::rulerBeats() const
{
    // Labels at least 50 pixels apart: every beat, or every 1, 2, 4... bars. Marks (and the snap
    // grid) every beat while beats are 8 pixels apart, else on the labels.
    const auto& s = shown();
    const auto beatPixels = 60.0 * s.sampleRate / s.tempo / framesPerPixel;
    const auto perBar = juce::jmax (1, s.beatsPerBar);
    auto label = 1;

    if (beatPixels < 50.0)
        for (label = perBar; label * beatPixels < 50.0 && label < (1 << 24);)
            label *= 2;

    return { label, beatPixels >= 8.0 ? 1 : label };
}

void MultitrackView::drawBarsRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto& s = shown();
    const auto perBar = juce::jmax (1, s.beatsPerBar);
    const auto [labelBeats, markBeats] = rulerBeats();
    const auto first = juce::jmax ((juce::int64) 0, Metronome::beatAt (frameForX ((float) area.getX()), s.tempo, s.sampleRate));
    const auto last = Metronome::beatAt (frameForX ((float) area.getRight()), s.tempo, s.sampleRate);
    g.setFont (11.0f);

    for (auto beat = first / markBeats * markBeats; beat <= last; beat += markBeats)
    {
        const auto x = xForFrame (Metronome::beatFrame (beat, s.tempo, s.sampleRate));
        const auto labelled = beat % labelBeats == 0;
        g.setColour (juce::Colours::grey);
        g.drawVerticalLine ((int) x, (float) area.getBottom() - (labelled ? 8.0f : 4.0f), (float) area.getBottom());

        if (! labelled)
            continue;

        // Bars count from 1, and a beat within a bar from 1: "5" is bar 5, "5.3" its third beat.
        const auto bar = beat / perBar + 1, inBar = beat % perBar + 1;
        g.setColour (juce::Colours::lightgrey);
        g.drawText (inBar == 1 ? juce::String (bar) : juce::String (bar) + "." + juce::String (inBar),
                    juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 60.0f, (float) area.getHeight() - 4.0f),
                    juce::Justification::centredLeft, false);
    }
}

void MultitrackView::drawRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (juce::Colour (0xff1b1e23));
    g.fillRect (area);

    if (showBars)
    {
        drawBarsRuler (g, area);
        return;
    }

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
    const auto colour = c.muted ? juce::Colour (0xff6e7681) : TrackColours::of (c, shown().tracks[(size_t) track], track);
    const auto selected = isClipSelected (c.id);

    g.setColour (colour.withAlpha (0.18f));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);

    auto body = r.reduced (1);
    auto title = body.removeFromTop (16);
    g.setColour (colour.withAlpha (0.45f));
    g.fillRect (title);
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);
    auto name = c.takes.size() > 1 ? c.name + "  (take " + juce::String (c.activeTake + 1) + " of " + juce::String ((int) c.takes.size()) + ")"
                                    : c.name;

    if (! juce::exactlyEqual (c.gainDb, 0.0f))
        name << "  " << (c.gainDb > 0.0f ? "+" : "") << juce::String (c.gainDb, 1) << " dB";

    if (c.muted)
        name << "  (muted)";
    g.drawText (name, title.withTrimmedLeft (12).withTrimmedRight (c.locked ? 24 : 10), juce::Justification::centredLeft, true);   // clear of the fade handles

    if (c.locked && title.getWidth() > 40)
    {
        // A padlock at the right of the title bar.
        const auto box = title.removeFromRight (20).withSizeKeepingCentre (8, 10).toFloat();
        const auto lockBody = box.withTrimmedTop (4.0f);
        juce::Path shackle;
        shackle.addCentredArc (box.getCentreX(), lockBody.getY(), 2.6f, 3.2f, 0.0f, -juce::MathConstants<float>::halfPi,
                               juce::MathConstants<float>::halfPi, true);
        g.strokePath (shackle, juce::PathStrokeType (1.3f));
        g.fillRect (lockBody);
    }

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

    if (c.note.isNotEmpty() && body.getHeight() > 40)
    {
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (11.0f);
        g.drawText (c.note, body.removeFromBottom (14).withTrimmedLeft (6).withTrimmedRight (6), juce::Justification::centredLeft, true);
    }

    if (c.fadeIn > 0)
        drawFade ((float) r.getX(), xForFrame (c.position + c.fadeIn));

    if (c.fadeOut > 0)
        drawFade ((float) r.getRight(), xForFrame (c.getEnd() - c.fadeOut));

    if (r.getWidth() > 3 * edgeGrab && ! c.locked)
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
            if (s.tracks[(size_t) t].hidden)
                continue;

            const auto lane = juce::Rectangle<int> (area.getX(), laneTop (t), area.getWidth(), laneHeight);
            g.setColour (s.tracks[(size_t) t].id == selectedTrackId ? juce::Colour (0xff20262e) : (rowOf (t) % 2 == 0 ? laneColour : laneAlternate));
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

        // In bars and beats, the grid of the session's tempo.
        if (showBars && ! s.tracks.empty())
        {
            const auto bottom = (float) juce::jmin (area.getBottom(), laneTop ((int) s.tracks.size()));
            const auto markBeats = rulerBeats().second;
            const auto first = juce::jmax ((juce::int64) 0, Metronome::beatAt (frameForX ((float) area.getX()), s.tempo, s.sampleRate));
            const auto last = Metronome::beatAt (frameForX ((float) area.getRight()), s.tempo, s.sampleRate);

            for (auto beat = first / markBeats * markBeats; beat <= last; beat += markBeats)
            {
                g.setColour (juce::Colours::white.withAlpha (beat % juce::jmax (1, s.beatsPerBar) == 0 ? 0.12f : 0.05f));
                g.drawVerticalLine ((int) xForFrame (Metronome::beatFrame (beat, s.tempo, s.sampleRate)), (float) area.getY(), bottom);
            }
        }

        if (! timeSelection.isEmpty())
        {
            const auto x0 = xForFrame (timeSelection.getStart()), x1 = xForFrame (timeSelection.getEnd());
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()));
        }

        if (! recording.isEmpty())
        {
            std::vector<int> lanes;

            for (int t = 0; t < (int) s.tracks.size(); ++t)
                if (isArmed (s.tracks[(size_t) t].id) && ! s.tracks[(size_t) t].hidden)
                    lanes.push_back (t);

            if (lanes.empty())
                lanes.push_back ((int) s.tracks.size());

            const auto x0 = xForFrame (recording.getStart()), x1 = xForFrame (recording.getEnd());

            for (auto lane : lanes)
            {
                const auto r = juce::Rectangle<float> (x0, (float) laneTop (lane) + 2.0f, juce::jmax (1.0f, x1 - x0), (float) laneHeight - 4.0f);
                g.setColour (juce::Colour (0xffd0453a).withAlpha (0.3f));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (juce::Colour (0xffd0453a));
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
            }
        }

        // A track being moved: a line where it would land, above or below the track there.
        if (trackDropIndex >= 0)
        {
            const auto from = getSelectedTrack();
            const auto y = trackDropIndex > from ? laneTop (trackDropIndex + 1) : laneTop (trackDropIndex);
            g.setColour (cursorColour);
            g.fillRect (0, y - 1, getWidth(), 3);
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

    drawRuler (g, { area.getX(), 0, area.getWidth(), timeRulerHeight });
    drawMarkers (g, area);

    if (! timeSelection.isEmpty())
    {
        const auto x0 = juce::jmax ((float) area.getX(), xForFrame (timeSelection.getStart()));
        const auto x1 = juce::jmin ((float) area.getRight(), xForFrame (timeSelection.getEnd()));
        g.setColour (cursorColour.withAlpha (0.35f));

        if (x1 > x0)
            g.fillRect (juce::Rectangle<float> (x0, (float) timeRulerHeight - 5.0f, x1 - x0, 5.0f));
    }
    g.setColour (juce::Colour (0xff1b1e23));
    g.fillRect (0, 0, headerWidth, rulerHeight);
    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    g.drawText (juce::String (s.sampleRate / 1000.0, 1) + " kHz session", juce::Rectangle<int> (8, 0, headerWidth - 8, timeRulerHeight),
                juce::Justification::centredLeft);
}

//==============================================================================
int MultitrackView::markerAt (juce::Point<int> p) const
{
    if (p.x < headerWidth || p.y < timeRulerHeight || p.y >= rulerHeight)
        return -1;

    const auto& markers = shown().markers;
    int best = -1;
    auto bestDistance = 7.0f;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto d = std::abs (xForFrame (markers[i].start) - (float) p.x);

        if (! markers[i].isRegion() && d < bestDistance)
        {
            best = (int) i;
            bestDistance = d;
        }
    }

    if (best >= 0)
        return best;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];

        if (m.isRegion() && (float) p.x >= xForFrame (m.start) - 3.0f && (float) p.x <= xForFrame (m.getEnd()) + 3.0f
            && (best < 0 || m.length < markers[(size_t) best].length))
            best = (int) i;
    }

    return best;
}

void MultitrackView::drawMarkers (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto lane = juce::Rectangle<int> (area.getX(), timeRulerHeight, area.getWidth(), markerLaneHeight).toFloat();
    g.setColour (juce::Colour (0xff181b20));
    g.fillRect (lane);

    g.saveState();
    g.reduceClipRegion (area.getX(), 0, area.getWidth(), area.getBottom());
    g.setFont (11.0f);
    const auto& markers = shown().markers;
    const auto top = lane.getBottom(), bottom = (float) area.getBottom();

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];
        const auto x1 = xForFrame (m.start);
        const auto active = drag == Drag::marker && (int) i == dragMarker;

        if (m.isRegion())
        {
            const auto x2 = xForFrame (m.getEnd());

            if (x2 < (float) area.getX() || x1 > (float) area.getRight())
                continue;

            const auto bar = juce::Rectangle<float> (x1, lane.getY() + 2.0f, juce::jmax (2.0f, x2 - x1), lane.getHeight() - 4.0f);
            g.setColour (regionColour.withAlpha (active ? 0.75f : 0.45f));
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (regionColour.withAlpha (0.45f));
            g.drawVerticalLine ((int) x1, top, bottom);
            g.drawVerticalLine ((int) x2, top, bottom);
            g.setColour (juce::Colours::white);
            g.drawText (m.name, bar.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            if (x1 < (float) area.getX() - 100.0f || x1 > (float) area.getRight())
                continue;

            juce::Path flag;
            flag.addTriangle (x1, lane.getY() + 2.0f, x1 + 7.0f, lane.getCentreY(), x1, lane.getBottom() - 2.0f);
            g.setColour (active ? markerColour.brighter (0.4f) : markerColour);
            g.fillPath (flag);
            g.setColour (markerColour.withAlpha (0.55f));
            g.drawVerticalLine ((int) x1, top, bottom);
            g.setColour (juce::Colours::white);
            g.drawText (m.name, juce::Rectangle<float> (x1 + 9.0f, lane.getY(), 120.0f, lane.getHeight()), juce::Justification::centredLeft, true);
        }
    }

    g.restoreState();
}

void MultitrackView::addMarker()
{
    auto next = *engine.session.getState();
    auto& player = engine.getPlayer();
    Marker m;

    // While the session plays (or records), what is playing; otherwise the time range or the cursor.
    if (player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session)
        m.start = juce::jmax ((juce::int64) 0, player.getPositionFrames());
    else if (! timeSelection.isEmpty())
        m = { timeSelection.getStart(), timeSelection.getLength(), {} };
    else
        m.start = cursor;

    m.name = MarkerEdits::nextName (next.markers, m.isRegion());
    next.markers.push_back (m);
    MarkerEdits::sort (next.markers);
    engine.session.apply (std::move (next), "Add " + m.name);
}

bool MultitrackView::goToMarker (bool forwards)
{
    const auto target = MarkerEdits::nextStop (engine.session.getState()->markers, cursor, forwards);

    if (target)
        setCursor (*target);

    return target.has_value();
}

void MultitrackView::renameMarker (int index)
{
    const auto& markers = engine.session.getState()->markers;

    if (! juce::isPositiveAndBelow (index, (int) markers.size()))
        return;

    const auto old = markers[(size_t) index];
    auto* box = new juce::AlertWindow ("Rename", {}, juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("name", old.name);
    box->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, old] (int result)
    {
        const auto name = box->getTextEditorContents ("name").trim();

        if (result != 1 || name.isEmpty() || name == old.name)
            return;

        // Found again by value: the markers may have changed while the box was open.
        auto next = *engine.session.getState();

        if (auto it = std::find (next.markers.begin(), next.markers.end(), old); it != next.markers.end())
        {
            it->name = name;
            engine.session.apply (std::move (next), "Rename " + old.name);
        }
    }), true);
}

void MultitrackView::askForTrackNote (juce::uint32 trackId)
{
    const auto* track = shown().findTrack (trackId);

    if (track == nullptr)
        return;

    auto* box = new juce::AlertWindow ("Note for " + track->name, {}, juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("note", track->note);
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, trackId] (int result)
    {
        if (result == 1)
            setTrackNote (trackId, box->getTextEditorContents ("note"));
    }), true);
}

void MultitrackView::setTrackNote (juce::uint32 trackId, const juce::String& note)
{
    const auto* track = engine.session.getState()->findTrack (trackId);

    if (track != nullptr && track->note != note.trim())
        editTrack (trackId, "Track Note", false, [text = note.trim()] (SessionTrack& t) { t.note = text; });
}

void MultitrackView::showMarkerMenu (int index)
{
    const auto old = engine.session.getState()->markers[(size_t) index];
    juce::PopupMenu menu;
    menu.addItem ("Rename...", [this, index] { renameMarker (index); });
    menu.addItem ("Delete " + old.name, [this, old]
    {
        auto next = *engine.session.getState();

        if (auto it = std::find (next.markers.begin(), next.markers.end(), old); it != next.markers.end())
        {
            next.markers.erase (it);
            engine.session.apply (std::move (next), "Delete " + old.name);
        }
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
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

std::vector<juce::uint32> MultitrackView::getArmedTracks() const
{
    std::vector<juce::uint32> ids;

    for (auto& t : shown().tracks)
        if (armed.count (t.id) > 0)
            ids.push_back (t.id);

    return ids;
}

bool MultitrackView::isArmed (juce::uint32 trackId) const
{
    return armed.count (trackId) > 0;
}

void MultitrackView::setArmed (juce::uint32 trackId, bool on)
{
    const auto* t = shown().findTrack (trackId);

    if (on && t != nullptr && ! t->isBus)
        armed.insert (trackId);
    else
        armed.erase (trackId);

    rebuildHeaders();
    repaint();
}

void MultitrackView::setTrackInput (juce::uint32 trackId, TrackInput input)
{
    editTrack (trackId, "Track Input", false, [input] (SessionTrack& t) { t.input = input.isDefault() ? TrackInput() : input; });
}

void MultitrackView::showInputMenu (juce::uint32 trackId, juce::Component& target)
{
    const auto* t = shown().findTrack (trackId);

    if (t == nullptr)
        return;

    constexpr int stereoBase = 1000, monoBase = 2000;
    const auto names = engine.getDeviceInputChannelNames();
    const auto current = t->input;
    juce::PopupMenu menu;
    menu.addItem (1, "Recording Input (chosen at the bottom of the window)", true, current.isDefault());
    menu.addSeparator();

    for (int ch = 0; ch + 1 < names.size(); ch += 2)
        menu.addItem (stereoBase + ch, names[ch] + " / " + names[ch + 1], true, current == TrackInput { ch, 2 });

    menu.addSeparator();

    for (int ch = 0; ch < names.size(); ++ch)
        menu.addItem (monoBase + ch, names[ch] + " (mono)", true, current == TrackInput { ch, 1 });

    if (names.isEmpty())
        menu.addItem (-1, "No inputs on this device", false, false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this, trackId, safe = juce::Component::SafePointer<MultitrackView> (this)] (int result)
        {
            if (safe == nullptr || result <= 0)
                return;

            setTrackInput (trackId, result >= monoBase ? TrackInput { result - monoBase, 1 }
                                    : result >= stereoBase ? TrackInput { result - stereoBase, 2 } : TrackInput());
        });
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
    {
        if (t.id == trackId)
        {
            const auto before = t;
            change (t);
            SessionEdits::followGroup (next, before);
            break;
        }
    }

    engine.session.apply (std::move (next), description, merge);
}

std::vector<juce::uint32> MultitrackView::getSelectedClips() const
{
    std::vector<juce::uint32> ids;

    for (auto& t : shown().tracks)
        for (auto& c : t.clips)
            if (isClipSelected (c.id))
                ids.push_back (c.id);

    return ids;
}

void MultitrackView::selectClip (juce::uint32 clipId, bool adding)
{
    if (! adding)
    {
        selectedClips.clear();
        selectedClip = clipId;
    }
    else if (selectedClips.count (clipId) > 0)
    {
        selectedClips.erase (clipId);

        if (selectedClip == clipId)
            selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

        repaint();
        return;
    }
    else
    {
        selectedClip = clipId;
    }

    if (clipId != 0)
        selectedClips.insert (clipId);

    repaint();
}

void MultitrackView::selectAllClips()
{
    selectClipsWhere ([] (const SessionTrack&, const SessionClip&) { return true; });
}

bool MultitrackView::selectClipsOnTrack()
{
    const auto* track = engine.session.getState()->findTrack (selectedTrackId);

    if (track == nullptr)
        return false;

    selectClipsWhere ([id = track->id] (const SessionTrack& t, const SessionClip&) { return t.id == id; });
    return true;
}

void MultitrackView::selectClipsAfterCursor()
{
    selectClipsWhere ([from = cursor] (const SessionTrack&, const SessionClip& c) { return c.position >= from; });
}

void MultitrackView::selectClipsWhere (const std::function<bool (const SessionTrack&, const SessionClip&)>& wanted)
{
    // Never clips on hidden tracks, which can't be seen.
    selectedClips.clear();

    for (auto& t : engine.session.getState()->tracks)
        for (auto& c : t.clips)
            if (! t.hidden && wanted (t, c))
                selectedClips.insert (c.id);

    if (! isClipSelected (selectedClip))
        selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void MultitrackView::deleteSelectedClip()
{
    auto next = *engine.session.getState();
    const auto ids = getSelectedClips();
    auto removed = 0;

    for (auto id : ids)
        removed += SessionEdits::removeClip (next, id) ? 1 : 0;

    if (removed > 0)
    {
        selectClip (0, false);
        engine.session.apply (std::move (next), removed > 1 ? "Delete " + juce::String (removed) + " Clips" : juce::String ("Delete Clip"));
    }
}

bool MultitrackView::copySelectedClip()
{
    const auto current = engine.session.getState();
    std::vector<std::pair<int, SessionClip>> copied;
    auto topmost = std::numeric_limits<int>::max();

    for (auto id : getSelectedClips())
    {
        const auto where = current->findClip (id);
        copied.emplace_back (where->first, current->tracks[(size_t) where->first].clips[(size_t) where->second]);
        topmost = juce::jmin (topmost, where->first);
    }

    if (copied.empty())
        return false;

    for (auto& [track, clip] : copied)
        track -= topmost;

    copiedClips = std::move (copied);
    copiedRate = current->sampleRate;
    return true;
}

bool MultitrackView::cutSelectedClip()
{
    if (! copySelectedClip())
        return false;

    // Locked clips are copied but stay where they are.
    auto next = *engine.session.getState();
    auto removed = 0;

    for (auto& copied : copiedClips)
        removed += SessionEdits::removeClip (next, copied.second.id) ? 1 : 0;

    if (removed > 0)
    {
        selectClip (0, false);
        engine.session.apply (std::move (next), removed > 1 ? "Cut " + juce::String (removed) + " Clips" : juce::String ("Cut Clip"));
    }

    return true;
}

bool MultitrackView::canPaste() const
{
    return ! copiedClips.empty() && juce::exactlyEqual (copiedRate, engine.session.getState()->sampleRate);
}

namespace
{
    /** Places clips, keeping their places relative to each other: each moves by shift in time,
        onto the track firstTrack plus its own offset, or onto new tracks past the last (clips in
        the order of their offsets). Returns the new ids. */
    std::vector<juce::uint32> placeClips (SessionState& s, const std::vector<std::pair<int, SessionClip>>& clips,
                                          int firstTrack, juce::int64 shift)
    {
        std::vector<juce::uint32> ids;

        for (auto& [offset, clip] : clips)
        {
            // Past the last track (or onto a bus), placeClip makes a new one.
            ids.push_back (SessionEdits::placeClip (s, firstTrack + offset, clip, clip.position + shift));
        }

        return ids;
    }
}

void MultitrackView::pasteClip()
{
    if (! canPaste())
        return;

    auto earliest = copiedClips.front().second.position;

    for (auto& c : copiedClips)
        earliest = juce::jmin (earliest, c.second.position);

    auto next = *engine.session.getState();
    auto firstTrack = getSelectedTrack();

    if (juce::isPositiveAndBelow (firstTrack, (int) next.tracks.size()) && next.tracks[(size_t) firstTrack].isBus)
        firstTrack = -1;

    // Without a selected track all of them go below the last, still a track apart.
    if (firstTrack < 0)
        firstTrack = (int) next.tracks.size();

    const auto ids = placeClips (next, copiedClips, firstTrack, cursor - earliest);
    auto end = cursor;

    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        end = juce::jmax (end, next.tracks[(size_t) where->first].clips[(size_t) where->second].getEnd());
    }

    selectedClips = { ids.begin(), ids.end() };
    selectedClip = ids.front();
    selectedTrackId = next.tracks[(size_t) next.findClip (ids.front())->first].id;
    engine.session.apply (std::move (next), ids.size() > 1 ? "Paste " + juce::String ((int) ids.size()) + " Clips" : juce::String ("Paste Clip"));
    setCursor (end);
}

bool MultitrackView::repeatSelectedClip()
{
    auto next = *engine.session.getState();
    const auto copies = SessionEdits::repeatClip (next, selectedClip, timeSelection.getEnd());

    if (copies == 0)
        return false;

    engine.session.apply (std::move (next), "Repeat Clip (" + juce::String (copies) + (copies == 1 ? " copy)" : " copies)"));
    return true;
}

void MultitrackView::duplicateSelectedClip()
{
    auto next = *engine.session.getState();
    std::vector<std::pair<int, SessionClip>> clips;
    juce::int64 start = std::numeric_limits<juce::int64>::max(), end = 0;

    for (auto id : getSelectedClips())
    {
        const auto where = next.findClip (id);
        const auto& c = next.tracks[(size_t) where->first].clips[(size_t) where->second];
        clips.emplace_back (where->first, c);
        start = juce::jmin (start, c.position);
        end = juce::jmax (end, c.getEnd());
    }

    if (clips.empty())
        return;

    const auto ids = placeClips (next, clips, 0, end - start);
    selectedClips = { ids.begin(), ids.end() };
    selectedClip = ids.front();
    engine.session.apply (std::move (next), ids.size() > 1 ? "Duplicate " + juce::String ((int) ids.size()) + " Clips" : juce::String ("Duplicate Clip"));
}

void MultitrackView::splitSelectedClip()
{
    auto next = *engine.session.getState();

    if (SessionEdits::splitClip (next, selectedClip, cursor))
        engine.session.apply (std::move (next), "Split Clip");
}

namespace
{
    bool allSelected (const SessionState& s, const std::vector<juce::uint32>& ids, bool SessionClip::* flag)
    {
        return ! ids.empty() && std::all_of (ids.begin(), ids.end(), [&s, flag] (auto id)
        {
            const auto where = s.findClip (id);
            return s.tracks[(size_t) where->first].clips[(size_t) where->second].*flag;
        });
    }
}

bool MultitrackView::areSelectedClipsLocked() const   { return allSelected (shown(), getSelectedClips(), &SessionClip::locked); }
bool MultitrackView::areSelectedClipsMuted() const    { return allSelected (shown(), getSelectedClips(), &SessionClip::muted); }

void MultitrackView::setSelectedClipsLocked (bool locked)
{
    setSelectedClipsFlag (SessionEdits::setClipLocked, locked, locked ? "Lock " : "Unlock ");
}

void MultitrackView::setSelectedClipsMuted (bool muted)
{
    setSelectedClipsFlag (SessionEdits::setClipMuted, muted, muted ? "Mute " : "Unmute ");
}

bool MultitrackView::showClip (juce::uint32 clipId)
{
    const auto where = shown().findClip (clipId);

    if (! where.has_value())
        return false;

    const auto& clip = shown().tracks[(size_t) where->first].clips[(size_t) where->second];
    selectClip (clipId, false);
    selectTrack (where->first);
    setCursor (clip.position);

    // Horizontally, a tenth of the way in unless it starts in view already; vertically, its lane
    // in view.
    const auto width = (double) juce::jmax (1, timelineArea().getWidth()) * framesPerPixel;

    if ((double) clip.position < viewStart || (double) clip.position >= viewStart + width)
        setView ((double) clip.position - width * 0.1, framesPerPixel);

    const auto visible = juce::jmax (1, timelineArea().getHeight());
    if (shown().tracks[(size_t) where->first].hidden)
    {
        setTrackHidden (shown().tracks[(size_t) where->first].id, false);
        changeListenerCallback (&engine.session);   // lay the lanes out with it now
    }

    const auto top = rowOf (where->first) * laneHeight;

    if (top < scrollY || top + laneHeight > scrollY + visible)
        scrollY = top;

    updateScrollBars();
    repaint();
    return true;
}

void MultitrackView::setSelectedClipsColour (int colour)
{
    auto next = *engine.session.getState();
    auto changed = false;

    for (auto id : getSelectedClips())
        changed = SessionEdits::setClipColour (next, id, colour) || changed;

    if (changed)
        engine.session.apply (std::move (next), "Clip Colour");
}

void MultitrackView::setSelectedClipsFlag (bool (*set) (SessionState&, juce::uint32, bool), bool on, const juce::String& verb)
{
    auto next = *engine.session.getState();
    auto changed = 0;

    for (auto id : getSelectedClips())
        changed += set (next, id, on) ? 1 : 0;

    if (changed > 0)
        engine.session.apply (std::move (next), verb + (changed > 1 ? juce::String (changed) + " Clips" : juce::String ("Clip")));
}

void MultitrackView::setTrackHidden (juce::uint32 trackId, bool hidden)
{
    editTrack (trackId, hidden ? "Hide Track" : "Show Track", false, [hidden] (SessionTrack& t) { t.hidden = hidden; });
}

bool MultitrackView::showAllTracks()
{
    auto next = *engine.session.getState();
    auto any = false;

    for (auto& t : next.tracks)
        any = std::exchange (t.hidden, false) || any;

    if (any)
        engine.session.apply (std::move (next), "Show All Tracks");

    return any;
}

bool MultitrackView::closeGapsOnSelectedTrack()
{
    auto next = *engine.session.getState();

    if (SessionEdits::closeGaps (next, getSelectedTrack()) == 0)
        return false;

    engine.session.apply (std::move (next), "Close Gaps");
    return true;
}

juce::uint32 MultitrackView::duplicateSelectedTrack()
{
    auto next = *engine.session.getState();
    const auto id = SessionEdits::duplicateTrack (next, getSelectedTrack());

    if (id != 0)
    {
        selectedTrackId = id;
        engine.session.apply (std::move (next), "Duplicate Track");
    }

    return id;
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

juce::int64 MultitrackView::gridStepFrom (juce::int64 frame, bool later) const
{
    const auto& s = shown();

    if (showBars)
    {
        // Beats a mark apart; the beat starting at or before the frame is where to count from.
        const auto step = (juce::int64) rulerBeats().second;
        const auto beat = Metronome::beatAt (frame, s.tempo, s.sampleRate);
        auto mark = (beat >= 0 ? beat : beat - step + 1) / step * step;

        if (later)
            mark += step;
        else if (Metronome::beatFrame (mark, s.tempo, s.sampleRate) >= frame)
            mark -= step;

        return juce::jmax ((juce::int64) 0, Metronome::beatFrame (mark, s.tempo, s.sampleRate));
    }

    const auto step = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (rulerTick() * s.sampleRate));
    const auto below = frame >= 0 ? frame / step * step : -((-frame + step - 1) / step) * step;

    if (later)
        return below + step;

    return juce::jmax ((juce::int64) 0, below < frame ? below : below - step);
}

void MultitrackView::nudge (bool later)
{
    const auto ids = getSelectedClips();

    if (ids.empty())
    {
        setCursor (gridStepFrom (cursor, later));
        return;
    }

    const auto current = engine.session.getState();
    const auto& s = *current;
    const auto primary = s.findClip (isClipSelected (selectedClip) ? selectedClip : ids.front());
    const auto start = s.tracks[(size_t) primary->first].clips[(size_t) primary->second].position;
    moveSelectedClips (gridStepFrom (start, later) - start, ids.size() > 1 ? "Nudge Clips" : "Nudge Clip", true);
}

bool MultitrackView::moveSelectedClipsToCursor()
{
    const auto ids = getSelectedClips();
    const auto current = engine.session.getState();
    const auto& s = *current;
    auto earliest = std::numeric_limits<juce::int64>::max();

    for (auto id : ids)
    {
        const auto where = s.findClip (id);
        earliest = juce::jmin (earliest, s.tracks[(size_t) where->first].clips[(size_t) where->second].position);
    }

    return ! ids.empty() && moveSelectedClips (cursor - earliest, ids.size() > 1 ? "Move Clips to Cursor" : "Move Clip to Cursor", false);
}

bool MultitrackView::moveSelectedClips (juce::int64 delta, const juce::String& description, bool merge)
{
    const auto ids = getSelectedClips();
    auto next = *engine.session.getState();
    auto earliest = std::numeric_limits<juce::int64>::max();

    // The clips move together or not at all, and none before the session's start.
    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        const auto& c = next.tracks[(size_t) where->first].clips[(size_t) where->second];

        if (c.locked)
            return false;

        earliest = juce::jmin (earliest, c.position);
    }

    delta = juce::jmax (delta, -earliest);

    if (ids.empty() || delta == 0)
        return false;

    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        SessionEdits::moveClip (next, id, where->first, next.tracks[(size_t) where->first].clips[(size_t) where->second].position + delta);
    }

    engine.session.apply (std::move (next), description, merge);
    return true;
}

void MultitrackView::setTrackColour (juce::uint32 trackId, int colour)
{
    editTrack (trackId, "Track Colour", false,
               [colour] (SessionTrack& t) { t.colour = juce::isPositiveAndBelow (colour, (int) TrackColours::palette.size()) ? colour : -1; });
}

void MultitrackView::setTrackGroup (juce::uint32 trackId, int group)
{
    editTrack (trackId, group == 0 ? juce::String ("Ungroup Track") : "Group " + juce::String (group), false,
               [group] (SessionTrack& t) { t.group = juce::jlimit (0, SessionTrack::maxGroups, group); });
}

void MultitrackView::moveTrack (int from, int to)
{
    auto next = *engine.session.getState();

    if (! SessionEdits::moveTrack (next, from, to))
        return;

    selectedTrackId = next.tracks[(size_t) to].id;
    engine.session.apply (std::move (next), "Move Track");
}

//==============================================================================
void MultitrackView::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    drag = Drag::none;
    preview.reset();

    if (p.x < headerWidth)
        return;

    if (p.y >= timeRulerHeight && p.y < rulerHeight)
    {
        if (const auto index = markerAt (p); index >= 0)
        {
            const auto m = shown().markers[(size_t) index];

            if (e.mods.isPopupMenu())
            {
                showMarkerMenu (index);
                return;
            }

            if (m.isRegion())
                setTimeSelection ({ m.start, m.getEnd() });
            else
                setTimeSelection ({});

            setCursor (m.start);
            drag = Drag::marker;
            dragMarker = index;
            dragMarkerGrab = frameForX ((float) p.x) - m.start;
            dragDescription = "Move " + m.name;
            return;
        }
    }

    if (p.y < rulerHeight)
    {
        drag = Drag::range;
        dragFrame = snapped (frameForX ((float) p.x), 0, false);
        setCursor (dragFrame);
        setTimeSelection ({});
        return;
    }

    // A right-click on a clip opens its menu: the take it plays, its colour, locking and muting.
    // It keeps a selection the clip is in, so the menu works on all of them.
    if (const auto hit = clipAt (p); hit.has_value() && e.mods.isPopupMenu())
    {
        const auto& clip = shown().tracks[(size_t) hit->first].clips[(size_t) hit->second];

        if (! isClipSelected (clip.id))
            selectClip (clip.id, false);

        selectedClip = clip.id;
        selectTrack (hit->first);
        showClipMenu (clip.id);
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
            dragEnvelope.point = envelope.add (frame, lineValueAt (track, hit->parameter, frame));
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

        if (e.mods.isShiftDown())
        {
            // Shift-click adds the clip to the selection or takes it out; no drag.
            selectClip (clip.id, true);
            selectTrack (hit->first);
            return;
        }

        // A click on a selected clip keeps the selection, to drag it all.
        if (! isClipSelected (clip.id))
            selectClip (clip.id, false);

        selectedClip = clip.id;
        dragClip = clip;
        dragGroup.clear();

        for (auto id : selectedClips)
            if (const auto where = shown().findClip (id); id != clip.id && where.has_value())
                dragGroup.push_back (shown().tracks[(size_t) where->first].clips[(size_t) where->second]);

        dragTrack = hit->first;
        dragFrame = frameForX ((float) p.x);
        drag = e.mods.isCommandDown() ? Drag::clipGain : fadeHandleAt (p, hit->first, clip);

        if (drag == Drag::none)
            drag = p.x <= r.getX() + edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimStart
                 : p.x >= r.getRight() - edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimEnd
                 : Drag::move;

        // A locked clip is only selected; so is a group being moved with one in it.
        if (clip.locked || (drag == Drag::move && std::any_of (dragGroup.begin(), dragGroup.end(), [] (auto& c) { return c.locked; })))
            drag = Drag::none;

        dragDescription = drag == Drag::move ? "Move Clip" : drag == Drag::clipGain ? "Clip Gain" : drag == Drag::fadeIn ? "Clip Fade In"
                        : drag == Drag::fadeOut ? "Clip Fade Out" : "Trim Clip";
        selectTrack (hit->first);
        return;
    }

    selectClip (0, false);
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
            // With other clips selected, they all move in time by the same amount and keep their tracks.
            auto earliest = dragClip.position;

            for (auto& c : dragGroup)
                earliest = juce::jmin (earliest, c.position);

            const auto target = dragGroup.empty() ? juce::jlimit (0, (int) next.tracks.size(), trackAt (juce::jmax (rulerHeight, p.y)))
                                                  : dragTrack;
            auto position = dragClip.position + juce::jmax (delta, -earliest);

            if (snapping)
            {
                auto targets = snapTargets (dragClip.id, true);

                for (auto& c : dragGroup)
                    targets.ignoreClips.push_back (c.id);

                position += SessionSnap::snapSpan (*state, { position, position + dragClip.length }, snapTolerance(), targets);
                position = juce::jmax (position, dragClip.position - earliest);
            }

            SessionEdits::moveClip (next, dragClip.id, target, position);

            for (auto& c : dragGroup)
                if (const auto where = next.findClip (c.id))
                    SessionEdits::moveClip (next, c.id, where->first, c.position + position - dragClip.position);

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
        case Drag::clipGain:
        {
            // Up is louder, in steps of 0.1 dB.
            const auto change = (float) (e.getMouseDownY() - p.y) * clipGainPerPixel;
            SessionEdits::setClipGain (next, dragClip.id, std::round ((dragClip.gainDb + change) * 10.0f) / 10.0f);
            break;
        }
        case Drag::marker:
        {
            if (! juce::isPositiveAndBelow (dragMarker, (int) next.markers.size()))
                break;

            auto& m = next.markers[(size_t) dragMarker];
            auto start = juce::jmax ((juce::int64) 0, frameForX ((float) p.x) - dragMarkerGrab);

            if (snapping)
            {
                // Either edge of a region may snap; against the session as it was, without this marker.
                auto targets = snapTargets (0, false);
                targets.ignoreMarker = dragMarker;
                start = juce::jmax ((juce::int64) 0, start + SessionSnap::snapSpan (*state, { start, start + m.length }, snapTolerance(), targets));
            }

            m.start = start;
            setCursor (start);
            setTimeSelection (m.isRegion() ? juce::Range<juce::int64> (start, m.getEnd()) : juce::Range<juce::int64>());
            break;
        }
        case Drag::none:
        case Drag::range:
            break;
    }

    if (crossfading && (drag == Drag::move || drag == Drag::trimStart || drag == Drag::trimEnd))
    {
        SessionEdits::crossfade (next, dragClip.id);

        if (drag == Drag::move)
            for (auto& c : dragGroup)
                SessionEdits::crossfade (next, c.id);
    }

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
        MarkerEdits::sort (next.markers);
        engine.session.apply (std::move (next), dragDescription);
    }

    drag = Drag::none;
}

void MultitrackView::mouseMove (const juce::MouseEvent& e)
{
    auto cursorShape = juce::MouseCursor::NormalCursor;
    const auto p = e.getPosition();

    if (markerAt (p) >= 0)
    {
        cursorShape = juce::MouseCursor::DraggingHandCursor;
    }
    else if (const auto envelope = envelopeAt (p))
    {
        cursorShape = envelope->point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor;
    }
    else if (const auto hit = clipAt (p))
    {
        const auto& clip = shown().tracks[(size_t) hit->first].clips[(size_t) hit->second];
        const auto r = clipBounds (hit->first, clip);
        cursorShape = clip.locked ? juce::MouseCursor::NormalCursor
                    : e.mods.isCommandDown() ? juce::MouseCursor::UpDownResizeCursor
                    : fadeHandleAt (p, hit->first, clip) != Drag::none
                          || (r.getWidth() > 3 * edgeGrab && (p.x <= r.getX() + edgeGrab || p.x >= r.getRight() - edgeGrab))
                          ? juce::MouseCursor::LeftRightResizeCursor
                          : juce::MouseCursor::DraggingHandCursor;
    }

    setMouseCursor (cursorShape);
}

void MultitrackView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (const auto index = markerAt (p); index >= 0)
    {
        renameMarker (index);
        return;
    }

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
