#include "Surround.h"

namespace Surround
{
juce::String channelName (int numChannels, int channel)
{
    if (numChannels == 1)
        return {};

    if (numChannels == 2 && juce::isPositiveAndBelow (channel, 2))
        return channel == 0 ? "L" : "R";

    if (numChannels == fiveOneChannels && juce::isPositiveAndBelow (channel, fiveOneChannels))
        return juce::StringArray { "L", "R", "C", "LFE", "Ls", "Rs" }[channel];

    if (numChannels == sevenOneChannels && juce::isPositiveAndBelow (channel, sevenOneChannels))
        return juce::StringArray { "L", "R", "C", "LFE", "Lrs", "Rrs", "Lss", "Rss" }[channel];

    return juce::String (channel + 1);
}

void foldDown (const float* const* in, float* left, float* right, int numFrames)
{
    for (int i = 0; i < numFrames; ++i)
    {
        const auto centre = foldGain * in[2][i];
        const auto l = in[0][i] + centre + foldGain * in[4][i];
        const auto r = in[1][i] + centre + foldGain * in[5][i];
        left[i] = l;
        right[i] = r;
    }
}

void foldDown (const float* const* in, int numChannels, float* left, float* right, int numFrames)
{
    if (numChannels != sevenOneChannels)
    {
        foldDown (in, left, right, numFrames);
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        const auto centre = foldGain * in[2][i];
        left[i] = in[0][i] + centre + foldGain * in[4][i] + foldGain * in[6][i];
        right[i] = in[1][i] + centre + foldGain * in[5][i] + foldGain * in[7][i];
    }
}

juce::Array<juce::File> orderForCombining (juce::Array<juce::File> files)
{
    const auto layout = [] (int count) -> juce::StringArray
    {
        if (count == fiveOneChannels)
            return { "l", "r", "c", "lfe", "ls", "rs" };

        if (count == sevenOneChannels)
            return { "l", "r", "c", "lfe", "lrs", "rrs", "lss", "rss" };

        return {};
    };
    const auto names = layout (files.size());
    std::vector<std::pair<int, juce::File>> placed;

    for (auto& f : files)
    {
        const auto stem = f.getFileNameWithoutExtension().toLowerCase();
        const auto cut = juce::jmax (stem.lastIndexOfChar (' '), stem.lastIndexOfChar ('_'), stem.lastIndexOfChar ('-'), stem.lastIndexOfChar ('.'));
        placed.emplace_back (cut >= 0 ? names.indexOf (stem.substring (cut + 1)) : -1, f);
    }

    std::sort (placed.begin(), placed.end(), [] (auto& a, auto& b) { return a.first < b.first; });
    auto named = ! names.isEmpty();

    for (size_t i = 0; named && i < placed.size(); ++i)
        named = placed[i].first == (int) i;

    if (named)
    {
        juce::Array<juce::File> ordered;

        for (auto& p : placed)
            ordered.add (p.second);

        return ordered;
    }

    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });
    return files;
}

juce::String channelFileName (const juce::String& stem, int numChannels, int channel)
{
    const auto name = channelName (numChannels, channel);
    return stem + " " + (name.isNotEmpty() ? name : juce::String (channel + 1));
}

juce::BigInteger extraOutputs (int numChannels, int outputPairFirstChannel, bool separateOutputs)
{
    juce::BigInteger outputs;

    if (separateOutputs && numChannels > 2)
        outputs.setRange (outputPairFirstChannel + 2, juce::jmin (numChannels, maxPlayedChannels) - 2, true);

    return outputs;
}
}
