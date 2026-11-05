#include "Analysis.h"
#include "Theme.h"

namespace Analysis
{
std::vector<float> spectrumDb (const float* samples, int numSamples)
{
    static juce::dsp::FFT fft (fftOrder);
    static juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);

    std::vector<float> data ((size_t) fftSize * 2, 0.0f);
    std::copy (samples, samples + juce::jlimit (0, fftSize, numSamples), data.begin());
    window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    // A sine of amplitude A on a bin reads A * N / 4 through a Hann window (its coherent gain is a half).
    std::vector<float> out ((size_t) fftSize / 2 + 1);

    for (size_t i = 0; i < out.size(); ++i)
        out[i] = juce::Decibels::gainToDecibels (data[i] * 4.0f / (float) fftSize, -150.0f);

    return out;
}

float levelAt (const std::vector<float>& spectrum, double frequency, double sampleRate)
{
    if (spectrum.empty() || sampleRate <= 0.0)
        return -150.0f;

    const auto binOf = [&] (double f) { return juce::jlimit (0, (int) spectrum.size() - 1, (int) std::lround (f * fftSize / sampleRate)); };
    const auto from = binOf (frequency / std::pow (2.0, 1.0 / 12.0)), to = binOf (frequency * std::pow (2.0, 1.0 / 12.0));
    auto best = -150.0f;

    for (int i = from; i <= to; ++i)
        best = juce::jmax (best, spectrum[(size_t) i]);

    return best;
}

float correlation (const float* left, const float* right, int numSamples)
{
    double lr = 0.0, ll = 0.0, rr = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        lr += (double) left[i] * right[i];
        ll += (double) left[i] * left[i];
        rr += (double) right[i] * right[i];
    }

    if (ll <= 1.0e-12 || rr <= 1.0e-12)
        return 0.0f;

    return (float) juce::jlimit (-1.0, 1.0, lr / std::sqrt (ll * rr));
}

juce::Point<float> goniometerPoint (float left, float right)
{
    return { (left - right) * -0.5f, (left + right) * 0.5f };
}

//==============================================================================
void OutputTap::write (const juce::AudioBuffer<float>& buffer)
{
    const auto start = written.load (std::memory_order_relaxed);
    const auto channels = buffer.getNumChannels();

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto at = (int) ((start + i) & (size - 1));
        frames[0][at] = channels > 0 ? buffer.getSample (0, i) : 0.0f;
        frames[1][at] = channels > 1 ? buffer.getSample (1, i) : frames[0][at];
    }

    written.store (start + buffer.getNumSamples(), std::memory_order_release);
}

void OutputTap::copyRecent (juce::AudioBuffer<float>& dest) const
{
    const auto count = juce::jmin (dest.getNumSamples(), size);
    const auto end = written.load (std::memory_order_acquire);
    dest.clear();

    for (int i = 0; i < count; ++i)
    {
        const auto frame = end - count + i;

        if (frame < 0)
            continue;

        const auto at = (int) (frame & (size - 1));

        for (int ch = 0; ch < juce::jmin (2, dest.getNumChannels()); ++ch)
            dest.setSample (ch, i, frames[ch][at]);
    }
}

//==============================================================================
namespace
{
    const juce::Colour& background = Theme::base;   // the theme's colours, read as they are when painting
    const juce::Colour& grid = Theme::line;
    const juce::Colour& label = Theme::dimText;
    const juce::Colour& curve = Theme::accent;

    juce::String frequencyText (double f)
    {
        return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
    }
}

SpectrumPanel::SpectrumPanel()
{
    startTimerHz (30);
}

float SpectrumPanel::xFor (double frequency, float width)
{
    return width * (float) (std::log (juce::jlimit (20.0, 20000.0, frequency) / 20.0) / std::log (1000.0));
}

double SpectrumPanel::frequencyAt (float x, float width)
{
    return 20.0 * std::pow (1000.0, (double) juce::jlimit (0.0f, 1.0f, x / juce::jmax (1.0f, width)));
}

void SpectrumPanel::take (const std::vector<float>& spectrum, double sampleRate)
{
    rate = sampleRate;

    if (shown.size() != spectrum.size())
    {
        shown = spectrum;
        return;
    }

    for (size_t i = 0; i < shown.size(); ++i)
        shown[i] = juce::jmax (spectrum[i], shown[i] - 0.6f);
}

