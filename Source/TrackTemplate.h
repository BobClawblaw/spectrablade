#pragma once

#include "Session.h"

/** Track templates (.sbtrack): a track's or bus's settings to start new ones from in any
    session. A template holds its name, whether it is a bus, its volume, pan, colour, input,
    note, sends (by the bus's name, as ids mean nothing in another session) and its effects
    chain; not its clips, envelopes, group, mute or solo. */
namespace TrackTemplate
{
    inline constexpr const char* extension = ".sbtrack";

    /** The template of the track with this id (null when there is none), with its effects as
        TrackEffects::createXml gives them (or none). */
    std::unique_ptr<juce::XmlElement> create (const SessionState&, juce::uint32 trackId, std::unique_ptr<juce::XmlElement> effects);

    struct Added
    {
        juce::uint32 trackId = 0;                     // 0 when the XML is not a template
        std::shared_ptr<juce::XmlElement> effects;    // to load onto it, or null
        juce::StringArray missingBuses;               // sends left out: no bus of that name here
    };

    /** Adds a track or bus made from a template at the end of the session. Sends go to the
        session's first bus of the same name. */
    Added add (SessionState&, const juce::XmlElement&);
}
