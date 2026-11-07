#include "TrackTemplate.h"
#include "TrackColours.h"

namespace TrackTemplate
{
std::unique_ptr<juce::XmlElement> create (const SessionState& s, juce::uint32 trackId, std::unique_ptr<juce::XmlElement> effects)
{
    const auto* t = s.findTrack (trackId);

    if (t == nullptr)
        return nullptr;

    auto xml = std::make_unique<juce::XmlElement> ("TRACKTEMPLATE");
    xml->setAttribute ("name", t->name);
    xml->setAttribute ("bus", t->isBus);
    xml->setAttribute ("gainDb", t->gainDb);
    xml->setAttribute ("pan", t->pan);

    if (t->fiveOne)
        xml->setAttribute ("fiveOne", true);

    if (t->sevenOne)
        xml->setAttribute ("sevenOne", true);

    if (! t->surroundPan.isDefault())
    {
        xml->setAttribute ("surroundX", t->surroundPan.x);
        xml->setAttribute ("surroundY", t->surroundPan.y);
        xml->setAttribute ("surroundWidth", t->surroundPan.width);
        xml->setAttribute ("lfeDb", t->surroundPan.lfeDb);
    }

    xml->setAttribute ("colour", t->colour);
    xml->setAttribute ("note", t->note);
    xml->setAttribute ("inputFirst", t->input.first);
    xml->setAttribute ("inputChannels", t->input.channels);

    for (auto& send : t->sends)
    {
        if (const auto* bus = s.findTrack (send.busId))
        {
            auto* sendXml = xml->createNewChildElement ("SEND");
            sendXml->setAttribute ("bus", bus->name);
            sendXml->setAttribute ("gainDb", send.gainDb);
            sendXml->setAttribute ("preFader", send.preFader);
        }
    }

    if (const auto* out = s.outputBusOf (*t))
        xml->setAttribute ("output", out->name);
    else if (t->deviceOutput >= 0)
        xml->setAttribute ("deviceOutput", t->deviceOutput);

    if (effects != nullptr)
        xml->addChildElement (effects.release());

    // Its envelopes, with the rate their frames are at.
    if (t->isAutomated())
    {
        xml->setAttribute ("sampleRate", s.sampleRate);
        SessionFile::writeEnvelopes (*t, *xml);
    }

    return xml;
}

Added add (SessionState& s, const juce::XmlElement& xml)
{
    Added added;

    if (! xml.hasTagName ("TRACKTEMPLATE"))
        return added;

    auto& t = xml.getBoolAttribute ("bus") ? s.addBus() : s.addTrack();
    t.name = xml.getStringAttribute ("name", t.name);
    t.gainDb = (float) xml.getDoubleAttribute ("gainDb");
    t.pan = juce::jlimit (-1.0f, 1.0f, (float) xml.getDoubleAttribute ("pan"));
    t.colour = juce::jlimit (-1, (int) TrackColours::palette.size() - 1, xml.getIntAttribute ("colour", -1));
    t.note = xml.getStringAttribute ("note");
    t.input.first = xml.getIntAttribute ("inputFirst", t.input.first);
    t.input.channels = xml.getIntAttribute ("inputChannels", t.input.channels);
    const auto id = t.id;
    SessionEdits::setTrackChannels (s, id, xml.getBoolAttribute ("fiveOne") ? 6 : xml.getBoolAttribute ("sevenOne") ? 8 : 2);
    SessionEdits::setSurroundPan (s, id, { (float) xml.getDoubleAttribute ("surroundX", 0.0), (float) xml.getDoubleAttribute ("surroundY", 1.0),
                                           (float) xml.getDoubleAttribute ("surroundWidth", 1.0),
                                           (float) xml.getDoubleAttribute ("lfeDb", SurroundPan::lfeOff) });

    if (! t.isBus)
    {
        for (auto* sendXml : xml.getChildWithTagNameIterator ("SEND"))
        {
            const auto busName = sendXml->getStringAttribute ("bus");
            const auto bus = std::find_if (s.tracks.begin(), s.tracks.end(), [&busName] (auto& b) { return b.isBus && b.name == busName; });

            if (bus == s.tracks.end())
            {
                added.missingBuses.addIfNotAlreadyThere (busName);
                continue;
            }

            SessionEdits::setSend (s, id, { bus->id, (float) sendXml->getDoubleAttribute ("gainDb"), sendXml->getBoolAttribute ("preFader") });
        }
    }

    // The output, by the bus's name (a bus template's own new bus isn't one to play into).
    if (const auto outName = xml.getStringAttribute ("output"); outName.isNotEmpty())
    {
        const auto bus = std::find_if (s.tracks.begin(), s.tracks.end(), [&outName, id] (auto& b) { return b.isBus && b.name == outName && b.id != id; });

        if (bus == s.tracks.end())
            added.missingBuses.addIfNotAlreadyThere (outName);
        else
            SessionEdits::setTrackOutput (s, id, bus->id);
    }

    if (xml.hasAttribute ("deviceOutput") && ! xml.hasAttribute ("output"))
        SessionEdits::setTrackDeviceOutput (s, id, xml.getIntAttribute ("deviceOutput", -1));

    if (const auto* effects = xml.getChildByName ("EFFECTS"))
        added.effects = std::make_shared<juce::XmlElement> (*effects);

    // Envelopes at the same times here; effect envelopes follow the effects' slots, loaded with them.
    const auto rate = xml.getDoubleAttribute ("sampleRate", s.sampleRate);
    SessionFile::readEnvelopes (s, id, xml, rate > 0.0 && s.sampleRate > 0.0 ? s.sampleRate / rate : 1.0);

    added.trackId = id;
    return added;
}
}
