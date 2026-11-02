#include "BuiltInEffects.h"

namespace BuiltInEffects
{
namespace
{
    constexpr double shelfQ = 0.7071067811865476;
    const juce::StringArray typeNames { "Bell", "Low Shelf", "High Shelf", "High-Pass", "Low-Pass" };

    // The panel's starting bands: a low shelf at 100 Hz, bells at 400 Hz, 1 kHz and 3 kHz and a
    // high shelf at 8 kHz, all at 0 dB.
    const std::array<Equalizer::Band, ParametricEqProcessor::numBands> startingBands {{
        { Equalizer::BandType::lowShelf, 100.0, 0.0, shelfQ },
        { Equalizer::BandType::peak, 400.0, 0.0, 1.0 },
        { Equalizer::BandType::peak, 1000.0, 0.0, 1.0 },
        { Equalizer::BandType::peak, 3000.0, 0.0, 1.0 },
        { Equalizer::BandType::highShelf, 8000.0, 0.0, shelfQ },
    }};

    /** Sets a float parameter to `plain` exactly when some normalised value gives it, else as near as
        one does: the normalised value JUCE works out is tried, then its neighbours a few floats away,
        since the conversion back through a skewed range can land a float off. */
    void setExactly (juce::AudioParameterFloat& p, float plain)
    {
        const auto& range = p.getNormalisableRange();
        plain = range.getRange().clipValue (plain);
        auto best = range.convertTo0to1 (plain);
        auto bestError = std::abs (range.convertFrom0to1 (best) - plain);
        auto down = best, up = best;

        for (int step = 0; step < 16 && bestError > 0.0f; ++step)
        {
            down = std::nextafter (down, 0.0f);
            up = std::nextafter (up, 1.0f);

            for (auto n : { down, up })
            {
                const auto error = std::abs (range.convertFrom0to1 (n) - plain);

                if (error < bestError)
                {
                    best = n;
                    bestError = error;
                }
            }
        }

        p.setValueNotifyingHost (best);
    }

    juce::PluginDescription parametricEqDescription()
    {
        juce::PluginDescription d;
        d.name = d.descriptiveName = "Parametric EQ";
        d.pluginFormatName = formatName;
        d.category = "EQ";
        d.manufacturerName = "SpectraBlade";
        d.version = "1";
        d.fileOrIdentifier = parametricEqId;
        d.uniqueId = d.deprecatedUid = 0x53425051;  // "SBPQ"
        d.isInstrument = false;
        d.numInputChannels = d.numOutputChannels = 2;
        return d;
    }

    class Format final : public juce::AudioPluginFormat
    {
    public:
        juce::String getName() const override                     { return formatName; }

        void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& id) override
        {
            for (auto& d : getDescriptions())
                if (d.fileOrIdentifier == id)
                    results.add (new juce::PluginDescription (d));
        }

        bool fileMightContainThisPluginType (const juce::String& id) override    { return id.startsWith ("spectrablade:"); }
        juce::String getNameOfPluginFromIdentifier (const juce::String& id) override
        {
            for (auto& d : getDescriptions())
                if (d.fileOrIdentifier == id)
                    return d.name;

            return id;
        }

