#pragma once

#include <JuceHeader.h>
#include "SessionMixer.h"

class TrackEffects;

namespace OfflineRender
{
    /** Processes audio in place through the rack described by rackXml (AudioEngine::createRackXml()),
        using fresh plug-in instances so the live rack keeps playing. Bypassed slots are skipped and each
        plug-in's reported latency is compensated, so the result lines up with the input. Effect tails
        past the end of the audio are cut off.

        Call from a background thread: plug-ins are created synchronously, which AUv3 requires to happen
        off the message thread. progress receives 0..1 and returns false to cancel.
        Returns an error message, or empty on success. */
    juce::String processThroughRack (const juce::XmlElement& rackXml,
                                     juce::AudioPluginFormatManager& formats,
                                     double sampleRate,
                                     juce::AudioBuffer<float>& audio,
                                     const std::function<bool (float progress)>& progress);

    /** Loads fresh instances of each track's effects (TrackEffects::createXml(), keyed by track id)
        into `into`, prepared at this rate for offline processing, so the live chains keep playing.
        A plug-in that can't be created is left out and named in the returned errors (one per line).

        Call from a background thread, as for processThroughRack(). */
    juce::String loadTrackEffects (const std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>& effects,
                                   juce::AudioPluginFormatManager& formats,
                                   double sampleRate,
                                   TrackEffects& into);

    /** Mixes session frames [range.start, range.end) into dest (resized to two channels) as
        playback would, primed for the range's start so effects and their delay compensation are
        already in time. Returns false if progress cancelled it. */
    bool mixSession (const SessionState&, juce::Range<juce::int64> range, juce::AudioBuffer<float>& dest,
                     SessionMixer::Inserts*, const std::function<bool (float progress)>& progress);
}
