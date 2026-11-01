#include "MainComponent.h"
#include "AudioFileIO.h"
#include "Metronome.h"
#include "OfflineRender.h"
#include "PluginScanner.h"
#include "SampleRateConverter.h"
#include "SessionRecording.h"
#include "SpectralEdit.h"

namespace
{
    constexpr const char* knownPluginsKey = "knownPlugins";
    constexpr const char* audioDeviceKey  = "audioDevice";
    constexpr const char* outputPairKey   = "outputPairFirstChannel";
    constexpr const char* lastFileKey     = "lastFile";
    constexpr const char* inputFirstKey   = "inputFirstChannel";
    constexpr const char* inputCountKey   = "inputNumChannels";
    constexpr const char* displayKey      = "display";
    constexpr const char* snappingKey     = "snapping";
    constexpr const char* crossfadingKey  = "crossfading";

    // Input choice IDs: none, a stereo pair from channel n, or mono channel n.
    constexpr int noInputId = 1, stereoInputBase = 1000, monoInputBase = 2000;

    constexpr double prerollSeconds = 2.0;

    enum CommandIDs
    {
        cmdOpen = 1, cmdSave, cmdSaveAs,
        cmdUndo, cmdRedo, cmdCut, cmdCopy, cmdPaste, cmdDelete, cmdSelectAll, cmdCrop,
        cmdGain, cmdNormalize, cmdFadeIn, cmdFadeOut, cmdSilence, cmdInvert, cmdReverse, cmdApplyRack,
        cmdPlayPause, cmdGoToStart, cmdGoToEnd,
        cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdZoomSelection,
        cmdPlugins, cmdAudioSettings, cmdSaveRack, cmdLoadRack,
        cmdRecord,
        cmdAddMarker, cmdPreviousMarker, cmdNextMarker, cmdDeleteAllMarkers,
        cmdConvertRate,
        cmdShowWaveform, cmdShowSpectrogram, cmdShowBoth,
        cmdSpectralAttenuate, cmdSpectralRemove,
        cmdLoudnessStats, cmdMatchLoudness,
        cmdCaptureNoisePrint, cmdNoiseReduction, cmdClickRepair,
        cmdMultitrack, cmdNewSession, cmdOpenSession, cmdImportToSession, cmdAddTrack, cmdDeleteTrack, cmdSplitClip,
        cmdAddBus, cmdSnapping, cmdMixDown, cmdCrossfade, cmdWriteAutomation, cmdLoopRecording, cmdInputMonitoring, cmdMetronome, cmdCountIn, cmdTempo, cmdBarsAndBeats
    };

    /** Commands that work on the open file only, off while the session is showing. */
    bool isEditorOnly (int id)
    {
        return (id >= cmdCut && id <= cmdApplyRack) || id == cmdZoomSelection
            || (id >= cmdAddMarker && id <= cmdConvertRate) || (id >= cmdSpectralAttenuate && id <= cmdClickRepair);
    }

    juce::String formatTime (juce::int64 frames, double sampleRate)
    {
        const auto total = (juce::int64) ((double) frames / juce::jmax (1.0, sampleRate) * 1000.0);
        return juce::String::formatted ("%d:%02d.%03d", (int) (total / 60000), (int) ((total / 1000) % 60), (int) (total % 1000));
    }

    /** A modal box with explicit result codes, so button order can never be misread. */
    void askThreeWay (const juce::String& title, const juce::String& message,
                      const juce::String& yes, const juce::String& no, std::function<void (int)> onResult)
    {
        juce::Logger::writeToLog ("Asking: " + title + ": " + message);
        auto* box = new juce::AlertWindow (title, message, juce::MessageBoxIconType::WarningIcon);
        box->addButton (yes, 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (no, 2);
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create (std::move (onResult)), true);
    }

    void showError (const juce::String& title, const juce::String& message)
    {
        juce::Logger::writeToLog ("Error shown: " + title + ": " + message);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message);
    }

    void showNote (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, message);
    }
}

//==============================================================================
/** Shows a plug-in's own editor, or a generic parameter editor when it has none. */
class MainComponent::PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioProcessor& p, std::function<void (PluginEditorWindow*)> onClose)
        : DocumentWindow (p.getName(), juce::Colours::darkgrey, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          processor (p), closeCallback (std::move (onClose))
    {
        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorAndMakeActive() : nullptr;

        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        setUsingNativeTitleBar (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginEditorWindow() override
    {
        // The editor must be deleted before the plug-in instance.
        clearContentComponent();
    }

    void closeButtonPressed() override  { closeCallback (this); }

    juce::AudioProcessor& processor;

private:
    std::function<void (PluginEditorWindow*)> closeCallback;
};

//==============================================================================
class MainComponent::PluginListWindow final : public juce::DocumentWindow
{
public:
    PluginListWindow (AudioEngine& audioEngine, const juce::File& deadMansPedal, juce::PropertiesFile* props,
                      std::function<void()> onClose)
        : DocumentWindow ("Plug-ins", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setContentOwned (new Panel (audioEngine, deadMansPedal, props), false);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (760, 600);
        setVisible (true);
    }

    void closeButtonPressed() override  { closeCallback(); }

private:
    /** The plug-in list, with scanning that never stops to ask: plug-ins that crash, hang or
        fail their check are marked not working (listed at the end) and logged. */
    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        Panel (AudioEngine& e, const juce::File& pedal, juce::PropertiesFile* props)
            : engine (e), list (e.pluginFormatManager, e.knownPlugins, pedal, props, true)
        {
            // Its own scan shows a modal progress window and a summary to dismiss; ours doesn't.
            list.getOptionsButton().setVisible (false);
            addAndMakeVisible (list);

            scanButton.setTooltip ("Find new VST3 and Audio Unit plug-ins, and check each one loads and plays");
            retryButton.setTooltip ("Check the plug-ins marked not working again");
            removeButton.setTooltip ("Remove the selected plug-ins from the list");
            scanButton.onClick = [this] { start (false); };
            retryButton.onClick = [this] { start (true); };
            removeButton.onClick = [this] { list.removeSelectedPlugins(); };
            stopButton.onClick = [this] { finish ("Scan stopped."); };

            for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &removeButton, &stopButton, &progressBar, &status })
                addAndMakeVisible (c);

            status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            showSummary ({});
            updateButtons();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto bar = r.removeFromTop (26);

            for (auto* b : { &scanButton, &retryButton, &removeButton })
            {
                b->setBounds (bar.removeFromLeft (b == &retryButton ? 150 : 140));
                bar.removeFromLeft (6);
            }

            stopButton.setBounds (bar.removeFromRight (60));
            bar.removeFromRight (6);
            progressBar.setBounds (bar);
            r.removeFromTop (6);
            status.setBounds (r.removeFromTop (22));
            r.removeFromTop (4);
            list.setBounds (r);
        }

    private:
        void start (bool retryNotWorking)
        {
            if (scan != nullptr)
                return;

            juce::Logger::writeToLog (retryNotWorking ? "Plug-in scan: retrying plug-ins marked not working" : "Plug-in scan started");
            scan = std::make_unique<PluginScan> (engine.pluginFormatManager, engine.knownPlugins, retryNotWorking);
            updateButtons();
            startTimerHz (10);
        }

        void finish (const juce::String& how)
        {
            stopTimer();

            if (scan == nullptr)
                return;

            const auto checked = scan->getNumWorking(), failed = scan->getNumNotWorking();
            scan = nullptr;
            progress = 0.0;
            const auto summary = how + " " + juce::String (checked) + " new plug-in files work, " + juce::String (failed) + " don't.";
            juce::Logger::writeToLog ("Plug-in scan: " + summary);
            showSummary (summary);
            updateButtons();
        }

        void timerCallback() override
        {
            progress = scan->getProgress();

            if (scan->isFinished())
            {
                finish ("Scan finished.");
                return;
            }

            status.setText ("Checking " + scan->getCurrentName() + "  (" + juce::String (scan->getNumWorking()) + " work, "
                                + juce::String (scan->getNumNotWorking()) + " don't)", juce::dontSendNotification);
        }

        void showSummary (const juce::String& lastScan)
        {
            const auto working = engine.knownPlugins.getNumTypes();
            const auto failed = engine.knownPlugins.getBlacklistedFiles().size();
            auto text = juce::String (working) + " working plug-ins";

            if (failed > 0)
                text << ", " << failed << " not working (at the end of the list; reasons in the log)";

            status.setText (lastScan.isEmpty() ? text : lastScan + "  " + text, juce::dontSendNotification);
        }

        void updateButtons()
        {
            const auto scanning = scan != nullptr;
            scanButton.setEnabled (! scanning);
            retryButton.setEnabled (! scanning && ! engine.knownPlugins.getBlacklistedFiles().isEmpty());
            removeButton.setEnabled (! scanning);
            stopButton.setEnabled (scanning);
        }

        AudioEngine& engine;
        juce::PluginListComponent list;
        juce::TextButton scanButton { "Scan for Plug-ins" }, retryButton { "Retry Not Working" },
                         removeButton { "Remove Selected" }, stopButton { "Stop" };
        double progress = 0.0;
        juce::ProgressBar progressBar { progress };
        juce::Label status;
        std::unique_ptr<PluginScan> scan;
    };

    std::function<void()> closeCallback;
};

//==============================================================================
/** Renders a range through fresh copies of the rack's plug-ins on a background thread. */
class MainComponent::RackRenderJob final : public juce::ThreadWithProgressWindow
{
public:
    RackRenderJob (MainComponent& o, std::unique_ptr<juce::XmlElement> rack, juce::Range<juce::int64> r)
        : ThreadWithProgressWindow ("Applying effects rack...", true, true, 30000, "Cancel", &o),
          owner (o), rackXml (std::move (rack)), range (r),
          snapshot (o.engine.document.getSnapshot()), version (o.engine.document.getVersion()),
          sampleRate (o.engine.document.getSampleRate())
    {
    }

    void run() override
    {
        audio.setSize (snapshot->getNumChannels(), (int) range.getLength());
        snapshot->read (audio, 0, range.getStart(), audio.getNumSamples());

        error = OfflineRender::processThroughRack (*rackXml, owner.engine.pluginFormatManager, sampleRate, audio,
                                                   [this] (float p) { setProgress (p); return ! threadShouldExit(); });
    }

    void threadComplete (bool userPressedCancel) override
    {
        auto& doc = owner.engine.document;

        if (! userPressedCancel && error.isEmpty())
        {
            if (doc.getVersion() != version)
            {
                showError ("Effects rack", "The audio changed while rendering, so the result was discarded.");
            }
            else
            {
                const auto length = (juce::int64) audio.getNumSamples();
                AudioClip rendered { audio.getNumChannels(), sampleRate, AudioEdits::fromBuffer (std::move (audio))->getSegments() };
                owner.applyEdit (AudioEdits::replaceWithClip (*snapshot, range.getStart(), range.getEnd(), rendered),
                                 "Apply Effects Rack", { range.getStart(), range.getStart() + length });
            }
        }
        else if (! userPressedCancel)
        {
            showError ("Effects rack", error);
        }

        // Deleting ourselves from inside our own callback is not safe; do it afterwards.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (&owner)]
        {
            if (safe != nullptr)
                safe->renderJob = nullptr;
        });
    }

private:
    MainComponent& owner;
    std::unique_ptr<juce::XmlElement> rackXml;
    juce::Range<juce::int64> range;
    AudioDocument::Snapshot snapshot;
    juce::uint32 version;
    double sampleRate;
    juce::AudioBuffer<float> audio;
    juce::String error;
};

//==============================================================================
/** Runs work on a background thread behind a progress window, then finish() on the message thread
    unless it was cancelled. Used for sample-rate conversion, loudness and restoration. */
class MainComponent::BackgroundJob final : public juce::ThreadWithProgressWindow
{
public:
    using Work = std::function<bool (const std::function<bool (float)>& progress)>;

    BackgroundJob (MainComponent& o, const juce::String& title, Work w, std::function<void()> f)
        : ThreadWithProgressWindow (title, true, true, 30000, "Cancel", &o),
          owner (o), work (std::move (w)), finish (std::move (f))
    {
    }

    void run() override
    {
        completed = work ([this] (float p) { setProgress (p); return ! threadShouldExit(); });
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (completed && ! userPressedCancel)
            finish();

        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (&owner)]
        {
            if (safe != nullptr)
            {
                safe->backgroundJob = nullptr;
                safe->commands.commandStatusChanged();
            }
        });
    }

private:
    MainComponent& owner;
    Work work;
    std::function<void()> finish;
    bool completed = false;
};

