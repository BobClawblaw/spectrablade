#include "BuiltInEffects.h"
#include "AudioFileIO.h"
#include "Theme.h"

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

    juce::PluginDescription describe (const char* name, const char* category, const char* id, int uid)
    {
        juce::PluginDescription d;
        d.name = d.descriptiveName = name;
        d.pluginFormatName = formatName;
        d.category = category;
        d.manufacturerName = "SpectraBlade";
        d.version = "1";
        d.fileOrIdentifier = id;
        d.uniqueId = d.deprecatedUid = uid;
        d.isInstrument = false;
        d.numInputChannels = d.numOutputChannels = 2;
        return d;
    }

    juce::PluginDescription parametricEqDescription()   { return describe ("Parametric EQ", "EQ", parametricEqId, 0x53425051); }  // "SBPQ"
    juce::PluginDescription compressorDescription()     { return describe ("Compressor", "Dynamics", compressorId, 0x53424350); }  // "SBCP"
    juce::PluginDescription limiterDescription()        { return describe ("Limiter", "Dynamics", limiterId, 0x53424c4d); }       // "SBLM"
    juce::PluginDescription chorusDescription()         { return describe ("Chorus", "Modulation", chorusId, 0x53424348); }       // "SBCH"
    juce::PluginDescription flangerDescription()        { return describe ("Flanger", "Modulation", flangerId, 0x5342464c); }     // "SBFL"
    juce::PluginDescription phaserDescription()         { return describe ("Phaser", "Modulation", phaserId, 0x53425048); }       // "SBPH"

    juce::PluginDescription reverbDescription()         { return describe ("Reverb", "Reverb", reverbId, 0x53425256); }         // "SBRV"

    juce::PluginDescription convolutionDescription()    { return describe ("Convolution Reverb", "Reverb", convolutionId, 0x5342434f); }  // "SBCO"

    juce::PluginDescription multibandDescription()      { return describe ("Multiband Compressor", "Dynamics", multibandId, 0x53424d42); }   // "SBMB"
    juce::PluginDescription deEsserDescription()        { return describe ("De-esser", "Dynamics", deEsserId, 0x53424445); }                 // "SBDE"
    juce::PluginDescription dynamicEqDescription()      { return describe ("Dynamic EQ", "EQ", dynamicEqId, 0x53424451); }                   // "SBDQ"
    juce::PluginDescription multibandWidthDescription() { return describe ("Multiband Stereo Width", "Spatial", multibandWidthId, 0x53424d57); }   // "SBMW"
    juce::PluginDescription exciterDescription()        { return describe ("Exciter", "Distortion", exciterId, 0x53424558); }                     // "SBEX"
    juce::PluginDescription maximizerDescription()      { return describe ("Maximizer", "Dynamics", maximizerId, 0x53424d58); }                   // "SBMX"
    juce::PluginDescription transientShaperDescription() { return describe ("Transient Shaper", "Dynamics", transientShaperId, 0x53425453); }     // "SBTS"
    juce::PluginDescription basicSynthDescription()
    {
        auto d = describe ("Basic Synth", "Instrument", basicSynthId, 0x53425359);   // "SBSY"
        d.isInstrument = true;
        d.numInputChannels = 0;
        return d;
    }

    juce::PluginDescription graphicEqDescription (int bands)
    {
        return bands == 31 ? describe ("Graphic EQ (31 Bands)", "EQ", graphicEq31Id, 0x53424733)    // "SBG3"
                           : describe ("Graphic EQ (10 Bands)", "EQ", graphicEq10Id, 0x53424731);   // "SBG1"
    }

    const juce::StringArray stageNames { "2", "4", "6", "8", "10", "12" };

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
        bool doesPluginStillExist (const juce::PluginDescription& d) override
        {
            for (auto& known : getDescriptions())
                if (known.fileOrIdentifier == d.fileOrIdentifier)
                    return true;

            return false;
        }
        bool canScanForPlugins() const override                                    { return false; }
        bool isTrivialToScan() const override                                      { return true; }
        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
        juce::FileSearchPath getDefaultLocationsToSearch() override               { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    protected:
        void createPluginInstance (const juce::PluginDescription& d, double rate, int blockSize, PluginCreationCallback done) override
        {
            std::unique_ptr<BuiltInEffect> effect;

            if (d.fileOrIdentifier == parametricEqId)
                effect = std::make_unique<ParametricEqProcessor>();
            else if (d.fileOrIdentifier == compressorId)
                effect = std::make_unique<CompressorProcessor>();
            else if (d.fileOrIdentifier == limiterId)
                effect = std::make_unique<LimiterProcessor>();
            else if (d.fileOrIdentifier == chorusId)
                effect = std::make_unique<SweptDelayProcessor> (SweptDelayProcessor::Kind::chorus);
            else if (d.fileOrIdentifier == flangerId)
                effect = std::make_unique<SweptDelayProcessor> (SweptDelayProcessor::Kind::flanger);
            else if (d.fileOrIdentifier == phaserId)
                effect = std::make_unique<PhaserProcessor>();
            else if (d.fileOrIdentifier == reverbId)
                effect = std::make_unique<ReverbProcessor>();
            else if (d.fileOrIdentifier == convolutionId)
                effect = std::make_unique<ConvolutionProcessor>();
            else if (d.fileOrIdentifier == graphicEq10Id)
                effect = std::make_unique<GraphicEqProcessor> (10);
            else if (d.fileOrIdentifier == graphicEq31Id)
                effect = std::make_unique<GraphicEqProcessor> (31);
            else if (d.fileOrIdentifier == multibandId)
                effect = std::make_unique<MultibandProcessor>();
            else if (d.fileOrIdentifier == deEsserId)
                effect = std::make_unique<DeEsserProcessor>();
            else if (d.fileOrIdentifier == dynamicEqId)
                effect = std::make_unique<DynamicEqProcessor>();
            else if (d.fileOrIdentifier == multibandWidthId)
                effect = std::make_unique<MultibandWidthProcessor>();
            else if (d.fileOrIdentifier == exciterId)
                effect = std::make_unique<ExciterProcessor>();
            else if (d.fileOrIdentifier == maximizerId)
                effect = std::make_unique<MaximizerProcessor>();
            else if (d.fileOrIdentifier == transientShaperId)
                effect = std::make_unique<TransientShaperProcessor>();
            else if (d.fileOrIdentifier == basicSynthId)
                effect = std::make_unique<BasicSynthProcessor>();

            if (effect == nullptr)
            {
                done (nullptr, "SpectraBlade has no built-in effect called " + d.name.quoted() + ".");
                return;
            }

            effect->setRateAndBufferSizeDetails (rate, blockSize);
            done (std::move (effect), {});
        }
    };
}

std::unique_ptr<juce::AudioPluginFormat> createFormat()
{
    return std::make_unique<Format>();
}

juce::Array<juce::PluginDescription> getDescriptions()
{
    return { parametricEqDescription(), compressorDescription(), limiterDescription(), chorusDescription(), flangerDescription(), phaserDescription(), reverbDescription(), convolutionDescription(),
             graphicEqDescription (10), graphicEqDescription (31), multibandDescription(), deEsserDescription(), dynamicEqDescription(), multibandWidthDescription(), exciterDescription(), maximizerDescription(), transientShaperDescription(),
             basicSynthDescription() };
}

void addTo (juce::KnownPluginList& list)
{
    for (auto& d : getDescriptions())
        list.addType (d);
}

//==============================================================================
void setExactly (juce::AudioParameterFloat& p, float plain)
{
    // The normalised value JUCE works out is tried, then its neighbours a few floats away, since the
    // conversion back through a skewed range can land a float off.
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

//==============================================================================
juce::AudioProcessor::BusesProperties BuiltInEffect::busesFor (bool sidechain, bool instrument)
{
    if (instrument)
        return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo());

    auto buses = BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo())
                                  .withOutput ("Output", juce::AudioChannelSet::stereo());
    return sidechain ? buses.withInput ("Sidechain", juce::AudioChannelSet::stereo(), false) : buses;
}

BuiltInEffect::BuiltInEffect (const juce::PluginDescription& d, const juce::String& stateTag, bool sidechain, bool isInstrument)
    : AudioPluginInstance (busesFor (sidechain, isInstrument)), description (d), tag (stateTag), instrument (isInstrument)
{
}

bool BuiltInEffect::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto in = layout.getMainInputChannelSet(), out = layout.getMainOutputChannelSet();

    if (instrument)
        return in.isDisabled() && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());

    const auto key = layout.inputBuses.size() > 1 ? layout.getChannelSet (true, 1) : juce::AudioChannelSet::disabled();
    const auto wide = linksChannels() && in.isDiscreteLayout() && in.size() > 2 && in.size() <= mostLinkedChannels;
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo() || wide)
           && (key.isDisabled() || key == juce::AudioChannelSet::mono() || key == juce::AudioChannelSet::stereo());
}

juce::AudioParameterFloat* BuiltInEffect::addFloat (const juce::String& id, const juce::String& name, juce::NormalisableRange<float> range,
                                                    float start, const juce::String& label)
{
    // Shown to two decimals under 100, one under 1,000 and none above, so a value a skewed range
    // holds a float off (3.9999998) reads as what was set (4.00).
    const auto text = [] (float v, int)
    {
        const auto size = std::abs (v);
        return juce::String (v, size >= 1000.0f ? 0 : size >= 100.0f ? 1 : 2);
    };
    auto p = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, start,
                                                          juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (text));
    auto* raw = p.get();
    addHostedParameter (std::move (p));

    // A parameter starts at its default exactly, which no normalised value may give on a skewed
    // range; setting it through one makes every value it holds one a saved state can restore.
    setExactly (*raw, start);
    return raw;
}

juce::AudioParameterChoice* BuiltInEffect::addChoice (const juce::String& id, const juce::String& name, const juce::StringArray& choices, int start)
{
    auto p = std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, start);
    auto* raw = p.get();
    addHostedParameter (std::move (p));
    return raw;
}

juce::AudioParameterBool* BuiltInEffect::addBool (const juce::String& id, const juce::String& name, bool start)
{
    auto p = std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, start);
    auto* raw = p.get();
    addHostedParameter (std::move (p));
    return raw;
}

