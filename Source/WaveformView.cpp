#include "WaveformView.h"
#include "Theme.h"

namespace
{
    constexpr int rulerHeight = 22;
    constexpr int markerLaneHeight = 18;
    constexpr int scrollBarHeight = 12;
    constexpr double minFramesPerPixel = 1.0 / 64.0;

    const juce::Colour background  (0xff15181d);
    const juce::Colour laneLine    (0xff2a2f37);
    const juce::Colour waveColour  (0xff4fc3a1);
    const juce::Colour selectColour (0x3346a0ff);
    const juce::Colour cursorColour = Theme::cursor;
    const juce::Colour markerColour (0xffff8a3d);
    const juce::Colour regionColour (0xff58a6ff);

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
    return getLocalBounds().withTrimmedTop (rulerHeight + markerLaneHeight).withTrimmedBottom (scrollBarHeight);
}

juce::Rectangle<int> WaveformView::getMarkerLane() const
{
    return getLocalBounds().withTrimmedTop (rulerHeight).withHeight (markerLaneHeight);
}

const Markers& WaveformView::shownMarkers() const
{
    return draggedMarker >= 0 ? dragMarkers : engine.document.getMarkers();
}

int WaveformView::markerAt (juce::Point<float> p) const
{
    if (! getMarkerLane().toFloat().contains (p))
        return -1;

    const auto& markers = shownMarkers();

    // Point markers win over regions (their flags are small); among them, the nearest.
    int best = -1;
    auto bestDistance = 7.0;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        if (markers[i].isRegion())
            continue;

        const auto d = std::abs (frameToX (markers[i].start) - (double) p.x);

        if (d < bestDistance)
        {
            best = (int) i;
            bestDistance = d;
        }
    }

    if (best >= 0)
        return best;

    // The shortest region under the mouse, so nested regions stay reachable.
    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];

        if (m.isRegion() && (double) p.x >= frameToX (m.start) - 3.0 && (double) p.x <= frameToX (m.getEnd()) + 3.0
            && (best < 0 || m.length < markers[(size_t) best].length))
            best = (int) i;
    }

    return best;
}

void WaveformView::drawMarkers (juce::Graphics& g)
{
    const auto lane = getMarkerLane().toFloat();
    const auto wave = getWaveArea().toFloat();

    g.setColour (Theme::raised);
    g.fillRect (lane);

    if (! engine.document.hasAudio())
        return;

    g.setFont (11.0f);
    const auto& markers = shownMarkers();

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];
        const auto x1 = (float) frameToX (m.start);
        const auto active = (int) i == draggedMarker;

        if (m.isRegion())
        {
            const auto x2 = (float) frameToX (m.getEnd());

            if (x2 < 0.0f || x1 > (float) getWidth())
                continue;

            const auto bar = juce::Rectangle<float> (x1, lane.getY() + 2.0f, juce::jmax (2.0f, x2 - x1), lane.getHeight() - 4.0f);
            g.setColour (regionColour.withAlpha (active ? 0.75f : 0.45f));
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (regionColour.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (x1, wave.getY(), x2 - x1, wave.getHeight()));
            g.setColour (regionColour.withAlpha (0.6f));
            g.drawVerticalLine ((int) x1, wave.getY(), wave.getBottom());
            g.drawVerticalLine ((int) x2, wave.getY(), wave.getBottom());
            g.setColour (juce::Colours::white);
            g.drawText (m.name, bar.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        else
        {
            if (x1 < -100.0f || x1 > (float) getWidth())
                continue;

            juce::Path flag;
            flag.addTriangle (x1 - 5.0f, lane.getY() + 2.0f, x1 + 5.0f, lane.getY() + 2.0f, x1, lane.getBottom() - 2.0f);
            g.setColour (active ? juce::Colours::white : markerColour);
            g.fillPath (flag);

            const float dashes[] = { 4.0f, 3.0f };
            g.setColour (markerColour.withAlpha (0.7f));
            g.drawDashedLine (juce::Line<float> (x1, wave.getY(), x1, wave.getBottom()), dashes, 2);
            g.setColour (Theme::text);
            g.drawText (m.name, juce::Rectangle<float> (x1 + 7.0f, lane.getY(), 120.0f, lane.getHeight()),
                        juce::Justification::centredLeft, true);
        }
    }
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

    if (r.isEmpty())
        band = {};
    repaint();

    if (onSelectionChanged != nullptr)
        onSelectionChanged();
}

