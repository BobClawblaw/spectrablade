#include "DynamicEq.h"
#include "Dynamics.h"
#include "Theme.h"

namespace DynamicEq
{
namespace
{
    constexpr double minHz = EqDrawing::minHz, maxHz = EqDrawing::maxHz;
    constexpr double floorLevelDb = -80.0;   // the meter's bottom, and the threshold's

    bool moves (const Band& b)                { return b.on && ! juce::exactlyEqual (b.gainDb, 0.0); }

    // The time the level is averaged over: three cycles of a bell's or a high shelf's frequency (from
    // 5 to 50 ms), 50 ms for a low shelf, whose part reaches down to the lowest notes.
    double detectorMs (const Band& b)
    {
        return b.shape == Shape::lowShelf ? 50.0 : juce::jlimit (5.0, 50.0, 3000.0 / b.frequency);
    }

    juce::String hertz (double hz)            { return hz >= 1000.0 ? juce::String (hz / 1000.0, 2) + " kHz" : juce::String (hz, 0) + " Hz"; }
    juce::String signedDb (double db)         { return (db > 0.0 ? "+" : "") + juce::String (db, 1) + " dB"; }

    const juce::StringArray shapeNames { "Bell", "Low Shelf", "High Shelf" };
    const juce::StringArray modeNames { "Over the threshold", "Under the threshold" };
}

Settings defaultSettings()
{
    Settings s;
    const std::array<std::pair<Shape, double>, numBands> starts {{ { Shape::lowShelf, 100.0 }, { Shape::bell, 400.0 }, { Shape::bell, 1000.0 },
                                                                    { Shape::bell, 3000.0 }, { Shape::highShelf, 8000.0 } }};

    for (size_t i = 0; i < starts.size(); ++i)
    {
        s.bands[i].shape = starts[i].first;
        s.bands[i].frequency = starts[i].second;
    }

    return s;
}

bool isValid (const Settings& s, double sampleRate)
{
    if (sampleRate <= 0.0 || std::abs (s.outputDb) > rangeDb)
        return false;

    for (const auto& b : s.bands)
        if (b.frequency < minHz || b.frequency > maxHz || b.frequency > 0.45 * sampleRate || b.q < 0.1 || b.q > 20.0
            || std::abs (b.gainDb) > rangeDb || b.thresholdDb < floorLevelDb || b.thresholdDb > 0.0 || b.ratio < 1.0 || b.ratio > 20.0
            || b.attackMs < 0.1 || b.attackMs > 500.0 || b.releaseMs < 5.0 || b.releaseMs > 5000.0)
            return false;

    return true;
}

Settings withinRate (Settings s, double sampleRate)
{
    for (auto& b : s.bands)
        b.frequency = juce::jmin (b.frequency, 0.45 * sampleRate);

    return s;
}

double staticGainDb (const Band& b, double levelDb)
{
    const auto past = b.mode == Mode::over ? levelDb - b.thresholdDb : b.thresholdDb - levelDb;

    if (past <= 0.0 || ! moves (b))
        return 0.0;

    const auto amount = juce::jmin (std::abs (b.gainDb), past * (1.0 - 1.0 / b.ratio));
    return b.gainDb < 0.0 ? -amount : amount;
}

namespace
{
    struct Biquad { double b0, b1, b2, a1, a2; };

