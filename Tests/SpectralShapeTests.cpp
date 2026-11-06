#include <JuceHeader.h>
#include "../Source/SpectralEdit.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/WaveformView.h"
#include "TestUtils.h"
#include "SpectralShapeTests.h"

namespace
{
    using TestUtils::check;
    using SpectralEdit::Shape;
    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double amplitude (const juce::AudioBuffer<float>& b, int channel, double frequency, int from, int n)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + n; ++i)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency * i / rate;
            re += b.getSample (channel, i) * std::cos (w);
            im += b.getSample (channel, i) * std::sin (w);
        }

        return 2.0 * std::sqrt (re * re + im * im) / n;
    }

    /** Power from lo to hi Hz in one Hann-windowed 4096-sample frame starting at from. */
    double bandPower (const juce::AudioBuffer<float>& b, int channel, int from, double lo, double hi)
    {
        constexpr int order = 12, size = 1 << order;
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann, false);
        std::vector<float> data (2 * size, 0.0f);
        std::copy_n (b.getReadPointer (channel, from), size, data.begin());
        window.multiplyWithWindowingTable (data.data(), size);
        fft.performFrequencyOnlyForwardTransform (data.data());
        double sum = 0.0;

        for (int k = (int) std::ceil (lo * size / rate); k < (int) (hi * size / rate); ++k)
            sum += juce::square ((double) data[(size_t) k]);

        return sum;
    }

    double db (double ratio)        { return 20.0 * std::log10 (juce::jmax (ratio, 1.0e-12)); }
    double powerDb (double ratio)   { return 10.0 * std::log10 (juce::jmax (ratio, 1.0e-24)); }
    juce::String r (double v, int places = 1)   { return juce::String (v, places); }

    /** A rectangle as a lasso, frames by Hz. */
    Shape box (double fromFrame, double toFrame, double loHz, double hiHz)
    {
        Shape s;
        s.lassos.push_back ({ { fromFrame, std::log2 (loHz) }, { toFrame, std::log2 (loHz) },
                              { toFrame, std::log2 (hiHz) }, { fromFrame, std::log2 (hiHz) } });
        return s;
    }

    void testGeometry()
    {
        Shape s;
        s.lassos.push_back ({ { 0.0, 8.0 }, { 1000.0, 8.0 }, { 500.0, 12.0 } });   // a triangle, 256 Hz to 4 kHz at its tip
        const auto mid = s.octavesAt (500.0), quarter = s.octavesAt (250.0);
        s.dabs.push_back ({ 2000.0, 10.0, 100.0, 0.5 });
        const auto frames = s.frames();
        const auto band = s.frequencies();

        check (mid.size() == 1 && std::abs (mid[0].getStart() - 8.0) < 1.0e-9 && std::abs (mid[0].getEnd() - 12.0) < 1.0e-9
                   && quarter.size() == 1 && std::abs (quarter[0].getEnd() - 10.0) < 1.0e-9
                   && s.contains (500.0, 512.0) && ! s.contains (100.0, 2048.0) && ! s.contains (1500.0, 1024.0)
                   && s.contains (2000.0, std::exp2 (10.4)) && ! s.contains (2090.0, std::exp2 (10.4)) && s.contains (2090.0, 1024.0)
                   && juce::exactlyEqual (frames.getStart(), 0.0) && juce::exactlyEqual (frames.getEnd(), 2100.0)
                   && std::abs (band.getStart() - 256.0) < 1.0e-6 && std::abs (band.getEnd() - 4096.0) < 1.0e-6,
               "shapes: a lasso holds the frequencies between its edges at each frame and a brush dab its ellipse, together, "
               "with their span in time and frequency");

        Shape thin;
        thin.lassos.push_back ({ { 0.0, 8.0 }, { 10.0, 9.0 } });
        check (thin.isEmpty() && SpectralEdit::maskFor (thin, rate, 48000).rows.empty() && Shape {}.isEmpty(),
               "shapes: an outline of fewer than three points is no shape, and has no mask");
    }

    void testCut()
    {
        juce::AudioBuffer<float> b (2, (int) (3 * rate));

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            const auto v = (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * 500.0 * t)
                                    + 0.25 * std::sin (juce::MathConstants<double>::twoPi * 4000.0 * t));
            b.setSample (0, i, v);
            b.setSample (1, i, -v);
        }

        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (b));
        const auto from = (int) rate, to = (int) (2 * rate);
        const auto cut = SpectralEdit::changeShape (*source, box (from, to, 3000.0, 5500.0), rate, -std::numeric_limits<float>::infinity());
        const auto after = readAll (*cut);
        const auto inside = (int) (1.2 * rate), span = (int) (0.6 * rate);
        const auto gone = db (amplitude (after, 0, 4000.0, inside, span) / amplitude (b, 0, 4000.0, inside, span));
        const auto goneRight = db (amplitude (after, 1, 4000.0, inside, span) / amplitude (b, 1, 4000.0, inside, span));
        const auto kept = db (amplitude (after, 0, 500.0, inside, span) / amplitude (b, 0, 500.0, inside, span));
        const auto earlier = db (amplitude (after, 0, 4000.0, (int) (0.2 * rate), span) / amplitude (b, 0, 4000.0, (int) (0.2 * rate), span));

        // Untouched: everything more than the reach (half a window, the fade's two frames, and the
        // windows under them) from the shape.
        const auto reach = SpectralEdit::shapeSize + 2 * SpectralEdit::shapeHop;
        bool untouched = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (i < from - reach || i >= to + reach)
                    untouched = untouched && juce::exactlyEqual (after.getSample (ch, i), b.getSample (ch, i));

        check (gone < -60.0 && goneRight < -60.0 && std::abs (kept) < 0.05 && std::abs (earlier) < 0.01 && untouched,
               "shapes: cutting a lasso drawn over a 4 kHz tone from 1 s to 2 s takes it down " + r (-gone, 0) + " dB on both channels, "
                   "leaves a 500 Hz tone beside it within " + r (std::abs (kept), 3) + " dB and the 4 kHz tone before it within "
                   + r (std::abs (earlier), 3) + " dB, and every sample more than " + juce::String (reach) + " from the shape bit for bit");

        Shape brush;

        for (double f = from; f <= to; f += 2400.0)
            brush.dabs.push_back ({ f, std::log2 (4000.0), 2400.0, 0.3 });

        const auto brushed = readAll (*SpectralEdit::changeShape (*source, brush, rate, -12.0f));
        const auto down = db (amplitude (brushed, 0, 4000.0, inside, span) / amplitude (b, 0, 4000.0, inside, span));
        check (std::abs (down + 12.0) < 0.1,
               "shapes: a brush stroke along the 4 kHz tone at -12 dB lowers it by " + r (-down, 2) + " dB");
    }

    void testHeal()
    {
        juce::Random random (7);
        juce::AudioBuffer<float> noise (1, (int) (3 * rate));

        for (int i = 0; i < noise.getNumSamples(); ++i)
            noise.setSample (0, i, 0.055f * (2.0f * random.nextFloat() - 1.0f));   // white, about -30 dBFS RMS

        auto marred = noise;
        const auto burst = (int) rate, burstLength = (int) (0.03 * rate);

        for (int i = burst; i < burst + burstLength; ++i)
            marred.addSample (0, i, (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 3000.0 * i / rate)));

        const auto click = (int) (2 * rate);
        marred.addSample (0, click, 0.8f);

        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (marred));
        auto shape = box (burst, burst + burstLength, 2500.0, 3600.0);
        const auto healedBurst = readAll (*SpectralEdit::heal (*source, shape, rate));
        const auto at = burst + burstLength / 2 - 2048;
        const auto clean = bandPower (noise, 0, at, 2500.0, 3600.0);
        const auto wasUp = powerDb (bandPower (marred, 0, at, 2500.0, 3600.0) / clean);
        const auto nowUp = powerDb (bandPower (healedBurst, 0, at, 2500.0, 3600.0) / clean);
        const auto below = powerDb (bandPower (healedBurst, 0, at, 300.0, 2000.0) / bandPower (marred, 0, at, 300.0, 2000.0));
        check (wasUp > 20.0 && std::abs (nowUp) < 1.5 && std::abs (below) < 0.5,
               "heal: a 30 ms 3 kHz burst in noise, " + r (wasUp) + " dB above the noise in its band, is healed to " + r (nowUp)
                   + " dB from the noise alone, the band below the shape changed by " + r (below, 2) + " dB");

        const auto clickShape = box (click - 500, click + 500, 30.0, 23000.0);
        const auto healedClick = readAll (*SpectralEdit::heal (*source, clickShape, rate));
        const auto peakBefore = marred.getMagnitude (0, click - 1000, 2000);
        const auto peakAfter = healedClick.getMagnitude (0, click - 1000, 2000);
        const auto noiseRms = noise.getRMSLevel (0, click - 4800, 9600);
        const auto level = powerDb (juce::square ((double) healedClick.getRMSLevel (0, click - 2048, 4096))
                                    / juce::square ((double) noise.getRMSLevel (0, click - 2048, 4096)));
        check (peakAfter < 5.0f * noiseRms && std::abs (level) < 1.0,
               "heal: a click of 0.8 in that noise (" + r (peakBefore / noiseRms) + " times its RMS) is healed to a peak of " + r (peakAfter, 3)
                   + " (" + r (peakAfter / noiseRms) + " times), the level around it " + r (level, 2) + " dB from the noise's");
    }
}

