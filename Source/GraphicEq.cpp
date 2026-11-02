#include "GraphicEq.h"

namespace GraphicEq
{
const std::vector<double>& centres (int numBands)
{
    static const std::vector<double> octaves { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    static const std::vector<double> thirds { 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
                                              800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0,
                                              12500.0, 16000.0, 20000.0 };
    return numBands == 31 ? thirds : octaves;
}

double bandQ (int numBands)
{
    // A bell spanning the octaves between its neighbours: Q = sqrt(2^w) / (2^w - 1).
    const auto ratio = std::pow (2.0, numBands == 31 ? 1.0 / 3.0 : 1.0);
    return std::sqrt (ratio) / (ratio - 1.0);
}

bool isValid (const std::vector<double>& gains)
{
    if (gains.size() != 10 && gains.size() != 31)
        return false;

    for (auto g : gains)
        if (! (std::abs (g) <= maxGainDb))
            return false;

    return true;
}

std::vector<Equalizer::Band> bandsFor (const std::vector<double>& gains, double sampleRate)
{
    std::vector<Equalizer::Band> bands;

    if (! isValid (gains))
        return bands;

    const auto& f = centres ((int) gains.size());
    const auto q = bandQ ((int) gains.size());

    for (size_t i = 0; i < gains.size(); ++i)
    {
        const Equalizer::Band band { Equalizer::BandType::peak, f[i], gains[i], q };

        if (! Equalizer::isFlat (band) && Equalizer::isValid (band, sampleRate))
            bands.push_back (band);
    }

    return bands;
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<double>& gains)
{
    const auto bands = bandsFor (gains, sampleRate);
    return bands.empty() ? nullptr : Equalizer::apply (s, start, end, sampleRate, bands);
}

//==============================================================================
Panel::Panel (int numBands)
{
    const auto& f = centres (numBands);

    for (auto hz : f)
    {
        auto* s = sliders.add (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        s->setRange (-maxGainDb, maxGainDb, 0.1);
        s->setValue (0.0, juce::dontSendNotification);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, 18);
        s->setDoubleClickReturnValue (true, 0.0);
        s->onValueChange = [this]
        {
            if (onChange != nullptr)
                onChange();
        };
        addAndMakeVisible (s);

        auto* l = labels.add (new juce::Label ({}, hz >= 1000.0 ? juce::String (hz / 1000.0) + "k" : juce::String (hz)));   // 31.5, 63, 1k, 12.5k
        l->setJustificationType (juce::Justification::centred);
        l->setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (l);
    }

    flat.onClick = [this]
    {
        setGains (std::vector<double> ((size_t) sliders.size(), 0.0));

        if (onChange != nullptr)
            onChange();
    };
    addAndMakeVisible (flat);

    for (auto* l : { &top, &bottom })
    {
        l->setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (l);
    }

    setSize (60 + numBands * (numBands == 31 ? 30 : 52), 300);
}

std::vector<double> Panel::getGains() const
{
    std::vector<double> g;

    for (auto* s : sliders)
        g.push_back (s->getValue());

    return g;
}

void Panel::setGains (const std::vector<double>& gains)
{
    for (int i = 0; i < sliders.size() && i < (int) gains.size(); ++i)
        sliders[i]->setValue (juce::jlimit (-maxGainDb, maxGainDb, gains[(size_t) i]), juce::dontSendNotification);
}

void Panel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto bottomRow = r.removeFromBottom (26);
    flat.setBounds (bottomRow.removeFromRight (70));
    auto scale = r.removeFromLeft (48);
    top.setBounds (scale.removeFromTop (20));
    bottom.setBounds (scale.removeFromBottom (58).removeFromTop (20));
    const auto width = r.getWidth() / juce::jmax (1, sliders.size());

    for (int i = 0; i < sliders.size(); ++i)
    {
        auto column = r.removeFromLeft (width);
        labels[i]->setBounds (column.removeFromBottom (18));
        sliders[i]->setBounds (column);
    }
}
}
