#include <JuceHeader.h>
#include "../Source/DynamicEq.h"
#include "../Source/BuiltInEffects.h"
#include "TestUtils.h"
#include "DynamicEqTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** Sines at these frequencies and amplitudes added, on two channels, for a second. */
    juce::AudioBuffer<float> sines (std::initializer_list<std::pair<double, double>> parts, int frames = 48000)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int i = 0; i < frames; ++i)
        {
            double v = 0.0;

            for (auto [hz, amp] : parts)
                v += amp * std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);

            b.setSample (0, i, (float) v);
            b.setSample (1, i, (float) v);
        }

        return b;
    }

    /** A frequency's amplitude in channel 0 over [from, from + length): a single-bin DFT. */
    double amplitude (const juce::AudioBuffer<float>& b, double hz, int from, int length)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + length; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * hz * i / rate;
            re += b.getSample (0, i) * std::cos (phase);
            im += b.getSample (0, i) * std::sin (phase);
        }

        return 2.0 * std::sqrt (re * re + im * im) / length;
    }

    double db (double ratio)    { return 20.0 * std::log10 (ratio); }

    /** One band on, the rest off. */
    DynamicEq::Settings only (const DynamicEq::Band& band)
    {
        auto s = DynamicEq::defaultSettings();

        for (auto& b : s.bands)
            b.on = false;

        s.bands[2] = band;
        s.bands[2].on = true;
        return s;
    }

    juce::AudioBuffer<float> applied (const juce::AudioBuffer<float>& in, const DynamicEq::Settings& s)
    {
        return readAll (*DynamicEq::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, in.getNumSamples(), rate, s));
    }

    juce::MouseEvent eventAt (juce::Component& c, juce::Point<float> at, juce::Point<float> down, bool dragged = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                 juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, dragged);
    }
}