EffectPresets::Values BuiltInEffect::getPresetValues() const
{
    EffectPresets::Values v;

    for (auto* p : getParameters())
    {
        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
            v[f->paramID] = (double) f->get();
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
            v[c->paramID] = c->getIndex();
        else if (auto* b = dynamic_cast<juce::AudioParameterBool*> (p))
            v[b->paramID] = b->get() ? 1.0 : 0.0;
    }

    return v;
}

void BuiltInEffect::setPresetValues (const EffectPresets::Values& values)
{
    for (auto* p : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        const auto found = ranged != nullptr ? values.find (ranged->paramID) : values.end();

        if (found == values.end() || ! std::isfinite (found->second))
            continue;

        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
            setExactly (*f, (float) found->second);
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
            c->setValueNotifyingHost (c->convertTo0to1 ((float) juce::jlimit (0, c->choices.size() - 1, (int) std::lround (found->second))));
        else if (auto* b = dynamic_cast<juce::AudioParameterBool*> (p))
            b->setValueNotifyingHost (found->second != 0.0 ? 1.0f : 0.0f);
    }
}

namespace
{
    /** A built-in effect's plain window: its presets above the list of its parameters, and, for an
        effect that turns the sound down, a strip under them showing by how much as it plays. */
    class PresetsAndParameters final : public juce::AudioProcessorEditor,
                                       private juce::Timer
    {
    public:
        explicit PresetsAndParameters (BuiltInEffect& e, std::function<double()> reductionDb = {})
            : AudioProcessorEditor (e),
              presets (EffectPresets::userStore(), e.getPresetKey(), [&e] { return e.getPresetValues(); },
                       [&e] (const EffectPresets::Values& v) { e.setPresetValues (v); }),
              parameters (e), reading (std::move (reductionDb))
        {
            addAndMakeVisible (presets);
            addAndMakeVisible (parameters);
            setSize (juce::jmax (400, parameters.getWidth()), parameters.getHeight() + 40 + (reading != nullptr ? stripHeight : 0));

            if (reading != nullptr)
                startTimerHz (20);
        }

        ~PresetsAndParameters() override
        {
            stopTimer();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

            if (reading == nullptr)
                return;

            // "Reduction", the dB, and a bar growing from the right as the sound is turned down (24 dB full).
            auto strip = getLocalBounds().removeFromBottom (stripHeight).reduced (12, 8);
            g.setColour (Theme::dimText);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("Reduction", strip.removeFromLeft (80), juce::Justification::centredLeft);
            g.drawText (juce::String (shownDb, 1) + " dB", strip.removeFromRight (64), juce::Justification::centredRight);
            auto bar = strip.reduced (8, 0).withSizeKeepingCentre (strip.getWidth() - 16, 6);
            g.setColour (Theme::inset);
            g.fillRect (bar);
            g.setColour (Theme::meterHigh);
            g.fillRect (bar.removeFromRight (juce::roundToInt (bar.getWidth() * juce::jlimit (0.0, 1.0, -shownDb / 24.0))));
        }

        void resized() override
        {
            auto r = getLocalBounds();
            presets.setBounds (r.removeFromTop (40).reduced (8, 6));

            if (reading != nullptr)
                r.removeFromBottom (stripHeight);

            parameters.setBounds (r);
        }

        double getShownReductionDb() const noexcept                   { return shownDb; }

    private:
        static constexpr int stripHeight = 32;

        void timerCallback() override
        {
            // Rounded to a tenth of a dB, so a steady signal doesn't repaint.
            if (const auto now = std::round (reading() * 10.0) / 10.0; ! juce::exactlyEqual (now, shownDb))
            {
                shownDb = now;
                repaint (getLocalBounds().removeFromBottom (stripHeight));
            }
        }

        EffectPresets::Bar presets;
        juce::GenericAudioProcessorEditor parameters;
        std::function<double()> reading;
        double shownDb = 0.0;
    };
}

juce::AudioProcessorEditor* BuiltInEffect::createEditor()
{
    return new PresetsAndParameters (*this);
}

void BuiltInEffect::getStateInformation (juce::MemoryBlock& block)
{
    juce::XmlElement xml (tag);
    xml.setAttribute ("version", 1);

    for (auto* p : getParameters())
    {
        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p))
            xml.setAttribute (f->paramID, (double) f->get());
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
            xml.setAttribute (c->paramID, c->getIndex());
        else if (auto* b = dynamic_cast<juce::AudioParameterBool*> (p))
            xml.setAttribute (b->paramID, b->get() ? 1 : 0);
    }

    addToState (xml);
    copyXmlToBinary (xml, block);
}

void BuiltInEffect::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);

    if (xml == nullptr || ! xml->hasTagName (tag))
        return;

    for (auto* p : getParameters())
    {
        if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (p); f != nullptr && xml->hasAttribute (f->paramID))
            setExactly (*f, (float) xml->getDoubleAttribute (f->paramID));
        else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p); c != nullptr && xml->hasAttribute (c->paramID))
            c->setValueNotifyingHost (c->convertTo0to1 ((float) juce::jlimit (0, c->choices.size() - 1, xml->getIntAttribute (c->paramID))));
        else if (auto* b = dynamic_cast<juce::AudioParameterBool*> (p); b != nullptr && xml->hasAttribute (b->paramID))
            b->setValueNotifyingHost (xml->getIntAttribute (b->paramID) != 0 ? 1.0f : 0.0f);
    }

    readFromState (*xml);
}

//==============================================================================
ParametricEqProcessor::ParametricEqProcessor()
    : BuiltInEffect (parametricEqDescription(), "SPECTRABLADE_PARAMETRIC_EQ")
{
    for (int i = 0; i < numBands; ++i)
    {
        const auto n = juce::String (i + 1);
        const auto& start = startingBands[(size_t) i];
        auto& p = bandParameters[(size_t) i];
        const juce::NormalisableRange<float> hz (20.0f, 20000.0f, 0.0f, 0.2f);
        const juce::NormalisableRange<float> db (-24.0f, 24.0f);
        const juce::NormalisableRange<float> q (0.1f, 20.0f, 0.0f, 0.3f);

        p.on = addBool ("band" + n + "On", "Band " + n + " On", true);
        p.type = addChoice ("band" + n + "Type", "Band " + n + " Type", typeNames, (int) start.type);
        p.channel = addChoice ("band" + n + "Channel", "Band " + n + " Channel", Equalizer::channelNames(), (int) start.channel);
        p.frequency = addFloat ("band" + n + "Frequency", "Band " + n + " Frequency", hz, (float) start.frequency, "Hz");
        p.gain = addFloat ("band" + n + "Gain", "Band " + n + " Gain", db, (float) start.gainDb, "dB");
        p.q = addFloat ("band" + n + "Q", "Band " + n + " Q", q, (float) start.q);
    }

    linearPhase = addBool ("linearPhase", "Linear Phase", false);
}

Equalizer::Band ParametricEqProcessor::bandFromParameters (int index) const
{
    const auto& p = bandParameters[(size_t) index];
    return { (Equalizer::BandType) p.type->getIndex(), (double) p.frequency->get(), (double) p.gain->get(), (double) p.q->get(),
             (Equalizer::Channel) p.channel->getIndex() };
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
    p.channel->setValueNotifyingHost (p.channel->convertTo0to1 ((float) (int) band.channel));
    setExactly (*p.frequency, (float) band.frequency);
    setExactly (*p.gain, (float) band.gainDb);
    setExactly (*p.q, (float) band.q);
}

void ParametricEqProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    reset();
}

void ParametricEqProcessor::reset()
{
    for (auto& f : stagesSetFor)
        f.reset();

    linear.reset();
}

void ParametricEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());

    if (channels != channelsRunning)
    {
        channelsRunning = channels;
        reset();
        linearRunning = false;
    }

    if (linearPhase->get())
    {
        // The impulse for the bands as they are now, remade when they change; with no bands, the
        // audio is only delayed, in step with the latency.
        if (auto bands = getActiveBands (rate); ! linearRunning || bands != linearSetFor)
        {
            linear.prepare (bands, rate, channels);
            linearSetFor = std::move (bands);
            linearRunning = true;
            setLatencySamples (Equalizer::LinearPhase::latency);
            reset();
        }

        linear.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
        return;
    }

    if (linearRunning)
    {
        linearRunning = false;
        setLatencySamples (0);
        reset();
    }

    for (int i = 0; i < numBands; ++i)
    {
        const auto band = bandFromParameters (i);
        auto& set = stagesSetFor[(size_t) i];
        auto& stage = stages[(size_t) i];

        if (! bandParameters[(size_t) i].on->get() || Equalizer::isFlat (band) || ! Equalizer::isValid (band, rate))
        {
            set.reset();
            continue;
        }

        if (! set.has_value())
        {
            stage.set (band, rate, channels);
            stage.reset();   // a band coming back starts from silence
            set = band;
        }
        else if (*set != band)
        {
            stage.set (band, rate, channels);
            set = band;
        }

        stage.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
    }
}

