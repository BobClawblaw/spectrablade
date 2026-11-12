#include "MultitrackView.h"
#include "Instrument.h"
#include "OpenFiles.h"
#include "Theme.h"
#include "TimeFormat.h"
#include "Surround.h"
#include "TimeStretch.h"
#include "PluginMenu.h"
#include <map>
#include "Metronome.h"
#include "TrackColours.h"

namespace
{
    // The theme's colours, read as they are when painting.
    const juce::Colour& background = Theme::canvas;
    const juce::Colour& laneColour = Theme::lane;
    const juce::Colour& laneAlternate = Theme::laneAlternate;
    const juce::Colour frozenColour (0xff9fd8ff);
    const juce::Colour& laneLine = Theme::line;
    const juce::Colour& cursorColour = Theme::cursor;
    const juce::Colour& playColour = Theme::textStrong;

    constexpr int edgeGrab = 6;   // pixels from a clip's edge that trim instead of move

    /** A raised-cosine fade's curve, from silence at silentX (on the bottom) to full level at
        fullX (on the top). */
    juce::Path fadeCurve (FadeShape shape, float silentX, float fullX, float top, float bottom)
    {
        juce::Path curve;

        for (int i = 0; i <= 32; ++i)
        {
            const auto t = (float) i / 32.0f;
            const auto level = FadeShapes::level (shape, i, 32);
            const auto point = juce::Point<float> (silentX + (fullX - silentX) * t, bottom - level * (bottom - top));

            if (i == 0)
                curve.startNewSubPath (point);
            else
                curve.lineTo (point);
        }

        return curve;
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
        Theme::follow (arm, juce::TextButton::buttonOnColourId, Theme::danger);
        arm.setTooltip ("Arm for recording: Record puts a take on each armed track, from its input");
        arm.onClick = [this] { view.setArmed (trackId, arm.getToggleState()); };

        inputButton.setTooltip ("The input this track records from");
        inputButton.onClick = [this] { view.showInputMenu (trackId, inputButton); };

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            Theme::follow (*b, juce::TextButton::buttonOnColourId, b == &mute ? Theme::danger : Theme::warn);
        }

        Theme::follow (sendsButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
        sendsButton.onClick = [this] { view.showSends (trackId, sendsButton); };

        effectsButton.setTooltip ("This track's effects");
        Theme::follow (effectsButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
        effectsButton.onClick = [this] { view.showEffectsMenu (trackId, effectsButton); };

        instrumentButton.setTooltip ("This track's instrument: the plug-in its note clips and live MIDI play, before its effects");
        Theme::follow (instrumentButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
        instrumentButton.onClick = [this] { view.showInstrumentMenu (trackId, instrumentButton); };

        automationButton.setTooltip ("Volume and pan envelopes");
        Theme::follow (automationButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
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
            Theme::follow (*s, juce::Slider::trackColourId, Theme::lineStrong);
            s->onDragEnd = [this]
            {
                view.finishAutomationPass();
                view.rebuildHeaders();   // an automated control that wrote nothing goes back to its envelope
            };
        }

        for (auto* c : std::initializer_list<juce::Component*> { &name, &effectsButton, &instrumentButton, &arm, &mute, &solo, &gain, &pan, &sendsButton, &inputButton, &automationButton })
            addAndMakeVisible (c);
    }

    /** A control moved. While automation is being written and the session plays, it records
        into the envelope; an automated control does nothing otherwise. False to edit the setting. */
    bool controlChanged (AutomationParameter p, float value)
    {
        // A folder's volume isn't automated: it is always the setting.
        if (isFolder)
            return false;

        auto& player = view.engine.getPlayer();
        const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session;

        if (view.isAutomationPassUnderWay (trackId, p))
        {
            view.continueAutomationPass (player.getHeardFrame(), value);
            return true;
        }

        if (view.isWritingAutomation() && playing)
        {
            view.startAutomationPass (trackId, p, player.getHeardFrame(), value);
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
        isFolder = t.isFolder;
        isInstrument = t.isInstrument;
        collapsed = t.collapsed;
        inFolder = t.folderId != 0;
        members = (int) std::count_if (view.shown().tracks.begin(), view.shown().tracks.end(), [&t] (auto& m) { return m.folderId == t.id; });

        // A folder has only its name, mute, solo and volume.
        for (auto* c : std::initializer_list<juce::Component*> { &effectsButton, &pan, &automationButton })
            c->setVisible (! isFolder);

        sendsButton.setVisible (! isFolder);
        // An instrument track plays its instrument, so it has no input to record or monitor; an
        // audio track has no instrument.
        arm.setVisible (! isBus && ! isFolder && ! isInstrument);
        arm.setToggleState (view.isArmed (trackId), juce::dontSendNotification);
        inputButton.setVisible (! isBus && ! isFolder && ! isInstrument);
        instrumentButton.setVisible (isInstrument);
        inputButton.setButtonText (t.input.isDefault() ? juce::String ("In")
                                   : t.input.channels == 2 ? juce::String (t.input.first + 1) + "/" + juce::String (t.input.first + 2)
                                                           : juce::String (t.input.first + 1));
        sendsButton.setButtonText (isBus ? juce::String ("Output") : t.sends.empty() ? juce::String ("Sends") : "Sends " + juce::String ((int) t.sends.size()));
        sendsButton.setToggleState (! t.sends.empty() || t.outputBusId != 0 || t.deviceOutput >= 0 || t.ducking.isOn(), juce::dontSendNotification);
        sendsButton.setTooltip (isBus ? "Play this bus into the mix, into another bus, or out of a pair of the interface's outputs"
                                      : "Send this track to buses, play it into one or out of a pair of the interface's outputs in place of the mix, "
                                        "or duck it under another track");
        effectsButton.setTooltip (isBus ? "This bus's effects" : "This track's effects");
        resized();

        if (! name.isBeingEdited())
            name.setText (t.name, juce::dontSendNotification);

        mute.setToggleState (t.mute, juce::dontSendNotification);
        solo.setToggleState (t.solo, juce::dontSendNotification);
        solo.setTooltip (t.soloSafe ? "Solo: play only soloed tracks (this one is solo safe: it plays anyway)" : "Solo: play only soloed tracks");

        if (std::exchange (soloSafe, t.soloSafe) != t.soloSafe)
            repaint();

        if (std::exchange (inverted, t.invertPolarity) != t.invertPolarity)
        {
            name.setTooltip (inverted ? "Polarity inverted" : juce::String());
            resized();
            repaint();
        }

        const auto& effects = view.engine.getTrackEffects();
        const auto* instrument = effects.getInstrument (trackId);
        const auto slots = effects.getNumSlots (trackId) - (instrument != nullptr ? 1 : 0);   // the effects, the instrument apart
        effectsButton.setButtonText (slots > 0 ? "FX " + juce::String (slots) : juce::String ("FX"));
        instrumentButton.setButtonText (instrument != nullptr ? instrument->getName() : juce::String ("Inst"));
        instrumentButton.setToggleState (instrument != nullptr, juce::dontSendNotification);
        auto anyOn = false;

        for (int i = 0; i < slots; ++i)
            anyOn = anyOn || ! effects.isBypassed (trackId, i);

        effectsButton.setToggleState (anyOn, juce::dontSendNotification);

        // An automated volume or pan follows its envelope; the control shows its value at the cursor.
        const auto volumeAutomated = ! t.volumeEnvelope.isEmpty(), panAutomated = ! t.panEnvelope.isEmpty();
        automationButton.setToggleState (t.isAutomated(), juce::dontSendNotification);
        const auto writing = view.isWritingAutomation();
        gain.setEnabled (! volumeAutomated || writing);
        pan.setEnabled ((! panAutomated || writing) && ! t.isWide());
        const juce::String automatedTip = writing ? " follows its envelope; drag while playing to write it" : " follows its envelope (A to edit it)";
        gain.setTooltip (isFolder ? "Folder volume: added to the volume of each track and bus in it"
                         : volumeAutomated ? "Volume" + automatedTip : writing ? "Track volume; drag while playing to write automation" : "Track volume");
        pan.setTooltip (t.isWide() ? juce::String (t.sevenOne ? "A 7.1" : "A 5.1") + " track plays its channels to their own speakers: it has no pan"
                        : panAutomated ? "Pan" + automatedTip : writing ? "Pan; drag while playing to write automation" : "Pan");

        if (t.fiveOne)
            effectsButton.setTooltip ("This 5.1 track's effects: each runs on L R, C LFE and Ls Rs as three stereo copies; the built-in compressor and limiter hear all six at once");
        else if (t.sevenOne)
            effectsButton.setTooltip ("This 7.1 track's effects: each runs on L R, C LFE, Lrs Rrs and Lss Rss as four stereo copies; the built-in compressor and limiter hear all eight at once");

        const auto frame = view.controlFrame();

        if (! gain.isMouseButtonDown())
            gain.setValue (t.valueAt (AutomationParameter::volume, frame), juce::dontSendNotification);

        if (! pan.isMouseButtonDown())
            pan.setValue (std::round (t.valueAt (AutomationParameter::pan, frame) * 100.0f), juce::dontSendNotification);

        repaint();
    }

    /** Moves the meter on to these peaks (input: from the track's input while armed). */
    void pushLevels (float left, float right, bool input)
    {
        // Falls by about 4 dB a tick at 30 a second, as the mixer's meters do. The highest peak
        // holds for 1.5 s, then falls the same way.
        constexpr float fall = 0.63f;
        constexpr int holdTicks = 45;
        const auto before = std::tuple (levels[0], levels[1], holds[0], holds[1], clipped);

        // Switching between the input and the mix starts the holds afresh; a clip stays lit.
        if (showingInput != input)
            holds[0] = holds[1] = 0.0f;

        const float peaks[2] { left, right };

        for (int ch = 0; ch < 2; ++ch)
        {
            levels[ch] = juce::jmax (peaks[ch], levels[ch] * fall);

            if (peaks[ch] >= holds[ch])
            {
                holds[ch] = peaks[ch];
                holdLeft[ch] = holdTicks;
            }
            else if (holdLeft[ch] > 0)
            {
                --holdLeft[ch];
            }
            else
            {
                holds[ch] = juce::jmax (levels[ch], holds[ch] * fall);
            }

            clipped = clipped || peaks[ch] >= 1.0f;
        }

        if (levels[0] < 0.001f && levels[1] < 0.001f)
            levels[0] = levels[1] = 0.0f;

        for (auto& h : holds)
            if (h < 0.001f)
                h = 0.0f;

        if (before != std::tuple (levels[0], levels[1], holds[0], holds[1], clipped) || showingInput != input)
        {
            showingInput = input;
            repaint (meterArea());
        }
    }

    float getLevel (int channel) const noexcept     { return levels[juce::jlimit (0, 1, channel)]; }
    float getHold (int channel) const noexcept      { return holds[juce::jlimit (0, 1, channel)]; }
    bool isShowingInput() const noexcept            { return showingInput; }
    bool hasClipped() const noexcept                { return clipped; }

    void clearClip()
    {
        if (std::exchange (clipped, false))
            repaint (meterArea());
    }

    juce::Rectangle<int> meterArea() const          { return { getWidth() - 9, 6, 6, getHeight() - 13 }; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (isSelected ? Theme::hover : (isBus ? Theme::base : isFolder ? Theme::raised : Theme::surface));

        // Two bars, left and right, -60 dB at the bottom to +6 dB at the top: red-tinted while
        // they show the input, each with a line at its held peak. A red light on top once
        // either has reached 0 dB, until clicked.
        if (! isFolder)
        {
            auto area = meterArea().toFloat();
            g.setColour (clipped ? Theme::danger : Theme::line);
            g.fillRect (area.removeFromTop (3.0f).withTrimmedRight (1.0f));
            area.removeFromTop (1.0f);
            g.setColour (Theme::inset);
            g.fillRect (area);

            const auto heightOf = [&area] (float level)
            {
                return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (level, -60.0f) + 60.0f) / 66.0f) * area.getHeight();
            };

            const auto barColour = showingInput ? juce::Colour (0xffe0705a) : Theme::waveform;

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto x = area.getX() + (float) ch * 3.0f;
                const auto height = heightOf (levels[ch]);
                g.setColour (barColour);
                g.fillRect (juce::Rectangle<float> (x, area.getBottom() - height, 2.0f, height));

                if (holds[ch] > 0.0f)
                {
                    g.setColour (holds[ch] >= 1.0f ? Theme::danger : barColour.brighter (0.5f));
                    g.fillRect (juce::Rectangle<float> (x, area.getBottom() - juce::jmax (1.0f, heightOf (holds[ch])), 2.0f, 1.0f));
                }
            }
        }
        g.setColour (colour);
        g.fillRect (inFolder ? folderIndent : 0, 0, isBus ? 8 : 4, getHeight());

        // A member of a folder: a rule down its left, inset under the folder's triangle.
        if (inFolder)
        {
            g.setColour (Theme::overlay.withAlpha (0.15f));
            g.fillRect (0, 0, 3, getHeight());
        }

        if (isFolder)
        {
            const auto box = triangleArea().toFloat().withSizeKeepingCentre (10.0f, 10.0f);
            juce::Path triangle;

            if (collapsed)
                triangle.addTriangle (box.getTopLeft(), box.getBottomLeft(), { box.getRight(), box.getCentreY() });
            else
                triangle.addTriangle (box.getTopLeft(), box.getTopRight(), { box.getCentreX(), box.getBottom() });

            g.setColour (Theme::text);
            g.fillPath (triangle);
            g.setFont (12.0f);
            g.setColour (Theme::dimText);
            g.drawText (juce::String (members) + (members == 1 ? " track" : " tracks") + (collapsed ? ", collapsed" : ""),
                        getLocalBounds().withTrimmedLeft (32).withTrimmedTop (32).withHeight (20), juce::Justification::centredLeft);
        }

        if (group != 0)
        {
            g.setColour (Theme::text);
            g.setFont (11.0f);
            g.drawText ("G" + juce::String (group), juce::Rectangle<int> (getWidth() - 30, getHeight() - 15, 26, 12), juce::Justification::centredRight);
        }

        if (inverted && ! isFolder)
        {
            g.setColour (Theme::warn);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc3\x98")), name.getX() - 14, name.getY(), 14, name.getHeight(), juce::Justification::centred);
        }

        // Solo safe: a bar under the solo button.
        if (soloSafe && solo.isVisible() && ! solo.getBounds().isEmpty())
        {
            g.setColour (Theme::warn);
            g.fillRect (solo.getBounds().getX() + 3, solo.getBounds().getBottom() + 1, solo.getWidth() - 6, 2);
        }

        g.setColour (laneLine);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (10 + (inFolder ? folderIndent : 0) + (isFolder ? 18 : 0)).withTrimmedRight (isFolder ? 0 : 8).reduced (4, 6);
        auto top = r.removeFromTop (24);
        solo.setBounds (top.removeFromRight (26));
        top.removeFromRight (4);
        mute.setBounds (top.removeFromRight (26));
        top.removeFromRight (4);

        if (isFolder)
        {
            name.setBounds (top);
            r.removeFromTop (26);   // the count of its tracks
            gain.setBounds (r.removeFromTop (22));

            if (gain.getBottom() > getHeight() - 2)
                gain.setBounds ({});

            return;
        }

        if (! isBus && ! isInstrument)
        {
            arm.setBounds (top.removeFromRight (26));
            top.removeFromRight (4);
        }

        effectsButton.setBounds (top.removeFromRight (40));

        if (instrumentButton.isVisible())
            instrumentButton.setBounds (top.removeFromRight (juce::jmin (90, juce::jmax (44, top.getWidth() / 3))).withTrimmedRight (2));

        // Inverted polarity: a Ø before the name.
        if (inverted)
            top.removeFromLeft (14);

        name.setBounds (top);
        r.removeFromTop (8);
        gain.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        auto bottom = r.removeFromTop (22);

        automationButton.setBounds (bottom.removeFromRight (26));
        bottom.removeFromRight (4);
        sendsButton.setBounds (bottom.removeFromRight (56));
        bottom.removeFromRight (4);

        if (! isBus && ! isInstrument)
        {
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

        if (! isFolder && ! e.mods.isPopupMenu() && meterArea().expanded (2).contains (e.getPosition()))
            clearClip();
        else if (e.mods.isPopupMenu())
            showGroupMenu();
        else if (isFolder && triangleArea().contains (e.getPosition()))
            view.setFolderCollapsed (trackId, ! collapsed);
    }

    /** Where a folder's collapse triangle is. */
    juce::Rectangle<int> triangleArea() const    { return { 8, 6, 20, 24 }; }

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

        // Folders hold tracks and buses, not other folders.
        if (! t->isFolder)
        {
            juce::PopupMenu folders;
            folders.addItem ("None", true, t->folderId == 0, [this] { view.setTrackFolder (trackId, 0); });

            for (auto& f : view.engine.session.getState()->tracks)
                if (f.isFolder)
                    folders.addItem (f.name, true, t->folderId == f.id, [this, id = f.id] { view.setTrackFolder (trackId, id); });

            menu.addSubMenu ("Folder", folders);
        }
        else
        {
            menu.addItem (t->collapsed ? "Expand" : "Collapse", [this, c = t->collapsed] { view.setFolderCollapsed (trackId, ! c); });
        }

        menu.addSubMenu ("Group", groups);
        menu.addItem ("Solo Safe", true, t->soloSafe, [this, safe = t->soloSafe] { view.setTrackSoloSafe (trackId, ! safe); });

        if (! t->isFolder)
            menu.addItem ("Invert Polarity", true, t->invertPolarity, [this, on = t->invertPolarity] { view.setTrackPolarityInverted (trackId, ! on); });

        if (! t->isFolder && ! t->isBus)
        {
            // An instrument track holds note clips alone, an audio track audio clips alone: one
            // with no clips of its own kind can become the other kind.
            const auto canTurn = ! t->isFrozen() && ! t->isWide() && (t->isInstrument ? t->noteClips.empty() : t->clips.empty());
            menu.addItem ("Instrument Track", canTurn, t->isInstrument, [this, on = t->isInstrument] { view.setTrackKind (trackId, ! on); });
            menu.addItem ("Musical Timebase", true, t->musical, [this, on = t->musical] { view.setTrackMusical (trackId, ! on); });

            if (t->isInstrument)
            {
                menu.addSubMenu ("Instrument", view.instrumentMenuFor (trackId), ! t->isFrozen());
                menu.addItem ("Add Notes...", ! t->isFrozen() && view.onEditNotes != nullptr, false, [this] { view.onEditNotes (trackId, 0); });
                menu.addItem ("Add Trigger", ! t->isFrozen(), false, [this] { view.addTrigger (trackId); });
            }
            else
            {
                menu.addItem ("5.1 Track", ! t->isFrozen(), t->fiveOne, [this, on = t->fiveOne] { view.setTrackChannels (trackId, on ? 2 : 6); });
                menu.addItem ("7.1 Track", ! t->isFrozen(), t->sevenOne, [this, on = t->sevenOne] { view.setTrackChannels (trackId, on ? 2 : 8); });
            }
        }

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
            view.dropTrack (index(), to);
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
    juce::TextButton effectsButton { "FX" }, instrumentButton { "Inst" }, arm { "R" }, mute { "M" }, solo { "S" }, sendsButton { "Sends" }, inputButton { "In" }, automationButton { "A" };
    juce::Slider gain, pan;
    juce::Colour colour;
    bool isSelected = false, isBus = false, isFolder = false, isInstrument = false, collapsed = false, inFolder = false, soloSafe = false, inverted = false;
    int members = 0;
    float levels[2] {}, holds[2] {};
    int holdLeft[2] {};
    bool showingInput = false, clipped = false;
    static constexpr int folderIndent = 8;
};

//==============================================================================
/** A track's routing: its output (the mix or a bus), ducking under another track, one row per
    bus for its sends (on/off, level, before or after the track's volume), and a button that
    makes a new bus with a send to it. A bus's has only its output: the mix, or another bus that
    doesn't play into it. Follows the session, so undo shows here too. */
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

