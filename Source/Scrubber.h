#pragma once

#include "AudioDocument.h"

/** Scrubbing: hearing the audio at a place as it is dragged over, like tape moved by hand past the
    head. While it is on, a read head follows the place it is given (the target) over about 50 ms,
    no faster than four times normal speed either way, its speed smoothed over 10 ms, reading
    between samples with a cubic (Catmull-Rom) curve. So dragging at normal speed plays the audio
    as it is, faster raises its pitch, backwards plays it backwards, and holding still lets the head
    catch up and fall silent: the level follows the speed, full from a quarter of normal up.

    Everything is worked out sample by sample, so how the device splits the sound into blocks
    changes nothing. The target and on/off are set from the message thread; render runs on the
    audio thread. */
class Scrubber
{
public:
    static constexpr double lagSeconds = 0.05, smoothSeconds = 0.01, fullLevelSpeed = 0.25, fastest = 4.0;

    /** Makes room for blocks of up to this many device frames, at up to four audio frames a
        device frame, so render doesn't allocate. */
    void prepare (int blockSize);

    /** Starts scrubbing at this frame: the head is put there, silent until the target moves. */
    void start (juce::int64 frame);

    /** Where the head is to follow: the frame dragged over. */
    void moveTo (juce::int64 frame);

    /** Stops: the sound fades out in a straight line over the smoothing time (10 ms). */
    void stop();

    bool isOn() const noexcept                    { return on.load(); }

    /** Whether there is sound still to make: on, or fading out. Audio thread. */
    bool isSounding() const noexcept              { return on.load() || level > 0.0 || restart.load(); }

    /** Adds the scrubbed sound of the audio to the first two channels of [start, start + n) of the
        buffer (a mono file on both, a file past stereo its first two channels), the device running
        at deviceRate and the audio at its own rate. Audio thread. */
    void render (const AudioSnapshot&, double audioRate, double deviceRate, juce::AudioBuffer<float>&, int start, int n);

    /** Where the head is now, in frames of the audio (for tests). */
    double getHead() const noexcept               { return head; }

private:
    std::atomic<bool> on { false }, restart { false };
    std::atomic<double> target { 0.0 }, startAt { 0.0 };   // startAt: where start() puts the head, whatever moves follow before it is rendered

    // The audio thread's own.
    double head = 0.0, speed = 0.0, level = 0.0;
    juce::AudioBuffer<float> window { 2, 0 };   // the first two channels' frames around the head
};
