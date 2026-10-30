#include "WaveformView.h"

namespace
{
    constexpr int rulerHeight = 22;
    constexpr int scrollBarHeight = 12;
    constexpr double minFramesPerPixel = 1.0 / 64.0;

    const juce::Colour background  (0xff15181d);
    const juce::Colour laneLine    (0xff2a2f37);
    const juce::Colour waveColour  (0xff4fc3a1);
    const juce::Colour selectColour (0x3346a0ff);
    const juce::Colour cursorColour (0xffffd34d);

    juce::String formatTime (double seconds, double secondsPerTick)
    {
        const auto mins = (int) (seconds / 60.0);
        const auto secs = seconds - mins * 60.0;
        const auto decimals = secondsPerTick >= 1.0 ? 0 : secondsPerTick >= 0.1 ? 1 : secondsPerTick >= 0.01 ? 2 : 3;
        return juce::String (mins) + ":" + (secs < 10.0 ? "0" : "") + juce::String (secs, decimals);
    }
}

WaveformView::WaveformView (AudioEngine& e) : engine (e)
{
    addAndMakeVisible (scrollBar);
    scrollBar.addListener (this);
    scrollBar.setAutoHide (false);
    engine.document.addChangeListener (this);
    startTimerHz (30);
    setWantsKeyboardFocus (false);
}

WaveformView::~WaveformView()
{
    engine.document.removeChangeListener (this);
}

//==============================================================================
juce::Rectangle<int> WaveformView::getWaveArea() const
{
    return getLocalBounds().withTrimmedTop (rulerHeight).withTrimmedBottom (scrollBarHeight);
}

double WaveformView::frameToX (juce::int64 frame) const
{
    return ((double) frame - viewStart) / framesPerPixel;
}

juce::int64 WaveformView::xToFrame (float x) const
{
    const auto frame = (juce::int64) std::llround (viewStart + (double) x * framesPerPixel);
    return juce::jlimit ((juce::int64) 0, engine.document.getNumFrames(), frame);
}

void WaveformView::setView (double startFrame, double fpp)
{
    const auto length = (double) engine.document.getNumFrames();
    const auto width = juce::jmax (1, getWidth());
    const auto maxFpp = juce::jmax (minFramesPerPixel, length / width);

    framesPerPixel = juce::jlimit (minFramesPerPixel, maxFpp, fpp);
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - framesPerPixel * width), startFrame);

    scrollBar.setRangeLimits (0.0, juce::jmax (1.0, length), juce::dontSendNotification);
    scrollBar.setCurrentRange (viewStart, framesPerPixel * width, juce::dontSendNotification);
    invalidateWaveform();
}

void WaveformView::zoomAround (float x, double factor)
{
    const auto anchor = viewStart + (double) x * framesPerPixel;
    const auto fpp = framesPerPixel * factor;
    setView (anchor - (double) x * fpp, fpp);
}

void WaveformView::zoomIn()        { zoomAround ((float) getWidth() * 0.5f, 0.5); }
void WaveformView::zoomOut()       { zoomAround ((float) getWidth() * 0.5f, 2.0); }
void WaveformView::zoomToFit()     { setView (0.0, 1.0e18); }

void WaveformView::zoomToSelection()
{
    if (! selection.isEmpty())
        setView ((double) selection.getStart(), (double) selection.getLength() / juce::jmax (1, getWidth()));
}