void WaveformView::setCursor (juce::int64 frame)
{
    setSelection ({ frame, frame });
}

void WaveformView::showFrame (juce::int64 frame)
{
    setCursor (frame);
    const auto width = framesPerPixel * juce::jmax (1, getWidth());

    if ((double) frame < viewStart || (double) frame >= viewStart + width)
        setView ((double) frame - width * 0.1, framesPerPixel);
}

FilePlace WaveformView::getPlace() const
{
    return { selection, cursor, viewStart, framesPerPixel, band };
}

void WaveformView::setPlace (const FilePlace& place)
{
    // Taken as the file's arrival, so the change it brings doesn't fit it to the window afterwards.
    lastResetCount = engine.document.getResetCount();
    lastVersion = engine.document.getVersion();

    if (place.framesPerPixel <= 0.0)
    {
        zoomToFit();
        setCursor (0);
        return;
    }

    setView (place.viewStart, place.framesPerPixel);
    setSelection (place.selection);
    band = place.selection.isEmpty() ? juce::Range<double>() : place.band;
    cursor = juce::jlimit ((juce::int64) 0, engine.document.getNumFrames(), place.cursor);
    repaint();
}

void WaveformView::selectAll()
{
    band = {};
    setSelection ({ 0, engine.document.getNumFrames() });
}

//==============================================================================
void WaveformView::invalidateWaveform()
{
    waveformValid = false;
    repaint();
}

WaveformView::Lane WaveformView::getLane (int channel, int numChannels) const
{
    const auto area = getWaveArea();
    const auto laneHeight = area.getHeight() / juce::jmax (1, numChannels);
    auto lane = juce::Rectangle<int> (0, laneHeight * channel, area.getWidth(), laneHeight);

    switch (display)
    {
        case Display::waveform:    return { lane, {} };
        case Display::spectrogram: return { {}, lane };
        case Display::both:        break;
    }

    const auto wave = lane.removeFromTop (juce::roundToInt (lane.getHeight() * waveShare));
    return { wave, lane.withTrimmedTop (1) };
}

int WaveformView::dividerAt (juce::Point<float> p) const
{
    if (display != Display::both || ! engine.document.hasAudio())
        return -1;

    const auto channels = engine.document.getNumChannels();
    const auto area = getWaveArea();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto line = (float) (area.getY() + getLane (ch, channels).wave.getBottom());

        if (std::abs (p.y - line) <= 3.0f && p.x >= (float) area.getX() && p.x < (float) area.getRight())
            return ch;
    }

    return -1;
}

void WaveformView::setWaveformShare (double share)
{
    const auto s = juce::jlimit (0.1, 0.9, share);

    if (juce::exactlyEqual (s, waveShare))
        return;

    waveShare = s;

    if (display == Display::both)
        invalidateWaveform();
}

void WaveformView::setDisplay (Display d)
{
    display = d;
    invalidateWaveform();
}