//==============================================================================
MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "SpectraBlade";
    options.filenameSuffix      = "settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName          = "SpectraBlade";
    appProperties.setStorageParameters (options);

    // The app used to be called WaveDrill, and before that Waveloom: bring the newest of their
    // settings (devices, plug-in list) across once.
    if (const auto newFile = options.getDefaultFile(); ! newFile.exists())
    {
        for (auto* previousName : { "WaveDrill", "Waveloom" })
        {
            auto old = options;
            old.applicationName = old.folderName = previousName;
            const auto oldFile = old.getDefaultFile();

            if (oldFile.existsAsFile() && newFile.getParentDirectory().createDirectory())
            {
                oldFile.copyFileTo (newFile);
                oldFile.getSiblingFile ("RecentlyCrashedPluginsList").copyFileTo (newFile.getSiblingFile ("RecentlyCrashedPluginsList"));
                break;
            }
        }
    }

    auto* settings = appProperties.getUserSettings();
    deadMansPedalFile = settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");

    if (auto xml = settings->getXmlValue (knownPluginsKey))
        engine.knownPlugins.recreateFromXml (*xml);

    engine.knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    engine.knownPlugins.addChangeListener (this);
    engine.addChangeListener (this);
    engine.document.addChangeListener (this);
    engine.session.addChangeListener (this);

    const auto savedDevice = settings->getXmlValue (audioDeviceKey);
    const auto deviceError = engine.openDevice (savedDevice.get());

    if (deviceError.isNotEmpty())
        showError ("Audio device", deviceError);

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
        juce::Logger::writeToLog ("Audio device: " + device->getName() + ", " + juce::String (device->getCurrentSampleRate())
                                  + " Hz, " + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
    else
        juce::Logger::writeToLog ("Audio device: none");

    engine.setOutputPair (settings->getIntValue (outputPairKey, 0));

    if (const auto count = settings->getIntValue (inputCountKey, 0); count > 0)
        if (const auto error = engine.setInput (settings->getIntValue (inputFirstKey, 0), count); error.isNotEmpty())
            showError ("Recording input", error);

    for (auto* b : { &multitrackButton, &openButton, &playButton, &stopButton, &recordButton, &pluginsButton, &audioButton, &saveRackButton, &loadRackButton })
        addAndMakeVisible (b);

    multitrackButton.setTooltip ("Switch between the file and the multitrack session (Cmd+4)");
    multitrackButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2d5a8a));
    multitrackButton.onClick = [this] { setMultitrack (! multitrackMode); };

    openButton.onClick     = [this] { commands.invokeDirectly (cmdOpen, true); };
    playButton.onClick     = [this] { togglePlay(); };
    stopButton.onClick     = [this]
    {
        if (isRecording())
            stopRecording();
        else
            engine.getPlayer().pause();

        waveform.repaint();
        multitrack.repaint();
    };
    recordButton.onClick   = [this] { toggleRecord(); };
    recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkred);
    recordButton.setTooltip ("Record from the input (Shift+Space). Over a selection it punches in and out with a "
                             + juce::String (prerollSeconds, 0) + " s preroll; otherwise it records from the cursor. "
                             "In the multitrack, the take goes onto the armed track (R), or a new track.");
    pluginsButton.onClick  = [this] { showPluginList(); };
    audioButton.onClick    = [this] { showAudioSettings(); };
    saveRackButton.onClick = [this] { saveRack(); };
    loadRackButton.onClick = [this] { loadRack(); };

    addAndMakeVisible (outputLabel);
    addAndMakeVisible (outputPair);
    outputPair.setTooltip ("Device outputs that receive the rack's stereo signal");
    outputPair.onChange = [this]
    {
        if (outputPair.getSelectedId() > 0)
            engine.setOutputPair (outputPair.getSelectedId() - 1);
    };

    addAndMakeVisible (inputLabel);
    addAndMakeVisible (inputChoice);
    addAndMakeVisible (inputMeter);
    inputChoice.setTooltip ("Device inputs to record from");
    inputMeter.readPeak = [this] (int ch) { return engine.getRecorder().takePeak (ch); };
    inputChoice.onChange = [this]
    {
        const auto id = inputChoice.getSelectedId();

        if (id <= 0 || isRecording())
            return;

        const auto first = id >= monoInputBase ? id - monoInputBase : id >= stereoInputBase ? id - stereoInputBase : 0;
        const auto count = id >= monoInputBase ? 1 : id >= stereoInputBase ? 2 : 0;

        if (const auto error = engine.setInput (first, count); error.isNotEmpty())
            showError ("Recording input", error);

        saveSettings();
    };

    for (auto* l : { &fileLabel, &deviceLabel, &timeLabel, &selectionLabel })
        addAndMakeVisible (l);

    deviceLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    selectionLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain));
    selectionLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));

    addAndMakeVisible (waveform);
    waveform.setDisplay ((WaveformView::Display) juce::jlimit (0, 2, settings->getIntValue (displayKey, 0)));
    waveform.onFileDropped = [this] (const juce::File& f) { confirmDiscardChanges ([this, f] { openFile (f); }); };
    waveform.onSelectionChanged = [this] { commands.commandStatusChanged(); timerCallback(); };

    addChildComponent (multitrack);
    multitrack.setSnapping (settings->getBoolValue (snappingKey, true));
    multitrack.setCrossfading (settings->getBoolValue (crossfadingKey, true));
    multitrack.onOpenInEditor = [this] (const juce::File& f)
    {
        confirmDiscardChanges ([this, f]
        {
            setMultitrack (false);
            openFile (f);
        });
    };
    multitrack.onFilesDropped = [this] (const juce::StringArray& files, int track, juce::int64 frame) { addFilesToSession (files, track, frame); };
    multitrack.onSelectionChanged = [this] { commands.commandStatusChanged(); timerCallback(); };
    multitrack.onEditPlugin = [this] (juce::AudioPluginInstance& p) { showEditor (p); };

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
    {
        SessionState empty;
        empty.sampleRate = device->getCurrentSampleRate();
        engine.clearTrackEffects();
        engine.session.reset (std::move (empty), {});
    }

    addAndMakeVisible (rackView);
    rackView.onEditSlot = [this] (int index) { showEditor (index); };
    engine.onPluginAboutToBeRemoved = [this] (juce::AudioProcessor* p) { closeEditorFor (p); };

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    addKeyListener (commands.getKeyMappings());
    setApplicationCommandManagerToWatch (&commands);   // menus refresh when command state changes
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (this);
   #endif

    // Reopening the last file is quiet: one that can no longer be read is logged and forgotten.
    if (const juce::File last (settings->getValue (lastFileKey)); last.existsAsFile())
    {
        if (const auto error = engine.openFile (last); error.isNotEmpty())
        {
            juce::Logger::writeToLog ("Could not reopen " + last.getFullPathName() + ": " + error);
            settings->removeValue (lastFileKey);
        }
    }

    refreshOutputChoices();
    refreshInputChoices();
    refreshTitle();
    setWantsKeyboardFocus (true);
    setSize (1200, 680);
    startTimerHz (20);
}

MainComponent::~MainComponent()
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    if (isRecording())
        engine.getRecorder().stop();   // the take is lost: the app is closing

    saveSettings();
    renderJob = nullptr;
    backgroundJob = nullptr;
    engine.onPluginAboutToBeRemoved = nullptr;
    engine.document.removeChangeListener (this);
    engine.session.removeChangeListener (this);
    engine.removeChangeListener (this);
    engine.knownPlugins.removeChangeListener (this);
    editorWindows.clear();
    pluginListWindow = nullptr;
}

