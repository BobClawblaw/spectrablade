#include "ParametricEq.h"
#include "Theme.h"

namespace
{
    constexpr int rowHeight = 28, labelWidth = 60;
    constexpr const char* typeNames[] = { "Bell", "Low Shelf", "High Shelf", "High-Pass", "Low-Pass" };

    bool isPass (const Equalizer::Band& band)
    {
        return band.type == Equalizer::BandType::highPass || band.type == Equalizer::BandType::lowPass;
    }

}

ParametricEq::Settings ParametricEq::defaultSettings()
{
    using Equalizer::BandType;
    constexpr auto shelfQ = 0.7071067811865476;
    Settings s;
    s[0].band = { BandType::lowShelf, 100.0, 0.0, shelfQ };
    s[1].band = { BandType::peak, 400.0, 0.0, 1.0 };
    s[2].band = { BandType::peak, 1000.0, 0.0, 1.0 };
    s[3].band = { BandType::peak, 3000.0, 0.0, 1.0 };
    s[4].band = { BandType::highShelf, 8000.0, 0.0, shelfQ };
    return s;
}

ParametricEq::ParametricEq (const Settings& initial, double rate)
    : settings (initial), sampleRate (rate)
{
    // A band can go up to just under Nyquist at a low rate, and never past 20 kHz.
    const auto topHz = juce::jmin (maxHz, sampleRate * 0.45);

    for (int i = 0; i < numBands; ++i)
    {
        auto row = std::make_unique<Row>();
        auto& band = settings[(size_t) i].band;
        band.frequency = juce::jlimit (minHz, topHz, band.frequency);

        row->on.setButtonText (juce::String (i + 1));
        row->on.setColour (juce::ToggleButton::tickColourId, bandColour (i));
        row->on.setToggleState (settings[(size_t) i].on, juce::dontSendNotification);

        for (int t = 0; t < (int) std::size (typeNames); ++t)
            row->type.addItem (typeNames[t], t + 1);

        row->type.setSelectedItemIndex ((int) band.type, juce::dontSendNotification);

        row->frequency.setSliderStyle (juce::Slider::LinearHorizontal);
        row->frequency.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        row->frequency.setRange (minHz, topHz, 0.1);
        row->frequency.setSkewFactorFromMidPoint (std::sqrt (minHz * topHz));
        row->frequency.setTextValueSuffix (" Hz");
        row->frequency.setValue (band.frequency, juce::dontSendNotification);

        row->gain.setSliderStyle (juce::Slider::LinearHorizontal);
        row->gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
        row->gain.setRange (-rangeDb, rangeDb, 0.1);
        row->gain.setTextValueSuffix (" dB");
        row->gain.setValue (band.gainDb, juce::dontSendNotification);
        row->gain.setEnabled (! isPass (band));

        row->q.setSliderStyle (juce::Slider::LinearHorizontal);
        row->q.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        row->q.setRange (0.1, 20.0);  // no step, so a Q of 0.7071 stays as it is
        row->q.setNumDecimalPlacesToDisplay (2);
        row->q.setSkewFactorFromMidPoint (1.0);
        row->q.setValue (band.q, juce::dontSendNotification);

        row->on.onClick = [this, i] { readRow (i); };
        row->type.onChange = [this, i] { readRow (i); };
        row->frequency.onValueChange = [this, i] { readRow (i); };
        row->gain.onValueChange = [this, i] { readRow (i); };
        row->q.onValueChange = [this, i] { readRow (i); };

        for (juce::Component* c : { (juce::Component*) &row->on, (juce::Component*) &row->type, (juce::Component*) &row->frequency,
                                    (juce::Component*) &row->gain, (juce::Component*) &row->q })
            addAndMakeVisible (c);

        rows[(size_t) i] = std::move (row);
    }

    setSize (640, 220 + numBands * rowHeight);
    startTimerHz (25);
}

juce::Colour ParametricEq::bandColour (int index)
{
    // Five hues spread round the wheel, bright enough to read on a dark or a light drawing.
    static const juce::Colour colours[] { juce::Colour (0xffff7a59), juce::Colour (0xfff2c14e), juce::Colour (0xff5fd38d),
                                          juce::Colour (0xff4fb3f6), juce::Colour (0xffb58cf5) };
    return colours[(size_t) juce::jlimit (0, numBands - 1, index)];
}

