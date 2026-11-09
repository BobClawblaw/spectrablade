#include "SoundLibraryPreview.h"
#include "AudioFileIO.h"
#include "Surround.h"
#include "Theme.h"

using Player = SoundLibraryPreview::Player;

Player::Player()
{
    AudioFileIO::registerFormats (formats);
    readAhead.startThread (juce::Thread::Priority::normal);
}

Player::~Player()
{
    unload();
    readAhead.stopThread (2000);
}

bool Player::load (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> opened (formats.createReaderFor (f));

    if (opened == nullptr)
    {
        unload();
        return false;
    }

    const auto rate = opened->sampleRate;
    const auto channels = (int) opened->numChannels;
    const auto frames = opened->lengthInSamples;
    auto buffered = std::make_unique<juce::BufferingAudioReader> (opened.release(), readAhead, (int) juce::jmax (8192.0, 2.0 * rate));
    buffered->setReadTimeout (readTimeout);

    const juce::ScopedLock sl (lock);
    playing = false;
    reader = std::move (buffered);
    file = f;
    fileChannels = channels;
    fileRate = rate;
    length = frames;
    range = {};
    position = 0;
    flush = true;
    scratch.setSize (juce::jmax (2, channels), juce::jmax (scratch.getNumSamples(), 4096), false, false, true);

    if (deviceRate > 0.0)
        resampler.setResamplingRatio (rate / deviceRate);

    return true;
}

void Player::unload()
{
    const juce::ScopedLock sl (lock);
    playing = false;
    reader.reset();
    file = juce::File();
    fileRate = 0.0;
    flush = true;
    length = 0;
    position = 0;
    range = {};
}

juce::File Player::getFile() const
{
    const juce::ScopedLock sl (lock);
    return file;
}

void Player::setRange (juce::Range<juce::int64> r)
{
    const juce::ScopedLock sl (lock);
    range = r.getIntersectionWith ({ 0, length.load() });
}

juce::Range<juce::int64> Player::getRange() const
{
    const juce::ScopedLock sl (lock);
    return range;
}

void Player::setPosition (juce::int64 frame)
{
    position = juce::jlimit ((juce::int64) 0, length.load(), frame);
}

void Player::play()
{
    const auto r = getRange();
    const auto part = r.isEmpty() ? juce::Range<juce::int64> (0, length.load()) : r;
    const auto at = position.load();

    if (at < part.getStart() || at >= part.getEnd())
        position = part.getStart();

    playing = length.load() > 0;
}

void Player::playFrom (juce::int64 frame)
{
    setPosition (frame);
    playing = length.load() > 0 && frame < length.load();
}

void Player::setGainDb (float db) noexcept
{
    gainDb = db;
    gain = juce::Decibels::decibelsToGain (db, -100.0f);
}

void Player::setReadTimeout (int milliseconds)
{
    const juce::ScopedLock sl (lock);
    readTimeout = milliseconds;

    if (auto* b = dynamic_cast<juce::BufferingAudioReader*> (reader.get()))
        b->setReadTimeout (milliseconds);
}

void Player::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    deviceRate = sampleRate;
    {
        const juce::ScopedLock sl (lock);
        scratch.setSize (juce::jmax (2, fileChannels), juce::jmax (4096, samplesPerBlockExpected * 4), false, false, true);
    }

    if (fileRate > 0.0)
        resampler.setResamplingRatio (fileRate / sampleRate);

    resampler.prepareToPlay (samplesPerBlockExpected, sampleRate);
}

void Player::releaseResources()
{
    resampler.releaseResources();
}

void Player::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    // At the device's rate the sound's own samples, untouched; otherwise through the resampler.
    if (flush.exchange (false))
        resampler.flushBuffers();

    if (fileRate <= 0.0 || juce::approximatelyEqual (fileRate.load(), deviceRate.load()))
        source.getNextAudioBlock (info);
    else
        resampler.getNextAudioBlock (info);
}

void Player::Source::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    auto& p = player;
    info.clearActiveBufferRegion();
    const juce::ScopedTryLock sl (p.lock);

    if (! sl.isLocked() || p.reader == nullptr || ! p.playing)
        return;

    const auto part = p.range.isEmpty() ? juce::Range<juce::int64> (0, p.length.load()) : p.range;
    const auto g = p.gain.load();
    auto& out = *info.buffer;
    int done = 0;

    while (done < info.numSamples && p.playing)
    {
        auto at = p.position.load();

        // Before the part, from its start; past its end, from its start again when looping.
        if (at >= part.getEnd() && ! p.looping)
        {
            p.playing = false;
            p.position = part.getEnd();
            break;
        }

        if (at < part.getStart() || at >= part.getEnd())
            at = part.getStart();

        const auto n = (int) juce::jmin ((juce::int64) (info.numSamples - done), part.getEnd() - at, (juce::int64) p.scratch.getNumSamples());
        p.reader->read (p.scratch.getArrayOfWritePointers(), p.fileChannels, at, n);
        const auto dest = info.startSample + done;

        // Into two channels: mono to both, 5.1 and 7.1 folded down, otherwise the first two.
        if (p.fileChannels == 1)
        {
            for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
                out.copyFrom (ch, dest, p.scratch.getReadPointer (0), n, g);
        }
        else if ((p.fileChannels == 6 || p.fileChannels == 8) && out.getNumChannels() >= 2)
        {
            Surround::foldDown (p.scratch.getArrayOfReadPointers(), p.fileChannels, out.getWritePointer (0, dest), out.getWritePointer (1, dest), n);
            out.applyGain (dest, n, g);
        }
        else
        {
            for (int ch = 0; ch < juce::jmin (2, out.getNumChannels(), p.fileChannels); ++ch)
                out.copyFrom (ch, dest, p.scratch.getReadPointer (ch), n, g);
        }

        p.position = at + n;
        done += n;
    }
}