void SpectrumPanel::timerCallback()
{
    if (readOutput == nullptr || ! isShowing())
        return;

    const auto sampleRate = readOutput (recent);

    if (sampleRate <= 0.0)
        return;

    std::vector<float> mono ((size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
        mono[(size_t) i] = 0.5f * (recent.getSample (0, i) + recent.getSample (1, i));

    take (spectrumDb (mono.data(), fftSize), sampleRate);
    repaint();
}

juce::Rectangle<float> SpectrumPanel::plotArea() const
{
    return getLocalBounds().toFloat().reduced (8.0f).withTrimmedLeft (26.0f).withTrimmedBottom (14.0f);
}

void SpectrumPanel::paint (juce::Graphics& g)
{
    g.fillAll (background);
    const auto plot = plotArea();

    if (plot.getWidth() < 20.0f || plot.getHeight() < 20.0f)
        return;

    const auto yFor = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, db / floorDb); };
    g.setFont (juce::FontOptions (10.0f));

    for (auto db = 0; db >= (int) floorDb; db -= 10)
    {
        const auto y = yFor ((float) db);
        g.setColour (grid);
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);

        if (db % 20 == 0)
        {
            g.setColour (label);
            g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX() - 28.0f, y - 6.0f, 24.0f, 12.0f), juce::Justification::centredRight);
        }
    }

    for (auto f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const auto x = plot.getX() + xFor (f, plot.getWidth());
        g.setColour (grid);
        g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
        g.setColour (label);
        g.drawText (f >= 1000.0 ? juce::String (juce::roundToInt (f / 1000.0)) + "k" : juce::String (juce::roundToInt (f)),
                    juce::Rectangle<float> (x - 16.0f, plot.getBottom() + 1.0f, 32.0f, 12.0f), juce::Justification::centred);
    }

    if (shown.empty() || rate <= 0.0)
    {
        g.setColour (label);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("The spectrum of what plays shows here.", plot, juce::Justification::centred);
        return;
    }

    // Each pixel shows the loudest bin it covers; where bins are wider than pixels (the low end),
    // the level between the two nearest bins, so the curve doesn't step.
    juce::Path line;
    const auto bins = (int) shown.size();

    for (int px = 0; px <= (int) plot.getWidth(); ++px)
    {
        const auto f0 = frequencyAt ((float) px, plot.getWidth()), f1 = frequencyAt ((float) px + 1.0f, plot.getWidth());
        const auto at = f0 * fftSize / rate;
        auto level = -150.0f;

        if ((f1 - f0) * fftSize / rate < 1.0)
        {
            const auto below = juce::jlimit (0, bins - 1, (int) at), above = juce::jmin (bins - 1, below + 1);
            level = juce::jmap ((float) (at - below), shown[(size_t) below], shown[(size_t) above]);
        }
        else
        {
            const auto from = juce::jlimit (0, bins - 1, (int) std::lround (at));
            const auto to = juce::jlimit (from, bins - 1, (int) std::lround (f1 * fftSize / rate) - 1);

            for (int i = from; i <= to; ++i)
                level = juce::jmax (level, shown[(size_t) i]);
        }

        const auto point = juce::Point<float> (plot.getX() + (float) px, yFor (level));

        if (px == 0)
            line.startNewSubPath (point);
        else
            line.lineTo (point);
    }

    auto fill = line;
    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.lineTo (plot.getX(), plot.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient::vertical (curve.withAlpha (0.35f), plot.getY(), curve.withAlpha (0.05f), plot.getBottom()));
    g.fillPath (fill);
    g.setColour (curve);
    g.strokePath (line, juce::PathStrokeType (1.5f));

    if (hover.has_value() && plot.contains (*hover))
    {
        const auto f = frequencyAt (hover->x - plot.getX(), plot.getWidth());
        g.setColour (Theme::overlay.withAlpha (0.4f));
        g.fillRect (hover->x, plot.getY(), 1.0f, plot.getHeight());
        g.setColour (Theme::textStrong);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (frequencyText (f) + "   " + juce::String (levelAt (shown, f, rate), 1) + " dB",
                    plot.withHeight (16.0f).reduced (4.0f, 0.0f), juce::Justification::centredRight);
    }
}