    Biquad partOf (const Band& b, double sampleRate)
    {
        const auto w = juce::MathConstants<double>::twoPi * b.frequency / sampleRate;

        if (b.shape == Shape::bell)
        {
            // A band-pass with unity at its centre (the bilinear transform, prewarped): what an
            // all-pass of the same width takes away, halved.
            const auto alpha = std::sin (w) / (2.0 * b.q), a0 = 1.0 + alpha;
            return { alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos (w) / a0, (1.0 - alpha) / a0 };
        }

        // First-order low- or high-pass at the corner: half of one plus or minus a first-order all-pass,
        // so the sound less its low-pass is exactly its high-pass.
        const auto t = std::tan (w / 2.0), a = (t - 1.0) / (t + 1.0);
        return b.shape == Shape::lowShelf ? Biquad { (1.0 + a) / 2.0, (1.0 + a) / 2.0, 0.0, a, 0.0 }
                                          : Biquad { (1.0 - a) / 2.0, -(1.0 - a) / 2.0, 0.0, a, 0.0 };
    }
}

double responseDb (const Band& b, double sampleRate, double frequency, double gainDb)
{
    const auto f = partOf (b, sampleRate);
    const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    const auto part = (f.b0 + f.b1 * z1 + f.b2 * z2) / (1.0 + f.a1 * z1 + f.a2 * z2);
    const auto factor = std::pow (10.0, gainDb / 20.0);
    return 20.0 * std::log10 (std::max (1.0e-12, std::abs (1.0 + (factor - 1.0) * part)));
}

//==============================================================================
void Core::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    states.assign ((size_t) juce::jmax (1, numChannels), {});
    parts.assign (states.size(), 0.0);
    set (settings);
}

void Core::reset() noexcept
{
    for (auto& channel : states)
        channel.fill ({});

    meanSquare.fill (0.0);
    gainDb.fill (0.0);
}

void Core::set (const Settings& s)
{
    settings = s;
    output = std::pow (10.0, s.outputDb / 20.0);

    for (size_t i = 0; i < (size_t) numBands; ++i)
    {
        const auto& b = s.bands[i];
        const auto now = moves (b);

        // A band starting to move starts afresh; one that stops lets its gain go.
        if (now && ! active[i])
        {
            for (auto& channel : states)
                channel[i] = {};

            meanSquare[i] = 0.0;
        }

        if (! now)
            gainDb[i] = 0.0;

        active[i] = now;
        const auto f = partOf (b, rate);
        filters[i] = { f.b0, f.b1, f.b2, f.a1, f.a2 };
        attack[i] = Dynamics::smoothingCoefficient (b.attackMs, rate);
        release[i] = Dynamics::smoothingCoefficient (b.releaseMs, rate);
        detector[i] = Dynamics::smoothingCoefficient (detectorMs (b), rate);
    }
}

std::array<double, numBands> Core::getLevelDb() const noexcept
{
    std::array<double, numBands> levels {};

    for (size_t i = 0; i < levels.size(); ++i)
        levels[i] = 10.0 * std::log10 (std::max (1.0e-15, 2.0 * meanSquare[i]));

    return levels;
}

void Core::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) states.size());
    const auto unity = juce::exactlyEqual (output, 1.0);

    for (int frame = 0; frame < numFrames; ++frame)
    {
        for (size_t i = 0; i < (size_t) numBands; ++i)
        {
            if (! active[i])
                continue;

            const auto& f = filters[i];
            auto loudest = 0.0;

            for (int ch = 0; ch < n; ++ch)
            {
                auto& st = states[(size_t) ch][i];
                const double x = channels[ch][frame];
                const auto y = f.b0 * x + st.z1;
                st.z1 = f.b1 * x - f.a1 * y + st.z2;
                st.z2 = f.b2 * x - f.a2 * y;
                parts[(size_t) ch] = y;
                loudest = juce::jmax (loudest, y * y);
            }

            meanSquare[i] = loudest + detector[i] * (meanSquare[i] - loudest);
            const auto levelDb = 10.0 * std::log10 (std::max (1.0e-15, 2.0 * meanSquare[i]));
            const auto target = staticGainDb (settings.bands[i], levelDb);
            const auto coefficient = std::abs (target) > std::abs (gainDb[i]) ? attack[i] : release[i];
            gainDb[i] = target + coefficient * (gainDb[i] - target);

            if (juce::exactlyEqual (gainDb[i], 0.0))
                continue;

            const auto change = std::pow (10.0, gainDb[i] / 20.0) - 1.0;

            for (int ch = 0; ch < n; ++ch)
                channels[ch][frame] = (float) ((double) channels[ch][frame] + change * parts[(size_t) ch]);
        }

        if (! unity)
            for (int ch = 0; ch < n; ++ch)
                channels[ch][frame] = (float) ((double) channels[ch][frame] * output);
    }
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    if (! isValid (settings, sampleRate))
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 2)
        return nullptr;

    Core core;
    core.prepare (s.getNumChannels(), sampleRate);
    core.set (settings);
    const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));

    return AudioEdits::processRange (s, start - prime, end, [&core, prime] (juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> before (b.getNumChannels(), prime);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            before.copyFrom (ch, 0, b, ch, 0, prime);

        core.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, before, ch, 0, prime);
    });
}