        output.onChange = [this]
        {
            const auto index = output.getSelectedId() - 1;

            if (! juce::isPositiveAndBelow (index, (int) outputChoices.size()))
                return;

            const auto [busId, pair] = outputChoices[(size_t) index];
            auto next = *engine.session.getState();

            if (pair >= 0 ? SessionEdits::setTrackDeviceOutput (next, trackId, pair) : SessionEdits::setTrackOutput (next, trackId, busId))
                engine.session.apply (std::move (next), "Track Output");
        };
        output.setTooltip ("Where it plays: the mix, a bus in its place, or a pair of the audio interface's outputs (past the rack, and left out of mixdowns)");
        addAndMakeVisible (outputLabel);
        addAndMakeVisible (output);
        outputLabel.setText ("Output", juce::dontSendNotification);

        duckKey.setTooltip ("Bring this track down while another track is heard (music under a voice, say)");
        duckKey.onChange = [this] { applyDucking ("Duck Under", false); };
        duckLabel.setText ("Duck under", juce::dontSendNotification);
        addAndMakeVisible (duckLabel);
        addAndMakeVisible (duckKey);

        const auto setUp = [this] (juce::Slider& s, double lo, double hi, double step, double mid, const juce::String& suffix, const juce::String& tip)
        {
            s.setSliderStyle (juce::Slider::LinearBar);
            s.setRange (lo, hi, step);
            s.setSkewFactorFromMidPoint (mid);
            s.setTextValueSuffix (suffix);
            s.setTooltip (tip);
            Theme::follow (s, juce::Slider::trackColourId, Theme::lineStrong);
            s.onValueChange = [this] { applyDucking ("Ducking", true); };
            addAndMakeVisible (s);
        };

        setUp (duckAmount, -60.0, -1.0, 0.5, -12.0, " dB down", "How far the track comes down");
        setUp (duckThreshold, -80.0, 0.0, 0.5, -40.0, " dB key", "How loud the other track must be to bring it down");
        setUp (duckAttack, 1.0, 500.0, 1.0, 20.0, " ms attack", "How quickly it comes down");
        setUp (duckRelease, 10.0, 5000.0, 10.0, 400.0, " ms release", "How slowly it goes back up");
        addAndMakeVisible (newBus);
        addChildComponent (empty);
        empty.setText ("No buses yet.", juce::dontSendNotification);
        Theme::follow (empty, juce::Label::textColourId, Theme::dimText);
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
        auto top = r.removeFromTop (rowHeight).reduced (0, 2);
        outputLabel.setBounds (top.removeFromLeft (80));
        output.setBounds (top);
        auto duck = r.removeFromTop (rowHeight).reduced (0, 2);
        duckLabel.setBounds (duck.removeFromLeft (80));
        duckKey.setBounds (duck);

        for (auto pair : { std::pair { &duckAmount, &duckThreshold }, std::pair { &duckAttack, &duckRelease } })
        {
            auto line = r.removeFromTop (rowHeight).reduced (0, 2);
            pair.first->setBounds (line.removeFromLeft (line.getWidth() / 2 - 2));
            line.removeFromLeft (4);
            pair.second->setBounds (line);
        }

        r.removeFromTop (6);

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
    juce::Label outputLabel, duckLabel;
    juce::ComboBox output, duckKey;
    juce::Slider duckAmount, duckThreshold, duckAttack, duckRelease;
    std::vector<std::pair<juce::uint32, int>> outputChoices;   // the output list's items: a bus (0 for the mix) or an interface pair
    std::vector<juce::uint32> keyChoices;      // the tracks in the duck-under list, after None

    void applyDucking (const juce::String& description, bool merge)
    {
        const auto index = duckKey.getSelectedItemIndex();
        const Ducking d { index <= 0 ? 0u : keyChoices[(size_t) index - 1], (float) duckAmount.getValue(), (float) duckThreshold.getValue(),
                          (float) duckAttack.getValue(), (float) duckRelease.getValue() };
        auto next = *engine.session.getState();

        if (SessionEdits::setDucking (next, trackId, d))
            engine.session.apply (std::move (next), description, merge);
        else
            refresh();   // refused (a loop): show what it is
    }

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

        output.clear (juce::dontSendNotification);
        outputChoices.assign (1, { 0u, -1 });
        output.addItem ("Mix", 1);

        // A bus can't play into itself, nor into a bus that plays into it.
        for (auto* bus : buses)
        {
            if (track != nullptr && track->isBus && (bus->id == trackId || current->feeds (*bus, trackId)))
                continue;

            outputChoices.push_back ({ bus->id, -1 });
            output.addItem (bus->name, (int) outputChoices.size());
        }

        // The interface's output pairs, past the rack; one the session names that this interface
        // doesn't have is listed too, so it shows.
        const auto names = engine.getDeviceOutputChannelNames();
        const auto pairs = juce::jmin (names.size() / 2, SessionTrack::maxDeviceOutputs / 2);
        const auto chosen = track != nullptr && track->outputBusId == 0 ? track->deviceOutput : -1;
        output.addSectionHeading ("Interface outputs");

        for (int p = 0; p < pairs; ++p)
        {
            outputChoices.push_back ({ 0u, 2 * p });
            output.addItem (names[2 * p] + " / " + names[2 * p + 1], (int) outputChoices.size());
        }

        if (chosen >= 2 * pairs)
        {
            outputChoices.push_back ({ 0u, chosen });
            output.addItem ("Out " + juce::String (chosen + 1) + "/" + juce::String (chosen + 2) + " (not on this interface)", (int) outputChoices.size());
        }

        const auto now = std::pair<juce::uint32, int> { track != nullptr ? track->outputBusId : 0u, track != nullptr && track->outputBusId == 0 ? track->deviceOutput : -1 };
        const auto out = std::find (outputChoices.begin(), outputChoices.end(), now);
        output.setSelectedId (out == outputChoices.end() ? 1 : (int) (out - outputChoices.begin()) + 1, juce::dontSendNotification);

        if (track != nullptr && track->isBus)
        {
            for (auto* c : std::initializer_list<juce::Component*> { &duckLabel, &duckKey, &duckAmount, &duckThreshold, &duckAttack, &duckRelease, &newBus, &empty })
                c->setVisible (false);

            rows.clear();
            setSize (300, 16 + rowHeight);
            return;
        }

        duckKey.clear (juce::dontSendNotification);
        duckKey.addItem ("None", 1);
        keyChoices.clear();

        for (auto& t : current->tracks)
        {
            if (! t.isBus && ! t.isFolder && t.id != trackId)
            {
                keyChoices.push_back (t.id);
                duckKey.addItem (t.name, (int) keyChoices.size() + 1);
            }
        }

        const auto ducking = track != nullptr ? track->ducking : Ducking();
        const auto key = std::find (keyChoices.begin(), keyChoices.end(), ducking.keyTrackId);
        duckKey.setSelectedItemIndex (key == keyChoices.end() ? 0 : (int) (key - keyChoices.begin()) + 1, juce::dontSendNotification);
        duckAmount.setValue (ducking.amountDb, juce::dontSendNotification);
        duckThreshold.setValue (ducking.thresholdDb, juce::dontSendNotification);
        duckAttack.setValue (ducking.attackMs, juce::dontSendNotification);
        duckRelease.setValue (ducking.releaseMs, juce::dontSendNotification);

        for (auto* s : { &duckAmount, &duckThreshold, &duckAttack, &duckRelease })
            s->setEnabled (ducking.isOn());

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
                Theme::follow (row->level, juce::Slider::trackColourId, Theme::lineStrong);
                row->pre.setClickingTogglesState (true);
                row->pre.setTooltip ("Send before the track's volume");
                Theme::follow (row->pre, juce::TextButton::buttonOnColourId, Theme::accentDeep);

                // Turning a level or Pre on a send that is off turns it on.
                row->on.onClick = [this, row] { apply (*row, row->on.getToggleState() ? "Add Send" : "Remove Send", false); };
                row->level.onValueChange = [this, row] { row->on.setToggleState (true, juce::dontSendNotification); apply (*row, "Send Level", true); };
                row->pre.onClick = [this, row] { row->on.setToggleState (true, juce::dontSendNotification); apply (*row, "Send Before or After Volume", false); };

                for (auto* c : std::initializer_list<juce::Component*> { &row->on, &row->level, &row->pre })
                    addAndMakeVisible (c);
            }

            empty.setVisible (rows.isEmpty());
            setSize (360, 16 + 4 * rowHeight + 6 + juce::jmax (1, rows.size()) * rowHeight + 30);
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
        effectParameterTouched (touch, playing ? std::optional<juce::int64> (player.getHeardFrame()) : std::nullopt);
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

namespace
{
    bool isSurroundParameter (AutomationParameter p)
    {
        return std::find (surroundParameters.begin(), surroundParameters.end(), p) != surroundParameters.end();
    }

    /** A surround envelope's name in the menu and on its lane, saying which way is up. */
    juce::String surroundEnvelopeName (AutomationParameter p)
    {
        return p == AutomationParameter::surroundX ? "Left-Right (left at the top)"
             : p == AutomationParameter::surroundY ? "Front-Back (front at the top)"
             : p == AutomationParameter::surroundWidth ? "Width" : "LFE (off at the bottom)";
    }
}

void MultitrackView::showAutomationMenu (juce::uint32 trackId, juce::Component& target)
{
    const auto* track = shown().findTrack (trackId);

    if (track == nullptr)
        return;

    enum { showVolume = 1, showPan, hide, clearVolume, clearPan, clearEffect, clearSurround, firstSurround = 100, firstEffect = 1000 };
    const auto showing = getShownEnvelope (trackId);
    const auto showingEffect = showing.has_value() && showing->parameter == AutomationParameter::effect;
    const auto showingSurround = showing.has_value() && isSurroundParameter (showing->parameter);
    juce::PopupMenu menu;
    menu.addItem (showVolume, "Show Volume Envelope", true, showing == AutomationParameter::volume);
    menu.addItem (showPan, "Show Pan Envelope", true, showing == AutomationParameter::pan);

    // Where it plays in a surround master (or kept from one): a 5.1 or 7.1 track has no surround pan.
    if ((shown().surround || track->hasSurroundAutomation()) && ! track->isFolder && ! track->isWide())
    {
        juce::PopupMenu surround;

        for (size_t i = 0; i < surroundParameters.size(); ++i)
            surround.addItem (firstSurround + (int) i,
                              surroundEnvelopeName (surroundParameters[i]) + (track->surroundEnvelopes[i].isEmpty() ? "" : " (automated)"),
                              true, showing == surroundParameters[i]);

        menu.addSubMenu ("Show Surround Envelope", surround);
    }

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

    if (track->hasSurroundAutomation() || showingSurround)
        menu.addItem (clearSurround, "Clear Shown Surround Envelope", showingSurround && ! track->envelope (*showing).isEmpty());

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

            if (result >= firstSurround && result - firstSurround < (int) surroundParameters.size())
            {
                showEnvelope (trackId, surroundParameters[(size_t) (result - firstSurround)]);
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
                case clearSurround:
                {
                    auto next = *engine.session.getState();

                    if (showing.has_value() && SessionEdits::setEnvelope (next, trackId, *showing, {}))
                        engine.session.apply (std::move (next), "Clear Surround Envelope");

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

    for (auto fadeOut : { false, true })
    {
        juce::PopupMenu shapes;
        const auto current = fadeOut ? clip.fadeOutShape : clip.fadeInShape;

        for (auto shape : FadeShapes::all)
            shapes.addItem (FadeShapes::displayName (shape), ! clip.locked, shape == current,
                            [safe, fadeOut, shape] { if (safe != nullptr) safe->setSelectedClipsFadeShape (fadeOut, shape); });

        menu.addSubMenu (fadeOut ? "Fade Out Shape" : "Fade In Shape", shapes);
    }

    menu.addItem ("Lock", true, areSelectedClipsLocked(), [safe] { if (safe != nullptr) safe->setSelectedClipsLocked (! safe->areSelectedClipsLocked()); });
    menu.addItem ("Mute", true, areSelectedClipsMuted(), [safe] { if (safe != nullptr) safe->setSelectedClipsMuted (! safe->areSelectedClipsMuted()); });
    menu.addSeparator();
    menu.addItem ("Group", canGroupSelectedClips(), false, [safe] { if (safe != nullptr) safe->groupSelectedClips(); });
    menu.addItem ("Ungroup", areSelectedClipsGrouped(), false, [safe] { if (safe != nullptr) safe->ungroupSelectedClips(); });
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

    // Effects only: the instrument has its own button and menu (showInstrumentMenu).
    juce::Array<juce::PluginDescription> types;
    const auto* track = state->findTrack (trackId);

    for (auto& type : engine.knownPlugins.getTypes())
        if (! type.isInstrument)
            types.add (type);

    // Plug-in choices take ids from 1 (PluginMenu); each slot's actions sit above them.
    // Sidechain choices: sidechainBase + slot * keyChoices + 0 for none, or 1 + the key track's place.
    enum { slotBase = 1000000, editSlot = 0, bypassSlot, moveSlotUp, moveSlotDown, removeSlot, actions };
    enum { sidechainBase = 2000000, keyChoices = 10000 };
    juce::PopupMenu menu;
    const auto first = effects.getInstrument (trackId) != nullptr ? 1 : 0;   // the instrument's slot is not an effect

    for (int i = first; i < slots; ++i)
    {
        const auto* plugin = effects.getPlugin (trackId, i);
        const auto bypassed = effects.isBypassed (trackId, i);
        const auto id = slotBase + i * actions;
        const auto keyId = track != nullptr ? track->sidechainKey (effects.getSlotId (trackId, i)) : 0u;
        const auto* key = state->findTrack (keyId);
        juce::PopupMenu slot;
        slot.addItem (id + editSlot, "Edit...");
        slot.addItem (id + bypassSlot, "Bypass", true, bypassed);
        slot.addItem (id + moveSlotUp, "Move Up", i > first);
        slot.addItem (id + moveSlotDown, "Move Down", i + 1 < slots);

        // A track whose output goes into this plug-in's sidechain input: not this one, nor one
        // that listens to it.
        if (track != nullptr && effects.hasSidechain (trackId, i) && (int) state->tracks.size() < keyChoices)
        {
            juce::PopupMenu keys;
            keys.addItem (sidechainBase + i * keyChoices, "None", true, key == nullptr);

            for (size_t k = 0; k < state->tracks.size(); ++k)
            {
                const auto& t = state->tracks[k];

                if (! t.isBus && ! t.isFolder && t.id != trackId)
                    keys.addItem (sidechainBase + i * keyChoices + (int) k + 1, t.name, ! state->listensTo (t, trackId), t.id == keyId);
            }

            slot.addSubMenu ("Sidechain", keys);
        }

        slot.addSeparator();
        slot.addItem (id + removeSlot, "Remove");
        menu.addSubMenu (juce::String (i - first + 1) + ". " + plugin->getName() + (bypassed ? " (bypassed)" : "")
                             + (key != nullptr ? ", sidechain from " + key->name : juce::String()),
                         slot);
    }

    if (const auto latency = effects.getChainLatency (trackId); latency > 0)
        menu.addItem (-1, "Latency " + juce::String (latency) + " frames, compensated", false);

    if (slots > first)
        menu.addSeparator();

    if (types.isEmpty())
    {
        menu.addItem (-2, "No effects: scan with Plug-ins... first", false);
    }
    else
    {
        juce::PopupMenu add;
        PluginMenu::add (add, types);
        menu.addSubMenu ("Add Effect", add);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this, trackId, types, safe = juce::Component::SafePointer<MultitrackView> (this)] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result >= sidechainBase)
            {
                const auto slot = (result - sidechainBase) / keyChoices, choice = (result - sidechainBase) % keyChoices;
                const auto slotId = engine.getTrackEffects().getSlotId (trackId, slot);
                auto next = *engine.session.getState();
                const auto keyId = juce::isPositiveAndNotGreaterThan (choice, (int) next.tracks.size()) && choice > 0
                                       ? next.tracks[(size_t) choice - 1].id : 0u;

                if (SessionEdits::setSidechain (next, trackId, slotId, keyId))
                    engine.session.apply (std::move (next), "Sidechain");

                return;
            }

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

            const auto chosen = PluginMenu::chosen (types, result);

            if (juce::isPositiveAndBelow (chosen, types.size()))
                addTrackEffect (trackId, types[chosen]);
        });
}