void MainComponent::saveSettings()
{
    auto* settings = appProperties.getUserSettings();

    if (auto xml = engine.deviceManager.createStateXml())
        settings->setValue (audioDeviceKey, xml.get());

    settings->setValue (outputPairKey, engine.getOutputPair());
    settings->setValue (inputFirstKey, engine.getInputFirstChannel());
    settings->setValue (inputCountKey, engine.getInputNumChannels());
    settings->setValue (displayKey, (int) waveform.getDisplay());
    settings->setValue (lastFileKey, engine.document.getFile().getFullPathName());
    settings->saveIfNeeded();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff111317));
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (10);

    auto toolbar = r.removeFromTop (30);

    for (auto* b : { &multitrackButton, &openButton, &playButton, &stopButton, &recordButton })
    {
        b->setBounds (toolbar.removeFromLeft (b == &multitrackButton ? 96 : b == &openButton || b == &recordButton ? 80 : 64));
        toolbar.removeFromLeft (b == &multitrackButton ? 14 : 6);
    }

    toolbar.removeFromLeft (10);
    timeLabel.setBounds (toolbar.removeFromLeft (120));

    for (auto* b : { &audioButton, &pluginsButton, &loadRackButton, &saveRackButton })
    {
        b->setBounds (toolbar.removeFromRight (b == &audioButton ? 130 : 100));
        toolbar.removeFromRight (6);
    }

    selectionLabel.setBounds (toolbar);

    r.removeFromTop (8);
    auto status = r.removeFromBottom (24);
    outputLabel.setBounds (status.removeFromLeft (52));
    outputPair.setBounds (status.removeFromLeft (240));
    status.removeFromLeft (12);
    inputLabel.setBounds (status.removeFromLeft (42));
    inputChoice.setBounds (status.removeFromLeft (200));
    status.removeFromLeft (6);
    inputMeter.setBounds (status.removeFromLeft (160).reduced (0, 3));
    status.removeFromLeft (12);
    deviceLabel.setBounds (status.removeFromRight (status.getWidth() / 2));
    fileLabel.setBounds (status);
    r.removeFromBottom (8);

    rackView.setBounds (r.removeFromRight (juce::jlimit (300, 420, r.getWidth() / 3)));
    r.removeFromRight (8);
    waveform.setBounds (r);
    multitrack.setBounds (r);
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (int id = cmdOpen; id <= cmdBarsAndBeats; ++id)
        ids.add (id);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const auto& doc = engine.document;
    const auto hasAudio = doc.hasAudio();
    const auto hasSelection = ! waveform.getSelection().isEmpty();

    switch (id)
    {
        case cmdOpen:       info.setInfo ("Open...", "Open an audio file", "File", 0); info.addDefaultKeypress ('o', cmd); break;
        case cmdSave:       info.setInfo ("Save", "Save the file", "File", 0); info.addDefaultKeypress ('s', cmd);
                            info.setActive (hasAudio && doc.isDirty()); break;
        case cmdSaveAs:     info.setInfo ("Save As...", "Save to a new file or format", "File", 0); info.addDefaultKeypress ('s', cmd | shift);
                            info.setActive (hasAudio); break;

        case cmdUndo:       info.setInfo (doc.canUndo() ? "Undo " + doc.getUndoDescription() : juce::String ("Undo"), "Undo", "Edit", 0);
                            info.addDefaultKeypress ('z', cmd); info.setActive (doc.canUndo()); break;
        case cmdRedo:       info.setInfo (doc.canRedo() ? "Redo " + doc.getRedoDescription() : juce::String ("Redo"), "Redo", "Edit", 0);
                            info.addDefaultKeypress ('z', cmd | shift); info.setActive (doc.canRedo()); break;
        case cmdCut:        info.setInfo ("Cut", "Cut the selection", "Edit", 0); info.addDefaultKeypress ('x', cmd); info.setActive (hasSelection); break;
        case cmdCopy:       info.setInfo ("Copy", "Copy the selection", "Edit", 0); info.addDefaultKeypress ('c', cmd); info.setActive (hasSelection); break;
        case cmdPaste:      info.setInfo ("Paste", "Paste at the cursor or over the selection", "Edit", 0); info.addDefaultKeypress ('v', cmd);
                            info.setActive (! clipboard.isEmpty()); break;
        case cmdDelete:     info.setInfo ("Delete", "Delete the selection", "Edit", 0);
                            info.addDefaultKeypress (juce::KeyPress::backspaceKey, 0); info.addDefaultKeypress (juce::KeyPress::deleteKey, 0);
                            info.setActive (hasSelection); break;
        case cmdSelectAll:  info.setInfo ("Select All", "Select the whole file", "Edit", 0); info.addDefaultKeypress ('a', cmd); info.setActive (hasAudio); break;
        case cmdCrop:       info.setInfo ("Crop to Selection", "Keep only the selection", "Edit", 0); info.addDefaultKeypress ('t', cmd); info.setActive (hasSelection); break;

        case cmdGain:       info.setInfo ("Gain...", "Change the level", "Effects", 0); info.setActive (hasAudio); break;
        case cmdNormalize:  info.setInfo ("Normalize...", "Set the peak level", "Effects", 0); info.setActive (hasAudio); break;
        case cmdFadeIn:     info.setInfo ("Fade In", "Fade in over the selection", "Effects", 0); info.setActive (hasSelection); break;
        case cmdFadeOut:    info.setInfo ("Fade Out", "Fade out over the selection", "Effects", 0); info.setActive (hasSelection); break;
        case cmdSilence:    info.setInfo ("Silence", "Replace with silence", "Effects", 0); info.setActive (hasSelection); break;
        case cmdInvert:     info.setInfo ("Invert", "Flip the polarity", "Effects", 0); info.setActive (hasAudio); break;
        case cmdReverse:    info.setInfo ("Reverse", "Play backwards", "Effects", 0); info.setActive (hasAudio); break;
        case cmdApplyRack:  info.setInfo ("Apply Effects Rack", "Render the rack's effects into the audio", "Effects", 0);
                            info.addDefaultKeypress ('r', cmd); info.setActive (hasAudio && engine.getNumRackSlots() > 0); break;

        case cmdPlayPause:  info.setInfo ("Play/Pause", "Play the selection, or from the cursor", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::spaceKey, 0); info.setActive (hasAudio); break;
        case cmdGoToStart:  info.setInfo ("Go to Start", "Move the cursor to the start", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::homeKey, 0); info.setActive (hasAudio); break;
        case cmdGoToEnd:    info.setInfo ("Go to End", "Move the cursor to the end", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::endKey, 0); info.setActive (hasAudio); break;
        case cmdRecord:     info.setInfo (isRecording() ? "Stop Recording" : "Record",
                                          "Record from the input", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::spaceKey, shift); break;

        case cmdShowWaveform: info.setInfo ("Waveform", "Show each channel's waveform", "View", 0);
                            info.addDefaultKeypress ('1', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::waveform); break;
        case cmdShowSpectrogram: info.setInfo ("Spectrogram", "Show each channel's frequencies over time", "View", 0);
                            info.addDefaultKeypress ('2', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::spectrogram); break;
        case cmdShowBoth:   info.setInfo ("Waveform and Spectrogram", "Show both, one above the other", "View", 0);
                            info.addDefaultKeypress ('3', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::both); break;
        case cmdSpectralAttenuate: info.setInfo ("Attenuate Frequencies...", "Lower the level inside the box selected in the spectrogram", "Effects", 0);
                            info.setActive (hasSelection && ! waveform.getSpectralBand().isEmpty()); break;
        case cmdSpectralRemove: info.setInfo ("Remove Frequencies", "Remove what is inside the box selected in the spectrogram", "Effects", 0);
                            info.addDefaultKeypress (juce::KeyPress::backspaceKey, alt);
                            info.setActive (hasSelection && ! waveform.getSpectralBand().isEmpty()); break;

        case cmdLoudnessStats: info.setInfo ("Loudness Statistics...", "Measure loudness (EBU R128) and true peak of the selection or file", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdMatchLoudness: info.setInfo ("Match Loudness...", "Set the integrated loudness of the selection or file", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdCaptureNoisePrint: info.setInfo ("Capture Noise Print", "Learn the noise from a selection of noise alone", "Effects", 0);
            info.addDefaultKeypress ('p', juce::ModifierKeys::shiftModifier);
            info.setActive (waveform.getSelection().getLength() >= NoiseReduction::frameSize); break;
        case cmdNoiseReduction: info.setInfo ("Noise Reduction...", "Reduce the captured noise in the selection or file", "Effects", 0);
            info.addDefaultKeypress ('p', cmd | juce::ModifierKeys::shiftModifier);
            info.setActive (hasAudio && noisePrint.has_value() && backgroundJob == nullptr); break;
        case cmdClickRepair: info.setInfo ("Click and Pop Repair...", "Find clicks and pops in the selection or file and rebuild them", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdMultitrack: info.setInfo ("Multitrack Session", "Show the multitrack session instead of the file", "View", 0);
                            info.addDefaultKeypress ('4', cmd); info.setTicked (multitrackMode); break;
        case cmdNewSession: info.setInfo ("New Session", "Start an empty multitrack session", "File", 0);
                            info.addDefaultKeypress ('n', cmd | shift); break;
        case cmdOpenSession: info.setInfo ("Open Session...", "Open a multitrack session", "File", 0);
                            info.addDefaultKeypress ('o', cmd | shift); break;
        case cmdImportToSession: info.setInfo ("Import Audio...", "Add audio files to the session, each on a new track, at the cursor", "Multitrack", 0);
                            info.addDefaultKeypress ('i', cmd); info.setActive (multitrackMode); break;
        case cmdAddTrack:   info.setInfo ("Add Track", "Add an empty track", "Multitrack", 0);
                            info.addDefaultKeypress ('t', cmd | shift); info.setActive (multitrackMode); break;
        case cmdAddBus:     info.setInfo ("Add Bus", "Add a bus for tracks to send to", "Multitrack", 0);
                            info.addDefaultKeypress ('b', cmd | shift); info.setActive (multitrackMode); break;
        case cmdSnapping:   info.setInfo ("Snapping", "Line dragged clips, edges, fades and points up with clip edges, the cursor and the ruler", "Multitrack", 0);
                            info.addDefaultKeypress ('s', 0); info.setTicked (multitrack.isSnapping()); info.setActive (multitrackMode); break;
        case cmdMetronome:  info.setInfo ("Metronome", "Click on each beat of the session's tempo while it plays", "Multitrack", 0);
                            info.addDefaultKeypress ('c', 0); info.setTicked (engine.getPlayer().isMetronomeOn()); info.setActive (multitrackMode); break;
        case cmdCountIn:    info.setInfo ("Count-In", "Record after one bar of clicks instead of the preroll", "Multitrack", 0);
                            info.setTicked (countIn); info.setActive (multitrackMode && ! isRecording()); break;
        case cmdBarsAndBeats: info.setInfo ("Bars and Beats", "Count the ruler in bars and beats of the tempo, and snap to its beats", "Multitrack", 0);
                            info.addDefaultKeypress ('b', 0); info.setTicked (multitrack.isShowingBars()); info.setActive (multitrackMode); break;
        case cmdTempo:      info.setInfo ("Tempo...", "The session's tempo and beats in a bar", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdInputMonitoring: info.setInfo ("Input Monitoring", "Hear each armed track's input through its effects, volume and pan", "Multitrack", 0);
                            info.addDefaultKeypress ('i', 0); info.setTicked (inputMonitoring); info.setActive (multitrackMode); break;
        case cmdLoopRecording: info.setInfo ("Loop Recording", "Record over the time range again and again, keeping each pass as a take", "Multitrack", 0);
                            info.addDefaultKeypress ('l', 0); info.setTicked (loopRecording);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdWriteAutomation: info.setInfo ("Write Automation", "While playing, dragging a track's volume or pan writes it into its envelope", "Multitrack", 0);
                            info.addDefaultKeypress ('w', 0); info.setTicked (multitrack.isWritingAutomation()); info.setActive (multitrackMode); break;
        case cmdCrossfade:  info.setInfo ("Crossfade Overlaps", "Crossfade a clip moved or trimmed over part of another", "Multitrack", 0);
                            info.addDefaultKeypress ('x', 0); info.setTicked (multitrack.isCrossfading()); info.setActive (multitrackMode); break;
        case cmdMixDown:    info.setInfo ("Mix Down...", "Mix the session, or its time range, to an audio file", "Multitrack", 0);
                            info.addDefaultKeypress ('e', cmd | shift);
                            info.setActive (multitrackMode && backgroundJob == nullptr
                                            && (engine.session.getState()->getLength() > 0 || ! multitrack.getTimeSelection().isEmpty()));
                            break;
        case cmdDeleteTrack: info.setInfo ("Delete Track", "Delete the selected track or bus", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0); break;
        case cmdSplitClip:  info.setInfo ("Split Clip", "Split the selected clip at the cursor", "Multitrack", 0);
                            info.addDefaultKeypress ('k', cmd); info.setActive (multitrackMode && multitrack.getSelectedClip() != 0); break;
        case cmdConvertRate: info.setInfo ("Convert Sample Rate...", "Resample the whole file to another rate", "Edit", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;

        case cmdAddMarker:  info.setInfo (hasSelection ? "Add Region" : "Add Marker",
                                          "Mark the cursor or play position, or make a region of the selection", "Markers", 0);
                            info.addDefaultKeypress ('m', 0); info.setActive (hasAudio); break;
        case cmdPreviousMarker: info.setInfo ("Previous Marker", "Move the cursor to the previous marker", "Markers", 0);
                            info.addDefaultKeypress (juce::KeyPress::leftKey, alt); info.setActive (! doc.getMarkers().empty()); break;
        case cmdNextMarker: info.setInfo ("Next Marker", "Move the cursor to the next marker", "Markers", 0);
                            info.addDefaultKeypress (juce::KeyPress::rightKey, alt); info.setActive (! doc.getMarkers().empty()); break;
        case cmdDeleteAllMarkers: info.setInfo ("Delete All Markers", "Remove every marker and region", "Markers", 0);
                            info.setActive (! doc.getMarkers().empty()); break;

        case cmdZoomIn:     info.setInfo ("Zoom In", "Zoom in", "View", 0); info.addDefaultKeypress ('=', cmd); info.setActive (hasAudio); break;
        case cmdZoomOut:    info.setInfo ("Zoom Out", "Zoom out", "View", 0); info.addDefaultKeypress ('-', cmd); info.setActive (hasAudio); break;
        case cmdZoomFit:    info.setInfo ("Zoom to Fit", "Show the whole file", "View", 0); info.addDefaultKeypress ('0', cmd); info.setActive (hasAudio); break;
        case cmdZoomSelection: info.setInfo ("Zoom to Selection", "Fill the view with the selection", "View", 0);
                            info.addDefaultKeypress ('e', cmd); info.setActive (hasSelection); break;

        case cmdPlugins:    info.setInfo ("Plug-ins...", "Scan and manage plug-ins", "Window", 0); break;
        case cmdAudioSettings: info.setInfo ("Audio Settings...", "Choose the audio interface", "Window", 0); info.addDefaultKeypress (',', cmd); break;
        case cmdSaveRack:   info.setInfo ("Save Rack...", "Save the effects rack", "Window", 0); break;
        case cmdLoadRack:   info.setInfo ("Load Rack...", "Load an effects rack", "Window", 0); break;
        default: break;
    }

    if (multitrackMode)
    {
        const auto& session = engine.session;
        const auto hasClips = session.getState()->getLength() > 0;

        switch (id)
        {
            case cmdSave:   info.shortName = "Save Session"; info.setActive (session.isDirty() || session.getFile() == juce::File()); break;
            case cmdSaveAs: info.shortName = "Save Session As..."; info.setActive (true); break;
            case cmdUndo:   info.shortName = session.canUndo() ? "Undo " + session.getUndoDescription() : juce::String ("Undo");
                            info.setActive (session.canUndo()); break;
            case cmdRedo:   info.shortName = session.canRedo() ? "Redo " + session.getRedoDescription() : juce::String ("Redo");
                            info.setActive (session.canRedo()); break;
            case cmdDelete: info.shortName = "Delete Clip"; info.setActive (multitrack.getSelectedClip() != 0); break;
            case cmdPlayPause: case cmdGoToStart: case cmdGoToEnd: case cmdZoomIn: case cmdZoomOut: case cmdZoomFit:
                            info.setActive (hasClips); break;
            case cmdShowWaveform: case cmdShowSpectrogram: case cmdShowBoth:
                            info.setTicked (false); break;
            default:        if (isEditorOnly (id)) info.setActive (false); break;
        }
    }

    // While recording, the document must stay as it is so the take lands where it was started.
    if (isRecording() && id != cmdRecord && id != cmdPlayPause && id != cmdAddMarker)
        info.setActive (false);
}

bool MainComponent::perform (const InvocationInfo& info)
{
    switch (info.commandID)
    {
        case cmdOpen:       confirmDiscardChanges ([this] { chooseFile(); }); break;
        case cmdSave:       if (multitrackMode) saveSession(); else save(); break;
        case cmdSaveAs:     if (multitrackMode) saveSessionAs(); else saveAs(); break;
        case cmdUndo:       if (multitrackMode) engine.session.undo(); else engine.document.undo(); break;
        case cmdRedo:       if (multitrackMode) engine.session.redo(); else engine.document.redo(); break;
        case cmdCut:        cut(); break;
        case cmdCopy:       copy(); break;
        case cmdPaste:      paste(); break;
        case cmdDelete:     if (multitrackMode) multitrack.deleteSelectedClip(); else deleteSelection(); break;
        case cmdSelectAll:  waveform.selectAll(); break;
        case cmdCrop:       cropToSelection(); break;
        case cmdGain:       applyGain(); break;
        case cmdNormalize:  applyNormalize(); break;
        case cmdFadeIn:     applySimpleEffect ("Fade In", &AudioEdits::fadeIn, true); break;
        case cmdFadeOut:    applySimpleEffect ("Fade Out", &AudioEdits::fadeOut, true); break;
        case cmdSilence:    applySimpleEffect ("Silence", &AudioEdits::silence, true); break;
        case cmdInvert:     applySimpleEffect ("Invert", &AudioEdits::invert, false); break;
        case cmdReverse:    applySimpleEffect ("Reverse", &AudioEdits::reverse, false); break;
        case cmdApplyRack:  applyRackToSelection(); break;
        case cmdPlayPause:  togglePlay(); break;
        case cmdGoToStart:  if (multitrackMode) multitrack.setCursor (0); else waveform.setCursor (0); break;
        case cmdGoToEnd:    if (multitrackMode) multitrack.setCursor (engine.session.getState()->getLength());
                            else waveform.setCursor (engine.document.getNumFrames());
                            break;
        case cmdZoomIn:     if (multitrackMode) multitrack.zoomIn(); else waveform.zoomIn(); break;
        case cmdZoomOut:    if (multitrackMode) multitrack.zoomOut(); else waveform.zoomOut(); break;
        case cmdZoomFit:    if (multitrackMode) multitrack.zoomToFit(); else waveform.zoomToFit(); break;
        case cmdZoomSelection: waveform.zoomToSelection(); break;
        case cmdPlugins:    showPluginList(); break;
        case cmdAudioSettings: showAudioSettings(); break;
        case cmdSaveRack:   saveRack(); break;
        case cmdLoadRack:   loadRack(); break;
        case cmdRecord:     toggleRecord(); break;
        case cmdAddMarker:  addMarker(); break;
        case cmdConvertRate: chooseSampleRate(); break;
        case cmdShowWaveform: setMultitrack (false); setDisplay (WaveformView::Display::waveform); break;
        case cmdShowSpectrogram: setMultitrack (false); setDisplay (WaveformView::Display::spectrogram); break;
        case cmdShowBoth:   setMultitrack (false); setDisplay (WaveformView::Display::both); break;
        case cmdMultitrack: setMultitrack (! multitrackMode); break;
        case cmdNewSession: confirmDiscardSession ([this] { newSession(); }); break;
        case cmdOpenSession: confirmDiscardSession ([this] { chooseSession(); }); break;
        case cmdImportToSession: chooseFilesForSession(); break;
        case cmdAddTrack:
        {
            auto next = *engine.session.getState();
            next.addTrack();
            engine.session.apply (std::move (next), "Add Track");
            break;
        }
        case cmdAddBus:
        {
            auto next = *engine.session.getState();
            next.addBus();
            engine.session.apply (std::move (next), "Add Bus");
            break;
        }
        case cmdDeleteTrack: multitrack.deleteSelectedTrack(); break;
        case cmdSnapping:
            multitrack.setSnapping (! multitrack.isSnapping());
            appProperties.getUserSettings()->setValue (snappingKey, multitrack.isSnapping());
            commands.commandStatusChanged();
            break;
        case cmdSplitClip:  multitrack.splitSelectedClip(); break;
        case cmdMixDown:    chooseMixDownFile(); break;
        case cmdMetronome:
            engine.getPlayer().setMetronome (! engine.getPlayer().isMetronomeOn());
            commands.commandStatusChanged();
            break;
        case cmdCountIn:
            countIn = ! countIn;
            commands.commandStatusChanged();
            break;
        case cmdTempo:
            showTempoDialog();
            break;
        case cmdBarsAndBeats:
            multitrack.setShowBars (! multitrack.isShowingBars());
            commands.commandStatusChanged();
            break;
        case cmdInputMonitoring:
            inputMonitoring = ! inputMonitoring;
            updateInputs();
            commands.commandStatusChanged();
            break;
        case cmdLoopRecording:
            loopRecording = ! loopRecording;
            commands.commandStatusChanged();
            break;
        case cmdWriteAutomation:
            multitrack.setWritingAutomation (! multitrack.isWritingAutomation());
            commands.commandStatusChanged();
            break;
        case cmdCrossfade:
            multitrack.setCrossfading (! multitrack.isCrossfading());
            appProperties.getUserSettings()->setValue (crossfadingKey, multitrack.isCrossfading());
            commands.commandStatusChanged();
            break;
        case cmdSpectralAttenuate:
            askForDecibels ("Attenuate Frequencies", "Change (dB)", -12.0f, [this] (float db) { applySpectral (db); });
            break;
        case cmdSpectralRemove: applySpectral (-std::numeric_limits<float>::infinity()); break;
        case cmdLoudnessStats: showLoudness(); break;
        case cmdMatchLoudness: askForLoudnessTarget(); break;
        case cmdCaptureNoisePrint: captureNoisePrint(); break;
        case cmdNoiseReduction: askForNoiseReduction(); break;
        case cmdClickRepair: askForClickRepair(); break;
        case cmdPreviousMarker: goToMarker (false); break;
        case cmdNextMarker: goToMarker (true); break;
        case cmdDeleteAllMarkers: engine.document.setMarkers ({}, "Delete All Markers"); break;
        default:            return false;
    }

    return true;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Effects", "View", "Multitrack", "Transport", "Markers", "Window" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu menu;
    auto add = [&] (std::initializer_list<int> ids)
    {
        for (auto id : ids)
        {
            if (id == 0)
                menu.addSeparator();
            else
                menu.addCommandItem (&commands, id);
        }
    };

    switch (index)
    {
        case 0: add ({ cmdOpen, 0, cmdSave, cmdSaveAs, 0, cmdNewSession, cmdOpenSession }); break;
        case 1: add ({ cmdUndo, cmdRedo, 0, cmdCut, cmdCopy, cmdPaste, cmdDelete, 0, cmdSelectAll, cmdCrop, 0, cmdConvertRate }); break;
        case 2: add ({ cmdGain, cmdNormalize, cmdMatchLoudness, cmdLoudnessStats, 0, cmdFadeIn, cmdFadeOut, 0, cmdSilence, cmdInvert, cmdReverse, 0,
                       cmdSpectralAttenuate, cmdSpectralRemove, 0, cmdCaptureNoisePrint, cmdNoiseReduction, cmdClickRepair, 0, cmdApplyRack }); break;
        case 3: add ({ cmdShowWaveform, cmdShowSpectrogram, cmdShowBoth, cmdMultitrack, 0, cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdZoomSelection }); break;
        case 4: add ({ cmdImportToSession, 0, cmdAddTrack, cmdAddBus, cmdDeleteTrack, 0, cmdSplitClip, 0, cmdSnapping, cmdCrossfade, cmdWriteAutomation, cmdLoopRecording, cmdInputMonitoring, 0, cmdMetronome, cmdCountIn, cmdTempo, cmdBarsAndBeats, 0, cmdMixDown }); break;
        case 5: add ({ cmdPlayPause, cmdRecord, 0, cmdGoToStart, cmdGoToEnd }); break;
        case 6: add ({ cmdAddMarker, 0, cmdPreviousMarker, cmdNextMarker, 0, cmdDeleteAllMarkers }); break;
        case 7: add ({ cmdPlugins, cmdAudioSettings, 0, cmdSaveRack, cmdLoadRack }); break;
        default: break;
    }

    return menu;
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.knownPlugins)
    {
        if (auto xml = engine.knownPlugins.createXml())
        {
            appProperties.getUserSettings()->setValue (knownPluginsKey, xml.get());
            appProperties.saveIfNeeded();
        }

        return;
    }

    if (source == &engine.document || source == &engine.session)
    {
        refreshTitle();
        commands.commandStatusChanged();
        return;
    }

    refreshOutputChoices();
    refreshInputChoices();
    commands.commandStatusChanged();
}

bool MainComponent::isRecording()
{
    const auto recorders = engine.getRecorders();
    return std::any_of (recorders.begin(), recorders.end(), [] (auto* r) { return r->isRecording(); });
}

void MainComponent::updateInputs()
{
    // Arming and track inputs can change at any time: keep the armed tracks' inputs on (not while
    // recording, when the recorders must stay as they are) and monitored when that is on.
    const auto state = engine.session.getState();
    std::vector<TrackInput> inputs;
    std::vector<std::pair<juce::uint32, TrackInput>> monitored;

    for (auto id : multitrack.getArmedTracks())
    {
        if (const auto* t = state->findTrack (id))
        {
            inputs.push_back (t->input);
            monitored.emplace_back (id, t->input);
        }
    }

    if (! isRecording())
    {
        if (const auto error = engine.setTrackInputs (inputs); error.isNotEmpty() && error != lastInputError)
            juce::Logger::writeToLog ("Track inputs: " + error);
        else if (error.isEmpty())
            lastInputError = {};
    }

    engine.setMonitoredTracks (inputMonitoring && multitrackMode ? monitored : std::vector<std::pair<juce::uint32, TrackInput>>());
}

void MainComponent::timerCallback()
{
    auto& player = engine.getPlayer();
    auto& recorder = engine.getRecorder();

    updateInputs();

    if (multitrackMode)
    {
        const auto rate = engine.session.getSampleRate();
        recordButton.setToggleState (isRecording(), juce::dontSendNotification);

        if (isRecording() && recordingSession)
        {
            // The take so far, as it will land: what has come back through the input by now.
            const auto played = player.getPlayedFrame().load() - recordLatency;
            const auto end = punchOut >= 0 ? juce::jmin (played, punchOut) : played;
            multitrack.showRecording ({ punchIn, juce::jmax (punchIn, end) });

            // A loop records until it is stopped.
            if (! recordingLoop && punchOut >= 0 && played >= punchOut + (juce::int64) (rate * 0.25))
            {
                stopRecording();
                return;
            }
        }

        const auto pos = player.isPlaying() ? player.getPositionFrames() : multitrack.getCursor();
        timeLabel.setText ((isRecording() ? "REC " : "") + formatTime (pos, rate), juce::dontSendNotification);
        playButton.setButtonText (player.isPlaying() ? "Pause" : "Play");

        const auto state = engine.session.getState();
        juce::String clipText;

        if (const auto where = state->findClip (multitrack.getSelectedClip()))
        {
            const auto& clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];
            clipText = clip.name + "  " + formatTime (clip.position, rate) + " - " + formatTime (clip.getEnd(), rate)
                       + "  (" + formatTime (clip.length, rate) + ")";
        }

        selectionLabel.setText (clipText, juce::dontSendNotification);
        return;
    }

    const auto rate = engine.document.getSampleRate();

    if (recorder.isRecording())
    {
        // A punch-out stops by itself once the end of the range has come back through the input.
        const auto margin = (juce::int64) (rate * 0.25);

        if (punchOut >= 0 && player.getPlayedFrame().load() >= punchOut + recordLatency + margin)
        {
            stopRecording();
            return;
        }

        timeLabel.setText ("REC " + formatTime (recorder.getRecordedFrames(), rate), juce::dontSendNotification);
    }
    else
    {
        const auto pos = player.isPlaying() ? player.getPositionFrames() : waveform.getCursor();
        timeLabel.setText (formatTime (pos, rate), juce::dontSendNotification);
    }

    playButton.setButtonText (player.isPlaying() ? "Pause" : "Play");
    recordButton.setToggleState (recorder.isRecording(), juce::dontSendNotification);

    const auto sel = waveform.getSelection();
    selectionLabel.setText (sel.isEmpty() ? juce::String()
                                          : "Selection  " + formatTime (sel.getStart(), rate) + " - " + formatTime (sel.getEnd(), rate)
                                              + "  (" + formatTime (sel.getLength(), rate) + ")",
                            juce::dontSendNotification);
}

void MainComponent::refreshTitle()
{
    multitrackButton.setToggleState (multitrackMode, juce::dontSendNotification);

    if (multitrackMode)
    {
        auto& session = engine.session;
        const auto state = session.getState();
        const auto name = session.getFile() == juce::File() ? juce::String ("Untitled Session") : session.getFile().getFileName();
        fileLabel.setText (name + "  |  " + juce::String (state->sampleRate / 1000.0, 1) + " kHz, "
                               + juce::String ((int) state->tracks.size()) + (state->tracks.size() == 1 ? " track" : " tracks"),
                           juce::dontSendNotification);

        if (onTitleChanged != nullptr)
            onTitleChanged ((session.isDirty() ? "* " : "") + name + " - SpectraBlade");

        return;
    }

    auto& doc = engine.document;
    const auto name = doc.getFile() == juce::File() ? juce::String ("Untitled") : doc.getFile().getFileName();
    fileLabel.setText (doc.hasAudio() ? name + "  |  " + juce::String (doc.getSampleRate() / 1000.0, 1) + " kHz, "
                                          + juce::String (doc.getNumChannels()) + " ch, "
                                          + (doc.getSourceBitDepth() == 32 ? juce::String ("32-bit float")
                                                                            : juce::String (doc.getSourceBitDepth()) + "-bit")
                                      : juce::String(),
                       juce::dontSendNotification);

    if (onTitleChanged != nullptr)
        onTitleChanged ((doc.isDirty() ? "* " : "") + name + " - SpectraBlade");
}

void MainComponent::refreshOutputChoices()
{
    outputPair.clear (juce::dontSendNotification);
    const auto names = engine.getDeviceOutputChannelNames();

    // Offer every adjacent pair, starting on odd channels (1/2, 3/4, ...) as interfaces label them.
    for (int ch = 0; ch < names.size(); ch += 2)
    {
        const auto label = ch + 1 < names.size() ? names[ch] + " / " + names[ch + 1] : names[ch] + " (mono)";
        outputPair.addItem (label, ch + 1);
    }

    outputPair.setSelectedId (engine.getOutputPair() + 1, juce::dontSendNotification);

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
    {
        deviceLabel.setText (device->getName()
                               + "  |  " + juce::String (device->getCurrentSampleRate() / 1000.0, 1) + " kHz"
                               + "  |  " + juce::String (device->getCurrentBufferSizeSamples()) + " samples"
                               + "  |  " + juce::String (device->getOutputLatencyInSamples()) + " out latency",
                             juce::dontSendNotification);
    }
    else
    {
        deviceLabel.setText ("No audio device", juce::dontSendNotification);
    }
}

void MainComponent::refreshInputChoices()
{
    inputChoice.clear (juce::dontSendNotification);
    inputChoice.addItem ("No input", noInputId);
    const auto names = engine.getDeviceInputChannelNames();

    for (int ch = 0; ch + 1 < names.size(); ch += 2)
        inputChoice.addItem (names[ch] + " / " + names[ch + 1], stereoInputBase + ch);

    inputChoice.addSeparator();

    for (int ch = 0; ch < names.size(); ++ch)
        inputChoice.addItem (names[ch] + " (mono)", monoInputBase + ch);

    const auto first = engine.getInputFirstChannel();
    const auto count = engine.getInputNumChannels();
    inputChoice.setSelectedId (count == 2 ? stereoInputBase + first : count == 1 ? monoInputBase + first : noInputId,
                               juce::dontSendNotification);
    inputChoice.setEnabled (! isRecording());
    inputMeter.setNumChannels (engine.getRecorder().isInputActive() ? count : 0);
}

//==============================================================================
void MainComponent::confirmDiscardChanges (std::function<void()> proceed)
{
    if (! engine.document.isDirty() || ! engine.document.hasAudio())
    {
        proceed();
        return;
    }

    askThreeWay ("Unsaved changes",
                 "Save changes to " + engine.document.getFile().getFileName() + " first?",
                 "Save", "Don't Save",
                 [this, then = std::move (proceed)] (int result)
                 {
                     if (result == 2)
                     {
                         then();
                     }
                     else if (result == 1)
                     {
                         const auto file = engine.document.getFile();
                         const auto depth = engine.document.getSourceBitDepth();

                         if (AudioFileIO::bitDepthsFor (file, engine.formatManager).contains (depth))
                             writeFile (file, depth, [then] (bool saved) { if (saved) then(); });
                         else
                             saveAs ([then] (bool saved) { if (saved) then(); });
                     }
                 });
}

void MainComponent::confirmQuit (std::function<void()> proceed)
{
    confirmDiscardChanges ([this, then = std::move (proceed)] { confirmDiscardSession (then); });
}

//==============================================================================
void MainComponent::setMultitrack (bool on)
{
    if (on == multitrackMode || isRecording())
        return;

    engine.getPlayer().pause();
    multitrackMode = on;
    engine.getPlayer().setMode (on ? DocumentPlayerProcessor::Mode::session : DocumentPlayerProcessor::Mode::document);
    waveform.setVisible (! on);
    multitrack.setVisible (on);
    refreshTitle();
    timerCallback();
    commands.commandStatusChanged();
}

void MainComponent::confirmDiscardSession (std::function<void()> proceed)
{
    auto& session = engine.session;

    if (! session.isDirty() || session.getState()->tracks.empty())
    {
        proceed();
        return;
    }

    const auto name = session.getFile() == juce::File() ? juce::String ("the session") : session.getFile().getFileName();
    askThreeWay ("Unsaved session", "Save changes to " + name + " first?", "Save", "Don't Save",
                 [this, then = std::move (proceed)] (int result)
                 {
                     if (result == 2)
                         then();
                     else if (result == 1)
                         saveSession ([then] (bool saved) { if (saved) then(); });
                 });
}

void MainComponent::newSession()
{
    engine.getPlayer().pause();
    SessionState empty;
    empty.sampleRate = engine.deviceManager.getCurrentAudioDevice() != nullptr
                           ? engine.deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                           : engine.session.getSampleRate();
    engine.clearTrackEffects();
    engine.session.reset (std::move (empty), {});
    multitrack.setCursor (0);
    setMultitrack (true);
    refreshTitle();
}

void MainComponent::chooseSession()
{
    chooser = std::make_unique<juce::FileChooser> ("Open session", engine.session.getFile().getParentDirectory(), "*.sbsession");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  openSession (fc.getResult());
                          });
}