juce::AudioProcessorEditor* ParametricEqProcessor::createEditor()
{
    return new ParametricEqEditor (*this);
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

EffectPresets::Values eqPresetValues (const ParametricEq::Settings& s, bool linearPhase)
{
    EffectPresets::Values v { { "linearPhase", linearPhase ? 1.0 : 0.0 } };

    for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
    {
        const auto n = "band" + juce::String (i + 1);
        const auto& b = s[(size_t) i];
        v[n + "On"] = b.on ? 1.0 : 0.0;
        v[n + "Type"] = (double) (int) b.band.type;
        v[n + "Channel"] = (double) (int) b.band.channel;
        v[n + "Frequency"] = b.band.frequency;
        v[n + "Gain"] = b.band.gainDb;
        v[n + "Q"] = b.band.q;
    }

    return v;
}

bool eqLinearPhaseFrom (const EffectPresets::Values& v, bool start)
{
    const auto f = v.find ("linearPhase");
    return f != v.end() && std::isfinite (f->second) ? f->second != 0.0 : start;
}

ParametricEq::Settings eqSettingsFrom (const EffectPresets::Values& v, ParametricEq::Settings s)
{
    const auto take = [&v] (const juce::String& id, double& into)
    {
        if (const auto f = v.find (id); f != v.end() && std::isfinite (f->second))
            into = f->second;
    };

    for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
    {
        const auto n = "band" + juce::String (i + 1);
        auto& b = s[(size_t) i];
        auto on = b.on ? 1.0 : 0.0, type = (double) (int) b.band.type, channel = (double) (int) b.band.channel;
        take (n + "On", on);
        take (n + "Type", type);
        take (n + "Channel", channel);
        take (n + "Frequency", b.band.frequency);
        take (n + "Gain", b.band.gainDb);
        take (n + "Q", b.band.q);
        b.on = on != 0.0;
        b.band.type = (Equalizer::BandType) juce::jlimit (0, typeNames.size() - 1, (int) std::lround (type));
        b.band.channel = (Equalizer::Channel) juce::jlimit (0, Equalizer::channelNames().size() - 1, (int) std::lround (channel));
    }

    return s;
}

ParametricEqEditor::ParametricEqEditor (ParametricEqProcessor& owner)
    : AudioProcessorEditor (owner), eq (owner),
      presets (EffectPresets::userStore(), owner.getPresetKey(), [&owner] { return owner.getPresetValues(); },
               [this, &owner] (const EffectPresets::Values& v)
               {
                   owner.setPresetValues (v);
                   refresh();
               })
{
    const auto rate = owner.getSampleRate() > 0.0 ? owner.getSampleRate() : 48000.0;
    shown = settingsOf (eq);
    shownLinear = eq.linearPhaseParameter().get();
    panel = std::make_unique<ParametricEq> (shown, rate);
    panel->setLinearPhase (shownLinear);
    panel->onChange = [this] { panelChanged(); };
    addAndMakeVisible (*panel);
    addAndMakeVisible (presets);
    setResizable (true, false);
    setResizeLimits (520, 340, 3000, 2040);
    setSize (panel->getWidth(), panel->getHeight() + 40);
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

    if (panel->isLinearPhase() != shownLinear)
    {
        auto& p = eq.linearPhaseParameter();
        p.beginChangeGesture();
        p.setValueNotifyingHost (panel->isLinearPhase() ? 1.0f : 0.0f);
        p.endChangeGesture();
        shownLinear = panel->isLinearPhase();
    }

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
    if (const auto linearNow = eq.linearPhaseParameter().get(); linearNow != shownLinear)
    {
        shownLinear = linearNow;
        panel->setLinearPhase (linearNow);
    }

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
    auto r = getLocalBounds();
    presets.setBounds (r.removeFromTop (40).reduced (8, 6));
    panel->setBounds (r);
}

//==============================================================================
CompressorProcessor::CompressorProcessor()
    : BuiltInEffect (compressorDescription(), "SPECTRABLADE_COMPRESSOR", true)
{
    const Dynamics::Compressor start;
    // The offline Compressor's ranges, so any setting it takes can be heard live.
    threshold = addFloat ("threshold", "Threshold", { -80.0f, 0.0f }, (float) start.thresholdDb, "dB");
    ratio = addFloat ("ratio", "Ratio", { 1.0f, 100.0f, 0.0f, 0.25f }, (float) start.ratio, ":1");
    knee = addFloat ("knee", "Knee", { 0.0f, 24.0f }, (float) start.kneeDb, "dB");
    attack = addFloat ("attack", "Attack", { 0.01f, 1000.0f, 0.0f, 0.2f }, (float) start.attackMs, "ms");
    release = addFloat ("release", "Release", { 1.0f, 5000.0f, 0.0f, 0.25f }, (float) start.releaseMs, "ms");
    makeup = addFloat ("makeup", "Makeup Gain", { -48.0f, 48.0f }, (float) start.makeupDb, "dB");
    sidechain = addBool ("sidechain", "Sidechain", false);
}

Dynamics::Compressor CompressorProcessor::getSettings() const
{
    return { (double) threshold->get(), (double) ratio->get(), (double) knee->get(), (double) attack->get(), (double) release->get(), (double) makeup->get() };
}

void CompressorProcessor::setSettings (const Dynamics::Compressor& s)
{
    setExactly (*threshold, (float) s.thresholdDb);
    setExactly (*ratio, (float) s.ratio);
    setExactly (*knee, (float) s.kneeDb);
    setExactly (*attack, (float) s.attackMs);
    setExactly (*release, (float) s.releaseMs);
    setExactly (*makeup, (float) s.makeupDb);
}

void CompressorProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    applied = getSettings();
    core.set (applied, rate);
    core.reset();
}

void CompressorProcessor::refreshSettings()
{
    const auto now = getSettings();

    if (! juce::exactlyEqual (now.thresholdDb, applied.thresholdDb) || ! juce::exactlyEqual (now.ratio, applied.ratio)
        || ! juce::exactlyEqual (now.kneeDb, applied.kneeDb) || ! juce::exactlyEqual (now.attackMs, applied.attackMs)
        || ! juce::exactlyEqual (now.releaseMs, applied.releaseMs) || ! juce::exactlyEqual (now.makeupDb, applied.makeupDb))
    {
        applied = now;
        core.set (applied, rate);
    }
}

void CompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    refreshSettings();
    const auto channels = juce::jmin (buffer.getNumChannels(), getChannelCountOfBus (true, 0));

    // The level comes from the sidechain input when it is switched on (silence when nothing feeds it).
    const auto keyed = sidechain->get() && getBusCount (true) > 1 && getChannelCountOfBus (true, 1) > 0;
    const auto firstKey = keyed ? getChannelIndexInProcessBlockBuffer (true, 1, 0) : 0;
    const auto keyChannels = keyed ? juce::jmin (getChannelCountOfBus (true, 1), buffer.getNumChannels() - firstKey) : channels;
    compress (buffer, channels, firstKey, keyChannels);
}

bool CompressorProcessor::processLinked (juce::AudioBuffer<float>& audio)
{
    if (sidechain->get() || audio.getNumChannels() > mostLinkedChannels)
        return false;

    refreshSettings();
    compress (audio, audio.getNumChannels(), 0, audio.getNumChannels());
    return true;
}

void CompressorProcessor::compress (juce::AudioBuffer<float>& buffer, int channels, int firstKey, int keyChannels)
{
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        double level = 0.0;

        for (int ch = firstKey; ch < firstKey + keyChannels; ++ch)
            level = juce::jmax (level, (double) std::abs (buffer.getSample (ch, i)));

        const auto factor = core.next (level);

        for (int ch = 0; ch < channels; ++ch)
            buffer.setSample (ch, i, (float) (buffer.getSample (ch, i) * factor));
    }
}

//==============================================================================
LimiterProcessor::LimiterProcessor (double lookahead)
    : BuiltInEffect (limiterDescription(), "SPECTRABLADE_LIMITER"), lookaheadMs (juce::jlimit (0.1, 50.0, lookahead))
{
    // The offline Limiter's ranges.
    const Dynamics::Limiter start;
    gain = addFloat ("gain", "Gain", { -48.0f, 48.0f }, (float) start.inputGainDb, "dB");
    ceiling = addFloat ("ceiling", "Ceiling", { -48.0f, 0.0f }, (float) start.ceilingDb, "dBFS");
    release = addFloat ("release", "Release", { 1.0f, 5000.0f, 0.0f, 0.25f }, (float) start.releaseMs, "ms");
}

Dynamics::Limiter LimiterProcessor::getSettings() const
{
    return { (double) gain->get(), (double) ceiling->get(), (double) release->get(), lookaheadMs, false };
}

void LimiterProcessor::setSettings (const Dynamics::Limiter& s)
{
    setExactly (*gain, (float) s.inputGainDb);
    setExactly (*ceiling, (float) s.ceilingDb);
    setExactly (*release, (float) s.releaseMs);
}

void LimiterProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    const auto ahead = Dynamics::lookaheadFrames (getSettings(), rate);
    setLatencySamples (ahead);
    delay.setSize (juce::jmax (mostLinkedChannels, getTotalNumInputChannels()), ahead);
    reset();
}

void LimiterProcessor::reset()
{
    const auto ahead = Dynamics::lookaheadFrames (getSettings(), rate);
    core.prepare (ahead, Dynamics::smoothingCoefficient ((double) release->get(), rate));
    delay.clear();
    delayPosition = 0;
}

void LimiterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    limit (buffer, juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels()));
}

bool LimiterProcessor::processLinked (juce::AudioBuffer<float>& audio)
{
    if (audio.getNumChannels() > delay.getNumChannels())
        return false;

    limit (audio, audio.getNumChannels());
    return true;
}

void LimiterProcessor::limit (juce::AudioBuffer<float>& buffer, int channels)
{
    const auto settings = getSettings();
    const auto input = std::pow (10.0, settings.inputGainDb / 20.0);
    const auto top = Dynamics::ceilingGain (settings.ceilingDb);
    const auto unity = juce::exactlyEqual (settings.inputGainDb, 0.0);
    core.setRelease (Dynamics::smoothingCoefficient (settings.releaseMs, rate));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        double peak = 0.0;

        for (int ch = 0; ch < channels; ++ch)
            peak = juce::jmax (peak, (double) std::abs (buffer.getSample (ch, i)));

        const auto frameGain = core.push (Dynamics::limiterNeed (peak, input, top));

        // Out goes the frame a look-ahead old (silence until one has come in), in the new one.
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto old = delay.getSample (ch, delayPosition);
            delay.setSample (ch, delayPosition, buffer.getSample (ch, i));

            if (! frameGain.has_value() || (unity && juce::exactlyEqual (*frameGain, 1.0)))
                buffer.setSample (ch, i, old);
            else
                buffer.setSample (ch, i, (float) juce::jlimit (-top, top, (double) old * input * *frameGain));
        }

        delayPosition = (delayPosition + 1) % delay.getNumSamples();
    }
}