//==============================================================================
void WaveformView::setSelection (juce::Range<juce::int64> r)
{
    const auto length = engine.document.getNumFrames();
    r = { juce::jlimit ((juce::int64) 0, length, r.getStart()), juce::jlimit ((juce::int64) 0, length, r.getEnd()) };
    selection = r;
    cursor = r.getStart();
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void WaveformView::setCursor (juce::int64 frame)
{
    setSelection ({ frame, frame });
}

void WaveformView::selectAll()
{
    setSelection ({ 0, engine.document.getNumFrames() });
}

//==============================================================================
void WaveformView::invalidateWaveform()
{
    waveformValid = false;
    repaint();
}

void WaveformView::renderWaveform()
{
    const auto area = getWaveArea();
    waveformImage = juce::Image (juce::Image::ARGB, juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), true);
    juce::Graphics g (waveformImage);
    g.fillAll (background);
    waveformValid = true;

    const auto snapshot = engine.document.getSnapshot();
    const auto channels = snapshot->getNumChannels();

    if (snapshot->getNumFrames() == 0 || channels == 0)
        return;

    const auto laneHeight = (float) waveformImage.getHeight() / (float) channels;
    const auto width = waveformImage.getWidth();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto top = laneHeight * (float) ch;
        const auto mid = top + laneHeight * 0.5f;
        const auto half = laneHeight * 0.45f;

        g.setColour (laneLine);
        g.drawHorizontalLine ((int) mid, 0.0f, (float) width);

        if (ch > 0)
            g.drawHorizontalLine ((int) top, 0.0f, (float) width);

        g.setColour (waveColour);

        if (framesPerPixel >= 1.0)
        {
            // One min/max bar per pixel column, from the blocks' peak tables.
            for (int x = 0; x < width; ++x)
            {
                const auto a = (juce::int64) (viewStart + x * framesPerPixel);
                const auto b = juce::jmax (a + 1, (juce::int64) (viewStart + (x + 1) * framesPerPixel));

                if (a >= snapshot->getNumFrames())
                    break;

                const auto peak = snapshot->getPeak (ch, a, b);
                const auto y1 = mid - juce::jlimit (-1.0f, 1.0f, peak.getEnd()) * half;
                const auto y2 = mid - juce::jlimit (-1.0f, 1.0f, peak.getStart()) * half;
                g.drawVerticalLine (x, y1, juce::jmax (y1 + 1.0f, y2));
            }
        }
        else
        {
            // Zoomed in past one frame per pixel: draw the samples themselves.
            const auto first = (juce::int64) viewStart;
            const auto count = (int) juce::jmin ((juce::int64) (width * framesPerPixel) + 3, snapshot->getNumFrames() - first);

            if (count <= 0)
                continue;

            juce::AudioBuffer<float> samples (channels, count);
            snapshot->read (samples, 0, first, count);
            juce::Path path;

            for (int i = 0; i < count; ++i)
            {
                const auto x = (float) frameToX (first + i);
                const auto y = mid - juce::jlimit (-1.0f, 1.0f, samples.getSample (ch, i)) * half;

                if (i == 0)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);

                if (framesPerPixel < 1.0 / 8.0)
                    g.fillEllipse (x - 2.0f, y - 2.0f, 4.0f, 4.0f);
            }

            g.strokePath (path, juce::PathStrokeType (1.2f));
        }
    }
}

void WaveformView::drawRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff1b1e23));
    g.fillRect (area);

    const auto rate = engine.document.getSampleRate();

    if (rate <= 0.0 || ! engine.document.hasAudio())
        return;

    // Pick a tick spacing of roughly 100 px from a 1-2-5 series.
    const auto secondsPerPixel = framesPerPixel / rate;
    double tick = 0.001;

    for (auto candidate : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
    {
        tick = candidate;

        if (candidate / secondsPerPixel >= 100.0)
            break;
    }

    g.setFont (11.0f);
    const auto startSeconds = viewStart / rate;
    const auto endSeconds = startSeconds + getWidth() * secondsPerPixel;

    for (auto t = std::floor (startSeconds / tick) * tick; t <= endSeconds; t += tick)
    {
        const auto x = (float) ((t * rate - viewStart) / framesPerPixel);
        g.setColour (juce::Colours::grey);
        g.drawVerticalLine ((int) x, (float) area.getBottom() - 6.0f, (float) area.getBottom());
        g.setColour (juce::Colours::lightgrey);
        g.drawText (formatTime (juce::jmax (0.0, t), tick), juce::Rectangle<float> (x + 3.0f, (float) area.getY(), 80.0f, (float) area.getHeight() - 4.0f),
                    juce::Justification::centredLeft, false);
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto area = getWaveArea();
    drawRuler (g, getLocalBounds().removeFromTop (rulerHeight));

    if (! engine.document.hasAudio())
    {
        g.setColour (background);
        g.fillRect (area);
        g.setColour (juce::Colours::grey);
        g.setFont (16.0f);
        g.drawText ("Drop an audio file here, or use File > Open", area, juce::Justification::centred);
        return;
    }

    if (! waveformValid || waveformImage.getWidth() != area.getWidth() || waveformImage.getHeight() != area.getHeight())
        renderWaveform();

    g.drawImageAt (waveformImage, area.getX(), area.getY());

    if (! selection.isEmpty())
    {
        const auto x1 = juce::jmax (0.0, frameToX (selection.getStart()));
        const auto x2 = juce::jmin ((double) getWidth(), frameToX (selection.getEnd()));
        g.setColour (selectColour);
        g.fillRect (juce::Rectangle<double> (x1, area.getY(), juce::jmax (1.0, x2 - x1), area.getHeight()).toFloat());
    }

    g.setColour (cursorColour);
    g.drawVerticalLine ((int) frameToX (cursor), (float) area.getY(), (float) area.getBottom());

    if (engine.getPlayer().isPlaying())
    {
        g.setColour (juce::Colours::white);
        g.drawVerticalLine ((int) frameToX (engine.getPlayer().getPositionFrames()), 0.0f, (float) area.getBottom());
    }
}

void WaveformView::resized()
{
    scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollBarHeight));
    setView (viewStart, framesPerPixel);
}

