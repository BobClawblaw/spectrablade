#include "MainComponent.h"
#include "AudioFileIO.h"
#include "Bounce.h"
#include "ClipList.h"
#include "MarkerTable.h"
#include "ClipProperties.h"
#include "Metronome.h"
#include "MixerView.h"
#include "OfflineRender.h"
#include "PluginScanner.h"
#include "SampleRateConverter.h"
#include "SessionCollect.h"
#include "SessionRecovery.h"
#include "KeyShortcuts.h"
#include "SessionRecording.h"
#include "SpectralEdit.h"
#include "TimeStretch.h"
#include "TrackTemplate.h"
#include "UndoHistory.h"

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
    constexpr const char* zeroCrossingsKey = "zeroCrossings";
    constexpr const char* prerollKey      = "preroll";
    constexpr const char* postrollKey     = "postroll";
    constexpr const char* laneHeightKey   = "laneHeight";
    constexpr const char* loopPlaybackKey = "loopPlayback";

    // Input choice IDs: none, a stereo pair from channel n, or mono channel n.
    constexpr int noInputId = 1, stereoInputBase = 1000, monoInputBase = 2000;

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
        cmdAddBus, cmdSnapping, cmdMixDown, cmdCrossfade, cmdWriteAutomation, cmdLoopRecording, cmdInputMonitoring, cmdMetronome, cmdCountIn, cmdTempo, cmdBarsAndBeats, cmdMixer,
        cmdDuplicateClip, cmdBounceTrack, cmdMoveTrackUp, cmdMoveTrackDown,
        cmdNudgeEarlier, cmdNudgeLater, cmdExportStems, cmdClipProperties,
        cmdRepeatClip, cmdReverseClip, cmdRecordingOptions,
        cmdTallerTracks, cmdShorterTracks, cmdLockClip, cmdMuteClip, cmdClipList, cmdDuplicateTrack, cmdHideTrack, cmdShowAllTracks, cmdSelectTrackClips, cmdSelectClipsAfter, cmdMoveClipsToCursor, cmdCloseGaps, cmdZeroCrossings, cmdStretchClip, cmdShiftPitch, cmdUndoHistory, cmdSaveTrackTemplate, cmdAddFromTemplate, cmdSaveCopyWithAudio,
        cmdClearRange, cmdDeleteTime, cmdInsertTime, cmdCutTime, cmdCopyTime, cmdPasteTime, cmdTempoChange, cmdMarkerList, cmdExportMarkers, cmdExportRegions, cmdQuantize, cmdLoopPlayback, cmdKeyShortcuts, cmdFreezeTrack, cmdGroupClips, cmdUngroupClips, cmdClipGainLines, cmdAddFolder, cmdExportFolderStems
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
/** The selected clip's settings, in a small window that follows the selection. */
class MainComponent::ClipPropertiesWindow final : public juce::DocumentWindow
{
public:
    ClipPropertiesWindow (AudioEngine& audioEngine, MultitrackView& view, std::function<void()> onClose)
        : DocumentWindow ("Clip Properties", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ClipProperties (audioEngine, [&view] { return view.getSelectedClip(); }), false);
        centreWithSize (340, 280);
        setAlwaysOnTop (true);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** Every clip in the session, in a window that follows the selection. */
class MainComponent::ClipListWindow final : public juce::DocumentWindow
{
public:
    ClipListWindow (AudioEngine& audioEngine, MultitrackView& view, std::function<void()> onClose)
        : DocumentWindow ("Clip List", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new ClipList (audioEngine, [&view] { return view.getSelectedClip(); },
                                       [&view] (juce::uint32 id) { view.showClip (id); }), false);
        setResizable (true, false);
        setResizeLimits (300, 150, 3000, 2000);
        centreWithSize (710, 320);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** Every command and its keys, to change; the changes are saved as they are made. */
class MainComponent::KeyShortcutsWindow final : public juce::DocumentWindow
{
public:
    KeyShortcutsWindow (juce::KeyPressMappingSet& mappings, std::function<void()> onClose)
        : DocumentWindow ("Keyboard Shortcuts", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new juce::KeyMappingEditorComponent (mappings, true), false);
        setResizable (true, false);
        setResizeLimits (400, 300, 3000, 3000);
        centreWithSize (560, 640);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** The session's markers and regions, in a window beside the main one. */
class MainComponent::MarkerListWindow final : public juce::DocumentWindow
{
public:
    MarkerListWindow (Session& session, MultitrackView& view, std::function<void()> onClose)
        : DocumentWindow ("Marker List", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MarkerTable (session, { [&view] (int i) { view.showMarker (i); },
                                                    [&view] (int i) { view.renameMarker (i); },
                                                    [&view] (int i) { view.deleteMarker (i); } }), false);
        setResizable (true, false);
        setResizeLimits (250, 150, 3000, 2000);
        centreWithSize (470, 320);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** The session's undo steps, in a window beside the main one. */
class MainComponent::UndoHistoryWindow final : public juce::DocumentWindow
{
public:
    UndoHistoryWindow (Session& session, std::function<void()> onClose)
        : DocumentWindow ("Undo History", juce::Colours::darkgrey, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new UndoHistory (session), false);
        setResizable (true, false);
        setResizeLimits (200, 150, 2000, 3000);
        centreWithSize (280, 400);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** The mixer, in a window of its own beside the main one. */
class MainComponent::MixerWindow final : public juce::DocumentWindow
{
public:
    MixerWindow (AudioEngine& audioEngine, std::function<void()> onClose)
        : DocumentWindow ("Mixer", juce::Colours::darkgrey, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MixerView (audioEngine), false);
        setResizable (true, false);
        setResizeLimits (MixerView::stripWidth * 2 + 12, 300, 4000, 2000);
        centreWithSize (MixerView::stripWidth * 8 + 12, 420);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

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
    recordButton.setTooltip ("Record from the input (Shift+Space). Over a selection it punches in and out after a "
                             "pre-roll (Transport > Recording Options); otherwise it records from the cursor. "
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
    addChildComponent (overview);
    multitrack.setSnapping (settings->getBoolValue (snappingKey, true));
    multitrack.setCrossfading (settings->getBoolValue (crossfadingKey, true));
    multitrack.setFindingZeroCrossings (settings->getBoolValue (zeroCrossingsKey, false));
    loopPlayback = settings->getBoolValue (loopPlaybackKey, false);
    multitrack.setLaneHeight (settings->getIntValue (laneHeightKey, MultitrackView::trackHeight));
    rolls.preroll = juce::jlimit (0.0, SessionRecording::Rolls::longest, settings->getDoubleValue (prerollKey, rolls.preroll));
    rolls.postroll = juce::jlimit (0.0, SessionRecording::Rolls::longest, settings->getDoubleValue (postrollKey, rolls.postroll));
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
    KeyShortcuts::restore (*commands.getKeyMappings(), *settings);
    commands.getKeyMappings()->addChangeListener (this);
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
    lastRecoveryCheck = juce::Time::getMillisecondCounter();

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->offerRecoveredSession();
    });
}

juce::File MainComponent::recoveryFolder()
{
    return appProperties.getUserSettings()->getFile().getParentDirectory().getChildFile ("Recovery");
}

void MainComponent::keepRecoveryCopy()
{
    if (! engine.session.isDirty())
    {
        if (recoveryCopyKept)
            SessionRecovery::clear (recoveryFolder());

        recoveryCopyKept = false;
        return;
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (now - lastRecoveryCheck < (juce::uint32) SessionRecovery::intervalSeconds * 1000u)
        return;

    lastRecoveryCheck = now;

    // Not while recording: the takes aren't in the session until they stop.
    if (engine.session.getVersion() == recoveredVersion || isRecording())
        return;

    recoveredVersion = engine.session.getVersion();
    const auto error = SessionRecovery::write (*engine.session.getState(), engine.session.getFile(),
                                               [this] (juce::uint32 id) { return engine.getTrackEffects().createXml (id); }, recoveryFolder());
    recoveryCopyKept = error.isEmpty();

    if (error.isNotEmpty())
        juce::Logger::writeToLog ("Could not write the recovery copy: " + error);
}

void MainComponent::offerRecoveredSession()
{
    const auto copy = SessionRecovery::find (recoveryFolder());

    if (! copy.has_value())
        return;

    const auto name = copy->original != juce::File() ? copy->original.getFileName() : juce::String ("an untitled session");
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Recover Session",
                                        "SpectraBlade closed without saving changes to " + name + ". A copy from "
                                            + copy->written.toString (true, true, false) + " was kept. Open it?",
                                        "Open", "Discard", this,
                                        juce::ModalCallbackFunction::create ([this, found = *copy] (int result)
                                        {
                                            if (result == 1)
                                            {
                                                openSession (found.session, found.original);
                                                recoveryCopyKept = true;   // kept until the session is saved
                                            }
                                            else
                                            {
                                                SessionRecovery::clear (recoveryFolder());
                                            }
                                        }));
}

MainComponent::~MainComponent()
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    for (auto* r : engine.getRecorders())     // takes are lost: the app is closing
        if (r->isRecording())
            r->stop();

