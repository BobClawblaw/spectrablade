#include "ParametricEq.h"

namespace
{
    constexpr int rowHeight = 28, labelWidth = 60;
    constexpr const char* typeNames[] = { "Bell", "Low Shelf", "High Shelf", "High-Pass", "Low-Pass" };

    bool isPass (const Equalizer::Band& band)
    {
        return band.type == Equalizer::BandType::highPass || band.type == Equalizer::BandType::lowPass;
    }

    bool changesNothing (const Equalizer::Band& band)
    {
        return ! isPass (band) && juce::exactlyEqual (band.gainDb, 0.0);
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
}

void ParametricEq::readRow (int index)
{
    auto& row = *rows[(size_t) index];
    auto& setting = settings[(size_t) index];
    setting.on = row.on.getToggleState();
    setting.band.type = (Equalizer::BandType) juce::jmax (0, row.type.getSelectedItemIndex());
    setting.band.frequency = row.frequency.getValue();
    setting.band.gainDb = row.gain.getValue();
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
        if (s.on && ! changesNothing (s.band))
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

void ParametricEq::paint (juce::Graphics& g)
{
    const auto area = getCurveArea();
    const auto text = getLookAndFeel().findColour (juce::Label::textColourId);
    const auto background = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    g.fillAll (background);
    g.setColour (background.darker (0.3f));
    g.fillRect (area);

    g.setFont (11.0f);

    for (auto hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const auto x = xForFrequency (hz);
        g.setColour (text.withAlpha (0.15f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (text.withAlpha (0.5f));
        g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x + 2.0f, area.getBottom() - 14.0f, 30.0f, 12.0f), juce::Justification::left);
    }

    for (auto db : { -rangeDb, -12.0, 0.0, 12.0, rangeDb })
    {
        const auto y = yForDb (db);
        g.setColour (text.withAlpha (juce::exactlyEqual (db, 0.0) ? 0.35f : 0.15f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (text.withAlpha (0.5f));
        const auto labelY = juce::jmax (area.getY(), y - 12.0f);  // the top label goes below its line
        g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getX() + 2.0f, labelY, 30.0f, 12.0f),
                    juce::Justification::left);
    }

    juce::Path curve;
    const auto nyquist = sampleRate / 2.0;
    auto started = false;

    for (auto x = area.getX(); x <= area.getRight(); x += 1.0f)
    {
        const auto hz = frequencyForX (x);

        if (hz >= nyquist)
            break;

        const auto y = yForDb (responseDb (hz));
        // Path::isEmpty() stays true after a move alone, so a flag marks the start.
        started ? curve.lineTo (x, y) : curve.startNewSubPath (x, y);
        started = true;
    }

    g.setColour (getLookAndFeel().findColour (juce::Slider::thumbColourId));
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    for (int i = 0; i < numBands; ++i)
    {
        auto& s = settings[(size_t) i];

        if (! s.on)
            continue;

        const juce::Point<float> p (xForFrequency (s.band.frequency), yForDb (responseDb (s.band.frequency)));
        g.setColour (text);
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (12.0f, 12.0f).withCentre (p.translated (0.0f, -12.0f)), juce::Justification::centred);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p));
    }
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