//==============================================================================
SweptDelayProcessor::SweptDelayProcessor (Kind kind)
    : BuiltInEffect (kind == Kind::chorus ? chorusDescription() : flangerDescription(),
                     kind == Kind::chorus ? "SPECTRABLADE_CHORUS" : "SPECTRABLADE_FLANGER")
{
    // Modulation::Delay's ranges; each kind starts at its own defaults.
    const auto start = kind == Kind::chorus ? Modulation::chorusDefaults() : Modulation::flangerDefaults();
    rate = addFloat ("rate", "Rate", { 0.01f, 10.0f, 0.0f, 0.3f }, (float) start.rateHz, "Hz");
    delay = addFloat ("delay", "Delay", { 0.0f, 50.0f, 0.0f, 0.4f }, (float) start.delayMs, "ms");
    depth = addFloat ("depth", "Depth", { 0.0f, 50.0f, 0.0f, 0.4f }, (float) start.depthMs, "ms");
    feedback = addFloat ("feedback", "Feedback", { -0.95f, 0.95f }, (float) start.feedback);
    mix = addFloat ("mix", "Mix", { 0.0f, 100.0f }, (float) start.mixPercent, "%");
    spread = addFloat ("spread", "Stereo Spread", { 0.0f, 180.0f }, (float) start.spreadDegrees, "deg");
}

Modulation::Delay SweptDelayProcessor::getSettings() const
{
    return { (double) rate->get(), (double) delay->get(), (double) depth->get(), (double) feedback->get(), (double) mix->get(), (double) spread->get() };
}

void SweptDelayProcessor::setSettings (const Modulation::Delay& s)
{
    setExactly (*rate, (float) s.rateHz);
    setExactly (*delay, (float) s.delayMs);
    setExactly (*depth, (float) s.depthMs);
    setExactly (*feedback, (float) s.feedback);
    setExactly (*mix, (float) s.mixPercent);
    setExactly (*spread, (float) s.spreadDegrees);
}

void SweptDelayProcessor::prepareToPlay (double sampleRate, int)
{
    core.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
}

void SweptDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    core.set (getSettings());
    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels()), buffer.getNumSamples());
}

//==============================================================================
PhaserProcessor::PhaserProcessor()
    : BuiltInEffect (phaserDescription(), "SPECTRABLADE_PHASER")
{
    const Modulation::Phaser start;
    const juce::NormalisableRange<float> hz (20.0f, 20000.0f, 0.0f, 0.2f);
    rate = addFloat ("rate", "Rate", { 0.01f, 10.0f, 0.0f, 0.3f }, (float) start.rateHz, "Hz");
    low = addFloat ("low", "Low Frequency", hz, (float) start.lowHz, "Hz");
    high = addFloat ("high", "High Frequency", hz, (float) start.highHz, "Hz");
    stages = addChoice ("stages", "Stages", stageNames, start.stages / 2 - 1);
    feedback = addFloat ("feedback", "Feedback", { -0.95f, 0.95f }, (float) start.feedback);
    mix = addFloat ("mix", "Mix", { 0.0f, 100.0f }, (float) start.mixPercent, "%");
    spread = addFloat ("spread", "Stereo Spread", { 0.0f, 180.0f }, (float) start.spreadDegrees, "deg");
}

Modulation::Phaser PhaserProcessor::getSettings() const
{
    return { (double) rate->get(), (double) low->get(), (double) high->get(), 2 * (stages->getIndex() + 1),
             (double) feedback->get(), (double) mix->get(), (double) spread->get() };
}

void PhaserProcessor::setSettings (const Modulation::Phaser& s)
{
    setExactly (*rate, (float) s.rateHz);
    setExactly (*low, (float) s.lowHz);
    setExactly (*high, (float) s.highHz);
    stages->setValueNotifyingHost (stages->convertTo0to1 ((float) juce::jlimit (0, stageNames.size() - 1, s.stages / 2 - 1)));
    setExactly (*feedback, (float) s.feedback);
    setExactly (*mix, (float) s.mixPercent);
    setExactly (*spread, (float) s.spreadDegrees);
}

void PhaserProcessor::prepareToPlay (double sampleRate, int)
{
    core.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
}

void PhaserProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    core.set (getSettings());   // a high frequency below the low one sweeps the other way
    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels()), buffer.getNumSamples());
}

//==============================================================================
ReverbProcessor::ReverbProcessor()
    : BuiltInEffect (reverbDescription(), "SPECTRABLADE_REVERB")
{
    const RoomReverb::Settings start;
    preDelay = addFloat ("preDelay", "Pre-Delay", { 0.0f, 200.0f, 0.0f, 0.5f }, (float) start.preDelayMs, "ms");
    decay = addFloat ("decay", "Decay", { 0.1f, 20.0f, 0.0f, 0.35f }, (float) start.decaySeconds, "s");
    size = addFloat ("size", "Size", { 50.0f, 150.0f }, (float) start.sizePercent, "%");
    damping = addFloat ("damping", "Damping", { 0.0f, 100.0f }, (float) start.dampingPercent, "%");
    width = addFloat ("width", "Width", { 0.0f, 100.0f }, (float) start.widthPercent, "%");
    mix = addFloat ("mix", "Mix", { 0.0f, 100.0f }, (float) start.mixPercent, "%");
}

RoomReverb::Settings ReverbProcessor::getSettings() const
{
    return { (double) preDelay->get(), (double) decay->get(), (double) size->get(), (double) damping->get(), (double) width->get(), (double) mix->get() };
}

void ReverbProcessor::setSettings (const RoomReverb::Settings& s)
{
    setExactly (*preDelay, (float) s.preDelayMs);
    setExactly (*decay, (float) s.decaySeconds);
    setExactly (*size, (float) s.sizePercent);
    setExactly (*damping, (float) s.dampingPercent);
    setExactly (*width, (float) s.widthPercent);
    setExactly (*mix, (float) s.mixPercent);
}

void ReverbProcessor::prepareToPlay (double sampleRate, int)
{
    core.prepare (sampleRate);
    applied = getSettings();
    core.set (applied);
}

void ReverbProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Set again only on a change: setting works out the lines' lengths and losses.
    const auto now = getSettings();

    if (! juce::exactlyEqual (now.preDelayMs, applied.preDelayMs) || ! juce::exactlyEqual (now.decaySeconds, applied.decaySeconds)
        || ! juce::exactlyEqual (now.sizePercent, applied.sizePercent) || ! juce::exactlyEqual (now.dampingPercent, applied.dampingPercent)
        || ! juce::exactlyEqual (now.widthPercent, applied.widthPercent) || ! juce::exactlyEqual (now.mixPercent, applied.mixPercent))
    {
        applied = now;
        core.set (applied);
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels()), buffer.getNumSamples());
}

//==============================================================================
ConvolutionProcessor::ConvolutionProcessor()
    : BuiltInEffect (convolutionDescription(), "SPECTRABLADE_CONVOLUTION_REVERB")
{
    mix = addFloat ("mix", "Mix", { 0.0f, 100.0f }, 30.0f, "%");
    rebuild();
}

ConvolutionProcessor::~ConvolutionProcessor() = default;

void ConvolutionProcessor::setImpulse (Convolver::Impulse i)
{
    impulse = std::move (i);
    rebuild();
}

void ConvolutionProcessor::rebuild()
{
    // Built here, then swapped in, so the audio thread waits only for the swap.
    auto next = std::make_unique<Convolver::Core>();
    const auto usable = Convolver::problemWith (impulse).isEmpty();
    next->prepare (usable ? Convolver::atRate (impulse, rate) : juce::AudioBuffer<float>(), juce::jmax (1, getTotalNumInputChannels()));
    setLatencySamples (Convolver::blockSize);

    {
        const juce::SpinLock::ScopedLockType sl (coreLock);
        std::swap (core, next);
        coreHasImpulse = usable;
    }
}

void ConvolutionProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    rebuild();
}

void ConvolutionProcessor::reset()
{
    const juce::SpinLock::ScopedLockType sl (coreLock);
    core->reset();
}

void ConvolutionProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::SpinLock::ScopedLockType sl (coreLock);
    core->setMix (coreHasImpulse ? (double) mix->get() : 0.0);
    core->process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels()), buffer.getNumSamples());
}

double ConvolutionProcessor::getTailLengthSeconds() const
{
    return impulse.sampleRate > 0.0 ? impulse.samples.getNumSamples() / impulse.sampleRate : 0.0;
}

void ConvolutionProcessor::addToState (juce::XmlElement& xml) const
{
    // The impulse's samples as they were read, each channel's floats in base 64.
    if (Convolver::problemWith (impulse).isNotEmpty())
        return;

    auto* e = xml.createNewChildElement ("IMPULSE");
    e->setAttribute ("name", impulse.name);
    e->setAttribute ("rate", impulse.sampleRate);
    e->setAttribute ("frames", impulse.samples.getNumSamples());

    for (int ch = 0; ch < impulse.samples.getNumChannels(); ++ch)
        e->createNewChildElement ("CHANNEL")->addTextElement (
            juce::MemoryBlock (impulse.samples.getReadPointer (ch), (size_t) impulse.samples.getNumSamples() * sizeof (float)).toBase64Encoding());
}

void ConvolutionProcessor::readFromState (const juce::XmlElement& xml)
{
    Convolver::Impulse i;

    if (auto* e = xml.getChildByName ("IMPULSE"))
    {
        const auto frames = e->getIntAttribute ("frames");
        const auto count = e->getNumChildElements();
        i.name = e->getStringAttribute ("name");
        i.sampleRate = e->getDoubleAttribute ("rate");

        if (frames > 0 && count >= 1 && count <= 2)
        {
            i.samples.setSize (count, frames);

            for (int ch = 0; ch < count; ++ch)
            {
                juce::MemoryBlock block;

                if (! block.fromBase64Encoding (e->getChildElement (ch)->getAllSubText()) || block.getSize() != (size_t) frames * sizeof (float))
                    return;

                std::memcpy (i.samples.getWritePointer (ch), block.getData(), block.getSize());
            }
        }
    }

    if (Convolver::problemWith (i).isEmpty())
        setImpulse (std::move (i));
}

juce::AudioProcessorEditor* ConvolutionProcessor::createEditor()
{
    return new ConvolutionEditor (*this);
}