namespace
{
    void testHealThroughTone()
    {
        juce::AudioBuffer<float> b (1, (int) (4 * rate));
        juce::Random random (11);
        const auto cough = (int) (2 * rate), coughLength = (int) (0.05 * rate);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            auto v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate);

            if (i >= cough && i < cough + coughLength)
                v += 0.3f * (2.0f * random.nextFloat() - 1.0f);

            b.setSample (0, i, v);
        }

        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (b));
        const auto healed = readAll (*SpectralEdit::heal (*source, box (cough, cough + coughLength, 30.0, 24000.0), rate));
        const auto window = (int) (0.1 * rate);
        const auto from = cough + coughLength / 2 - window / 2;
        const auto tone = db (amplitude (healed, 0, 1000.0, from, window) / 0.3);

        // What is left besides the tone, over the cough, against what the cough added.
        double left = 0.0, added = 0.0;

        for (int i = cough; i < cough + coughLength; ++i)
        {
            const auto clean = 0.3 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate);
            left += juce::square (healed.getSample (0, i) - clean);
            added += juce::square (b.getSample (0, i) - clean);
        }

        const auto residue = powerDb (left / added);
        check (std::abs (tone) < 1.0 && residue < -15.0,
               "heal: a 1 kHz tone with a 50 ms burst of loud noise over it, healed, runs on through at " + r (tone, 2)
                   + " dB of its level, what the burst added down " + r (-residue) + " dB");

        juce::AudioBuffer<float> big (2, (int) (12 * rate));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < big.getNumSamples(); ++i)
                big.setSample (ch, i, 0.1f * (2.0f * random.nextFloat() - 1.0f));

        const auto bigSource = AudioEdits::fromBuffer (std::move (big));
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto bigHealed = SpectralEdit::heal (*bigSource, box (rate, 11 * rate, 30.0, 23000.0), rate);
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        check (bigHealed != nullptr && seconds < 10.0,
               "heal: a lasso over 10 s of stereo at every frequency heals in " + r (seconds, 2) + " s");
    }
}

