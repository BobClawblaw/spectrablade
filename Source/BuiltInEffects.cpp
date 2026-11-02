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
    return { parametricEqDescription(), compressorDescription(), limiterDescription() };
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
BuiltInEffect::BuiltInEffect (const juce::PluginDescription& d, const juce::String& stateTag)
    : AudioPluginInstance (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo())
                                            .withOutput ("Output", juce::AudioChannelSet::stereo())),
      description (d), tag (stateTag)
{
}

bool BuiltInEffect::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto in = layout.getMainInputChannelSet(), out = layout.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

juce::AudioParameterFloat* BuiltInEffect::addFloat (const juce::String& id, const juce::String& name, juce::NormalisableRange<float> range,
                                                    float start, const juce::String& label)
{
    auto p = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, start,
                                                          juce::AudioParameterFloatAttributes().withLabel (label));
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
        p.frequency = addFloat ("band" + n + "Frequency", "Band " + n + " Frequency", hz, (float) start.frequency, "Hz");
        p.gain = addFloat ("band" + n + "Gain", "Band " + n + " Gain", db, (float) start.gainDb, "dB");
        p.q = addFloat ("band" + n + "Q", "Band " + n + " Q", q, (float) start.q);
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

//==============================================================================
CompressorProcessor::CompressorProcessor()
    : BuiltInEffect (compressorDescription(), "SPECTRABLADE_COMPRESSOR")
{
    const Dynamics::Compressor start;
    // The offline Compressor's ranges, so any setting it takes can be heard live.
    threshold = addFloat ("threshold", "Threshold", { -80.0f, 0.0f }, (float) start.thresholdDb, "dB");
    ratio = addFloat ("ratio", "Ratio", { 1.0f, 100.0f, 0.0f, 0.25f }, (float) start.ratio, ":1");
    knee = addFloat ("knee", "Knee", { 0.0f, 24.0f }, (float) start.kneeDb, "dB");
    attack = addFloat ("attack", "Attack", { 0.01f, 1000.0f, 0.0f, 0.2f }, (float) start.attackMs, "ms");
    release = addFloat ("release", "Release", { 1.0f, 5000.0f, 0.0f, 0.25f }, (float) start.releaseMs, "ms");
    makeup = addFloat ("makeup", "Makeup Gain", { -48.0f, 48.0f }, (float) start.makeupDb, "dB");
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

void CompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto now = getSettings();

    if (! juce::exactlyEqual (now.thresholdDb, applied.thresholdDb) || ! juce::exactlyEqual (now.ratio, applied.ratio)
        || ! juce::exactlyEqual (now.kneeDb, applied.kneeDb) || ! juce::exactlyEqual (now.attackMs, applied.attackMs)
        || ! juce::exactlyEqual (now.releaseMs, applied.releaseMs) || ! juce::exactlyEqual (now.makeupDb, applied.makeupDb))
    {
        applied = now;
        core.set (applied, rate);
    }

    const auto channels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        double level = 0.0;

        for (int ch = 0; ch < channels; ++ch)
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
    delay.setSize (juce::jmax (1, getTotalNumInputChannels()), ahead);
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
    const auto settings = getSettings();
    const auto input = std::pow (10.0, settings.inputGainDb / 20.0);
    const auto top = Dynamics::ceilingGain (settings.ceilingDb);
    const auto unity = juce::exactlyEqual (settings.inputGainDb, 0.0);
    const auto channels = juce::jmin (buffer.getNumChannels(), delay.getNumChannels());
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
}
