#include "TrackTools.h"
#include "Theme.h"

using Tool = MultitrackView::Tool;

class TrackTools::ToolButton final : public juce::Button
{
public:
    ToolButton (Tool t, int commandId) : juce::Button ({}), tool (t)
    {
        setComponentID (juce::String (commandId));
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const auto on = getToggleState();

        if (on || highlighted || down)
        {
            g.setColour (on ? Theme::accentDeep : Theme::hover);
            g.fillRoundedRectangle (r, 3.0f);
        }

        const auto ink = ! isEnabled() ? Theme::faintText : on ? Theme::textStrong : Theme::text;
        g.setColour (ink);
        const auto box = juce::Rectangle<float> (16.0f, 16.0f).withCentre (r.getCentre());
        const auto stroke = juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        switch (tool)
        {
            case Tool::select:
            {
                // A pointer: an arrowhead with its tail.
                juce::Path arrow;
                arrow.startNewSubPath (box.getX() + 3.0f, box.getY() + 1.5f);
                arrow.lineTo (box.getX() + 3.0f, box.getBottom() - 3.5f);
                arrow.lineTo (box.getX() + 6.5f, box.getBottom() - 6.5f);
                arrow.lineTo (box.getX() + 9.0f, box.getBottom() - 1.0f);
                arrow.lineTo (box.getX() + 11.0f, box.getBottom() - 2.0f);
                arrow.lineTo (box.getX() + 8.5f, box.getBottom() - 7.5f);
                arrow.lineTo (box.getRight() - 3.0f, box.getBottom() - 7.5f);
                arrow.closeSubPath();
                g.fillPath (arrow);
                break;
            }

            case Tool::draw:
            {
                // A pencil, slanted, its tip at the bottom left.
                const auto centre = box.getCentre();
                const auto turn = juce::AffineTransform::rotation (juce::MathConstants<float>::pi / 4.0f, centre.x, centre.y);
                juce::Path body;
                body.addRoundedRectangle (juce::Rectangle<float> (5.0f, 11.0f).withCentre ({ centre.x, centre.y - 1.5f }), 1.0f);
                g.fillPath (body, turn);
                juce::Path tip;
                tip.addTriangle (centre.x - 2.5f, centre.y + 4.5f, centre.x + 2.5f, centre.y + 4.5f, centre.x, centre.y + 8.0f);
                g.fillPath (tip, turn);
                break;
            }

            case Tool::split:
            {
                // Scissors: two blades crossing, their rings below.
                juce::Path blades;
                blades.startNewSubPath (box.getX() + 4.0f, box.getBottom() - 4.0f);
                blades.lineTo (box.getRight() - 3.0f, box.getY() + 1.5f);
                blades.startNewSubPath (box.getRight() - 4.0f, box.getBottom() - 4.0f);
                blades.lineTo (box.getX() + 3.0f, box.getY() + 1.5f);
                g.strokePath (blades, stroke);
                g.drawEllipse (box.getX() + 1.0f, box.getBottom() - 5.5f, 5.0f, 5.0f, 1.5f);
                g.drawEllipse (box.getRight() - 6.0f, box.getBottom() - 5.5f, 5.0f, 5.0f, 1.5f);
                break;
            }

            case Tool::stretch:
            {
                // A two-headed arrow between two bars.
                const auto y = box.getCentreY();
                juce::Path arrows;
                arrows.startNewSubPath (box.getX() + 3.5f, y);
                arrows.lineTo (box.getRight() - 3.5f, y);
                arrows.startNewSubPath (box.getX() + 6.5f, y - 3.0f);
                arrows.lineTo (box.getX() + 3.5f, y);
                arrows.lineTo (box.getX() + 6.5f, y + 3.0f);
                arrows.startNewSubPath (box.getRight() - 6.5f, y - 3.0f);
                arrows.lineTo (box.getRight() - 3.5f, y);
                arrows.lineTo (box.getRight() - 6.5f, y + 3.0f);
                g.strokePath (arrows, stroke);
                g.fillRect (box.getX() + 0.5f, box.getY() + 2.0f, 1.5f, box.getHeight() - 4.0f);
                g.fillRect (box.getRight() - 2.0f, box.getY() + 2.0f, 1.5f, box.getHeight() - 4.0f);
                break;
            }

            case Tool::glue:
            {
                // Two blocks joined by a band.
                g.fillRoundedRectangle (box.getX() + 1.0f, box.getY() + 4.0f, 6.0f, 8.0f, 1.5f);
                g.fillRoundedRectangle (box.getRight() - 7.0f, box.getY() + 4.0f, 6.0f, 8.0f, 1.5f);
                g.fillRect (box.getX() + 6.0f, box.getY() + 6.5f, 4.0f, 3.0f);
                break;
            }

            case Tool::range:
            {
                // A dashed box: a span picked out of the lanes.
                juce::Path outline, dashed;
                outline.addRectangle (box.reduced (1.5f, 2.5f));
                const float dashes[] = { 2.5f, 2.0f };
                juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
                g.fillPath (dashed);
                break;
            }

            case Tool::mute:
            {
                // A speaker with a cross beside it: sound stopped.
                const auto y = box.getCentreY();
                juce::Path speaker;
                speaker.startNewSubPath (box.getX() + 1.0f, y - 2.5f);
                speaker.lineTo (box.getX() + 4.0f, y - 2.5f);
                speaker.lineTo (box.getX() + 8.0f, box.getY() + 2.0f);
                speaker.lineTo (box.getX() + 8.0f, box.getBottom() - 2.0f);
                speaker.lineTo (box.getX() + 4.0f, y + 2.5f);
                speaker.lineTo (box.getX() + 1.0f, y + 2.5f);
                speaker.closeSubPath();
                g.fillPath (speaker);
                juce::Path cross;
                cross.startNewSubPath (box.getRight() - 5.5f, y - 2.5f);
                cross.lineTo (box.getRight() - 1.0f, y + 2.5f);
                cross.startNewSubPath (box.getRight() - 1.0f, y - 2.5f);
                cross.lineTo (box.getRight() - 5.5f, y + 2.5f);
                g.strokePath (cross, stroke);
                break;
            }
        }
    }

private:
    Tool tool;
};

