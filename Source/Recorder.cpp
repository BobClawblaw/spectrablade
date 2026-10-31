#include "Recorder.h"

namespace
{
    constexpr int framesPerBlock = 1 << 20;
    constexpr double fifoSeconds = 4.0;
}

//==============================================================================
class Recorder::Writer final : public juce::Thread
{
public:
    explicit Writer (Recorder& r) : Thread ("Waveloom recorder"), owner (r) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            owner.drain();
            wait (5);
        }
    }

private:
    Recorder& owner;
};

//==============================================================================
Recorder::Recorder (const std::atomic<juce::int64>& position) : playPosition (position) {}

Recorder::~Recorder()
{
    if (recording)
        stop();
}

void Recorder::setInput (int firstChannel, int numChannels)
{
    jassert (! recording);
    inputFirst = juce::jmax (0, firstChannel);
    inputCount = juce::jlimit (1, 2, numChannels);
    updateMapping();
}

void Recorder::configure (double rate, const juce::BigInteger& active)
{
    sampleRate = rate;
    activeInputs = active;
    updateMapping();
}

void Recorder::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    configure (device->getCurrentSampleRate(), device->getActiveInputChannels());
}

void Recorder::updateMapping()
{
    // The callback sees only active inputs, in order, so map absolute channels to that order.
    const auto first = inputFirst.load();
    const auto count = inputCount.load();
    bool allActive = true;

    for (int ch = first; ch < first + count; ++ch)
        allActive = allActive && activeInputs[ch];

    if (! allActive)
    {
        mappedFirst = -1;
        mappedCount = 0;
        return;
    }

    int index = 0;

    for (int ch = 0; ch < first; ++ch)
        if (activeInputs[ch])
            ++index;

    mappedCount = count;
    mappedFirst = index;
}

float Recorder::takePeak (int channel)
{
    return juce::isPositiveAndBelow (channel, 2) ? peaks[channel].exchange (0.0f) : 0.0f;
}

//==============================================================================
bool Recorder::start()
{
    if (recording || ! isInputActive())
        return false;

    recordChannels = mappedCount.load();
    const auto ringSize = (int) (sampleRate * fifoSeconds);
    ring.setSize (recordChannels, ringSize);
    fifo.setTotalSize (ringSize);
    fifo.reset();

    chunk.setSize (recordChannels, framesPerBlock);
    chunkFill = 0;
    segments.clear();

    framesCaptured = 0;
    dropped = 0;
    anchorRecorded = -1;
    anchorPlayed = -1;
    lastPlayPosition = -1;
    steadyBlocks = 0;

    writer = std::make_unique<Writer> (*this);
    writer->startThread (juce::Thread::Priority::high);
    recording = true;
    return true;
}

Recorder::Take Recorder::stop()
{
    recording = false;

    // Wait for a callback that may still be writing.
    while (inCallback.load())
        std::this_thread::yield();

    if (writer != nullptr)
    {
        writer->stopThread (2000);
        writer = nullptr;
    }

    drain();

    if (chunkFill > 0)
    {
        chunk.setSize (recordChannels, chunkFill, true);
        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (chunk)), 0, chunkFill });
        chunkFill = 0;
    }

    Take take;
    take.clip = { recordChannels, sampleRate, std::move (segments) };
    take.anchorRecorded = anchorRecorded;
    take.anchorPlayed = anchorPlayed;
    take.droppedFrames = dropped;
    segments.clear();
    return take;
}

void Recorder::drain()
{
    for (;;)
    {
        const auto available = fifo.getNumReady();

        if (available <= 0)
            return;

        const auto count = juce::jmin (available, framesPerBlock - chunkFill);
        const auto scope = fifo.read (count);

        for (int ch = 0; ch < recordChannels; ++ch)
        {
            if (scope.blockSize1 > 0)
                chunk.copyFrom (ch, chunkFill, ring, ch, scope.startIndex1, scope.blockSize1);

            if (scope.blockSize2 > 0)
                chunk.copyFrom (ch, chunkFill + scope.blockSize1, ring, ch, scope.startIndex2, scope.blockSize2);
        }

        chunkFill += count;

        if (chunkFill == framesPerBlock)
        {
            segments.push_back ({ std::make_shared<const AudioBlock> (std::move (chunk)), 0, framesPerBlock });
            chunk = juce::AudioBuffer<float> (recordChannels, framesPerBlock);
            chunkFill = 0;
        }
    }
}

//==============================================================================
void Recorder::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                 float* const* outputs, int numOutputs, int numSamples,
                                                 const juce::AudioIODeviceCallbackContext&)
{
    inCallback = true;

    // This callback adds nothing to the output.
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    const auto first = mappedFirst.load();
    const auto count = mappedCount.load();

    if (first >= 0 && first + count <= numInputs)
    {
        for (int c = 0; c < count; ++c)
        {
            const auto level = juce::FloatVectorOperations::findMaximum (inputs[first + c], numSamples);
            const auto low = juce::FloatVectorOperations::findMinimum (inputs[first + c], numSamples);
            const auto peak = juce::jmax (level, -low);

            if (peak > peaks[c].load())
                peaks[c] = peak;
        }

        if (recording.load())
        {
            const auto captured = framesCaptured.load();
            const auto position = playPosition.load();

            // The player runs before us in each cycle: if it advanced by exactly this block,
            // it played [position - numSamples, position) in the cycle that delivered these input
            // frames. Two such steps in a row rule out a jump (a new play position) that happens to
            // equal one block.
            steadyBlocks = (lastPlayPosition >= 0 && position - lastPlayPosition == numSamples) ? steadyBlocks + 1 : 0;

            if (anchorPlayed.load() < 0 && steadyBlocks >= 2)
            {
                anchorRecorded = captured;
                anchorPlayed = position - numSamples;
            }

            lastPlayPosition = position;

            const auto channels = juce::jmin (count, recordChannels);
            const auto scope = fifo.write (numSamples);
            const auto written = scope.blockSize1 + scope.blockSize2;

            for (int c = 0; c < channels; ++c)
            {
                if (scope.blockSize1 > 0)
                    ring.copyFrom (c, scope.startIndex1, inputs[first + c], scope.blockSize1);

                if (scope.blockSize2 > 0)
                    ring.copyFrom (c, scope.startIndex2, inputs[first + c] + scope.blockSize1, scope.blockSize2);
            }

            framesCaptured = captured + written;

            if (written < numSamples)
                dropped += numSamples - written;
        }
    }

    inCallback = false;
}

//==============================================================================
Recorder::Placement Recorder::place (const Take& take, juce::int64 punchIn, juce::int64 punchOut, int roundTripLatency)
{
    const auto length = take.clip.getNumFrames();

    // Document frame of recorded frame r is r + offset.
    const auto offset = take.anchorPlayed >= 0
                            ? take.anchorPlayed - take.anchorRecorded - roundTripLatency
                            : punchIn;

    Placement p;
    p.takeStart = juce::jlimit ((juce::int64) 0, length, punchIn - offset);
    p.takeEnd = punchOut >= 0 ? juce::jlimit (p.takeStart, length, punchOut - offset) : length;
    p.destStart = p.takeStart + offset;
    return p;
}
