#include "Scrubber.h"

void Scrubber::prepare (int blockSize)
{
    window.setSize (2, (int) (2.0 * (fastest * 4.0 * blockSize + 4.0)) + 4, false, true, false);
}

void Scrubber::start (juce::int64 frame)
{
    target = (double) frame;
    startAt = (double) frame;
    restart = true;
    on = true;
}

void Scrubber::moveTo (juce::int64 frame)
{
    target = (double) frame;
}

void Scrubber::stop()
{
    on = false;
}

void Scrubber::render (const AudioSnapshot& audio, double audioRate, double deviceRate, juce::AudioBuffer<float>& buffer, int start, int n)
{
    if (audioRate <= 0.0 || deviceRate <= 0.0 || buffer.getNumChannels() == 0 || n <= 0)
        return;

    if (restart.exchange (false))
    {
        head = startAt.load();
        speed = 0.0;
        level = 0.0;
    }

    const auto isOnNow = on.load();
    const auto goal = target.load();
    const auto step = audioRate / deviceRate;                     // audio frames a device frame at normal speed
    const auto lag = juce::jmax (1.0, lagSeconds * deviceRate);    // in device frames
    const auto smooth = 1.0 - std::exp (-1.0 / juce::jmax (1.0, smoothSeconds * deviceRate));
    const auto fadeStep = 1.0 / juce::jmax (1.0, smoothSeconds * deviceRate);
    const auto channels = juce::jmin (2, audio.getNumChannels());
    const auto length = audio.getNumFrames();

    // The frames the head can reach in this block, with two either side for the curve.
    const auto reach = fastest * step * n + 4.0;
    const auto first = (juce::int64) std::floor (head - reach);
    const auto count = (int) (2.0 * reach) + 4;

    if (window.getNumSamples() < count)   // a block longer than prepared for
        window.setSize (2, count, false, false, true);

    if (channels > 0)
        audio.read (window, 0, first, count);   // outside the audio reads as silence

    const auto sampleAt = [&] (int ch, double at)
    {
        const auto base = (juce::int64) std::floor (at);
        const auto t = (float) (at - (double) base);
        const auto i = (int) (base - first);

        if (base < -2 || base > length + 1 || i < 1 || i + 2 >= count)
            return 0.0f;

        const auto* d = window.getReadPointer (ch);
        const auto p0 = d[i - 1], p1 = d[i], p2 = d[i + 1], p3 = d[i + 2];
        return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    };

    for (int i = 0; i < n; ++i)
    {
        // The speed that would bring the head to the target over the lag, within the fastest,
        // smoothed; the level follows the speed, and falls away once stopped.
        const auto wanted = isOnNow ? juce::jlimit (-fastest, fastest, (goal - head) / (lag * step)) : speed;
        speed += (wanted - speed) * smooth;
        // Let go, the level falls in a straight line over the smoothing time.
        if (isOnNow)
            level += (juce::jmin (1.0, std::abs (speed) / fullLevelSpeed) - level) * smooth;
        else
            level = juce::jmax (0.0, level - fadeStep);

        if (level > 0.0 && channels > 0)
        {
            const auto left = sampleAt (0, head), right = channels > 1 ? sampleAt (1, head) : left;
            buffer.addSample (0, start + i, (float) level * left);

            if (buffer.getNumChannels() > 1)
                buffer.addSample (1, start + i, (float) level * right);
        }

        head = juce::jlimit (-1.0, (double) length, head + speed * step);
    }
}