void MainComponent::openSession (const juce::File& file)
{
    auto loaded = SessionFile::load (file, engine.formatManager);

    if (loaded.error.isNotEmpty())
    {
        showError ("Can't open session", loaded.error);
        return;
    }

    engine.getPlayer().pause();
    engine.clearTrackEffects();
    engine.session.reset (std::move (loaded.state), file);
    multitrack.setCursor (0);
    setMultitrack (true);
    multitrack.zoomToFit();
    refreshTitle();

    if (! loaded.missing.isEmpty())
        showError ("Missing audio", "These files could not be read, so their clips were left out:\n\n"
                                        + loaded.missing.joinIntoString ("\n"));

    engine.loadTrackEffects (std::move (loaded.effects), [this] (const juce::String& errors)
    {
        engine.session.sendChangeMessage();   // the track headers show the effects

        if (errors.isNotEmpty())
            showError ("Track effects", "Some track effects could not be loaded:\n\n" + errors);
    });
}

void MainComponent::saveSession (std::function<void (bool)> done)
{
    const auto file = engine.session.getFile();

    if (file == juce::File())
    {
        saveSessionAs (std::move (done));
        return;
    }

    const auto error = SessionFile::save (*engine.session.getState(), file, [this] (juce::uint32 id) { return engine.getTrackEffects().createXml (id); });

    if (error.isNotEmpty())
        showError ("Save Session", error);
    else
        engine.session.markSaved (file);

    if (done != nullptr)
        done (error.isEmpty());
}