//==============================================================================
namespace
{
    constexpr int tabRowHeight = 30, sliderRowHeight = 28, meterWidth = 34;
}

Panel::Panel (const Settings& initial, double rate)
    : settings (withinRate (initial, rate)), sampleRate (rate)
{
    auto& c = controls;
    const auto topHz = juce::jmin (maxHz, 0.45 * sampleRate);

    for (int i = 0; i < numBands; ++i)
    {
        auto& tab = c.tabs[(size_t) i];
        tab.setButtonText (juce::String (i + 1));
        tab.setClickingTogglesState (true);
        tab.setRadioGroupId (1);
        tab.setColour (juce::TextButton::buttonOnColourId, EqDrawing::bandColour (i).withAlpha (0.55f));
        tab.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < numBands - 1 ? juce::Button::ConnectedOnRight : 0));
        tab.onClick = [this, i] { if (controls.tabs[(size_t) i].getToggleState()) selectBand (i); };
        addAndMakeVisible (tab);
    }

    c.shape.addItemList (shapeNames, 1);
    c.mode.addItemList (modeNames, 1);

    const auto setUp = [this] (juce::Slider& s, const juce::String& name, double low, double high, double interval, const juce::String& suffix, double mid)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
        s.setRange (low, high, interval);

        if (mid > 0.0)
            s.setSkewFactorFromMidPoint (mid);

        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] { readControls(); };
        addAndMakeVisible (s);
        auto* label = labels.add (new juce::Label ({}, name));
        label->setJustificationType (juce::Justification::centredRight);
        label->attachToComponent (&s, true);
    };

    setUp (c.frequency, "Frequency", minHz, topHz, 0.1, " Hz", std::sqrt (minHz * topHz));
    setUp (c.q, "Q", 0.1, 20.0, 0.01, {}, 1.0);
    setUp (c.gain, "Gain", -rangeDb, rangeDb, 0.1, " dB", 0.0);
    setUp (c.threshold, "Threshold", floorLevelDb, 0.0, 0.1, " dB", 0.0);
    setUp (c.ratio, "Ratio", 1.0, 20.0, 0.1, ":1", 4.0);
    setUp (c.attack, "Attack", 0.1, 500.0, 0.1, " ms", 10.0);
    setUp (c.release, "Release", 5.0, 5000.0, 1.0, " ms", 150.0);
    setUp (c.output, "Output", -rangeDb, rangeDb, 0.1, " dB", 0.0);
    c.output.setValue (settings.outputDb, juce::dontSendNotification);

    c.on.onClick = [this] { readControls(); };
    c.shape.onChange = [this] { readControls(); };
    c.mode.onChange = [this] { readControls(); };

    for (juce::Component* comp : { (juce::Component*) &c.on, (juce::Component*) &c.shape, (juce::Component*) &c.mode })
        addAndMakeVisible (comp);

    showSelected();
    setSize (760, 300 + tabRowHeight + 4 * sliderRowHeight + 24);
    startTimerHz (25);
}

Panel::~Panel()
{
    stopTimer();
}