//==============================================================================
ConvolutionEditor::ConvolutionEditor (ConvolutionProcessor& owner)
    : AudioProcessorEditor (owner), convolution (owner),
      presets (EffectPresets::userStore(), owner.getPresetKey(), [&owner] { return owner.getPresetValues(); },
               [&owner] (const EffectPresets::Values& v) { owner.setPresetValues (v); })
{
    addAndMakeVisible (presets);
    addAndMakeVisible (impulseName);
    addAndMakeVisible (load);
    addAndMakeVisible (mix);
    addAndMakeVisible (mixLabel);
    mix.setTextValueSuffix (" %");
    mixAttachment = std::make_unique<juce::SliderParameterAttachment> (*owner.mix, mix);
    mix.setNumDecimalPlacesToDisplay (1);
    load.onClick = [this] { chooseImpulse(); };
    showImpulse();
    setSize (420, 150);
}

ConvolutionEditor::~ConvolutionEditor() = default;

void ConvolutionEditor::showImpulse()
{
    const auto& i = convolution.getImpulse();
    impulseName.setText (Convolver::problemWith (i).isEmpty()
                             ? i.name + " (" + juce::String (i.samples.getNumSamples() / i.sampleRate, 2) + " s, "
                                   + (i.samples.getNumChannels() == 1 ? "mono" : "stereo") + ")"
                             : juce::String ("No impulse response: the sound passes through."),
                         juce::dontSendNotification);
}

void ConvolutionEditor::chooseImpulse()
{
    chooser = std::make_unique<juce::FileChooser> ("Load impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<ConvolutionEditor> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || ! fc.getResult().existsAsFile())
                                  return;

                              juce::AudioFormatManager formats;
                              AudioFileIO::registerFormats (formats);
                              juce::String error;
                              auto impulse = Convolver::read (fc.getResult(), formats, error);

                              if (error.isNotEmpty())
                              {
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Load Impulse", error);
                                  return;
                              }

                              safe->convolution.setImpulse (std::move (impulse));
                              safe->showImpulse();
                          });
}

void ConvolutionEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void ConvolutionEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    presets.setBounds (r.removeFromTop (28));
    r.removeFromTop (12);
    auto top = r.removeFromTop (28);
    load.setBounds (top.removeFromRight (130));
    impulseName.setBounds (top);
    r.removeFromTop (16);
    auto row = r.removeFromTop (28);
    mixLabel.setBounds (row.removeFromLeft (50));
    mix.setBounds (row);
}

//==============================================================================
GraphicEqProcessor::GraphicEqProcessor (int numBands)
    : BuiltInEffect (graphicEqDescription (numBands), numBands == 31 ? "SPECTRABLADE_GRAPHIC_EQ_31" : "SPECTRABLADE_GRAPHIC_EQ_10")
{
    const auto& centres = GraphicEq::centres (numBands == 31 ? 31 : 10);
    const auto g = (float) GraphicEq::maxGainDb;

    for (size_t i = 0; i < centres.size(); ++i)
        gains.push_back (addFloat ("band" + juce::String ((int) i + 1), juce::String (centres[i]) + " Hz", { -g, g }, 0.0f, "dB"));

    filters.resize (centres.size());
    filtersSetFor.resize (centres.size());
}

std::vector<double> GraphicEqProcessor::getGains() const
{
    std::vector<double> g;

    for (auto* p : gains)
        g.push_back ((double) p->get());

    return g;
}

void GraphicEqProcessor::setGains (const std::vector<double>& g)
{
    for (size_t i = 0; i < gains.size() && i < g.size(); ++i)
        setExactly (*gains[i], (float) g[i]);
}

void GraphicEqProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    reset();
}

void GraphicEqProcessor::reset()
{
    for (auto& f : filtersSetFor)
        f.reset();
}

void GraphicEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    const auto& centres = GraphicEq::centres (getNumBands());
    const auto q = GraphicEq::bandQ (getNumBands());

    for (size_t i = 0; i < gains.size(); ++i)
    {
        const Equalizer::Band band { Equalizer::BandType::peak, centres[i], (double) gains[i]->get(), q };
        auto& set = filtersSetFor[i];
        auto& bandFilters = filters[i];

        if (Equalizer::isFlat (band) || ! Equalizer::isValid (band, rate))
        {
            set.reset();
            continue;
        }

        if (! set.has_value() || (int) bandFilters.size() != channels)
        {
            bandFilters.assign ((size_t) channels, {});   // a band coming back starts from silence
            set.reset();
        }

        if (! set.has_value() || ! juce::exactlyEqual (*set, band.gainDb))
        {
            for (auto& f : bandFilters)
                f.setBand (band, rate);

            set = band.gainDb;
        }

        for (int ch = 0; ch < channels; ++ch)
            bandFilters[(size_t) ch].process (buffer.getWritePointer (ch), buffer.getNumSamples());
    }
}

juce::AudioProcessorEditor* GraphicEqProcessor::createEditor()
{
    return new GraphicEqEditor (*this);
}

//==============================================================================
GraphicEqEditor::GraphicEqEditor (GraphicEqProcessor& owner)
    : AudioProcessorEditor (owner), eq (owner),
      presets (EffectPresets::userStore(), owner.getPresetKey(), [&owner] { return owner.getPresetValues(); },
               [this, &owner] (const EffectPresets::Values& v)
               {
                   owner.setPresetValues (v);
                   refresh();
               }),
      panel (owner.getNumBands())
{
    shown = eq.getGains();
    panel.setGains (shown);
    panel.onChange = [this] { panelChanged(); };
    addAndMakeVisible (presets);
    addAndMakeVisible (panel);
    setSize (juce::jmax (420, panel.getWidth()), panel.getHeight() + 40);
    startTimerHz (10);
}

GraphicEqEditor::~GraphicEqEditor()
{
    stopTimer();
}

void GraphicEqEditor::panelChanged()
{
    // Only the bands moved, each as one gesture, so automation writing records just those.
    const auto wanted = panel.getGains();

    for (size_t i = 0; i < wanted.size() && i < eq.gains.size(); ++i)
    {
        if (juce::exactlyEqual (wanted[i], shown[i]))
            continue;

        eq.gains[i]->beginChangeGesture();
        setExactly (*eq.gains[i], (float) wanted[i]);
        eq.gains[i]->endChangeGesture();
    }

    shown = eq.getGains();
}

void GraphicEqEditor::refresh()
{
    const auto now = eq.getGains();

    if (now == shown)
        return;

    shown = now;
    panel.setGains (now);
}

void GraphicEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void GraphicEqEditor::resized()
{
    auto r = getLocalBounds();
    presets.setBounds (r.removeFromTop (40).reduced (8, 6));
    panel.setBounds (r);
}

//==============================================================================
const std::array<const char*, 3> MultibandProcessor::bandNames { "low", "mid", "high" };