    saveSettings();

    // A normal close: the user has saved or discarded the session's changes.
    SessionRecovery::clear (recoveryFolder());
    renderJob = nullptr;
    backgroundJob = nullptr;
    engine.onPluginAboutToBeRemoved = nullptr;
    engine.document.removeChangeListener (this);
    engine.session.removeChangeListener (this);
    engine.removeChangeListener (this);
    engine.knownPlugins.removeChangeListener (this);
    commands.getKeyMappings()->removeChangeListener (this);
    keyShortcutsWindow = nullptr;
    editorWindows.clear();
    pluginListWindow = nullptr;
    mixerWindow = nullptr;
    clipPropertiesWindow = nullptr;
    clipListWindow = nullptr;
    undoHistoryWindow = nullptr;
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
    overview.setBounds (r.removeFromTop (SessionOverview::height));
    r.removeFromTop (4);
    multitrack.setBounds (r);
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (int id = cmdOpen; id <= cmdExportFolderStems; ++id)
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
        case cmdLoopPlayback: info.setInfo ("Loop Playback", "Play the selection or time range over and over", "Transport", 0);
                            info.addDefaultKeypress ('l', cmd); info.setTicked (loopPlayback); break;
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
        case cmdAddFolder:  info.setInfo ("Add Folder", "Add a folder above the selected track, with it inside: its mute and solo act on its tracks, and it can be collapsed", "Multitrack", 0);
                            info.setActive (multitrackMode); break;
        case cmdAddBus:     info.setInfo ("Add Bus", "Add a bus for tracks to send to", "Multitrack", 0);
                            info.addDefaultKeypress ('b', cmd | shift); info.setActive (multitrackMode); break;
        case cmdSnapping:   info.setInfo ("Snapping", "Line dragged clips, edges, fades and points up with clip edges, the cursor and the ruler", "Multitrack", 0);
                            info.addDefaultKeypress ('s', 0); info.setTicked (multitrack.isSnapping()); info.setActive (multitrackMode); break;
        case cmdMetronome:  info.setInfo ("Metronome", "Click on each beat of the session's tempo while it plays", "Multitrack", 0);
                            info.addDefaultKeypress ('c', 0); info.setTicked (engine.getPlayer().isMetronomeOn()); info.setActive (multitrackMode); break;
        case cmdCountIn:    info.setInfo ("Count-In", "Record after one bar of clicks instead of the preroll", "Multitrack", 0);
                            info.setTicked (countIn); info.setActive (multitrackMode && ! isRecording()); break;
        case cmdMixer:      info.setInfo ("Mixer", "A window with a strip for each track and bus, and the master meter", "Multitrack", 0);
                            info.addDefaultKeypress ('m', cmd | shift); info.setTicked (mixerWindow != nullptr); break;
        case cmdTallerTracks: info.setInfo ("Taller Tracks", "Make every track's lane taller", "View", 0);
                            info.addDefaultKeypress ('=', cmd | alt);
                            info.setActive (multitrackMode && multitrack.getLaneHeight() < MultitrackView::maxLaneHeight); break;
        case cmdShorterTracks: info.setInfo ("Shorter Tracks", "Make every track's lane shorter, to see more of them", "View", 0);
                            info.addDefaultKeypress ('-', cmd | alt);
                            info.setActive (multitrackMode && multitrack.getLaneHeight() > MultitrackView::minLaneHeight); break;
        case cmdRecordingOptions: info.setInfo ("Recording Options...", "How long playback runs before a punch-in and after a punch-out", "Transport", 0);
                            info.setActive (! isRecording()); break;
        case cmdRepeatClip: info.setInfo ("Repeat Clip to Range End", "Lay copies of the selected clip end to end up to the end of the time range", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0 && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdReverseClip: info.setInfo ("Reverse Clip", "Play the selected clip back to front, from a new file", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0); break;
        case cmdClipGainLines: info.setInfo ("Clip Gain Lines", "Show each clip's gain line to edit it: click the line to add a point, drag one, Alt-click to remove it", "Multitrack", 0);
                            info.setTicked (multitrack.isShowingClipGain()); info.setActive (multitrackMode); break;
        case cmdGroupClips: info.setInfo ("Group Clips", "Group the selected clips, so clicking one selects them all, to move or delete together", "Multitrack", 0);
                            info.addDefaultKeypress ('g', cmd); info.setActive (multitrackMode && multitrack.canGroupSelectedClips()); break;
        case cmdUngroupClips: info.setInfo ("Ungroup Clips", "Break up the groups the selected clips are in", "Multitrack", 0);
                            info.addDefaultKeypress ('g', cmd | shift); info.setActive (multitrackMode && multitrack.areSelectedClipsGrouped()); break;
        case cmdLockClip:   info.setInfo ("Lock Clip", "Keep the selected clips from being moved, trimmed or deleted, or let them be again", "Multitrack", 0);
                            info.addDefaultKeypress ('l', cmd | shift); info.setTicked (multitrack.areSelectedClipsLocked());
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdClipList:   info.setInfo ("Clip List", "A window listing every clip in the session; click one to go to it", "Multitrack", 0);
                            info.setTicked (clipListWindow != nullptr); break;
        case cmdMuteClip:   info.setInfo ("Mute Clip", "Keep the selected clips in place but silent, or let them be heard again", "Multitrack", 0);
                            info.setTicked (multitrack.areSelectedClipsMuted());
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdClipProperties: info.setInfo ("Clip Properties", "A window with the selected clip's name, start, length, gain and fades", "Multitrack", 0);
                            info.addDefaultKeypress ('i', cmd | shift); info.setTicked (clipPropertiesWindow != nullptr); break;
        case cmdExportFolderStems: info.setInfo ("Export Folder Stems...", "Write each folder's tracks and buses, mixed together through their effects, to a file of its own", "Multitrack", 0);
                            info.setActive (multitrackMode && backgroundJob == nullptr && ! Bounce::stemFolders (*engine.session.getState()).empty()); break;
        case cmdExportStems: info.setInfo ("Export Stems...", "Write each track, alone through its effects, to a file of its own", "Multitrack", 0);
                            info.setActive (multitrackMode && backgroundJob == nullptr && ! Bounce::stemTracks (*engine.session.getState()).empty());
                            break;
        case cmdExportRegions: info.setInfo ("Export Regions...", "Mix each region of the session to a file of its own", "Multitrack", 0);
                            info.setActive (multitrackMode && backgroundJob == nullptr
                                            && std::any_of (engine.session.getState()->markers.begin(), engine.session.getState()->markers.end(),
                                                            [] (auto& m) { return m.isRegion(); }));
                            break;
        case cmdNudgeEarlier: info.setInfo ("Nudge Earlier", "Move the selected clips, or the cursor, back to the grid point before", "Multitrack", 0);
                            info.addDefaultKeypress (',', 0); info.setActive (multitrackMode); break;
        case cmdNudgeLater: info.setInfo ("Nudge Later", "Move the selected clips, or the cursor, on to the next grid point", "Multitrack", 0);
                            info.addDefaultKeypress ('.', 0); info.setActive (multitrackMode); break;
        case cmdMoveTrackUp: info.setInfo ("Move Track Up", "Move the selected track or bus up the list", "Multitrack", 0);
                            info.addDefaultKeypress (juce::KeyPress::upKey, cmd | alt); info.setActive (multitrackMode && multitrack.getSelectedTrack() > 0); break;
        case cmdMoveTrackDown: info.setInfo ("Move Track Down", "Move the selected track or bus down the list", "Multitrack", 0);
                            info.addDefaultKeypress (juce::KeyPress::downKey, cmd | alt);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0
                                            && multitrack.getSelectedTrack() < (int) engine.session.getState()->tracks.size() - 1); break;
        case cmdBounceTrack: info.setInfo ("Bounce Track", "Render the selected track through its effects onto a new track, and mute it", "Multitrack", 0);
                            info.setActive (multitrackMode && backgroundJob == nullptr && multitrack.getSelectedTrack() >= 0
                                            && ! engine.session.getState()->tracks[(size_t) multitrack.getSelectedTrack()].isBus
                                            && ! engine.session.getState()->tracks[(size_t) multitrack.getSelectedTrack()].clips.empty());
                            break;
        case cmdFreezeTrack:
        {
            const auto state = engine.session.getState();
            const auto index = multitrack.getSelectedTrack();
            const auto* track = juce::isPositiveAndBelow (index, (int) state->tracks.size()) ? &state->tracks[(size_t) index] : nullptr;
            const auto frozen = track != nullptr && track->isFrozen();
            info.setInfo (frozen ? "Unfreeze Track" : "Freeze Track",
                          "Render the selected track through its effects to a file it plays instead, freeing the CPU; or play its clips live again", "Multitrack", 0);
            info.setTicked (frozen);
            info.setActive (multitrackMode && backgroundJob == nullptr && track != nullptr && ! track->isBus && (frozen || ! track->clips.empty()));
            break;
        }
        case cmdDuplicateClip: info.setInfo ("Duplicate Clip", "Place a copy of the selected clip right after it", "Multitrack", 0);
                            info.addDefaultKeypress ('d', cmd); info.setActive (multitrackMode && multitrack.getSelectedClip() != 0); break;
        case cmdBarsAndBeats: info.setInfo ("Bars and Beats", "Count the ruler in bars and beats of the tempo, and snap to its beats", "Multitrack", 0);
                            info.addDefaultKeypress ('b', 0); info.setTicked (multitrack.isShowingBars()); info.setActive (multitrackMode); break;
        case cmdTempoChange: info.setInfo ("Tempo Change at Cursor...", "Change the tempo or beats in a bar from the bar the cursor is in, or remove a change there", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
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
        case cmdSelectTrackClips: info.setInfo ("Select Clips on Track", "Select every clip on the selected track", "Edit", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0); break;
        case cmdSelectClipsAfter: info.setInfo ("Select Clips After Cursor", "Select every clip that starts at or after the cursor", "Edit", 0);
                            info.setActive (multitrackMode); break;
        case cmdMoveClipsToCursor: info.setInfo ("Move Clips to Cursor", "Move the selected clips together so the earliest starts at the cursor", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdZeroCrossings: info.setInfo ("Cut at Zero Crossings", "Move splits and dragged clip edges to the nearest zero crossing, up to 5 ms away", "Multitrack", 0);
                            info.setTicked (multitrack.isFindingZeroCrossings()); info.setActive (multitrackMode); break;
        case cmdStretchClip: info.setInfo ("Stretch Clip...", "Make the selected clip longer or shorter without changing its pitch", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0 && backgroundJob == nullptr); break;
        case cmdSaveCopyWithAudio: info.setInfo ("Save a Copy with Audio...", "Save a copy of the session with every audio file it uses in a folder beside it", "File", 0);
                            info.setActive (multitrackMode); break;
        case cmdSaveTrackTemplate: info.setInfo ("Save Track as Template...", "Save the selected track's settings, sends and effects to start new tracks from", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0
                                            && ! engine.session.getState()->tracks[(size_t) multitrack.getSelectedTrack()].isFolder); break;
        case cmdAddFromTemplate: info.setInfo ("Add Track from Template...", "Add a track or bus made from a saved template", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdUndoHistory: info.setInfo ("Undo History", "A window listing the session's undo steps; click one to go back or forward to it", "Edit", 0);
                            info.setTicked (undoHistoryWindow != nullptr); break;
        case cmdShiftPitch: info.setInfo ("Shift Clip Pitch...", "Raise or lower the selected clip's pitch without changing its length", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0 && backgroundJob == nullptr); break;
        case cmdClearRange: info.setInfo ("Clear Range", "Clear the time range on every track, leaving the time empty", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdDeleteTime: info.setInfo ("Delete Time", "Take the time range out of the session, moving what is after it back", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdInsertTime: info.setInfo ("Insert Time", "Put as much empty time as the range is long in at its start, moving what is after it on", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdMarkerList: info.setInfo ("Marker List", "Show the session's markers and regions in a list", "Multitrack", 0);
                            info.setActive (multitrackMode);
                            info.setTicked (markerListWindow != nullptr); break;
        case cmdExportMarkers: info.setInfo ("Export Markers as CSV...", "Write the markers and regions of the session or file to a CSV file", "File", 0);
                            info.setActive (multitrackMode ? ! engine.session.getState()->markers.empty() : ! engine.document.getMarkers().empty()); break;
        case cmdQuantize:   info.setInfo ("Quantize Clips to Grid", "Move each selected clip so it starts on the nearest grid line", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdCutTime:    info.setInfo ("Cut Time", "Copy the time range from every track, then take it out of the session", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdCopyTime:   info.setInfo ("Copy Time", "Copy the time range from every track: clips, envelopes and markers", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdPasteTime:  info.setInfo ("Paste Time", "Put the copied time in at the cursor, moving what is after it on", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.canPasteTime()); break;
        case cmdCloseGaps:  info.setInfo ("Close Gaps", "Move the selected track's clips back so each starts where the one before ends", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0); break;
        case cmdHideTrack:  info.setInfo ("Hide Track", "Leave the selected track or bus out of the lanes; it still plays", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0); break;
        case cmdShowAllTracks: info.setInfo ("Show All Tracks", "Show the hidden tracks in the lanes again", "View", 0);
                            info.setActive (multitrackMode && std::any_of (engine.session.getState()->tracks.begin(), engine.session.getState()->tracks.end(),
                                                                           [] (auto& t) { return t.hidden; })); break;
        case cmdDuplicateTrack: info.setInfo ("Duplicate Track", "Copy the selected track or bus, with its clips, effects and settings", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0 && ! isRecording()); break;
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
        case cmdKeyShortcuts: info.setInfo ("Keyboard Shortcuts...", "See and change the keys for every command", "Window", 0); break;
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
            case cmdSelectAll: info.shortName = "Select All Clips"; info.setActive (hasClips); break;
            case cmdZoomSelection: info.shortName = "Zoom to Time Range"; info.setActive (! multitrack.getTimeSelection().isEmpty()); break;
            case cmdCut:    info.shortName = "Cut Clip"; info.setActive (multitrack.getSelectedClip() != 0); break;
            case cmdCopy:   info.shortName = "Copy Clip"; info.setActive (multitrack.getSelectedClip() != 0); break;
            case cmdPaste:  info.shortName = "Paste Clip"; info.setActive (multitrack.canPaste()); break;
            case cmdPlayPause: case cmdGoToStart: case cmdGoToEnd: case cmdZoomIn: case cmdZoomOut: case cmdZoomFit:
                            info.setActive (hasClips); break;
            case cmdShowWaveform: case cmdShowSpectrogram: case cmdShowBoth:
                            info.setTicked (false); break;
            case cmdGain:   info.shortName = "Clip Gain..."; info.description = "Set the selected clip's gain";
                            info.setActive (multitrack.getSelectedClip() != 0); break;
            case cmdNormalize: info.shortName = "Normalize Clip..."; info.description = "Set the selected clip's gain from its peak level";
                            info.setActive (multitrack.getSelectedClip() != 0); break;
            case cmdAddMarker: info.shortName = multitrack.getTimeSelection().isEmpty() ? "Add Marker" : "Add Region";
                            info.setActive (true); break;
            case cmdPreviousMarker: case cmdNextMarker: case cmdDeleteAllMarkers:
                            info.setActive (! session.getState()->markers.empty()); break;
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
        case cmdCut:        if (multitrackMode) multitrack.cutSelectedClip(); else cut(); break;
        case cmdCopy:       if (multitrackMode) { multitrack.copySelectedClip(); commands.commandStatusChanged(); } else copy(); break;
        case cmdPaste:      if (multitrackMode) multitrack.pasteClip(); else paste(); break;
        case cmdDuplicateClip: multitrack.duplicateSelectedClip(); break;
        case cmdDelete:     if (multitrackMode) multitrack.deleteSelectedClip(); else deleteSelection(); break;
        case cmdSelectAll:  if (multitrackMode) multitrack.selectAllClips(); else waveform.selectAll(); break;
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
        case cmdLoopPlayback:
            loopPlayback = ! loopPlayback;
            appProperties.getUserSettings()->setValue (loopPlaybackKey, loopPlayback);
            updatePlaybackLoop();
            commands.commandStatusChanged();
            break;
        case cmdGoToStart:  if (multitrackMode) multitrack.setCursor (0); else waveform.setCursor (0); break;
        case cmdGoToEnd:    if (multitrackMode) multitrack.setCursor (engine.session.getState()->getLength());
                            else waveform.setCursor (engine.document.getNumFrames());
                            break;
        case cmdZoomIn:     if (multitrackMode) multitrack.zoomIn(); else waveform.zoomIn(); break;
        case cmdZoomOut:    if (multitrackMode) multitrack.zoomOut(); else waveform.zoomOut(); break;
        case cmdZoomFit:    if (multitrackMode) multitrack.zoomToFit(); else waveform.zoomToFit(); break;
        case cmdZoomSelection: if (multitrackMode) multitrack.zoomToRange (multitrack.getTimeSelection()); else waveform.zoomToSelection(); break;
        case cmdBounceTrack: bounceTrack(); break;
        case cmdFreezeTrack: freezeOrUnfreezeTrack(); break;
        case cmdExportStems: chooseStemsFolder (false); break;
        case cmdExportFolderStems: chooseStemsFolder (true); break;
        case cmdExportRegions: chooseRegionsFolder(); break;
        case cmdClipProperties: toggleClipProperties(); break;
        case cmdClipList:   toggleClipList(); break;
        case cmdRepeatClip: multitrack.repeatSelectedClip(); break;
        case cmdRecordingOptions: showRecordingOptions(); break;
        case cmdTallerTracks:
        case cmdShorterTracks:
            multitrack.setLaneHeight (multitrack.getLaneHeight()
                                      + (info.commandID == cmdTallerTracks ? 1 : -1) * MultitrackView::laneHeightStep);
            appProperties.getUserSettings()->setValue (laneHeightKey, multitrack.getLaneHeight());
            commands.commandStatusChanged();
            break;
        case cmdReverseClip: reverseSelectedClip(); break;
        case cmdClipGainLines: multitrack.setShowClipGain (! multitrack.isShowingClipGain()); commands.commandStatusChanged(); break;
        case cmdGroupClips: multitrack.groupSelectedClips(); break;
        case cmdUngroupClips: multitrack.ungroupSelectedClips(); break;
        case cmdLockClip:   multitrack.setSelectedClipsLocked (! multitrack.areSelectedClipsLocked()); commands.commandStatusChanged(); break;
        case cmdMuteClip:   multitrack.setSelectedClipsMuted (! multitrack.areSelectedClipsMuted()); commands.commandStatusChanged(); break;
        case cmdNudgeEarlier: multitrack.nudge (false); break;
        case cmdNudgeLater: multitrack.nudge (true); break;
        case cmdMoveTrackUp: multitrack.moveTrack (multitrack.getSelectedTrack(), multitrack.getSelectedTrack() - 1); break;
        case cmdMoveTrackDown: multitrack.moveTrack (multitrack.getSelectedTrack(), multitrack.getSelectedTrack() + 1); break;
        case cmdPlugins:    showPluginList(); break;
        case cmdAudioSettings: showAudioSettings(); break;
        case cmdKeyShortcuts: showKeyShortcuts(); break;
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
        case cmdAddFolder: multitrack.addFolder(); break;
        case cmdDeleteTrack: multitrack.deleteSelectedTrack(); break;
        case cmdDuplicateTrack: duplicateTrack(); break;
        case cmdHideTrack:
            if (const auto index = multitrack.getSelectedTrack(); juce::isPositiveAndBelow (index, (int) engine.session.getState()->tracks.size()))
                multitrack.setTrackHidden (engine.session.getState()->tracks[(size_t) index].id, true);
            break;
        case cmdShowAllTracks: multitrack.showAllTracks(); break;
        case cmdSelectTrackClips: multitrack.selectClipsOnTrack(); break;
        case cmdSelectClipsAfter: multitrack.selectClipsAfterCursor(); break;
        case cmdMoveClipsToCursor: multitrack.moveSelectedClipsToCursor(); break;
        case cmdCloseGaps:  multitrack.closeGapsOnSelectedTrack(); break;
        case cmdClearRange: multitrack.clearTimeSelection(); break;
        case cmdDeleteTime: multitrack.deleteTimeSelection(); break;
        case cmdInsertTime: multitrack.insertTimeAtSelection(); break;
        case cmdCutTime:    multitrack.cutTimeSelection(); break;
        case cmdMarkerList: toggleMarkerList(); break;
        case cmdQuantize:   multitrack.quantizeSelectedClips(); break;
        case cmdExportMarkers: chooseMarkersCsv(); break;
        case cmdCopyTime:   multitrack.copyTimeSelection(); break;
        case cmdPasteTime:  multitrack.pasteTime(); break;
        case cmdStretchClip: askToStretchSelectedClip(); break;
        case cmdShiftPitch: askToShiftSelectedClip(); break;
        case cmdUndoHistory: toggleUndoHistory(); break;
        case cmdSaveTrackTemplate: chooseTrackTemplateToSave(); break;
        case cmdSaveCopyWithAudio: saveSessionCopyWithAudio(); break;
        case cmdAddFromTemplate: chooseTrackTemplateToAdd(); break;
        case cmdZeroCrossings:
            multitrack.setFindingZeroCrossings (! multitrack.isFindingZeroCrossings());
            appProperties.getUserSettings()->setValue (zeroCrossingsKey, multitrack.isFindingZeroCrossings());
            commands.commandStatusChanged();
            break;
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
            showTempoDialog (false);
            break;
        case cmdTempoChange:
            showTempoDialog (true);
            break;
        case cmdBarsAndBeats:
            multitrack.setShowBars (! multitrack.isShowingBars());
            commands.commandStatusChanged();
            break;
        case cmdMixer:
            toggleMixer();
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
        case cmdDeleteAllMarkers:
            if (multitrackMode)
            {
                auto next = *engine.session.getState();
                next.markers.clear();
                engine.session.apply (std::move (next), "Delete All Markers");
            }
            else
            {
                engine.document.setMarkers ({}, "Delete All Markers");
            }
            break;
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
        case 0: add ({ cmdOpen, 0, cmdSave, cmdSaveAs, 0, cmdNewSession, cmdOpenSession, cmdSaveCopyWithAudio, 0, cmdExportMarkers }); break;
        case 1: add ({ cmdUndo, cmdRedo, cmdUndoHistory, 0, cmdCut, cmdCopy, cmdPaste, cmdDelete, 0, cmdSelectAll, cmdSelectTrackClips, cmdSelectClipsAfter, cmdCrop, 0, cmdConvertRate }); break;
        case 2: add ({ cmdGain, cmdNormalize, cmdMatchLoudness, cmdLoudnessStats, 0, cmdFadeIn, cmdFadeOut, 0, cmdSilence, cmdInvert, cmdReverse, 0,
                       cmdSpectralAttenuate, cmdSpectralRemove, 0, cmdCaptureNoisePrint, cmdNoiseReduction, cmdClickRepair, 0, cmdApplyRack }); break;
        case 3: add ({ cmdShowWaveform, cmdShowSpectrogram, cmdShowBoth, cmdMultitrack, 0, cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdZoomSelection,
                       0, cmdTallerTracks, cmdShorterTracks, cmdShowAllTracks }); break;
        case 4: add ({ cmdMixer, cmdClipProperties, cmdClipList, cmdMarkerList, 0, cmdImportToSession, 0, cmdAddTrack, cmdAddBus, cmdAddFolder, cmdAddFromTemplate, cmdSaveTrackTemplate, cmdDuplicateTrack, cmdDeleteTrack, cmdMoveTrackUp, cmdMoveTrackDown, cmdHideTrack, 0, cmdSplitClip, cmdDuplicateClip, cmdRepeatClip, cmdReverseClip, cmdStretchClip, cmdShiftPitch, cmdLockClip, cmdMuteClip, cmdGroupClips, cmdUngroupClips, cmdClipGainLines, cmdNudgeEarlier, cmdNudgeLater, cmdMoveClipsToCursor, cmdCloseGaps, cmdQuantize, 0, cmdClearRange, cmdDeleteTime, cmdInsertTime, cmdCutTime, cmdCopyTime, cmdPasteTime, 0, cmdSnapping, cmdZeroCrossings, cmdCrossfade, cmdWriteAutomation, cmdLoopRecording, cmdInputMonitoring, 0, cmdMetronome, cmdCountIn, cmdTempo, cmdTempoChange, cmdBarsAndBeats, 0, cmdBounceTrack, cmdFreezeTrack, cmdMixDown, cmdExportStems, cmdExportFolderStems, cmdExportRegions }); break;
        case 5: add ({ cmdPlayPause, cmdLoopPlayback, cmdRecord, cmdRecordingOptions, 0, cmdGoToStart, cmdGoToEnd }); break;
        case 6: add ({ cmdAddMarker, 0, cmdPreviousMarker, cmdNextMarker, 0, cmdDeleteAllMarkers }); break;
        case 7: add ({ cmdPlugins, cmdAudioSettings, cmdKeyShortcuts, 0, cmdSaveRack, cmdLoadRack }); break;
        default: break;
    }

    return menu;
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == commands.getKeyMappings())
    {
        KeyShortcuts::store (*commands.getKeyMappings(), *appProperties.getUserSettings());
        return;
    }

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
    keepRecoveryCopy();
    updatePlaybackLoop();

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
            if (! recordingLoop && rolls.isDone (played, punchOut, rate))
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
        // A punch-out stops by itself once the end of the range and the post-roll have come back through the input.
        if (rolls.isDone (player.getPlayedFrame().load() - recordLatency, punchOut, rate))
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
    overview.setVisible (on);
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

void MainComponent::openSession (const juce::File& file, std::optional<juce::File> as)
{
    auto loaded = SessionFile::load (file, engine.formatManager);

    if (loaded.error.isNotEmpty())
    {
        showError ("Can't open session", loaded.error);
        return;
    }

    engine.getPlayer().pause();
    engine.clearTrackEffects();
    engine.session.reset (std::move (loaded.state), as.value_or (file));

    if (as.has_value())
        engine.session.markUnsaved();

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

void MainComponent::toggleMixer()
{
    // Deleted later, not inside its own close button's callback.
    if (mixerWindow != nullptr)
        mixerWindow = nullptr;
    else
        mixerWindow = std::make_unique<MixerWindow> (engine, [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                                                     {
                                                                         if (safe != nullptr)
                                                                             safe->toggleMixer();
                                                                     }); });

    commands.commandStatusChanged();
}

void MainComponent::duplicateTrack()
{
    const auto state = engine.session.getState();
    const auto index = multitrack.getSelectedTrack();

    if (! juce::isPositiveAndBelow (index, (int) state->tracks.size()))
        return;

    // The copy's effects load after it lands, from the source's chain as a session file holds it.
    auto effects = engine.getTrackEffects().createXml (state->tracks[(size_t) index].id);
    const auto copy = multitrack.duplicateSelectedTrack();

    if (copy == 0 || effects == nullptr)
        return;

    engine.session.markEffectsChanged();
    engine.loadTrackEffects ({ { copy, std::shared_ptr<juce::XmlElement> (std::move (effects)) } }, [this] (const juce::String& errors)
    {
        engine.session.sendChangeMessage();   // the track headers show the effects

        if (errors.isNotEmpty())
            showError ("Track effects", "Some of the track's effects could not be copied:\n\n" + errors);
    });
}

void MainComponent::chooseTrackTemplateToSave()
{
    const auto state = engine.session.getState();
    const auto index = multitrack.getSelectedTrack();

    if (! juce::isPositiveAndBelow (index, (int) state->tracks.size()))
        return;

    const auto trackId = state->tracks[(size_t) index].id;
    const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                               .getChildFile (juce::File::createLegalFileName (state->tracks[(size_t) index].name) + TrackTemplate::extension);

    chooser = std::make_unique<juce::FileChooser> ("Save track template", suggested, juce::String ("*") + TrackTemplate::extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, trackId] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() == juce::File())
                                  return;

                              const auto file = fc.getResult().withFileExtension (TrackTemplate::extension);

                              const auto xml = TrackTemplate::create (*engine.session.getState(), trackId, engine.getTrackEffects().createXml (trackId));

                              if (xml == nullptr || ! xml->writeTo (file))
                                  showError ("Save Track as Template", "Could not write " + file.getFullPathName());
                          });
}

void MainComponent::chooseTrackTemplateToAdd()
{
    chooser = std::make_unique<juce::FileChooser> ("Add track from template", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   juce::String ("*") + TrackTemplate::extension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              const auto xml = juce::XmlDocument::parse (file);
                              auto next = *engine.session.getState();
                              const auto added = xml != nullptr ? TrackTemplate::add (next, *xml) : TrackTemplate::Added {};

                              if (added.trackId == 0)
                              {
                                  showError ("Add Track from Template", file.getFileName() + " is not a track template.");
                                  return;
                              }

                              engine.session.apply (std::move (next), "Add Track from Template");

                              if (! added.missingBuses.isEmpty())
                                  showError ("Add Track from Template", "This session has no bus called " + added.missingBuses.joinIntoString (", ")
                                                                            + ", so the track doesn't send there.");

                              if (added.effects == nullptr)
                                  return;

                              engine.session.markEffectsChanged();
                              engine.loadTrackEffects ({ { added.trackId, added.effects } }, [this] (const juce::String& errors)
                              {
                                  engine.session.sendChangeMessage();   // the track headers show the effects

                                  if (errors.isNotEmpty())
                                      showError ("Track effects", "Some of the template's effects could not be loaded:\n\n" + errors);
                              });
                          });
}

void MainComponent::toggleUndoHistory()
{
    // Deleted later, not inside its own close button's callback.
    if (undoHistoryWindow != nullptr)
        undoHistoryWindow = nullptr;
    else
        undoHistoryWindow = std::make_unique<UndoHistoryWindow> (engine.session, [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->toggleUndoHistory();
            });
        });

    commands.commandStatusChanged();
}

void MainComponent::toggleMarkerList()
{
    // Deleted later, not inside its own close button's callback.
    if (markerListWindow != nullptr)
        markerListWindow = nullptr;
    else
        markerListWindow = std::make_unique<MarkerListWindow> (engine.session, multitrack, [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->toggleMarkerList();
            });
        });