void Panel::setSettings (const Settings& s)
{
    settings = withinRate (s, sampleRate);
    controls.output.setValue (settings.outputDb, juce::dontSendNotification);
    showSelected();
    repaint();
}

void Panel::setReadings (const Readings& r)
{
    readings = r;
    repaint();
}

void Panel::selectBand (int band)
{
    selected = juce::jlimit (0, numBands - 1, band);
    showSelected();
    repaint();
}

void Panel::showSelected()
{
    showing = true;
    const auto& b = settings.bands[(size_t) selected];
    auto& c = controls;
    c.tabs[(size_t) selected].setToggleState (true, juce::dontSendNotification);
    c.on.setToggleState (b.on, juce::dontSendNotification);
    c.on.setColour (juce::ToggleButton::tickColourId, EqDrawing::bandColour (selected));
    c.shape.setSelectedItemIndex ((int) b.shape, juce::dontSendNotification);
    c.mode.setSelectedItemIndex ((int) b.mode, juce::dontSendNotification);
    c.frequency.setValue (b.frequency, juce::dontSendNotification);
    c.q.setValue (b.q, juce::dontSendNotification);
    c.q.setEnabled (b.shape == Shape::bell);
    c.gain.setValue (b.gainDb, juce::dontSendNotification);
    c.threshold.setValue (b.thresholdDb, juce::dontSendNotification);
    c.ratio.setValue (b.ratio, juce::dontSendNotification);
    c.attack.setValue (b.attackMs, juce::dontSendNotification);
    c.release.setValue (b.releaseMs, juce::dontSendNotification);
    showing = false;
}

void Panel::readControls()
{
    if (showing)
        return;

    auto& b = settings.bands[(size_t) selected];
    const auto& c = controls;
    b.on = c.on.getToggleState();
    b.shape = (Shape) juce::jmax (0, c.shape.getSelectedItemIndex());
    b.mode = (Mode) juce::jmax (0, c.mode.getSelectedItemIndex());
    b.frequency = c.frequency.getValue();
    b.q = c.q.getValue();
    b.gainDb = c.gain.getValue();
    b.thresholdDb = c.threshold.getValue();
    b.ratio = c.ratio.getValue();
    b.attackMs = c.attack.getValue();
    b.releaseMs = c.release.getValue();
    settings.outputDb = c.output.getValue();
    controls.q.setEnabled (b.shape == Shape::bell);
    changed();
}

void Panel::changed()
{
    repaint();

    if (onChange != nullptr)
        onChange();
}

void Panel::timerCallback()
{
    if (! isShowing())
        return;

    auto dirty = spectrum.read (readOutput, recent);

    if (readLive != nullptr)
    {
        readings = readLive();
        dirty = true;
    }

    if (dirty)
        repaint();
}

//==============================================================================
juce::Rectangle<float> Panel::getCurveArea() const
{
    return getLocalBounds().removeFromTop (300).reduced (8).withTrimmedRight (meterWidth + 6).toFloat();
}

juce::Rectangle<float> Panel::getMeterArea() const
{
    const auto curve = getCurveArea();
    return { curve.getRight() + 6.0f, curve.getY(), (float) meterWidth, curve.getHeight() };
}

float Panel::xForFrequency (double hz) const      { return EqDrawing::xForFrequency (getCurveArea(), hz); }
double Panel::frequencyForX (float x) const       { return EqDrawing::frequencyForX (getCurveArea(), x); }

float Panel::yForDb (double db) const
{
    const auto area = getCurveArea();
    return area.getCentreY() - (float) (juce::jlimit (-rangeDb, rangeDb, db) / rangeDb) * area.getHeight() / 2.0f;
}

double Panel::dbForY (float y) const
{
    const auto area = getCurveArea();
    return (double) ((area.getCentreY() - y) / (area.getHeight() / 2.0f)) * rangeDb;
}

