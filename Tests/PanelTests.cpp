#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/OpenFiles.h"
#include "../Source/UndoHistory.h"
#include "../Source/LevelsPanel.h"
#include "../Source/Analysis.h"
#include "TestUtils.h"
#include "PanelTests.h"

namespace
{
    using TestUtils::check;

    void settle()   { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); }

    void loadTone (AudioEngine& engine, const juce::String& name, int frames)
    {
        juce::AudioBuffer<float> b (1, frames);
        b.clear();
        b.setSample (0, 0, 0.5f);
        AudioFileIO::Loaded loaded;
        loaded.snapshot = AudioEdits::fromBuffer (std::move (b));
        loaded.sampleRate = 48000.0;
        loaded.bitDepth = 24;
        engine.adoptFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name), std::move (loaded));
    }

    void testFileHistory()
    {
        AudioEngine engine;
        OpenFiles files (engine);
        auto& doc = engine.document;
        loadTone (engine, "steps.wav", 1000);
        doc.apply (AudioEdits::gain (*doc.getSnapshot(), 0, 1000, 3.0f), "Gain");
        doc.apply (AudioEdits::remove (*doc.getSnapshot(), 0, 100), "Delete");

        bool sessionShown = false;
        UndoHistory history (engine.session, doc, [&] { return sessionShown; });
        history.setSize (280, 300);
        settle();

        const auto listed = history.getNumRows() == 3 && history.getText (0) == "Start" && history.getText (1) == "Gain"
                            && history.getText (2) == "Delete" && history.getHighlightedRow() == 2;
        history.chooseRow (0);
        settle();
        const auto back = doc.getPosition() == 0 && ! doc.isDirty() && doc.getNumFrames() == 1000 && history.getHighlightedRow() == 0;
        history.chooseRow (2);
        settle();
        const auto forward = doc.getPosition() == 2 && doc.getNumFrames() == 900;
        check (listed && back && forward,
               "undo history: in the editor it lists the file's steps from Start, and clicking one goes back or forward to it");

        sessionShown = true;
        history.refresh();
        const auto sessionSteps = engine.session.getHistory().size();
        check (history.getNumRows() == sessionSteps && history.getText (0) == "Start",
               "undo history: with the session showing it lists the session's steps instead");

        sessionShown = false;
        history.refresh();
        files.prepareForAnother ({});
        loadTone (engine, "other.wav", 500);
        settle();
        const auto otherShown = history.getNumRows() == 1 && history.getHighlightedRow() == 0;
        files.activate (0, {});
        settle();
        check (otherShown && history.getNumRows() == 3 && history.getHighlightedRow() == 2,
               "undo history: switching files shows the steps of the file switched to, each kept with its file");
    }

    void testLevels()
    {
        LevelsPanel levels;
        levels.setSize (800, 70);
        check (juce::exactlyEqual (LevelsPanel::xFor (-60.0f, 600.0f), 0.0f) && juce::exactlyEqual (LevelsPanel::xFor (-30.0f, 600.0f), 300.0f)
                   && juce::exactlyEqual (LevelsPanel::xFor (0.0f, 600.0f), 600.0f) && juce::exactlyEqual (LevelsPanel::xFor (-90.0f, 600.0f), 0.0f)
                   && juce::exactlyEqual (LevelsPanel::xFor (3.0f, 600.0f), 600.0f),
               "levels: the scale runs evenly in dB from -60 at the left to 0 at the right, kept inside the bar");

        const auto silent = levels.readout (0) == "-inf" && levels.readout (1) == "-inf";
        levels.take (0.5f, 0.25f);
        levels.take (0.1f, 0.1f);
        const auto peaks = levels.readout (0) == "-6.0" && levels.readout (1) == "-12.0" && ! levels.isClipped (0);
        levels.take (1.122f, 0.0f);
        const auto over = levels.readout (0) == "+1.0" && levels.isClipped (0) && ! levels.isClipped (1);
        levels.createComponentSnapshot (levels.getLocalBounds());   // paints
        levels.reset();
        const auto cleared = levels.readout (0) == "-inf" && ! levels.isClipped (0);
        check (silent && peaks && over && cleared,
               "levels: the highest peak of each channel since the reset, in dB, a light for a clipped one, and the reset clearing both");
    }

    std::vector<float> sine (double frequency, float amplitude, int n, double rate = 48000.0)
    {
        std::vector<float> out ((size_t) n);

        for (int i = 0; i < n; ++i)
            out[(size_t) i] = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate);

        return out;
    }

    void testSpectrum()
    {
        using namespace Analysis;
        constexpr auto rate = 48000.0;
        const auto onBin = 128.0 * rate / fftSize;   // 750 Hz, exactly on bin 128
        const auto full = spectrumDb (sine (onBin, 1.0f, fftSize).data(), fftSize);
        const auto half = spectrumDb (sine (onBin, 0.5f, fftSize).data(), fftSize);
        check (full.size() == (size_t) fftSize / 2 + 1 && std::abs (full[128]) < 0.01f && std::abs (half[128] + 6.02f) < 0.01f,
               "spectrum: a full-scale sine on a bin reads 0 dB there, and one at half the level -6.0 dB");

        const auto peak = (size_t) std::distance (full.begin(), std::max_element (full.begin(), full.end()));
        check (peak == 128 && full[100] < -100.0f && full[1000] < -100.0f,
               "spectrum: the sine's energy stays at its own frequency (-100 dB or lower 28 bins away)");

        const auto between = spectrumDb (sine (1000.0, 1.0f, fftSize).data(), fftSize);
        const auto read = levelAt (between, 1000.0, rate);
        check (read <= 0.0f && read > -1.5f && levelAt (between, 4000.0, rate) < -100.0f,
               "spectrum: a sine between bins reads within 1.5 dB of its level at its frequency, and not two octaves up");

        std::vector<float> silence ((size_t) fftSize, 0.0f);
        const auto quiet = spectrumDb (silence.data(), fftSize);
        check (std::all_of (quiet.begin(), quiet.end(), [] (float v) { return juce::exactlyEqual (v, -150.0f); }),
               "spectrum: silence reads -150 dB in every bin");

        check (std::abs (SpectrumPanel::xFor (20.0, 300.0f)) < 1.0e-4f && std::abs (SpectrumPanel::xFor (20000.0, 300.0f) - 300.0f) < 1.0e-3f
                   && std::abs (SpectrumPanel::xFor (632.46, 300.0f) - 150.0f) < 0.01f && std::abs (SpectrumPanel::frequencyAt (150.0f, 300.0f) - 632.46) < 0.01,
               "spectrum: the frequency scale is logarithmic, 20 Hz to 20 kHz, the middle at 632 Hz, and reads back");

        SpectrumPanel panel;
        panel.setSize (600, 240);
        panel.take (full, rate);
        panel.take (quiet, rate);
        const auto falling = std::abs (panel.getShown()[128] - (full[128] - 0.6f)) < 1.0e-5f;
        panel.take (full, rate);
        check (falling && std::abs (panel.getShown()[128]) < 0.01f,
               "spectrum: the panel shows a rise at once and lets a fall back 0.6 dB a tick");
        panel.createComponentSnapshot (panel.getLocalBounds());   // paints
    }

    void testPhase()
    {
        using namespace Analysis;
        const auto a = sine (440.0, 0.8f, 4800), b = sine (1234.0, 0.8f, 4800);
        std::vector<float> inverted (a.size()), silent (a.size(), 0.0f);
        std::transform (a.begin(), a.end(), inverted.begin(), [] (float v) { return -v; });
        const auto n = (int) a.size();
        check (std::abs (correlation (a.data(), a.data(), n) - 1.0f) < 1.0e-5f && std::abs (correlation (a.data(), inverted.data(), n) + 1.0f) < 1.0e-5f
                   && std::abs (correlation (a.data(), b.data(), n)) < 0.02f && juce::exactlyEqual (correlation (a.data(), silent.data(), n), 0.0f),
               "phase: correlation +1 for the same signal, -1 for one inverted, about 0 for unrelated ones, 0 with a channel silent");

        check (goniometerPoint (1.0f, 1.0f) == juce::Point<float> (0.0f, 1.0f) && goniometerPoint (1.0f, -1.0f) == juce::Point<float> (-1.0f, 0.0f)
                   && goniometerPoint (1.0f, 0.0f) == juce::Point<float> (-0.5f, 0.5f) && goniometerPoint (0.0f, 1.0f) == juce::Point<float> (0.5f, 0.5f),
               "phase: the goniometer draws mono straight up, out of phase across, and each channel alone on its own diagonal");

        PhasePanel panel;
        panel.setSize (240, 280);
        juce::AudioBuffer<float> same (2, n), opposite (2, n);
        same.copyFrom (0, 0, a.data(), n);
        same.copyFrom (1, 0, a.data(), n);
        opposite.copyFrom (0, 0, a.data(), n);
        opposite.copyFrom (1, 0, inverted.data(), n);
        panel.take (same);
        const auto first = std::abs (panel.getCorrelation() - 1.0f) < 1.0e-5f;
        panel.take (opposite);
        check (first && std::abs (panel.getCorrelation() - 0.5f) < 1.0e-5f,
               "phase: the panel shows the first block's correlation, then moves a quarter of the way to each next one");
        panel.createComponentSnapshot (panel.getLocalBounds());
    }

    void testOutputTap()
    {
        Analysis::OutputTap tap;
        juce::AudioBuffer<float> recent (2, 8);
        tap.copyRecent (recent);
        const auto emptyIsSilent = recent.getMagnitude (0, 8) == 0.0f;

        juce::AudioBuffer<float> block (2, 5);

        for (int i = 0; i < 5; ++i)
        {
            block.setSample (0, i, (float) (i + 1));
            block.setSample (1, i, (float) -(i + 1));
        }

        tap.write (block);
        tap.copyRecent (recent);
        const auto padded = juce::exactlyEqual (recent.getSample (0, 2), 0.0f) && juce::exactlyEqual (recent.getSample (0, 3), 1.0f)
                            && juce::exactlyEqual (recent.getSample (0, 7), 5.0f) && juce::exactlyEqual (recent.getSample (1, 7), -5.0f);

        juce::AudioBuffer<float> mono (1, 4);

        for (int i = 0; i < 4; ++i)
            mono.setSample (0, i, 10.0f + (float) i);

        tap.write (mono);
        tap.copyRecent (recent);
        const auto newest = juce::exactlyEqual (recent.getSample (0, 3), 5.0f) && juce::exactlyEqual (recent.getSample (0, 7), 13.0f)
                            && juce::exactlyEqual (recent.getSample (1, 7), 13.0f) && juce::exactlyEqual (recent.getSample (1, 3), -5.0f);

        // Past the ring's end: the newest frames still come back in order.
        juce::AudioBuffer<float> big (2, Analysis::OutputTap::size - 3);

        for (int i = 0; i < big.getNumSamples(); ++i)
            big.setSample (0, i, (float) i), big.setSample (1, i, 0.0f);

        tap.write (big);
        tap.copyRecent (recent);
        const auto last = (float) (big.getNumSamples() - 1);
        const auto wrapped = juce::exactlyEqual (recent.getSample (0, 7), last) && juce::exactlyEqual (recent.getSample (0, 0), last - 7.0f);
        check (emptyIsSilent && padded && newest && wrapped,
               "output tap: the newest frames oldest first, silence before the first, a mono block in both channels, in order across the ring's end");
    }
}

void runPanelTests()
{
    testFileHistory();
    testLevels();
    testSpectrum();
    testPhase();
    testOutputTap();
}