    commands.commandStatusChanged();
}

void MainComponent::chooseMarkersCsv()
{
    // Taken now: what is shown when the command is chosen.
    const auto markers = multitrackMode ? engine.session.getState()->markers : engine.document.getMarkers();
    const auto rate = multitrackMode ? engine.session.getState()->sampleRate : engine.document.getSampleRate();
    const auto from = multitrackMode ? engine.session.getFile() : engine.document.getFile();
    const auto start = from != juce::File() ? from.getSiblingFile (from.getFileNameWithoutExtension() + " Markers.csv")
                                            : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Markers.csv");
    chooser = std::make_unique<juce::FileChooser> ("Export markers as CSV", start, "*.csv");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [markers, rate] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file != juce::File() && ! file.replaceWithText (MarkerEdits::toCsv (markers, rate)))
                                  showError ("Export Markers", "Couldn't write " + file.getFullPathName() + ".");
                          });
}

void MainComponent::toggleClipList()
{
    // Deleted later, not inside its own close button's callback.
    if (clipListWindow != nullptr)
        clipListWindow = nullptr;
    else
        clipListWindow = std::make_unique<ClipListWindow> (engine, multitrack, [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->toggleClipList();
            });
        });

    commands.commandStatusChanged();
}

void MainComponent::toggleClipProperties()
{
    // Deleted later, not inside its own close button's callback.
    if (clipPropertiesWindow != nullptr)
        clipPropertiesWindow = nullptr;
    else
        clipPropertiesWindow = std::make_unique<ClipPropertiesWindow> (engine, multitrack, [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->toggleClipProperties();
            });
        });

    commands.commandStatusChanged();
}