void runDynamicEqTests()
{
    std::cout << "Dynamic EQ:" << std::endl;

    // Nothing changes until a band is given a gain, or while the part stays under the threshold.
    {
        const auto in = sines ({ { 100.0, 0.3 }, { 1000.0, 0.3 }, { 8000.0, 0.3 } });
        auto s = DynamicEq::defaultSettings();
        const auto flat = applied (in, s);

        DynamicEq::Band quiet;   // a bell cutting 1 kHz, but only over -3 dB, which 0.3 (-10.5 dB) never reaches
        quiet.gainDb = -12.0;
        quiet.thresholdDb = -3.0;
        const auto under = applied (in, only (quiet));
        auto same = true;

        for (int ch = 0; ch < 2 && same; ++ch)
            for (int i = 0; i < in.getNumSamples() && same; ++i)
                same = juce::exactlyEqual (flat.getSample (ch, i), in.getSample (ch, i)) && juce::exactlyEqual (under.getSample (ch, i), in.getSample (ch, i));

        check (same, "dynamic eq: every band at 0 dB passes the sound bit for bit, and so does a band whose part stays under its threshold");
    }

    // A bell cutting 1 kHz over -20 dB: a loud 1 kHz is taken down toward the cut, the 100 Hz and
    // 8 kHz beside it kept, and once it falls quiet the band lets go.
    {
        juce::AudioBuffer<float> in (2, 96000);
        const auto loud = sines ({ { 100.0, 0.2 }, { 1000.0, 0.5 }, { 8000.0, 0.2 } });
        const auto soft = sines ({ { 100.0, 0.2 }, { 1000.0, 0.02 }, { 8000.0, 0.2 } });

        for (int ch = 0; ch < 2; ++ch)
        {
            in.copyFrom (ch, 0, loud, ch, 0, 48000);
            in.copyFrom (ch, 48000, soft, ch, 0, 48000);
        }

        DynamicEq::Band band;
        band.gainDb = -9.0;
        band.thresholdDb = -20.0;
        band.ratio = 20.0;
        band.attackMs = 5.0;
        band.releaseMs = 50.0;
        const auto out = applied (in, only (band));
        const auto cut = db (amplitude (out, 1000.0, 24000, 24000) / amplitude (in, 1000.0, 24000, 24000));
        const auto lowKept = db (amplitude (out, 100.0, 24000, 24000) / amplitude (in, 100.0, 24000, 24000));
        const auto highKept = db (amplitude (out, 8000.0, 24000, 24000) / amplitude (in, 8000.0, 24000, 24000));
        const auto letGo = db (amplitude (out, 1000.0, 72000, 24000) / amplitude (in, 1000.0, 72000, 24000));
        std::cout << "      1 kHz over the threshold " << cut << " dB (100 Hz " << lowKept << ", 8 kHz " << highKept << "), after it falls quiet " << letGo << " dB" << std::endl;
        check (cut < -8.5 && cut > -9.5 && std::abs (lowKept) < 0.3 && std::abs (highKept) < 0.3 && std::abs (letGo) < 0.1,
               "dynamic eq: a bell cutting up to 9 dB at 1 kHz over -20 dB takes a loud 1 kHz (-6 dB) down by its full 9 dB at a ratio of 20, "
               "keeps 100 Hz and 8 kHz within 0.3 dB, and lets go within 0.1 dB once the 1 kHz falls to -34 dB");
    }

    // Under mode: a shelf lifting the highs only while they are quiet. The ratio sets how far: at 2:1,
    // 10 dB under the threshold moves 5 dB.
    {
        // 10 kHz at -40 dB. The shelf's part is a first-order high-pass, so it hears a low tone faintly
        // (6 dB an octave under the corner): 60 Hz at -20 dB reaches it at -57 dB, too little to count.
        const auto in = sines ({ { 60.0, 0.1 }, { 10000.0, 0.01 } });
        DynamicEq::Band band;
        band.shape = DynamicEq::Shape::highShelf;
        band.frequency = 4000.0;
        band.mode = DynamicEq::Mode::under;
        band.gainDb = 12.0;
        band.thresholdDb = -30.0;
        band.ratio = 2.0;
        const auto out = applied (in, only (band));
        const auto lifted = db (amplitude (out, 10000.0, 24000, 24000) / amplitude (in, 10000.0, 24000, 24000));
        const auto lowKept = db (amplitude (out, 60.0, 24000, 24000) / amplitude (in, 60.0, 24000, 24000));
        const auto staticDb = DynamicEq::staticGainDb (band, -40.0);
        std::cout << "      10 kHz under the threshold lifted " << lifted << " dB (static " << staticDb << "), 60 Hz " << lowKept << " dB" << std::endl;
        check (std::abs (staticDb - 5.0) < 1.0e-9 && lifted > 4.3 && lifted < 5.3 && std::abs (lowKept) < 0.2,
               "dynamic eq: a high shelf lifting up to 12 dB under -30 dB at 2:1 lifts a 10 kHz at -40 dB by 5 dB (the static gain exactly, "
               "the sound within 0.7 dB of it: the shelf at 4 kHz is not yet at its full lift at 10 kHz), the 60 Hz under it within 0.2 dB");
    }

    // The response: a bell at its full gain is the gain at its centre, half at its edges (Q), nothing far
    // off; shelves 6 dB an octave; the drawing's curve from them.
    {
        DynamicEq::Band bell;
        bell.frequency = 1000.0;
        bell.q = 2.0;
        bell.gainDb = -12.0;
        const auto centre = DynamicEq::responseDb (bell, rate, 1000.0, -12.0);
        const auto far = DynamicEq::responseDb (bell, rate, 100.0, -12.0);
        const auto none = DynamicEq::responseDb (bell, rate, 1000.0, 0.0);
        DynamicEq::Band shelf;
        shelf.shape = DynamicEq::Shape::lowShelf;
        shelf.frequency = 200.0;
        const auto low = DynamicEq::responseDb (shelf, rate, 20.0, 6.0);
        const auto high = DynamicEq::responseDb (shelf, rate, 5000.0, 6.0);
        std::cout << "      bell at its centre " << centre << " dB, a decade off " << far << "; low shelf +6 dB: at 20 Hz " << low << ", at 5 kHz " << high << std::endl;
        check (std::abs (centre + 12.0) < 1.0e-6 && std::abs (far) < 0.3 && juce::exactlyEqual (none, 0.0) && std::abs (low - 6.0) < 0.1 && std::abs (high) < 0.1,
               "dynamic eq: a bell's response at its centre is its gain exactly and a decade off within 0.3 dB, a band moving 0 dB is flat, "
               "a low shelf's +6 dB reaches 20 Hz within 0.1 dB and 5 kHz is left within 0.1 dB");
    }

    // Refused settings, and an empty range.
    {
        const auto snap = AudioEdits::fromBuffer (sines ({ { 1000.0, 0.3 } }, 4800));
        auto high = DynamicEq::defaultSettings();
        high.bands[0].frequency = 22000.0;
        auto q = DynamicEq::defaultSettings();
        q.bands[1].q = 30.0;
        auto ratio = DynamicEq::defaultSettings();
        ratio.bands[2].ratio = 0.5;
        auto out = DynamicEq::defaultSettings();
        out.outputDb = 30.0;
        auto atRate = DynamicEq::defaultSettings();
        atRate.bands[4].frequency = 20000.0;
        check (DynamicEq::apply (*snap, 0, 4800, rate, high) == nullptr && DynamicEq::apply (*snap, 0, 4800, rate, q) == nullptr
                   && DynamicEq::apply (*snap, 0, 4800, rate, ratio) == nullptr && DynamicEq::apply (*snap, 0, 4800, rate, out) == nullptr
                   && DynamicEq::apply (*snap, 0, 4800, 22050.0, atRate) == nullptr && DynamicEq::apply (*snap, 0, 4800, rate, atRate) != nullptr
                   && DynamicEq::apply (*snap, 100, 100, rate, DynamicEq::defaultSettings()) == nullptr
                   && juce::exactlyEqual (DynamicEq::withinRate (atRate, 22050.0).bands[4].frequency, 0.45 * 22050.0),
               "dynamic eq: a frequency past 20 kHz or 0.45 of the rate, a Q past 20, a ratio under 1, an output past 24 dB and an empty range "
               "are refused; withinRate brings a frequency under the rate");
    }

    // Live, bit for bit with offline in uneven blocks; state, presets, readings and the window.
    {
        juce::Random r (71);
        juce::AudioBuffer<float> in (2, 96000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < in.getNumSamples(); ++i)
                in.setSample (ch, i, (r.nextFloat() - 0.5f) * (i < 48000 ? 0.8f : 0.05f));

        auto s = DynamicEq::defaultSettings();
        s.bands[0].gainDb = -4.0;
        s.bands[2].gainDb = -6.0;
        s.bands[2].q = 1.5;
        s.bands[2].thresholdDb = -24.0;
        s.bands[3].on = false;
        s.bands[4].gainDb = 3.0;
        s.bands[4].mode = DynamicEq::Mode::under;
        s.bands[4].thresholdDb = -30.0;
        s.outputDb = -1.5;

        BuiltInEffects::DynamicEqProcessor live;
        live.setSettings (s);
        live.setPlayConfigDetails (2, 2, rate, 512);
        live.prepareToPlay (rate, 3000);
        juce::AudioBuffer<float> played (in);
        juce::MidiBuffer midi;
        const int sizes[] = { 1, 7, 512, 3000 };

        for (int pos = 0, k = 0; pos < played.getNumSamples(); ++k)
        {
            const auto n = juce::jmin (sizes[k % 4], played.getNumSamples() - pos);
            juce::AudioBuffer<float> block (played.getArrayOfWritePointers(), 2, pos, n);
            live.processBlock (block, midi);
            pos += n;
        }

        const auto offline = applied (in, live.getSettings());
        auto same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 96000; ++i)
                same = same && juce::exactlyEqual (played.getSample (ch, i), offline.getSample (ch, i));

        // The readings are what a core ended the same audio at.
        DynamicEq::Core core;
        core.prepare (2, rate);
        core.set (live.getSettings());
        juce::AudioBuffer<float> again (in);
        core.process (again.getArrayOfWritePointers(), 2, 96000);
        const auto readings = live.getReadings();
        const auto readingsRight = readings.gainDb == core.getGainDb() && readings.levelDb == core.getLevelDb()
                                   && readings.gainDb[0] < 0.0 && juce::exactlyEqual (readings.gainDb[3], 0.0) && readings.gainDb[4] > 0.0;

        juce::MemoryBlock state;
        live.getStateInformation (state);
        BuiltInEffects::DynamicEqProcessor copy, fromPreset;
        copy.setStateInformation (state.getData(), (int) state.getSize());
        fromPreset.setPresetValues (live.getPresetValues());
        const auto values = [] (const DynamicEq::Settings& x)
        {
            std::vector<double> v { x.outputDb };

            for (auto& b : x.bands)
                v.insert (v.end(), { b.on ? 1.0 : 0.0, (double) b.shape, (double) b.mode, b.frequency, b.q, b.gainDb, b.thresholdDb, b.ratio, b.attackMs, b.releaseMs });

            return v;
        };
        // A parameter holds a setting as near as its float range can (100 Hz on a skewed range lands
        // at 99.99999), but what it holds, its state and its presets restore exactly.
        const auto near = [&values] (const DynamicEq::Settings& a, const DynamicEq::Settings& b)
        {
            const auto x = values (a), y = values (b);
            auto close = x.size() == y.size();

            for (size_t i = 0; i < x.size() && close; ++i)
                close = std::abs (x[i] - y[i]) < 1.0e-3 * juce::jmax (1.0, std::abs (y[i]));

            return close;
        };
        const auto kept = values (copy.getSettings()) == values (live.getSettings()) && values (fromPreset.getSettings()) == values (live.getSettings())
                          && near (live.getParameterSettings(), s);

        auto windowWorks = false;

        {
            std::unique_ptr<juce::AudioProcessorEditor> made (live.createEditor());

            if (auto* editor = dynamic_cast<BuiltInEffects::DynamicEqEditor*> (made.get()))
            {
                auto& panel = editor->getPanel();
                const auto showsEffect = values (panel.getSettings()) == values (live.getParameterSettings());
                panel.selectBand (2);
                panel.getControls().gain.setValue (-10.0, juce::sendNotificationSync);
                const auto reachedEffect = std::abs (live.getParameterSettings().bands[2].gainDb + 10.0) < 0.01;
                auto moved = s;
                moved.bands[1].frequency = 640.0;
                live.setSettings (moved);
                editor->refresh();
                const auto followsEffect = std::abs (panel.getSettings().bands[1].frequency - 640.0) < 0.01;
                windowWorks = showsEffect && reachedEffect && followsEffect;
            }
        }

        // Closing the window leaves the effect as it was.
        const auto leftAlone = std::abs (live.getParameterSettings().bands[1].frequency - 640.0) < 0.01 && std::abs (live.getParameterSettings().bands[2].gainDb + 6.0) < 0.01;

        check (same && readingsRight && kept && windowWorks && leftAlone && live.getLatencySamples() == 0,
               "built-in dynamic eq: played in blocks of 1, 7, 512 and 3,000 frames it gives the offline samples bit for bit; the readings "
               "are the core's gains and levels (a cut, 0 for a band off, a lift under); its state and preset values restore every setting; "
               "its window shows the effect's settings, a change on it reaches the effect, the effect's changes reach it, and closing it leaves the effect as it is; no latency");
    }

    // The panel's mouse: dragging a handle sets frequency and gain, double-click off and on, the wheel a
    // bell's Q, the meter's line the picked band's threshold.
    {
        DynamicEq::Panel panel (DynamicEq::defaultSettings(), rate);
        panel.setSize (760, 440);
        auto changes = 0;
        panel.onChange = [&changes] { ++changes; };
        const auto& s = panel.getSettings();

        const auto from = panel.pointFor (2);
        panel.mouseDown (eventAt (panel, from, from));
        const juce::Point<float> to (panel.xForFrequency (2000.0), panel.yForDb (-8.0));
        panel.mouseDrag (eventAt (panel, to, from, true));
        panel.mouseUp (eventAt (panel, to, from));
        const auto dragged = panel.getSelectedBand() == 2 && std::abs (s.bands[2].frequency - 2000.0) < 1.0 && std::abs (s.bands[2].gainDb + 8.0) < 0.11
                             && panel.getControls().gain.getValue() < -7.8;

        juce::MouseWheelDetails up {};
        up.deltaY = 0.3f;
        panel.mouseWheelMove (eventAt (panel, panel.pointFor (2), panel.pointFor (2)), up);
        const auto narrowed = std::abs (s.bands[2].q - 1.1) < 1.0e-9;

        panel.mouseDoubleClick (eventAt (panel, panel.pointFor (1), panel.pointFor (1)));
        const auto ring = juce::Point<float> (panel.xForFrequency (s.bands[1].frequency), panel.yForDb (0.0));
        const auto off = ! s.bands[1].on && panel.offBandAt (ring) == 1 && panel.bandAt (ring) == -1;
        panel.mouseDoubleClick (eventAt (panel, ring, ring));
        const auto on = s.bands[1].on && panel.getSelectedBand() == 1;

        panel.selectBand (4);
        const auto meter = panel.getMeterArea();
        const juce::Point<float> line (meter.getCentreX(), panel.yForLevel (-42.0));
        panel.mouseDown (eventAt (panel, line, line));
        panel.mouseUp (eventAt (panel, line, line));
        const auto threshold = std::abs (s.bands[4].thresholdDb + 42.0) < 0.11 && std::abs (panel.getControls().threshold.getValue() + 42.0) < 0.11;

        const auto before = changes;
        panel.setSettings (DynamicEq::defaultSettings());
        const auto quiet = changes == before && juce::exactlyEqual (s.bands[2].gainDb, 0.0);

        check (dragged && narrowed && off && on && threshold && quiet,
               "dynamic eq panel: dragging a handle sets its band's frequency (2 kHz) and gain (-8 dB) and shows them in the controls; the wheel narrows "
               "a bell's Q by a tenth; double-click switches a band off (a ring on the 0 dB line) and on again; dragging the meter's line sets the "
               "picked band's threshold (-42 dB); setSettings says nothing");
    }
}