MultibandProcessor::MultibandProcessor()
    : BuiltInEffect (multibandDescription(), "SPECTRABLADE_MULTIBAND")
{
    const Multiband::Compressor start;
    const std::array<const char*, 3> titles { "Low", "Mid", "High" };
    lowCrossover = addFloat ("lowCrossover", "Low Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.lowCrossover, "Hz");
    highCrossover = addFloat ("highCrossover", "High Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.highCrossover, "Hz");

    for (size_t b = 0; b < 3; ++b)
    {
        const juce::String id (bandNames[b]), name (titles[b]);
        const auto& s = start.bands[b];
        bands[b].threshold = addFloat (id + "Threshold", name + " Threshold", { -80.0f, 0.0f }, (float) s.thresholdDb, "dB");
        bands[b].ratio = addFloat (id + "Ratio", name + " Ratio", { 1.0f, 100.0f, 0.0f, 0.25f }, (float) s.ratio, ":1");
        bands[b].attack = addFloat (id + "Attack", name + " Attack", { 0.01f, 1000.0f, 0.0f, 0.2f }, (float) s.attackMs, "ms");
        bands[b].release = addFloat (id + "Release", name + " Release", { 1.0f, 5000.0f, 0.0f, 0.25f }, (float) s.releaseMs, "ms");
        bands[b].makeup = addFloat (id + "Makeup", name + " Makeup", { -48.0f, 48.0f }, (float) s.makeupDb, "dB");
    }

    output = addFloat ("output", "Output", { -24.0f, 24.0f }, (float) start.outputDb, "dB");
    linearPhase = addBool ("linearPhase", "Linear Phase", start.linearPhase);
}

std::vector<juce::AudioParameterFloat*> MultibandProcessor::parameterOrder() const
{
    std::vector<juce::AudioParameterFloat*> order { lowCrossover, highCrossover, output };

    for (auto& b : bands)
        order.insert (order.end(), { b.threshold, b.ratio, b.attack, b.release, b.makeup });

    return order;
}

std::vector<float> MultibandProcessor::valuesOf (const Multiband::Compressor& s)
{
    std::vector<float> v { (float) s.lowCrossover, (float) s.highCrossover, (float) s.outputDb };

    for (auto& b : s.bands)
        v.insert (v.end(), { (float) b.thresholdDb, (float) b.ratio, (float) b.attackMs, (float) b.releaseMs, (float) b.makeupDb });

    v.push_back (s.linearPhase ? 1.0f : 0.0f);
    return v;
}

Multiband::Compressor MultibandProcessor::getParameterSettings() const
{
    Multiband::Compressor s;
    s.lowCrossover = lowCrossover->get();
    s.highCrossover = highCrossover->get();
    s.outputDb = output->get();
    s.linearPhase = linearPhase->get();

    for (size_t b = 0; b < 3; ++b)
        s.bands[b] = { (double) bands[b].threshold->get(), (double) bands[b].ratio->get(), Dynamics::Compressor().kneeDb,
                       (double) bands[b].attack->get(), (double) bands[b].release->get(), (double) bands[b].makeup->get() };

    return s;
}

Multiband::Compressor MultibandProcessor::getSettings() const
{
    return Multiband::spaced (getParameterSettings(), rate);
}

void MultibandProcessor::setSettings (const Multiband::Compressor& s)
{
    const auto order = parameterOrder();
    const auto v = valuesOf (s);

    for (size_t i = 0; i < order.size(); ++i)
        setExactly (*order[i], v[i]);

    *linearPhase = s.linearPhase;
}

juce::AudioProcessorEditor* MultibandProcessor::createEditor()
{
    return new MultibandEditor (*this);
}

void MultibandProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = valuesOf (getParameterSettings());
    core.set (getSettings());
    core.reset();
    setLatencySamples (core.getLatency());
}

void MultibandProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (auto now = valuesOf (getParameterSettings()); now != applied)
    {
        applied = now;
        core.set (getSettings());

        if (core.getLatency() != getLatencySamples())
            setLatencySamples (core.getLatency());
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
    const auto now = core.getReductionDb();

    for (size_t b = 0; b < 3; ++b)
        reductionDb[b] = now[b];
}

std::array<double, 3> MultibandProcessor::getReductionDb() const noexcept
{
    return { reductionDb[0].load(), reductionDb[1].load(), reductionDb[2].load() };
}

void MultibandProcessor::reset()
{
    core.reset();

    for (auto& r : reductionDb)
        r = 0.0;
}

//==============================================================================
MultibandEditor::MultibandEditor (MultibandProcessor& owner)
    : AudioProcessorEditor (owner), effect (owner),
      presets (EffectPresets::userStore(), owner.getPresetKey(), [&owner] { return owner.getPresetValues(); },
               [this, &owner] (const EffectPresets::Values& v)
               {
                   owner.setPresetValues (v);
                   refresh();
               })
{
    shown = MultibandProcessor::valuesOf (effect.getParameterSettings());
    panel.setSettings (effect.getParameterSettings());
    panelValues = MultibandProcessor::valuesOf (panel.getSettings());
    panel.onChange = [this] { panelChanged(); };
    addAndMakeVisible (presets);
    addAndMakeVisible (panel);
    setSize (panel.getWidth(), panel.getHeight() + 40);
    startTimerHz (20);
}

MultibandEditor::~MultibandEditor()
{
    stopTimer();
}

void MultibandEditor::panelChanged()
{
    // Only the sliders that moved, each as one gesture, so automation writing records just those.
    const auto order = effect.parameterOrder();
    const auto wanted = MultibandProcessor::valuesOf (panel.getSettings());

    for (size_t i = 0; i < order.size(); ++i)
    {
        if (juce::exactlyEqual (wanted[i], panelValues[i]))
            continue;

        order[i]->beginChangeGesture();
        setExactly (*order[i], wanted[i]);
        order[i]->endChangeGesture();
    }

    if (! juce::exactlyEqual (wanted.back(), panelValues.back()))
    {
        auto& p = effect.linearPhaseParameter();
        p.beginChangeGesture();
        p.setValueNotifyingHost (wanted.back());
        p.endChangeGesture();
    }

    panelValues = wanted;
    shown = MultibandProcessor::valuesOf (effect.getParameterSettings());
}

void MultibandEditor::refresh()
{
    panel.setReductionDb (effect.getReductionDb());

    if (auto now = MultibandProcessor::valuesOf (effect.getParameterSettings()); now != shown)
    {
        shown = std::move (now);
        panel.setSettings (effect.getParameterSettings());
        panelValues = MultibandProcessor::valuesOf (panel.getSettings());
    }
}

void MultibandEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MultibandEditor::resized()
{
    auto r = getLocalBounds();
    presets.setBounds (r.removeFromTop (40).reduced (8, 6));
    panel.setBounds (r);
}

//==============================================================================
DeEsserProcessor::DeEsserProcessor()
    : BuiltInEffect (deEsserDescription(), "SPECTRABLADE_DE_ESSER")
{
    const Multiband::DeEsser start;
    frequency = addFloat ("frequency", "Frequency", { 1000.0f, 16000.0f, 0.0f, 0.4f }, (float) start.frequency, "Hz");
    threshold = addFloat ("threshold", "Threshold", { -60.0f, 0.0f }, (float) start.thresholdDb, "dB");
    reduction = addFloat ("reduction", "Reduction", { 0.0f, 24.0f }, (float) start.maxReductionDb, "dB");
    mode = addChoice ("mode", "Turn Down", { "The high band", "The whole sound" }, 0);   // the high band, as Multiband::DeEsser starts
    linearPhase = addBool ("linearPhase", "Linear Phase", start.linearPhase);
    listen = addBool ("listen", "Listen", false);
}

Multiband::DeEsser DeEsserProcessor::getSettings() const
{
    // Below 0.45 of the rate, as the offline effect needs it under half.
    return { juce::jmin ((double) frequency->get(), 0.45 * rate), (double) threshold->get(), (double) reduction->get(), mode->getIndex() == 1, linearPhase->get() };
}

void DeEsserProcessor::setSettings (const Multiband::DeEsser& s)
{
    setExactly (*frequency, (float) s.frequency);
    setExactly (*threshold, (float) s.thresholdDb);
    setExactly (*reduction, (float) s.maxReductionDb);
    *mode = s.wholeSound ? 1 : 0;
    *linearPhase = s.linearPhase;
}

void DeEsserProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = getSettings();
    core.set (applied);
    core.reset();
    setLatencySamples (core.getLatency());
}

void DeEsserProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto now = getSettings();

    if (! juce::exactlyEqual (now.frequency, applied.frequency) || ! juce::exactlyEqual (now.thresholdDb, applied.thresholdDb)
        || ! juce::exactlyEqual (now.maxReductionDb, applied.maxReductionDb) || now.wholeSound != applied.wholeSound
        || now.linearPhase != applied.linearPhase)
    {
        applied = now;
        core.set (applied);

        if (core.getLatency() != getLatencySamples())
            setLatencySamples (core.getLatency());
    }

    core.setListening (listen->get());
    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
    reductionDb = core.getReductionDb();
}

void DeEsserProcessor::reset()
{
    core.reset();
    reductionDb = 0.0;
}

juce::AudioProcessorEditor* DeEsserProcessor::createEditor()
{
    return new PresetsAndParameters (*this, [this] { return getReductionDb(); });
}