namespace
{
    juce::MouseEvent mouse (juce::Component& c, juce::Point<float> at, juce::Point<float> down, juce::ModifierKeys mods = {})
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                 now, down, now, 1, at != down);
    }

    void drag (WaveformView& view, std::initializer_list<juce::Point<float>> points, juce::ModifierKeys mods = {})
    {
        const auto down = *points.begin();
        view.mouseDown (mouse (view, down, down, mods));

        for (auto p : points)
            view.mouseDrag (mouse (view, p, down, mods));

        view.mouseUp (mouse (view, *(points.end() - 1), down, mods));
    }

    void testTools()
    {
        AudioEngine engine;
        juce::AudioBuffer<float> silence (2, (int) (2 * rate));
        silence.clear();
        engine.document.reset (AudioEdits::fromBuffer (std::move (silence)), rate, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 600);
        view.setDisplay (WaveformView::Display::spectrogram);
        view.zoomToFit();
        const auto area = view.getWaveArea().toFloat();
        const auto top = area.withHeight (area.getHeight() / 2.0f);   // the left channel's spectrogram
        const auto at = [&top] (float fx, float fy) { return juce::Point<float> (top.getX() + top.getWidth() * fx, top.getY() + top.getHeight() * fy); };
        const auto frameAt = [&view, &area] (float x) { return view.getVisibleFrames().getStart() + (double) (x - area.getX()) / area.getWidth() * view.getVisibleFrames().getLength(); };
        const auto hzAt = [&top] (float y) { return Spectrogram::yToFrequency (y - top.getY(), rate, (int) top.getHeight()); };

        view.setSpectralTool (WaveformView::SpectralTool::lasso);
        drag (view, { at (0.2f, 0.3f), at (0.5f, 0.3f), at (0.5f, 0.7f), at (0.2f, 0.7f) });
        const auto& shape = view.getSpectralShape();
        const auto centre = at (0.35f, 0.5f), outside = at (0.6f, 0.5f), above = at (0.35f, 0.1f);
        const auto sel = view.getSelection();
        check (shape.lassos.size() == 1 && shape.lassos[0].size() >= 3 && shape.contains (frameAt (centre.x), hzAt (centre.y))
                   && ! shape.contains (frameAt (outside.x), hzAt (outside.y)) && ! shape.contains (frameAt (above.x), hzAt (above.y))
                   && std::abs ((double) sel.getStart() - frameAt (at (0.2f, 0.0f).x)) < 200.0 && std::abs ((double) sel.getEnd() - frameAt (at (0.5f, 0.0f).x)) < 200.0,
               "spectral tools: a lasso drawn in a spectrogram holds what is inside it and not beside or above it, and selects the time it spans");

        drag (view, { at (0.7f, 0.2f), at (0.9f, 0.2f), at (0.8f, 0.4f) }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        const auto two = view.getSpectralShape().lassos.size() == 2 && view.getSelection().getEnd() > sel.getEnd();
        drag (view, { at (0.1f, 0.5f) });
        check (two && view.getSpectralShape().isEmpty() && view.getSelection().isEmpty(),
               "spectral tools: Shift adds a second lasso and widens the selection; a click without drawing clears both");

        view.setSpectralTool (WaveformView::SpectralTool::brush);
        view.setBrushRadius (12.0f);
        drag (view, { at (0.2f, 0.5f), at (0.3f, 0.5f), at (0.4f, 0.5f) });
        const auto& brushed = view.getSpectralShape();
        const auto mid = at (0.3f, 0.5f), off = at (0.3f, 0.5f) + juce::Point<float> (0.0f, 20.0f);
        const auto fpp = view.getVisibleFrames().getLength() / area.getWidth();
        const auto span = view.getSelection();
        check (brushed.dabs.size() >= 40 && brushed.contains (frameAt (mid.x), hzAt (mid.y)) && brushed.contains (frameAt (mid.x), hzAt (mid.y + 10.0f))
                   && ! brushed.contains (frameAt (off.x), hzAt (off.y))
                   && std::abs ((double) span.getStart() - (frameAt (at (0.2f, 0.0f).x) - 12.0 * fpp)) < 2.0 * fpp
                   && std::abs ((double) span.getEnd() - (frameAt (at (0.4f, 0.0f).x) + 12.0 * fpp)) < 2.0 * fpp,
               "spectral tools: the brush paints a band as wide as its circle along the stroke (" + juce::String ((int) brushed.dabs.size())
                   + " dabs), selecting the time it covers");

        const auto place = view.getPlace();
        view.setSelection ({ 0, 100 });
        const auto cleared = view.getSpectralShape().isEmpty();
        view.setPlace (place);
        check (cleared && view.getSpectralShape().dabs.size() == brushed.dabs.size() && view.getSelection() == span,
               "spectral tools: choosing another selection clears the drawing, and the file's kept place brings it back");

        view.setBrushRadius (500.0f);
        const auto big = view.getBrushRadius();
        view.setBrushRadius (0.0f);
        view.setSpectralTool (WaveformView::SpectralTool::box);
        drag (view, { at (0.2f, 0.5f), at (0.4f, 0.5f) });
        check (juce::exactlyEqual (big, WaveformView::maxBrushRadius) && juce::exactlyEqual (view.getBrushRadius(), WaveformView::minBrushRadius)
                   && view.getSpectralShape().isEmpty() && view.getSelection().getLength() > 0 && view.getSpectralBand().isEmpty(),
               "spectral tools: the brush is 3 to 60 pixels; with the box tool a plain drag in a spectrogram selects time, as before");
    }
}

void runSpectralShapeTests()
{
    testGeometry();
    testCut();
    testHeal();
    testHealThroughTone();
    testTools();
}