        bool pluginNeedsRescanning (const juce::PluginDescription&) override     { return false; }
        bool doesPluginStillExist (const juce::PluginDescription& d) override    { return d.fileOrIdentifier == parametricEqId; }
        bool canScanForPlugins() const override                                    { return false; }
        bool isTrivialToScan() const override                                      { return true; }
        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
        juce::FileSearchPath getDefaultLocationsToSearch() override               { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    protected:
        void createPluginInstance (const juce::PluginDescription& d, double rate, int blockSize, PluginCreationCallback done) override
        {
            if (d.fileOrIdentifier != parametricEqId)
            {
                done (nullptr, "SpectraBlade has no built-in effect called " + d.name.quoted() + ".");
                return;
            }

            auto eq = std::make_unique<ParametricEqProcessor>();
            eq->setRateAndBufferSizeDetails (rate, blockSize);
            done (std::move (eq), {});
        }
    };
}

std::unique_ptr<juce::AudioPluginFormat> createFormat()
{
    return std::make_unique<Format>();
}

juce::Array<juce::PluginDescription> getDescriptions()
{
    return { parametricEqDescription() };
}

void addTo (juce::KnownPluginList& list)
{
    for (auto& d : getDescriptions())
        list.addType (d);
}

//==============================================================================
ParametricEqProcessor::ParametricEqProcessor()
    : AudioPluginInstance (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo())
                                            .withOutput ("Output", juce::AudioChannelSet::stereo()))
{
    // Hosted parameters, so automation and saved sessions find each by its ID, as a plug-in's.
    const auto add = [this] (auto parameter)
    {
        auto* raw = parameter.get();
        addHostedParameter (std::move (parameter));
        return raw;
    };

    for (int i = 0; i < numBands; ++i)
    {
        const auto n = juce::String (i + 1);
        const auto& start = startingBands[(size_t) i];
        auto& p = bandParameters[(size_t) i];
        const juce::NormalisableRange<float> hz (20.0f, 20000.0f, 0.0f, 0.2f);
        const juce::NormalisableRange<float> db (-24.0f, 24.0f);
        const juce::NormalisableRange<float> q (0.1f, 20.0f, 0.0f, 0.3f);

        p.on = add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "band" + n + "On", 1 }, "Band " + n + " On", true));
        p.type = add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "band" + n + "Type", 1 }, "Band " + n + " Type", typeNames, (int) start.type));
        p.frequency = add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "band" + n + "Frequency", 1 }, "Band " + n + " Frequency", hz,
                                                                        (float) start.frequency, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        p.gain = add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "band" + n + "Gain", 1 }, "Band " + n + " Gain", db,
                                                                   (float) start.gainDb, juce::AudioParameterFloatAttributes().withLabel ("dB")));
        p.q = add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "band" + n + "Q", 1 }, "Band " + n + " Q", q, (float) start.q));

        // A parameter starts at its default exactly, which no normalised value may give on a skewed
        // range; setting it through one makes every value it holds one a saved state can restore.
        for (auto* f : { p.frequency, p.gain, p.q })
            setExactly (*f, f->get());
    }
}

Equalizer::Band ParametricEqProcessor::bandFromParameters (int index) const
{
    const auto& p = bandParameters[(size_t) index];
    return { (Equalizer::BandType) p.type->getIndex(), (double) p.frequency->get(), (double) p.gain->get(), (double) p.q->get() };
}

std::array<std::pair<bool, Equalizer::Band>, ParametricEqProcessor::numBands> ParametricEqProcessor::getBands() const
{
    std::array<std::pair<bool, Equalizer::Band>, numBands> bands;

    for (int i = 0; i < numBands; ++i)
        bands[(size_t) i] = { bandParameters[(size_t) i].on->get(), bandFromParameters (i) };

    return bands;
}

std::vector<Equalizer::Band> ParametricEqProcessor::getActiveBands (double sampleRate) const
{
    std::vector<Equalizer::Band> active;

    for (auto& [on, band] : getBands())
        if (on && ! Equalizer::isFlat (band) && Equalizer::isValid (band, sampleRate))
            active.push_back (band);

    return active;
}

void ParametricEqProcessor::setBand (int index, bool on, const Equalizer::Band& band)
{
    auto& p = bandParameters[(size_t) index];
    p.on->setValueNotifyingHost (on ? 1.0f : 0.0f);
    p.type->setValueNotifyingHost (p.type->convertTo0to1 ((float) (int) band.type));
    setExactly (*p.frequency, (float) band.frequency);
    setExactly (*p.gain, (float) band.gainDb);
    setExactly (*p.q, (float) band.q);
}

void ParametricEqProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d = parametricEqDescription();
}

bool ParametricEqProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto in = layout.getMainInputChannelSet(), out = layout.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void ParametricEqProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    reset();
}

void ParametricEqProcessor::reset()
{
    for (auto& f : filtersSetFor)
        f.reset();
}

void ParametricEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());

    for (int i = 0; i < numBands; ++i)
    {
        const auto band = bandFromParameters (i);
        auto& set = filtersSetFor[(size_t) i];
        auto& bandFilters = filters[(size_t) i];

        if (! bandParameters[(size_t) i].on->get() || Equalizer::isFlat (band) || ! Equalizer::isValid (band, rate))
        {
            set.reset();
            continue;
        }

        if (! set.has_value() || (int) bandFilters.size() != channels)
        {
            bandFilters.assign ((size_t) channels, {});  // a band coming back starts from silence
            set.reset();
        }

        if (! set.has_value() || set->type != band.type || ! juce::exactlyEqual (set->frequency, band.frequency)
            || ! juce::exactlyEqual (set->gainDb, band.gainDb) || ! juce::exactlyEqual (set->q, band.q))
        {
            for (auto& f : bandFilters)
                f.setBand (band, rate);

            set = band;
        }

        for (int ch = 0; ch < channels; ++ch)
            bandFilters[(size_t) ch].process (buffer.getWritePointer (ch), buffer.getNumSamples());
    }
}

juce::AudioProcessorEditor* ParametricEqProcessor::createEditor()
{
    return new ParametricEqEditor (*this);
}