//==============================================================================
MultibandWidthProcessor::MultibandWidthProcessor()
    : BuiltInEffect (multibandWidthDescription(), "SPECTRABLADE_MULTIBAND_WIDTH")
{
    const Multiband::Width start;
    const std::array<const char*, 3> ids { "low", "mid", "high" }, names { "Low Width", "Mid Width", "High Width" };
    lowCrossover = addFloat ("lowCrossover", "Low Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.lowCrossover, "Hz");
    highCrossover = addFloat ("highCrossover", "High Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.highCrossover, "Hz");

    for (size_t b = 0; b < 3; ++b)
        widths[b] = addFloat (ids[b], names[b], { 0.0f, 200.0f }, (float) start.percent[b], "%");

    linearPhase = addBool ("linearPhase", "Linear Phase", start.linearPhase);
}

Multiband::Width MultibandWidthProcessor::getParameterSettings() const
{
    return { (double) lowCrossover->get(), (double) highCrossover->get(), { (double) widths[0]->get(), (double) widths[1]->get(), (double) widths[2]->get() },
             linearPhase->get() };
}

Multiband::Width MultibandWidthProcessor::getSettings() const
{
    return Multiband::spaced (getParameterSettings(), rate);
}

void MultibandWidthProcessor::setSettings (const Multiband::Width& w)
{
    setExactly (*lowCrossover, (float) w.lowCrossover);
    setExactly (*highCrossover, (float) w.highCrossover);

    for (size_t b = 0; b < 3; ++b)
        setExactly (*widths[b], (float) w.percent[b]);

    *linearPhase = w.linearPhase;
}

juce::AudioProcessorEditor* MultibandWidthProcessor::createEditor()
{
    return new PresetsAndParameters (*this);
}

void MultibandWidthProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = { lowCrossover->get(), highCrossover->get(), widths[0]->get(), widths[1]->get(), widths[2]->get(), linearPhase->get() ? 1.0f : 0.0f };
    core.set (getSettings());
    core.reset();
    setLatencySamples (core.getLatency());
}

void MultibandWidthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (const std::array<float, 6> now { lowCrossover->get(), highCrossover->get(), widths[0]->get(), widths[1]->get(), widths[2]->get(),
                                         linearPhase->get() ? 1.0f : 0.0f }; now != applied)
    {
        applied = now;
        core.set (getSettings());

        if (core.getLatency() != getLatencySamples())
            setLatencySamples (core.getLatency());
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
}

void MultibandWidthProcessor::reset()
{
    core.reset();
}

//==============================================================================
ExciterProcessor::ExciterProcessor()
    : BuiltInEffect (exciterDescription(), "SPECTRABLADE_EXCITER")
{
    const Exciter::Settings start;
    const std::array<const char*, 3> ids { "low", "mid", "high" }, names { "Low", "Mid", "High" };
    lowCrossover = addFloat ("lowCrossover", "Low Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.lowCrossover, "Hz");
    highCrossover = addFloat ("highCrossover", "High Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.highCrossover, "Hz");

    for (size_t b = 0; b < 3; ++b)
    {
        const juce::String id (ids[b]), name (names[b]);
        bands[b].drive = addFloat (id + "Drive", name + " Drive", { 0.0f, 36.0f }, (float) start.bands[b].driveDb, "dB");
        bands[b].curve = addChoice (id + "Curve", name + " Curve", Exciter::curveNames(), (int) start.bands[b].curve);
        bands[b].mix = addFloat (id + "Mix", name + " Mix", { 0.0f, 100.0f }, (float) start.bands[b].mixPercent, "%");
    }

    output = addFloat ("output", "Output", { -24.0f, 24.0f }, (float) start.outputDb, "dB");
    linearPhase = addBool ("linearPhase", "Linear Phase", start.linearPhase);
}

std::vector<float> ExciterProcessor::valuesOfParameters() const
{
    std::vector<float> v { lowCrossover->get(), highCrossover->get(), output->get() };

    for (auto& b : bands)
        v.insert (v.end(), { b.drive->get(), (float) b.curve->getIndex(), b.mix->get() });

    v.push_back (linearPhase->get() ? 1.0f : 0.0f);
    return v;
}

Exciter::Settings ExciterProcessor::getParameterSettings() const
{
    Exciter::Settings s;
    s.lowCrossover = lowCrossover->get();
    s.highCrossover = highCrossover->get();
    s.outputDb = output->get();

    for (size_t b = 0; b < 3; ++b)
        s.bands[b] = { (double) bands[b].drive->get(), (Exciter::Curve) bands[b].curve->getIndex(), (double) bands[b].mix->get() };

    s.linearPhase = linearPhase->get();
    return s;
}

Exciter::Settings ExciterProcessor::getSettings() const
{
    return Exciter::spaced (getParameterSettings(), rate);
}

void ExciterProcessor::setSettings (const Exciter::Settings& s)
{
    setExactly (*lowCrossover, (float) s.lowCrossover);
    setExactly (*highCrossover, (float) s.highCrossover);
    setExactly (*output, (float) s.outputDb);

    for (size_t b = 0; b < 3; ++b)
    {
        setExactly (*bands[b].drive, (float) s.bands[b].driveDb);
        *bands[b].curve = (int) s.bands[b].curve;
        setExactly (*bands[b].mix, (float) s.bands[b].mixPercent);
    }

    *linearPhase = s.linearPhase;
}

juce::AudioProcessorEditor* ExciterProcessor::createEditor()
{
    return new PresetsAndParameters (*this);
}

void ExciterProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = valuesOfParameters();
    core.set (getSettings());
    core.reset();
    setLatencySamples (core.getLatency());
}

void ExciterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (auto now = valuesOfParameters(); now != applied)
    {
        applied = std::move (now);
        core.set (getSettings());

        if (core.getLatency() != getLatencySamples())
            setLatencySamples (core.getLatency());
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
}

void ExciterProcessor::reset()
{
    core.reset();
}

//==============================================================================
MaximizerProcessor::MaximizerProcessor()
    : BuiltInEffect (maximizerDescription(), "SPECTRABLADE_MAXIMIZER")
{
    const Maximizer::Settings start;
    gain = addFloat ("gain", "Gain", { 0.0f, 24.0f }, (float) start.gainDb, "dB");
    ceiling = addFloat ("ceiling", "Ceiling", { -24.0f, 0.0f }, (float) start.ceilingDb, "dBTP");
    character = addChoice ("character", "Character", Maximizer::characterNames(), (int) start.character);
    emphasis = addFloat ("emphasis", "Transient Emphasis", { 0.0f, 100.0f }, (float) start.emphasisPercent, "%");
}

Maximizer::Settings MaximizerProcessor::getSettings() const
{
    return { (double) gain->get(), (double) ceiling->get(), (Maximizer::Character) character->getIndex(), (double) emphasis->get() };
}

void MaximizerProcessor::setSettings (const Maximizer::Settings& s)
{
    setExactly (*gain, (float) s.gainDb);
    setExactly (*ceiling, (float) s.ceilingDb);
    *character = (int) s.character;
    setExactly (*emphasis, (float) s.emphasisPercent);
}

juce::AudioProcessorEditor* MaximizerProcessor::createEditor()
{
    return new PresetsAndParameters (*this, [this] { return getReductionDb(); });
}

void MaximizerProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = getSettings();
    core.set (applied);
    core.reset();
    setLatencySamples (Maximizer::latencyFrames (applied.character, rate));
}

void MaximizerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto now = getSettings();

    if (! juce::exactlyEqual (now.gainDb, applied.gainDb) || ! juce::exactlyEqual (now.ceilingDb, applied.ceilingDb) || now.character != applied.character
        || ! juce::exactlyEqual (now.emphasisPercent, applied.emphasisPercent))
    {
        const auto characterChanged = now.character != applied.character;
        applied = now;
        core.set (applied);

        if (characterChanged)
            setLatencySamples (Maximizer::latencyFrames (applied.character, rate));
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
    reductionDb = core.getReductionDb();
}

void MaximizerProcessor::reset()
{
    core.reset();
    reductionDb = 0.0;
}

//==============================================================================
TransientShaperProcessor::TransientShaperProcessor()
    : BuiltInEffect (transientShaperDescription(), "SPECTRABLADE_TRANSIENT_SHAPER")
{
    const TransientShaper::Settings start;
    const std::array<const char*, 3> ids { "low", "mid", "high" }, names { "Low", "Mid", "High" };
    lowCrossover = addFloat ("lowCrossover", "Low Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.lowCrossover, "Hz");
    highCrossover = addFloat ("highCrossover", "High Crossover", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) start.highCrossover, "Hz");

    for (size_t b = 0; b < 3; ++b)
    {
        const juce::String id (ids[b]), name (names[b]);
        bands[b].first = addFloat (id + "Attack", name + " Attack", { -12.0f, 12.0f }, (float) start.bands[b].attackDb, "dB");
        bands[b].second = addFloat (id + "Sustain", name + " Sustain", { -12.0f, 12.0f }, (float) start.bands[b].sustainDb, "dB");
    }

    output = addFloat ("output", "Output", { -24.0f, 24.0f }, (float) start.outputDb, "dB");
    linearPhase = addBool ("linearPhase", "Linear Phase", start.linearPhase);
}

std::vector<float> TransientShaperProcessor::valuesOfParameters() const
{
    std::vector<float> v { lowCrossover->get(), highCrossover->get(), output->get() };

    for (auto& b : bands)
        v.insert (v.end(), { b.first->get(), b.second->get() });

    v.push_back (linearPhase->get() ? 1.0f : 0.0f);
    return v;
}

TransientShaper::Settings TransientShaperProcessor::getParameterSettings() const
{
    TransientShaper::Settings s;
    s.lowCrossover = lowCrossover->get();
    s.highCrossover = highCrossover->get();
    s.outputDb = output->get();

    for (size_t b = 0; b < 3; ++b)
        s.bands[b] = { (double) bands[b].first->get(), (double) bands[b].second->get() };

    s.linearPhase = linearPhase->get();
    return s;
}

TransientShaper::Settings TransientShaperProcessor::getSettings() const
{
    return TransientShaper::spaced (getParameterSettings(), rate);
}

void TransientShaperProcessor::setSettings (const TransientShaper::Settings& s)
{
    setExactly (*lowCrossover, (float) s.lowCrossover);
    setExactly (*highCrossover, (float) s.highCrossover);
    setExactly (*output, (float) s.outputDb);

    for (size_t b = 0; b < 3; ++b)
    {
        setExactly (*bands[b].first, (float) s.bands[b].attackDb);
        setExactly (*bands[b].second, (float) s.bands[b].sustainDb);
    }

    *linearPhase = s.linearPhase;
}

juce::AudioProcessorEditor* TransientShaperProcessor::createEditor()
{
    return new PresetsAndParameters (*this);
}

void TransientShaperProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = valuesOfParameters();
    core.set (getSettings());
    core.reset();
    setLatencySamples (core.getLatency());
}

void TransientShaperProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (auto now = valuesOfParameters(); now != applied)
    {
        applied = std::move (now);
        core.set (getSettings());

        if (core.getLatency() != getLatencySamples())
            setLatencySamples (core.getLatency());
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
}

void TransientShaperProcessor::reset()
{
    core.reset();
}

//==============================================================================
DynamicEqProcessor::DynamicEqProcessor()
    : BuiltInEffect (dynamicEqDescription(), "SPECTRABLADE_DYNAMIC_EQ")
{
    const auto start = DynamicEq::defaultSettings();

    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto id = "band" + juce::String ((int) i + 1), name = "Band " + juce::String ((int) i + 1);
        const auto& b = start.bands[i];
        auto& p = bands[i];
        p.on = addBool (id + "On", name + " On", b.on);
        p.shape = addChoice (id + "Shape", name + " Shape", { "Bell", "Low Shelf", "High Shelf" }, (int) b.shape);
        p.mode = addChoice (id + "Mode", name + " Mode", { "Over", "Under" }, (int) b.mode);
        p.channel = addChoice (id + "Channel", name + " Channel", Equalizer::channelNames(), (int) b.channel);
        p.frequency = addFloat (id + "Frequency", name + " Frequency", { 20.0f, 20000.0f, 0.0f, 0.25f }, (float) b.frequency, "Hz");
        p.q = addFloat (id + "Q", name + " Q", { 0.1f, 20.0f, 0.0f, 0.4f }, (float) b.q);
        p.gain = addFloat (id + "Gain", name + " Gain", { -24.0f, 24.0f }, (float) b.gainDb, "dB");
        p.threshold = addFloat (id + "Threshold", name + " Threshold", { -80.0f, 0.0f }, (float) b.thresholdDb, "dB");
        p.ratio = addFloat (id + "Ratio", name + " Ratio", { 1.0f, 20.0f, 0.0f, 0.5f }, (float) b.ratio, ":1");
        p.attack = addFloat (id + "Attack", name + " Attack", { 0.1f, 500.0f, 0.0f, 0.25f }, (float) b.attackMs, "ms");
        p.release = addFloat (id + "Release", name + " Release", { 5.0f, 5000.0f, 0.0f, 0.3f }, (float) b.releaseMs, "ms");
    }

    output = addFloat ("output", "Output", { -24.0f, 24.0f }, (float) start.outputDb, "dB");
}

std::vector<float> DynamicEqProcessor::parameterValues() const
{
    std::vector<float> v { output->get() };

    for (auto& p : bands)
        v.insert (v.end(), { p.on->get() ? 1.0f : 0.0f, (float) p.shape->getIndex(), (float) p.mode->getIndex(), (float) p.channel->getIndex(),
                             p.frequency->get(), p.q->get(), p.gain->get(), p.threshold->get(), p.ratio->get(), p.attack->get(), p.release->get() });

    return v;
}

DynamicEq::Settings DynamicEqProcessor::getParameterSettings() const
{
    DynamicEq::Settings s;
    s.outputDb = output->get();

    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& p = bands[i];
        s.bands[i] = { p.on->get(), (DynamicEq::Shape) p.shape->getIndex(), (double) p.frequency->get(), (double) p.q->get(), (double) p.gain->get(),
                       (double) p.threshold->get(), (double) p.ratio->get(), (double) p.attack->get(), (double) p.release->get(), (DynamicEq::Mode) p.mode->getIndex(),
                       (Equalizer::Channel) p.channel->getIndex() };
    }

    return s;
}

