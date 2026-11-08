#include "Multiband.h"
#include "Theme.h"

namespace Multiband
{
namespace
{
    constexpr double butterworthQ = 0.7071067811865476;

    bool inRange (double v, double low, double high)    { return std::isfinite (v) && v >= low && v <= high; }
}

//==============================================================================
double Crossover::Biquad::run (double x) noexcept
{
    const auto y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void Crossover::set (double frequency, double sampleRate)
{
    // Second-order Butterworth low- and high-passes by the bilinear transform, prewarped.
    rateSet = sampleRate;
    const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const auto cosW = std::cos (w), alpha = std::sin (w) / (2.0 * butterworthQ), a0 = 1.0 + alpha;
    const auto setTo = [&] (Biquad& f, bool low)
    {
        const auto edge = low ? (1.0 - cosW) / 2.0 : (1.0 + cosW) / 2.0;
        f.b0 = edge / a0;
        f.b1 = (low ? 2.0 * edge : -2.0 * edge) / a0;
        f.b2 = edge / a0;
        f.a1 = -2.0 * cosW / a0;
        f.a2 = (1.0 - alpha) / a0;
    };

    for (auto* group : { &lows, &allLows })
        for (auto& f : *group)
            setTo (f, true);

    for (auto* group : { &highs, &allHighs })
        for (auto& f : *group)
            setTo (f, false);
}

void Crossover::reset() noexcept
{
    for (auto* group : { &lows, &highs, &allLows, &allHighs })
        for (auto& f : *group)
            f.z1 = f.z2 = 0.0;
}

void Crossover::split (double in, double& low, double& high) noexcept
{
    low = lows[1].run (lows[0].run (in));
    high = highs[1].run (highs[0].run (in));
}

double Crossover::allPass (double in) noexcept
{
    return allLows[1].run (allLows[0].run (in)) + allHighs[1].run (allHighs[0].run (in));
}

double Crossover::lowLevelAt (double hz) const noexcept
{
    // The two low-passes are the same filter, so the level is one biquad's squared.
    const auto& f = lows[0];
    const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * hz / rateSet);
    const auto one = std::abs ((f.b0 + f.b1 * z + f.b2 * z * z) / (1.0 + f.a1 * z + f.a2 * z * z));
    return one * one;
}

//==============================================================================
void Split::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    channels.assign ((size_t) juce::jmax (0, numChannels), {});
    parts.assign (channels.size(), {});
    sames.assign (channels.size(), nullptr);

    for (auto& p : parts)
        for (auto& band : p)
            band.assign ((size_t) chunk, 0.0);

    for (auto& ch : channels)
        ch.delay.assign ((size_t) linearLatency, 0.0f);

    const auto n = (int) channels.size();
    lowWork.setSize (n, chunk);
    highWork.setSize (n, chunk);
    sameWork.setSize (n, chunk);
    built = false;

    if (low > 0.0)
        set (low, high, linear);

    reset();
}

void Split::reset() noexcept
{
    for (auto& ch : channels)
    {
        for (auto* c : { &ch.first, &ch.second, &ch.lowAlign })
            c->reset();

        std::fill (ch.delay.begin(), ch.delay.end(), 0.0f);
    }

    lows.reset();
    highs.reset();
    delayPos = 0;
}

void Split::set (double lowCrossover, std::optional<double> highCrossover, bool linearPhase)
{
    const auto changed = ! built || ! juce::exactlyEqual (lowCrossover, low) || highCrossover != high || linearPhase != linear;
    low = lowCrossover;
    high = highCrossover;
    linear = linearPhase;

    if (! changed || channels.empty())
        return;

    built = true;

    for (auto& ch : channels)
    {
        ch.first.set (low, rate);

        if (high)
        {
            ch.second.set (*high, rate);
            ch.lowAlign.set (*high, rate);
        }
    }

    if (! linear)
        return;

    // The impulses with the crossovers' own levels: the lows' low-pass, and for the highs the part
    // above the high crossover (what its low-pass leaves).
    const auto n = (int) channels.size();
    const auto& model = channels[0];
    const auto decibels = [] (double level) { return 20.0 * std::log10 (juce::jmax (1.0e-12, level)); };
    lows.prepare ([&model, decibels] (double hz) { return decibels (model.first.lowLevelAt (hz)); }, rate, n);

    if (high)
        highs.prepare ([&model, decibels] (double hz) { return decibels (1.0 - model.second.lowLevelAt (hz)); }, rate, n);
}