void MainComponent::chooseMixDownFile()
{
    const auto session = engine.session.getFile();
    const auto suggested = session != juce::File() ? session.withFileExtension ("wav")
                                                   : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Mixdown.wav");

    chooser = std::make_unique<juce::FileChooser> ("Mix down to", suggested, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! file.hasFileExtension ("wav;aif;aiff;flac"))
                                  file = file.withFileExtension ("wav");

                              askForMixDownFormat (file);
                          });
}

void MainComponent::askForMixDownFormat (const juce::File& file)
{
    const auto depths = AudioFileIO::bitDepthsFor (file, engine.formatManager);

    if (depths.isEmpty())
    {
        showError ("Mix Down", "SpectraBlade can't write " + file.getFileExtension() + " files.");
        return;
    }

    // The time range if there is one, otherwise the whole session from its start.
    const auto timeRange = multitrack.getTimeSelection();
    const auto range = ! timeRange.isEmpty() ? timeRange : juce::Range<juce::int64> (0, engine.session.getState()->getLength());
    const auto rate = engine.session.getSampleRate();
    const auto seconds = [rate] (juce::int64 frame) { return juce::String ((double) frame / rate, 2) + " s"; };
    const auto what = ! timeRange.isEmpty() ? "the time range, " + seconds (range.getStart()) + " to " + seconds (range.getEnd())
                                            : "the whole session, " + seconds (range.getLength());

    auto* box = new juce::AlertWindow ("Mix Down", "Mixes " + what + ", as it plays, to " + file.getFileName() + ".",
                                       juce::MessageBoxIconType::NoIcon);
    juce::StringArray names;

    for (auto d : depths)
        names.add (d == 32 ? juce::String ("32-bit float") : juce::String (d) + "-bit (dithered)");

    box->addComboBox ("depth", names, "Sample format");
    box->getComboBoxComponent ("depth")->setSelectedItemIndex (0);

    // Effects such as reverbs ring on past the last clip: the tail keeps that.
    const std::array<double, 5> tails { 0.0, 1.0, 2.0, 5.0, 10.0 };
    box->addComboBox ("tail", { "None", "1 s", "2 s", "5 s", "10 s" }, "Tail after the end");
    box->getComboBoxComponent ("tail")->setSelectedItemIndex (0);
    box->addButton ("Mix Down", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, box, file, depths, range, rate, tails] (int result)
        {
            if (result != 1)
                return;

            const auto depth = depths[box->getComboBoxComponent ("depth")->getSelectedItemIndex()];
            const auto tail = (juce::int64) std::llround (tails[(size_t) box->getComboBoxComponent ("tail")->getSelectedItemIndex()] * rate);
            mixDown (file, depth, range.withEnd (range.getEnd() + tail));
        }), true);
}

void MainComponent::showTempoDialog()
{
    const auto state = engine.session.getState();
    auto* box = new juce::AlertWindow ("Tempo", "The metronome clicks on this tempo, from the start of the session.",
                                       juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("tempo", juce::String (state->tempo, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."), "Beats a minute (20 to 400)");
    box->addTextEditor ("beats", juce::String (state->beatsPerBar), "Beats in a bar (1 to 16)");
    box->getTextEditor ("tempo")->setInputRestrictions (7, ".0123456789");
    box->getTextEditor ("beats")->setInputRestrictions (2, "0123456789");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, box] (int result)
        {
            if (result != 1)
                return;

            const auto tempo = box->getTextEditorContents ("tempo").getDoubleValue();
            const auto beats = box->getTextEditorContents ("beats").getIntValue();

            if (tempo < 20.0 || tempo > 400.0 || beats < 1 || beats > 16)
            {
                showError ("Tempo", "The tempo must be 20 to 400 beats a minute, with 1 to 16 beats in a bar.");
                return;
            }

            auto next = *engine.session.getState();
            next.tempo = tempo;
            next.beatsPerBar = beats;
            engine.session.apply (std::move (next), "Tempo");
        }), true);
}

void MainComponent::mixDown (const juce::File& file, int bitDepth, juce::Range<juce::int64> range)
{
    const auto state = engine.session.getState();
    const auto rate = engine.session.getSampleRate();
    auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

    for (auto& track : state->tracks)
        if (auto xml = engine.getTrackEffects().createXml (track.id))
            (*effects)[track.id] = std::move (xml);

    // Fresh plug-ins, made and run on the job's thread. The job is deleted on the message thread,
    // and these with it, as some plug-ins require.
    auto inserts = std::make_shared<TrackEffects>();
    auto error = std::make_shared<juce::String>();
    auto warnings = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Mixing down...",
        [this, state, rate, effects, inserts, error, warnings, file, bitDepth, range] (const std::function<bool (float)>& progress)
        {
            *warnings = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts);
            juce::AudioBuffer<float> mix;

            if (! OfflineRender::mixSession (*state, range, mix, inserts.get(), progress))
                return false;

            *error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), rate, file, bitDepth, engine.formatManager);
            return true;
        },
        [error, warnings, file]
        {
            if (error->isNotEmpty())
                showError ("Mix Down", *error);
            else if (warnings->isNotEmpty())
                showError ("Mix Down", file.getFileName() + " was written without these effects, which could not be loaded:\n" + *warnings);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::saveSessionAs (std::function<void (bool)> done)
{
    const auto current = engine.session.getFile();
    chooser = std::make_unique<juce::FileChooser> ("Save session",
                                                   current != juce::File() ? current
                                                                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                                                                 .getChildFile ("Untitled.sbsession"),
                                                   "*.sbsession");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, done] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                              {
                                  if (done != nullptr) done (false);
                                  return;
                              }

                              file = file.withFileExtension ("sbsession");
                              const auto error = SessionFile::save (*engine.session.getState(), file, [this] (juce::uint32 id) { return engine.getTrackEffects().createXml (id); });

                              if (error.isNotEmpty())
                                  showError ("Save Session", error);
                              else
                                  engine.session.markSaved (file);

                              if (done != nullptr)
                                  done (error.isEmpty());
                          });
}