void MainComponent::showRecordingOptions()
{
    auto* box = new juce::AlertWindow ("Recording Options",
                                       "Over a selection or time range, playback starts the pre-roll before the punch-in and "
                                       "recording stops the post-roll after the punch-out. A count-in replaces the pre-roll.",
                                       juce::MessageBoxIconType::NoIcon);
    const auto text = [] (double seconds) { return juce::String (seconds, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };
    const auto limit = juce::String ((int) SessionRecording::Rolls::longest);
    box->addTextEditor ("preroll", text (rolls.preroll), "Pre-roll (seconds, 0 to " + limit + ")");
    box->addTextEditor ("postroll", text (rolls.postroll), "Post-roll (seconds, 0 to " + limit + ")");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        const auto read = [box] (const juce::String& name, double& into)
        {
            const auto value = box->getTextEditorContents (name).trim();

            if (value.isNotEmpty() && value.containsOnly ("0123456789."))
                into = juce::jlimit (0.0, SessionRecording::Rolls::longest, value.getDoubleValue());
        };

        read ("preroll", rolls.preroll);
        read ("postroll", rolls.postroll);
        appProperties.getUserSettings()->setValue (prerollKey, rolls.preroll);
        appProperties.getUserSettings()->setValue (postrollKey, rolls.postroll);
    }), true);
}