void Split::run (const float* const* in, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) channels.size());
    numFrames = juce::jmin (numFrames, chunk);

    if (! linear)
    {
        for (int c = 0; c < n; ++c)
        {
            auto& ch = channels[(size_t) c];
            auto& p = parts[(size_t) c];
            sames[(size_t) c] = in[c];

            for (int i = 0; i < numFrames; ++i)
            {
                // Low and the rest at the low crossover; the rest into middle and high at the high one;
                // the low band turned as the high split turns the others, so the three add up flat.
                double lowPart = 0.0, rest = 0.0, middle = 0.0, highPart = 0.0;
                ch.first.split (in[c][i], lowPart, rest);

                if (high)
                {
                    ch.second.split (rest, middle, highPart);
                    lowPart = ch.lowAlign.allPass (lowPart);
                }
                else
                {
                    middle = rest;
                }

                p[0][(size_t) i] = lowPart;
                p[1][(size_t) i] = middle;
                p[2][(size_t) i] = highPart;
            }
        }

        return;
    }

    for (int c = 0; c < n; ++c)
    {
        lowWork.copyFrom (c, 0, in[c], numFrames);

        if (high)
            highWork.copyFrom (c, 0, in[c], numFrames);
    }

    lows.process (lowWork.getArrayOfWritePointers(), n, numFrames);

    if (high)
        highs.process (highWork.getArrayOfWritePointers(), n, numFrames);

    for (int c = 0; c < n; ++c)
    {
        auto& ch = channels[(size_t) c];
        auto& p = parts[(size_t) c];
        auto* same = sameWork.getWritePointer (c);
        const auto* lowOut = lowWork.getReadPointer (c);
        const auto* highOut = highWork.getReadPointer (c);
        auto pos = delayPos;

        for (int i = 0; i < numFrames; ++i)
        {
            // The sound as it was, late by the impulses' delay, is the three bands together.
            const auto was = ch.delay[(size_t) pos];
            ch.delay[(size_t) pos] = in[c][i];
            same[i] = was;

            if (++pos == linearLatency)
                pos = 0;

            const auto lowPart = lowOut[i], highPart = high ? highOut[i] : 0.0f;
            p[0][(size_t) i] = lowPart;
            p[1][(size_t) i] = was - lowPart - highPart;
            p[2][(size_t) i] = highPart;
        }

        sames[(size_t) c] = same;
    }

    delayPos = (delayPos + numFrames) % linearLatency;
}

//==============================================================================
bool isValid (const Compressor& c, double sampleRate)
{
    const auto nyquist = sampleRate / 2.0;

    if (sampleRate <= 0.0 || ! inRange (c.lowCrossover, 20.0, 20000.0) || ! inRange (c.highCrossover, 20.0, 20000.0)
        || c.highCrossover >= nyquist || c.highCrossover < 2.0 * c.lowCrossover || ! inRange (c.outputDb, -24.0, 24.0))
        return false;

    for (auto& band : c.bands)
        if (! Dynamics::isValid (band))
            return false;

    return true;
}

Compressor spaced (Compressor c, double sampleRate)
{
    const auto top = juce::jmin (20000.0, 0.45 * sampleRate);
    c.lowCrossover = juce::jlimit (20.0, top / 2.0, c.lowCrossover);
    c.highCrossover = juce::jlimit (2.0 * c.lowCrossover, top, c.highCrossover);
    return c;
}

void CompressorCore::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    split.prepare (numChannels, rate);
    set (settings);
    reset();
}

void CompressorCore::reset() noexcept
{
    split.reset();

    for (auto& c : cores)
        c.reset();

    reduction = {};
}

void CompressorCore::set (const Compressor& c)
{
    settings = c;
    split.set (c.lowCrossover, c.highCrossover, c.linearPhase);

    for (size_t b = 0; b < cores.size(); ++b)
        cores[b].set (c.bands[b], rate);

    output = std::pow (10.0, c.outputDb / 20.0);
}

void CompressorCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, split.getNumChannels());

    for (int done = 0; done < numFrames && n > 0;)
    {
        const auto frames = juce::jmin (Split::chunk, numFrames - done);
        std::array<const float*, 64> in {};

        for (int ch = 0; ch < juce::jmin (n, 64); ++ch)
            in[(size_t) ch] = channels[ch] + done;

        split.run (in.data(), juce::jmin (n, 64), frames);

        for (int i = 0; i < frames; ++i)
        {
            std::array<double, 3> loudest {};

            for (int ch = 0; ch < n; ++ch)
                for (size_t b = 0; b < 3; ++b)
                    loudest[b] = juce::jmax (loudest[b], std::abs (split.band (ch, (int) b)[i]));

            std::array<double, 3> factors {};

            for (size_t b = 0; b < 3; ++b)
            {
                factors[b] = cores[b].next (loudest[b]) * output;
                reduction[b] = 20.0 * std::log10 (factors[b] / output) - settings.bands[b].makeupDb;
            }

            for (int ch = 0; ch < n; ++ch)
                channels[ch][done + i] = (float) (split.band (ch, 0)[i] * factors[0] + split.band (ch, 1)[i] * factors[1] + split.band (ch, 2)[i] * factors[2]);
        }

        done += frames;
    }
}

AudioEdits::Snapshot compress (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Compressor& c)
{
    if (! isValid (c, sampleRate))
        return nullptr;

    CompressorCore core;
    core.prepare (s.getNumChannels(), sampleRate);
    core.set (c);
    return runPrimed (s, start, end, sampleRate, core);
}

//==============================================================================
namespace
{
    juce::String hertz (double hz)    { return hz >= 1000.0 ? juce::String (hz / 1000.0, hz < 10000.0 ? 2 : 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz"; }

    const std::array<const char*, 5> rowNames { "Threshold", "Ratio", "Attack", "Release", "Makeup" };
}

juce::Slider* Panel::make (juce::NormalisableRange<double> range, double interval, const juce::String& suffix)
{
    range.interval = interval;
    auto* s = sliders.add (new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight));
    s->setNormalisableRange (range);
    s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
    s->setTextValueSuffix (suffix);
    s->onValueChange = [this]
    {
        repaint();   // the bands' frequencies over their columns

        if (onChange != nullptr)
            onChange();
    };
    addAndMakeVisible (s);
    return s;
}

Panel::Panel()
{
    lowCrossover = make ({ 20.0, 20000.0, 0.0, 0.25 }, 1.0, " Hz");
    highCrossover = make ({ 20.0, 20000.0, 0.0, 0.25 }, 1.0, " Hz");
    output = make ({ -24.0, 24.0 }, 0.1, " dB");

    for (auto& band : bands)
    {
        band[0] = make ({ -80.0, 0.0 }, 0.1, " dB");
        band[1] = make ({ 1.0, 100.0, 0.0, 0.25 }, 0.1, ":1");
        band[2] = make ({ 0.01, 1000.0, 0.0, 0.2 }, 0.01, " ms");
        band[3] = make ({ 1.0, 5000.0, 0.0, 0.25 }, 1.0, " ms");
        band[4] = make ({ -48.0, 48.0 }, 0.1, " dB");
    }

    for (auto* text : { "Low crossover", "High crossover", "Output" })
    {
        auto* l = labels.add (new juce::Label ({}, text));
        l->setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (l);
    }

    linearPhase.setTooltip ("The band split as symmetric impulses: the same band levels with no phase turned, so the bands add back to the sound exactly "
                            "as it was and its peaks are held; " + juce::String (Split::linearLatency) + " frames late when played live (181 ms at 48 kHz)");
    linearPhase.onClick = [this]
    {
        if (onChange != nullptr)
            onChange();
    };
    addAndMakeVisible (linearPhase);

    for (auto* text : rowNames)
    {
        auto* l = labels.add (new juce::Label ({}, text));
        l->setFont (juce::FontOptions (12.0f));
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    }

    setSettings (Compressor());
    setSize (720, 270);
}

Compressor Panel::getSettings() const
{
    Compressor c;
    c.lowCrossover = lowCrossover->getValue();
    c.highCrossover = highCrossover->getValue();
    c.outputDb = output->getValue();
    c.linearPhase = linearPhase.getToggleState();

    for (size_t b = 0; b < 3; ++b)
        c.bands[b] = { bands[b][0]->getValue(), bands[b][1]->getValue(), Dynamics::Compressor().kneeDb, bands[b][2]->getValue(),
                       bands[b][3]->getValue(), bands[b][4]->getValue() };

    return c;
}

void Panel::setSettings (const Compressor& c)
{
    const auto set = [] (juce::Slider* s, double v) { s->setValue (v, juce::dontSendNotification); };
    set (lowCrossover, c.lowCrossover);
    set (highCrossover, c.highCrossover);
    set (output, c.outputDb);
    linearPhase.setToggleState (c.linearPhase, juce::dontSendNotification);

    for (size_t b = 0; b < 3; ++b)
    {
        set (bands[b][0], c.bands[b].thresholdDb);
        set (bands[b][1], c.bands[b].ratio);
        set (bands[b][2], c.bands[b].attackMs);
        set (bands[b][3], c.bands[b].releaseMs);
        set (bands[b][4], c.bands[b].makeupDb);
    }

    repaint();
}

void Panel::setReductionDb (const std::array<double, 3>& db)
{
    // Rounded to a tenth of a dB, so a steady signal doesn't repaint.
    std::array<double, 3> rounded {};

    for (size_t b = 0; b < 3; ++b)
        rounded[b] = std::round (db[b] * 10.0) / 10.0;

    if (reductionDb != rounded)
    {
        reductionDb = rounded;
        repaint();
    }
}

void Panel::paint (juce::Graphics& g)
{
    // Over each column, the band's name and the frequencies it covers.
    auto r = getLocalBounds().reduced (6);
    r.removeFromTop (56);
    auto header = r.removeFromTop (34);
    header.removeFromLeft (80);
    const auto width = header.getWidth() / 3;
    const auto low = lowCrossover->getValue(), high = highCrossover->getValue();
    const std::array<juce::String, 3> names { "Low", "Mid", "High" };
    const std::array<juce::String, 3> spans { "below " + hertz (low), hertz (low) + " to " + hertz (high), "above " + hertz (high) };

    for (size_t b = 0; b < 3; ++b)
    {
        auto column = header.removeFromLeft (width).reduced (4, 0);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (names[b], column.removeFromTop (17), juce::Justification::centred);
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));