void MainComponent::chooseFilesForSession()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio", engine.document.getFile().getParentDirectory(),
                                                   engine.formatManager.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray paths;

                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());

                              if (! paths.isEmpty())
                                  addFilesToSession (paths, -1, multitrack.getCursor());
                          });
}

void MainComponent::addFilesToSession (const juce::StringArray& files, int track, juce::int64 frame)
{
    auto next = *engine.session.getState();
    juce::StringArray errors;
    int added = 0;

    for (auto& path : files)
    {
        const juce::File file (path);
        auto loaded = AudioFileIO::load (file, engine.formatManager);

        if (loaded.error.isNotEmpty())
        {
            errors.add (file.getFileName() + ": " + loaded.error);
            continue;
        }

        // An empty session takes the first file's rate; later files are converted to it.
        if (next.getLength() == 0 && added == 0)
            next.sampleRate = loaded.sampleRate;
        else if (! juce::exactlyEqual (loaded.sampleRate, next.sampleRate))
            loaded.snapshot = SampleRateConverter::convert (*loaded.snapshot, loaded.sampleRate, next.sampleRate);

        // The first file goes on the track it was dropped on; the rest on new tracks.
        SessionEdits::addClip (next, added == 0 ? track : -1, file, loaded.snapshot, frame);
        ++added;
    }

    if (added > 0)
    {
        engine.getPlayer().pause();
        engine.session.apply (std::move (next), added == 1 ? "Import Audio" : "Import " + juce::String (added) + " Files");
        setMultitrack (true);
    }

    if (! errors.isEmpty())
        showError ("Import Audio", errors.joinIntoString ("\n"));
}

void MainComponent::openFile (const juce::File& file)
{
    const auto error = engine.openFile (file);

    if (error.isNotEmpty())
        showError ("Can't open file", file.getFullPathName() + "\n\n" + error);
    else
        setMultitrack (false);
}

void MainComponent::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Open audio file", engine.document.getFile().getParentDirectory(),
                                                   engine.formatManager.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  openFile (fc.getResult());
                          });
}

void MainComponent::save()
{
    auto& doc = engine.document;

    // Formats we can't write (MP3, for example) or a depth the format can't hold go through Save As.
    if (doc.getFile().existsAsFile() && AudioFileIO::bitDepthsFor (doc.getFile(), engine.formatManager).contains (doc.getSourceBitDepth()))
        writeFile (doc.getFile(), doc.getSourceBitDepth(), {});
    else
        saveAs();
}

void MainComponent::saveAs (std::function<void (bool)> done)
{
    auto& doc = engine.document;
    const auto suggested = doc.getFile().withFileExtension (doc.getFile().hasFileExtension ("mp3;ogg") ? "wav" : doc.getFile().getFileExtension());

    chooser = std::make_unique<juce::FileChooser> ("Save audio as", suggested, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, done] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                              {
                                  if (done != nullptr) done (false);
                                  return;
                              }

                              if (! file.hasFileExtension ("wav;aif;aiff;flac"))
                                  file = file.withFileExtension ("wav");

                              const auto depths = AudioFileIO::bitDepthsFor (file, engine.formatManager);

                              if (depths.isEmpty())
                              {
                                  showError ("Save As", "SpectraBlade can't write " + file.getFileExtension() + " files.");
                                  if (done != nullptr) done (false);
                                  return;
                              }

                              auto* box = new juce::AlertWindow ("Save As", "Sample format for " + file.getFileName(),
                                                                 juce::MessageBoxIconType::NoIcon);
                              juce::StringArray names;

                              for (auto d : depths)
                                  names.add (d == 32 ? juce::String ("32-bit float") : juce::String (d) + "-bit (dithered)");

                              box->addComboBox ("depth", names);
                              const auto current = depths.indexOf (engine.document.getSourceBitDepth());
                              box->getComboBoxComponent ("depth")->setSelectedItemIndex (current >= 0 ? current : 0);
                              box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
                              box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                              box->enterModalState (true, juce::ModalCallbackFunction::create (
                                  [this, box, file, depths, done] (int result)
                                  {
                                      if (result != 1)
                                      {
                                          if (done != nullptr) done (false);
                                          return;
                                      }

                                      const auto depth = depths[box->getComboBoxComponent ("depth")->getSelectedItemIndex()];
                                      writeFile (file, depth, done);
                                  }), true);
                          });
}

void MainComponent::writeFile (const juce::File& file, int bitDepth, std::function<void (bool)> done)
{
    auto& doc = engine.document;
    const auto error = AudioFileIO::save (*doc.getSnapshot(), doc.getSampleRate(), file, bitDepth, engine.formatManager,
                                          doc.getMarkers());

    if (error.isNotEmpty())
    {
        showError ("Save failed", error);
    }
    else
    {
        doc.markSaved (file, bitDepth);

        if (! doc.getMarkers().empty() && ! AudioFileIO::canStoreMarkers (file))
            showError ("Markers not saved", file.getFileExtension().toUpperCase().trimCharactersAtStart (".")
                                              + " files cannot hold markers. Save as WAV to keep them.");
    }

    if (done != nullptr)
        done (error.isEmpty());
}

//==============================================================================
juce::Range<juce::int64> MainComponent::selectionOrAll() const
{
    const auto sel = waveform.getSelection();
    return sel.isEmpty() ? juce::Range<juce::int64> (0, engine.document.getNumFrames()) : sel;
}

void MainComponent::applyEdit (AudioDocument::Snapshot next, const juce::String& description, juce::Range<juce::int64> newSelection)
{
    if (next == nullptr)
        return;

    engine.document.apply (std::move (next), description);
    waveform.setSelection (newSelection);
}

void MainComponent::applyEdit (AudioDocument::Snapshot next, const juce::String& description,
                               juce::Range<juce::int64> newSelection, Markers markers)
{
    if (next == nullptr)
        return;

    engine.document.apply (std::move (next), description, std::move (markers));
    waveform.setSelection (newSelection);
}

void MainComponent::setDisplay (WaveformView::Display d)
{
    waveform.setDisplay (d);
    commands.commandStatusChanged();
    saveSettings();
}

void MainComponent::applySpectral (float gainDb)
{
    const auto sel = waveform.getSelection();
    const auto band = waveform.getSpectralBand();

    if (sel.isEmpty() || band.isEmpty())
        return;

    auto& doc = engine.document;
    const auto name = std::isinf (gainDb) ? juce::String ("Remove Frequencies")
                                          : "Attenuate Frequencies " + juce::String (gainDb, 1) + " dB";
    applyEdit (SpectralEdit::changeBand (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), band, doc.getSampleRate(), gainDb),
               name, sel);
}

void MainComponent::addMarker()
{
    auto& doc = engine.document;
    auto& player = engine.getPlayer();
    const auto sel = waveform.getSelection();
    auto markers = doc.getMarkers();

    // During playback (or recording) M marks what is playing; otherwise the cursor or selection.
    Marker m;

    if (player.isPlaying())
        m.start = juce::jlimit ((juce::int64) 0, doc.getNumFrames(), player.getPositionFrames());
    else if (! sel.isEmpty())
        m = { sel.getStart(), sel.getLength(), {} };
    else
        m.start = waveform.getCursor();

    m.name = MarkerEdits::nextName (markers, m.isRegion());
    markers.push_back (m);
    doc.setMarkers (std::move (markers), "Add " + m.name);
}

void MainComponent::goToMarker (bool forwards)
{
    const auto& markers = engine.document.getMarkers();
    const auto cursor = waveform.getCursor();
    std::optional<juce::int64> target;

    for (const auto& m : markers)
    {
        for (auto frame : { m.start, m.isRegion() ? m.getEnd() : m.start })
        {
            if (forwards ? frame > cursor && (! target || frame < *target)
                         : frame < cursor && (! target || frame > *target))
                target = frame;
        }
    }

    if (target)
        waveform.setCursor (*target);
}

void MainComponent::copy()
{
    const auto sel = waveform.getSelection();

    if (! sel.isEmpty())
        clipboard = AudioEdits::copy (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd(), engine.document.getSampleRate());

    commands.commandStatusChanged();
}

void MainComponent::cut()
{
    const auto sel = waveform.getSelection();

    if (sel.isEmpty())
        return;

    copy();
    applyEdit (AudioEdits::remove (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd()), "Cut", { sel.getStart(), sel.getStart() },
               MarkerEdits::afterReplace (engine.document.getMarkers(), sel.getStart(), sel.getEnd(), 0));
}

void MainComponent::paste()
{
    if (clipboard.isEmpty())
        return;

    auto& doc = engine.document;

    if (doc.hasAudio() && ! juce::exactlyEqual (clipboard.sampleRate, doc.getSampleRate()))
    {
        // Convert a copy of the clipboard to this file's rate first, then paste it.
        if (backgroundJob != nullptr)
            return;

        auto converted = std::make_shared<AudioClip>();
        const auto source = clipboard;
        const auto rate = doc.getSampleRate();
        const auto version = doc.getVersion();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Converting the clipboard to " + juce::String (rate / 1000.0, 1) + " kHz...",
            [converted, source, rate] (const std::function<bool (float)>& progress)
            {
                *converted = SampleRateConverter::convert (source, rate, progress);
                return converted->numChannels > 0;
            },
            [this, converted, version]
            {
                if (engine.document.getVersion() != version)
                    showError ("Paste", "The audio changed while the clipboard was converted, so nothing was pasted.");
                else
                    pasteClip (*converted);
            });
        backgroundJob->launchThread();
        return;
    }

    pasteClip (clipboard);
}

void MainComponent::pasteClip (const AudioClip& clip)
{
    auto& doc = engine.document;
    auto sel = waveform.getSelection();

    if (sel.isEmpty())
        sel = { waveform.getCursor(), waveform.getCursor() };

    applyEdit (AudioEdits::replaceWithClip (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), clip),
               "Paste", { sel.getStart(), sel.getStart() + clip.getNumFrames() },
               MarkerEdits::afterReplace (doc.getMarkers(), sel.getStart(), sel.getEnd(), clip.getNumFrames()));
}

void MainComponent::deleteSelection()
{
    const auto sel = waveform.getSelection();

    if (! sel.isEmpty())
        applyEdit (AudioEdits::remove (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd()), "Delete", { sel.getStart(), sel.getStart() },
                   MarkerEdits::afterReplace (engine.document.getMarkers(), sel.getStart(), sel.getEnd(), 0));
}

void MainComponent::cropToSelection()
{
    const auto sel = waveform.getSelection();

    if (! sel.isEmpty())
        applyEdit (AudioEdits::crop (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd()), "Crop", { 0, sel.getLength() },
                   MarkerEdits::afterCrop (engine.document.getMarkers(), sel.getStart(), sel.getEnd()));
}

//==============================================================================
void MainComponent::askForDecibels (const juce::String& title, const juce::String& label, float initial,
                                    std::function<void (float)> onOk)
{
    auto* box = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("db", juce::String (initial, 2), label);
    box->getTextEditor ("db")->setInputRestrictions (8, "-.0123456789");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([box, onOk] (int result)
    {
        if (result == 1)
            onOk (box->getTextEditorContents ("db").getFloatValue());
    }), true);
}

void MainComponent::applyGain()
{
    askForDecibels ("Gain", "Gain (dB)", 0.0f, [this] (float db)
    {
        const auto r = selectionOrAll();
        applyEdit (AudioEdits::gain (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), db),
                   "Gain " + juce::String (db, 2) + " dB", waveform.getSelection());
    });
}

void MainComponent::applyNormalize()
{
    askForDecibels ("Normalize", "Peak level (dBFS)", -0.1f, [this] (float db)
    {
        const auto r = selectionOrAll();
        auto next = AudioEdits::normalize (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), juce::jmin (db, 0.0f));

        if (next == nullptr)
            showError ("Normalize", "The selection is silent.");
        else
            applyEdit (std::move (next), "Normalize", waveform.getSelection());
    });
}

void MainComponent::measureLoudness (std::function<void (const Loudness::Result&, juce::Range<juce::int64>)> done)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr || doc.getNumFrames() == 0)
        return;

    auto result = std::make_shared<Loudness::Result>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Measuring loudness...",
        [result, snapshot, rate, range] (const std::function<bool (float)>& progress)
        {
            const auto r = Loudness::measure (*snapshot, range.getStart(), range.getEnd(), rate, progress);

            if (r.has_value())
                *result = *r;

            return r.has_value();
        },
        [this, result, range, version, done]
        {
            if (engine.document.getVersion() != version)
                showError ("Loudness", "The audio changed while it was measured, so the result was discarded.");
            else
                done (*result, range);
        });
    backgroundJob->launchThread();
}