void MainComponent::showTempoDialog (bool atCursor)
{
    const auto state = engine.session.getState();
    const auto map = state->tempoMap();
    const auto bar = atCursor ? map.barAt (multitrack.getCursor()) : (juce::int64) 0;
    const auto existing = std::find_if (state->tempoChanges.begin(), state->tempoChanges.end(), [bar] (auto& c) { return c.bar == bar; });
    const auto hasChange = existing != state->tempoChanges.end();
    const auto barFrame = map.barFrame (bar);
    auto* box = new juce::AlertWindow (bar == 0 ? "Tempo" : "Tempo Change at Bar " + juce::String (bar + 1),
                                       bar == 0 ? "The metronome clicks on this tempo from the start of the session, up to the first tempo change."
                                                : "The tempo and beats in a bar from the start of bar " + juce::String (bar + 1)
                                                      + " (where the cursor is) up to the next change. Clips stay where they are.",
                                       juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("tempo", juce::String (hasChange ? existing->tempo : map.tempoAt (barFrame), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."),
                        "Beats a minute (20 to 400)");
    box->addTextEditor ("beats", juce::String (hasChange ? existing->beatsPerBar : map.beatsPerBarAt (barFrame)), "Beats in a bar (1 to 16)");
    box->getTextEditor ("tempo")->setInputRestrictions (7, ".0123456789");
    box->getTextEditor ("beats")->setInputRestrictions (2, "0123456789");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));

    if (hasChange)
        box->addButton ("Remove", 2);

    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, box, bar] (int result)
        {
            if (result == 2)
            {
                auto next = *engine.session.getState();

                if (SessionEdits::removeTempoChange (next, bar))
                    engine.session.apply (std::move (next), "Remove Tempo Change");

                return;
            }

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

            if (bar > 0)
            {
                if (SessionEdits::setTempoChange (next, { bar, tempo, beats }))
                    engine.session.apply (std::move (next), "Tempo Change");

                return;
            }

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

            // The session's markers go with it, where the format holds them.
            *error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), rate, file, bitDepth, engine.formatManager,
                                        MarkerEdits::afterCrop (state->markers, range.getStart(), range.getEnd()));
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