float Panel::yForLevel (double levelDb) const
{
    const auto area = getMeterArea().reduced (0.0f, 4.0f);
    return area.getBottom() - area.getHeight() * (float) (juce::jlimit (floorLevelDb, 0.0, levelDb) / floorLevelDb * -1.0 + 1.0);
}

double Panel::levelForY (float y) const
{
    const auto area = getMeterArea().reduced (0.0f, 4.0f);
    return juce::jlimit (floorLevelDb, 0.0, (double) ((area.getBottom() - y) / area.getHeight()) * -floorLevelDb + floorLevelDb);
}

juce::Point<float> Panel::pointFor (int band) const
{
    const auto& b = settings.bands[(size_t) band];
    return { xForFrequency (b.frequency), yForDb (b.gainDb) };
}

int Panel::bandAt (juce::Point<float> p) const
{
    auto nearest = -1;
    auto best = grabRadius;

    for (int i = 0; i < numBands; ++i)
        if (settings.bands[(size_t) i].on)
            if (const auto d = pointFor (i).getDistanceFrom (p); d <= best)
                best = d, nearest = i;

    return nearest;
}

int Panel::offBandAt (juce::Point<float> p) const
{
    auto nearest = -1;
    auto best = grabRadius;

    for (int i = 0; i < numBands; ++i)
        if (! settings.bands[(size_t) i].on)
            if (const auto d = juce::Point<float> (xForFrequency (settings.bands[(size_t) i].frequency), yForDb (0.0)).getDistanceFrom (p); d <= best)
                best = d, nearest = i;

    return nearest;
}

//==============================================================================
void Panel::mouseMove (const juce::MouseEvent& e)
{
    const auto on = bandAt (e.position);
    const auto now = on >= 0 ? on : offBandAt (e.position);
    const auto overThreshold = getMeterArea().contains (e.position) && std::abs (e.position.y - yForLevel (settings.bands[(size_t) selected].thresholdDb)) < 6.0f;
    setMouseCursor (on >= 0 ? juce::MouseCursor::DraggingHandCursor : overThreshold ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void Panel::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
    }
}

void Panel::mouseDown (const juce::MouseEvent& e)
{
    draggingThreshold = getMeterArea().contains (e.position);

    if (draggingThreshold)
    {
        mouseDrag (e);
        return;
    }

    dragging = bandAt (e.position);

    if (dragging >= 0 && dragging != selected)
        selectBand (dragging);
}

void Panel::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingThreshold)
    {
        settings.bands[(size_t) selected].thresholdDb = std::round (levelForY (e.position.y) * 10.0) / 10.0;
        showSelected();
        changed();
        return;
    }

    if (dragging < 0)
        return;

    auto& b = settings.bands[(size_t) dragging];
    b.frequency = juce::jlimit (controls.frequency.getMinimum(), controls.frequency.getMaximum(), std::round (frequencyForX (e.position.x) * 10.0) / 10.0);
    b.gainDb = juce::jlimit (-rangeDb, rangeDb, std::round (dbForY (e.position.y) * 10.0) / 10.0);
    showSelected();
    changed();
}

void Panel::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
    draggingThreshold = false;
}

void Panel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! getCurveArea().contains (e.position))
        return;

    auto band = bandAt (e.position);

    if (band >= 0)
    {
        settings.bands[(size_t) band].on = false;
    }
    else if ((band = offBandAt (e.position)) >= 0)
    {
        settings.bands[(size_t) band].on = true;
    }
    else
    {
        // The first band switched off, put here; none when all are on.
        for (int i = 0; i < numBands && band < 0; ++i)
            if (! settings.bands[(size_t) i].on)
                band = i;

        if (band < 0)
            return;

        auto& b = settings.bands[(size_t) band];
        b.on = true;
        b.frequency = juce::jlimit (controls.frequency.getMinimum(), controls.frequency.getMaximum(), std::round (frequencyForX (e.position.x) * 10.0) / 10.0);
        b.gainDb = juce::jlimit (-rangeDb, rangeDb, std::round (dbForY (e.position.y) * 10.0) / 10.0);
    }

    selected = band;
    showSelected();
    changed();
}