void MainComponent::showLoudness()
{
    measureLoudness ([this] (const Loudness::Result& r, juce::Range<juce::int64> range)
    {
        const auto whole = range.getLength() == engine.document.getNumFrames();
        const auto rows = juce::StringArray {
            "Integrated loudness:   " + Loudness::format (r.integrated, "LUFS"),
            "Loudness range:        " + juce::String (r.range, 1) + " LU",
            "Max momentary:         " + Loudness::format (r.maxMomentary, "LUFS"),
            "Max short-term:        " + Loudness::format (r.maxShortTerm, "LUFS"),
            "True peak:             " + Loudness::format (r.truePeak, "dBTP"),
            "Sample peak:           " + Loudness::format (r.samplePeak, "dBFS"),
        };

        auto* box = new juce::AlertWindow ("Loudness Statistics",
                                           (whole ? juce::String ("Whole file") : juce::String ("Selection")) + ", "
                                               + formatTime (range.getLength(), engine.document.getSampleRate())
                                               + ". EBU R128 / ITU-R BS.1770-4.",
                                           juce::MessageBoxIconType::NoIcon, this);
        auto* text = new juce::TextEditor();
        text->setMultiLine (true);
        text->setReadOnly (true);
        text->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
        text->setText (rows.joinIntoString ("\n"));
        text->setSize (360, 120);
        box->addCustomComponent (text);
        box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([text] (int) { delete text; }), true);
    });
}

void MainComponent::askForLoudnessTarget()
{
    auto* settings = appProperties.getUserSettings();
    auto* box = new juce::AlertWindow ("Match Loudness",
                                       "Common targets: EBU R128 -23, ATSC A/85 -24, podcasts -16, streaming -14 LUFS.\n"
                                       "Gain stops short of the target if the true peak would pass the ceiling.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("target", juce::String (settings->getDoubleValue ("loudnessTarget", -23.0), 1), "Target (LUFS)");
    box->addTextEditor ("ceiling", juce::String (settings->getDoubleValue ("truePeakCeiling", -1.0), 1), "True peak ceiling (dBTP)");

    for (auto* name : { "target", "ceiling" })
        box->getTextEditor (name)->setInputRestrictions (8, "-.0123456789");

    box->addButton ("Match", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, settings] (int result)
    {
        if (result != 1)
            return;

        const auto target = juce::jlimit (-70.0, 0.0, box->getTextEditorContents ("target").getDoubleValue());
        const auto ceiling = juce::jlimit (-30.0, 0.0, box->getTextEditorContents ("ceiling").getDoubleValue());
        settings->setValue ("loudnessTarget", target);
        settings->setValue ("truePeakCeiling", ceiling);
        matchLoudness (target, ceiling);
    }), true);
}

void MainComponent::matchLoudness (double target, double ceiling)
{
    measureLoudness ([this, target, ceiling] (const Loudness::Result& r, juce::Range<juce::int64> range)
    {
        if (! std::isfinite (r.integrated))
        {
            showError ("Match Loudness", "There is nothing above the -70 LUFS gate to measure.");
            return;
        }

        const auto wanted = target - r.integrated;
        const auto gain = juce::jmin (wanted, ceiling - r.truePeak);

        applyEdit (AudioEdits::gain (*engine.document.getSnapshot(), range.getStart(), range.getEnd(), (float) gain),
                   "Match Loudness " + Loudness::format (target, "LUFS"), waveform.getSelection());

        if (gain < wanted - 0.05)
            showError ("Match Loudness", "The true peak ceiling of " + Loudness::format (ceiling, "dBTP") + " allowed only "
                                             + juce::String (gain, 1) + " dB of gain, so the result is "
                                             + Loudness::format (r.integrated + gain, "LUFS") + " instead of "
                                             + Loudness::format (target, "LUFS") + ".");
    });
}

void MainComponent::captureNoisePrint()
{
    const auto sel = waveform.getSelection();
    auto& doc = engine.document;
    noisePrint = NoiseReduction::capture (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), doc.getSampleRate());

    if (! noisePrint.has_value())
        showError ("Capture Noise Print", "Select at least " + formatTime (NoiseReduction::frameSize, doc.getSampleRate())
                                              + " of noise alone, then capture it.");

    commands.commandStatusChanged();
}

void MainComponent::askForNoiseReduction()
{
    if (! noisePrint.has_value())
        return;

    if (! juce::exactlyEqual (noisePrint->sampleRate, engine.document.getSampleRate()))
    {
        showError ("Noise Reduction", "The noise print was captured at " + juce::String (noisePrint->sampleRate / 1000.0, 1)
                                          + " kHz. Capture a new one from this file.");
        return;
    }

    auto* settings = appProperties.getUserSettings();
    auto* box = new juce::AlertWindow ("Noise Reduction",
                                       "Lowers whatever matches the captured noise print by up to the reduction. Raise the "
                                       "threshold if noise is left between sounds; lower it if quiet sounds are thinned.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("reduction", juce::String (settings->getDoubleValue ("noiseReduction", 12.0), 1), "Reduction (dB)");
    box->addTextEditor ("threshold", juce::String (settings->getDoubleValue ("noiseThreshold", 3.0), 1), "Threshold above the print (dB)");

    for (auto* name : { "reduction", "threshold" })
        box->getTextEditor (name)->setInputRestrictions (8, "-.0123456789");

    box->addButton ("Reduce", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, settings] (int result)
    {
        if (result != 1)
            return;

        NoiseReduction::Settings s;
        s.reductionDb = juce::jlimit (0.0f, 60.0f, box->getTextEditorContents ("reduction").getFloatValue());
        s.thresholdDb = juce::jlimit (-10.0f, 20.0f, box->getTextEditorContents ("threshold").getFloatValue());
        settings->setValue ("noiseReduction", s.reductionDb);
        settings->setValue ("noiseThreshold", s.thresholdDb);
        reduceNoise (s);
    }), true);
}

void MainComponent::reduceNoise (NoiseReduction::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr || ! noisePrint.has_value())
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Reducing noise...",
        [result, snapshot, rate, range, print = *noisePrint, s] (const std::function<bool (float)>& progress)
        {
            *result = NoiseReduction::reduce (*snapshot, range.getStart(), range.getEnd(), rate, print, s, progress);
            return *result != nullptr;
        },
        [this, result, version, s]
        {
            if (engine.document.getVersion() != version)
                showError ("Noise Reduction", "The audio changed during processing, so the result was discarded.");
            else
                applyEdit (*result, "Noise Reduction " + juce::String (s.reductionDb, 1) + " dB", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::askForClickRepair()
{
    auto* settings = appProperties.getUserSettings();
    auto* box = new juce::AlertWindow ("Click and Pop Repair",
                                       "Finds clicks and pops and rebuilds those samples from the audio around them; nothing else "
                                       "changes. Lower the threshold to catch fainter clicks; raise it if drums or plucks are "
                                       "being taken for clicks.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("threshold", juce::String (settings->getDoubleValue ("clickThreshold", 18.0), 1), "Threshold (dB above the background)");
    box->addTextEditor ("length", juce::String (settings->getDoubleValue ("clickMaxLength", 3.0), 1), "Longest click (ms)");

    for (auto* name : { "threshold", "length" })
        box->getTextEditor (name)->setInputRestrictions (8, ".0123456789");

    box->addButton ("Repair", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, settings] (int result)
    {
        if (result != 1)
            return;

        ClickRepair::Settings s;
        s.thresholdDb = juce::jlimit (6.0f, 60.0f, box->getTextEditorContents ("threshold").getFloatValue());
        s.maxLengthMs = juce::jlimit (0.1f, 20.0f, box->getTextEditorContents ("length").getFloatValue());
        settings->setValue ("clickThreshold", s.thresholdDb);
        settings->setValue ("clickMaxLength", s.maxLengthMs);
        repairClicks (s);
    }), true);
}

void MainComponent::repairClicks (ClickRepair::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr || doc.getNumFrames() == 0)
        return;

    auto result = std::make_shared<ClickRepair::Result>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Repairing clicks...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            const auto r = ClickRepair::repair (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);

            if (r.has_value())
                *result = *r;

            return r.has_value();
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
            {
                showError ("Click and Pop Repair", "The audio changed during processing, so the result was discarded.");
                return;
            }

            const auto skipped = result->skipped > 0 ? " " + juce::String (result->skipped) + " longer event"
                                                           + (result->skipped == 1 ? " was" : "s were") + " left alone."
                                                     : juce::String();

            if (result->snapshot == nullptr)
            {
                showNote ("Click and Pop Repair", "No clicks found." + skipped);
                return;
            }

            applyEdit (result->snapshot, "Click Repair (" + juce::String (result->repaired) + ")", waveform.getSelection());

            if (result->skipped > 0)
                showNote ("Click and Pop Repair", "Repaired " + juce::String (result->repaired) + " click"
                                                      + (result->repaired == 1 ? "." : "s.") + skipped);
        });
    backgroundJob->launchThread();
}

void MainComponent::applySimpleEffect (const juce::String& name,
                                       AudioDocument::Snapshot (*effect) (const AudioSnapshot&, juce::int64, juce::int64),
                                       bool needsSelection)
{
    const auto r = needsSelection ? waveform.getSelection() : selectionOrAll();

    if (! r.isEmpty())
        applyEdit (effect (*engine.document.getSnapshot(), r.getStart(), r.getEnd()), name, waveform.getSelection());
}

void MainComponent::applyRackToSelection()
{
    const auto r = selectionOrAll();

    if (r.isEmpty() || engine.getNumRackSlots() == 0 || renderJob != nullptr)
        return;

    if (r.getLength() > std::numeric_limits<int>::max())
    {
        showError ("Effects rack", "The range is too long to render in one pass.");
        return;
    }

    engine.getPlayer().pause();
    renderJob = std::make_unique<RackRenderJob> (*this, engine.createRackXml(), r);
    renderJob->launchThread();
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (isRecording())
    {
        stopRecording();
        return;
    }

    auto& player = engine.getPlayer();

    if (player.isPlaying())
    {
        player.pause();
        return;
    }

    if (multitrackMode)
    {
        // As in the editor: a time range plays and stops at its end.
        const auto range = multitrack.getTimeSelection();
        const auto cursor = multitrack.getCursor();

        if (! range.isEmpty())
            player.play (range.getStart(), range.getEnd());
        else
            player.play (cursor >= engine.session.getState()->getLength() ? 0 : cursor, -1);

        return;
    }

    // A selection plays and stops at its end; otherwise play from the cursor to the end.
    const auto sel = waveform.getSelection();

    if (sel.isEmpty())
        player.play (waveform.getCursor() >= engine.document.getNumFrames() ? 0 : waveform.getCursor(), -1);
    else
        player.play (sel.getStart(), sel.getEnd());
}

void MainComponent::chooseSampleRate()
{
    auto& doc = engine.document;
    const juce::Array<int> rates { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };

    auto* box = new juce::AlertWindow ("Convert Sample Rate",
                                       "This file is " + juce::String (doc.getSampleRate()) + " Hz. Convert it to:",
                                       juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray names;

    for (auto r : rates)
        names.add (juce::String (r) + " Hz");

    box->addComboBox ("rate", names);
    auto* combo = box->getComboBoxComponent ("rate");

    // Offer the interface's rate first, since that is usually why a file is converted.
    auto suggested = rates.indexOf (48000);

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
        if (const auto i = rates.indexOf ((int) std::llround (device->getCurrentSampleRate()));
            i >= 0 && ! juce::exactlyEqual ((double) rates[i], doc.getSampleRate()))
            suggested = i;

    combo->setSelectedItemIndex (suggested);
    box->addButton ("Convert", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, rates] (int result)
    {
        if (result == 1)
            convertSampleRate ((double) rates[box->getComboBoxComponent ("rate")->getSelectedItemIndex()]);
    }), true);
}

void MainComponent::convertSampleRate (double newRate)
{
    auto& doc = engine.document;

    if (juce::exactlyEqual (newRate, doc.getSampleRate()) || backgroundJob != nullptr)
        return;

    engine.getPlayer().pause();

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto oldRate = doc.getSampleRate();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Converting to " + juce::String (newRate / 1000.0, 1) + " kHz...",
        [result, snapshot, oldRate, newRate] (const std::function<bool (float)>& progress)
        {
            *result = SampleRateConverter::convert (*snapshot, oldRate, newRate, progress);
            return *result != nullptr;
        },
        [this, result, oldRate, newRate, version]
        {
            auto& d = engine.document;

            if (d.getVersion() != version)
            {
                showError ("Convert Sample Rate", "The audio changed during the conversion, so the result was discarded.");
                return;
            }

            const auto scale = [&] (juce::int64 frame) { return (juce::int64) std::llround ((double) frame * newRate / oldRate); };
            const auto sel = waveform.getSelection();
            d.applyNewRate (*result, newRate, SampleRateConverter::convert (d.getMarkers(), oldRate, newRate),
                            "Convert to " + juce::String (newRate / 1000.0, 1) + " kHz");
            waveform.setSelection ({ scale (sel.getStart()), scale (sel.getEnd()) });
        });
    backgroundJob->launchThread();
}

