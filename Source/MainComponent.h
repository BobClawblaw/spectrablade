#pragma once

#include "AudioEdits.h"
#include "AudioEngine.h"
#include "AuditionPreview.h"
#include "WindowPlaces.h"
#include "FileChecks.h"
#include "LoadingScreen.h"
#include "ClickRepair.h"
#include "LevelMeter.h"
#include "TransportBar.h"
#include "LoadReadout.h"
#include "TapTempo.h"
#include "TimeEntry.h"
#include "RecentFiles.h"
#include "Loudness.h"
#include "MultitrackView.h"
#include "NoiseReduction.h"
#include "ParametricEq.h"
#include "Dynamics.h"
#include "Multiband.h"
#include "Modulation.h"
#include "Reverb.h"
#include "Convolver.h"
#include "EffectPresets.h"
#include "RackView.h"
#include "SessionOverview.h"
#include "SessionRecording.h"
#include "WaveformView.h"
#include "Dock.h"
#include "Batch.h"
#include "EditorBar.h"

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            private juce::ChangeListener,
                            private juce::Timer,
                            private Dock::Host
{
public:
    MainComponent();
    ~MainComponent() override;

    /** Asks about unsaved changes to the file, then calls proceed unless the user cancels. */
    void confirmDiscardChanges (std::function<void()> proceed);

    /** Asks about unsaved changes to the file and then the session, then calls proceed. */
    void confirmQuit (std::function<void()> proceed);

    /** A file Finder, the Dock or the command line asked to open: a session in the multitrack, any
        other file in the editor, each after asking about unsaved changes. */
    void openDocument (const juce::File&);

    /** Opens several files at once, each beside the others (the last shows); a session among them
        is opened instead of the audio files. */
    void openDocuments (const juce::Array<juce::File>&);

    std::function<void (const juce::String& title)> onTitleChanged;

    /** Shows the loading screen centred on where the main window will be; whenReady shows that
        window, once loading is done or Continue is pressed. */
    void showLoadingScreen (juce::Rectangle<int> windowBounds, std::function<void()> whenReady);

    /** Puts a window where it was left last time (under this name) and remembers where it goes. */
    void rememberPlace (juce::ResizableWindow& window, const juce::String& name)   { places->watch (window, name); }

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    ApplicationCommandTarget* getNextCommandTarget() override  { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    void describeCommand (juce::CommandID, juce::ApplicationCommandInfo&);
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int menuItemId, int) override;

private:
    class PluginEditorWindow;
    class EditorPanel;
    class KeyShortcutsWindow;
    class EqWindow;
    class PluginListWindow;
    class RackRenderJob;
    class BackgroundJob;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateInputs();
    bool isRecording();

    // File
    void openFile (const juce::File&);
    void chooseFile();
    juce::String openWildcard();
    void save();
    void saveAs (std::function<void (bool saved)> done = {});
    void saveSelectionAs();
    void chooseFileRegionsFolder (bool betweenMarkers);
    void saveFileRegions (const juce::File& folder, bool betweenMarkers);

    /** Asks for a file to save audio to and its sample format, then calls chosen with them, or with
        no file when the person cancels or picks a type that can't be written. */
    void askForAudioTarget (const juce::String& title, const juce::File& suggested, std::function<void (const juce::File&, int)> chosen);
    void writeFile (const juce::File&, int bitDepth, std::function<void (bool saved)> done);

    // Edit
    juce::Range<juce::int64> selectionOrAll() const;
    void applyEdit (AudioDocument::Snapshot next, const juce::String& description, juce::Range<juce::int64> newSelection);
    void applyEdit (AudioDocument::Snapshot next, const juce::String& description, juce::Range<juce::int64> newSelection,
                    Markers);
    void cut();
    void copy();
    enum class PasteMode { insert, mix, fill };
    void paste (PasteMode);
    void pasteClip (const AudioClip&, PasteMode);
    void chooseSampleRate();
    void convertSampleRate (double newRate);
    void deleteSelection();
    void cropToSelection();

    // Spectrogram
    void setDisplay (WaveformView::Display);
    void applySpectral (float gainDb);
    void healSpectral();   // the drawn shape, or the box, healed from the sound around it

    // Markers
    void addMarker();
    void goToMarker (bool forwards);
    void selectBetweenMarkers();
    void selectRegionAtCursor();
    void loopRegion();
    void selectAdjacentRegion (bool forwards);

    /** The open file's markers, or the session's while it shows. */
    Markers markersShown() const;
    void showMarkersAtIntervals();
    void regionsFromMarkers();
    void showRenameMarkers();
    void showGoToTime();

    // Effects
    void showInsertSilence();
    void insertSilence (juce::int64 frames);
    void trimSilence (float thresholdDb);
    void selectAudibleSpan();
    void findNextClipping();
    /** Asks for a silence level and shortest time (kept for next time), then calls found with the time in frames. */
    void askForSilence (const juce::String& title, std::function<void (juce::int64)> found);
    void addMarkersAtSilences (juce::int64 minFrames);
    void shortenPauses (juce::int64 keepFrames);
    void noiseGate (juce::int64 minFrames);
    void repairSelection();
    void showFadeEdges (bool eachRegion);
    void showGainRamp();
    void showTremolo();
    void showEcho();
    void showEqBand();
    void showRemoveHum();
    void showDeclip();
    void showGenerateTone();
    void showGenerateNoise();
    void exportLossy();
    void askLossySettings (const juce::File&, juce::Range<juce::int64> selection);
    void exportWithFfmpeg();
    void batchProcess();
    void askForBatchSettings (const juce::Array<juce::File>&);
    void runBatch (const juce::Array<juce::File>&, const Batch::Settings&);
    void showBatchReport (const std::vector<Batch::Outcome>&);

    // Recorded command sequences
    Sequences::Result runStep (const Sequences::Step&);   // a command's edit, through the step that plays it back

    /** A step that takes a while (Stretch, Shift Pitch) on the selection or the whole file, in
        the background with a progress window, applied as one undo step unless the file changed
        meanwhile; a selection is kept over the processed range. */
    void runStepInBackground (const Sequences::Step&, const juce::String& busy);
    void askToStretch();
    void askToShiftPitch();
    void askToReduceReverb();
    void askForCentreChannel();
    void askForPhaseCorrection();
    void askToLevelSpeech();
    void askForVideoStart();
    void detachVideo();
    void saveMixIntoVideo();
    double stretchPercent = 100.0, pitchSemitones = 0.0;   // as last asked
    void recordStep (const Sequences::Step&);              // after the edit is applied
    void noticeUnrecordedEdits();
    void toggleSequenceRecording();
    void choosePlaySequence();
    void playSequence (const Sequences::Sequence&);
    Sequences::Recorder sequenceRecorder;

    // Surround files
    void saveChannels();
    void newFileFromChannels();
    void attachVideo();
    void saveIntoVideo();
    void showVideoPanel();
    void chooseProject();
    void openProject (const juce::File&);
    void restoreProject (const juce::File&);
    void saveProject (bool chooseFile);
    void writeProject (const juce::File&);
    juce::String projectSuffix() const;
    void combineLoaded (const juce::Array<juce::File>&, const std::vector<AudioFileIO::Loaded>&);
    int sequencePosition = 0, sequenceResetCount = 0;      // where the document was after the last recorded step
    void showFileInfo();
    void insertGenerated (const juce::AudioBuffer<float>&, const juce::String& name);
    int toneWave = 0;                // the generators' last settings
    double toneHz = 1000.0, toneEndHz = 1000.0, toneLevelDb = -12.0, toneSeconds = 1.0, toneFadeMs = 5.0;
    int noiseColour = 0;
    double noiseLevelDb = -20.0, noiseSeconds = 1.0, noiseFadeMs = 5.0;
    double declipThreshold = 0.99;   // Repair Clipping's last settings
    bool declipKeepPeak = true;
    int lossyMp3Choice = 1, lossyAacChoice = 1, ffmpegTargetChoice = 0;   // Export as MP3 or AAC's last settings
    bool humIs60 = false;   // Remove Hum's last settings
    int humHarmonics = 8;
    double humQ = 30.0;
    void toggleParametricEq();
    void showCompressor();
    void showLimiter();
    Dynamics::Compressor compressorSettings;
    Multiband::Compressor multibandSettings;
    Multiband::DeEsser deEsserSettings;
    void showMultiband();
    void showDeEsser();
    Dynamics::Limiter limiterSettings;
    void showSweptDelay (bool flanger);
    void showPhaser();
    Modulation::Delay chorusSettings = Modulation::chorusDefaults(), flangerSettings = Modulation::flangerDefaults();
    Modulation::Phaser phaserSettings;
    void showReverb();
    RoomReverb::Settings reverbSettings;
    void showConvolutionReverb();
    void showConvolutionDialog (std::shared_ptr<const Convolver::Impulse>);
    juce::File lastImpulseFile;
    double convolutionMix = 30.0;
    void showGraphicEq (int numBands);
    void showAbout();
    std::vector<double> graphicEq10Gains = std::vector<double> (10, 0.0), graphicEq31Gains = std::vector<double> (31, 0.0);

    void applyParametricEq (const std::vector<Equalizer::Band>&);
    void previewParametricEq (const ParametricEq*);   // nullptr: stop
    ParametricEq::Settings eqSettings = ParametricEq::defaultSettings();
    int eqBandType = 0;
    double eqBandHz = 1000.0, eqBandGainDb = 6.0, eqBandQ = 1.0;
    int echoDelayMs = 250, echoFeedbackPercent = 40;
    double tremoloRateHz = 5.0;
    int tremoloDepthPercent = 50;
    void showPanRamp();
    void showStereoWidth();
    int stereoWidthPercent = 50;
    float rampFromDb = 0.0f, rampToDb = -6.0f;
    void showCropMargins();
    void showMoveMarkers();
    double cropMarginSeconds = 0.5;
    double edgeFadeMs = 10.0;
    void addMarkersAtClipping();
    void addRegionsOverSounds (juce::int64 minFrames);
    void selectNextSound();
    void selectPreviousSound();
    void addMarkersAt (const std::vector<juce::int64>& frames, const juce::String& title, const juce::String& noneFound);
    static constexpr double gateFadeSeconds = 0.01;
    void fitRegionsToSound();
    void findNextSilence (juce::int64 minFrames);

    /** What Find Next Clipping counts: samples within about 0.01 dB of full scale, three in a row. */
    static constexpr float clipLevel = 0.999f;
    static constexpr int clipRun = 3;
    void applyFade (bool out);
    void showCrossfadeLoop();
    void showRepeatSelection();
    static constexpr int maxRepeats = 100;
    void showBalance();
    void showSetSelection();
    void convertChannels (int numChannels);
    void askForDecibels (const juce::String& title, const juce::String& label, float initial,
                         std::function<void (float)> onOk);
    void applyGain();
    void applyNormalize();
    void applySimpleEffect (const juce::String& name,
                            AudioDocument::Snapshot (*effect) (const AudioSnapshot&, juce::int64, juce::int64),
                            bool needsSelection);
    void applyRackToSelection();

    // Loudness
    void measureLoudness (std::function<void (const Loudness::Result&, juce::Range<juce::int64>)> done);
    void showLoudness();
    void askForLoudnessTarget();
    void matchLoudness (double target, double ceiling, bool limitToTarget);

    // Noise reduction
    void captureNoisePrint();
    void askForNoiseReduction();
    void reduceNoise (NoiseReduction::Settings);
    void askForClickRepair();
    void repairClicks (ClickRepair::Settings);

    // Multitrack
    void setMultitrack (bool);
    void confirmDiscardSession (std::function<void()> proceed);
    void newSession();
    void chooseSession();
    /** Opens a session file. With `as`, the session takes that file as its own (none for an
        unsaved one) and starts with changes to save: a recovered copy. */
    void openSession (const juce::File&, std::optional<juce::File> as = std::nullopt);

    /** Writes a recovery copy of the session when it has changes made since the last copy, at
        most every SessionRecovery::intervalSeconds; removes it as soon as there is nothing
        unsaved (the session was saved, or undone back to where it was). */
    void keepRecoveryCopy();

    /** At startup, offers the copy left by a session that wasn't closed normally. */
    void offerRecoveredSession();

    // Launch: a small loading screen stands where the window will be until the plug-in list is
    // read, the audio device is open and the last file is back (or Continue is pressed).
    std::unique_ptr<LoadingScreen> loading;
    std::function<void()> whenReady;
    bool booting() const                          { return loading != nullptr && ! loading->hasFinished(); }
    void readPluginList();
    void startOpeningDevice();
    void finishOpeningDevice (const juce::XmlElement* savedDevice);
    void bootFinished();
    juce::File recoveryFolder();
    void saveSession (std::function<void (bool saved)> done = {});
    void saveSessionAs (std::function<void (bool saved)> done = {});
    void saveSessionCopyWithAudio();
    void chooseFilesForSession();
    void addFilesToSession (const juce::StringArray& files, int track, juce::int64 frame);
    void chooseMixDownFile();
    void askForMixDownFormat (const juce::File&);
    void mixDown (const juce::File&, int bitDepth, juce::Range<juce::int64>);
    void bounceTrack();
    void freezeOrUnfreezeTrack();
    void duplicateTrack();
    void chooseTrackTemplateToSave();
    void chooseTrackTemplateToAdd();
    void reverseSelectedClip();
    void askToStretchSelectedClip();
    void askToShiftSelectedClip();
    void processSelectedClip (const juce::String& action, const juce::String& suffix,
                              std::function<AudioDocument::Snapshot (const SessionClip&, const std::function<bool (float)>&)> process);
    void chooseStemsFolder (bool folders);
    void exportStems (const juce::File& folder, juce::Range<juce::int64>, bool folders);

    // Transport and windows
    void togglePlay();
    void toggleRecord();
    void startRecording();
    void stopRecording();
    void startSessionRecording();
    void finishSessionRecording (const std::map<Recorder*, Recorder::Take>&);
    juce::File recordingsFolder() const;
    void showAudioSettings();
    /** Asks for the session's tempo and beats in a bar from the start, or with atCursor, for a
        tempo change at the bar the cursor is in (offering to remove one there). */
    void showTempoDialog (bool atCursor);
    void showRecordingOptions();
    void showKeyShortcuts();

    /** Asks for a folder, then mixes each region of the session to a file of its own there. */
    void chooseRegionsFolder();
    void exportRegions (const juce::File& folder);

    /** Writes the markers of the session (in multitrack mode) or the open file as CSV. */
    void chooseMarkersCsv();
    void chooseMarkersToImport();
    void importMarkers (const juce::String& csv, const juce::String& fileName);
    void chooseClipsCsv();
    void showPluginList();
    void showEditor (int slotIndex);
    void showEditor (juce::AudioProcessor&);
    void closeEditorFor (juce::AudioProcessor*);
    void saveRack();
    void loadRack();
    void refreshOutputChoices();
    void refreshInputChoices();
    void refreshTitle();
    void saveSettings();

    juce::ApplicationProperties appProperties;
    std::unique_ptr<WindowPlaces> places;   // where each window was left
    juce::uint32 recoveredVersion = 0;         // the session version last copied for recovery
    juce::uint32 lastRecoveryCheck = 0;        // when, in milliseconds since startup
    bool recoveryCopyKept = false;
    juce::TooltipWindow tooltips { this, 700 };   // without one, no control's tooltip ever shows
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    AudioClip clipboard;

    juce::TextButton multitrackButton { "Multitrack" };
    juce::TextButton openButton { "Open..." }, pluginsButton { "Plug-ins..." }, audioButton { "Audio Settings..." };
    std::unique_ptr<TransportBar::Bar> transportBar;   // made once the commands are registered
    TransportBar::SelectionPanel selectionPanel;
    void stopTransport();
    juce::Label outputLabel { {}, "Output" };
    juce::ComboBox outputPair;
    juce::Label inputLabel { {}, "Input" };
    juce::ComboBox inputChoice;
    LevelMeter inputMeter, outputMeter;
    std::pair<float, float> outputPeaks;   // read for both channels at once, handed out one at a time
    juce::Label fileLabel, deviceLabel, selectionLabel;   // selectionLabel: the selected clip, in the multitrack
    TapTempo tapper;
    juce::RecentlyOpenedFilesList recentFiles;
    FileChecks fileChecks;   // whether the recent files are there, found out off the message thread
    bool isThere (const juce::File& f)            { return fileChecks.check (f) == FileChecks::Known::exists; }
    static constexpr int recentBaseId = 0x7000;   // menu ids for File > Open Recent, clear of the command ids
    static constexpr int fadeShapeBaseId = 0x7100;   // menu ids for Effects > Fade Shape
    void noteRecent (const juce::File&);
    LoadReadout loadReadout { [this] { return LoadReadout::Reading { engine.deviceManager.getCpuUsage(), engine.deviceManager.getXRunCount() }; } };

    WaveformView waveform { engine };
    MultitrackView multitrack { engine };
    OpenFiles openFiles { engine };
    FileTabs fileTabs { openFiles };

    /** Shows another open file in the editor, where it was left. */
    void switchToFile (int index);

    /** Closes an open file, asking about its changes first. */
    void closeFile (int index);

    /** Asks about each open file with unsaved changes in turn (from the index given), then proceeds. */
    void confirmAllFiles (std::function<void()> proceed, int from = 0);
    SessionOverview overview { engine, multitrack };
    bool multitrackMode = false;
    FadeShape fadeShape = FadeShape::sCurve;   // the editor's Fade In and Fade Out
    bool normalizeChannelsApart = false;   // the Normalize dialog's last choice
    float silenceThresholdDb = -60.0f;   // Trim Silence's last level, also used by Select Audible Span and Find Next Silence
    double silenceMinSeconds = 0.5;      // Find Next Silence's last time
    RackView rackView { engine };

    std::unique_ptr<AuditionPreview> openPreview;   // before the chooser showing it, which outlives its callback
    std::unique_ptr<juce::FileChooser> chooser;
    AuditionPreview* makeOpenPreview();
    juce::OwnedArray<PluginEditorWindow> editorWindows;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<KeyShortcutsWindow> keyShortcutsWindow;
    std::unique_ptr<EqWindow> eqWindow;
    std::unique_ptr<RackRenderJob> renderJob;
    std::unique_ptr<BackgroundJob> backgroundJob;
    std::optional<NoiseReduction::Print> noisePrint;
    juce::File deadMansPedalFile;

    // The take in progress: where it goes in the document ([punchIn, punchOut), or open-ended when
    // punchOut < 0) and the round-trip latency measured when it started.
    juce::int64 punchIn = 0, punchOut = -1;
    int recordLatency = 0;

    // Takes going onto session tracks (the armed ones, or a new one) rather than into the document.
    bool recordingSession = false, recordChangedRate = false;
    bool loopRecording = false, recordingLoop = false;   // the option, and a take under way that loops
    bool loopPlayback = false;                 // the option: a selection or time range plays over and over
    juce::Range<juce::int64> playingLoop;      // the loop playing now (empty: none)

    /** The selection (file) or time range (session) that plays when Play is pressed. */
    juce::Range<juce::int64> playRange() const;

    /** Loops the player over the play range while looping is on and something plays; else none. */
    void updatePlaybackLoop();

    /** Previewing an effect: the selection (or the whole file) plays over and over through it,
        before the rack, until stopPreview. A preview from another owner takes over, and the old
        one's ended callback is called (so it can untick its Preview button); stopPreview calls
        it too. An owner already previewing just has its effect swapped. Files only. */
    enum class PreviewOwner { none, parametricEq, compressor, limiter, chorus, flanger, phaser, reverb, convolution, graphicEq, multiband, deEsser };
    void startPreview (PreviewOwner, std::unique_ptr<juce::AudioPluginInstance>, std::function<void()> ended);
    void stopPreview();
    juce::AudioPluginInstance* previewEffectOf (PreviewOwner) const;
    PreviewOwner previewOwner = PreviewOwner::none;
    std::function<void()> previewEnded;

    /** An effect's dialog: a text field (or a menu, for a field with choices, its value the
        choice's index) for each setting, the effect's presets (under presetKey, the fields' IDs
        naming the values), and, when `make` is given, a Preview switch that plays the selection
        through the live effect (made by `make` for the values, set by `setLive` as the fields change,
        the last valid settings while one is being typed; setLive returns false when it needs a new one).
        OK applies the effect when the values are valid (else shows invalidText); the preview ends
        whenever the dialog closes. */
    struct EffectField
    {
        juce::String id;      // its own copies, as the dialog's callbacks keep the fields after the call
        juce::String label;
        double value;
        int decimals;
        juce::StringArray choices {};
    };
    using EffectValues = EffectPresets::Values;
    void showEffectDialog (const juce::String& title, const juce::String& message, const std::vector<EffectField>&,
                           const juce::String& presetKey, PreviewOwner,
                           std::function<bool (const EffectValues&)> valid, const juce::String& invalidText,
                           std::function<std::unique_ptr<juce::AudioPluginInstance> (const EffectValues&)> make,
                           std::function<bool (juce::AudioPluginInstance&, const EffectValues&)> setLive,
                           std::function<void (const EffectValues&)> apply);
    bool inputMonitoring = false;                        // the armed tracks play their inputs
    bool countIn = false;                                // record after a bar of clicks
    SessionRecording::Rolls rolls;                       // pre-roll and post-roll, kept in the settings
    std::vector<std::pair<juce::uint32, Recorder*>> recordTracks;   // the tracks recording (0: a new one), and from what
    juce::String lastInputError;

    // The dock: the editor with the panels around it, laid out by the current workspace.
    juce::Component* getPanel (const juce::String&) override;
    void panelHidden (const juce::String&) override;
    juce::String getTitle (const juce::String&) override;
    bool isMovable (const juce::String&) override;
    void layoutChanged() override;
    std::pair<juce::String, Dock::Side> defaultPlace (const juce::String&) override;
    double defaultShare (const juce::String&) override;
    void windowCreated (juce::DocumentWindow&) override;

    void selectWorkspace (const juce::String& name);
    void askToSaveWorkspace();
    void askToDeleteWorkspace();
    void resetWorkspace();
    static constexpr int workspaceBaseId = 0x7200;   // menu ids for Window > Workspace
    static constexpr int themeBaseId = 0x7300, accentBaseId = 0x7380, waveformColourBaseId = 0x73c0;   // menu ids for View > Theme

    /** Puts a theme, accent and waveform colour into use and keeps them for next time. */
    void setTheme (const juce::String& theme, const juce::String& accentName, const juce::String& waveformName);

    int lastRepeatable = 0;   // the last command from the Effects menu, for Repeat
    std::unique_ptr<EditorBar> editorBar;   // made once the commands are registered
    std::unique_ptr<EditorPanel> editorPanel;
    juce::File projectFile;   // the project last opened or saved (Save Project writes it)
    std::map<juce::String, std::unique_ptr<juce::Component>> madePanels;   // the panels made when shown
    std::unique_ptr<Dock::Workspaces> workspaces;
    std::unique_ptr<Dock::Area> dock;   // last, so it lets go of the panels before they go

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