//==============================================================================
void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (! engine.document.hasAudio())
        return;

    const auto frame = xToFrame (e.position.x);

    if (e.mods.isShiftDown())
    {
        // Extend from whichever edge is farther away.
        const auto anchorAtStart = std::abs (frame - selection.getStart()) > std::abs (frame - selection.getEnd());
        dragAnchor = anchorAtStart ? selection.getStart() : selection.getEnd();
        setSelection (juce::Range<juce::int64>::between (dragAnchor, frame));
    }
    else
    {
        dragAnchor = frame;
        setCursor (frame);
    }
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (! engine.document.hasAudio())
        return;

    // Auto-scroll when dragging past the edges.
    if (e.position.x < 0.0f)
        setView (viewStart + e.position.x * framesPerPixel * 0.1, framesPerPixel);
    else if (e.position.x > (float) getWidth())
        setView (viewStart + (e.position.x - (float) getWidth()) * framesPerPixel * 0.1, framesPerPixel);

    const auto frame = xToFrame (e.position.x);
    setSelection (juce::Range<juce::int64>::between (dragAnchor, frame));
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        setView (viewStart - (double) wheel.deltaX * getWidth() * framesPerPixel * 0.5, framesPerPixel);
    else if (wheel.deltaY != 0.0f)
        zoomAround (e.position.x, std::pow (2.0, (double) -wheel.deltaY * 2.0));
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (scaleFactor > 0.0f)
        zoomAround (e.position.x, 1.0 / (double) scaleFactor);
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    setView (newRangeStart, framesPerPixel);
}

//==============================================================================
bool WaveformView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& path : files)
        if (engine.formatManager.findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            return true;

    return false;
}

void WaveformView::filesDropped (const juce::StringArray& files, int, int)
{
    if (onFileDropped != nullptr && ! files.isEmpty())
        onFileDropped (juce::File (files[0]));
}

void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (engine.document.getResetCount() != lastResetCount)
    {
        // A new file: fit it to the window.
        lastResetCount = engine.document.getResetCount();
        lastVersion = engine.document.getVersion();
        zoomToFit();
        setCursor (0);
        return;
    }

    if (engine.document.getVersion() != lastVersion)
    {
        // An edit, undo or redo: keep the view, clamp the selection to the new length.
        lastVersion = engine.document.getVersion();
        const auto length = engine.document.getNumFrames();
        setView (viewStart, framesPerPixel);
        setSelection ({ juce::jmin (selection.getStart(), length), juce::jmin (selection.getEnd(), length) });
    }

    invalidateWaveform();
}

void WaveformView::timerCallback()
{
    engine.document.releaseUnusedSnapshots();

    auto& player = engine.getPlayer();

    if (! player.isPlaying())
        return;

    // Stop at the end of a played range, and keep the playhead on screen.
    const auto pos = player.getPositionFrames();

    if (player.getStopFrame() >= 0 && pos >= player.getStopFrame())
        player.pause();

    const auto x = frameToX (pos);

    if (x > getWidth() * 0.95 || x < 0.0)
        setView ((double) pos - getWidth() * framesPerPixel * 0.05, framesPerPixel);

    repaint();
}
