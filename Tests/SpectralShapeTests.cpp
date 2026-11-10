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

    /** A rectangle as a lasso stroke, frames by Hz. */
    SpectralEdit::Stroke outline (double fromFrame, double toFrame, double loHz, double hiHz, bool subtract = false)
    {
        SpectralEdit::Stroke s;
        s.lasso = { { fromFrame, std::log2 (loHz) }, { toFrame, std::log2 (loHz) }, { toFrame, std::log2 (hiHz) }, { fromFrame, std::log2 (hiHz) } };
        s.subtract = subtract;
        return s;
    }

    Shape box (double fromFrame, double toFrame, double loHz, double hiHz)
    {
        Shape s;
        s.strokes.push_back (outline (fromFrame, toFrame, loHz, hiHz));
        return s;
    }

    void testGeometry()
    {
        Shape s;
        SpectralEdit::Stroke triangle;
        triangle.lasso = { { 0.0, 8.0 }, { 1000.0, 8.0 }, { 500.0, 12.0 } };   // 256 Hz to 4 kHz at its tip
        s.strokes.push_back (triangle);
        const auto mid = s.octavesAt (500.0), quarter = s.octavesAt (250.0);
        SpectralEdit::Stroke dab;
        dab.dabs.push_back ({ 2000.0, 10.0, 100.0, 0.5 });
        s.strokes.push_back (dab);
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
        SpectralEdit::Stroke two;
        two.lasso = { { 0.0, 8.0 }, { 10.0, 9.0 } };
        thin.strokes.push_back (two);
        Shape taken;
        taken.strokes.push_back (outline (0.0, 100.0, 100.0, 200.0, true));
        check (thin.isEmpty() && SpectralEdit::maskFor (thin, rate, 48000).rows.empty() && Shape {}.isEmpty() && taken.isEmpty(),
               "shapes: an outline of fewer than three points is no shape, and has no mask; a stroke that only takes is nothing on its own");

        // Taking from a drawing, and the brush's strength: a box over 1 to 2 kHz for frames 0 to
        // 1000, a box taken out of its middle (1.2 to 1.5 kHz, frames 200 to 400), a half-strength
        // brush over 3 kHz, and a half-strength take over the box's upper part.
        Shape drawn = box (0.0, 1000.0, 1000.0, 2000.0);
        drawn.strokes.push_back (outline (200.0, 400.0, 1200.0, 1500.0, true));
        const auto gap = drawn.octavesAt (300.0), whole = drawn.octavesAt (600.0);
        SpectralEdit::Stroke half;
        half.strength = 0.5f;
        half.dabs.push_back ({ 600.0, std::log2 (3000.0), 50.0, 0.2 });
        drawn.strokes.push_back (half);
        auto lighten = outline (0.0, 1000.0, 1700.0, 2000.0, true);
        lighten.strength = 0.5f;
        drawn.strokes.push_back (lighten);
        check (gap.size() == 2 && std::abs (gap[0].getStart() - std::log2 (1000.0)) < 1.0e-9 && std::abs (gap[0].getEnd() - std::log2 (1200.0)) < 1.0e-9
                   && std::abs (gap[1].getStart() - std::log2 (1500.0)) < 1.0e-9 && std::abs (gap[1].getEnd() - std::log2 (2000.0)) < 1.0e-9
                   && whole.size() == 1 && ! drawn.contains (300.0, 1300.0) && drawn.contains (600.0, 1300.0) && drawn.contains (300.0, 1100.0)
                   && juce::exactlyEqual (drawn.weightAt (600.0, 3000.0), 0.5f) && juce::exactlyEqual (drawn.weightAt (600.0, 1100.0), 1.0f)
                   && juce::exactlyEqual (drawn.weightAt (600.0, 1800.0), 0.5f) && juce::exactlyEqual (drawn.frames().getEnd(), 1000.0)
                   && std::abs (drawn.frequencies().getEnd() - 3000.0 * std::exp2 (0.2)) < 1.0e-6,
               "shapes: a stroke drawn with Option takes its part out of the drawing (the box's middle gone at frame 300, whole at 600), "
               "a half-strength brush weighs a half, a half-strength take lowers a full part to a half, and the span is the adding strokes'");
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

        // What plays with the lasso selected: everything outside it over its span removed.
        const auto only = readAll (*SpectralEdit::changeOutsideShape (*source, box (from, to, 3000.0, 5500.0), rate, -std::numeric_limits<float>::infinity()));
        const auto toneKept = db (amplitude (only, 0, 4000.0, inside, span) / amplitude (b, 0, 4000.0, inside, span));
        const auto besideGone = db (amplitude (only, 0, 500.0, inside, span) / amplitude (b, 0, 500.0, inside, span));
        const auto besideGoneRight = db (amplitude (only, 1, 500.0, inside, span) / amplitude (b, 1, 500.0, inside, span));
        bool onlyUntouched = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (i < from - reach || i >= to + reach)
                    onlyUntouched = onlyUntouched && juce::exactlyEqual (only.getSample (ch, i), b.getSample (ch, i));

        check (std::abs (toneKept) < 0.05 && besideGone < -60.0 && besideGoneRight < -60.0 && onlyUntouched,
               "shapes: keeping only the lasso leaves the 4 kHz tone within " + r (std::abs (toneKept), 3) + " dB, takes the 500 Hz tone beside it down "
                   + r (-besideGone, 0) + " dB on both channels, and leaves every sample more than " + juce::String (reach) + " from the shape bit for bit");

        Shape brush;
        brush.strokes.push_back ({});

        for (double f = from; f <= to; f += 2400.0)
            brush.strokes.front().dabs.push_back ({ f, std::log2 (4000.0), 2400.0, 0.3 });

        // A box over the 4 kHz tone from 0.5 s to 2.8 s with 1.3 s to 2.1 s taken back out: gone each side, kept in the middle.
        auto taken = box (0.5 * rate, 2.8 * rate, 3000.0, 5500.0);
        taken.strokes.push_back (outline (1.3 * rate, 2.1 * rate, 3000.0, 5500.0, true));
        const auto afterTake = readAll (*SpectralEdit::changeShape (*source, taken, rate, -std::numeric_limits<float>::infinity()));
        const auto level = [&] (const juce::AudioBuffer<float>& a, double seconds) { return db (amplitude (a, 0, 4000.0, (int) (seconds * rate), span) / amplitude (b, 0, 4000.0, (int) (seconds * rate), span)); };
        const auto goneBefore = level (afterTake, 0.6), keptMiddle = level (afterTake, 1.4), goneAfter = level (afterTake, 2.15);
        check (goneBefore < -60.0 && std::abs (keptMiddle) < 0.01 && goneAfter < -60.0,
               "shapes: a box over 0.5 s to 2.8 s with 1.3 s to 2.1 s taken back out of it removes the tone before (" + r (-goneBefore, 0) + " dB) and after ("
                   + r (-goneAfter, 0) + " dB) and leaves the middle within " + r (std::abs (keptMiddle), 3) + " dB");

        // A half-strength brush removing: half the tone goes (-6.02 dB).
        auto halfBrush = brush;
        halfBrush.strokes.front().strength = 0.5f;
        const auto halved = readAll (*SpectralEdit::changeShape (*source, halfBrush, rate, -std::numeric_limits<float>::infinity()));
        const auto halfDown = db (amplitude (halved, 0, 4000.0, inside, span) / amplitude (b, 0, 4000.0, inside, span));
        check (std::abs (halfDown - -6.02) < 0.1, "shapes: removing with a brush at half strength takes the tone down " + r (-halfDown, 2) + " dB (half)");

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
        check (shape.strokes.size() == 1 && shape.strokes[0].lasso.size() >= 3 && shape.contains (frameAt (centre.x), hzAt (centre.y))
                   && ! shape.contains (frameAt (outside.x), hzAt (outside.y)) && ! shape.contains (frameAt (above.x), hzAt (above.y))
                   && std::abs ((double) sel.getStart() - frameAt (at (0.2f, 0.0f).x)) < 200.0 && std::abs ((double) sel.getEnd() - frameAt (at (0.5f, 0.0f).x)) < 200.0,
               "spectral tools: a lasso drawn in a spectrogram holds what is inside it and not beside or above it, and selects the time it spans");

        drag (view, { at (0.7f, 0.2f), at (0.9f, 0.2f), at (0.8f, 0.4f) }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        const auto two = view.getSpectralShape().strokes.size() == 2 && view.getSelection().getEnd() > sel.getEnd();
        drag (view, { at (0.3f, 0.4f), at (0.45f, 0.4f), at (0.45f, 0.6f), at (0.3f, 0.6f) }, juce::ModifierKeys (juce::ModifierKeys::altModifier));
        const auto& withTake = view.getSpectralShape();
        const auto taken = withTake.strokes.size() == 3 && withTake.strokes.back().subtract && ! withTake.contains (frameAt (at (0.38f, 0.5f).x), hzAt (at (0.38f, 0.5f).y))
                           && withTake.contains (frameAt (at (0.25f, 0.5f).x), hzAt (at (0.25f, 0.5f).y))
                           && view.getSelection().getEnd() > sel.getEnd();
        drag (view, { at (0.1f, 0.5f) });
        check (two && taken && view.getSpectralShape().isEmpty() && view.getSelection().isEmpty(),
               "spectral tools: Shift adds a second lasso and widens the selection, Option draws one that takes from the drawing (its middle no longer "
               "selected, the rest still, the time selection kept); a click without drawing clears both");

        view.setSpectralTool (WaveformView::SpectralTool::brush);
        view.setBrushRadius (12.0f);
        drag (view, { at (0.2f, 0.5f), at (0.3f, 0.5f), at (0.4f, 0.5f) });
        const auto& brushed = view.getSpectralShape();
        const auto mid = at (0.3f, 0.5f), off = at (0.3f, 0.5f) + juce::Point<float> (0.0f, 20.0f);
        const auto fpp = view.getVisibleFrames().getLength() / area.getWidth();
        const auto span = view.getSelection();
        const auto dabCount = brushed.strokes.size() == 1 ? brushed.strokes.front().dabs.size() : (size_t) 0;
        check (dabCount >= 40 && brushed.contains (frameAt (mid.x), hzAt (mid.y)) && brushed.contains (frameAt (mid.x), hzAt (mid.y + 10.0f))
                   && ! brushed.contains (frameAt (off.x), hzAt (off.y))
                   && std::abs ((double) span.getStart() - (frameAt (at (0.2f, 0.0f).x) - 12.0 * fpp)) < 2.0 * fpp
                   && std::abs ((double) span.getEnd() - (frameAt (at (0.4f, 0.0f).x) + 12.0 * fpp)) < 2.0 * fpp,
               "spectral tools: the brush paints a band as wide as its circle along the stroke (" + juce::String ((int) dabCount)
                   + " dabs), selecting the time it covers");

        const auto place = view.getPlace();
        view.setSelection ({ 0, 100 });
        const auto cleared = view.getSpectralShape().isEmpty();
        view.setPlace (place);
        check (cleared && view.getSpectralShape().strokes.size() == 1 && view.getSpectralShape().strokes.front().dabs.size() == dabCount && view.getSelection() == span,
               "spectral tools: choosing another selection clears the drawing, and the file's kept place brings it back");

        view.clearSpectralSelection();
        const auto dropped = view.getSpectralShape().isEmpty() && view.getSpectralBand().isEmpty() && view.getSelection() == span;
        view.setPlace (place);
        check (dropped && view.getSpectralShape().strokes.size() == 1,
               "spectral tools: clearing the spectral selection drops the drawing and keeps the time selection (what Remove Frequencies and Heal do once done)");

        view.setBrushRadius (500.0f);
        const auto big = view.getBrushRadius();
        view.setBrushRadius (0.0f);
        view.setSpectralTool (WaveformView::SpectralTool::box);
        drag (view, { at (0.2f, 0.5f), at (0.4f, 0.5f) });
        check (juce::exactlyEqual (big, WaveformView::maxBrushRadius) && juce::exactlyEqual (view.getBrushRadius(), WaveformView::minBrushRadius)
                   && view.getSpectralShape().isEmpty() && view.getSelection().getLength() > 0 && view.getSpectralBand().isEmpty(),
               "spectral tools: the brush is 3 to 60 pixels; with the box tool a plain drag in a spectrogram selects time, as before");
    }

    /** How the lasso looks: while it is drawn, only its line so far (in white dashes) and no wash
        inside; once the mouse goes up, the closed shape washed white with a dashed edge; and the
        lasso tool's own pointer over a spectrogram, the crosshair still for the brush. */
    void testLassoLook()
    {
        AudioEngine engine;
        juce::AudioBuffer<float> silence (2, (int) (2 * rate));
        silence.clear();
        engine.document.reset (AudioEdits::fromBuffer (std::move (silence)), rate, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 600);
        view.setDisplay (WaveformView::Display::spectrogram);
        view.zoomToFit();
        view.setSpectralTool (WaveformView::SpectralTool::lasso);
        const auto area = view.getWaveArea().toFloat();
        const auto top = area.withHeight (area.getHeight() / 2.0f);
        const auto at = [&top] (float fx, float fy) { return juce::Point<float> (top.getX() + top.getWidth() * fx, top.getY() + top.getHeight() * fy); };
        const auto brightness = [] (const juce::Image& image, juce::Point<float> p) { return image.getPixelAt ((int) p.x, (int) p.y).getBrightness(); };

        // The brightest and darkest pixel along a line, sampled every pixel.
        const auto along = [&brightness] (const juce::Image& image, juce::Point<float> a, juce::Point<float> b)
        {
            float brightest = 0.0f, darkest = 1.0f;
            const auto n = (int) a.getDistanceFrom (b);

            for (int i = 0; i <= n; ++i)
            {
                const auto v = brightness (image, a + (b - a) * ((float) i / (float) juce::jmax (1, n)));
                brightest = juce::jmax (brightest, v);
                darkest = juce::jmin (darkest, v);
            }

            return std::pair (brightest, darkest);
        };

        // Three corners drawn, the fourth not yet. The lasso's time span becomes the selection as
        // it is drawn, so what is inside the lasso is compared with a point under it, in the
        // selection but outside the shape; the closing side slants so it is clear of the
        // selection's edge.
        const auto a = at (0.2f, 0.3f), b = at (0.6f, 0.3f), c = at (0.6f, 0.7f), d = at (0.25f, 0.7f);
        const auto centre = at (0.45f, 0.4f), under = at (0.45f, 0.9f);   // the centre clear of the closing edge, which runs corner to corner
        view.mouseDown (mouse (view, a, a));
        view.mouseDrag (mouse (view, b, a));
        view.mouseDrag (mouse (view, c, a));
        const auto drawing = view.createComponentSnapshot (view.getLocalBounds());
        const auto background = brightness (drawing, under);
        const auto [lineBright, lineDark] = along (drawing, a + juce::Point<float> (6.0f, 0.0f), b - juce::Point<float> (6.0f, 0.0f));
        const auto [closingBright, closingDark] = along (drawing, c + (a - c) * 0.1f, c + (a - c) * 0.9f);
        check (juce::exactlyEqual (brightness (drawing, centre), background)
                   && lineBright > 0.5f && lineDark < 0.2f && closingBright > 0.3f && closingBright < lineBright,
               "lasso look: while it is drawn, no wash inside (centre " + juce::String (brightness (drawing, centre), 2) + " = under it "
                   + juce::String (background, 2) + "), its line so far in dashes (brightest " + juce::String (lineBright, 2) + ", darkest "
                   + juce::String (lineDark, 2) + ") and the edge that will close it dotted faintly (" + juce::String (closingBright, 2) + ")");

        view.mouseDrag (mouse (view, d, a));
        view.mouseUp (mouse (view, d, a));
        const auto closed = view.createComponentSnapshot (view.getLocalBounds());
        const auto [edgeBright, edgeDark] = along (closed, d + (a - d) * 0.1f, d + (a - d) * 0.9f);
        check (brightness (closed, centre) > brightness (closed, under) + 0.1f && edgeBright > 0.5f && edgeDark < edgeBright - 0.3f,
               "lasso look: released, the shape is washed inside (" + juce::String (brightness (closed, centre), 2) + " over " + juce::String (brightness (closed, under), 2)
                   + ") with a dashed edge along the closing side (brightest " + juce::String (edgeBright, 2) + ", darkest " + juce::String (edgeDark, 2) + ")");

        view.mouseMove (mouse (view, centre, centre));
        const auto lassoPointer = view.getMouseCursor();
        view.setSpectralTool (WaveformView::SpectralTool::brush);
        view.mouseMove (mouse (view, centre, centre));
        check (lassoPointer != juce::MouseCursor (juce::MouseCursor::CrosshairCursor) && lassoPointer != juce::MouseCursor (juce::MouseCursor::NormalCursor)
                   && view.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::CrosshairCursor),
               "lasso look: the lasso has a pointer of its own over a spectrogram, and the brush keeps the crosshair");
    }

    void testStrokeUndo()
    {
        AudioEngine engine;
        juce::AudioBuffer<float> silence (2, (int) (2 * rate));
        silence.clear();
        engine.document.reset (AudioEdits::fromBuffer (std::move (silence)), rate, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 600);
        view.setDisplay (WaveformView::Display::spectrogram);
        view.zoomToFit();
        const auto top = view.getWaveArea().toFloat().withHeight (view.getWaveArea().toFloat().getHeight() / 2.0f);
        const auto at = [&top] (float fx, float fy) { return juce::Point<float> (top.getX() + top.getWidth() * fx, top.getY() + top.getHeight() * fy); };

        const auto nothing = ! view.canUndoStroke() && ! view.undoStroke();
        view.setSpectralTool (WaveformView::SpectralTool::lasso);
        drag (view, { at (0.2f, 0.3f), at (0.5f, 0.3f), at (0.5f, 0.7f), at (0.2f, 0.7f) });
        const auto first = view.getSelection();
        drag (view, { at (0.7f, 0.2f), at (0.9f, 0.2f), at (0.8f, 0.4f) }, juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
        const auto second = view.getSelection();
        drag (view, { at (0.3f, 0.4f), at (0.45f, 0.4f), at (0.45f, 0.6f), at (0.3f, 0.6f) }, juce::ModifierKeys (juce::ModifierKeys::altModifier));
        const auto three = view.getSpectralShape().strokes.size() == 3 && view.canUndoStroke();

        const auto undoTake = view.undoStroke() && view.getSpectralShape().strokes.size() == 2 && ! view.getSpectralShape().strokes.back().subtract
                              && view.getSelection() == second;
        const auto undoSecond = view.undoStroke() && view.getSpectralShape().strokes.size() == 1 && view.getSelection() == first;
        const auto undoFirst = view.undoStroke() && view.getSpectralShape().isEmpty() && view.getSelection().isEmpty() && view.getCursor() == first.getStart();
        check (nothing && three && undoTake && undoSecond && undoFirst && ! view.canUndoStroke() && ! view.undoStroke(),
               "stroke undo: each call takes back the last stroke (the take, then the added lasso, then the first), the time selection "
               "following the strokes left and the cursor landing at the drawing's start; nothing to take back before or after");

        drag (view, { at (0.2f, 0.3f), at (0.5f, 0.3f), at (0.5f, 0.7f), at (0.2f, 0.7f) });
        const auto drawn = view.canUndoStroke();
        juce::AudioBuffer<float> other (2, (int) (2 * rate));
        other.clear();
        engine.document.apply (AudioEdits::fromBuffer (std::move (other)), "Nothing");
        check (drawn && ! view.canUndoStroke() && ! view.undoStroke() && view.getSpectralShape().strokes.size() == 1,
               "stroke undo: once the file has changed since the stroke, Undo is the file's again and the drawing is left as it is");
    }

    void testSnap()
    {
        // A 100-frame square wave crosses zero at every 50th frame: with the view asked to snap,
        // a selection dragged in the waveform lands both ends on crossings near where the mouse was.
        AudioEngine engine;
        juce::AudioBuffer<float> square (2, (int) (2 * rate));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < square.getNumSamples(); ++i)
                square.setSample (ch, i, (i / 50) % 2 == 0 ? 0.5f : -0.5f);

        engine.document.reset (AudioEdits::fromBuffer (std::move (square)), rate, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 400);
        view.setDisplay (WaveformView::Display::waveform);
        view.zoomToFit();
        const auto area = view.getWaveArea().toFloat();
        const auto y = area.getCentreY();
        const auto frameAt = [&view, &area] (float x) { return view.getVisibleFrames().getStart() + (double) (x - area.getX()) / area.getWidth() * view.getVisibleFrames().getLength(); };

        drag (view, { { 101.0f, y }, { 503.0f, y } });
        const auto plain = view.getSelection();
        view.snapSelectionFrame = [&engine] (juce::int64 frame) { return AudioEdits::zeroCrossingNear (engine.document.getSnapshot(), frame, 60); };
        drag (view, { { 101.0f, y }, { 503.0f, y } });
        const auto snapped = view.getSelection();
        view.snapSelectionFrame = nullptr;

        check (! plain.isEmpty() && snapped.getStart() % 50 == 0 && snapped.getEnd() % 50 == 0
                   && std::abs ((double) snapped.getStart() - frameAt (101.0f)) < 200.0 && std::abs ((double) snapped.getEnd() - frameAt (503.0f)) < 200.0
                   && std::abs (snapped.getStart() - plain.getStart()) <= 60 && std::abs (snapped.getEnd() - plain.getEnd()) <= 60,
               "snapping: a selection dragged with the view asked to snap lands both ends on zero crossings (" + juce::String (snapped.getStart()) + " and "
                   + juce::String (snapped.getEnd()) + ", from " + juce::String (plain.getStart()) + " and " + juce::String (plain.getEnd()) + " without)");
    }
}

void runSpectralShapeTests()
{
    testGeometry();
    testCut();
    testHeal();
    testHealThroughTone();
    testTools();
    testLassoLook();
    testStrokeUndo();
    testSnap();
}
