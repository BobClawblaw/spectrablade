#pragma once

#include "AudioEdits.h"

#include <thread>

/** Captures audio from chosen device inputs. Runs as its own device callback, registered after the
    player, so in each cycle it can see which document frames the player has just played; that pairing
    (the anchor) is what lines a punch-in up with the existing audio.

    The audio thread only copies into a lock-free FIFO; a writer thread moves the data into blocks. */
class Recorder final : public juce::AudioIODeviceCallback
{
public:
    /** playPosition is the player's next document frame (DocumentSource's position). */
    explicit Recorder (const std::atomic<juce::int64>& playPosition);
    ~Recorder() override;

    /** Device input channel (absolute index) and 1 or 2 channels. */
    void setInput (int firstChannel, int numChannels);
    int getInputFirstChannel() const noexcept     { return inputFirst.load(); }
    int getInputNumChannels() const noexcept      { return inputCount.load(); }

    /** Which device inputs are active, and the device rate. Called by the device, and by tests. */
    void configure (double sampleRate, const juce::BigInteger& activeInputs);
    bool isInputActive() const noexcept           { return mappedFirst.load() >= 0; }

    /** Peak level of input channel 0 or 1 since the last call (for meters). */
    float takePeak (int channel);

    struct Take
    {
        AudioClip clip;                  // the recorded audio
        juce::int64 anchorRecorded = -1; // a recorded frame...
        juce::int64 anchorPlayed = -1;   // ...and the document frame playing in that same cycle (-1: no playback)
        juce::int64 droppedFrames = 0;   // lost to a full FIFO (should be 0)
    };

    bool start();
    Take stop();
    bool isRecording() const noexcept             { return recording.load(); }
    juce::int64 getRecordedFrames() const noexcept { return framesCaptured.load(); }

    /** Where a take lands in the document: recorded frames [takeStart, takeEnd) go to document frame destStart. */
    struct Placement
    {
        juce::int64 takeStart = 0, takeEnd = 0, destStart = 0;
    };

    /** Keeps the part of the take that maps into [punchIn, punchOut) of the document
        (punchOut < 0: no limit), shifting by the round-trip latency. Without an anchor (nothing
        was playing) the take starts at punchIn as it is. */
    static Placement place (const Take&, juce::int64 punchIn, juce::int64 punchOut, int roundTripLatency);

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override            {}

private:
    class Writer;
    void drain();
    void updateMapping();

    const std::atomic<juce::int64>& playPosition;

    std::atomic<int> inputFirst { 0 }, inputCount { 2 }, mappedFirst { -1 }, mappedCount { 0 };
    juce::BigInteger activeInputs;
    double sampleRate = 48000.0;

    std::atomic<bool> recording { false }, inCallback { false };
    std::atomic<juce::int64> framesCaptured { 0 }, dropped { 0 };
    std::atomic<juce::int64> anchorRecorded { -1 }, anchorPlayed { -1 };
    juce::int64 lastPlayPosition = -1;
    int steadyBlocks = 0;
    std::atomic<float> peaks[2] { { 0.0f }, { 0.0f } };

    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> ring;
    int recordChannels = 2;

    // Writer side.
    juce::AudioBuffer<float> chunk;
    int chunkFill = 0;
    std::vector<AudioSnapshot::Segment> segments;
    std::unique_ptr<Writer> writer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