void ParametricEq::takeSpectrum (const std::vector<float>& levels, double rate)
{
    // Falls back 0.6 dB a frame (15 dB a second at 25 frames), so peaks linger long enough to read.
    if (spectrum.size() != levels.size() || ! juce::exactlyEqual (rate, spectrumRate))
        spectrum.assign (levels.size(), -150.0f);

    spectrumRate = rate;

    for (size_t i = 0; i < levels.size(); ++i)
        spectrum[i] = juce::jmax (levels[i], spectrum[i] - 0.6f);
}

void ParametricEq::timerCallback()
{
    if (readOutput == nullptr || ! isShowing())
        return;

    const auto rate = readOutput (recent);

    if (rate <= 0.0)
        return;

    std::vector<float> mono ((size_t) Analysis::fftSize);

    for (int i = 0; i < Analysis::fftSize; ++i)
        mono[(size_t) i] = 0.5f * (recent.getSample (0, i) + recent.getSample (1, i));

    takeSpectrum (Analysis::spectrumDb (mono.data(), Analysis::fftSize), rate);
    repaint (getCurveArea().toNearestInt());
}

void ParametricEq::setSettings (const Settings& s)
{
    settings = s;

    for (int i = 0; i < numBands; ++i)
    {
        auto& row = *rows[(size_t) i];
        const auto& setting = settings[(size_t) i];
        row.on.setToggleState (setting.on, juce::dontSendNotification);
        row.type.setSelectedItemIndex ((int) setting.band.type, juce::dontSendNotification);
        row.frequency.setValue (setting.band.frequency, juce::dontSendNotification);
        row.gain.setValue (setting.band.gainDb, juce::dontSendNotification);
        row.gain.setEnabled (! isPass (setting.band));
        row.q.setValue (setting.band.q, juce::dontSendNotification);
    }

    repaint();
}

void ParametricEq::readRow (int index)
{
    auto& row = *rows[(size_t) index];
    auto& setting = settings[(size_t) index];
    setting.on = row.on.getToggleState();
    setting.band.type = (Equalizer::BandType) juce::jmax (0, row.type.getSelectedItemIndex());
    setting.band.frequency = row.frequency.getValue();
    // The slider steps from its bottom end, so 0 dB comes back as about 1e-15; the step is 0.1 dB.
    setting.band.gainDb = std::round (row.gain.getValue() * 10.0) / 10.0;
    setting.band.q = row.q.getValue();

    // A pass ignores the gain, so its slider is greyed out.
    row.gain.setEnabled (! isPass (setting.band));

    repaint();

    if (onChange)
        onChange();
}

std::vector<Equalizer::Band> ParametricEq::getActiveBands() const
{
    std::vector<Equalizer::Band> active;

    for (auto& s : settings)
        if (s.on && ! Equalizer::isFlat (s.band))
            active.push_back (s.band);

    return active;
}

double ParametricEq::responseDb (double frequency) const
{
    double total = 0.0;

    for (auto& band : getActiveBands())
        total += Equalizer::responseDb (band, sampleRate, frequency);

    return total;
}

juce::Rectangle<float> ParametricEq::getCurveArea() const
{
    return getLocalBounds().removeFromTop (getHeight() - numBands * rowHeight - 8).reduced (8).toFloat();
}

float ParametricEq::xForFrequency (double frequency) const
{
    const auto area = getCurveArea();
    return area.getX() + area.getWidth() * (float) (std::log (frequency / minHz) / std::log (maxHz / minHz));
}

double ParametricEq::frequencyForX (float x) const
{
    const auto area = getCurveArea();
    return minHz * std::pow (maxHz / minHz, (double) ((x - area.getX()) / area.getWidth()));
}

float ParametricEq::yForDb (double db) const
{
    const auto area = getCurveArea();
    return area.getCentreY() - (float) (juce::jlimit (-rangeDb, rangeDb, db) / rangeDb) * area.getHeight() / 2.0f;
}

double ParametricEq::dbForY (float y) const
{
    const auto area = getCurveArea();
    return (double) ((area.getCentreY() - y) / (area.getHeight() / 2.0f)) * rangeDb;
}

juce::Point<float> ParametricEq::pointFor (int index) const
{
    const auto frequency = settings[(size_t) index].band.frequency;
    return { xForFrequency (frequency), yForDb (responseDb (frequency)) };
}