void WaveformView::drawFrequencyScale (juce::Graphics& g, juce::Rectangle<int> spectrum, double sampleRate)
{
    g.setFont (10.0f);

    for (auto f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        if (f >= sampleRate * 0.5)
            break;

        const auto y = (float) spectrum.getY() + Spectrogram::frequencyToY (f, sampleRate, spectrum.getHeight());

        if (y < (float) spectrum.getY() + 6.0f || y > (float) spectrum.getBottom() - 4.0f)
            continue;

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawHorizontalLine ((int) y, (float) spectrum.getX(), (float) spectrum.getX() + 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> ((float) spectrum.getX() + 8.0f, y - 6.0f, 40.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    }
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

    const auto width = waveformImage.getWidth();
    const auto rate = engine.document.getSampleRate();

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto lane = getLane (ch, channels);

        if (ch > 0)
        {
            g.setColour (laneLine);
            g.drawHorizontalLine (juce::jmin (lane.wave.isEmpty() ? lane.spectrum.getY() : lane.wave.getY(), area.getHeight()) - 1,
                                  0.0f, (float) width);
        }

        if (! lane.spectrum.isEmpty())
        {
            g.drawImageAt (Spectrogram::render (*snapshot, ch, rate, viewStart, framesPerPixel, width, lane.spectrum.getHeight()),
                           lane.spectrum.getX(), lane.spectrum.getY());
            drawFrequencyScale (g, lane.spectrum, rate);
        }

        if (lane.wave.isEmpty())
            continue;

        const auto top = (float) lane.wave.getY();
        const auto mid = top + (float) lane.wave.getHeight() * 0.5f;
        const auto half = (float) lane.wave.getHeight() * 0.45f;

        g.setColour (laneLine);
        g.drawHorizontalLine ((int) mid, 0.0f, (float) width);
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
    g.setColour (Theme::surface);
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
        g.setColour (Theme::dimText);
        g.drawVerticalLine ((int) x, (float) area.getBottom() - 6.0f, (float) area.getBottom());
        g.setColour (Theme::text);
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
        drawMarkers (g);
        g.setColour (background);
        g.fillRect (area);
        drawEmpty (g, area);
        return;
    }

    if (! waveformValid || waveformImage.getWidth() != area.getWidth() || waveformImage.getHeight() != area.getHeight())
        renderWaveform();

    g.drawImageAt (waveformImage, area.getX(), area.getY());
    drawMarkers (g);

    if (! selection.isEmpty())
    {
        const auto x1 = (float) juce::jmax (0.0, frameToX (selection.getStart()));
        const auto x2 = (float) juce::jmin ((double) getWidth(), frameToX (selection.getEnd()));
        const auto w = juce::jmax (1.0f, x2 - x1);

        if (band.isEmpty())
        {
            g.setColour (selectColour);
            g.fillRect (juce::Rectangle<float> (x1, (float) area.getY(), w, (float) area.getHeight()));
        }
        else
        {
            // A spectral box: drawn on every channel's spectrogram, since edits apply to all channels.
            const auto channels = engine.document.getNumChannels();
            const auto rate = engine.document.getSampleRate();

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto spectrum = getLane (ch, channels).spectrum.translated (area.getX(), area.getY());

                if (spectrum.isEmpty())
                    continue;

                const auto y1 = (float) spectrum.getY() + Spectrogram::frequencyToY (band.getEnd(), rate, spectrum.getHeight());
                const auto y2 = (float) spectrum.getY() + Spectrogram::frequencyToY (band.getStart(), rate, spectrum.getHeight());
                const juce::Rectangle<float> box (x1, y1, w, juce::jmax (1.0f, y2 - y1));
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRect (box);
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawRect (box, 1.0f);
            }
        }
    }

    drawFades (g, area);

    g.setColour (cursorColour);
    g.drawVerticalLine ((int) frameToX (cursor), (float) area.getY(), (float) area.getBottom());

    if (engine.getPlayer().isPlaying())
    {
        g.setColour (juce::Colours::white);
        g.drawVerticalLine ((int) frameToX (engine.getPlayer().getPositionFrames()), 0.0f, (float) area.getBottom());
    }
}