void MainComponent::bounceTrack()
{
    const auto state = engine.session.getState();
    const auto index = multitrack.getSelectedTrack();

    if (! juce::isPositiveAndBelow (index, (int) state->tracks.size()) || state->tracks[(size_t) index].isBus)
        return;

    // The time range if there is one, otherwise from the start to the track's last clip.
    const auto& track = state->tracks[(size_t) index];
    const auto trackId = track.id;
    const auto timeRange = multitrack.getTimeSelection();
    juce::int64 end = 0;

    for (auto& c : track.clips)
        end = juce::jmax (end, c.getEnd());

    const auto range = ! timeRange.isEmpty() ? timeRange : juce::Range<juce::int64> (0, end);
    const auto rate = engine.session.getSampleRate();
    const auto file = recordingsFolder().getNonexistentChildFile (juce::File::createLegalFileName (track.name) + " Bounce "
                                                                      + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S"),
                                                                  ".wav", false);
    auto alone = std::make_shared<SessionState> (Bounce::isolate (*state, trackId));
    auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

    if (auto xml = engine.getTrackEffects().createXml (trackId))
        (*effects)[trackId] = std::move (xml);

    auto inserts = std::make_shared<TrackEffects>();
    auto audio = std::make_shared<AudioDocument::Snapshot>();
    auto error = std::make_shared<juce::String>();
    auto warnings = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Bouncing " + track.name + "...",
        [this, alone, effects, inserts, audio, error, warnings, file, rate, range] (const std::function<bool (float)>& progress)
        {
            *warnings = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts);
            juce::AudioBuffer<float> mix;

            if (! OfflineRender::mixSession (*alone, range, mix, inserts.get(), progress))
                return false;

            *audio = AudioEdits::fromBuffer (std::move (mix));

            if (! file.getParentDirectory().createDirectory())
                *error = "Could not make the folder " + file.getParentDirectory().getFullPathName();
            else
                *error = AudioFileIO::save (**audio, rate, file, 32, engine.formatManager);

            return true;
        },
        [this, audio, error, warnings, file, trackId, range]
        {
            if (error->isNotEmpty())
            {
                showError ("Bounce Track", *error);
                return;
            }

            auto next = *engine.session.getState();

            if (Bounce::land (next, trackId, file, *audio, range.getStart()) == 0)
            {
                showError ("Bounce Track", "The track was deleted while it was bouncing; the bounce is in " + file.getFullPathName() + ".");
                return;
            }

            engine.session.apply (std::move (next), "Bounce Track");

            if (warnings->isNotEmpty())
                showError ("Bounce Track", file.getFileName() + " was bounced without these effects, which could not be loaded:\n" + *warnings);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::freezeOrUnfreezeTrack()
{
    const auto state = engine.session.getState();
    const auto index = multitrack.getSelectedTrack();

    if (! juce::isPositiveAndBelow (index, (int) state->tracks.size()) || state->tracks[(size_t) index].isBus)
        return;

    const auto& track = state->tracks[(size_t) index];
    const auto trackId = track.id;

    if (track.isFrozen())
    {
        auto next = *state;
        SessionEdits::unfreezeTrack (next, trackId);
        engine.session.apply (std::move (next), "Unfreeze Track");
        return;
    }

    // From frame 0, so the frozen audio lines up with the session, to the last clip's end
    // plus a tail for reverbs and delays to ring out.
    juce::int64 end = 0;

    for (auto& c : track.clips)
        end = juce::jmax (end, c.getEnd());

    if (end == 0)
        return;

    const auto rate = engine.session.getSampleRate();
    const auto range = juce::Range<juce::int64> (0, end + (juce::int64) (SessionEdits::freezeTailSeconds * rate));
    const auto file = recordingsFolder().getNonexistentChildFile (juce::File::createLegalFileName (track.name) + " Frozen "
                                                                      + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S"),
                                                                  ".wav", false);
    auto alone = std::make_shared<SessionState> (SessionEdits::frozenSource (*state, trackId));
    auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

    if (auto xml = engine.getTrackEffects().createXml (trackId))
        (*effects)[trackId] = std::move (xml);

    auto inserts = std::make_shared<TrackEffects>();
    auto audio = std::make_shared<AudioDocument::Snapshot>();
    auto error = std::make_shared<juce::String>();
    auto warnings = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Freezing " + track.name + "...",
        [this, alone, effects, inserts, audio, error, warnings, file, rate, range] (const std::function<bool (float)>& progress)
        {
            *warnings = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts);
            juce::AudioBuffer<float> mix;

            if (! OfflineRender::mixSession (*alone, range, mix, inserts.get(), progress))
                return false;

            *audio = AudioEdits::fromBuffer (std::move (mix));

            if (! file.getParentDirectory().createDirectory())
                *error = "Could not make the folder " + file.getParentDirectory().getFullPathName();
            else
                *error = AudioFileIO::save (**audio, rate, file, 32, engine.formatManager);

            return true;
        },
        [this, audio, error, warnings, file, trackId]
        {
            if (error->isNotEmpty())
            {
                showError ("Freeze Track", *error);
                return;
            }

            auto next = *engine.session.getState();

            if (! SessionEdits::freezeTrack (next, trackId, file, *audio))
            {
                showError ("Freeze Track", "The track was deleted while it was freezing; its audio is in " + file.getFullPathName() + ".");
                return;
            }

            engine.session.apply (std::move (next), "Freeze Track");

            if (warnings->isNotEmpty())
                showError ("Freeze Track", file.getFileName() + " was frozen without these effects, which could not be loaded:\n" + *warnings);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::reverseSelectedClip()
{
    const auto state = engine.session.getState();
    const auto clipId = multitrack.getSelectedClip();
    const auto where = state->findClip (clipId);

    if (! where.has_value())
        return;

    // The reversed audio is written to a file of its own, so the session can be saved and opened again.
    const auto& clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];

    if (clip.locked)
        return;

    const auto reversed = SessionEdits::reversedAudio (clip);
    const auto folder = recordingsFolder();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (clip.name) + " Reversed", ".wav", false);
    auto error = reversed == nullptr ? juce::String ("The clip has no audio.")
                                     : ! folder.createDirectory() ? "Could not make the folder " + folder.getFullPathName()
                                                                  : AudioFileIO::save (*reversed, state->sampleRate, file, 32, engine.formatManager);

    if (error.isNotEmpty())
    {
        showError ("Reverse Clip", error);
        return;
    }

    auto next = *state;
    SessionEdits::reverseClip (next, clipId, file, reversed);
    engine.session.apply (std::move (next), "Reverse Clip");
}