juce::PopupMenu MultitrackView::instrumentMenuFor (juce::uint32 trackId)
{
    juce::PopupMenu menu;
    const auto* current = engine.getTrackEffects().getInstrument (trackId);
    const juce::Component::SafePointer<MultitrackView> safe (this);

    if (current != nullptr)
    {
        menu.addItem ("Instrument Window (" + current->getName() + ")", [safe, trackId] { if (safe != nullptr) safe->showInstrumentWindow (trackId); });
        menu.addItem ("Remove Instrument", [safe, trackId] { if (safe != nullptr) safe->removeTrackInstrument (trackId); });
        menu.addSeparator();
    }

    const auto instruments = Instrument::known (engine.knownPlugins);

    if (instruments.isEmpty())
        menu.addItem ("No instruments: scan with Plug-ins... first", false, false, nullptr);

    const auto ticked = current != nullptr ? current->getPluginDescription() : juce::PluginDescription();
    Instrument::addMenu (menu, instruments, [safe, trackId] (const juce::PluginDescription& d) { if (safe != nullptr) safe->setTrackInstrument (trackId, d); },
                         current != nullptr ? &ticked : nullptr);
    return menu;
}

void MultitrackView::showInstrumentMenu (juce::uint32 trackId, juce::Component& target)
{
    instrumentMenuFor (trackId).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

void MultitrackView::setTrackInstrument (juce::uint32 trackId, const juce::PluginDescription& description)
{
    if (engine.getTrackEffects().getInstrument (trackId) != nullptr)
        engine.removeTrackPlugin (trackId, 0);

    addTrackEffect (trackId, description);
}

void MultitrackView::removeTrackInstrument (juce::uint32 trackId)
{
    if (engine.getTrackEffects().getInstrument (trackId) != nullptr)
        engine.removeTrackPlugin (trackId, 0);
}

void MultitrackView::setTrackKind (juce::uint32 trackId, bool instrument)
{
    auto next = *engine.session.getState();

    if (! SessionEdits::setTrackKind (next, trackId, instrument))
        return;

    if (! instrument)
        removeTrackInstrument (trackId);

    engine.session.apply (std::move (next), instrument ? "Instrument Track" : "Audio Track");
}

void MultitrackView::setTrackMusical (juce::uint32 trackId, bool musical)
{
    auto next = *engine.session.getState();

    if (SessionEdits::setTrackMusical (next, trackId, musical))
        engine.session.apply (std::move (next), musical ? "Musical Timebase" : "Linear Timebase");
}

void MultitrackView::showInstrumentWindow (juce::uint32 trackId)
{
    if (auto* instrument = engine.getTrackEffects().getInstrument (trackId); instrument != nullptr && onEditPlugin != nullptr)
        onEditPlugin (*instrument);
}

void MultitrackView::addInstrumentTrack (const juce::PluginDescription& description)
{
    auto next = *engine.session.getState();
    auto& track = next.addInstrumentTrack();
    track.name = description.name;
    const auto id = track.id;
    engine.session.apply (std::move (next), "Add Instrument Track");
    chooseTrack (id);
    setTrackInstrument (id, description);
}

void MultitrackView::chooseTrack (juce::uint32 trackId)
{
    selectedTrackId = trackId;
    rebuildHeaders();
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void MultitrackView::addTrackEffect (juce::uint32 trackId, const juce::PluginDescription& description)
{
    const auto name = description.name;
    const auto instrument = description.isInstrument;

    engine.addTrackPlugin (trackId, description, [this, trackId, name, instrument, safe = juce::Component::SafePointer<MultitrackView> (this)] (const juce::String& error)
    {
        if (error.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not load " + name, error);
            return;
        }

        if (safe == nullptr || onEditPlugin == nullptr)
            return;

        const auto& effects = engine.getTrackEffects();
        const auto added = instrument ? 0 : effects.getNumSlots (trackId) - 1;

        if (auto* plugin = effects.getPlugin (trackId, added))
            onEditPlugin (*plugin);
    });
}

void MultitrackView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    state = engine.session.getState();

    // Clips that went, or whose track was hidden or folded away, leave the selection.
    for (auto it = selectedClips.begin(); it != selectedClips.end();)
    {
        const auto where = state->findClip (*it);
        it = where.has_value() && state->isShown (state->tracks[(size_t) where->first]) ? std::next (it) : selectedClips.erase (it);
    }

    if (const auto* t = state->findTrack (selectedTrackId); t != nullptr && ! state->isShown (*t))
        selectedTrackId = 0;

    if (const auto where = state->findClip (selectedClip); ! where.has_value() || ! state->isShown (state->tracks[(size_t) where->first]))
        selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

    for (auto it = selectedNoteClips.begin(); it != selectedNoteClips.end();)
    {
        const auto where = state->findNoteClip (*it);
        it = where.has_value() && state->isShown (state->tracks[(size_t) where->first]) ? std::next (it) : selectedNoteClips.erase (it);
    }

    if (! isNoteClipSelected (selectedNoteClip))
        selectedNoteClip = selectedNoteClips.empty() ? 0 : *selectedNoteClips.begin();

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
    updateHeaderMeters();

    if (engine.getPlayer().isPlaying() && engine.getPlayer().getMode() == DocumentPlayerProcessor::Mode::session)
    {
        // Keep the play position in view.
        const auto x = xForFrame (engine.getPlayer().getPositionFrames());
        const auto area = timelineArea();

        if (x > (float) area.getRight() - 20.0f)
            setView ((double) engine.getPlayer().getPositionFrames() - 20.0 * framesPerPixel, framesPerPixel);

        // A control held still keeps writing its value as the play position moves on.
        if (pass.has_value())
        {
            std::vector<float> held;

            for (auto& points : pass->points)
                held.push_back (points.back().value);

            continueAutomationPass (engine.getPlayer().getPositionFrames(), held);
        }

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
    return player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session ? player.getHeardFrame() : cursor;
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
    startAutomationPass (trackId, std::vector<AutomationTarget> { p }, frame, { value });
}

void MultitrackView::startAutomationPass (juce::uint32 trackId, const std::vector<AutomationTarget>& parameters, juce::int64 frame,
                                          const std::vector<float>& values)
{
    finishAutomationPass();

    const auto current = engine.session.getState();

    if (current->findTrack (trackId) == nullptr || parameters.empty() || parameters.size() != values.size())
        return;

    pass = AutomationPass { trackId, parameters, *current, {} };

    for (auto value : values)
        pass->points.push_back ({ { juce::jmax ((juce::int64) 0, frame), value } });

    const auto& p = parameters.front();

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
    continueAutomationPass (frame, std::vector<float> { value });
}

void MultitrackView::continueAutomationPass (juce::int64 frame, const std::vector<float>& values)
{
    if (! pass.has_value() || values.size() != pass->points.size())
        return;

    if (frame < pass->points.front().back().frame)
    {
        finishAutomationPass();
        return;
    }

    auto changed = false;

    for (size_t i = 0; i < values.size(); ++i)
    {
        auto& points = pass->points[i];
        const auto value = values[i];
        const auto held = points.size() >= 2 && juce::exactlyEqual (points[points.size() - 2].value, points.back().value)
                          && juce::exactlyEqual (points.back().value, value);

        if (frame == points.back().frame || held)
        {
            // The same frame, or a value held still: the last point moves rather than adding one.
            if (frame == points.back().frame && juce::exactlyEqual (points.back().value, value))
                continue;

            points.back() = { frame, value };
        }
        else
        {
            points.push_back ({ frame, value });
        }

        changed = true;
    }

    if (changed)
        applyAutomationPass (false);
}

void MultitrackView::finishAutomationPass()
{
    if (pass.has_value() && pass->parameters.front().parameter == AutomationParameter::effect)
    {
        const auto& p = pass->parameters.front();
        engine.setTrackEffectTouched (pass->trackId, p.slotId, p.parameterId, false);
    }

    pass.reset();
}

void MultitrackView::applyAutomationPass (bool first)
{
    auto next = pass->base;
    const auto returnFrames = (juce::int64) std::llround (returnSeconds * next.sampleRate);

    for (size_t i = 0; i < pass->parameters.size(); ++i)
        SessionEdits::writeAutomation (next, pass->trackId, pass->parameters[i], pass->points[i], returnFrames);

    // The first write is a step of its own; the rest of the pass merges into it.
    const auto kind = pass->parameters.front().parameter;
    engine.session.apply (std::move (next), kind == AutomationParameter::volume ? "Write Volume Automation"
                                          : kind == AutomationParameter::pan ? "Write Pan Automation"
                                          : kind == AutomationParameter::effect ? "Write Effect Automation" : "Write Surround Automation", ! first);
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
        const auto hidden = i < tracks.size() && ! shown().isShown (tracks[i]);
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
        row += shown().isShown (tracks[(size_t) t]) ? 1 : 0;

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
        if (shown().isShown (tracks[(size_t) t]) && row-- == 0)
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

juce::Rectangle<int> MultitrackView::noteClipBounds (int track, const NoteClip& c) const
{
    const auto x0 = xForFrame (c.position), x1 = xForFrame (c.getEnd());
    return juce::Rectangle<float> (x0, (float) laneTop (track) + 3.0f, juce::jmax (2.0f, x1 - x0), (float) laneHeight - 6.0f).toNearestInt();
}

std::optional<std::pair<int, int>> MultitrackView::noteClipAt (juce::Point<int> p) const
{
    const auto& tracks = shown().tracks;
    const auto t = trackAt (p.y);

    if (! juce::isPositiveAndBelow (t, (int) tracks.size()) || p.x < headerWidth)
        return std::nullopt;

    for (auto c = (int) tracks[(size_t) t].noteClips.size() - 1; c >= 0; --c)
        if (noteClipBounds (t, tracks[(size_t) t].noteClips[(size_t) c]).expanded (2, 0).contains (p))
            return std::pair<int, int> { t, c };

    return std::nullopt;
}

void MultitrackView::drawNoteClip (juce::Graphics& g, int track, const NoteClip& c) const
{
    const auto r = noteClipBounds (track, c);
    const auto& t = shown().tracks[(size_t) track];
    const auto own = juce::isPositiveAndBelow (c.colour, (int) TrackColours::palette.size()) ? juce::Colour (TrackColours::palette[(size_t) c.colour].argb)
                                                                                              : TrackColours::of (t, track);
    const auto colour = c.muted ? Theme::faintText : own;
    const auto selected = isNoteClipSelected (c.id);
    const auto playable = engine.getTrackEffects().getInstrument (t.id) != nullptr;

    g.setColour (colour.withAlpha (0.14f));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);

    auto body = r.reduced (1);
    auto title = body.removeFromTop (16);
    g.setColour (colour.withAlpha (0.45f));
    g.fillRect (title);

    g.setColour (juce::Colours::white);
    g.setFont (12.0f);
    auto name = c.name;

    if (c.loopLength > 0 && c.loopLength < c.length)
        name << "  (repeats)";

    if (c.muted)
        name << "  (muted)";
    else if (! playable)
        name << "  (no instrument)";

    g.drawText (name, title.withTrimmedLeft (6).withTrimmedRight (6), juce::Justification::centredLeft, true);

    // The notes as bars, the pitch up the body over the notes' own range (a lone pitch in the
    // middle), each pass of a repeating clip drawn where it plays, with a line at each repeat.
    const auto left = juce::jmax (body.getX(), headerWidth), right = juce::jmin (body.getRight(), timelineArea().getRight());

    if (body.getHeight() < 6 || right <= left || c.notes.empty())
        return;

    int low = 127, high = 0;

    for (auto& n : c.notes)
    {
        low = juce::jmin (low, n.pitch);
        high = juce::jmax (high, n.pitch);
    }

    const auto rows = juce::jmax (1, high - low + 1);
    const auto rowHeight = juce::jmin (6.0f, (float) (body.getHeight() - 2) / (float) rows);
    const auto top = (float) body.getCentreY() - rowHeight * (float) rows * 0.5f;
    const auto period = juce::jmax ((juce::int64) 1, c.getPeriod());
    const auto clip = g.getClipBounds();

    for (auto passStart = c.getOrigin(); passStart < c.getEnd(); passStart += period)
    {
        if (passStart > c.position)
        {
            const auto x = xForFrame (passStart);

            if (x >= (float) left && x <= (float) right)
            {
                g.setColour (colour.withAlpha (0.35f));
                g.drawVerticalLine ((int) x, (float) body.getY(), (float) body.getBottom());
            }
        }

        g.setColour (colour.withAlpha (playable && ! c.muted ? 0.95f : 0.5f));

        for (auto& n : c.notes)
        {
            const auto start = passStart + n.start, end = juce::jmin (c.getEnd(), start + n.length);

            if (n.start >= period || start < c.position || start >= c.getEnd())
                continue;

            const auto x0 = juce::jmax ((float) left, xForFrame (start)), x1 = juce::jmin ((float) right, xForFrame (end));
            const auto y = top + rowHeight * (float) (high - n.pitch);

            if (x1 > x0 && (int) y <= clip.getBottom())
                g.fillRect (juce::Rectangle<float> (x0, y + 0.5f, juce::jmax (1.0f, x1 - x0), juce::jmax (1.0f, rowHeight - 1.0f)));
        }
    }

    if (selected)
    {
        g.setColour (juce::Colours::white);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 2.0f);
    }
}

//==============================================================================
void MultitrackView::setView (double start, double fpp)
{
    const auto width = (double) juce::jmax (1, timelineArea().getWidth());
    framesPerPixel = juce::jlimit (0.05, 1.0e6, fpp);
    // As far as the session, a minute, or a screen past the cursor, whichever is furthest.
    const auto length = juce::jmax ((double) juce::jmax (shown().getLength(), (juce::int64) (shown().sampleRate * 60.0)),
                                    (double) cursor + width * framesPerPixel);
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - width * framesPerPixel * 0.5), start);
    updateScrollBars();
    repaint();
}

void MultitrackView::scrollTo (double startFrame)
{
    setView (startFrame, framesPerPixel);
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

    // Pan, and the surround pan's left and right: left at the top.
    if (p.parameter == AutomationParameter::pan || p.parameter == AutomationParameter::surroundX)
        proportion = 1.0f - proportion;

    return bottom - proportion * (bottom - top);
}

float MultitrackView::valueForY (int track, const AutomationTarget& p, float y) const
{
    const auto top = (float) laneTop (track) + envelopeMargin, bottom = (float) (laneTop (track) + laneHeight) - envelopeMargin;
    auto proportion = juce::jlimit (0.0f, 1.0f, (bottom - y) / (bottom - top));

    if (p.parameter == AutomationParameter::pan || p.parameter == AutomationParameter::surroundX)
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

namespace
{
    // A clip's gain line: its whole range, 0 dB half way up.
    juce::NormalisableRange<float> clipGainScale()
    {
        juce::NormalisableRange<float> scale (SessionClip::minGainDb, SessionClip::maxGainDb);
        scale.setSkewForCentre (0.0f);
        return scale;
    }

    float yForClipGain (juce::Rectangle<float> area, float db)
    {
        return area.getBottom() - clipGainScale().convertTo0to1 (juce::jlimit (SessionClip::minGainDb, SessionClip::maxGainDb, db)) * area.getHeight();
    }

    float clipGainForY (juce::Rectangle<float> area, float y)
    {
        return clipGainScale().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, (area.getBottom() - y) / area.getHeight()));
    }
}

juce::Rectangle<float> MultitrackView::clipGainArea (int track, const SessionClip& c) const
{
    // The body below the title bar, a little in from its top and bottom.
    return clipBounds (track, c).toFloat().withTrimmedTop (17.0f).reduced (0.0f, 4.0f);
}

juce::int64 MultitrackView::clipAudioFrameForX (const SessionClip& c, float x) const
{
    const auto frame = juce::jlimit (c.position, juce::jmax (c.position, c.getEnd() - 1), frameForX (x));
    return c.offset + (frame - c.position);
}

std::optional<MultitrackView::ClipGainHit> MultitrackView::clipGainAt (juce::Point<int> p) const
{
    if (! showClipGain)
        return std::nullopt;

    const auto hit = clipAt (p);

    if (! hit.has_value())
        return std::nullopt;

    const auto& track = shown().tracks[(size_t) hit->first];
    const auto& clip = track.clips[(size_t) hit->second];
    const auto area = clipGainArea (hit->first, clip);

    if (clip.locked || track.isFrozen() || area.getHeight() < 8.0f)
        return std::nullopt;

    ClipGainHit found { hit->first, hit->second, -1 };
    auto nearest = 7.0f;
    const auto& points = clip.gainEnvelope.points;

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto x = xForFrame (clip.position + points[i].frame - clip.offset);
        const auto distance = p.toFloat().getDistanceFrom ({ x, yForClipGain (area, points[i].value) });

        if (distance < nearest)
        {
            nearest = distance;
            found.point = (int) i;
        }
    }

    if (found.point >= 0)
        return found;

    const auto lineY = yForClipGain (area, clip.gainEnvelope.valueAt (clipAudioFrameForX (clip, (float) p.x)));
    return std::abs ((float) p.y - lineY) <= 5.0f ? std::optional<ClipGainHit> (found) : std::nullopt;
}