juce::File SoundLibraryPreview::writePart (const juce::File& sound, juce::Range<juce::int64> part, const juce::File& folder)
{
    juce::AudioFormatManager fm;
    AudioFileIO::registerFormats (fm);
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (sound));

    if (reader == nullptr)
        return {};

    part = part.getIntersectionWith ({ 0, reader->lengthInSamples });

    if (part.isEmpty() || ! folder.createDirectory())
        return {};

    const auto seconds = [&reader] (juce::int64 frame) { return juce::String ((double) frame / reader->sampleRate, 3); };
    const auto target = folder.getChildFile (juce::File::createLegalFileName (sound.getFileNameWithoutExtension() + " " + seconds (part.getStart())
                                                                              + "-" + seconds (part.getEnd()) + " s") + ".wav");

    // The same part again: the file already written, when it still holds it.
    if (target.existsAsFile())
        if (std::unique_ptr<juce::AudioFormatReader> there (fm.createReaderFor (target));
            there != nullptr && there->lengthInSamples == part.getLength() && there->numChannels == reader->numChannels)
            return target;

    const auto temp = target.getSiblingFile (target.getFileNameWithoutExtension() + ".partial");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (temp.createOutputStream());

        if (out == nullptr)
            return {};

        const auto bits = reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample;
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (reader->sampleRate)
                                                    .withNumChannels ((int) reader->numChannels).withBitsPerSample (bits)
                                                    .withSampleFormat (reader->usesFloatingPointData ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                                                   : juce::AudioFormatWriterOptions::SampleFormat::integral));

        if (writer == nullptr || ! writer->writeFromAudioReader (*reader, part.getStart(), part.getLength()))
        {
            writer.reset();
            temp.deleteFile();
            return {};
        }
    }

    return temp.moveFileTo (target) ? target : juce::File();
}