void MainComponent::askToStretchSelectedClip()
{
    auto* box = new juce::AlertWindow ("Stretch Clip", "The clip's new length, keeping its pitch.", juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("percent", "100", "Length (% of now, " + juce::String (juce::roundToInt (TimeStretch::shortest * 100.0)) + " to "
                                              + juce::String (juce::roundToInt (TimeStretch::longest * 100.0)) + ")");
    box->getTextEditor ("percent")->setInputRestrictions (6, ".0123456789");
    box->addButton ("Stretch", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        const auto ratio = box->getTextEditorContents ("percent").getDoubleValue() / 100.0;

        if (result != 1)
            return;

        if (ratio < TimeStretch::shortest || ratio > TimeStretch::longest)
            showError ("Stretch Clip", "The length must be from 50% to 200% of what it is now.");
        else if (std::abs (ratio - 1.0) > 1.0e-9)
            processSelectedClip ("Stretch Clip", "Stretched", [ratio, rate = engine.session.getSampleRate()] (const SessionClip& c, const auto& progress)
            {
                return SessionEdits::stretchedAudio (c, ratio, rate, progress);
            });
    }), true);
}

void MainComponent::askToShiftSelectedClip()
{
    auto* box = new juce::AlertWindow ("Shift Clip Pitch", "The clip's pitch, keeping its length.", juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("semitones", "0", "Semitones (-12 to +12)");
    box->getTextEditor ("semitones")->setInputRestrictions (6, "-+.0123456789");
    box->addButton ("Shift", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        const auto semitones = box->getTextEditorContents ("semitones").getDoubleValue();

        if (result != 1)
            return;

        if (std::abs (semitones) > TimeStretch::widestShift)
            showError ("Shift Clip Pitch", "The shift must be from -12 to +12 semitones.");
        else if (std::abs (semitones) > 1.0e-9)
            processSelectedClip ("Shift Clip Pitch", "Shifted", [semitones, rate = engine.session.getSampleRate()] (const SessionClip& c, const auto& progress)
            {
                return SessionEdits::pitchedAudio (c, semitones, rate, progress);
            });
    }), true);
}

void MainComponent::processSelectedClip (const juce::String& action, const juce::String& suffix,
                                         std::function<AudioDocument::Snapshot (const SessionClip&, const std::function<bool (float)>&)> process)
{
    const auto state = engine.session.getState();
    const auto clipId = multitrack.getSelectedClip();
    const auto where = state->findClip (clipId);

    if (! where.has_value())
        return;

    // Like a reversed clip, the processed audio gets a file of its own.
    const auto& clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];

    if (clip.locked)
        return;

    if (clip.audio == nullptr || backgroundJob != nullptr)
        return;

    // Processed and written on a thread of its own, with a progress window and Cancel.
    const auto folder = recordingsFolder();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (clip.name) + " " + suffix, ".wav", false);
    const auto rate = state->sampleRate;
    auto stretched = std::make_shared<AudioDocument::Snapshot>();
    auto error = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, action + ": " + clip.name + "...",
        [this, clip, process, rate, folder, file, stretched, error] (const std::function<bool (float)>& progress)
        {
            *stretched = process (clip, progress);

            if (*stretched == nullptr)
                return false;

            *error = ! folder.createDirectory() ? "Could not make the folder " + folder.getFullPathName()
                                                : AudioFileIO::save (**stretched, rate, file, 32, engine.formatManager);
            return true;
        },
        [this, clipId, file, stretched, error, action]
        {
            if (error->isNotEmpty())
            {
                showError (action, *error);
                return;
            }

            auto next = *engine.session.getState();

            if (SessionEdits::stretchClip (next, clipId, file, *stretched))
                engine.session.apply (std::move (next), action);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::chooseStemsFolder (bool folders)
{
    const auto session = engine.session.getFile();
    chooser = std::make_unique<juce::FileChooser> (folders ? "Export folder stems to" : "Export stems to", session != juce::File() ? session.getParentDirectory()
                                                                                          : juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, folders] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();

                              if (folder == juce::File())
                                  return;

                              // The time range if there is one, otherwise the whole session from its start.
                              const auto timeRange = multitrack.getTimeSelection();
                              exportStems (folder, ! timeRange.isEmpty() ? timeRange
                                                                         : juce::Range<juce::int64> (0, engine.session.getState()->getLength()),
                                           folders);
                          });
}

void MainComponent::chooseRegionsFolder()
{
    const auto session = engine.session.getFile();
    chooser = std::make_unique<juce::FileChooser> ("Export regions to", session != juce::File() ? session.getParentDirectory()
                                                                                           : juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (const auto folder = fc.getResult(); folder != juce::File())
                                  exportRegions (folder);
                          });
}