//==============================================================================
PhasePanel::PhasePanel()
{
    startTimerHz (30);
}

void PhasePanel::take (const juce::AudioBuffer<float>& block)
{
    const auto n = block.getNumSamples();
    const auto* left = block.getReadPointer (0);
    const auto* right = block.getReadPointer (juce::jmin (1, block.getNumChannels() - 1));
    const auto value = correlation (left, right, n);
    shownCorrelation = first ? value : shownCorrelation + 0.25f * (value - shownCorrelation);
    first = false;

    points.clear();

    for (int i = 0; i < n; i += 2)
        points.push_back (goniometerPoint (left[i], right[i]));
}

void PhasePanel::timerCallback()
{
    if (readOutput == nullptr || ! isShowing())
        return;

    if (readOutput (recent) <= 0.0)
        return;

    take (recent);
    repaint();
}

void PhasePanel::paint (juce::Graphics& g)
{
    g.fillAll (background);
    auto r = getLocalBounds().toFloat().reduced (10.0f);
    auto barArea = r.removeFromBottom (34.0f);
    r.removeFromBottom (6.0f);

    const auto side = juce::jmin (r.getWidth(), r.getHeight());
    const auto scope = r.withSizeKeepingCentre (side, side);
    const auto centre = scope.getCentre();
    const auto radius = side * 0.5f;

    if (radius > 10.0f)
    {
        g.setColour (grid);
        juce::Path diamond;
        diamond.addPolygon (centre, 4, radius, 0.0f);
        g.strokePath (diamond, juce::PathStrokeType (1.0f));
        g.drawLine ({ centre.translated (0.0f, -radius), centre.translated (0.0f, radius) }, 1.0f);
        g.drawLine ({ centre.translated (-radius, 0.0f), centre.translated (radius, 0.0f) }, 1.0f);
        g.drawLine ({ centre.translated (-radius * 0.5f, -radius * 0.5f), centre.translated (radius * 0.5f, radius * 0.5f) }, 1.0f);
        g.drawLine ({ centre.translated (radius * 0.5f, -radius * 0.5f), centre.translated (-radius * 0.5f, radius * 0.5f) }, 1.0f);

        g.setColour (label);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("M", juce::Rectangle<float> (centre.x - 8.0f, scope.getY() - 2.0f, 16.0f, 12.0f), juce::Justification::centred);
        g.drawText ("L", juce::Rectangle<float> (centre.x - radius * 0.5f - 14.0f, centre.y - radius * 0.5f - 14.0f, 12.0f, 12.0f), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (centre.x + radius * 0.5f + 2.0f, centre.y - radius * 0.5f - 14.0f, 12.0f, 12.0f), juce::Justification::centred);

        g.setColour (curve.withAlpha (0.55f));

        for (auto p : points)
        {
            const auto at = centre + juce::Point<float> (juce::jlimit (-1.0f, 1.0f, p.x), -juce::jlimit (-1.0f, 1.0f, p.y)) * radius;
            g.fillRect (at.x, at.y, 1.5f, 1.5f);
        }
    }

    // The correlation, -1 to +1.
    auto bar = barArea.removeFromTop (12.0f);
    g.setColour (Theme::inset);
    g.fillRoundedRectangle (bar, 3.0f);
    const auto zero = bar.getCentreX();
    const auto x = zero + shownCorrelation * bar.getWidth() * 0.5f;
    g.setColour (shownCorrelation < 0.0f ? Theme::danger : Theme::good);
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zero, x), bar.getY() + 2.0f, juce::jmax (zero, x), bar.getBottom() - 2.0f));
    g.setColour (Theme::textStrong);
    g.fillRect (x - 1.0f, bar.getY(), 2.0f, bar.getHeight());

    g.setColour (label);
    g.setFont (juce::FontOptions (10.0f));
    auto labels = barArea.withTrimmedTop (2.0f);
    g.drawText ("-1", labels, juce::Justification::topLeft);
    g.drawText ("+1", labels, juce::Justification::topRight);
    g.setColour (Theme::textStrong);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    g.drawText ((shownCorrelation >= 0.0f ? "+" : "") + juce::String (shownCorrelation, 2), labels, juce::Justification::centredTop);
}
}