int ParametricEq::bandAt (juce::Point<float> p) const
{
    auto nearest = -1;
    auto best = grabRadius;

    for (int i = 0; i < numBands; ++i)
    {
        if (! settings[(size_t) i].on)
            continue;

        const auto d = pointFor (i).getDistanceFrom (p);

        if (d <= best)
        {
            best = d;
            nearest = i;
        }
    }

    return nearest;
}

int ParametricEq::offBandAt (juce::Point<float> p) const
{
    auto nearest = -1;
    auto best = grabRadius;

    for (int i = 0; i < numBands; ++i)
    {
        if (settings[(size_t) i].on)
            continue;

        const auto d = juce::Point<float> (xForFrequency (settings[(size_t) i].band.frequency), yForDb (0.0)).getDistanceFrom (p);

        if (d <= best)
        {
            best = d;
            nearest = i;
        }
    }

    return nearest;
}

void ParametricEq::mouseMove (const juce::MouseEvent& e)
{
    const auto on = bandAt (e.position);
    const auto now = on >= 0 ? on : offBandAt (e.position);
    setMouseCursor (on >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void ParametricEq::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
    }
}

void ParametricEq::mouseDown (const juce::MouseEvent& e)
{
    dragging = bandAt (e.position);

    if (dragging >= 0 && dragging != selected)
    {
        selected = dragging;
        repaint();
    }
}

void ParametricEq::mouseDoubleClick (const juce::MouseEvent& e)
{
    dragging = -1;

    // A handle: that band off. A ring: that band on. Elsewhere on the drawing: the first band off, there.
    if (const auto on = bandAt (e.position); on >= 0)
    {
        rows[(size_t) on]->on.setToggleState (false, juce::sendNotificationSync);
        return;
    }

    auto index = offBandAt (e.position);
    const auto placing = index < 0;

    if (placing)
    {
        if (! getCurveArea().contains (e.position))
            return;

        for (int i = 0; i < numBands && index < 0; ++i)
            if (! settings[(size_t) i].on)
                index = i;

        if (index < 0)
            return;
    }

    auto& row = *rows[(size_t) index];
    row.on.setToggleState (true, juce::sendNotificationSync);
    selected = index;

    if (placing)
    {
        // Set where it was double-clicked, as a drag there would.
        dragging = index;
        mouseDrag (e);
        dragging = -1;
    }

    repaint();
}

void ParametricEq::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    auto& row = *rows[(size_t) dragging];
    const auto& band = settings[(size_t) dragging].band;
    const auto frequency = juce::jlimit (row.frequency.getMinimum(), row.frequency.getMaximum(), frequencyForX (e.position.x));
    row.frequency.setValue (frequency, juce::sendNotificationSync);

    if (isPass (band))
        return;

    // The gain that puts the curve under the mouse at this frequency, given the other bands: a bell
    // makes all its gain at its frequency, a shelf half of it.
    double others = 0.0;

    for (int i = 0; i < numBands; ++i)
        if (i != dragging && settings[(size_t) i].on && ! Equalizer::isFlat (settings[(size_t) i].band))
            others += Equalizer::responseDb (settings[(size_t) i].band, sampleRate, frequency);

    const auto share = band.type == Equalizer::BandType::peak ? 1.0 : 0.5;
    row.gain.setValue (juce::jlimit (-rangeDb, rangeDb, (dbForY (e.position.y) - others) / share), juce::sendNotificationSync);
}

void ParametricEq::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
}

void ParametricEq::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto index = bandAt (e.position);

    if (index < 0 || juce::exactlyEqual (wheel.deltaY, 0.0f))
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    // Up narrows the band (a higher Q), down widens it, by a tenth a step.
    auto& q = rows[(size_t) index]->q;
    q.setValue (juce::jlimit (q.getMinimum(), q.getMaximum(), q.getValue() * (wheel.deltaY > 0.0f ? 1.1 : 1.0 / 1.1)), juce::sendNotificationSync);
}

juce::Path ParametricEq::responsePath (int band) const
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

        const auto db = band < 0 ? responseDb (hz) : Equalizer::responseDb (settings[(size_t) band].band, sampleRate, hz);
        const auto y = yForDb (db);
        // Path::isEmpty() stays true after a move alone, so a flag marks the start.
        started ? path.lineTo (x, y) : path.startNewSubPath (x, y);
        started = true;
    }

    return path;
}