void Panel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto band = bandAt (e.position);

    if (band < 0 || juce::exactlyEqual (wheel.deltaY, 0.0f) || settings.bands[(size_t) band].shape != Shape::bell)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    // Up narrows the bell (a higher Q), down widens it, by a tenth a step.
    auto& q = settings.bands[(size_t) band].q;
    q = juce::jlimit (0.1, 20.0, std::round (q * (wheel.deltaY > 0.0f ? 1.1 : 1.0 / 1.1) * 100.0) / 100.0);

    if (band == selected)
        showSelected();

    changed();
}

//==============================================================================
juce::Path Panel::responsePath (int band, bool now) const
{
    const auto area = getCurveArea();
    const auto nyquist = sampleRate / 2.0;
    juce::Path path;
    auto started = false;

    for (auto x = area.getX(); x <= area.getRight(); x += 1.0f)
    {
        const auto hz = frequencyForX (x);

        if (hz >= nyquist)
            break;

        auto db = 0.0;

        for (int i = 0; i < numBands; ++i)
        {
            const auto& b = settings.bands[(size_t) i];

            if ((band < 0 || band == i) && b.on)
                db += responseDb (b, sampleRate, hz, now ? readings.gainDb[(size_t) i] : b.gainDb);
        }

        const auto y = yForDb (db);
        started ? path.lineTo (x, y) : path.startNewSubPath (x, y);
        started = true;
    }

    return path;
}

juce::String Panel::describe (int band) const
{
    const auto& b = settings.bands[(size_t) band];

    if (! b.on)
        return "Off: double-click to switch on";

    auto text = shapeNames[(int) b.shape] + "  " + hertz (b.frequency);

    if (b.shape == Shape::bell)
        text << "  Q " << juce::String (b.q, 2);

    text << "  " << signedDb (b.gainDb) << (b.mode == Mode::over ? " over " : " under ") << juce::String (b.thresholdDb, 1) << " dB";
    text << "  now " << signedDb (readings.gainDb[(size_t) band]);
    return text;
}