//==============================================================================
juce::Rectangle<float> WaveformView::getFadeHandle (bool out) const
{
    const auto area = getWaveArea().toFloat();
    const auto length = engine.document.getNumFrames();
    const auto frames = shownFade (out).frames;
    constexpr auto size = 14.0f;
    const auto x = (float) frameToX (out ? length - frames : frames);
    const auto centre = juce::jlimit (area.getX() + size * 0.5f + 1.0f, area.getRight() - size * 0.5f - 1.0f, x);
    return { centre - size * 0.5f, area.getY() + 3.0f, size, size };
}

int WaveformView::fadeHandleAt (juce::Point<float> p) const
{
    if (! engine.document.hasAudio())
        return -1;

    // The fade out's handle wins where the two meet, since a fade in can then only shrink.
    for (auto out : { true, false })
        if (getFadeHandle (out).expanded (4.0f).contains (p))
            return out ? 1 : 0;

    return -1;
}

void WaveformView::dragFade (const juce::MouseEvent& e)
{
    const auto out = fadeDragged == 1;
    const auto length = engine.document.getNumFrames();
    const auto frame = juce::jlimit ((juce::int64) 0, length, xToFrame (e.position.x));

    // Sideways sets the length; up bends the curve to rise sooner (120 pixels from straight to the end).
    pendingFade.frames = juce::jmin (out ? length - frame : frame, fades.maxFrames (engine.document, out));
    pendingFade.curve = juce::jlimit (-1.0f, 1.0f, fadeAtDragStart.curve - (e.position.y - e.mouseDownPosition.y) / 120.0f);
    fadePending = true;

    // Put in at most 30 times a second; the timer puts in the last one.
    if (juce::Time::getMillisecondCounterHiRes() - lastFadeApplied >= 33.0)
        applyPendingFade();
}

void WaveformView::applyPendingFade()
{
    if (! fadePending || fadeDragged < 0)
        return;

    fadePending = false;
    lastFadeApplied = juce::Time::getMillisecondCounterHiRes();

    if (pendingFade != getFade (fadeDragged == 1))
        fades.set (engine.document, fadeDragged == 1, pendingFade);

    repaint();
}

void WaveformView::drawFades (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto channels = engine.document.getNumChannels();
    const auto length = engine.document.getNumFrames();
    const auto rate = engine.document.getSampleRate();

    for (auto out : { false, true })
    {
        const auto fade = shownFade (out);
        const auto handle = getFadeHandle (out);
        const auto dragging = fadeDragged == (out ? 1 : 0);

        if (fade.frames > 0)
        {
            const auto startX = (float) frameToX (out ? length - fade.frames : 0);
            const auto endX = (float) frameToX (out ? length : fade.frames);

            // The gain curve over each channel's waveform, and a shade over what it takes away.
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto lane = getLane (ch, channels).wave.translated (area.getX(), area.getY()).toFloat();
                const auto from = juce::jmax (startX, lane.getX()), to = juce::jmin (endX, lane.getRight());

                if (lane.isEmpty() || to <= from)
                    continue;

                juce::Path curve;

                for (auto x = from; ; x += 2.0f)
                {
                    const auto at = juce::jmin (x, to);
                    const auto t = (double) (at - startX) / (double) juce::jmax (1.0f, endX - startX);
                    const auto level = FileFades::level (out ? 1.0 - t : t, fade.curve);
                    const juce::Point<float> p (at, lane.getBottom() - level * lane.getHeight());

                    if (at <= from)
                        curve.startNewSubPath (p);
                    else
                        curve.lineTo (p);

                    if (at >= to)
                        break;
                }

                auto shade = curve;
                shade.lineTo (to, lane.getY());
                shade.lineTo (from, lane.getY());
                shade.closeSubPath();
                g.setColour (Theme::base.withAlpha (0.45f));
                g.fillPath (shade);
                g.setColour (Theme::accent);
                g.strokePath (curve, juce::PathStrokeType (1.5f));
            }

            g.setColour (Theme::accent.withAlpha (0.5f));
            g.drawVerticalLine (juce::roundToInt (out ? startX : endX), (float) area.getY(), (float) area.getBottom());
        }

        // The handle: an accent square holding a ramp, rising for the fade in and falling for the
        // fade out, so the two mirror each other. A white edge while it has a fade or is dragged.
        const auto active = fade.frames > 0 || dragging;
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (handle, 3.0f);
        const auto inner = handle.reduced (3.0f);
        juce::Path ramp;
        ramp.addTriangle (out ? inner.getTopLeft() : inner.getTopRight(), inner.getBottomLeft(), inner.getBottomRight());
        g.setColour (Theme::textStrong.withAlpha (active ? 1.0f : 0.85f));
        g.fillPath (ramp);
        g.setColour (active ? Theme::textStrong : Theme::accent.brighter (0.4f));
        g.drawRoundedRectangle (handle.reduced (0.5f), 3.0f, 1.0f);

        if (dragging && rate > 0.0)
        {
            const auto text = juce::String (out ? "Fade out " : "Fade in ") + juce::String ((double) fade.frames / rate, 3) + " s, curve "
                              + (fade.curve > 0.005f ? "+" : "") + juce::String (juce::roundToInt (fade.curve * 100.0f)) + "%";
            g.setFont (juce::FontOptions (Theme::bodyText));
            const auto w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 16.0f;
            const auto box = juce::Rectangle<float> (out ? handle.getX() - w - 6.0f : handle.getRight() + 6.0f, handle.getY() - 2.0f, w, 18.0f)
                                 .constrainedWithin (area.toFloat());
            g.setColour (Theme::raised.withAlpha (0.92f));
            g.fillRoundedRectangle (box, Theme::radius);
            g.setColour (Theme::text);
            g.drawText (text, box, juce::Justification::centred);
        }
    }
}

