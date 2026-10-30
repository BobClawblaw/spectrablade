#pragma once

#include <JuceHeader.h>

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
}
