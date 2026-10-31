// Samples are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Recorder.h"
#include "TestUtils.h"
#include "RecorderTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;

    /** Device input channel c at absolute frame f carries a value unique to both. */
    float inputValue (int channel, juce::int64 frame)
    {
        return (float) channel + (float) (frame % 100000) * 1.0e-5f;
    }

    /** Stands in for the device: hands the recorder blocks of its active inputs, as CoreAudio would. */
    struct FakeDevice
    {
        FakeDevice (Recorder& r, juce::BigInteger activeInputs) : recorder (r), active (activeInputs)
        {
            for (int ch = 0; ch <= active.getHighestBit(); ++ch)
                if (active[ch])
                    absoluteChannels.push_back (ch);

            buffer.setSize ((int) absoluteChannels.size(), blockSize);
            recorder.configure (rate, active);
        }

        /** One cycle; fill(c, i) gives active input c's sample i of this block. */
        void cycle (const std::function<float (int absoluteChannel, int index)>& fill)
        {
            for (int c = 0; c < buffer.getNumChannels(); ++c)
                for (int i = 0; i < blockSize; ++i)
                    buffer.setSample (c, i, fill (absoluteChannels[(size_t) c], i));

            std::fill (out[0], out[0] + blockSize, 1.0f);   // the recorder must clear these
            std::fill (out[1], out[1] + blockSize, 1.0f);
            float* outs[2] = { out[0], out[1] };
            recorder.audioDeviceIOCallbackWithContext (buffer.getArrayOfReadPointers(), buffer.getNumChannels(),
                                                       outs, 2, blockSize, {});
        }

        Recorder& recorder;
        juce::BigInteger active;
        std::vector<int> absoluteChannels;
        juce::AudioBuffer<float> buffer;
        float out[2][blockSize] {};
    };

    juce::BigInteger bits (std::initializer_list<int> channels)
    {
        juce::BigInteger b;

        for (auto c : channels)
            b.setBit (c);

        return b;
    }

    float sampleAt (const AudioClip& clip, int channel, juce::int64 frame)
    {
        const AudioSnapshot s (clip.numChannels, clip.segments);
        juce::AudioBuffer<float> one (clip.numChannels, 1);
        s.read (one, 0, frame, 1);
        return one.getSample (channel, 0);
    }

    void testMapping()
    {
        std::atomic<juce::int64> played { 0 };
        Recorder recorder (played);

        recorder.setInput (3, 2);
        recorder.configure (rate, bits ({ 0, 3, 5 }));
        check (! recorder.isInputActive(), "recorder: a pair with an inactive channel is not armed");
        check (! recorder.start(), "recorder: refuses to start without an active input");

        recorder.setInput (5, 1);
        recorder.configure (rate, bits ({ 0, 3, 5 }));
        check (recorder.isInputActive(), "recorder: an active mono input is armed");

        // Inputs 0, 3, 5 arrive as buffers 0, 1, 2; input 5 must be read from buffer 2.
        FakeDevice device (recorder, bits ({ 0, 3, 5 }));
        recorder.start();
        device.cycle ([] (int ch, int i) { return inputValue (ch, i); });
        const auto take = recorder.stop();

        bool ok = take.clip.numChannels == 1 && take.clip.getNumFrames() == blockSize;

        for (int i = 0; ok && i < blockSize; ++i)
            ok = sampleAt (take.clip, 0, i) == inputValue (5, i);

        check (ok, "recorder: records the chosen absolute input among the active ones");
        check (juce::exactlyEqual (device.out[0][7], 0.0f), "recorder: leaves the outputs silent");
    }

    void testCapture()
    {
        std::atomic<juce::int64> played { 0 };
        Recorder recorder (played);
        recorder.setInput (2, 2);
        FakeDevice device (recorder, bits ({ 0, 1, 2, 3 }));

        // 30 s: crosses the recorder's 2^20-frame block boundary and wraps its FIFO many times.
        const int blocks = (int) (30.0 * rate) / blockSize;
        juce::int64 frame = 0;
        float maxPeak = 0.0f;

        recorder.start();

        for (int b = 0; b < blocks; ++b)
        {
            device.cycle ([&] (int ch, int i) { return inputValue (ch, frame + i); });
            frame += blockSize;
            maxPeak = juce::jmax (maxPeak, recorder.takePeak (1));

            if (b % 40 == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));   // faster than real time, slower than the writer
        }

        const auto take = recorder.stop();
        check (take.droppedFrames == 0, "recorder: no frames dropped (" + juce::String (take.droppedFrames) + ")");
        check (take.clip.getNumFrames() == frame && take.clip.numChannels == 2 && juce::exactlyEqual (take.clip.sampleRate, rate),
               "recorder: take has every frame (" + juce::String (take.clip.getNumFrames()) + " of " + juce::String (frame) + ")");
        check (take.clip.segments.size() >= 2, "recorder: long takes are split into blocks");

        const AudioSnapshot s (take.clip.numChannels, take.clip.segments);
        juce::AudioBuffer<float> all (2, (int) frame);
        s.read (all, 0, 0, (int) frame);
        juce::int64 mismatches = 0;

        for (int c = 0; c < 2; ++c)
            for (juce::int64 f = 0; f < frame; ++f)
                mismatches += all.getSample (c, (int) f) == inputValue (2 + c, f) ? 0 : 1;

        check (mismatches == 0, "recorder: every sample of a 30 s stereo take is exact (" + juce::String (mismatches) + " wrong)");
        check (maxPeak >= inputValue (3, 99999) - 1.0e-4f, "recorder: meter peaks follow the input");
        check (take.anchorPlayed < 0, "recorder: no anchor without playback");
    }

    void testAnchor()
    {
        std::atomic<juce::int64> played { 1000 };
        Recorder recorder (played);
        recorder.setInput (0, 1);
        FakeDevice device (recorder, bits ({ 0 }));
        auto silence = [] (int, int) { return 0.0f; };

        recorder.start();
        device.cycle (silence);                       // stopped
        played = 1000 + blockSize;                    // a relocation that happens to be one block long...
        device.cycle (silence);
        played = 50000;                               // ...then a real start elsewhere
        device.cycle (silence);
        played = 50000 + blockSize;                   // first played block
        device.cycle (silence);
        check (recorder.stop().anchorPlayed < 0, "recorder: a single one-block step is not taken as playback");

        played = 1000;
        recorder.start();
        device.cycle (silence);                       // recorded [0, 256): stopped
        device.cycle (silence);                       // [256, 512): stopped
        played += blockSize;
        device.cycle (silence);                       // [512, 768): played [1000, 1256)
        played += blockSize;
        device.cycle (silence);                       // [768, 1024): played [1256, 1512)
        const auto take = recorder.stop();
        check (take.anchorRecorded == 3 * blockSize && take.anchorPlayed == 1000 + blockSize,
               "recorder: anchor pairs recorded frame " + juce::String (take.anchorRecorded)
                   + " with document frame " + juce::String (take.anchorPlayed));
    }

    /** A loop cable through a simulated interface: the input hears the document's own audio,
        roundTrip frames after the player sent it. The punched-in take must line up exactly. */
    void testPunchInLoopback()
    {
        constexpr int roundTrip = 517;   // odd, and not a multiple of the block
        std::atomic<juce::int64> played { 0 };
        Recorder recorder (played);
        recorder.setInput (1, 1);
        FakeDevice device (recorder, bits ({ 0, 1 }));

        auto documentValue = [] (juce::int64 frame) { return frame < 0 ? 0.0f : (float) frame * 1.0e-6f; };

        const juce::int64 punchIn = 96000, punchOut = 144000, preroll = 96000;
        played = punchIn - preroll;

        // Ten idle cycles (the recorder starts before the player), then playback.
        recorder.start();

        for (int b = 0; b < 10; ++b)
            device.cycle ([] (int, int) { return 0.0f; });

        while (played.load() < punchOut + roundTrip + 4 * blockSize)
        {
            // The player runs first in the cycle and advances; the input carries what it sent
            // roundTrip frames earlier.
            const auto start = played.load();
            played = start + blockSize;
            device.cycle ([&] (int, int i) { return documentValue (start + i - roundTrip); });
        }

        const auto take = recorder.stop();
        const auto p = Recorder::place (take, punchIn, punchOut, roundTrip);
        check (p.destStart == punchIn && p.takeEnd - p.takeStart == punchOut - punchIn,
               "punch-in: placed at " + juce::String (p.destStart) + " for " + juce::String (p.takeEnd - p.takeStart) + " frames");

        bool aligned = true;

        for (auto f = p.takeStart; aligned && f < p.takeEnd; ++f)
            aligned = sampleAt (take.clip, 0, f) == documentValue (p.destStart + (f - p.takeStart));

        check (aligned, "punch-in: the take lines up sample for sample after latency compensation");

        const auto open = Recorder::place (take, punchIn, -1, roundTrip);
        check (open.destStart == punchIn && open.takeEnd == take.clip.getNumFrames(), "punch-in: without a punch-out the take runs to its end");
    }

    void testPlacementEdges()
    {
        Recorder::Take take;
        juce::AudioBuffer<float> b (1, 1000);
        b.clear();
        take.clip = { 1, rate, { { std::make_shared<const AudioBlock> (std::move (b)), 0, 1000 } } };

        auto p = Recorder::place (take, 500, 700, 100);
        check (p.destStart == 500 && p.takeStart == 0 && p.takeEnd == 200,
               "placement: without an anchor the take starts at the punch-in and stops at the punch-out");

        take.anchorRecorded = 0;
        take.anchorPlayed = 450;   // recorded frame r is document frame r + 350 after 100 frames of latency
        p = Recorder::place (take, 300, 2000, 100);
        check (p.takeStart == 0 && p.destStart == 350 && p.takeEnd == 1000,
               "placement: a punch-in before the take starts is clipped to the take");
    }
}

void runRecorderTests()
{
    testMapping();
    testCapture();
    testAnchor();
    testPunchInLoopback();
    testPlacementEdges();
}