void WaveformView::resized()
{
    scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollBarHeight));
    setView (viewStart, framesPerPixel);
}

void WaveformView::drawEmpty (juce::Graphics& g, juce::Rectangle<int> area)
{
    // The hint, and under it the recent files to open with a click, their folders dimmed beside them.
    std::vector<std::pair<juce::Rectangle<int>, juce::File>> links;
    juce::Array<juce::File> recent;

    if (getRecentFiles != nullptr)
        for (auto& f : getRecentFiles())
            if (recent.size() < 8)
                recent.add (f);

    const auto lineHeight = 24;
    const auto listHeight = recent.isEmpty() ? 0 : 34 + recent.size() * lineHeight;
    auto block = area.withSizeKeepingCentre (juce::jmin (area.getWidth() - 40, 560), 30 + listHeight);
    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (16.0f));
    g.drawText ("Drop an audio file here, or use File > Open", block.removeFromTop (30), juce::Justification::centred);

    if (! recent.isEmpty())
    {
        block.removeFromTop (14);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (Theme::dimText);
        g.drawText ("RECENT", block.removeFromTop (20), juce::Justification::centredLeft);

        for (int i = 0; i < recent.size(); ++i)
        {
            auto line = block.removeFromTop (lineHeight);
            const auto hovered = i == hoveredRecent;

            if (hovered)
            {
                g.setColour (Theme::line);
                g.fillRoundedRectangle (line.toFloat(), 4.0f);
            }

            auto text = line.reduced (8, 0);
            const auto name = recent[i].getFileName();
            g.setFont (juce::FontOptions (14.0f));
            const auto nameWidth = juce::jmin (text.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 16);
            g.setColour (hovered ? Theme::accentLight : Theme::text);
            g.drawText (name, text.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
            g.setFont (juce::FontOptions (12.0f));
            g.setColour (Theme::faintText);
            g.drawText (recent[i].getParentDirectory().getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~"),
                        text, juce::Justification::centredLeft, true);
            links.emplace_back (line, recent[i]);
        }
    }

    recentLinks = std::move (links);
}