void MainComponent::exportRegions (const juce::File& folder)
{
    const auto state = engine.session.getState();
    const auto rate = engine.session.getSampleRate();
    const auto sessionName = engine.session.getFile() != juce::File() ? engine.session.getFile().getFileNameWithoutExtension()
                                                                      : juce::String ("Session");
    const auto regions = std::make_shared<const std::vector<std::pair<juce::Range<juce::int64>, juce::File>>> (Bounce::regionFiles (*state, sessionName, folder));
    auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

    for (auto& track : state->tracks)
        if (auto xml = engine.getTrackEffects().createXml (track.id))
            (*effects)[track.id] = std::move (xml);

    // Fresh plug-ins for each region, so none carries another's tail; kept until the job is
    // deleted on the message thread.
    auto inserts = std::make_shared<std::vector<std::unique_ptr<TrackEffects>>>();
    auto errors = std::make_shared<juce::StringArray>();
    auto warnings = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Exporting regions...",
        [this, state, regions, effects, inserts, errors, warnings, rate] (const std::function<bool (float)>& progress)
        {
            const auto count = (float) regions->size();

            for (size_t i = 0; i < regions->size(); ++i)
            {
                const auto& [range, file] = (*regions)[i];
                inserts->push_back (std::make_unique<TrackEffects>());
                const auto missing = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts->back());

                if (i == 0)
                    *warnings = missing;

                juce::AudioBuffer<float> mix;
                const auto part = [&progress, i, count] (float p) { return progress (((float) i + p) / count); };

                if (! OfflineRender::mixSession (*state, range, mix, inserts->back().get(), part))
                    return false;

                if (const auto error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), rate, file, 32, engine.formatManager);
                    error.isNotEmpty())
                    errors->add (error);
            }

            return true;
        },
        [errors, warnings, folder]
        {
            if (! errors->isEmpty())
                showError ("Export Regions", errors->joinIntoString ("\n"));
            else if (warnings->isNotEmpty())
                showError ("Export Regions", "The regions were written to " + folder.getFullPathName()
                                                 + " without these effects, which could not be loaded:\n" + *warnings);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::exportStems (const juce::File& folder, juce::Range<juce::int64> range, bool folders)
{
    const auto state = engine.session.getState();
    const auto rate = engine.session.getSampleRate();
    const auto sessionName = engine.session.getFile() != juce::File() ? engine.session.getFile().getFileNameWithoutExtension()
                                                                      : juce::String ("Session");
    struct Stem
    {
        std::shared_ptr<const SessionState> alone;
        std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> effects;   // of each track and bus in it
        juce::File file;
    };

    // A track alone, or a folder with what is in it.
    auto stems = std::make_shared<std::vector<Stem>>();
    const auto ids = folders ? Bounce::stemFolders (*state) : Bounce::stemTracks (*state);

    for (size_t i = 0; i < ids.size(); ++i)
    {
        Stem stem;
        stem.alone = std::make_shared<const SessionState> (folders ? Bounce::isolateFolder (*state, ids[i]) : Bounce::isolate (*state, ids[i]));
        stem.file = folder.getChildFile (Bounce::stemFileName (sessionName, (int) i + 1, state->findTrack (ids[i])->name));

        for (auto& t : stem.alone->tracks)
            if (auto xml = engine.getTrackEffects().createXml (t.id))
                stem.effects[t.id] = std::move (xml);

        stems->push_back (std::move (stem));
    }

    // Fresh plug-ins for each stem, kept until the job is deleted on the message thread.
    auto inserts = std::make_shared<std::vector<std::unique_ptr<TrackEffects>>>();
    auto errors = std::make_shared<juce::StringArray>();
    auto warnings = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Exporting stems...",
        [this, stems, inserts, errors, warnings, rate, range] (const std::function<bool (float)>& progress)
        {
            const auto count = (float) stems->size();

            for (size_t i = 0; i < stems->size(); ++i)
            {
                auto& stem = (*stems)[i];
                inserts->push_back (std::make_unique<TrackEffects>());
                *warnings += OfflineRender::loadTrackEffects (stem.effects, engine.pluginFormatManager, rate, *inserts->back());
                juce::AudioBuffer<float> mix;
                const auto part = [&progress, i, count] (float p) { return progress (((float) i + p) / count); };

                if (! OfflineRender::mixSession (*stem.alone, range, mix, inserts->back().get(), part))
                    return false;

                if (const auto error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), rate, stem.file, 32, engine.formatManager);
                    error.isNotEmpty())
                    errors->add (error);
            }

            return true;
        },
        [errors, warnings, folder]
        {
            if (! errors->isEmpty())
                showError ("Export Stems", errors->joinIntoString ("\n"));
            else if (warnings->isNotEmpty())
                showError ("Export Stems", "The stems were written to " + folder.getFullPathName()
                                               + " without these effects, which could not be loaded:\n" + *warnings);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::saveSessionCopyWithAudio()
{
    const auto current = engine.session.getFile();
    const auto name = current != juce::File() ? current.getFileNameWithoutExtension() : juce::String ("Untitled");
    chooser = std::make_unique<juce::FileChooser> ("Save a copy with audio, in a new folder",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (name + " Copy"));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();

                              if (folder == juce::File())
                                  return;

                              // A folder of its own: the session file and its Audio folder inside.
                              if (folder.exists() || ! folder.createDirectory())
                              {
                                  showError ("Save a Copy with Audio", folder.getFullPathName() + " already exists or can't be made; choose a new name.");
                                  return;
                              }

                              juce::StringArray missing;
                              const auto file = folder.getChildFile (folder.getFileName() + ".sbsession");
                              const auto error = SessionCollect::save (*engine.session.getState(), file,
                                                                       [this] (juce::uint32 id) { return engine.getTrackEffects().createXml (id); }, missing);

                              if (error.isNotEmpty())
                                  showError ("Save a Copy with Audio", error);
                              else if (! missing.isEmpty())
                                  showError ("Save a Copy with Audio", "The copy was saved, but these files were missing and could not go with it:\n\n"
                                                                           + missing.joinIntoString ("\n"));
                          });
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
    if (multitrackMode)
    {
        multitrack.addMarker();
        return;
    }

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
    if (multitrackMode)
    {
        multitrack.goToMarker (forwards);
        return;
    }

    if (const auto target = MarkerEdits::nextStop (engine.document.getMarkers(), waveform.getCursor(), forwards))
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
    if (multitrackMode)
    {
        const auto state = engine.session.getState();
        const auto where = state->findClip (multitrack.getSelectedClip());

        if (! where.has_value())
            return;

        const auto clipId = multitrack.getSelectedClip();
        askForDecibels ("Clip Gain", "Gain (dB, " + juce::String ((int) SessionClip::minGainDb) + " to +" + juce::String ((int) SessionClip::maxGainDb) + ")",
                        state->tracks[(size_t) where->first].clips[(size_t) where->second].gainDb, [this, clipId] (float db)
        {
            auto next = *engine.session.getState();

            if (SessionEdits::setClipGain (next, clipId, db))
                engine.session.apply (std::move (next), "Clip Gain");
        });
        return;
    }

    askForDecibels ("Gain", "Gain (dB)", 0.0f, [this] (float db)
    {
        const auto r = selectionOrAll();
        applyEdit (AudioEdits::gain (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), db),
                   "Gain " + juce::String (db, 2) + " dB", waveform.getSelection());
    });
}

void MainComponent::applyNormalize()
{
    if (multitrackMode)
    {
        const auto clipId = multitrack.getSelectedClip();
        askForDecibels ("Normalize Clip", "Peak level (dBFS)", -0.1f, [this, clipId] (float db)
        {
            auto next = *engine.session.getState();

            if (! next.findClip (clipId).has_value())
                return;

            if (SessionEdits::normalizeClip (next, clipId, juce::jmin (db, 0.0f)))
                engine.session.apply (std::move (next), "Normalize Clip");
            else
                showError ("Normalize Clip", "The clip is silent.");
        });
        return;
    }

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

    // A selection or time range plays and stops at its end (or, looping, starts again);
    // otherwise play from the cursor to the end.
    const auto range = playRange();
    const auto cursor = multitrackMode ? multitrack.getCursor() : waveform.getCursor();
    const auto length = multitrackMode ? engine.session.getState()->getLength() : engine.document.getNumFrames();

    if (! range.isEmpty())
        player.play (range.getStart(), range.getEnd());
    else
        player.play (cursor >= length ? 0 : cursor, -1);

    playingLoop = {};
    updatePlaybackLoop();
}

juce::Range<juce::int64> MainComponent::playRange() const
{
    return multitrackMode ? multitrack.getTimeSelection() : waveform.getSelection();
}

void MainComponent::updatePlaybackLoop()
{
    auto& player = engine.getPlayer();

    // Loop recording keeps its own loop.
    if (isRecording())
        return;

    const auto wanted = loopPlayback && player.isPlaying() ? playRange() : juce::Range<juce::int64>();

    if (wanted == playingLoop)
        return;

    playingLoop = wanted;

    if (wanted.isEmpty())
        player.clearLoop();
    else
        player.setLoop (wanted.getStart(), wanted.getEnd());
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
        player.play (rolls.startFrame (punchIn, deviceRate), -1);

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

    // A count-in starts a bar (as long as the one the punch-in is in) before the punch-in, with
    // clicks up to it; otherwise the pre-roll.
    const auto now = session.getState();
    const auto map = now->tempoMap();
    const auto bar = map.barAt (punchIn);
    const auto from = countIn ? juce::jmax ((juce::int64) 0, punchIn - (map.barFrame (bar + 1) - map.barFrame (bar)))
                              : rolls.startFrame (punchIn, deviceRate);
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
    player.play (from, std::numeric_limits<juce::int64>::max() / 4);

    // A playback loop gives way: a take loops only with loop recording.
    playingLoop = {};

    if (recordingLoop)
        player.setLoop (punchIn, punchOut);
    else
        player.clearLoop();

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

void MainComponent::showKeyShortcuts()
{
    // Deleted later, not inside its own close button's callback.
    if (keyShortcutsWindow == nullptr)
        keyShortcutsWindow = std::make_unique<KeyShortcutsWindow> (*commands.getKeyMappings(), [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->keyShortcutsWindow = nullptr;
            });
        });
    else
        keyShortcutsWindow->toFront (true);
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