void ParametricEq::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (spectrum.empty() || spectrumRate <= 0.0)
        return;

    // -90 dBFS at the bottom to 0 at the top, one point a pixel and a half.
    constexpr float floorDb = -90.0f;
    juce::Path shape;
    shape.startNewSubPath (area.getX(), area.getBottom());

    const auto binOf = [this] (double hz) { return hz * Analysis::fftSize / spectrumRate; };
    const auto last = (int) spectrum.size() - 1;

    for (auto x = area.getX(); x <= area.getRight(); x += 1.5f)
    {
        // Where a point covers less than a bin (the lows), between the two bins round it, so the
        // shape is smooth rather than stepped; where it covers several, the loudest of them.
        const auto from = binOf (frequencyForX (x)), to = binOf (frequencyForX (x + 1.5f));
        auto level = -150.0f;

        if (to - from < 1.0)
        {
            const auto below = juce::jlimit (0, last, (int) std::floor (from));
            const auto above = juce::jmin (last, below + 1);
            level = juce::jmap ((float) (from - std::floor (from)), spectrum[(size_t) below], spectrum[(size_t) above]);
        }
        else
        {
            for (auto b = juce::jlimit (0, last, (int) std::floor (from)); b <= juce::jlimit (0, last, (int) std::ceil (to)); ++b)
                level = juce::jmax (level, spectrum[(size_t) b]);
        }

        shape.lineTo (x, area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (level - floorDb) / -floorDb));
    }

    shape.lineTo (area.getRight(), area.getBottom());
    shape.closeSubPath();
    g.setGradientFill (juce::ColourGradient (Theme::overlay.withAlpha (0.16f), area.getX(), area.getY(),
                                             Theme::overlay.withAlpha (0.03f), area.getX(), area.getBottom(), false));
    g.fillPath (shape);
    g.setColour (Theme::overlay.withAlpha (0.22f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));
}