DynamicEq::Settings DynamicEqProcessor::getSettings() const
{
    return DynamicEq::withinRate (getParameterSettings(), rate);
}

void DynamicEqProcessor::setSettings (const DynamicEq::Settings& s)
{
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& b = s.bands[i];
        auto& p = bands[i];
        *p.on = b.on;
        *p.shape = (int) b.shape;
        *p.mode = (int) b.mode;
        *p.channel = (int) b.channel;
        setExactly (*p.frequency, (float) b.frequency);
        setExactly (*p.q, (float) b.q);
        setExactly (*p.gain, (float) b.gainDb);
        setExactly (*p.threshold, (float) b.thresholdDb);
        setExactly (*p.ratio, (float) b.ratio);
        setExactly (*p.attack, (float) b.attackMs);
        setExactly (*p.release, (float) b.releaseMs);
    }

    setExactly (*output, (float) s.outputDb);
}

DynamicEq::Panel::Readings DynamicEqProcessor::getReadings() const noexcept
{
    DynamicEq::Panel::Readings r;

    for (size_t i = 0; i < bands.size(); ++i)
    {
        r.gainDb[i] = gainDb[i].load();
        r.levelDb[i] = levelDb[i].load();
    }

    return r;
}

juce::AudioProcessorEditor* DynamicEqProcessor::createEditor()
{
    return new DynamicEqEditor (*this);
}

void DynamicEqProcessor::prepareToPlay (double sampleRate, int)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, getTotalNumInputChannels());
    core.prepare (channelsPrepared, rate);
    applied = parameterValues();
    core.set (getSettings());
    core.reset();
}

void DynamicEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (auto now = parameterValues(); now != applied)
    {
        applied = now;
        core.set (getSettings());
    }

    core.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), channelsPrepared), buffer.getNumSamples());
    const auto& gains = core.getGainDb();
    const auto levels = core.getLevelDb();

    for (size_t i = 0; i < bands.size(); ++i)
    {
        gainDb[i] = gains[i];
        levelDb[i] = levels[i];
    }
}

void DynamicEqProcessor::reset()
{
    core.reset();

    for (size_t i = 0; i < bands.size(); ++i)
    {
        gainDb[i] = 0.0;
        levelDb[i] = -150.0;
    }
}

//==============================================================================
DynamicEqEditor::DynamicEqEditor (DynamicEqProcessor& owner)
    : AudioProcessorEditor (owner), effect (owner),
      presets (EffectPresets::userStore(), owner.getPresetKey(), [&owner] { return owner.getPresetValues(); },
               [this, &owner] (const EffectPresets::Values& v)
               {
                   owner.setPresetValues (v);
                   refresh();
               }),
      panel (owner.getParameterSettings(), owner.getSampleRate() > 0.0 ? owner.getSampleRate() : 48000.0)
{
    shown = effect.parameterValues();
    panel.onChange = [this] { panelChanged(); };
    panel.readLive = [this] { return effect.getReadings(); };
    addAndMakeVisible (presets);
    addAndMakeVisible (panel);
    setSize (panel.getWidth(), panel.getHeight() + 40);
    startTimerHz (20);
}

DynamicEqEditor::~DynamicEqEditor()
{
    stopTimer();
}

void DynamicEqEditor::panelChanged()
{
    effect.setSettings (panel.getSettings());
    shown = effect.parameterValues();
}

void DynamicEqEditor::refresh()
{
    if (auto now = effect.parameterValues(); now != shown)
    {
        shown = std::move (now);
        panel.setSettings (effect.getParameterSettings());
    }
}

void DynamicEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void DynamicEqEditor::resized()
{
    auto r = getLocalBounds();
    presets.setBounds (r.removeFromTop (40).reduced (8, 6));
    panel.setBounds (r);
}

//==============================================================================
BasicSynthProcessor::BasicSynthProcessor()
    : BuiltInEffect (basicSynthDescription(), "SPECTRABLADE_BASIC_SYNTH", false, true)
{
    wave = addChoice ("wave", "Wave", { "Sine", "Triangle", "Sawtooth", "Square" }, 2);
    level = addFloat ("level", "Level", { -40.0f, 0.0f }, -12.0f, "dB");
    attack = addFloat ("attack", "Attack", { 0.0f, 5000.0f, 0.0f, 0.3f }, 5.0f, "ms");
    decay = addFloat ("decay", "Decay", { 0.0f, 5000.0f, 0.0f, 0.3f }, 100.0f, "ms");
    sustain = addFloat ("sustain", "Sustain", { 0.0f, 100.0f }, 70.0f, "%");
    release = addFloat ("release", "Release", { 0.0f, 10000.0f, 0.0f, 0.3f }, 200.0f, "ms");
    cutoff = addFloat ("cutoff", "Cutoff", { 20.0f, 20000.0f, 0.0f, 0.25f }, 20000.0f, "Hz");
    resonance = addFloat ("resonance", "Resonance", { 0.0f, 100.0f }, 0.0f, "%");
}

void BasicSynthProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    rate = sampleRate;

    for (auto& v : voices)
    {
        v.envelope.setSampleRate (rate);
        v.filter.prepare ({ rate, (juce::uint32) juce::jmax (1, maximumBlockSize), 1 });
        v.filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    }

    reset();
}

void BasicSynthProcessor::reset()
{
    allOff (true);
}

double BasicSynthProcessor::getTailLengthSeconds() const
{
    return release->get() * 0.001;
}

int BasicSynthProcessor::getNumSounding() const
{
    return (int) std::count_if (voices.begin(), voices.end(), [] (const Voice& v) { return v.note >= 0; });
}

void BasicSynthProcessor::noteOn (int note, float velocity)
{
    // The voice already playing the note, else a free one, else the oldest.
    Voice* voice = nullptr;

    for (auto& v : voices)
        if (v.note == note)
            voice = &v;

    if (voice == nullptr)
        for (auto& v : voices)
            if (v.note < 0)
            {
                voice = &v;
                break;
            }

    if (voice == nullptr)
        voice = &*std::min_element (voices.begin(), voices.end(), [] (const Voice& a, const Voice& b) { return a.age < b.age; });

    voice->note = note;
    voice->velocity = velocity;
    voice->phase = 0.0;
    voice->filter.reset();
    voice->envelope.noteOn();
    voice->age = ++counter;
}

void BasicSynthProcessor::noteOff (int note)
{
    for (auto& v : voices)
        if (v.note == note)
            v.envelope.noteOff();
}

void BasicSynthProcessor::allOff (bool atOnce)
{
    for (auto& v : voices)
    {
        if (atOnce)
        {
            v.note = -1;
            v.envelope.reset();
            v.filter.reset();
        }
        else
        {
            v.envelope.noteOff();
        }
    }
}

float BasicSynthProcessor::oscillate (Voice& v, double increment) const
{
    // A step's correction near a wave's jump (PolyBLEP), so the sawtooth and square don't alias.
    const auto blep = [] (double t, double dt)
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }

        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return t * t + t + t + 1.0;
        }

        return 0.0;
    };

    const auto t = v.phase;
    double s = 0.0;

    switch (wave->getIndex())
    {
        case 0:  s = std::sin (juce::MathConstants<double>::twoPi * t); break;
        case 1:  s = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t; break;
        case 2:  s = 2.0 * t - 1.0 - blep (t, increment); break;
        default: s = (t < 0.5 ? 1.0 : -1.0) + blep (t, increment) - blep (std::fmod (t + 0.5, 1.0), increment); break;
    }

    v.phase += increment;

    if (v.phase >= 1.0)
        v.phase -= 1.0;

    return (float) s;
}

void BasicSynthProcessor::renderVoices (juce::AudioBuffer<float>& buffer, int from, int count, bool filterOpen, float gain)
{
    if (count <= 0 || buffer.getNumChannels() == 0)
        return;

    auto* out = buffer.getWritePointer (0);

    for (auto& v : voices)
    {
        if (v.note < 0)
            continue;

        const auto increment = juce::MidiMessage::getMidiNoteInHertz (v.note) / rate;

        for (int i = from; i < from + count; ++i)
        {
            if (! v.envelope.isActive())
            {
                v.note = -1;
                break;
            }

            auto s = oscillate (v, increment);

            if (! filterOpen)
                s = v.filter.processSample (0, s);

            out[i] += s * v.envelope.getNextSample() * v.velocity;
        }
    }

    buffer.applyGain (0, from, count, gain);

    for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom (ch, from, buffer, 0, from, count);
}

void BasicSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const auto n = buffer.getNumSamples();
    buffer.clear();

    // The settings, taken once a block.
    const juce::ADSR::Parameters envelope { attack->get() * 0.001f, decay->get() * 0.001f, sustain->get() * 0.01f, release->get() * 0.001f };
    const auto hz = juce::jmin (cutoff->get(), (float) (0.49 * rate));
    const auto q = resonance->get();
    const auto open = cutoff->get() >= 19999.0f && q <= 0.0f;
    const auto gain = juce::Decibels::decibelsToGain (level->get());

    for (auto& v : voices)
    {
        v.envelope.setParameters (envelope);

        if (! open)
        {
            v.filter.setCutoffFrequency (hz);
            v.filter.setResonance (juce::MathConstants<float>::sqrt2 * 0.5f + q * 0.01f * (20.0f - juce::MathConstants<float>::sqrt2 * 0.5f));
        }
    }

    int done = 0;

    for (const auto meta : midi)
    {
        const auto at = juce::jlimit (done, n, meta.samplePosition);
        renderVoices (buffer, done, at - done, open, gain);
        done = at;

        const auto m = meta.getMessage();

        if (m.isNoteOn())
            noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            noteOff (m.getNoteNumber());
        else if (m.isAllSoundOff())
            allOff (true);
        else if (m.isAllNotesOff())
            allOff (false);
    }

    renderVoices (buffer, done, n - done, open, gain);
    midi.clear();
}
}