void MultitrackView::drawClipGain (juce::Graphics& g, int track, const SessionClip& c) const
{
    if (! showClipGain && c.gainEnvelope.isEmpty())
        return;

    const auto area = clipGainArea (track, c);
    const auto left = juce::jmax (area.getX(), (float) headerWidth), right = juce::jmin (area.getRight(), (float) timelineArea().getRight());

    if (area.getHeight() < 8.0f || right <= left)
        return;

    // Sampled every other pixel: the line is straight in dB, and dB aren't spaced evenly here.
    juce::Path line;

    for (auto x = left; ; x = juce::jmin (right, x + 2.0f))
    {
        const auto y = yForClipGain (area, c.gainEnvelope.valueAt (clipAudioFrameForX (c, x)));

        if (line.isEmpty())
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);

        if (x >= right)
            break;
    }

    const auto colour = juce::Colour (0xffffb35c).withAlpha (showClipGain ? 1.0f : 0.5f);
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (1.5f));

    if (! showClipGain)
        return;

    for (auto& p : c.gainEnvelope.points)
    {
        const auto x = xForFrame (c.position + p.frame - c.offset);

        if (x >= left - 4.0f && x <= right + 4.0f)
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, yForClipGain (area, p.value) }));
    }
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
                      : p.parameter == AutomationParameter::pan ? juce::Colour (0xffff9de0)
                      : isSurroundParameter (p.parameter) ? juce::Colour (0xffffc46b) : juce::Colour (0xffb8f07a);
    const auto left = (float) area.getX(), right = (float) area.getRight();
    juce::String label = p.parameter == AutomationParameter::volume ? "Volume" : "Pan (left at the top)";

    if (isSurroundParameter (p.parameter))
    {
        const auto value = lineValueAt (t, p, controlFrame());
        const auto text = p.parameter == AutomationParameter::surroundWidth ? juce::String (juce::roundToInt (value * 100.0f)) + "%"
                        : p.parameter == AutomationParameter::lfe ? (value <= -60.0f ? juce::String ("off") : juce::String (value, 1) + " dB")
                        : juce::String (value, 2);
        label = "Surround " + surroundEnvelopeName (p.parameter) + " " + text + (shown().surround ? "" : " (in a 5.1 or 7.1 master)");
    }

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
    // The finest spacing that leaves at least 100 pixels between marks, in the steps the time
    // format counts in.
    const auto rate = shown().sampleRate;
    return TimeFormat::rulerTick (framesPerPixel / rate, rate);
}

SessionSnap::Targets MultitrackView::snapTargets (juce::uint32 ignoreClip, bool toCursorAndRange) const
{
    SessionSnap::Targets targets;
    targets.ignoreClip = ignoreClip;

    if (showBars)
        targets.beatGrid = rulerBeats().second;
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

std::pair<Metronome::Grid, Metronome::Grid> MultitrackView::rulerBeats() const
{
    // Labels at least 50 pixels apart: every beat, or every 1, 2, 4... bars. Marks (and the snap
    // grid) every beat while beats are 8 pixels apart, else on the labels. The shortest beat and
    // bar anywhere in the tempo map decide, so the spacing holds at every tempo.
    const auto map = shown().tempoMap();
    const auto beatPixels = map.shortestBeat() / framesPerPixel, barPixels = map.shortestBar() / framesPerPixel;
    Metronome::Grid label;

    if (beatPixels < 50.0)
        for (label.bars = 1; label.bars * barPixels < 50.0 && label.bars < (1 << 24);)
            label.bars *= 2;

    return { label, beatPixels >= 8.0 ? Metronome::Grid {} : label };
}

void MultitrackView::drawBarsRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto& s = shown();
    const auto map = s.tempoMap();
    const auto [label, mark] = rulerBeats();
    const auto first = juce::jmax ((juce::int64) 0, frameForX ((float) area.getX())), last = frameForX ((float) area.getRight());
    const auto changeAt = [&s] (juce::int64 bar)
    {
        return std::find_if (s.tempoChanges.begin(), s.tempoChanges.end(), [bar] (auto& c) { return c.bar == bar; });
    };
    const auto full = area.getHeight() >= 20;   // the ruler to itself, or the row above the time
    g.setFont (full ? 11.0f : 9.5f);

    map.forEachLine (first, last, mark, [&] (juce::int64 frame, juce::int64 beat)
    {
        const auto x = xForFrame (frame);
        const auto [bar, inBar] = map.barAndBeat (beat);
        const auto labelled = label.bars == 0 || (inBar == 0 && bar % label.bars == 0);
        const auto change = inBar == 0 ? changeAt (bar) : s.tempoChanges.end();
        g.setColour (change != s.tempoChanges.end() ? juce::Colour (0xfff2cc60) : Theme::dimText);
        g.drawVerticalLine ((int) x, change != s.tempoChanges.end() ? (float) area.getY() : (float) area.getBottom() - (labelled ? (full ? 8.0f : 5.0f) : (full ? 4.0f : 3.0f)),
                            (float) area.getBottom());

        if (! labelled && change == s.tempoChanges.end())
            return;

        // Bars count from 1, and a beat within a bar from 1: "5" is bar 5, "5.3" its third beat.
        // A bar where the tempo changes shows the new tempo and time signature after its number.
        const auto text = inBar == 0 ? juce::String (bar + 1) : juce::String (bar + 1) + "." + juce::String (inBar + 1);
        const auto box = juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 120.0f, (float) area.getHeight() - (full ? 4.0f : 1.0f));
        g.setColour (Theme::text);
        g.drawText (text, box, juce::Justification::centredLeft, false);

        if (change != s.tempoChanges.end())
        {
            g.setColour (juce::Colour (0xfff2cc60));
            g.drawText (juce::String (change->tempo, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " bpm, " + juce::String (change->beatsPerBar) + " beats",
                        box.withTrimmedLeft (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 5.0f),
                        juce::Justification::centredLeft, false);
        }
    });

    // Changes between the marks still show where they are.
    for (auto& c : s.tempoChanges)
        if (const auto frame = map.barFrame (c.bar); frame >= first && frame <= last && label.bars > 1 && c.bar % label.bars != 0)
        {
            g.setColour (juce::Colour (0xfff2cc60));
            g.drawVerticalLine ((int) xForFrame (frame), (float) area.getY(), (float) area.getBottom());
        }
}

void MultitrackView::drawRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (Theme::surface);
    g.fillRect (area);

    // Bars and beats; with the time too, in a second, smaller row under them.
    if (showBars && timeWithBars)
    {
        drawBarsRuler (g, area.removeFromTop (area.getHeight() * 7 / 12));
        drawTimeRuler (g, area);
        return;
    }

    if (showBars)
    {
        drawBarsRuler (g, area);
        return;
    }

    drawTimeRuler (g, area);
}

void MultitrackView::drawTimeRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto rate = shown().sampleRate;
    const auto secondsPerPixel = framesPerPixel / rate;
    const auto tick = rulerTick();
    const auto full = area.getHeight() >= 20;   // the ruler to itself, or a row under the bars

    g.setFont (full ? 11.0f : 9.0f);
    const auto startSeconds = viewStart / rate;
    const auto endSeconds = startSeconds + area.getWidth() * secondsPerPixel;

    for (auto t = std::floor (startSeconds / tick) * tick; t <= endSeconds; t += tick)
    {
        const auto x = xForFrame ((juce::int64) std::llround (t * rate));
        g.setColour (Theme::dimText);
        g.drawVerticalLine ((int) x, (float) area.getBottom() - (full ? 6.0f : 3.0f), (float) area.getBottom());
        g.setColour (Theme::text);
        g.drawText (TimeFormat::rulerLabel (t, tick, rate), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 96.0f, (float) area.getHeight() - (full ? 4.0f : 1.0f)),
                    juce::Justification::centredLeft, false);
    }
}

