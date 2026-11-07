#include "Batch.h"
#include "Delivery.h"
#include "AudioFileIO.h"
#include "AudioEdits.h"
#include "OfflineRender.h"
#include "SampleRateConverter.h"

namespace Batch
{
namespace
{
    bool isNative (const juce::String& type)    { return type == "wav" || type == "aiff" || type == "flac"; }
    bool isLossy (const juce::String& type)     { return type == "mp3" || type == "aac"; }

    const Ffmpeg::Target* ffmpegTarget (const juce::String& type)
    {
        for (auto& t : Ffmpeg::targets())
            if (type == "ffmpeg:" + t.codec)
                return &t;

        return nullptr;
    }
}

juce::String Settings::problem() const
{
    if (! isNative (type) && ! isLossy (type) && ffmpegTarget (type) == nullptr)
        return "Choose a file type.";

    if (isLossy (type) && ! lossy.isValid())
        return "The MP3 or AAC settings aren't valid.";

    if (sampleRate != 0.0 && (sampleRate < 8000.0 || sampleRate > 384000.0))
        return "Choose a sample rate from 8 to 384 kHz.";

    if (channels < 0 || channels > 2)
        return "Choose one or two channels, or keep each file's.";

    if (level == Level::peak && (peakDb > 0.0f || peakDb < -60.0f))
        return "Choose a peak from -60 to 0 dBFS.";

    if (level == Level::loudness && (lufs < -70.0 || lufs > 0.0 || ceilingDbtp < -20.0 || ceilingDbtp > 0.0))
        return "Choose a loudness from -70 to 0 LUFS and a true-peak ceiling from -20 to 0 dBTP.";

    if (level == Level::spec && ! Delivery::isValid (specLufs, specToleranceLu, specCeilingDbtp))
        return "Choose a delivery spec with a target from -70 to 0 LUFS, a tolerance from 0.1 to 10 LU and a true-peak ceiling from -30 to 0 dBTP.";

    if (folder != juce::File() && ! folder.isDirectory())
        return "The folder to write into isn't there.";

    if (suffix.containsAnyOf ("/:"))
        return "The suffix can't hold / or :.";

    return {};
}

juce::StringArray types (bool withFfmpeg)
{
    juce::StringArray list { "wav", "aiff", "flac", "mp3", "aac" };

    if (withFfmpeg)
        for (auto& t : Ffmpeg::targets())
            list.add ("ffmpeg:" + t.codec);

    return list;
}

juce::String nameOf (const juce::String& type)
{
    if (const auto* t = ffmpegTarget (type))
        return t->name + " (ffmpeg)";

    return type == "aac" ? "AAC (.m4a)" : type.toUpperCase();
}

juce::String extensionOf (const juce::String& type)
{
    if (const auto* t = ffmpegTarget (type))
        return t->extension.trimCharactersAtStart (".");

    return type == "aac" ? "m4a" : type;
}

juce::File targetFor (const juce::File& source, const Settings& settings)
{
    const auto folder = settings.folder != juce::File() ? settings.folder : source.getParentDirectory();
    const auto name = juce::File::createLegalFileName (source.getFileNameWithoutExtension() + settings.suffix);
    const auto extension = "." + extensionOf (settings.type);
    auto target = folder.getChildFile (name + extension);

    for (int n = 2; target.exists() || target == source; ++n)
        target = folder.getChildFile (name + " " + juce::String (n) + extension);

    return target;
}

Outcome process (const juce::File& source, const Settings& settings, juce::AudioFormatManager& formats,
                 const juce::XmlElement* rack, juce::AudioPluginFormatManager* plugins,
                 const std::function<bool (float)>& progress)
{
    Outcome out;
    out.source = source;
    const auto keepGoing = [&progress] (float p) { return progress == nullptr || progress (p); };

    if (const auto problem = settings.problem(); problem.isNotEmpty())
    {
        out.error = problem;
        return out;
    }

    auto loaded = AudioFileIO::load (source, formats);

    if (loaded.snapshot == nullptr)
    {
        out.error = loaded.error.isNotEmpty() ? loaded.error : "It couldn't be read.";
        return out;
    }

    auto audio = loaded.snapshot;
    auto rate = loaded.sampleRate;
    auto markers = loaded.markers;

    if (! keepGoing (0.1f))
    {
        out.error = "Stopped.";
        return out;
    }

    // The effects rack, at the file's own rate.
    if (settings.applyRack && rack != nullptr && plugins != nullptr && rack->getNumChildElements() > 0)
    {
        juce::AudioBuffer<float> buffer (audio->getNumChannels(), (int) audio->getNumFrames());
        audio->read (buffer, 0, 0, buffer.getNumSamples());

        if (const auto error = OfflineRender::processThroughRack (*rack, *plugins, rate, buffer,
                                                                  [&keepGoing] (float p) { return keepGoing (0.1f + 0.3f * p); });
            error.isNotEmpty())
        {
            out.error = "The effects rack: " + error;
            return out;
        }

        audio = AudioEdits::fromBuffer (std::move (buffer));
    }

    // The sequence, on the whole file. A rate change in it moves the markers.
    if (settings.sequence != nullptr && ! settings.sequence->steps.empty())
    {
        auto played = Sequences::run (*settings.sequence, audio, 0, audio->getNumFrames(), rate, markers, plugins,
                                      [&keepGoing] (float p) { return keepGoing (0.4f + 0.1f * p); });

        if (played.error.isNotEmpty())
        {
            out.error = played.error == "Stopped." ? played.error : "The sequence: " + played.error;
            return out;
        }

        audio = played.snapshot;
        rate = played.sampleRate;
        markers = std::move (played.markers);
        out.notes.addArray (played.notes);
    }

    // The sample rate, with the markers moved to match.
    if (settings.sampleRate > 0.0 && ! juce::exactlyEqual (settings.sampleRate, rate))
    {
        auto converted = SampleRateConverter::convert (*audio, rate, settings.sampleRate,
                                                       [&keepGoing] (float p) { return keepGoing (0.5f + 0.2f * p); });

        if (converted == nullptr)
        {
            out.error = "Stopped.";
            return out;
        }

        markers = SampleRateConverter::convert (markers, rate, settings.sampleRate);
        audio = std::move (converted);
        rate = settings.sampleRate;
    }

    if (settings.channels > 0 && settings.channels != audio->getNumChannels())
        audio = AudioEdits::convertChannels (*audio, settings.channels);

    if (! keepGoing (0.7f))
    {
        out.error = "Stopped.";
        return out;
    }

    // The level, last, so it is what the file ends up with.
    const auto frames = audio->getNumFrames();

    // The level, as Normalize and Match Loudness do it.
    if (settings.level != Level::none)
    {
        const auto step = settings.level == Level::peak
                              ? Sequences::Step { "normalize", { { "db", settings.peakDb }, { "apart", 0.0 } }, nullptr }
                          : settings.level == Level::spec
                              ? Sequences::Step { "deliverySpec", { { "target", settings.specLufs }, { "tolerance", settings.specToleranceLu },
                                                                    { "ceiling", settings.specCeilingDbtp } }, nullptr }
                              : Sequences::Step { "matchLoudness", { { "target", settings.lufs }, { "ceiling", settings.ceilingDbtp },
                                                                     { "limit", settings.limit ? 1.0 : 0.0 } }, nullptr };
        auto levelled = Sequences::apply (step, audio, 0, frames, rate, markers, nullptr);

        if (levelled.error.isNotEmpty())
        {
            out.error = levelled.error;
            return out;
        }

        if (levelled.snapshot != nullptr)
            audio = std::move (levelled.snapshot);

        for (auto& note : levelled.notes)
            out.notes.add (settings.level == Level::spec ? step.name() + ": " + note : note);
    }

    if (! keepGoing (0.8f))
    {
        out.error = "Stopped.";
        return out;
    }

    // Written through a temporary name, so a file is there only when it is whole.
    const auto target = targetFor (source, settings);
    const auto writeProgress = [&keepGoing] (float p) { return keepGoing (0.8f + 0.2f * p); };
    juce::String error;

    if (isNative (settings.type))
    {
        const auto depths = AudioFileIO::bitDepthsFor (target, formats);
        auto depth = settings.bitDepth != 0 ? settings.bitDepth : loaded.bitDepth;

        if (! depths.contains (depth))
        {
            if (depth != 0 && settings.bitDepth == 0)
                out.notes.add (juce::String (depth == 32 ? "32-bit float" : juce::String (depth) + "-bit") + " isn't held by "
                               + nameOf (settings.type) + ", so " + juce::String (depths[0]) + "-bit");

            depth = depths.isEmpty() ? 24 : depths[0];
        }

        error = AudioFileIO::save (*audio, rate, target, depth, formats, markers, loaded.info);

        if (! markers.empty() && ! AudioFileIO::canStoreMarkers (target))
            out.notes.add ("its markers aren't kept in " + nameOf (settings.type) + " files");
    }
    else if (isLossy (settings.type))
    {
        auto lossy = settings.lossy;
        lossy.kind = settings.type == "mp3" ? LossyFiles::Kind::mp3 : LossyFiles::Kind::aac;
        error = LossyFiles::write (*audio, rate, target, lossy, writeProgress, loaded.info);
    }
    else
    {
        const auto ffmpeg = Ffmpeg::find();
        const auto* t = ffmpegTarget (settings.type);

        if (ffmpeg == juce::File() || t == nullptr)
        {
            error = "Writing " + nameOf (settings.type) + " files needs ffmpeg, which isn't installed.";
        }
        else
        {
            auto kbps = t->defaultBitRate;

            if (t->hasBitRate && settings.ffmpegKbps > 0)
            {
                for (auto r : t->bitRates)
                    if (std::abs (r - settings.ffmpegKbps) < std::abs (kbps - settings.ffmpegKbps))
                        kbps = r;

                if (kbps != settings.ffmpegKbps)
                    out.notes.add (t->name + " at " + juce::String (kbps) + " kbps, the nearest it offers");
            }

            juce::TemporaryFile wav (".wav");
            error = AudioFileIO::save (*audio, rate, wav.getFile(), 32, formats);

            if (error.isEmpty())
                error = Ffmpeg::encode (ffmpeg, wav.getFile(), target, *t, kbps, [&writeProgress] { return writeProgress (0.5f); }, loaded.info);
        }
    }

    if (error.isNotEmpty())
    {
        out.error = error;
        return out;
    }

    if (! isNative (settings.type) && ! markers.empty())
        out.notes.add ("its markers aren't kept in " + nameOf (settings.type) + " files");

    out.written = target;
    return out;
}

juce::Array<juce::File> collect (const juce::Array<juce::File>& chosen, juce::AudioFormatManager& formats)
{
    juce::StringArray extensions;

    for (auto* f : formats)
        for (auto e : f->getFileExtensions())
            extensions.addIfNotAlreadyThere (e.trimCharactersAtStart (".").toLowerCase());

    for (auto& e : Ffmpeg::importExtensions())
        extensions.addIfNotAlreadyThere (e.toLowerCase());

    const auto isAudio = [&extensions] (const juce::File& f)
    {
        return f.existsAsFile() && ! f.isHidden() && extensions.contains (f.getFileExtension().trimCharactersAtStart (".").toLowerCase());
    };

    juce::Array<juce::File> files;

    for (auto& c : chosen)
    {
        if (c.isDirectory())
        {
            auto found = c.findChildFiles (juce::File::findFiles, true);
            found.sort();

            for (auto& f : found)
                if (isAudio (f) && ! f.getFullPathName().contains ("/."))
                    files.addIfNotAlreadyThere (f);
        }
        else if (isAudio (c))
        {
            files.addIfNotAlreadyThere (c);
        }
    }

    return files;
}

juce::String report (const std::vector<Outcome>& outcomes)
{
    int written = 0;

    for (auto& o : outcomes)
        written += o.written != juce::File() ? 1 : 0;

    const auto failed = (int) outcomes.size() - written;
    juce::String text = juce::String (written) + " of " + juce::String ((int) outcomes.size()) + " files written"
                        + (failed > 0 ? ", " + juce::String (failed) + " not" : juce::String()) + ".\n";

    for (auto& o : outcomes)
    {
        text << "\n" << o.source.getFileName() << ": ";

        if (o.written != juce::File())
            text << "written as " << o.written.getFileName();
        else
            text << "not written. " << o.error;

        if (! o.notes.isEmpty())
            text << " (" << o.notes.joinIntoString ("; ") << ")";
    }

    return text;
}
}