void ParametricEqProcessor::getStateInformation (juce::MemoryBlock& block)
{
    // Plain values (Hz, dB, Q, the type's index, on or off), each exactly as the parameter holds it.
    juce::XmlElement xml ("SPECTRABLADE_PARAMETRIC_EQ");
    xml.setAttribute ("version", 1);

    for (auto& p : bandParameters)
    {
        xml.setAttribute (p.on->paramID, p.on->get() ? 1 : 0);
        xml.setAttribute (p.type->paramID, p.type->getIndex());

        for (auto* f : { p.frequency, p.gain, p.q })
            xml.setAttribute (f->paramID, (double) f->get());
    }

    copyXmlToBinary (xml, block);
}

void ParametricEqProcessor::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);

    if (xml == nullptr || ! xml->hasTagName ("SPECTRABLADE_PARAMETRIC_EQ"))
        return;

    for (auto& p : bandParameters)
    {
        if (xml->hasAttribute (p.on->paramID))
            p.on->setValueNotifyingHost (xml->getIntAttribute (p.on->paramID) != 0 ? 1.0f : 0.0f);

        if (xml->hasAttribute (p.type->paramID))
            p.type->setValueNotifyingHost (p.type->convertTo0to1 ((float) juce::jlimit (0, typeNames.size() - 1, xml->getIntAttribute (p.type->paramID))));

        for (auto* f : { p.frequency, p.gain, p.q })
            if (xml->hasAttribute (f->paramID))
                setExactly (*f, (float) xml->getDoubleAttribute (f->paramID));
    }
}

//==============================================================================
namespace
{
    ParametricEq::Settings settingsOf (const ParametricEqProcessor& eq)
    {
        ParametricEq::Settings s;
        const auto bands = eq.getBands();

        for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
            s[(size_t) i] = { bands[(size_t) i].first, bands[(size_t) i].second };

        return s;
    }

    bool same (const ParametricEq::BandSetting& a, const ParametricEq::BandSetting& b)
    {
        return a.on == b.on && a.band.type == b.band.type && juce::exactlyEqual (a.band.frequency, b.band.frequency)
            && juce::exactlyEqual (a.band.gainDb, b.band.gainDb) && juce::exactlyEqual (a.band.q, b.band.q);
    }
}

ParametricEqEditor::ParametricEqEditor (ParametricEqProcessor& owner)
    : AudioProcessorEditor (owner), eq (owner)
{
    const auto rate = owner.getSampleRate() > 0.0 ? owner.getSampleRate() : 48000.0;
    shown = settingsOf (eq);
    panel = std::make_unique<ParametricEq> (shown, rate);
    panel->onChange = [this] { panelChanged(); };
    addAndMakeVisible (*panel);
    setResizable (true, false);
    setResizeLimits (520, 300, 3000, 2000);
    setSize (panel->getWidth(), panel->getHeight());
    startTimerHz (10);
}

ParametricEqEditor::~ParametricEqEditor()
{
    stopTimer();
}

void ParametricEqEditor::panelChanged()
{
    // Only the bands the panel changed, each as one gesture per parameter, so nothing else moves.
    const auto& wanted = panel->getSettings();

    for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
    {
        if (same (wanted[(size_t) i], shown[(size_t) i]))
            continue;

        for (auto* p : { (juce::AudioProcessorParameter*) &eq.onParameter (i), (juce::AudioProcessorParameter*) &eq.typeParameter (i),
                         (juce::AudioProcessorParameter*) &eq.frequencyParameter (i), (juce::AudioProcessorParameter*) &eq.gainParameter (i),
                         (juce::AudioProcessorParameter*) &eq.qParameter (i) })
            p->beginChangeGesture();

        eq.setBand (i, wanted[(size_t) i].on, wanted[(size_t) i].band);

        for (auto* p : { (juce::AudioProcessorParameter*) &eq.onParameter (i), (juce::AudioProcessorParameter*) &eq.typeParameter (i),
                         (juce::AudioProcessorParameter*) &eq.frequencyParameter (i), (juce::AudioProcessorParameter*) &eq.gainParameter (i),
                         (juce::AudioProcessorParameter*) &eq.qParameter (i) })
            p->endChangeGesture();
    }

    // What the parameters hold (the nearest values their ranges give) is what is shown from now on.
    shown = settingsOf (eq);
}

void ParametricEqEditor::refresh()
{
    const auto now = settingsOf (eq);
    auto changed = false;

    for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
        changed = changed || ! same (now[(size_t) i], shown[(size_t) i]);

    if (! changed)
        return;

    shown = now;
    panel->setSettings (now);
}

void ParametricEqEditor::resized()
{
    panel->setBounds (getLocalBounds());
}
}
