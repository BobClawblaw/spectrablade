#include "MainComponent.h"
#include "AudioFileIO.h"
#include "OfflineRender.h"
#include "PluginScanner.h"

namespace
{
    constexpr const char* knownPluginsKey = "knownPlugins";
    constexpr const char* audioDeviceKey  = "audioDevice";
    constexpr const char* outputPairKey   = "outputPairFirstChannel";
    constexpr const char* lastFileKey     = "lastFile";
    constexpr const char* inputFirstKey   = "inputFirstChannel";
    constexpr const char* inputCountKey   = "inputNumChannels";

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
        cmdAddMarker, cmdPreviousMarker, cmdNextMarker, cmdDeleteAllMarkers
    };

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
        auto* list = new juce::PluginListComponent (audioEngine.pluginFormatManager, audioEngine.knownPlugins,
                                                    deadMansPedal, props, true);
        list->setNumberOfThreadsForScanning (1);
        setContentOwned (list, false);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (760, 560);
        setVisible (true);
    }

    void closeButtonPressed() override  { closeCallback(); }

private:
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
MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName     = "Waveloom";
    options.filenameSuffix      = "settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName          = "Waveloom";
    appProperties.setStorageParameters (options);

    auto* settings = appProperties.getUserSettings();
    deadMansPedalFile = settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");

    if (auto xml = settings->getXmlValue (knownPluginsKey))
        engine.knownPlugins.recreateFromXml (*xml);

    engine.knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    engine.knownPlugins.addChangeListener (this);
    engine.addChangeListener (this);
    engine.document.addChangeListener (this);

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

    for (auto* b : { &openButton, &playButton, &stopButton, &recordButton, &pluginsButton, &audioButton, &saveRackButton, &loadRackButton })
        addAndMakeVisible (b);

    openButton.onClick     = [this] { commands.invokeDirectly (cmdOpen, true); };
    playButton.onClick     = [this] { togglePlay(); };
    stopButton.onClick     = [this]
    {
        if (engine.getRecorder().isRecording())
            stopRecording();
        else
            engine.getPlayer().pause();

        waveform.repaint();
    };
    recordButton.onClick   = [this] { toggleRecord(); };
    recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkred);
    recordButton.setTooltip ("Record from the input (Shift+Space). Over a selection it punches in and out with a "
                             + juce::String (prerollSeconds, 0) + " s preroll; otherwise it records from the cursor.");
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

        if (id <= 0 || engine.getRecorder().isRecording())
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
    waveform.onFileDropped = [this] (const juce::File& f) { confirmDiscardChanges ([this, f] { openFile (f); }); };
    waveform.onSelectionChanged = [this] { commands.commandStatusChanged(); timerCallback(); };

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

    if (const juce::File last (settings->getValue (lastFileKey)); last.existsAsFile())
        openFile (last);

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
    if (engine.getRecorder().isRecording())
        engine.getRecorder().stop();   // the take is lost: the app is closing

    saveSettings();
    renderJob = nullptr;
    engine.onPluginAboutToBeRemoved = nullptr;
    engine.document.removeChangeListener (this);
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

    for (auto* b : { &openButton, &playButton, &stopButton, &recordButton })
    {
        b->setBounds (toolbar.removeFromLeft (b == &openButton || b == &recordButton ? 80 : 64));
        toolbar.removeFromLeft (6);
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
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (int id = cmdOpen; id <= cmdDeleteAllMarkers; ++id)
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
        case cmdRecord:     info.setInfo (engine.getRecorder().isRecording() ? "Stop Recording" : "Record",
                                          "Record from the input", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::spaceKey, shift); break;

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

    // While recording, the document must stay as it is so the take lands where it was started.
    if (engine.getRecorder().isRecording() && id != cmdRecord && id != cmdPlayPause && id != cmdAddMarker)
        info.setActive (false);
}

bool MainComponent::perform (const InvocationInfo& info)
{
    switch (info.commandID)
    {
        case cmdOpen:       confirmDiscardChanges ([this] { chooseFile(); }); break;
        case cmdSave:       save(); break;
        case cmdSaveAs:     saveAs(); break;
        case cmdUndo:       engine.document.undo(); break;
        case cmdRedo:       engine.document.redo(); break;
        case cmdCut:        cut(); break;
        case cmdCopy:       copy(); break;
        case cmdPaste:      paste(); break;
        case cmdDelete:     deleteSelection(); break;
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
        case cmdGoToStart:  waveform.setCursor (0); break;
        case cmdGoToEnd:    waveform.setCursor (engine.document.getNumFrames()); break;
        case cmdZoomIn:     waveform.zoomIn(); break;
        case cmdZoomOut:    waveform.zoomOut(); break;
        case cmdZoomFit:    waveform.zoomToFit(); break;
        case cmdZoomSelection: waveform.zoomToSelection(); break;
        case cmdPlugins:    showPluginList(); break;
        case cmdAudioSettings: showAudioSettings(); break;
        case cmdSaveRack:   saveRack(); break;
        case cmdLoadRack:   loadRack(); break;
        case cmdRecord:     toggleRecord(); break;
        case cmdAddMarker:  addMarker(); break;
        case cmdPreviousMarker: goToMarker (false); break;
        case cmdNextMarker: goToMarker (true); break;
        case cmdDeleteAllMarkers: engine.document.setMarkers ({}, "Delete All Markers"); break;
        default:            return false;
    }

    return true;
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Effects", "View", "Transport", "Markers", "Window" };
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
        case 0: add ({ cmdOpen, 0, cmdSave, cmdSaveAs }); break;
        case 1: add ({ cmdUndo, cmdRedo, 0, cmdCut, cmdCopy, cmdPaste, cmdDelete, 0, cmdSelectAll, cmdCrop }); break;
        case 2: add ({ cmdGain, cmdNormalize, 0, cmdFadeIn, cmdFadeOut, 0, cmdSilence, cmdInvert, cmdReverse, 0, cmdApplyRack }); break;
        case 3: add ({ cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdZoomSelection }); break;
        case 4: add ({ cmdPlayPause, cmdRecord, 0, cmdGoToStart, cmdGoToEnd }); break;
        case 5: add ({ cmdAddMarker, 0, cmdPreviousMarker, cmdNextMarker, 0, cmdDeleteAllMarkers }); break;
        case 6: add ({ cmdPlugins, cmdAudioSettings, 0, cmdSaveRack, cmdLoadRack }); break;
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

    if (source == &engine.document)
    {
        refreshTitle();
        commands.commandStatusChanged();
        return;
    }

    refreshOutputChoices();
    refreshInputChoices();
    commands.commandStatusChanged();
}