SoundLibraryPreview::SoundLibraryPreview (const juce::File& parts) : partsFolder (parts)
{
    AudioFileIO::registerFormats (formats);
    thumbnail.addChangeListener (this);

    playButton.onClick = [this] { togglePlay(); };
    loopButton.onClick = [this] { player.setLooping (loopButton.getToggleState()); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (loopButton);
    autoButton.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (autoButton);

    volume.setRange (-40.0, 6.0, 0.5);
    volume.setValue (0.0, juce::dontSendNotification);
    volume.setTextValueSuffix (" dB");
    volume.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    volume.onValueChange = [this] { player.setGainDb ((float) volume.getValue()); };
    volume.setTooltip ("Preview volume");
    addAndMakeVisible (volume);

    Theme::follow (time, juce::Label::textColourId, Theme::dimText);
    time.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (time);
    updateButtons();
}

SoundLibraryPreview::~SoundLibraryPreview()
{
    thumbnail.removeChangeListener (this);
    player.unload();
}

void SoundLibraryPreview::show (const juce::File& f)
{
    selection = {};

    if (f == juce::File() || ! player.load (f))
    {
        player.unload();
        thumbnail.clear();
    }
    else
    {
        thumbnail.setSource (new juce::FileInputSource (f));

        if (autoButton.getToggleState())
            player.play();
    }

    updateButtons();
    repaint();
}

void SoundLibraryPreview::togglePlay()
{
    if (player.isPlaying())
        player.stop();
    else
        player.play();

    updateButtons();
}

void SoundLibraryPreview::setPlayOnSelect (bool on)
{
    autoButton.setToggleState (on, juce::dontSendNotification);
}

void SoundLibraryPreview::setSelection (juce::Range<juce::int64> r)
{
    selection = r.getIntersectionWith ({ 0, player.getLength() });
    player.setRange (selection);
    repaint();
}

juce::int64 SoundLibraryPreview::frameAt (float x) const
{
    if (wave.getWidth() <= 0)
        return 0;

    const auto proportion = juce::jlimit (0.0, 1.0, (double) (x - (float) wave.getX()) / (double) wave.getWidth());
    return (juce::int64) std::llround (proportion * (double) player.getLength());
}

float SoundLibraryPreview::xOf (juce::int64 frame) const
{
    const auto total = juce::jmax ((juce::int64) 1, player.getLength());
    return (float) wave.getX() + (float) wave.getWidth() * (float) ((double) frame / (double) total);
}

void SoundLibraryPreview::mouseDown (const juce::MouseEvent& e)
{
    gesture = Gesture::none;
    moved = false;

    if (! wave.contains (e.getPosition()) || player.getLength() == 0)
        return;

    const auto frame = frameAt (e.position.x);

    if (e.position.y < (float) (wave.getY() + scrubStripHeight))
    {
        gesture = Gesture::scrub;

        if (! selection.isEmpty() && ! selection.contains (frame))
            setSelection ({});

        player.playFrom (frame);
    }
    else if (! selection.isEmpty() && selection.contains (frame))
    {
        gesture = Gesture::dragPart;
    }
    else
    {
        gesture = Gesture::select;
        anchor = frame;
    }

    updateButtons();
}

void SoundLibraryPreview::mouseDrag (const juce::MouseEvent& e)
{
    const auto frame = frameAt (e.position.x);

    switch (gesture)
    {
        case Gesture::scrub:
            player.setPosition (frame);

            if (! player.isPlaying())
                player.playFrom (frame);

            break;

        case Gesture::select:
            if (moved || e.getDistanceFromDragStart() > 3)
            {
                moved = true;
                setSelection ({ juce::jmin (anchor, frame), juce::jmax (anchor, frame) });
            }

            break;

        case Gesture::dragPart:
            if (! moved && e.getDistanceFromDragStart() >= 6)
            {
                moved = true;
                const auto part = writePart (player.getFile(), selection, partsFolder);

                if (part != juce::File() && onDragOut != nullptr)
                    onDragOut ({ part.getFullPathName() });
            }

            break;

        case Gesture::none:
            break;
    }

    updateButtons();
    repaint();
}

void SoundLibraryPreview::mouseUp (const juce::MouseEvent& e)
{
    // A click (no drag) plays from there, dropping the selection; in the selection, from there within it.
    if ((gesture == Gesture::select || gesture == Gesture::dragPart) && ! moved)
    {
        const auto frame = frameAt (e.position.x);

        if (gesture == Gesture::select)
            setSelection ({});

        player.playFrom (frame);
    }

    gesture = Gesture::none;
    updateButtons();
    repaint();
}

void SoundLibraryPreview::timerCallback()
{
    updateButtons();
    repaint (wave.expanded (2));
}

void SoundLibraryPreview::updateButtons()
{
    const auto playing = player.isPlaying();
    playButton.setButtonText (playing ? "Stop" : "Play");
    playButton.setEnabled (player.getLength() > 0);

    const auto rate = juce::jmax (1.0, player.getSampleRate());
    const auto clock = [rate] (juce::int64 frame)
    {
        const auto s = (double) frame / rate;
        const auto m = (int) (s / 60.0);
        return juce::String (m) + ":" + juce::String (s - 60.0 * m, 2).paddedLeft ('0', 5);
    };
    time.setText (player.getLength() > 0 ? clock (player.getPosition()) + " / " + clock (player.getLength()) : juce::String(),
                  juce::dontSendNotification);

    if (playing && ! isTimerRunning())
        startTimerHz (30);
    else if (! playing && isTimerRunning())
        stopTimer();
}

void SoundLibraryPreview::paint (juce::Graphics& g)
{
    g.fillAll (Theme::surface);
    g.setColour (Theme::inset);
    g.fillRect (wave);

    if (player.getLength() == 0)
    {
        g.setColour (Theme::dimText);
        g.setFont (13.0f);
        g.drawText ("Select a sound to preview it", wave, juce::Justification::centred);
        return;
    }

    // The scrub strip, the selected part, the whole waveform, then the playhead.
    g.setColour (Theme::surface.brighter (0.08f));
    g.fillRect (wave.withHeight (scrubStripHeight));

    if (! selection.isEmpty())
    {
        g.setColour (Theme::selection.withAlpha (0.55f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (selection.getStart()), (float) wave.getY(), xOf (selection.getEnd()),
                                                                (float) wave.getBottom()));
    }

    g.setColour (Theme::accent);
    thumbnail.drawChannels (g, wave.withTrimmedTop (scrubStripHeight).reduced (0, 2), 0.0, thumbnail.getTotalLength(), 1.0f);

    g.setColour (Theme::textStrong);
    g.fillRect (juce::Rectangle<float> (xOf (player.getPosition()) - 0.75f, (float) wave.getY(), 1.5f, (float) wave.getHeight()));
}

void SoundLibraryPreview::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto controls = r.removeFromBottom (26);
    playButton.setBounds (controls.removeFromLeft (64));
    controls.removeFromLeft (6);
    loopButton.setBounds (controls.removeFromLeft (64));
    autoButton.setBounds (controls.removeFromLeft (120));
    time.setBounds (controls.removeFromRight (150));
    volume.setBounds (controls.removeFromRight (220));
    r.removeFromBottom (4);
    wave = r;
}