TrackTools::TrackTools (juce::ApplicationCommandManager& c, MultitrackView& v, AudioEngine& e, Commands commandIds)
    : commands (c), view (v), engine (e), ids (commandIds)
{
    run = [this] (int id) { commands.invokeDirectly (id, true); };

    for (auto [tool, id] : { std::pair (Tool::select, ids.select), std::pair (Tool::draw, ids.draw), std::pair (Tool::split, ids.split),
                             std::pair (Tool::stretch, ids.stretch), std::pair (Tool::glue, ids.glue), std::pair (Tool::range, ids.range),
                             std::pair (Tool::mute, ids.mute) })
    {
        auto* b = tools.add (new ToolButton (tool, id));
        b->onClick = [this, id] { if (run) run (id); update(); };
        addAndMakeVisible (b);
    }

    for (auto* b : { &tempoButton, &barsButton })
    {
        Theme::follow (*b, juce::TextButton::buttonOnColourId, Theme::accentDeep);
        addAndMakeVisible (b);
    }

    tempoButton.setTooltip ("The session's tempo and beats in a bar (Tempo...)");
    tempoButton.onClick = [this] { if (run) run (ids.tempo); };
    barsButton.setClickingTogglesState (false);
    barsButton.setTooltip ("Count the ruler in bars and beats of the tempo, and snap to its beats (Bars and Beats)");
    barsButton.onClick = [this] { if (run) run (ids.barsAndBeats); update(); };
    update();
    startTimerHz (10);
}

TrackTools::~TrackTools() = default;

juce::Button& TrackTools::getToolButton (Tool tool)
{
    return *tools[(int) tool];
}

juce::String TrackTools::hintFor (Tool tool)
{
    switch (tool)
    {
        case Tool::select:  return "Select: click or drag clips, or drag across empty lane to select what the band crosses; drag a clip's edge to trim it";
        case Tool::draw:    return "Draw: drag along an instrument track's lane to make a note clip there (below the tracks makes a track)";
        case Tool::split:   return "Split: click a clip to cut it there";
        case Tool::stretch: return "Stretch: drag a clip's edge to stretch what it plays, audio or notes";
        case Tool::glue:    return "Glue: click a note clip to join it with the next one on its track";
        case Tool::range:   return "Range: drag across the lanes to select a time range, leaving the clips as they are";
        case Tool::mute:    return "Mute: click a clip, audio or notes, to silence it or let it be heard again";
    }

    return {};
}

juce::String TrackTools::tempoText (double tempo, int beatsPerBar)
{
    return juce::String (tempo, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " bpm, " + juce::String (beatsPerBar) + "/4";
}

void TrackTools::timerCallback()
{
    update();
}

void TrackTools::update()
{
    const auto tool = view.getTool();

    for (int i = 0; i < tools.size(); ++i)
        tools[i]->setToggleState (i == (int) tool, juce::dontSendNotification);

    for (auto [button, name] : { std::pair (tools[(int) Tool::select], "Select"), std::pair (tools[(int) Tool::draw], "Draw"),
                                 std::pair (tools[(int) Tool::split], "Split"), std::pair (tools[(int) Tool::stretch], "Stretch"),
                                 std::pair (tools[(int) Tool::glue], "Glue"), std::pair (tools[(int) Tool::range], "Range"),
                                 std::pair (tools[(int) Tool::mute], "Mute") })
    {
        const auto keys = commands.getKeyMappings() != nullptr ? commands.getKeyMappings()->getKeyPressesAssignedToCommand (button->getComponentID().getIntValue()) : juce::Array<juce::KeyPress>();
        button->setTooltip (juce::String (name) + " tool" + (keys.isEmpty() ? juce::String() : " (" + keys[0].getTextDescription() + ")"));
    }

    const auto state = engine.session.getState();
    const auto tempo = tempoText (state->tempo, state->beatsPerBar);

    if (tempoButton.getButtonText() != tempo)
        tempoButton.setButtonText (tempo);

    barsButton.setToggleState (view.isShowingBars(), juce::dontSendNotification);

    if (shownTool != tool)
    {
        shownTool = tool;
        repaint();
    }
}

void TrackTools::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
    g.setColour (Theme::line);
    g.fillRect (0, 0, getWidth(), 1);
    g.fillRect (tools.getLast()->getRight() + 6, 5, 1, getHeight() - 9);

    // What the tool in hand does, between the tools and the tempo.
    const auto left = tools.getLast()->getRight() + 14;
    const auto right = tempoButton.getX() - 10;

    if (right > left + 40)
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (hintFor (shownTool), left, 0, right - left, getHeight(), juce::Justification::centredLeft, true);
    }
}

void TrackTools::resized()
{
    auto r = getLocalBounds().reduced (4, 2);

    for (auto* b : tools)
    {
        b->setBounds (r.removeFromLeft (r.getHeight()));
        r.removeFromLeft (2);
    }

    barsButton.setBounds (r.removeFromRight (44));
    r.removeFromRight (4);
    tempoButton.setBounds (r.removeFromRight (110));
}