void MainComponent::timerCallback()
{
    auto& player = engine.getPlayer();
    auto& recorder = engine.getRecorder();
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
    auto& doc = engine.document;
    const auto name = doc.getFile() == juce::File() ? juce::String ("Untitled") : doc.getFile().getFileName();
    fileLabel.setText (doc.hasAudio() ? name + "  |  " + juce::String (doc.getSampleRate() / 1000.0, 1) + " kHz, "
                                          + juce::String (doc.getNumChannels()) + " ch, "
                                          + (doc.getSourceBitDepth() == 32 ? juce::String ("32-bit float")
                                                                            : juce::String (doc.getSourceBitDepth()) + "-bit")
                                      : juce::String(),
                       juce::dontSendNotification);

    if (onTitleChanged != nullptr)
        onTitleChanged ((doc.isDirty() ? "* " : "") + name + " - Waveloom");
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
    inputChoice.setEnabled (! engine.getRecorder().isRecording());
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

void MainComponent::openFile (const juce::File& file)
{
    const auto error = engine.openFile (file);

    if (error.isNotEmpty())
        showError ("Can't open file", file.getFullPathName() + "\n\n" + error);
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
                                  showError ("Save As", "Waveloom can't write " + file.getFileExtension() + " files.");
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
        showError ("Paste", "The clipboard is " + juce::String (clipboard.sampleRate) + " Hz but this file is "
                              + juce::String (doc.getSampleRate()) + " Hz. Sample-rate conversion comes in a later version.");
        return;
    }

    auto sel = waveform.getSelection();

    if (sel.isEmpty())
        sel = { waveform.getCursor(), waveform.getCursor() };

    applyEdit (AudioEdits::replaceWithClip (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), clipboard),
               "Paste", { sel.getStart(), sel.getStart() + clipboard.getNumFrames() },
               MarkerEdits::afterReplace (doc.getMarkers(), sel.getStart(), sel.getEnd(), clipboard.getNumFrames()));
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
    if (engine.getRecorder().isRecording())
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

    // A selection plays and stops at its end; otherwise play from the cursor to the end.
    const auto sel = waveform.getSelection();

    if (sel.isEmpty())
        player.play (waveform.getCursor() >= engine.document.getNumFrames() ? 0 : waveform.getCursor(), -1);
    else
        player.play (sel.getStart(), sel.getEnd());
}

void MainComponent::toggleRecord()
{
    if (engine.getRecorder().isRecording())
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
            player.documentReset();
        }
    }
    else if (! juce::exactlyEqual (doc.getSampleRate(), deviceRate))
    {
        showError ("Record", "This file is " + juce::String (doc.getSampleRate()) + " Hz but the interface runs at "
                               + juce::String (deviceRate) + " Hz. Set the interface to the file's rate in Audio Settings "
                               "to record into it. Sample-rate conversion comes in a later version.");
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
                              + ", punch out " + juce::String (punchOut) + ", round trip " + juce::String (recordLatency));
    refreshInputChoices();
    commands.commandStatusChanged();
}

void MainComponent::stopRecording()
{
    auto& recorder = engine.getRecorder();

    if (! recorder.isRecording())
        return;

    auto take = recorder.stop();
    engine.getPlayer().pause();
    refreshInputChoices();
    commands.commandStatusChanged();

    juce::Logger::writeToLog ("Recorded " + juce::String (take.clip.getNumFrames()) + " frames, anchor "
                              + juce::String (take.anchorRecorded) + " -> " + juce::String (take.anchorPlayed)
                              + ", dropped " + juce::String (take.droppedFrames));

    if (take.droppedFrames > 0)
        showError ("Record", juce::String (take.droppedFrames) + " frames were lost because the computer could not keep up. "
                               "Listen to the take before keeping it.");

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
    auto* plugin = engine.getRackPlugin (slotIndex);

    if (plugin == nullptr)
        return;

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
    chooser = std::make_unique<juce::FileChooser> ("Save effects rack", juce::File(), "*.wlrack");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! engine.createRackXml()->writeTo (file.withFileExtension ("wlrack")))
                                  showError ("Save failed", "Could not write " + file.getFullPathName());
                          });
}

void MainComponent::loadRack()
{
    chooser = std::make_unique<juce::FileChooser> ("Load effects rack", juce::File(), "*.wlrack");
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