        if (! reductionDb)
        {
            g.drawText (spans[b], column, juce::Justification::centred);
            continue;
        }

        // The span and the reduction, and under them a bar growing from the right as the band is turned down (24 dB full).
        auto text = column.removeFromTop (12);
        g.drawText (spans[b] + ",  " + juce::String ((*reductionDb)[b], 1) + " dB", text, juce::Justification::centred);
        auto bar = column.withSizeKeepingCentre (juce::jmin (120, column.getWidth()), 4);
        g.setColour (Theme::inset);
        g.fillRect (bar);
        g.setColour (Theme::meterHigh);
        g.fillRect (bar.removeFromRight (juce::roundToInt (bar.getWidth() * juce::jlimit (0.0, 1.0, -(*reductionDb)[b] / 24.0))));
    }
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (48);
    const auto topWidth = top.getWidth() / 4;

    for (int i = 0; i < 3; ++i)
    {
        auto cell = top.removeFromLeft (topWidth).reduced (4, 0);
        labels[i]->setBounds (cell.removeFromTop (20));
        sliders[i]->setBounds (cell.removeFromTop (24));
    }

    auto cell = top.reduced (4, 0);
    cell.removeFromTop (20);
    linearPhase.setBounds (cell.removeFromTop (24));

    r.removeFromTop (8);
    r.removeFromTop (34);   // the bands' names, painted
    const auto rowHeight = juce::jmin (32, r.getHeight() / 5);

    for (size_t row = 0; row < 5; ++row)
    {
        auto line = r.removeFromTop (rowHeight);
        labels[3 + (int) row]->setBounds (line.removeFromLeft (80));
        const auto width = line.getWidth() / 3;

        for (size_t b = 0; b < 3; ++b)
            bands[b][row]->setBounds (line.removeFromLeft (width).reduced (4, 4));
    }
}

//==============================================================================
bool isValid (const DeEsser& d, double sampleRate)
{
    return sampleRate > 0.0 && inRange (d.frequency, 1000.0, 16000.0) && d.frequency < sampleRate / 2.0 && inRange (d.thresholdDb, -60.0, 0.0)
        && inRange (d.maxReductionDb, 0.0, 24.0);
}

void DeEsserCore::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    split.prepare (numChannels, rate);
    set (settings);
    reset();
}

void DeEsserCore::reset() noexcept
{
    split.reset();
    gainDb = 0.0;
}

void DeEsserCore::set (const DeEsser& d)
{
    settings = d;
    split.set (d.frequency, std::nullopt, d.linearPhase);
    attack = Dynamics::smoothingCoefficient (attackMs, rate);
    release = Dynamics::smoothingCoefficient (releaseMs, rate);
}

void DeEsserCore::setListening (bool on) noexcept
{
    listening = on;
}

void DeEsserCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, juce::jmin (64, split.getNumChannels()));

    for (int done = 0; done < numFrames && n > 0;)
    {
        const auto frames = juce::jmin (Split::chunk, numFrames - done);
        std::array<const float*, 64> in {};

        for (int ch = 0; ch < n; ++ch)
            in[(size_t) ch] = channels[ch] + done;

        split.run (in.data(), n, frames);

        for (int i = 0; i < frames; ++i)
        {
            double level = 0.0;

            for (int ch = 0; ch < n; ++ch)
                level = juce::jmax (level, std::abs (split.band (ch, 1)[i]));

            // Down by however far the band is over the threshold, up to the reduction.
            const auto overDb = level > 0.0 ? 20.0 * std::log10 (level) - settings.thresholdDb : -1.0;
            const auto target = -juce::jlimit (0.0, settings.maxReductionDb, overDb);
            const auto coefficient = target < gainDb ? attack : release;
            gainDb = target + coefficient * (gainDb - target);
            const auto factor = std::pow (10.0, gainDb / 20.0);

            for (int ch = 0; ch < n; ++ch)
            {
                const auto below = split.band (ch, 0)[i], above = split.band (ch, 1)[i];
                const auto turned = settings.wholeSound ? below + above : above;   // the part being turned down
                const auto kept = settings.wholeSound ? 0.0 : below;
                channels[ch][done + i] = (float) (listening ? turned * (1.0 - factor) : kept + turned * factor);
            }
        }

        done += frames;
    }
}

AudioEdits::Snapshot deEss (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const DeEsser& d)
{
    if (! isValid (d, sampleRate))
        return nullptr;

    DeEsserCore core;
    core.prepare (s.getNumChannels(), sampleRate);
    core.set (d);
    return runPrimed (s, start, end, sampleRate, core);
}

//==============================================================================
bool isValid (const Width& w, double sampleRate)
{
    const auto top = juce::jmin (20000.0, 0.45 * sampleRate);

    if (sampleRate <= 0.0 || w.lowCrossover < 20.0 || w.highCrossover > top || w.highCrossover < 2.0 * w.lowCrossover)
        return false;

    for (auto p : w.percent)
        if (p < 0.0 || p > 200.0)
            return false;

    return true;
}

Width spaced (Width w, double sampleRate)
{
    const auto top = juce::jmin (20000.0, 0.45 * sampleRate);
    w.lowCrossover = juce::jlimit (20.0, top / 2.0, w.lowCrossover);
    w.highCrossover = juce::jlimit (2.0 * w.lowCrossover, top, w.highCrossover);
    return w;
}

void WidthCore::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    channelsPrepared = juce::jmin (64, numChannels);
    split.prepare (channelsPrepared, rate);
    set (settings);
    reset();
}

void WidthCore::reset() noexcept
{
    split.reset();
}

void WidthCore::set (const Width& w)
{
    settings = w;
    split.set (w.lowCrossover, w.highCrossover, w.linearPhase);

    for (size_t b = 0; b < 3; ++b)
        factors[b] = w.percent[b] / 100.0;
}

void WidthCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, channelsPrepared);

    // In minimum phase a mono sound, and the channels past the first pair, pass as they are; in
    // linear phase they go through the split too, so they come out late with the pair, bit for bit.
    if (n <= 0 || (! settings.linearPhase && n < 2))
        return;

    const auto worked = settings.linearPhase ? n : 2;

    for (int done = 0; done < numFrames;)
    {
        const auto frames = juce::jmin (Split::chunk, numFrames - done);
        std::array<const float*, 64> in {};

        for (int ch = 0; ch < worked; ++ch)
            in[(size_t) ch] = channels[ch] + done;

        split.run (in.data(), worked, frames);

        if (n >= 2)
        {
            for (int i = 0; i < frames; ++i)
            {
                double left = 0.0, right = 0.0;

                for (int b = 0; b < 3; ++b)
                {
                    const auto l = split.band (0, b)[i], r = split.band (1, b)[i];
                    const auto mid = (l + r) / 2.0, side = (l - r) / 2.0 * factors[(size_t) b];
                    left += mid + side;
                    right += mid - side;
                }

                channels[0][done + i] = (float) left;
                channels[1][done + i] = (float) right;
            }
        }

        for (int ch = n >= 2 ? 2 : 0; ch < worked; ++ch)
            juce::FloatVectorOperations::copy (channels[ch] + done, split.same (ch), frames);

        done += frames;
    }
}

AudioEdits::Snapshot widen (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Width& w)
{
    if (! isValid (w, sampleRate))
        return nullptr;

    WidthCore core;
    core.prepare (s.getNumChannels(), sampleRate);
    core.set (w);
    return runPrimed (s, start, end, sampleRate, core);
}
}