void ParametricEq::paintReadout (juce::Graphics& g, int band) const
{
    const auto& b = settings[(size_t) band].band;
    const auto hz = b.frequency >= 1000.0 ? juce::String (b.frequency / 1000.0, 2) + " kHz" : juce::String (b.frequency, 0) + " Hz";
    auto text = juce::String (typeNames[(int) b.type]) + "  " + hz;

    if (! isPass (b))
        text << "  " << (b.gainDb > 0.0 ? "+" : "") << juce::String (b.gainDb, 1) << " dB";

    text << "  Q " << juce::String (b.q, 2);

    if (! settings[(size_t) band].on)
        text = "Off: double-click to switch on";

    const juce::Font font (juce::FontOptions (12.0f));
    const auto width = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f;
    const auto area = getCurveArea();
    const auto at = settings[(size_t) band].on ? pointFor (band) : juce::Point<float> (xForFrequency (b.frequency), yForDb (0.0));
    auto box = juce::Rectangle<float> (width, 22.0f).withCentre (at.translated (0.0f, at.y - area.getY() > 40.0f ? -30.0f : 30.0f));
    box = box.withX (juce::jlimit (area.getX() + 2.0f, area.getRight() - width - 2.0f, box.getX()));

    g.setColour (Theme::inset.withAlpha (0.92f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (bandColour (band).withAlpha (0.8f));
    g.drawRoundedRectangle (box, 5.0f, 1.0f);
    g.setColour (Theme::textStrong);
    g.setFont (font);
    g.drawText (text, box, juce::Justification::centred);
}

void ParametricEq::paint (juce::Graphics& g)
{
    const auto area = getCurveArea();
    g.fillAll (Theme::base);

    // The rows' colours: a strip by each, and the band picked out shaded across its row.
    for (int i = 0; i < numBands; ++i)
    {
        const auto rowArea = rows[(size_t) i]->on.getBounds().getUnion (rows[(size_t) i]->q.getBounds()).toFloat().expanded (4.0f, 1.0f);

        if (i == selected)
        {
            g.setColour (bandColour (i).withAlpha (0.12f));
            g.fillRoundedRectangle (rowArea, 4.0f);
        }

        g.setColour (bandColour (i).withAlpha (settings[(size_t) i].on ? 1.0f : 0.3f));
        g.fillRoundedRectangle (rowArea.withWidth (3.0f), 1.5f);
    }

    // The drawing: a well, darker at the bottom, with the spectrum behind everything else.
    g.setGradientFill (juce::ColourGradient (Theme::canvas.brighter (0.06f), area.getX(), area.getY(),
                                             Theme::canvas.darker (0.25f), area.getX(), area.getBottom(), false));
    g.fillRoundedRectangle (area, 6.0f);
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        paintSpectrum (g, area);
    }

    g.setFont (juce::FontOptions (10.5f));

    for (auto hz : { 30.0, 50.0, 100.0, 200.0, 300.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0, 10000.0 })
    {
        const auto x = xForFrequency (hz);
        const auto major = hz == 100.0 || hz == 1000.0 || hz == 10000.0;
        g.setColour (Theme::overlay.withAlpha (major ? 0.12f : 0.06f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (Theme::dimText);
        g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x - 15.0f, area.getBottom() - 15.0f, 30.0f, 12.0f), juce::Justification::centred);
    }

    for (auto db : { -18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0 })
    {
        const auto y = yForDb (db);
        g.setColour (Theme::overlay.withAlpha (juce::exactlyEqual (db, 0.0) ? 0.22f : 0.06f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());

        if (std::abs (db) > 3.0 && std::fmod (std::abs (db), 12.0) < 0.5)
        {
            g.setColour (Theme::dimText);
            g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getRight() - 30.0f, y - 7.0f, 26.0f, 14.0f),
                        juce::Justification::right);
        }
    }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (area.toNearestInt());
    const auto zero = yForDb (0.0);

    // Each band's own response, shaded from the 0 dB line in its colour; the one picked out or under
    // the mouse more strongly.
    for (int i = 0; i < numBands; ++i)
    {
        if (! settings[(size_t) i].on || Equalizer::isFlat (settings[(size_t) i].band))
            continue;

        const auto line = responsePath (i);
        auto shade = line;
        shade.lineTo (shade.getCurrentPosition().x, zero);
        shade.lineTo (area.getX(), zero);
        shade.closeSubPath();
        const auto strong = i == selected || i == hovered;
        g.setColour (bandColour (i).withAlpha (strong ? 0.30f : 0.14f));
        g.fillPath (shade);
        g.setColour (bandColour (i).withAlpha (strong ? 0.9f : 0.45f));
        g.strokePath (line, juce::PathStrokeType (strong ? 1.5f : 1.0f));
    }

    // All of them together, glowing.
    const auto total = responsePath (-1);
    const auto glow = Theme::accentLight;
    g.setColour (glow.withAlpha (0.10f));
    g.strokePath (total, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (glow.withAlpha (0.22f));
    g.strokePath (total, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (glow.interpolatedWith (juce::Colours::white, 0.5f));
    g.strokePath (total, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The handles: numbered discs with a halo, rings for the bands switched off.
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

    for (int i = 0; i < numBands; ++i)
    {
        const auto colour = bandColour (i);
        const auto strong = i == selected || i == hovered;

        if (! settings[(size_t) i].on)
        {
            const auto p = juce::Point<float> (xForFrequency (settings[(size_t) i].band.frequency), zero);
            g.setColour (colour.withAlpha (strong ? 0.8f : 0.35f));
            g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p), 1.5f);
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (14.0f, 14.0f).withCentre (p), juce::Justification::centred);
            continue;
        }

        const auto p = pointFor (i);
        const auto radius = strong ? 9.5f : 8.0f;
        g.setColour (colour.withAlpha (strong ? 0.30f : 0.16f));
        g.fillEllipse (juce::Rectangle<float> (radius * 3.2f, radius * 3.2f).withCentre (p));
        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p));

        if (i == selected)
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawEllipse (juce::Rectangle<float> (radius * 2.0f + 5.0f, radius * 2.0f + 5.0f).withCentre (p), 1.5f);
        }

        g.setColour (juce::Colour (0xff15171c));
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p), juce::Justification::centred);
    }

    const auto shown = dragging >= 0 ? dragging : hovered;

    if (shown >= 0)
        paintReadout (g, shown);
}

void ParametricEq::resized()
{
    auto controls = getLocalBounds().removeFromBottom (numBands * rowHeight + 8).reduced (8, 4);

    for (auto& row : rows)
    {
        auto line = controls.removeFromTop (rowHeight).reduced (0, 3);
        row->on.setBounds (line.removeFromLeft (44));
        row->type.setBounds (line.removeFromLeft (110));
        line.removeFromLeft (8);
        const auto third = line.getWidth() / 3;
        row->frequency.setBounds (line.removeFromLeft (third + labelWidth / 2));
        row->gain.setBounds (line.removeFromLeft (third));
        row->q.setBounds (line);
    }
}