//==============================================================================
void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (! engine.document.hasAudio())
    {
        for (auto& [area, file] : recentLinks)
            if (area.contains (e.getPosition()) && onRecentFileClicked != nullptr)
            {
                const auto chosen = file;   // the click may open it, which repaints the list
                onRecentFileClicked (chosen);
                return;
            }

        return;
    }

    if (const auto ch = dividerAt (e.position); ch >= 0)
    {
        dividerChannel = ch;
        return;
    }

    if (const auto handle = fadeHandleAt (e.position); handle >= 0 && ! e.mods.isPopupMenu())
    {
        fadeDragged = handle;
        fadeAtDragStart = getFade (handle == 1);
        return;
    }

    if (const auto index = markerAt (e.position); index >= 0)
    {
        const auto& m = engine.document.getMarkers()[(size_t) index];

        if (e.mods.isPopupMenu())
        {
            showMarkerMenu (index);
            return;
        }

        band = {};

        if (m.isRegion())
            setSelection ({ m.start, m.getEnd() });
        else
            setCursor (m.start);

        draggedMarker = index;
        dragMarkers = engine.document.getMarkers();
        dragGrabOffset = xToFrame (e.position.x) - m.start;
        markerWasMoved = false;
        return;
    }

    if (getMarkerLane().toFloat().contains (e.position))
        return;

    // Option-drag in a spectrogram selects a box of time and frequency.
    if (e.mods.isAltDown())
    {
        const auto channels = engine.document.getNumChannels();
        const auto area = getWaveArea();

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto spectrum = getLane (ch, channels).spectrum.translated (area.getX(), area.getY());

            if (spectrum.contains (e.position.toInt()))
            {
                bandLane = ch;
                bandAnchor = Spectrogram::yToFrequency (e.position.y - (float) spectrum.getY(), engine.document.getSampleRate(), spectrum.getHeight());
                dragAnchor = xToFrame (e.position.x);
                band = {};
                setCursor (dragAnchor);
                return;
            }
        }
    }

    bandLane = -1;
    band = {};

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

    if (fadeDragged >= 0)
    {
        dragFade (e);
        return;
    }

    if (dividerChannel >= 0)
    {
        const auto area = getWaveArea();
        const auto laneHeight = area.getHeight() / juce::jmax (1, engine.document.getNumChannels());
        const auto top = area.getY() + laneHeight * dividerChannel;
        setWaveformShare ((e.position.y - (float) top) / (float) juce::jmax (1, laneHeight));
        return;
    }

    if (draggedMarker >= 0)
    {
        if (! markerWasMoved && e.getDistanceFromDragStart() < 3)
            return;

        auto& m = dragMarkers[(size_t) draggedMarker];
        const auto length = engine.document.getNumFrames();
        m.start = juce::jlimit ((juce::int64) 0, length - m.length, xToFrame (e.position.x) - dragGrabOffset);
        markerWasMoved = true;

        if (m.isRegion())
            setSelection ({ m.start, m.getEnd() });
        else
            setCursor (m.start);

        return;
    }

    if (e.mouseDownPosition.y < (float) getWaveArea().getY())
        return;

    // Auto-scroll when dragging past the edges.
    if (e.position.x < 0.0f)
        setView (viewStart + e.position.x * framesPerPixel * 0.1, framesPerPixel);
    else if (e.position.x > (float) getWidth())
        setView (viewStart + (e.position.x - (float) getWidth()) * framesPerPixel * 0.1, framesPerPixel);

    const auto frame = xToFrame (e.position.x);

    if (bandLane >= 0)
    {
        const auto area = getWaveArea();
        const auto spectrum = getLane (bandLane, engine.document.getNumChannels()).spectrum.translated (area.getX(), area.getY());
        const auto y = juce::jlimit ((float) spectrum.getY(), (float) spectrum.getBottom(), e.position.y);
        const auto frequency = Spectrogram::yToFrequency (y - (float) spectrum.getY(), engine.document.getSampleRate(), spectrum.getHeight());
        band = juce::Range<double>::between (bandAnchor, frequency);
    }

    setSelection (juce::Range<juce::int64>::between (dragAnchor, frame));
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (fadeDragged >= 0)
    {
        applyPendingFade();
        fadeDragged = -1;
        repaint();
        return;
    }

    if (dividerChannel >= 0)
    {
        dividerChannel = -1;

        if (onWaveformShareChanged != nullptr)
            onWaveformShareChanged (waveShare);

        return;
    }

    if (draggedMarker < 0)
        return;

    const auto moved = std::exchange (markerWasMoved, false);
    const auto markers = std::move (dragMarkers);
    const auto name = markers[(size_t) draggedMarker].name;
    draggedMarker = -1;

    if (moved)
        engine.document.setMarkers (markers, "Move " + name);

    repaint();
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (! engine.document.hasAudio())
    {
        auto over = -1;

        for (size_t i = 0; i < recentLinks.size(); ++i)
            if (recentLinks[i].first.contains (e.getPosition()))
                over = (int) i;

        if (over != hoveredRecent)
        {
            hoveredRecent = over;
            repaint();
        }

        setMouseCursor (over >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        return;
    }

    setMouseCursor (fadeHandleAt (e.position) >= 0 ? juce::MouseCursor::UpDownLeftRightResizeCursor
                    : dividerAt (e.position) >= 0 ? juce::MouseCursor::UpDownResizeCursor
                    : markerAt (e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor
                                                 : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto handle = fadeHandleAt (e.position); handle >= 0)
    {
        fades.set (engine.document, handle == 1, {});
        repaint();
    }
    else if (const auto index = markerAt (e.position); index >= 0)
        renameMarker (index);
    else if (engine.document.hasAudio() && e.position.y >= (float) getWaveArea().getY() && dividerAt (e.position) < 0)
        selectAll();
}

void WaveformView::renameMarker (int index)
{
    const auto& markers = engine.document.getMarkers();

    if (! juce::isPositiveAndBelow (index, (int) markers.size()))
        return;

    const auto old = markers[(size_t) index];
    auto* box = new juce::AlertWindow ("Rename", {}, juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("name", old.name);
    box->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, old] (int result)
    {
        const auto name = box->getTextEditorContents ("name").trim();

        if (result != 1 || name.isEmpty() || name == old.name)
            return;

        // Find it again by value: the markers may have changed while the box was open.
        auto current = engine.document.getMarkers();

        if (auto it = std::find (current.begin(), current.end(), old); it != current.end())
        {
            it->name = name;
            engine.document.setMarkers (std::move (current), "Rename " + old.name);
        }
    }), true);
}

void WaveformView::showMarkerMenu (int index)
{
    const auto old = engine.document.getMarkers()[(size_t) index];
    juce::PopupMenu menu;
    menu.addItem ("Rename...", [this, index] { renameMarker (index); });
    menu.addItem ("Delete " + old.name, [this, old]
    {
        auto markers = engine.document.getMarkers();

        if (auto it = std::find (markers.begin(), markers.end(), old); it != markers.end())
        {
            markers.erase (it);
            engine.document.setMarkers (std::move (markers), "Delete " + old.name);
        }
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
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
    juce::Array<juce::File> audio;

    for (auto& path : files)
        if (engine.formatManager.findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            audio.add (juce::File (path));

    if (onFilesDropped != nullptr && ! audio.isEmpty())
        onFilesDropped (audio);
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
        invalidateWaveform();
    }

    // Markers (or the saved state) changed; the waveform image is still good.
    repaint();
}

void WaveformView::timerCallback()
{
    engine.document.releaseUnusedSnapshots();

    if (fadeDragged >= 0 && fadePending)
        applyPendingFade();

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