void MainComponent::toggleRecord()
{
    if (isRecording())
        stopRecording();
    else
        startRecording();
}

void MainComponent::startRecording()
{
    auto& recorder = engine.getRecorder();
    auto& player = engine.getPlayer();
    auto& doc = engine.document;
    auto* device = engine.deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        showError ("Record", "No audio device is open.");
        return;
    }

    if (multitrackMode)
    {
        startSessionRecording();
        return;
    }

    if (! recorder.isInputActive())
    {
        showError ("Record", "Choose an input to record from first (Input, at the bottom of the window).");
        return;
    }

    const auto deviceRate = device->getCurrentSampleRate();

    if (! doc.hasAudio())
    {
        // A new recording takes the interface's rate and the input's channel count.
        if (! juce::exactlyEqual (doc.getSampleRate(), deviceRate) || doc.getNumChannels() != engine.getInputNumChannels())
        {
            doc.reset (std::make_shared<const AudioSnapshot> (engine.getInputNumChannels(), std::vector<AudioSnapshot::Segment>()),
                       deviceRate, {}, 24);
            player.resetSource();
        }
    }
    else if (! juce::exactlyEqual (doc.getSampleRate(), deviceRate))
    {
        showError ("Record", "This file is " + juce::String (doc.getSampleRate()) + " Hz but the interface runs at "
                               + juce::String (deviceRate) + " Hz. Set the interface to the file's rate in Audio Settings, "
                               "or convert the file to " + juce::String (deviceRate) + " Hz (Edit > Convert Sample Rate).");
        return;
    }

    const auto sel = waveform.getSelection();
    punchIn = sel.isEmpty() ? juce::jmin (waveform.getCursor(), doc.getNumFrames()) : sel.getStart();
    punchOut = sel.isEmpty() ? -1 : sel.getEnd();
    recordLatency = engine.getRoundTripLatency();

    player.pause();

    if (! recorder.start())
    {
        showError ("Record", "The recorder could not start.");
        return;
    }

    // Over existing audio, play from the preroll so the performer hears what comes before the
    // punch-in; the player's frame counter is also what lines the take up. A new file just records.
    if (doc.hasAudio())
        player.play (juce::jmax ((juce::int64) 0, punchIn - (juce::int64) (deviceRate * prerollSeconds)), -1);

    juce::Logger::writeToLog ("Recording: input " + juce::String (engine.getInputFirstChannel() + 1) + " x"
                              + juce::String (engine.getInputNumChannels()) + ", punch in " + juce::String (punchIn)
                              + ", punch out " + juce::String (punchOut) + (recordingLoop ? ", looping" : "")
                              + ", round trip " + juce::String (recordLatency));
    refreshInputChoices();
    commands.commandStatusChanged();
}

void MainComponent::stopRecording()
{
    if (! isRecording())
        return;

    std::map<Recorder*, Recorder::Take> takes;

    for (auto* r : engine.getRecorders())
        if (r->isRecording())
            takes[r] = r->stop();

    engine.getPlayer().pause();
    engine.getPlayer().clearLoop();
    engine.getPlayer().setCountIn (-1);
    refreshInputChoices();
    commands.commandStatusChanged();
    juce::int64 dropped = 0;

    for (auto& [r, t] : takes)
    {
        juce::Logger::writeToLog ("Recorded " + juce::String (t.clip.getNumFrames()) + " frames from input "
                                  + juce::String (r->getInputFirstChannel() + 1) + " x" + juce::String (r->getInputNumChannels())
                                  + ", anchor " + juce::String (t.anchorRecorded) + " -> " + juce::String (t.anchorPlayed)
                                  + ", dropped " + juce::String (t.droppedFrames));
        dropped += t.droppedFrames;
    }

    if (dropped > 0)
        showError ("Record", juce::String (dropped) + " frames were lost because the computer could not keep up. "
                               "Listen to the take before keeping it.");

    if (recordingSession)
    {
        recordingSession = false;
        multitrack.showRecording ({});
        finishSessionRecording (takes);
        return;
    }

    const auto& take = takes.begin()->second;
    const auto placement = Recorder::place (take, punchIn, punchOut, recordLatency);

    if (placement.takeEnd <= placement.takeStart)
        return;

    const AudioSnapshot takeAudio (take.clip.numChannels, take.clip.segments);
    const auto clip = AudioEdits::copy (takeAudio, placement.takeStart, placement.takeEnd, take.clip.sampleRate);

    auto& doc = engine.document;
    const auto snapshot = doc.getSnapshot();
    const auto start = juce::jmin (placement.destStart, snapshot->getNumFrames());
    const auto end = juce::jmin (start + clip.getNumFrames(), snapshot->getNumFrames());

    applyEdit (AudioEdits::replaceWithClip (*snapshot, start, end, clip), "Record", { start, start + clip.getNumFrames() },
               MarkerEdits::afterReplace (doc.getMarkers(), start, end, clip.getNumFrames()));
}

void MainComponent::startSessionRecording()
{
    auto& player = engine.getPlayer();
    auto& session = engine.session;
    const auto deviceRate = engine.deviceManager.getCurrentAudioDevice()->getCurrentSampleRate();

    // Each armed track records from its input's recorder; with none armed, the take goes onto a
    // new track from the recording input.
    updateInputs();
    recordTracks.clear();

    for (auto id : multitrack.getArmedTracks())
    {
        const auto* t = session.getState()->findTrack (id);
        auto* r = engine.getRecorderFor (t->input);

        if (r == nullptr || ! r->isInputActive())
        {
            showError ("Record", "The input " + t->name + " records from is not on. Choose one with its In button, "
                                 "or the recording input at the bottom of the window.");
            return;
        }

        recordTracks.emplace_back (id, r);
    }

    if (recordTracks.empty())
    {
        if (! engine.getRecorder().isInputActive())
        {
            showError ("Record", "Choose an input to record from first (Input, at the bottom of the window).");
            return;
        }

        recordTracks.emplace_back (0, &engine.getRecorder());
    }

    const auto rateDiffers = ! juce::exactlyEqual (session.getSampleRate(), deviceRate);

    if (rateDiffers && session.getState()->getLength() > 0)
    {
        showError ("Record", "This session is " + juce::String (session.getSampleRate()) + " Hz but the interface runs at "
                               + juce::String (deviceRate) + " Hz. Set the interface to the session's rate in Audio Settings.");
        return;
    }

    // A session with no audio yet takes the interface's rate, as a Record step that the take
    // replaces when it lands (see finishSessionRecording).
    recordChangedRate = rateDiffers;

    if (rateDiffers)
    {
        auto next = *session.getState();
        next.sampleRate = deviceRate;
        session.apply (std::move (next), "Record");
        player.resetSource();
    }

    const auto range = multitrack.getTimeSelection();
    punchIn = range.isEmpty() ? multitrack.getCursor() : range.getStart();
    punchOut = range.isEmpty() ? -1 : range.getEnd();
    recordLatency = engine.getRoundTripLatency();
    recordingLoop = loopRecording && ! range.isEmpty();

    // A count-in starts one bar before the punch-in, with clicks up to it; otherwise the preroll.
    const auto& now = *session.getState();
    const auto lead = countIn ? Metronome::barFrames (now.tempo, now.beatsPerBar, now.sampleRate) : (juce::int64) (deviceRate * prerollSeconds);
    player.setCountIn (countIn ? punchIn : -1);

    player.pause();
    std::set<Recorder*> started;

    for (auto& [id, r] : recordTracks)
    {
        if (started.count (r) > 0)
            continue;

        if (! r->start())
        {
            for (auto* s : started)
                s->stop();

            showError ("Record", "The recorder could not start.");
            return;
        }

        started.insert (r);
    }

    // Always play, from the preroll: the performer hears the session, and the player's frame
    // counter lines the take up. Past the end of the session it plays silence until stopped.
    recordingSession = true;
    player.play (juce::jmax ((juce::int64) 0, punchIn - lead), std::numeric_limits<juce::int64>::max() / 4);

    if (recordingLoop)
        player.setLoop (punchIn, punchOut);

    juce::Logger::writeToLog ("Recording to the session: " + juce::String ((int) recordTracks.size()) + " track(s) from "
                              + juce::String ((int) started.size()) + " input(s), punch in " + juce::String (punchIn)
                              + ", punch out " + juce::String (punchOut) + (recordingLoop ? ", looping" : "")
                              + ", round trip " + juce::String (recordLatency));
    refreshInputChoices();
    commands.commandStatusChanged();
}

juce::File MainComponent::recordingsFolder() const
{
    // Beside a saved session, in a folder of its own; otherwise in the user's Music folder.
    const auto file = engine.session.getFile();

    if (file != juce::File())
        return file.getSiblingFile (file.getFileNameWithoutExtension() + " Recordings");

    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("SpectraBlade").getChildFile ("Recordings");
}

void MainComponent::finishSessionRecording (const std::map<Recorder*, Recorder::Take>& takes)
{
    const auto looped = recordingLoop;
    recordingLoop = false;

    const auto folder = recordingsFolder();

    if (const auto made = folder.createDirectory(); made.failed())
    {
        showError ("Record", "Could not make the recordings folder " + folder.getFullPathName() + ": " + made.getErrorMessage());
        return;
    }

    // Takes are kept as 32-bit float WAVs, so they are exactly what came in.
    auto state = *engine.session.getState();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    const auto save = [&] (const AudioDocument::Snapshot& audio, const juce::String& trackName, juce::String& error)
    {
        const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (trackName) + " " + stamp, ".wav", false);
        error = AudioFileIO::save (*audio, state.sampleRate, file, 32, engine.formatManager);
        return file;
    };

    const auto result = SessionRecording::land (state, recordTracks, takes, punchIn, punchOut, recordLatency, looped, save);

    if (result.error.isNotEmpty())
    {
        showError ("Record", "Could not save the take: " + result.error);
        return;
    }

    if (result.landed == 0)
        return;

    // One undo step, which also takes back the rate change of a session that had no audio yet.
    engine.session.apply (std::move (state), SessionRecording::describe (result, looped), recordChangedRate);
    multitrack.setTimeSelection ({});
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 0, 1, 256,
                                                             false, false, true, false);
    selector->setSize (520, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = juce::Colours::darkgrey;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showPluginList()
{
    if (pluginListWindow == nullptr)
        pluginListWindow = std::make_unique<PluginListWindow> (engine, deadMansPedalFile, appProperties.getUserSettings(),
                                                              [this] { pluginListWindow = nullptr; });
    else
        pluginListWindow->toFront (true);
}

void MainComponent::showEditor (int slotIndex)
{
    if (auto* plugin = engine.getRackPlugin (slotIndex))
        showEditor (*plugin);
}

void MainComponent::showEditor (juce::AudioProcessor& p)
{
    auto* plugin = &p;

    for (auto* w : editorWindows)
    {
        if (&w->processor == plugin)
        {
            w->toFront (true);
            return;
        }
    }

    editorWindows.add (new PluginEditorWindow (*plugin, [this] (PluginEditorWindow* w)
    {
        juce::MessageManager::callAsync ([this, safe = juce::Component::SafePointer<PluginEditorWindow> (w)]
        {
            if (safe != nullptr)
                editorWindows.removeObject (safe.getComponent());
        });
    }));
}

void MainComponent::closeEditorFor (juce::AudioProcessor* processor)
{
    for (int i = editorWindows.size(); --i >= 0;)
        if (&editorWindows[i]->processor == processor)
            editorWindows.remove (i);
}

void MainComponent::saveRack()
{
    chooser = std::make_unique<juce::FileChooser> ("Save effects rack", juce::File(), "*.sbrack");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! engine.createRackXml()->writeTo (file.withFileExtension ("sbrack")))
                                  showError ("Save failed", "Could not write " + file.getFullPathName());
                          });
}

void MainComponent::loadRack()
{
    chooser = std::make_unique<juce::FileChooser> ("Load effects rack", juce::File(), "*.sbrack;*.wdrack;*.wlrack");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto xml = juce::parseXML (fc.getResult());

                              if (xml == nullptr)
                                  return;

                              engine.loadRackXml (*xml, [] (const juce::String& errors)
                              {
                                  if (errors.isNotEmpty())
                                      showError ("Some plug-ins did not load", errors);
                              });
                          });
}
