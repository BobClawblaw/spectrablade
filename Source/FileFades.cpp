#include "FileFades.h"
#include "AudioEdits.h"

namespace FileFades
{
float level (double x, float curve)
{
    if (x <= 0.0)
        return 0.0f;

    if (x >= 1.0)
        return 1.0f;

    return (float) std::pow (x, std::pow (4.0, (double) -juce::jlimit (-1.0f, 1.0f, curve)));
}

std::shared_ptr<const AudioSnapshot> render (const AudioSnapshot& s, Fade in, Fade out)
{
    const auto length = s.getNumFrames();
    in.frames = juce::jlimit ((juce::int64) 0, length, in.frames);
    out.frames = juce::jlimit ((juce::int64) 0, length - in.frames, out.frames);
    std::shared_ptr<const AudioSnapshot> result;

    // Each frame of a fade in gets level ((i + 1) / n): the first is nearly silent and the last at
    // full level; a fade out mirrors it, ending on a frame nearly silent.
    if (in.frames > 0)
        result = AudioEdits::processRange (s, 0, in.frames, [in] (juce::AudioBuffer<float>& b)
        {
            const auto n = b.getNumSamples();

            for (int i = 0; i < n; ++i)
            {
                const auto g = level ((double) (i + 1) / (double) n, in.curve);

                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    b.getWritePointer (ch)[i] *= g;
            }
        });

    if (out.frames > 0)
        result = AudioEdits::processRange (result != nullptr ? *result : s, length - out.frames, length, [out] (juce::AudioBuffer<float>& b)
        {
            const auto n = b.getNumSamples();

            for (int i = 0; i < n; ++i)
            {
                const auto g = level ((double) (n - i) / (double) n, out.curve);

                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    b.getWritePointer (ch)[i] *= g;
            }
        });

    return result;
}

bool Draft::isLive (const AudioDocument& doc) const
{
    return base != nullptr && doc.getPosition() == stepPosition && doc.getVersion() == stepVersion;
}

Fade Draft::get (const AudioDocument& doc, bool isOut) const
{
    return isLive (doc) ? (isOut ? out : in) : Fade {};
}

juce::int64 Draft::maxFrames (const AudioDocument& doc, bool isOut) const
{
    return juce::jmax ((juce::int64) 0, doc.getNumFrames() - get (doc, ! isOut).frames);
}

Fade Draft::set (AudioDocument& doc, bool isOut, Fade fade)
{
    fade.frames = juce::jlimit ((juce::int64) 0, maxFrames (doc, isOut), fade.frames);
    fade.curve = juce::jlimit (-1.0f, 1.0f, fade.curve);

    if (isLive (doc))
    {
        doc.undo();   // back to the audio before the fades; they are made again below
    }
    else
    {
        base = doc.getSnapshot();
        in = out = {};
    }

    (isOut ? out : in) = fade;
    const auto description = in.frames > 0 && out.frames > 0 ? "Fade In and Out" : in.frames > 0 ? "Fade In" : "Fade Out";

    if (in.frames == 0 && out.frames == 0)
    {
        base = nullptr;   // both taken away: the document is as it was before them
        return fade;
    }

    doc.apply (render (*base, in, out), description);
    stepPosition = doc.getPosition();
    stepVersion = doc.getVersion();
    return fade;
}
}
