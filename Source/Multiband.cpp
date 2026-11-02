#include "Multiband.h"

namespace Multiband
{
namespace
{
    constexpr double butterworthQ = 0.7071067811865476;

    bool inRange (double v, double low, double high)    { return std::isfinite (v) && v >= low && v <= high; }

    /** Runs a core over [start, end) after priming it on up to a second of the audio before, which is
        kept as it was, so the range starts as it would in a longer pass (as the EQ does). */
    template <typename Core>
    AudioEdits::Snapshot runPrimed (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, Core& core)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);

        if (start == end || end - start > std::numeric_limits<int>::max() / 2)
            return nullptr;

        const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));

        return AudioEdits::processRange (s, start - prime, end, [&core, prime] (juce::AudioBuffer<float>& b)
        {
            juce::AudioBuffer<float> before (b.getNumChannels(), prime);

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                before.copyFrom (ch, 0, b, ch, 0, prime);

            core.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.copyFrom (ch, 0, before, ch, 0, prime);
        });
    }
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
    first.assign ((size_t) juce::jmax (0, numChannels), {});
    second.assign (first.size(), {});
    lowAlign.assign (first.size(), {});
    parts.assign (first.size(), {});
    set (settings);
    reset();
}

void CompressorCore::reset() noexcept
{
    for (auto* group : { &first, &second, &lowAlign })
        for (auto& c : *group)
            c.reset();

    for (auto& c : cores)
        c.reset();

    reduction = {};
}

void CompressorCore::set (const Compressor& c)
{
    settings = c;

    for (size_t ch = 0; ch < first.size(); ++ch)
    {
        first[ch].set (c.lowCrossover, rate);
        second[ch].set (c.highCrossover, rate);
        lowAlign[ch].set (c.highCrossover, rate);
    }

    for (size_t b = 0; b < cores.size(); ++b)
        cores[b].set (c.bands[b], rate);

    output = std::pow (10.0, c.outputDb / 20.0);
}

void CompressorCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) first.size());

    for (int i = 0; i < numFrames; ++i)
    {
        std::array<double, 3> loudest {};

        for (int ch = 0; ch < n; ++ch)
        {
            // Low and the rest at the low crossover; the rest into middle and high at the high one;
            // the low band turned as the high split turns the others, so the three add up flat.
            double low = 0.0, rest = 0.0, middle = 0.0, high = 0.0;
            first[(size_t) ch].split (channels[ch][i], low, rest);
            second[(size_t) ch].split (rest, middle, high);
            low = lowAlign[(size_t) ch].allPass (low);
            parts[(size_t) ch] = { low, middle, high };

            for (size_t b = 0; b < 3; ++b)
                loudest[b] = juce::jmax (loudest[b], std::abs (parts[(size_t) ch][b]));
        }

        std::array<double, 3> factors {};

        for (size_t b = 0; b < 3; ++b)
        {
            factors[b] = cores[b].next (loudest[b]) * output;
            reduction[b] = 20.0 * std::log10 (factors[b] / output) - settings.bands[b].makeupDb;
        }

        for (int ch = 0; ch < n; ++ch)
        {
            const auto& p = parts[(size_t) ch];
            channels[ch][i] = (float) (p[0] * factors[0] + p[1] * factors[1] + p[2] * factors[2]);
        }
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

    for (auto* text : rowNames)
    {
        auto* l = labels.add (new juce::Label ({}, text));
        l->setFont (juce::FontOptions (12.0f));
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    }

    setSettings (Compressor());
    setSize (620, 270);
}

Compressor Panel::getSettings() const
{
    Compressor c;
    c.lowCrossover = lowCrossover->getValue();
    c.highCrossover = highCrossover->getValue();
    c.outputDb = output->getValue();

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
        g.setColour (juce::Colour (0xffd8dde3));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (names[b], column.removeFromTop (17), juce::Justification::centred);
        g.setColour (juce::Colours::grey);
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
        g.setColour (juce::Colour (0xff1a1f24));
        g.fillRect (bar);
        g.setColour (juce::Colour (0xffe0a040));
        g.fillRect (bar.removeFromRight (juce::roundToInt (bar.getWidth() * juce::jlimit (0.0, 1.0, -(*reductionDb)[b] / 24.0))));
    }
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (48);
    const auto topWidth = top.getWidth() / 3;

    for (int i = 0; i < 3; ++i)
    {
        auto cell = top.removeFromLeft (topWidth).reduced (4, 0);
        labels[i]->setBounds (cell.removeFromTop (20));
        sliders[i]->setBounds (cell.removeFromTop (24));
    }

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
}