void Panel::paint (juce::Graphics& g)
{
    const auto area = getCurveArea();
    g.fillAll (Theme::base);
    EqDrawing::paintWell (g, area);
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        EqDrawing::paintSpectrum (g, area, spectrum);
    }

    EqDrawing::paintGrid (g, area, rangeDb);

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        const auto zero = yForDb (0.0);
        const float dashes[] { 4.0f, 3.0f };

        // Each band: the most it moves as a thin dashed line, what it is doing now shaded from 0 dB.
        for (int i = 0; i < numBands; ++i)
        {
            const auto& b = settings.bands[(size_t) i];

            if (! moves (b))
                continue;

            const auto colour = EqDrawing::bandColour (i);
            const auto strong = i == selected || i == hovered;
            juce::Path most;
            juce::PathStrokeType (1.0f).createDashedStroke (most, responsePath (i, false), dashes, 2);
            g.setColour (colour.withAlpha (strong ? 0.8f : 0.45f));
            g.fillPath (most);

            auto shade = responsePath (i, true);
            shade.lineTo (shade.getCurrentPosition().x, zero);
            shade.lineTo (area.getX(), zero);
            shade.closeSubPath();
            g.setColour (colour.withAlpha (strong ? 0.38f : 0.24f));
            g.fillPath (shade);
        }

        // All of them at most, faint, and now, glowing.
        juce::Path most;
        juce::PathStrokeType (1.2f).createDashedStroke (most, responsePath (-1, false), dashes, 2);
        g.setColour (Theme::overlay.withAlpha (0.35f));
        g.fillPath (most);
        EqDrawing::paintGlow (g, responsePath (-1, true));

        for (int i = 0; i < numBands; ++i)
        {
            const auto strong = i == selected || i == hovered;

            if (settings.bands[(size_t) i].on)
                EqDrawing::paintHandle (g, pointFor (i), i, strong, i == selected);
            else
                EqDrawing::paintOffRing (g, { xForFrequency (settings.bands[(size_t) i].frequency), zero }, i, strong);
        }

        const auto shown = dragging >= 0 ? dragging : hovered;

        // The readout along the top of the drawing, over the band, where it covers no curve.
        if (shown >= 0)
            EqDrawing::paintReadout (g, area, { xForFrequency (settings.bands[(size_t) shown].frequency), area.getY() + 45.0f }, describe (shown),
                                     EqDrawing::bandColour (shown));
    }

    // The picked band's level as it plays, its threshold across it, and how far it is moving now.
    const auto meter = getMeterArea();
    const auto colour = EqDrawing::bandColour (selected);
    const auto& b = settings.bands[(size_t) selected];
    g.setColour (Theme::canvas.darker (0.2f));
    g.fillRoundedRectangle (meter, 5.0f);

    const auto bar = meter.reduced (9.0f, 4.0f);
    const auto top = yForLevel (readings.levelDb[(size_t) selected]);
    const auto lit = bar.withTop (top);
    g.setGradientFill (juce::ColourGradient (colour, bar.getX(), bar.getY(), colour.withAlpha (0.25f), bar.getX(), bar.getBottom(), false));
    g.fillRoundedRectangle (lit, 2.0f);

    const auto ty = yForLevel (b.thresholdDb);
    const auto acting = b.mode == Mode::over ? meter.withBottom (ty) : meter.withTop (ty);
    g.setColour (colour.withAlpha (0.10f));
    g.fillRect (acting.reduced (2.0f, 0.0f));
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.fillRoundedRectangle (juce::Rectangle<float> (meter.getX() + 2.0f, ty - 1.5f, meter.getWidth() - 4.0f, 3.0f), 1.5f);

    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (juce::String (b.thresholdDb, 0), juce::Rectangle<float> (meter.getX(), ty + (ty > meter.getBottom() - 18.0f ? -15.0f : 3.0f), meter.getWidth(), 12.0f),
                juce::Justification::centred);
    g.drawText (juce::String (selected + 1), meter.withHeight (14.0f).translated (0.0f, 2.0f), juce::Justification::centred);

    // A strip of the picked band's colour beside its settings.
    const auto settingsArea = getLocalBounds().withTrimmedTop (300).reduced (8, 4).toFloat();
    g.setColour (colour.withAlpha (b.on ? 0.9f : 0.3f));
    g.fillRoundedRectangle (settingsArea.withWidth (3.0f), 1.5f);
}

void Panel::resized()
{
    auto r = getLocalBounds().withTrimmedTop (300).reduced (8, 4).withTrimmedLeft (10);
    auto& c = controls;
    auto top = r.removeFromTop (tabRowHeight).reduced (0, 3);

    for (auto& tab : c.tabs)
        tab.setBounds (top.removeFromLeft (32));

    top.removeFromLeft (12);
    c.on.setBounds (top.removeFromLeft (56));
    c.shape.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (8);
    c.mode.setBounds (top.removeFromLeft (180));

    // Two columns of sliders, each with its name to its left.
    r.removeFromTop (4);
    auto left = r.removeFromLeft (r.getWidth() / 2).withTrimmedLeft (70);
    auto right = r.withTrimmedLeft (80);

    for (auto* s : { &c.frequency, &c.q, &c.gain, &c.threshold })
        s->setBounds (left.removeFromTop (sliderRowHeight).reduced (0, 3));

    for (auto* s : { &c.ratio, &c.attack, &c.release, &c.output })
        s->setBounds (right.removeFromTop (sliderRowHeight).reduced (0, 3));
}
}
