#include "ClipProperties.h"
#include "Theme.h"

namespace
{
    juce::String secondsText (juce::int64 frames, double rate)
    {
        return juce::String ((double) frames / rate, 3);
    }

    /** Seconds typed into a field, as frames; nothing for text that isn't a number. */
    std::optional<juce::int64> framesFrom (const juce::String& text, double rate)
    {
        const auto trimmed = text.trim();

        if (trimmed.isEmpty() || ! trimmed.containsOnly ("0123456789.-+"))
            return std::nullopt;

        return (juce::int64) std::llround (trimmed.getDoubleValue() * rate);
    }
}

ClipProperties::ClipProperties (AudioEngine& e, std::function<juce::uint32()> toShow)
    : engine (e), clipToShow (std::move (toShow))
{
    const juce::StringArray names { "Name", "Start (s)", "Length (s)", "Gain (dB)", "Fade in (s)", "Fade out (s)", "Note" };

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = rows[i];
        row.label.setText (names[(int) i], juce::dontSendNotification);
        row.label.setJustificationType (juce::Justification::centredRight);
        const auto field = (Field) i;
        row.editor.onReturnKey = [this, field] { apply (field); };
        row.editor.onFocusLost = [this, field] { apply (field); };
        row.editor.onEscapeKey = [this] { refresh(); };
        addAndMakeVisible (row.label);
        addAndMakeVisible (row.editor);
    }

    Theme::follow (info, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (info);
    engine.session.addChangeListener (this);
    refresh();
    startTimerHz (8);
}

ClipProperties::~ClipProperties()
{
    engine.session.removeChangeListener (this);
}

juce::TextEditor& ClipProperties::getEditor (Field f)
{
    return rows[(size_t) f].editor;
}

void ClipProperties::apply (Field f)
{
    const auto text = getEditor (f).getText();
    auto next = *engine.session.getState();
    const auto where = next.findClip (shownClip);

    if (! where.has_value())
        return;

    const auto c = next.tracks[(size_t) where->first].clips[(size_t) where->second];
    const auto rate = next.sampleRate;
    const auto frames = framesFrom (text, rate);
    auto changed = false;
    juce::String description;

    switch (f)
    {
        case Field::name:
            if (text.trim().isNotEmpty() && text.trim() != c.name)
            {
                next.tracks[(size_t) where->first].clips[(size_t) where->second].name = text.trim();
                changed = true;
                description = "Rename Clip";
            }
            break;
        case Field::start:
            changed = frames.has_value() && *frames >= 0 && *frames != c.position && SessionEdits::moveClip (next, c.id, where->first, *frames);
            description = "Move Clip";
            break;
        case Field::length:
            changed = frames.has_value() && *frames > 0 && *frames != c.length
                      && SessionEdits::trimClip (next, c.id, c.position, c.position + *frames);
            description = "Trim Clip";
            break;
        case Field::gain:
        {
            const auto db = text.trim();

            if (db.isNotEmpty() && db.containsOnly ("0123456789.-+") && ! juce::exactlyEqual (db.getFloatValue(), c.gainDb))
                changed = SessionEdits::setClipGain (next, c.id, db.getFloatValue());

            description = "Clip Gain";
            break;
        }
        case Field::fadeIn:
            changed = frames.has_value() && *frames >= 0 && *frames != c.fadeIn && SessionEdits::setClipFades (next, c.id, *frames, c.fadeOut);
            description = "Clip Fade In";
            break;
        case Field::fadeOut:
            changed = frames.has_value() && *frames >= 0 && *frames != c.fadeOut && SessionEdits::setClipFades (next, c.id, c.fadeIn, *frames);
            description = "Clip Fade Out";
            break;
        case Field::note:
            // Any text, or none to clear it; locked clips take notes too.
            changed = text.trim() != c.note;
            next.tracks[(size_t) where->first].clips[(size_t) where->second].note = text.trim();
            description = "Clip Note";
            break;
    }

    if (changed)
        engine.session.apply (std::move (next), description);
    else
        refresh();   // shows the setting again in place of what couldn't apply
}

void ClipProperties::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void ClipProperties::timerCallback()
{
    if (clipToShow != nullptr && clipToShow() != shownClip)
        refresh();
}

void ClipProperties::refresh()
{
    shownClip = clipToShow != nullptr ? clipToShow() : 0;
    const auto state = engine.session.getState();
    const auto where = state->findClip (shownClip);

    if (! where.has_value())
    {
        shownClip = 0;
        info.setText ("No clip selected", juce::dontSendNotification);

        for (auto& row : rows)
        {
            row.editor.setText ({}, false);
            row.editor.setEnabled (false);
        }

        return;
    }

    const auto& c = state->tracks[(size_t) where->first].clips[(size_t) where->second];
    const auto rate = state->sampleRate;
    const std::array<juce::String, 7> values { c.name, secondsText (c.position, rate), secondsText (c.length, rate),
                                               juce::String (c.gainDb, 1), secondsText (c.fadeIn, rate), secondsText (c.fadeOut, rate), c.note };

    for (size_t i = 0; i < rows.size(); ++i)
    {
        rows[i].editor.setEnabled (true);

        // Not while it is being typed into.
        if (! rows[i].editor.hasKeyboardFocus (true))
            rows[i].editor.setText (values[i], false);
    }

    info.setText (c.source.getFileName() + (c.takes.size() > 1 ? ", take " + juce::String (c.activeTake + 1) + " of " + juce::String ((int) c.takes.size()) : juce::String()),
                  juce::dontSendNotification);
}

void ClipProperties::paint (juce::Graphics& g)
{
    g.fillAll (Theme::surface);
}

void ClipProperties::resized()
{
    auto r = getLocalBounds().reduced (10);
    info.setBounds (r.removeFromTop (22));
    r.removeFromTop (6);

    for (auto& row : rows)
    {
        auto line = r.removeFromTop (24);
        row.label.setBounds (line.removeFromLeft (90));
        line.removeFromLeft (8);
        row.editor.setBounds (line);
        r.removeFromTop (6);
    }
}