void MultitrackView::drawClip (juce::Graphics& g, int track, const SessionClip& c) const
{
    const auto r = clipBounds (track, c);
    const auto colour = c.muted ? Theme::faintText : TrackColours::of (c, shown().tracks[(size_t) track], track);
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
    const auto badges = (c.locked ? 20 : 0) + (c.groupId != 0 ? 20 : 0);
    g.drawText (name, title.withTrimmedLeft (12).withTrimmedRight (badges > 0 ? badges + 4 : 10), juce::Justification::centredLeft, true);   // clear of the fade handles

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

    if (c.groupId != 0 && title.getWidth() > 40)
    {
        // Two chain links: the clip is in a group.
        const auto box = title.removeFromRight (20).withSizeKeepingCentre (14, 8).toFloat();
        g.drawRoundedRectangle (box.withWidth (8.0f), 3.0f, 1.3f);
        g.drawRoundedRectangle (box.withTrimmedLeft (6.0f), 3.0f, 1.3f);
    }

    // Waveform: one peak per pixel column, each channel in its own band; the channels that play
    // (the first two, or up to six or eight on a 5.1 or 7.1 track, a 5.1 or 7.1 clip's named down the left).
    if (c.audio != nullptr && body.getHeight() > 4)
    {
        const auto wideChannels = shown().tracks[(size_t) track].wideChannels();
        const auto channels = juce::jmin (wideChannels > 0 ? wideChannels : 2, c.audio->getNumChannels());
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

            if (Surround::isSurround (channels) && bandHeight >= 10.0f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.setFont (9.0f);
                g.drawText (Surround::channelName (channels, ch),
                            juce::Rectangle<float> ((float) left + 3.0f, mid - bandHeight * 0.5f, 24.0f, bandHeight), juce::Justification::centredLeft, false);
                g.setColour (colour.withAlpha (0.9f));
            }
        }
    }

    // Fades as the curves they play, from silence up to full level.
    const auto drawFade = [&] (FadeShape shape, float silentX, float fullX)
    {
        const auto top = (float) r.getY() + 16.0f, bottom = (float) r.getBottom();
        const auto curve = fadeCurve (shape, silentX, fullX, top, bottom);
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
        drawFade (c.fadeInShape, (float) r.getX(), xForFrame (c.position + c.fadeIn));

    if (c.fadeOut > 0)
        drawFade (c.fadeOutShape, (float) r.getRight(), xForFrame (c.getEnd() - c.fadeOut));

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
            if (! s.isShown (s.tracks[(size_t) t]))
                continue;

            const auto lane = juce::Rectangle<int> (area.getX(), laneTop (t), area.getWidth(), laneHeight);
            g.setColour (s.tracks[(size_t) t].id == selectedTrackId ? Theme::hover : (rowOf (t) % 2 == 0 ? laneColour : laneAlternate));
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
                    else if (other.outputBusId == track.id && &other != &track)
                        feeders.add (other.name + " (output)");

                g.setColour (Theme::overlay.withAlpha (0.04f));
                g.fillRect (lane.reduced (0, 3));
                g.setColour (Theme::dimText);
                g.setFont (13.0f);
                const auto* into = s.outputBusOf (track);
                g.drawText ((feeders.isEmpty() ? juce::String ("Bus: nothing plays here yet. Use a track's Sends button.")
                                               : "Bus: from " + feeders.joinIntoString (", "))
                                + (into != nullptr ? "; plays into " + into->name
                                   : track.deviceOutput >= 0 ? "; plays out of interface outputs " + juce::String (track.deviceOutput + 1) + "/" + juce::String (track.deviceOutput + 2)
                                                             : juce::String()),
                            lane.reduced (12, 0), juce::Justification::centredLeft);
            }

            // A folder's lane: where its members have clips, one thin bar per member.
            if (track.isFolder)
            {
                std::vector<int> members;

                for (int m = 0; m < (int) s.tracks.size(); ++m)
                    if (s.tracks[(size_t) m].folderId == track.id)
                        members.push_back (m);

                const auto band = lane.reduced (0, 6).toFloat();
                const auto barHeight = members.empty() ? 0.0f : juce::jmin (8.0f, band.getHeight() / (float) members.size());

                for (size_t i = 0; i < members.size(); ++i)
                {
                    const auto& member = s.tracks[(size_t) members[i]];
                    const auto y = band.getY() + barHeight * (float) i;

                    for (auto& c : member.clips)
                    {
                        const auto x0 = juce::jmax ((float) lane.getX(), xForFrame (c.position)), x1 = juce::jmin ((float) lane.getRight(), xForFrame (c.getEnd()));

                        if (x1 > x0)
                        {
                            g.setColour (TrackColours::of (c, member, members[i]).withAlpha (c.muted ? 0.25f : 0.6f));
                            g.fillRect (juce::Rectangle<float> (x0, y + 1.0f, x1 - x0, juce::jmax (1.0f, barHeight - 2.0f)));
                        }
                    }
                }

                if (members.empty())
                {
                    g.setColour (Theme::dimText);
                    g.setFont (13.0f);
                    g.drawText ("Folder: empty. Right-click a track's header to put it in.", lane.reduced (12, 0), juce::Justification::centredLeft);
                }
            }

            for (auto& c : track.clips)
                drawClip (g, t, c);

            for (auto& c : track.noteClips)
                drawNoteClip (g, t, c);

            // Where clips overlap, a later clip covers the fade out under it: draw the fade outs
            // again on top, so a crossfade shows both its curves.
            for (auto& c : track.clips)
            {
                if (c.fadeOut <= 0)
                    continue;

                const auto r = clipBounds (t, c);
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.strokePath (fadeCurve (c.fadeOutShape, (float) r.getRight(), xForFrame (c.getEnd() - c.fadeOut), (float) r.getY() + 16.0f, (float) r.getBottom()),
                              juce::PathStrokeType (1.0f));
            }

            // A frozen track plays its rendered audio: an icy wash over the frames it covers, so
            // it is clear its clips can't be changed until it is unfrozen.
            if (track.isFrozen())
            {
                const auto x0 = juce::jmax ((float) lane.getX(), xForFrame (0));
                const auto x1 = juce::jmin ((float) lane.getRight(), xForFrame (track.frozenAudio->getNumFrames()));

                if (x1 > x0)
                {
                    g.setColour (frozenColour.withAlpha (0.18f));
                    g.fillRect (juce::Rectangle<float> (x0, (float) lane.getY(), x1 - x0, (float) lane.getHeight() - 1.0f));
                }

                g.setColour (frozenColour);
                g.setFont (12.0f);
                g.drawText ("Frozen", lane.withTrimmedLeft (6).withTrimmedBottom (3).withHeight (lane.getHeight() - 3),
                            juce::Justification::bottomLeft);
            }

            for (auto& c : track.clips)
                drawClipGain (g, t, c);

            if (const auto parameter = getShownEnvelope (track.id))
                drawEnvelope (g, t, *parameter);
        }

        if (s.tracks.empty())
        {
            g.setColour (Theme::dimText);
            g.setFont (15.0f);
            g.drawText ("Drop audio files here, or choose Multitrack > Import Audio", area, juce::Justification::centred);
        }

        // In bars and beats, the grid of the session's tempo.
        if (showBars && ! s.tracks.empty())
        {
            const auto bottom = (float) juce::jmin (area.getBottom(), laneTop ((int) s.tracks.size()));
            const auto map = s.tempoMap();

            map.forEachLine (juce::jmax ((juce::int64) 0, frameForX ((float) area.getX())), frameForX ((float) area.getRight()), rulerBeats().second,
                             [&] (juce::int64 frame, juce::int64 beat)
                             {
                                 g.setColour (Theme::overlay.withAlpha (map.barAndBeat (beat).second == 0 ? 0.12f : 0.05f));
                                 g.drawVerticalLine ((int) xForFrame (frame), (float) area.getY(), bottom);
                             });
        }

        if (! timeSelection.isEmpty())
        {
            const auto x0 = xForFrame (timeSelection.getStart()), x1 = xForFrame (timeSelection.getEnd());
            g.setColour (Theme::overlay.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()));
        }

        if (! recording.isEmpty())
        {
            std::vector<int> lanes;

            for (int t = 0; t < (int) s.tracks.size(); ++t)
                if (isArmed (s.tracks[(size_t) t].id) && s.isShown (s.tracks[(size_t) t]))
                    lanes.push_back (t);

            if (lanes.empty())
                lanes.push_back ((int) s.tracks.size());

            const auto x0 = xForFrame (recording.getStart()), x1 = xForFrame (recording.getEnd());

            for (auto lane : lanes)
            {
                const auto r = juce::Rectangle<float> (x0, (float) laneTop (lane) + 2.0f, juce::jmax (1.0f, x1 - x0), (float) laneHeight - 4.0f);
                g.setColour (Theme::danger.withAlpha (0.3f));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (Theme::danger);
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
            }
        }

        // A track being moved: a line where it would land, above or below the track there, or
        // the folder it would go into, outlined.
        if (trackDropIndex >= 0)
        {
            const auto from = getSelectedTrack();
            g.setColour (cursorColour);

            if (dropsIntoFolder (from, trackDropIndex))
            {
                g.drawRect (juce::Rectangle<int> (0, laneTop (trackDropIndex), getWidth(), laneHeight), 3);
            }
            else
            {
                const auto y = trackDropIndex > from ? laneTop (trackDropIndex + 1) : laneTop (trackDropIndex);
                g.fillRect (0, y - 1, getWidth(), 3);
            }
        }

        g.setColour (cursorColour);
        g.drawVerticalLine ((int) xForFrame (cursor), (float) area.getY(), (float) area.getBottom());

        auto& player = engine.getPlayer();

        if (player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session)
        {
            g.setColour (playColour);
            g.drawVerticalLine ((int) xForFrame (player.getHeardFrame()), (float) area.getY(), (float) area.getBottom());
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
    // A rubber band, over what it has crossed.
    if (drag == Drag::rubberBand && ! rubberBand.isEmpty())
    {
        g.setColour (Theme::accent.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (Theme::accent);
        g.drawRect (rubberBand, 1);
    }

    // A note clip being drawn: where it would go, outlined.
    if (drag == Drag::drawNotes && drawEnd != drawStart && drawTrack >= 0 && ! s.tracks.empty())
    {
        const auto last = juce::jmin (drawTrack, (int) s.tracks.size() - 1);
        const auto top = laneTop (last) + (drawTrack - last) * laneHeight;
        const auto x0 = xForFrame (juce::jmin (drawStart, drawEnd)), x1 = xForFrame (juce::jmax (drawStart, drawEnd));
        const auto r = juce::Rectangle<float> (x0, (float) top + 3.0f, juce::jmax (2.0f, x1 - x0), (float) laneHeight - 6.0f);
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (r, 3.0f, 1.5f);
    }

    // A note clip being stretched: where it would play, outlined, with the ratio.
    if (! stretchTo.isEmpty() && (drag == Drag::stretchNotesStart || drag == Drag::stretchNotesEnd) && dragTrack >= 0 && dragTrack < (int) s.tracks.size())
    {
        auto stretched = dragNoteClip;
        stretched.position = stretchTo.getStart();
        stretched.length = stretchTo.getLength();
        const auto r = noteClipBounds (dragTrack, stretched).toFloat();
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (r, 3.0f, 1.5f);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (100.0 * (double) stretchTo.getLength() / (double) juce::jmax ((juce::int64) 1, dragNoteClip.length), 1) + "%",
                    r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::topLeft, false);
    }

    // A clip being stretched: where it would play, outlined, with the ratio.
    if (! stretchTo.isEmpty() && (drag == Drag::stretchStart || drag == Drag::stretchEnd) && dragTrack >= 0 && dragTrack < (int) s.tracks.size())
    {
        auto stretched = dragClip;
        stretched.position = stretchTo.getStart();
        stretched.length = stretchTo.getLength();
        const auto r = clipBounds (dragTrack, stretched).toFloat();
        g.setColour (cursorColour);
        g.drawRect (r, 2.0f);
        g.setFont (11.0f);
        g.drawText (juce::String (100.0 * (double) stretchTo.getLength() / (double) juce::jmax ((juce::int64) 1, dragClip.length), 1) + "%",
                    r.reduced (6.0f, 2.0f), juce::Justification::topRight, false);
    }

    g.setColour (Theme::surface);
    g.fillRect (0, 0, headerWidth, rulerHeight);
    g.setColour (Theme::text);
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
    g.setColour (Theme::raised);
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
        const auto colour = TrackColours::of (m);

        if (m.isRegion())
        {
            const auto x2 = xForFrame (m.getEnd());

            if (x2 < (float) area.getX() || x1 > (float) area.getRight())
                continue;

            const auto bar = juce::Rectangle<float> (x1, lane.getY() + 2.0f, juce::jmax (2.0f, x2 - x1), lane.getHeight() - 4.0f);
            g.setColour (colour.withAlpha (active ? 0.75f : 0.45f));
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (colour.withAlpha (0.45f));
            g.drawVerticalLine ((int) x1, top, bottom);
            g.drawVerticalLine ((int) x2, top, bottom);
            g.setColour (Theme::textStrong);
            g.drawText (m.name, bar.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            if (x1 < (float) area.getX() - 100.0f || x1 > (float) area.getRight())
                continue;

            juce::Path flag;
            flag.addTriangle (x1, lane.getY() + 2.0f, x1 + 7.0f, lane.getCentreY(), x1, lane.getBottom() - 2.0f);
            g.setColour (active ? colour.brighter (0.4f) : colour);
            g.fillPath (flag);
            g.setColour (colour.withAlpha (0.55f));
            g.drawVerticalLine ((int) x1, top, bottom);
            g.setColour (Theme::textStrong);
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
        m.start = juce::jmax ((juce::int64) 0, player.getHeardFrame());
    else if (! timeSelection.isEmpty())
        m = { timeSelection.getStart(), timeSelection.getLength(), {} };
    else
        m.start = cursor;

    m.name = MarkerEdits::nextName (next.markers, m.isRegion());
    next.markers.push_back (m);
    MarkerEdits::sort (next.markers);
    engine.session.apply (std::move (next), "Add " + m.name);
}

bool MultitrackView::addRegionFromSelectedClips()
{
    const auto ids = getSelectedClips();
    auto next = *engine.session.getState();

    if (ids.empty())
        return false;

    auto start = std::numeric_limits<juce::int64>::max();
    juce::int64 end = 0;
    juce::String name;

    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        const auto& c = next.tracks[(size_t) where->first].clips[(size_t) where->second];
        start = juce::jmin (start, c.position);
        end = juce::jmax (end, c.getEnd());
        name = c.name;
    }

    Marker m { start, end - start, ids.size() == 1 && name.isNotEmpty() ? name : MarkerEdits::nextName (next.markers, true) };
    next.markers.push_back (m);
    MarkerEdits::sort (next.markers);
    engine.session.apply (std::move (next), "Add " + m.name);
    return true;
}

void MultitrackView::addMarkerAt (juce::int64 frame)
{
    auto next = *engine.session.getState();
    Marker m;
    m.start = juce::jmax ((juce::int64) 0, frame);
    m.name = MarkerEdits::nextName (next.markers, false);
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

bool MultitrackView::selectBetweenMarkers()
{
    const auto& s = *engine.session.getState();
    const auto span = MarkerEdits::spanAround (s.markers, cursor, s.getLength());

    if (span)
        setTimeSelection (*span);

    return span.has_value();
}

int MultitrackView::addMarkersAtIntervals (juce::int64 step)
{
    auto next = *engine.session.getState();
    const auto range = timeSelection.isEmpty() ? juce::Range<juce::int64> (0, next.getLength()) : timeSelection;
    const auto added = MarkerEdits::addAtIntervals (next.markers, range, step);

    if (added > 0)
        engine.session.apply (std::move (next), "Add " + juce::String (added) + (added == 1 ? " Marker" : " Markers"));

    return added;
}

bool MultitrackView::renameMarkersInSequence (const juce::String& base)
{
    auto next = *engine.session.getState();

    if (! MarkerEdits::renameInSequence (next.markers, base) || next.markers == engine.session.getState()->markers)
        return false;

    engine.session.apply (std::move (next), "Rename Markers");
    return true;
}

int MultitrackView::regionsFromMarkers()
{
    auto next = *engine.session.getState();
    const auto turned = MarkerEdits::regionsFromPoints (next.markers, next.getLength());

    if (turned > 0)
        engine.session.apply (std::move (next), "Regions from Markers");

    return turned;
}

bool MultitrackView::selectAdjacentRegion (bool forwards)
{
    const auto from = timeSelection.isEmpty() ? cursor : timeSelection.getStart();
    const auto region = MarkerEdits::adjacentRegion (engine.session.getState()->markers, from, forwards);

    if (! region)
        return false;

    setTimeSelection (*region);
    showFrame (region->getStart());
    return true;
}

bool MultitrackView::selectRegionAtCursor()
{
    const auto region = MarkerEdits::regionAt (engine.session.getState()->markers, cursor);

    if (region)
        setTimeSelection (*region);

    return region.has_value();
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

bool MultitrackView::setMarkerColour (const Marker& marker, int colour)
{
    // Found again by value: the markers may have changed while the menu was open.
    auto next = *engine.session.getState();
    const auto it = std::find (next.markers.begin(), next.markers.end(), marker);
    colour = juce::isPositiveAndBelow (colour, (int) TrackColours::palette.size()) ? colour : -1;

    if (it == next.markers.end() || it->colour == colour)
        return false;

    it->colour = colour;
    engine.session.apply (std::move (next), "Colour " + marker.name);
    return true;
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

    juce::PopupMenu colours;
    colours.addItem ("Default", true, old.colour < 0, [this, old] { setMarkerColour (old, -1); });

    for (int c = 0; c < (int) TrackColours::palette.size(); ++c)
    {
        juce::PopupMenu::Item item (TrackColours::palette[(size_t) c].name);
        item.setTicked (old.colour == c);
        item.setColour (juce::Colour (TrackColours::palette[(size_t) c].argb));
        item.setAction ([this, old, c] { setMarkerColour (old, c); });
        colours.addItem (std::move (item));
    }

    menu.addSubMenu ("Colour", colours);
    menu.addItem ("Delete " + old.name, [this, old]
    {
        // Found again by value: the markers may have changed while the menu was open.
        const auto& markers = engine.session.getState()->markers;

        if (auto it = std::find (markers.begin(), markers.end(), old); it != markers.end())
            deleteMarker ((int) std::distance (markers.begin(), it));
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

std::vector<juce::uint32> MultitrackView::getSelectedNoteClips() const
{
    std::vector<juce::uint32> ids;

    for (auto& t : shown().tracks)
        for (auto& c : t.noteClips)
            if (isNoteClipSelected (c.id))
                ids.push_back (c.id);

    return ids;
}

void MultitrackView::selectNoteClip (juce::uint32 clipId, bool adding)
{
    if (! adding)
    {
        // One note clip alone (or none, leaving any audio clips selected).
        if (clipId != 0)
        {
            selectedClips.clear();
            selectedClip = 0;
        }

        selectedNoteClips.clear();
        selectedNoteClip = clipId;
    }
    else if (selectedNoteClips.count (clipId) > 0)
    {
        selectedNoteClips.erase (clipId);

        if (selectedNoteClip == clipId)
            selectedNoteClip = selectedNoteClips.empty() ? 0 : *selectedNoteClips.begin();

        repaint();
        return;
    }
    else
    {
        selectedNoteClip = clipId;
    }

    if (clipId != 0)
    {
        selectedNoteClips.insert (clipId);
        lastPickedNotes = true;
    }

    repaint();
}

void MultitrackView::addTrigger (juce::uint32 trackId)
{
    auto next = *engine.session.getState();
    NoteClip hit;
    hit.name = "Hit";
    hit.position = cursor;
    hit.length = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (0.1 * next.sampleRate));
    hit.notes = { { 60, 0, hit.length, 1.0f } };

    if (const auto id = SessionEdits::addNoteClip (next, trackId, std::move (hit)); id != 0)
    {
        engine.session.apply (std::move (next), "Add Trigger");
        selectNoteClip (id);
    }
}

void MultitrackView::showNoteClipMenu (juce::uint32 clipId)
{
    const auto where = shown().findNoteClip (clipId);

    if (! where.has_value())
        return;

    const auto& clip = shown().tracks[(size_t) where->first].noteClips[(size_t) where->second];
    const auto trackId = shown().tracks[(size_t) where->first].id;
    const juce::Component::SafePointer<MultitrackView> safe (this);
    juce::PopupMenu menu;
    menu.addItem ("Piano Roll", onOpenNotes != nullptr, false, [safe, clipId] { if (safe != nullptr && safe->onOpenNotes != nullptr) safe->onOpenNotes (clipId); });
    menu.addItem ("Edit Notes...", onEditNotes != nullptr, false, [safe, trackId, clipId] { if (safe != nullptr && safe->onEditNotes != nullptr) safe->onEditNotes (trackId, clipId); });
    menu.addItem ("Merge with Next", nextNoteClip (clipId) != 0, false, [safe, clipId] { if (safe != nullptr) safe->mergeWithNext (clipId); });
    menu.addItem ("Mute", true, clip.muted, [safe, clipId, muted = clip.muted]
    {
        if (safe == nullptr)
            return;

        auto next = *safe->engine.session.getState();

        if (SessionEdits::setNoteClipMuted (next, clipId, ! muted))
            safe->engine.session.apply (std::move (next), muted ? "Unmute Notes" : "Mute Notes");
    });
    menu.addSeparator();
    menu.addItem ("Remove", [safe, clipId]
    {
        if (safe == nullptr)
            return;

        auto next = *safe->engine.session.getState();

        if (SessionEdits::removeNoteClip (next, clipId))
        {
            safe->selectNoteClip (0);
            safe->engine.session.apply (std::move (next), "Remove Notes");
        }
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void MultitrackView::gatherDragGroups (juce::uint32 exceptClip, juce::uint32 exceptNoteClip)
{
    dragGroup.clear();
    dragNoteGroup.clear();

    for (auto id : selectedClips)
        if (const auto where = shown().findClip (id); id != exceptClip && where.has_value())
            dragGroup.push_back (shown().tracks[(size_t) where->first].clips[(size_t) where->second]);

    for (auto id : selectedNoteClips)
        if (const auto where = shown().findNoteClip (id); id != exceptNoteClip && where.has_value())
            dragNoteGroup.push_back (shown().tracks[(size_t) where->first].noteClips[(size_t) where->second]);
}

void MultitrackView::selectClip (juce::uint32 clipId, bool adding)
{
    // A clip in a group brings the rest of its group with it, into the selection or out of it.
    const auto group = SessionEdits::clipGroup (shown(), clipId);

    if (clipId != 0)
        lastPickedNotes = false;

    if (! adding)
    {
        // One clip alone (or none, leaving any note clips selected).
        selectedClips.clear();
        selectedClip = clipId;

        if (clipId != 0)
        {
            selectedNoteClips.clear();
            selectedNoteClip = 0;
        }
    }
    else if (selectedClips.count (clipId) > 0)
    {
        selectedClips.erase (clipId);

        for (auto id : group)
            selectedClips.erase (id);

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

    selectedClips.insert (group.begin(), group.end());
    repaint();
}

bool MultitrackView::canGroupSelectedClips() const
{
    // Two or more, unless they already make up exactly one group.
    const auto ids = getSelectedClips();

    if (ids.size() < 2)
        return false;

    const auto group = SessionEdits::clipGroup (shown(), ids.front());
    return std::set<juce::uint32> (group.begin(), group.end()) != std::set<juce::uint32> (ids.begin(), ids.end())
        || ! areSelectedClipsGrouped();
}

bool MultitrackView::areSelectedClipsGrouped() const
{
    const auto& s = shown();

    for (auto id : getSelectedClips())
        if (const auto where = s.findClip (id); where.has_value() && s.tracks[(size_t) where->first].clips[(size_t) where->second].groupId != 0)
            return true;

    return false;
}

void MultitrackView::groupSelectedClips()
{
    auto next = *engine.session.getState();

    if (canGroupSelectedClips() && SessionEdits::groupClips (next, getSelectedClips()) != 0)
        engine.session.apply (std::move (next), "Group Clips");
}

void MultitrackView::ungroupSelectedClips()
{
    auto next = *engine.session.getState();

    if (SessionEdits::ungroupClips (next, getSelectedClips()))
        engine.session.apply (std::move (next), "Ungroup Clips");
}

void MultitrackView::selectAllClips()
{
    selectClipsWhere ([] (const SessionTrack&, const SessionClip&) { return true; }, [] (const SessionTrack&, const NoteClip&) { return true; });
}

bool MultitrackView::selectClipsOnTrack()
{
    const auto* track = engine.session.getState()->findTrack (selectedTrackId);

    if (track == nullptr)
        return false;

    selectClipsWhere ([id = track->id] (const SessionTrack& t, const SessionClip&) { return t.id == id; },
                      [id = track->id] (const SessionTrack& t, const NoteClip&) { return t.id == id; });
    return true;
}

void MultitrackView::selectClipsAfterCursor()
{
    selectClipsWhere ([from = cursor] (const SessionTrack&, const SessionClip& c) { return c.position >= from; },
                      [from = cursor] (const SessionTrack&, const NoteClip& c) { return c.position >= from; });
}

bool MultitrackView::selectClipsInRange()
{
    if (timeSelection.isEmpty())
        return false;

    selectClipsWhere ([range = timeSelection] (const SessionTrack&, const SessionClip& c)
                      { return c.position < range.getEnd() && c.getEnd() > range.getStart(); },
                      [range = timeSelection] (const SessionTrack&, const NoteClip& c)
                      { return c.position < range.getEnd() && c.getEnd() > range.getStart(); });
    return true;
}

bool MultitrackView::selectClipsInRegion()
{
    const auto region = MarkerEdits::regionAt (engine.session.getState()->markers, cursor);

    if (! region)
        return false;

    setTimeSelection (*region);
    return selectClipsInRange();
}

void MultitrackView::selectClipsWhere (const std::function<bool (const SessionTrack&, const SessionClip&)>& wanted,
                                       const std::function<bool (const SessionTrack&, const NoteClip&)>& wantedNotes)
{
    // Never clips on hidden or folded-away tracks, which can't be seen.
    selectedClips.clear();
    selectedNoteClips.clear();
    const auto& s = *engine.session.getState();

    for (auto& t : s.tracks)
    {
        if (! s.isShown (t))
            continue;

        for (auto& c : t.clips)
            if (wanted (t, c))
                selectedClips.insert (c.id);

        for (auto& c : t.noteClips)
            if (wantedNotes (t, c))
                selectedNoteClips.insert (c.id);
    }

    if (! isClipSelected (selectedClip))
        selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

    if (! isNoteClipSelected (selectedNoteClip))
        selectedNoteClip = selectedNoteClips.empty() ? 0 : *selectedNoteClips.begin();

    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void MultitrackView::deleteSelectedClip()
{
    // Every selected clip of either kind goes (locked audio clips stay).
    auto next = *engine.session.getState();
    auto removed = 0, notesRemoved = 0;

    for (auto id : getSelectedClips())
        removed += SessionEdits::removeClip (next, id) ? 1 : 0;

    for (auto id : getSelectedNoteClips())
        notesRemoved += SessionEdits::removeNoteClip (next, id) ? 1 : 0;

    if (removed + notesRemoved == 0)
        return;

    selectClip (0, false);
    selectNoteClip (0);
    engine.session.apply (std::move (next), removed + notesRemoved > 1 ? "Delete " + juce::String (removed + notesRemoved) + " Clips"
                                            : removed == 1 ? juce::String ("Delete Clip") : juce::String ("Remove Notes"));
}

bool MultitrackView::copySelectedClip()
{
    // Every selected clip of either kind, each with its track counted from the topmost copied.
    const auto current = engine.session.getState();
    std::vector<std::pair<int, SessionClip>> copied;
    std::vector<std::pair<int, NoteClip>> copiedNotes;
    auto topmost = std::numeric_limits<int>::max();

    for (auto id : getSelectedClips())
    {
        const auto where = current->findClip (id);
        copied.emplace_back (where->first, current->tracks[(size_t) where->first].clips[(size_t) where->second]);
        topmost = juce::jmin (topmost, where->first);
    }

    for (auto id : getSelectedNoteClips())
    {
        const auto where = current->findNoteClip (id);
        copiedNotes.emplace_back (where->first, current->tracks[(size_t) where->first].noteClips[(size_t) where->second]);
        topmost = juce::jmin (topmost, where->first);
    }

    if (copied.empty() && copiedNotes.empty())
        return false;

    for (auto& [track, clip] : copied)
        track -= topmost;

    for (auto& [track, clip] : copiedNotes)
        track -= topmost;

    copiedClips = std::move (copied);
    copiedNoteClips = std::move (copiedNotes);
    copiedRate = current->sampleRate;
    return true;
}

bool MultitrackView::cutSelectedClip()
{
    if (! copySelectedClip())
        return false;

    // Locked clips are copied but stay where they are.
    auto next = *engine.session.getState();
    auto removed = 0, notesRemoved = 0;

    for (auto& copied : copiedClips)
        removed += SessionEdits::removeClip (next, copied.second.id) ? 1 : 0;

    for (auto& copied : copiedNoteClips)
        notesRemoved += SessionEdits::removeNoteClip (next, copied.second.id) ? 1 : 0;

    if (removed + notesRemoved > 0)
    {
        selectClip (0, false);
        selectNoteClip (0);
        engine.session.apply (std::move (next), removed + notesRemoved > 1 ? "Cut " + juce::String (removed + notesRemoved) + " Clips"
                                                : removed == 1 ? juce::String ("Cut Clip") : juce::String ("Cut Notes"));
    }

    return true;
}

bool MultitrackView::canPaste() const
{
    return (! copiedClips.empty() || ! copiedNoteClips.empty()) && juce::exactlyEqual (copiedRate, engine.session.getState()->sampleRate);
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

    // Everything copied goes from the cursor, the earliest clip of either kind at it, each on the
    // selected track plus its own offset; note clips whose track takes no notes go on a new
    // instrument track (one for each offset), as a note clip alone does without a selected track.
    auto earliest = std::numeric_limits<juce::int64>::max();

    for (auto& c : copiedClips)
        earliest = juce::jmin (earliest, c.second.position);

    for (auto& c : copiedNoteClips)
        earliest = juce::jmin (earliest, c.second.position);

    auto next = *engine.session.getState();
    auto firstTrack = getSelectedTrack();

    if (juce::isPositiveAndBelow (firstTrack, (int) next.tracks.size()) && next.tracks[(size_t) firstTrack].isBus)
        firstTrack = -1;

    // Without a selected track all of them go below the last, still a track apart.
    if (firstTrack < 0)
        firstTrack = (int) next.tracks.size();

    const auto shift = cursor - earliest;
    const auto ids = placeClips (next, copiedClips, firstTrack, shift);
    std::vector<juce::uint32> noteIds;
    std::map<int, int> noteTracks;   // by offset: the track its note clips went on

    for (auto& [offset, clip] : copiedNoteClips)
    {
        auto found = noteTracks.find (offset);

        if (found == noteTracks.end())
        {
            const auto target = firstTrack + offset;

            if (target >= (int) next.tracks.size() || ! next.tracks[(size_t) target].takesNoteClips())
                next.addInstrumentTrack();

            found = noteTracks.emplace (offset, target < (int) next.tracks.size() && next.tracks[(size_t) target].takesNoteClips()
                                                    ? target : (int) next.tracks.size() - 1).first;
        }

        noteIds.push_back (SessionEdits::placeNoteClip (next, found->second, clip, clip.position + shift));
    }

    auto end = cursor;

    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        end = juce::jmax (end, next.tracks[(size_t) where->first].clips[(size_t) where->second].getEnd());
    }

    for (auto id : noteIds)
    {
        const auto where = next.findNoteClip (id);
        end = juce::jmax (end, next.tracks[(size_t) where->first].noteClips[(size_t) where->second].getEnd());
    }

    selectedClips = { ids.begin(), ids.end() };
    selectedClip = ids.empty() ? 0 : ids.front();
    selectedNoteClips = { noteIds.begin(), noteIds.end() };
    selectedNoteClip = noteIds.empty() ? 0 : noteIds.front();
    const auto first = ! ids.empty() ? next.findClip (ids.front())->first : next.findNoteClip (noteIds.front())->first;
    selectedTrackId = next.tracks[(size_t) first].id;
    const auto count = (int) (ids.size() + noteIds.size());
    engine.session.apply (std::move (next), count > 1 ? "Paste " + juce::String (count) + " Clips"
                                            : ids.size() == 1 ? juce::String ("Paste Clip") : juce::String ("Paste Notes"));
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
    // The copies go right after the selection's end, each on its own track.
    auto next = *engine.session.getState();
    std::vector<std::pair<int, SessionClip>> clips;
    std::vector<std::pair<int, NoteClip>> noteClips;
    juce::int64 start = std::numeric_limits<juce::int64>::max(), end = 0;

    for (auto id : getSelectedClips())
    {
        const auto where = next.findClip (id);
        const auto& c = next.tracks[(size_t) where->first].clips[(size_t) where->second];
        clips.emplace_back (where->first, c);
        start = juce::jmin (start, c.position);
        end = juce::jmax (end, c.getEnd());
    }

    for (auto id : getSelectedNoteClips())
    {
        const auto where = next.findNoteClip (id);
        const auto& c = next.tracks[(size_t) where->first].noteClips[(size_t) where->second];
        noteClips.emplace_back (where->first, c);
        start = juce::jmin (start, c.position);
        end = juce::jmax (end, c.getEnd());
    }

    if (clips.empty() && noteClips.empty())
        return;

    const auto ids = placeClips (next, clips, 0, end - start);
    std::vector<juce::uint32> noteIds;

    for (auto& [track, clip] : noteClips)
        noteIds.push_back (SessionEdits::placeNoteClip (next, track, clip, clip.position + end - start));

    selectedClips = { ids.begin(), ids.end() };
    selectedClip = ids.empty() ? 0 : ids.front();
    selectedNoteClips = { noteIds.begin(), noteIds.end() };
    selectedNoteClip = noteIds.empty() ? 0 : noteIds.front();
    const auto count = (int) (ids.size() + noteIds.size());
    engine.session.apply (std::move (next), count > 1 ? "Duplicate " + juce::String (count) + " Clips"
                                            : ids.size() == 1 ? juce::String ("Duplicate Clip") : juce::String ("Duplicate Notes"));
}

void MultitrackView::splitSelectedClip()
{
    auto next = *engine.session.getState();

    if (selectedNoteClip != 0)
    {
        if (SessionEdits::splitNoteClip (next, selectedNoteClip, cursor))
            engine.session.apply (std::move (next), "Split Notes");

        return;
    }

    const auto where = next.findClip (selectedClip);

    if (! where.has_value())
        return;

    const auto at = toZeroCrossing (next.tracks[(size_t) where->first].clips[(size_t) where->second], cursor);

    if (SessionEdits::splitClip (next, selectedClip, at))
        engine.session.apply (std::move (next), "Split Clip");
}

juce::int64 MultitrackView::toZeroCrossing (const SessionClip& c, juce::int64 frame) const
{
    if (! zeroCrossings)
        return frame;

    return SessionEdits::zeroCrossingNear (c, frame, (juce::int64) std::llround (zeroCrossingReach * shown().sampleRate));
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
bool MultitrackView::areSelectedClipsMuted() const
{
    const auto ids = getSelectedClips();
    const auto noteIds = getSelectedNoteClips();
    const auto& s = shown();

    if (ids.empty() && noteIds.empty())
        return false;

    return (ids.empty() || allSelected (s, ids, &SessionClip::muted))
        && std::all_of (noteIds.begin(), noteIds.end(), [&s] (auto id)
           {
               const auto where = s.findNoteClip (id);
               return s.tracks[(size_t) where->first].noteClips[(size_t) where->second].muted;
           });
}

void MultitrackView::setSelectedClipsLocked (bool locked)
{
    setSelectedClipsFlag (SessionEdits::setClipLocked, locked, locked ? "Lock " : "Unlock ");
}

void MultitrackView::setSelectedClipsMuted (bool muted)
{
    auto next = *engine.session.getState();
    auto changed = 0;

    for (auto id : getSelectedClips())
        changed += SessionEdits::setClipMuted (next, id, muted) ? 1 : 0;

    for (auto id : getSelectedNoteClips())
        changed += SessionEdits::setNoteClipMuted (next, id, muted) ? 1 : 0;

    if (changed > 0)
        engine.session.apply (std::move (next), juce::String (muted ? "Mute " : "Unmute ") + (changed > 1 ? juce::String (changed) + " Clips" : juce::String ("Clip")));
}

bool MultitrackView::showMarker (int index)
{
    const auto& markers = shown().markers;

    if (! juce::isPositiveAndBelow (index, (int) markers.size()))
        return false;

    const auto m = markers[(size_t) index];
    setTimeSelection (m.isRegion() ? juce::Range<juce::int64> (m.start, m.getEnd()) : juce::Range<juce::int64>());
    showFrame (m.start);
    return true;
}

void MultitrackView::showFrame (juce::int64 frame)
{
    setCursor (frame);

    // A tenth of the way in unless it is in view already.
    const auto width = (double) juce::jmax (1, timelineArea().getWidth()) * framesPerPixel;

    if ((double) frame < viewStart || (double) frame >= viewStart + width)
        setView ((double) frame - width * 0.1, framesPerPixel);
}

bool MultitrackView::deleteMarker (int index)
{
    auto next = *engine.session.getState();

    if (! juce::isPositiveAndBelow (index, (int) next.markers.size()))
        return false;

    const auto name = next.markers[(size_t) index].name;
    next.markers.erase (next.markers.begin() + index);
    engine.session.apply (std::move (next), "Delete " + name);
    return true;
}

bool MultitrackView::selectAdjacentClip (bool forwards)
{
    const auto& s = shown();
    const auto selected = s.findClip (selectedClip);
    const auto trackIndex = selected.has_value() ? selected->first : getSelectedTrack();

    if (! juce::isPositiveAndBelow (trackIndex, (int) s.tracks.size()))
        return false;

    // The track's clips in time order (by id where two start together).
    std::vector<const SessionClip*> clips;

    for (auto& c : s.tracks[(size_t) trackIndex].clips)
        clips.push_back (&c);

    std::sort (clips.begin(), clips.end(), [] (auto* a, auto* b) { return std::tie (a->position, a->id) < std::tie (b->position, b->id); });
    const SessionClip* target = nullptr;

    if (selected.has_value())
    {
        const auto at = std::find_if (clips.begin(), clips.end(), [this] (auto* c) { return c->id == selectedClip; });
        const auto index = (int) std::distance (clips.begin(), at) + (forwards ? 1 : -1);

        if (juce::isPositiveAndBelow (index, (int) clips.size()))
            target = clips[(size_t) index];
    }
    else if (forwards)
    {
        const auto it = std::find_if (clips.begin(), clips.end(), [this] (auto* c) { return c->position >= cursor; });
        target = it != clips.end() ? *it : nullptr;
    }
    else
    {
        const auto it = std::find_if (clips.rbegin(), clips.rend(), [this] (auto* c) { return c->position < cursor; });
        target = it != clips.rend() ? *it : nullptr;
    }

    return target != nullptr && showClip (target->id);
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

    if (const auto t = shown().tracks[(size_t) where->first]; ! shown().isShown (t))
    {
        const auto* folder = shown().findTrack (t.folderId);
        const auto folderId = folder != nullptr && folder->collapsed ? folder->id : 0;

        if (t.hidden)
            setTrackHidden (t.id, false);

        if (folderId != 0)
            setFolderCollapsed (folderId, false);

        changeListenerCallback (&engine.session);   // lay the lanes out with it now
    }

    scrollToTrack (where->first);
    return true;
}

void MultitrackView::scrollToTrack (int track)
{
    const auto visible = juce::jmax (1, timelineArea().getHeight());
    const auto top = rowOf (track) * laneHeight;

    if (top < scrollY || top + laneHeight > scrollY + visible)
        scrollY = top;

    updateScrollBars();   // lays the headers out too
    repaint();
}

bool MultitrackView::selectAdjacentTrack (bool down)
{
    const auto& s = shown();
    std::vector<int> visible;

    for (int i = 0; i < (int) s.tracks.size(); ++i)
        if (s.isShown (s.tracks[(size_t) i]))
            visible.push_back (i);

    if (visible.empty())
        return false;

    const auto at = std::find (visible.begin(), visible.end(), getSelectedTrack());
    int target;

    if (at == visible.end())
        target = down ? visible.front() : visible.back();
    else if (down ? std::next (at) != visible.end() : at != visible.begin())
        target = down ? *std::next (at) : *std::prev (at);
    else
        return false;

    selectTrack (target);
    scrollToTrack (target);
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

void MultitrackView::setSelectedClipsFadeShape (bool fadeOut, FadeShape shape)
{
    auto next = *engine.session.getState();
    auto changed = false;

    for (auto id : getSelectedClips())
        changed = SessionEdits::setClipFadeShape (next, id, fadeOut, shape) || changed;

    if (changed)
        engine.session.apply (std::move (next), fadeOut ? "Fade Out Shape" : "Fade In Shape");
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

juce::uint32 MultitrackView::addFolder()
{
    auto next = *engine.session.getState();
    const auto folderId = next.addFolder().id;
    const auto selected = std::find_if (next.tracks.begin(), next.tracks.end(), [this] (auto& t) { return t.id == selectedTrackId; });

    if (selected != next.tracks.end() && ! selected->isFolder)
    {
        const auto to = (int) std::distance (next.tracks.begin(), selected);
        SessionEdits::setTrackFolder (next, selectedTrackId, folderId);
        SessionEdits::moveTrack (next, (int) next.tracks.size() - 1, to);
    }

    engine.session.apply (std::move (next), "Add Folder");
    selectedTrackId = folderId;
    return folderId;
}

void MultitrackView::setFolderCollapsed (juce::uint32 folderId, bool collapsed)
{
    const auto* folder = engine.session.getState()->findTrack (folderId);

    if (folder != nullptr && folder->isFolder && folder->collapsed != collapsed)
        editTrack (folderId, collapsed ? "Collapse Folder" : "Expand Folder", false, [collapsed] (SessionTrack& t) { t.collapsed = collapsed; });
}

void MultitrackView::setTrackFolder (juce::uint32 trackId, juce::uint32 folderId)
{
    auto next = *engine.session.getState();

    if (SessionEdits::setTrackFolder (next, trackId, folderId))
        engine.session.apply (std::move (next), folderId != 0 ? "Move to Folder" : "Take Out of Folder");
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

int MultitrackView::quantizeSelectedClips()
{
    auto next = *engine.session.getState();
    const auto targets = snapTargets (0, false);
    const auto map = next.tempoMap();

    const auto nearest = [&] (juce::int64 frame)
    {
        if (targets.beatGrid.has_value())
        {
            const auto [below, above] = map.linesAround (frame, *targets.beatGrid);
            return frame - below <= above - frame ? below : above;
        }

        const auto step = juce::jmax ((juce::int64) 1, targets.gridStep);
        return (juce::int64) std::llround ((double) frame / (double) step) * step;
    };
    const auto moved = SessionEdits::quantizeClips (next, getSelectedClips(), nearest)
                     + SessionEdits::quantizeNoteClips (next, getSelectedNoteClips(), nearest);

    if (moved > 0)
        engine.session.apply (std::move (next), moved > 1 ? "Quantize " + juce::String (moved) + " Clips" : juce::String ("Quantize Clip"));

    return moved;
}

int MultitrackView::transposeSelectedNotes (int semitones)
{
    auto next = *engine.session.getState();
    const auto changed = SessionEdits::transposeNotes (next, getSelectedNoteClips(), semitones);

    if (changed > 0)
        engine.session.apply (std::move (next), "Transpose Notes");

    return changed;
}

int MultitrackView::setSelectedNotesVelocity (int to, double scale)
{
    auto next = *engine.session.getState();
    const auto changed = SessionEdits::setNotesVelocity (next, getSelectedNoteClips(), to, scale);

    if (changed > 0)
        engine.session.apply (std::move (next), "Note Velocity");

    return changed;
}

int MultitrackView::quantizeSelectedNotes (int division)
{
    auto next = *engine.session.getState();
    const auto map = next.tempoMap();
    const auto steps = (double) juce::jmax (1, division);
    const auto changed = SessionEdits::quantizeNotes (next, getSelectedNoteClips(), [&map, steps] (juce::int64 frame)
    {
        return map.frameAtBeatPosition (std::round (map.beatPosition (frame) * steps) / steps);
    });

    if (changed > 0)
        engine.session.apply (std::move (next), "Quantize Notes");

    return changed;
}

bool MultitrackView::clearTimeSelection()
{
    auto next = *engine.session.getState();

    if (timeSelection.isEmpty() || ! SessionEdits::clearTime (next, timeSelection.getStart(), timeSelection.getEnd()))
        return false;

    engine.session.apply (std::move (next), "Clear Range");
    return true;
}

bool MultitrackView::deleteTimeSelection()
{
    auto next = *engine.session.getState();
    const auto range = timeSelection;

    if (range.isEmpty() || ! SessionEdits::deleteTime (next, range.getStart(), range.getEnd()))
        return false;

    engine.session.apply (std::move (next), "Delete Time");
    setTimeSelection ({});
    setCursor (range.getStart());
    return true;
}

bool MultitrackView::insertTimeAtSelection()
{
    auto next = *engine.session.getState();

    if (timeSelection.isEmpty() || ! SessionEdits::insertTime (next, timeSelection.getStart(), timeSelection.getLength()))
        return false;

    engine.session.apply (std::move (next), "Insert Time");
    return true;
}

bool MultitrackView::copyTimeSelection()
{
    if (timeSelection.isEmpty())
        return false;

    copiedTime = SessionEdits::copyTime (*engine.session.getState(), timeSelection.getStart(), timeSelection.getEnd());
    return true;
}

bool MultitrackView::cutTimeSelection()
{
    const auto range = timeSelection;

    if (! copyTimeSelection())
        return false;

    auto next = *engine.session.getState();

    if (SessionEdits::deleteTime (next, range.getStart(), range.getEnd()))
    {
        engine.session.apply (std::move (next), "Cut Time");
        setTimeSelection ({});
        setCursor (range.getStart());
    }

    return true;
}

bool MultitrackView::canPasteTime() const
{
    return copiedTime.length > 0 && juce::exactlyEqual (copiedTime.sampleRate, engine.session.getState()->sampleRate);
}

bool MultitrackView::pasteTime()
{
    auto next = *engine.session.getState();
    const auto at = cursor;

    if (! canPasteTime() || ! SessionEdits::pasteTime (next, copiedTime, at))
        return false;

    engine.session.apply (std::move (next), "Paste Time");
    setTimeSelection ({ at, at + copiedTime.length });
    setCursor (at + copiedTime.length);
    return true;
}

bool MultitrackView::duplicateTimeSelection()
{
    const auto range = timeSelection;
    auto next = *engine.session.getState();

    if (range.isEmpty() || ! SessionEdits::pasteTime (next, SessionEdits::copyTime (next, range.getStart(), range.getEnd()), range.getEnd()))
        return false;

    engine.session.apply (std::move (next), "Duplicate Time");
    setTimeSelection (range + range.getLength());
    setCursor (range.getEnd());
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
        // The marks either side; one on the frame counts as before it.
        const auto map = s.tempoMap();
        const auto grid = rulerBeats().second;
        const auto [below, above] = map.linesAround (frame, grid);
        return juce::jmax ((juce::int64) 0, later ? above : below < frame ? below : map.linesAround (below - 1, grid).first);
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
    const auto noteIds = getSelectedNoteClips();

    if (ids.empty() && noteIds.empty())
    {
        setCursor (gridStepFrom (cursor, later));
        return;
    }

    // By the step from the clip clicked last (audio or notes); the rest keep their distance from it.
    const auto current = engine.session.getState();
    const auto& s = *current;
    juce::int64 start = 0;

    if (const auto primary = s.findClip (isClipSelected (selectedClip) ? selectedClip : ids.empty() ? 0 : ids.front()))
        start = s.tracks[(size_t) primary->first].clips[(size_t) primary->second].position;
    else if (const auto notes = s.findNoteClip (isNoteClipSelected (selectedNoteClip) ? selectedNoteClip : noteIds.front()))
        start = s.tracks[(size_t) notes->first].noteClips[(size_t) notes->second].position;

    moveSelectedClips (gridStepFrom (start, later) - start, ids.size() + noteIds.size() > 1 ? "Nudge Clips" : "Nudge Clip", true);
}

bool MultitrackView::moveSelectedClipsToCursor()
{
    const auto ids = getSelectedClips();
    const auto noteIds = getSelectedNoteClips();
    const auto current = engine.session.getState();
    const auto& s = *current;
    auto earliest = std::numeric_limits<juce::int64>::max();

    for (auto id : ids)
    {
        const auto where = s.findClip (id);
        earliest = juce::jmin (earliest, s.tracks[(size_t) where->first].clips[(size_t) where->second].position);
    }

    for (auto id : noteIds)
    {
        const auto where = s.findNoteClip (id);
        earliest = juce::jmin (earliest, s.tracks[(size_t) where->first].noteClips[(size_t) where->second].position);
    }

    return (! ids.empty() || ! noteIds.empty())
        && moveSelectedClips (cursor - earliest, ids.size() + noteIds.size() > 1 ? "Move Clips to Cursor" : "Move Clip to Cursor", false);
}

int MultitrackView::trimSelectedClipsToCursor (bool start)
{
    auto next = *engine.session.getState();
    auto trimmed = 0;

    for (auto id : getSelectedClips())
    {
        const auto where = next.findClip (id);
        const auto c = next.tracks[(size_t) where->first].clips[(size_t) where->second];

        if (cursor > c.position && cursor < c.getEnd()
            && SessionEdits::trimClip (next, id, start ? cursor : c.position, start ? c.getEnd() : cursor))
            ++trimmed;
    }

    if (trimmed > 0)
        engine.session.apply (std::move (next), juce::String (start ? "Trim Start" : "Trim End") + (trimmed > 1 ? " of Clips" : "") + " to Cursor");

    return trimmed;
}

bool MultitrackView::moveSelectedClips (juce::int64 delta, const juce::String& description, bool merge)
{
    const auto ids = getSelectedClips();
    const auto noteIds = getSelectedNoteClips();
    auto next = *engine.session.getState();
    auto earliest = std::numeric_limits<juce::int64>::max();

    // The clips, audio and notes, move together or not at all, and none before the session's start.
    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        const auto& c = next.tracks[(size_t) where->first].clips[(size_t) where->second];

        if (c.locked)
            return false;

        earliest = juce::jmin (earliest, c.position);
    }

    for (auto id : noteIds)
    {
        const auto where = next.findNoteClip (id);
        earliest = juce::jmin (earliest, next.tracks[(size_t) where->first].noteClips[(size_t) where->second].position);
    }

    delta = juce::jmax (delta, -earliest);

    if ((ids.empty() && noteIds.empty()) || delta == 0)
        return false;

    for (auto id : ids)
    {
        const auto where = next.findClip (id);
        SessionEdits::moveClip (next, id, where->first, next.tracks[(size_t) where->first].clips[(size_t) where->second].position + delta);
    }

    for (auto id : noteIds)
    {
        const auto where = next.findNoteClip (id);
        SessionEdits::moveNoteClip (next, id, where->first, next.tracks[(size_t) where->first].noteClips[(size_t) where->second].position + delta);
    }

    engine.session.apply (std::move (next), description, merge);
    return true;
}

void MultitrackView::setTrackColour (juce::uint32 trackId, int colour)
{
    editTrack (trackId, "Track Colour", false,
               [colour] (SessionTrack& t) { t.colour = juce::isPositiveAndBelow (colour, (int) TrackColours::palette.size()) ? colour : -1; });
}

void MultitrackView::setTrackFiveOne (juce::uint32 trackId, bool fiveOne)
{
    setTrackChannels (trackId, fiveOne ? 6 : 2);
}

void MultitrackView::setTrackChannels (juce::uint32 trackId, int channels)
{
    auto next = *engine.session.getState();

    if (SessionEdits::setTrackChannels (next, trackId, channels))
        engine.session.apply (std::move (next), channels == 8 ? "7.1 Track" : channels == 6 ? "5.1 Track" : "Stereo Track");
}

void MultitrackView::setTrackPolarityInverted (juce::uint32 trackId, bool inverted)
{
    editTrack (trackId, inverted ? "Invert Polarity" : "Restore Polarity", false, [inverted] (SessionTrack& t) { t.invertPolarity = inverted; });
}

bool MultitrackView::clearAllSolos()
{
    auto next = *engine.session.getState();
    auto changed = false;

    for (auto& t : next.tracks)
        changed = std::exchange (t.solo, false) || changed;

    if (changed)
        engine.session.apply (std::move (next), "Unsolo All");

    return changed;
}

bool MultitrackView::clearAllMutes()
{
    auto next = *engine.session.getState();
    auto changed = false;

    for (auto& t : next.tracks)
        changed = std::exchange (t.mute, false) || changed;

    if (changed)
        engine.session.apply (std::move (next), "Unmute All");

    return changed;
}

void MultitrackView::setTrackSoloSafe (juce::uint32 trackId, bool safe)
{
    editTrack (trackId, safe ? "Solo Safe" : "Not Solo Safe", false, [safe] (SessionTrack& t) { t.soloSafe = safe; });
}

void MultitrackView::setTrackGroup (juce::uint32 trackId, int group)
{
    editTrack (trackId, group == 0 ? juce::String ("Ungroup Track") : "Group " + juce::String (group), false,
               [group] (SessionTrack& t) { t.group = juce::jlimit (0, SessionTrack::maxGroups, group); });
}

void MultitrackView::updateHeaderMeters()
{
    const auto& tracks = shown().tracks;
    auto& mix = engine.getPlayer().getLevels();

    for (size_t i = 0; i < headers.size() && i < tracks.size(); ++i)
    {
        const auto& t = tracks[i];

        if (t.isFolder)
            continue;

        // The headers read their own copy of the peaks (reader 1), so the mixer's and the input
        // meter's stay theirs.
        if (! t.isBus && isArmed (t.id))
        {
            if (auto* recorder = engine.getRecorderFor (t.input); recorder != nullptr && recorder->isInputActive())
            {
                const auto left = recorder->takePeak (0, 1);
                headers[i]->pushLevels (left, recorder->getMappedInput().second == 2 ? recorder->takePeak (1, 1) : left, true);
                continue;
            }

            headers[i]->pushLevels (0.0f, 0.0f, true);
            continue;
        }

        const auto [left, right] = mix.take ((int) i, 1);
        headers[i]->pushLevels (left, right, false);
    }
}

float MultitrackView::getHeaderLevel (juce::uint32 trackId, int channel) const
{
    for (auto& h : headers)
        if (h->trackId == trackId)
            return h->getLevel (channel);

    return 0.0f;
}

float MultitrackView::getHeaderHold (juce::uint32 trackId, int channel) const
{
    for (auto& h : headers)
        if (h->trackId == trackId)
            return h->getHold (channel);

    return 0.0f;
}

bool MultitrackView::hasHeaderClipped (juce::uint32 trackId) const
{
    for (auto& h : headers)
        if (h->trackId == trackId)
            return h->hasClipped();

    return false;
}

bool MultitrackView::isHeaderShowingInput (juce::uint32 trackId) const
{
    for (auto& h : headers)
        if (h->trackId == trackId)
            return h->isShowingInput();

    return false;
}

bool MultitrackView::dropsIntoFolder (int from, int to) const
{
    const auto& tracks = shown().tracks;
    return juce::isPositiveAndBelow (from, (int) tracks.size()) && juce::isPositiveAndBelow (to, (int) tracks.size())
        && tracks[(size_t) to].isFolder && ! tracks[(size_t) from].isFolder;
}

void MultitrackView::dropTrack (int from, int to)
{
    auto next = *engine.session.getState();
    const auto count = (int) next.tracks.size();

    if (! juce::isPositiveAndBelow (from, count) || ! juce::isPositiveAndBelow (to, count)
        || ! next.tracks[(size_t) to].isFolder || next.tracks[(size_t) from].isFolder)
    {
        moveTrack (from, to);
        return;
    }

    // Just below the folder: where it is now, once the track has left its place above it.
    const auto trackId = next.tracks[(size_t) from].id;
    const auto joined = SessionEdits::setTrackFolder (next, trackId, next.tracks[(size_t) to].id);
    const auto below = from < to ? to : to + 1;
    const auto moved = SessionEdits::moveTrack (next, from, below);

    if (! joined && ! moved)
        return;

    selectedTrackId = trackId;
    engine.session.apply (std::move (next), "Move to Folder");
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

    // Dragging along the time ruler while stopped scrubs: the mix under the mouse is heard as it moves.
    if (p.y < timeRulerHeight && ! e.mods.isPopupMenu() && ! engine.getPlayer().isPlaying()
        && engine.getPlayer().getMode() == DocumentPlayerProcessor::Mode::session)
    {
        const auto frame = juce::jlimit ((juce::int64) 0, shown().getLength(), frameForX ((float) p.x));
        drag = Drag::scrub;
        engine.getPlayer().getScrubber().start (frame);
        setCursor (frame);
        setTimeSelection ({});
        return;
    }

    if (p.y < rulerHeight)
    {
        drag = Drag::range;
        dragFrame = snapped (frameForX ((float) p.x), 0, false);
        setCursor (dragFrame);
        setTimeSelection ({});
        return;
    }

    // The Split tool cuts the clip under the mouse; Glue joins a note clip with the next.
    if (! e.mods.isPopupMenu() && tool == Tool::split)
    {
        splitAt (p);
        return;
    }

    if (! e.mods.isPopupMenu() && tool == Tool::glue)
    {
        if (const auto hit = noteClipAt (p); hit.has_value() && ! clipAt (p).has_value())
        {
            const auto id = shown().tracks[(size_t) hit->first].noteClips[(size_t) hit->second].id;
            selectNoteClip (id);
            selectTrack (hit->first);
            mergeWithNext (id);
        }

        return;
    }

    // The Range tool: a drag anywhere across the lanes selects a time range; the clips stay as they are.
    if (! e.mods.isPopupMenu() && tool == Tool::range)
    {
        drag = Drag::range;
        dragFrame = snapped (frameForX ((float) p.x), 0, false);
        setCursor (dragFrame);
        setTimeSelection ({});
        selectTrack (trackAt (p.y));
        return;
    }

    // The Mute tool: a click on a clip, audio or notes, silences it or lets it be heard again.
    if (! e.mods.isPopupMenu() && tool == Tool::mute)
    {
        auto next = *engine.session.getState();

        if (const auto hit = noteClipAt (p); hit.has_value() && ! clipAt (p).has_value())
        {
            const auto& c = shown().tracks[(size_t) hit->first].noteClips[(size_t) hit->second];
            const auto muted = c.muted;

            if (SessionEdits::setNoteClipMuted (next, c.id, ! muted))
                engine.session.apply (std::move (next), muted ? "Unmute Notes" : "Mute Notes");
        }
        else if (const auto audio = clipAt (p))
        {
            const auto& c = shown().tracks[(size_t) audio->first].clips[(size_t) audio->second];
            const auto muted = c.muted;

            if (SessionEdits::setClipMuted (next, c.id, ! muted))
                engine.session.apply (std::move (next), muted ? "Unmute Clip" : "Mute Clip");
        }

        return;
    }

    // A note clip: right-click for its menu, a click selects it, a drag moves it in time (with
    // the Stretch tool, a drag on an edge stretches it).
    if (const auto hit = noteClipAt (p); hit.has_value() && ! clipAt (p).has_value())
    {
        const auto& clip = shown().tracks[(size_t) hit->first].noteClips[(size_t) hit->second];

        if (e.mods.isShiftDown() && ! e.mods.isPopupMenu())
        {
            // Shift-click adds the clip to the selection or takes it out; no drag.
            selectNoteClip (clip.id, true);
            selectTrack (hit->first);
            return;
        }

        // A click on a selected clip keeps the selection, to drag it all.
        if (! isNoteClipSelected (clip.id))
            selectNoteClip (clip.id);

        selectedNoteClip = clip.id;
        lastPickedNotes = true;
        selectTrack (hit->first);

        if (e.mods.isPopupMenu())
        {
            showNoteClipMenu (clip.id);
            return;
        }

        const auto r = noteClipBounds (hit->first, clip);
        const auto stretching = tool == Tool::stretch;
        dragNoteClip = clip;
        dragTrack = hit->first;
        dragFrame = frameForX ((float) p.x);
        drag = p.x <= r.getX() + edgeGrab && r.getWidth() > 3 * edgeGrab ? (stretching ? Drag::stretchNotesStart : Drag::trimNotesStart)
             : p.x >= r.getRight() - edgeGrab && r.getWidth() > 3 * edgeGrab ? (stretching ? Drag::stretchNotesEnd : Drag::trimNotesEnd)
             : Drag::moveNotes;
        stretchTo = {};
        dragDescription = drag == Drag::moveNotes ? "Move Notes" : stretching ? "Stretch Notes" : "Trim Notes";
        gatherDragGroups (0, clip.id);

        // A locked audio clip among the selected keeps them all where they are.
        if (drag == Drag::moveNotes && std::any_of (dragGroup.begin(), dragGroup.end(), [] (auto& c) { return c.locked; }))
            drag = Drag::none;

        return;
    }

    // The Draw tool: a drag along an instrument track's lane (or below the tracks) makes a note clip.
    if (! e.mods.isPopupMenu() && tool == Tool::draw && ! clipAt (p).has_value())
    {
        const auto t = trackAt (p.y);

        if (t < 0 || (t < (int) shown().tracks.size() && ! shown().tracks[(size_t) t].takesNoteClips()))
            return;

        selectClip (0, false);
        selectNoteClip (0);
        selectTrack (t);
        drawTrack = t;
        drawStart = drawEnd = snapped (frameForX ((float) p.x), 0, false);
        drag = Drag::drawNotes;
        repaint();
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

    if (const auto hit = clipGainAt (p))
    {
        const auto& clip = shown().tracks[(size_t) hit->track].clips[(size_t) hit->clip];
        auto envelope = clip.gainEnvelope;
        selectTrack (hit->track);

        if (hit->point >= 0 && e.mods.isAltDown())
        {
            envelope.remove (hit->point);
            auto next = *engine.session.getState();
            SessionEdits::setClipGainEnvelope (next, clip.id, std::move (envelope));
            engine.session.apply (std::move (next), "Delete Clip Gain Point");
            return;
        }

        dragClip = clip;
        dragClipGain = *hit;
        dragDescription = "Move Clip Gain Point";

        // A click on the line adds a point there, at the value the line has, and drags it.
        if (hit->point < 0)
        {
            const auto frame = clipAudioFrameForX (clip, (float) p.x);
            dragClipGain.point = envelope.add (frame, envelope.valueAt (frame));
            dragDescription = "Add Clip Gain Point";

            auto next = *engine.session.getState();
            SessionEdits::setClipGainEnvelope (next, clip.id, envelope);
            preview = std::move (next);
        }

        dragEnvelopeBase = std::move (envelope);
        drag = Drag::clipGainPoint;
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
        lastPickedNotes = false;
        dragClip = clip;
        gatherDragGroups (clip.id, 0);
        dragTrack = hit->first;
        dragFrame = frameForX ((float) p.x);
        drag = e.mods.isCommandDown() ? Drag::clipGain : fadeHandleAt (p, hit->first, clip);

        if (drag == Drag::none)
            drag = p.x <= r.getX() + edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimStart
                 : p.x >= r.getRight() - edgeGrab && r.getWidth() > 3 * edgeGrab ? Drag::trimEnd
                 : Drag::move;

        // With Option, or the Stretch tool, an edge stretches the clip instead of trimming it.
        if ((e.mods.isAltDown() || tool == Tool::stretch) && (drag == Drag::trimStart || drag == Drag::trimEnd))
            drag = drag == Drag::trimStart ? Drag::stretchStart : Drag::stretchEnd;

        stretchTo = {};

        // A locked clip is only selected; so is a group being moved with one in it.
        if (clip.locked || (drag == Drag::move && std::any_of (dragGroup.begin(), dragGroup.end(), [] (auto& c) { return c.locked; })))
            drag = Drag::none;

        dragDescription = drag == Drag::move ? "Move Clip" : drag == Drag::clipGain ? "Clip Gain" : drag == Drag::fadeIn ? "Clip Fade In"
                        : drag == Drag::fadeOut ? "Clip Fade Out" : drag == Drag::stretchStart || drag == Drag::stretchEnd ? "Stretch Clip"
                        : "Trim Clip";
        selectTrack (hit->first);
        return;
    }

    // Empty lane space: a click sets the cursor (and drops the selected audio clips); a drag
    // rubber-bands the clips it crosses, with Shift adding them to the selection it began with.
    if (! e.mods.isShiftDown())
        selectClip (0, false);

    // A band replaces the selection, note clips included (Shift adds to it); a click alone
    // leaves the note clips as they were.
    rubberClips = e.mods.isShiftDown() ? selectedClips : std::set<juce::uint32>();
    rubberNoteClips = e.mods.isShiftDown() ? selectedNoteClips : std::set<juce::uint32>();
    rubberStart = p;
    rubberBand = {};
    drag = Drag::rubberBand;
    dragFrame = snapped (frameForX ((float) p.x), 0, false);
    setCursor (dragFrame);
    setTimeSelection ({});
    selectTrack (trackAt (p.y));
}

void MultitrackView::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (drag == Drag::scrub)
    {
        const auto frame = juce::jlimit ((juce::int64) 0, shown().getLength(), frameForX ((float) juce::jmax (headerWidth, p.x)));
        engine.getPlayer().getScrubber().moveTo (frame);
        setCursor (frame);
        return;
    }

    if (drag == Drag::rubberBand)
    {
        // The band from where the mouse went down to here: the clips it crosses (a grouped clip
        // with its group) join the selection it began with.
        const auto lanes = juce::Rectangle<int> (headerWidth, rulerHeight, juce::jmax (0, getWidth() - headerWidth), juce::jmax (0, getHeight() - rulerHeight));
        rubberBand = juce::Rectangle<int>::leftTopRightBottom (juce::jmin (rubberStart.x, p.x), juce::jmin (rubberStart.y, p.y),
                                                               juce::jmax (rubberStart.x, p.x), juce::jmax (rubberStart.y, p.y))
                         .getIntersection (lanes);
        selectedClips = rubberClips;
        selectedNoteClips = rubberNoteClips;
        const auto& s = shown();

        for (int t = 0; t < (int) s.tracks.size(); ++t)
        {
            if (! s.isShown (s.tracks[(size_t) t]))
                continue;

            for (auto& c : s.tracks[(size_t) t].clips)
                if (clipBounds (t, c).intersects (rubberBand))
                {
                    selectedClips.insert (c.id);
                    const auto group = SessionEdits::clipGroup (s, c.id);
                    selectedClips.insert (group.begin(), group.end());
                }

            for (auto& c : s.tracks[(size_t) t].noteClips)
                if (noteClipBounds (t, c).intersects (rubberBand))
                    selectedNoteClips.insert (c.id);
        }

        if (! isClipSelected (selectedClip))
            selectedClip = selectedClips.empty() ? 0 : *selectedClips.begin();

        if (! isNoteClipSelected (selectedNoteClip))
            selectedNoteClip = selectedNoteClips.empty() ? 0 : *selectedNoteClips.begin();

        repaint();
        return;
    }

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

    if (drag == Drag::drawNotes)
    {
        drawEnd = snapped (frameForX ((float) juce::jmax (headerWidth, p.x)), 0, false);
        repaint();
        return;
    }

    if (drag == Drag::none || e.mouseWasDraggedSinceMouseDown() == false)
        return;

    const auto delta = frameForX ((float) p.x) - dragFrame;

    // Stretching notes: only where they would play is shown; the notes are scaled once let go.
    if (drag == Drag::stretchNotesStart || drag == Drag::stretchNotesEnd)
    {
        const auto end = dragNoteClip.getEnd(), start = dragNoteClip.position;
        const auto shortest = juce::jmax ((juce::int64) 1, (juce::int64) std::ceil ((double) dragNoteClip.length / 16.0));
        const auto longest = dragNoteClip.length * 16;

        if (drag == Drag::stretchNotesEnd)
            stretchTo = { start, start + juce::jlimit (shortest, longest, snapped (end + delta) - start) };
        else
            stretchTo = { end - juce::jlimit (juce::jmin (shortest, end), juce::jmin (longest, end), end - snapped (start + delta)), end };

        repaint();
        return;
    }

    // Stretching: only where it would play is shown; the audio is stretched once let go.
    if (drag == Drag::stretchStart || drag == Drag::stretchEnd)
    {
        const auto end = dragClip.getEnd(), start = dragClip.position;
        const auto shortest = (juce::int64) std::ceil ((double) dragClip.length * TimeStretch::shortest);
        const auto longest = (juce::int64) std::floor ((double) dragClip.length * TimeStretch::longest);

        if (drag == Drag::stretchEnd)
            stretchTo = { start, start + juce::jlimit (shortest, longest, snapped (end + delta, dragClip.id) - start) };
        else
            stretchTo = { end - juce::jlimit (juce::jmin (shortest, end), juce::jmin (longest, end), end - snapped (start + delta, dragClip.id)), end };

        repaint();
        return;
    }

    auto next = *engine.session.getState();

    switch (drag)
    {
        case Drag::move:
        {
            // With other clips selected, they all move in time by the same amount and keep their tracks.
            auto earliest = dragClip.position;

            for (auto& c : dragGroup)
                earliest = juce::jmin (earliest, c.position);

            for (auto& c : dragNoteGroup)
                earliest = juce::jmin (earliest, c.position);

            const auto target = dragGroup.empty() && dragNoteGroup.empty() ? juce::jlimit (0, (int) next.tracks.size(), trackAt (juce::jmax (rulerHeight, p.y)))
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

            for (auto& c : dragNoteGroup)
                if (const auto where = next.findNoteClip (c.id))
                    SessionEdits::moveNoteClip (next, c.id, where->first, c.position + position - dragClip.position);

            break;
        }
        case Drag::moveNotes:
        {
            // With other clips selected, they all move in time by the same amount and keep their tracks.
            auto earliest = dragNoteClip.position;

            for (auto& c : dragNoteGroup)
                earliest = juce::jmin (earliest, c.position);

            for (auto& c : dragGroup)
                earliest = juce::jmin (earliest, c.position);

            const auto alone = dragNoteGroup.empty() && dragGroup.empty();
            const auto target = alone ? juce::jlimit (0, (int) next.tracks.size(), trackAt (juce::jmax (rulerHeight, p.y))) : dragTrack;
            auto position = dragNoteClip.position + juce::jmax (delta, -earliest);

            if (snapping)
                position = juce::jmax (dragNoteClip.position - earliest,
                                       position + SessionSnap::snapSpan (*state, { position, position + dragNoteClip.length }, snapTolerance(), snapTargets (0, true)));

            SessionEdits::moveNoteClip (next, dragNoteClip.id, target, position);

            for (auto& c : dragNoteGroup)
                if (const auto where = next.findNoteClip (c.id))
                    SessionEdits::moveNoteClip (next, c.id, where->first, c.position + position - dragNoteClip.position);

            for (auto& c : dragGroup)
                if (const auto where = next.findClip (c.id))
                    SessionEdits::moveClip (next, c.id, where->first, c.position + position - dragNoteClip.position);

            break;
        }
        case Drag::trimNotesStart:
            SessionEdits::trimNoteClip (next, dragNoteClip.id, snapped (dragNoteClip.position + delta), dragNoteClip.getEnd());
            break;
        case Drag::trimNotesEnd:
            SessionEdits::trimNoteClip (next, dragNoteClip.id, dragNoteClip.position, snapped (dragNoteClip.getEnd() + delta));
            break;
        case Drag::trimStart:
            SessionEdits::trimClip (next, dragClip.id, toZeroCrossing (dragClip, snapped (dragClip.position + delta, dragClip.id)), dragClip.getEnd());
            break;
        case Drag::trimEnd:
            SessionEdits::trimClip (next, dragClip.id, dragClip.position, toZeroCrossing (dragClip, snapped (dragClip.getEnd() + delta, dragClip.id)));
            break;
        case Drag::fadeIn:
            SessionEdits::setClipFades (next, dragClip.id, snapped (frameForX ((float) p.x), dragClip.id) - dragClip.position, dragClip.fadeOut);
            break;
        case Drag::fadeOut:
            SessionEdits::setClipFades (next, dragClip.id, dragClip.fadeIn, dragClip.getEnd() - snapped (frameForX ((float) p.x), dragClip.id));
            break;
        case Drag::clipGainPoint:
        {
            // Kept within the part of the clip heard.
            auto envelope = dragEnvelopeBase;
            envelope.move (dragClipGain.point, clipAudioFrameForX (dragClip, (float) p.x), clipGainForY (clipGainArea (dragClipGain.track, dragClip), (float) p.y));
            SessionEdits::setClipGainEnvelope (next, dragClip.id, std::move (envelope));
            break;
        }
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
        case Drag::rubberBand:
        case Drag::scrub:
        case Drag::stretchStart:
        case Drag::stretchEnd:
        case Drag::stretchNotesStart:
        case Drag::stretchNotesEnd:
        case Drag::drawNotes:
            break;
    }

    if (crossfading && (drag == Drag::move || drag == Drag::trimStart || drag == Drag::trimEnd))
    {
        SessionEdits::crossfade (next, dragClip.id);

        if (drag == Drag::move)
            for (auto& c : dragGroup)
                SessionEdits::crossfade (next, c.id);
    }

    if (crossfading && drag == Drag::moveNotes)
        for (auto& c : dragGroup)
            SessionEdits::crossfade (next, c.id);

    preview = std::move (next);
    rebuildHeaders();
    repaint();
}

void MultitrackView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::scrub)
        engine.getPlayer().getScrubber().stop();

    if ((drag == Drag::stretchStart || drag == Drag::stretchEnd) && ! stretchTo.isEmpty())
    {
        const auto to = std::exchange (stretchTo, {});

        if (to.getLength() != dragClip.length && onStretchClip != nullptr)
            onStretchClip (dragClip.id, (double) to.getLength() / (double) dragClip.length, drag == Drag::stretchStart);

        repaint();
    }

    if ((drag == Drag::stretchNotesStart || drag == Drag::stretchNotesEnd) && ! stretchTo.isEmpty())
    {
        const auto to = std::exchange (stretchTo, {});

        if (to.getLength() != dragNoteClip.length)
        {
            auto next = *engine.session.getState();

            if (SessionEdits::stretchNoteClip (next, dragNoteClip.id, (double) to.getLength() / (double) dragNoteClip.length, drag == Drag::stretchNotesStart))
                engine.session.apply (std::move (next), "Stretch Notes");
        }

        repaint();
    }

    // A drawn note clip: from where the drag began to where it ended, on the track it was drawn on.
    if (drag == Drag::drawNotes)
    {
        const auto span = juce::Range<juce::int64>::between (drawStart, drawEnd);

        if (! span.isEmpty())
        {
            NoteClip clip;
            clip.name = "Notes";
            clip.length = span.getLength();
            auto next = *engine.session.getState();
            const auto id = SessionEdits::placeNoteClip (next, drawTrack, clip, span.getStart());
            engine.session.apply (std::move (next), "Draw Notes");
            selectNoteClip (id);
        }

        drawTrack = -1;
        repaint();
    }

    if (preview.has_value())
    {
        auto next = std::move (*preview);
        preview.reset();
        MarkerEdits::sort (next.markers);
        engine.session.apply (std::move (next), dragDescription);
    }

    if (drag == Drag::rubberBand)
    {
        rubberBand = {};
        repaint();

        if (onSelectionChanged != nullptr)
            onSelectionChanged();
    }

    drag = Drag::none;
}

void MultitrackView::mouseMove (const juce::MouseEvent& e)
{
    auto cursorShape = juce::MouseCursor::NormalCursor;
    const auto p = e.getPosition();
    const auto overNotes = noteClipAt (p).has_value() && ! clipAt (p).has_value();
    const auto t = trackAt (p.y);
    const auto overLane = p.x >= headerWidth && p.y >= rulerHeight && (t >= (int) shown().tracks.size() || (t >= 0 && shown().tracks[(size_t) t].takesNoteClips()));

    if (markerAt (p) >= 0)
    {
        cursorShape = juce::MouseCursor::DraggingHandCursor;
    }
    else if (const auto envelope = envelopeAt (p))
    {
        cursorShape = envelope->point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor;
    }
    else if (const auto gain = clipGainAt (p))
    {
        cursorShape = gain->point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor;
    }
    else if (const auto notes = noteClipAt (p); notes.has_value() && ! clipAt (p).has_value())
    {
        const auto r = noteClipBounds (notes->first, shown().tracks[(size_t) notes->first].noteClips[(size_t) notes->second]);
        cursorShape = r.getWidth() > 3 * edgeGrab && (p.x <= r.getX() + edgeGrab || p.x >= r.getRight() - edgeGrab)
                          ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
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

    // The tools say what a click would do: Split and Draw a crosshair where they act, Glue a hand
    // over a note clip, Range a text cursor over the lanes.
    if (tool == Tool::range && p.x >= headerWidth && p.y >= rulerHeight)
        cursorShape = juce::MouseCursor::IBeamCursor;
    else if (tool == Tool::mute && (overNotes || clipAt (p).has_value()))
        cursorShape = juce::MouseCursor::PointingHandCursor;
    else if (tool == Tool::split && (overNotes || clipAt (p).has_value()))
        cursorShape = juce::MouseCursor::CrosshairCursor;
    else if (tool == Tool::glue && overNotes)
        cursorShape = juce::MouseCursor::PointingHandCursor;
    else if (tool == Tool::draw && overLane && ! overNotes && ! clipAt (p).has_value())
        cursorShape = juce::MouseCursor::CrosshairCursor;

    setMouseCursor (cursorShape);
}

void MultitrackView::setTool (Tool t)
{
    if (tool == t)
        return;

    tool = t;
    drag = Drag::none;
    stretchTo = {};
    drawTrack = -1;
    repaint();
}

void MultitrackView::splitAt (juce::Point<int> p)
{
    auto next = *engine.session.getState();
    const auto frame = snapped (frameForX ((float) p.x), 0, false);

    if (const auto hit = noteClipAt (p); hit.has_value() && ! clipAt (p).has_value())
    {
        const auto id = next.tracks[(size_t) hit->first].noteClips[(size_t) hit->second].id;
        selectNoteClip (id);
        selectTrack (hit->first);

        if (SessionEdits::splitNoteClip (next, id, frame))
            engine.session.apply (std::move (next), "Split Notes");

        return;
    }

    if (const auto hit = clipAt (p))
    {
        const auto& clip = next.tracks[(size_t) hit->first].clips[(size_t) hit->second];
        const auto id = clip.id;
        const auto at = toZeroCrossing (clip, frame);
        selectClip (id, false);
        selectedClip = id;
        selectTrack (hit->first);

        if (! clip.locked && SessionEdits::splitClip (next, id, at))
            engine.session.apply (std::move (next), "Split Clip");
    }
}

juce::uint32 MultitrackView::nextNoteClip (juce::uint32 clipId) const
{
    const auto where = shown().findNoteClip (clipId);

    if (! where.has_value())
        return 0;

    const auto& clips = shown().tracks[(size_t) where->first].noteClips;
    const auto& clip = clips[(size_t) where->second];
    const NoteClip* next = nullptr;

    for (auto& c : clips)
        if (c.id != clip.id && (c.position > clip.position || (c.position == clip.position && c.id > clip.id))
            && (next == nullptr || c.position < next->position || (c.position == next->position && c.id < next->id)))
            next = &c;

    return next != nullptr ? next->id : 0;
}

void MultitrackView::mergeWithNext (juce::uint32 clipId)
{
    const auto other = nextNoteClip (clipId);

    if (other == 0)
        return;

    auto next = *engine.session.getState();

    if (SessionEdits::mergeNoteClips (next, clipId, other))
    {
        engine.session.apply (std::move (next), "Merge Notes");
        selectNoteClip (clipId);
    }
}

void MultitrackView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (const auto index = markerAt (p); index >= 0)
    {
        renameMarker (index);
        return;
    }

    // An empty spot in the marker lane: a new marker there.
    if (p.x >= headerWidth && p.y >= timeRulerHeight && p.y < rulerHeight)
    {
        addMarkerAt (snapped (frameForX ((float) p.x), 0, true));
        return;
    }

    if (envelopeAt (p).has_value() || clipGainAt (p).has_value())
        return;

    // A note clip opens in the piano roll.
    if (const auto hit = noteClipAt (p); hit.has_value() && ! clipAt (p).has_value() && onOpenNotes != nullptr)
    {
        onOpenNotes (shown().tracks[(size_t) hit->first].noteClips[(size_t) hit->second].id);
        return;
    }

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
        onFilesDropped (files, trackAt (y), x < headerWidth ? 0 : snapped (frameForX ((float) x)));
}

bool MultitrackView::isInterestedInDragSource (const SourceDetails& details)
{
    return FilesPanel::rowOfDrag (details.description) >= 0;
}

void MultitrackView::itemDropped (const SourceDetails& details)
{
    const auto row = FilesPanel::rowOfDrag (details.description);
    const auto x = details.localPosition.x;

    if (row >= 0 && onOpenFileDropped != nullptr)
        onOpenFileDropped (row, trackAt (details.localPosition.y), x < headerWidth ? 0 : snapped (frameForX ((float) x)));
}
