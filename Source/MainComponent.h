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
#include "DynamicEq.h"
#include "Exciter.h"
#include "Maximizer.h"
#include "TransientShaper.h"
#include "MatchEq.h"
#include "TonalBalance.h"
#include "Statistics.h"
#include "TimeFormat.h"
#include "Dither.h"
#include "DialogueIsolation.h"
#include "AdaptiveDenoise.h"
#include "Plosive.h"
#include "Breath.h"
#include "Spill.h"
#include "WowFlutter.h"
#include "SpectralRecovery.h"
#include "Rebalance.h"
#include "Ambience.h"
#include "DialogueContour.h"
#include "Azimuth.h"
#include "CodecPreview.h"
#include "Modulation.h"
#include "Reverb.h"
#include "Convolver.h"
#include "EffectPresets.h"
#include "EffectDialog.h"
#include "EffectPreview.h"
#include "RackView.h"
#include "SessionOverview.h"
#include "SessionRecording.h"
#include "WaveformView.h"
#include "Dock.h"
#include "Batch.h"
#include "EditorBar.h"
#include "PitchEditor.h"
#include "Delivery.h"
#include "SoundLibraryView.h"

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
    /** Asks for a silence level and shortest time (kept for next time), then calls found with the time in frames.
        With `rendered`, the dialog previews its request for the level (dB) and time (frames). */
    void askForSilence (const juce::String& title, std::function<void (juce::int64)> found,
                        std::function<std::optional<EffectPreview::Request> (float, juce::int64)> rendered = nullptr);
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
    void showGenerateTones();
    void exportLossy();
    void askLossySettings (const juce::File&, juce::Range<juce::int64> selection);
    void exportWithFfmpeg();
    void batchProcess();
    void askForBatchSettings (const juce::Array<juce::File>&);
    void runBatch (const juce::Array<juce::File>&, const Batch::Settings&);
    /** Shows the Sound Library window (making it the first time), or brings it to the front. */
    void showSoundLibrary();
    std::unique_ptr<SoundLibraryWindow> soundLibrary;

    void showBatchReport (const std::vector<Batch::Outcome>&, const juce::File& deliveryReport = {});

    // Recorded command sequences
    Sequences::Result runStep (const Sequences::Step&);   // a command's edit, through the step that plays it back

    /** A step that takes a while (Stretch, Shift Pitch) on the selection or the whole file, in
        the background with a progress window, applied as one undo step unless the file changed
        meanwhile; a selection is kept over the processed range. */
    void runStepInBackground (const Sequences::Step&, const juce::String& busy);
    void askToStretch();
    void askToShiftPitch();
    void askToExtremeStretch();
    void askToReduceReverb();
    void askForCentreChannel();
    void askForPhaseCorrection();
    void askForPitchCorrection();
    void moveNotesInBackground (const PitchEditor::Source&, const PitchCorrection::Track&, const std::vector<PitchCorrection::Note>&);

    /** The Pitch Editor on a session clip: the notes moved in the part of its audio it plays, saved
       as a file of its own and played by the clip from then on (as Shift Clip Pitch does), as one
       step to undo. */
    void moveClipNotesInBackground (const PitchEditor::Source&, const PitchCorrection::Track&, const std::vector<PitchCorrection::Note>&);

    /** Something that changes when the clip's audio, offset or length do (0 when it's gone). */
    juce::uint32 clipPitchVersion (juce::uint32 clipId) const;
    juce::uint32 pitchClipId = 0;   // the clip the Pitch Editor last analysed
    void askToLevelSpeech();
    void askForVideoStart();
    void detachVideo();
    void saveMixIntoVideo();
    static juce::StringArray stretchMethods();   // the Method menu of Stretch and Shift Pitch
    double stretchPercent = 100.0, pitchSemitones = 0.0, stretchMethod = 0.0;   // as last asked (method: 0 Signalsmith, 1 SpectraBlade)
    double extremeFactor = 8.0, extremeWindow = 0.25;                            // Extreme Stretch as last asked
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

    /** A preview of generated audio as Generate would put it in (at the cursor, or the start of
        the selection): make runs on the preview's thread; the first EffectPreview::longestSeconds play. */
    std::optional<EffectPreview::Request> generatedPreview (const juce::String& kind, const EffectPresets::Values&,
                                                            std::function<juce::AudioBuffer<float>()> make);
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
    Multiband::Width multibandWidthSettings;
    void showMultibandWidth();
    Exciter::Settings exciterSettings;
    void showExciter();
    Maximizer::Settings maximizerSettings;
    void showMaximizer();
    TransientShaper::Settings transientShaperSettings;
    void showTransientShaper();
    std::optional<MatchEq::Profile> matchReference;
    int matchReferenceCaptures = 0;   // counts captures, so a preview knows its reference
    MatchEq::Settings matchSettings;
    Dither::Settings ditherSettings;
    void showDither();
    DialogueIsolation::Settings dialogueSettings;
    AdaptiveDenoise::Settings adaptiveSettings;
    Plosive::Settings plosiveSettings;
    Breath::Settings breathSettings;
    Spill::Settings spillSettings;
    WowFlutter::Settings wowSettings;
    SpectralRecovery::Settings recoverySettings;
    Rebalance::Settings rebalanceSettings;
    std::optional<Ambience::Print> ambiencePrint;
    int ambienceCaptures = 0;
    Ambience::Settings ambienceSettings;
    DialogueContour::Settings contourSettings;
    Azimuth::Settings azimuthSettings;
    LossyFiles::Settings codecPreviewSettings;
    void showCodecPreview();
    void applyCodecPreview (LossyFiles::Settings);
    void showAzimuth();
    void correctAzimuth (Azimuth::Settings);
    void showDialogueContour();
    void reshapeContour (DialogueContour::Settings);
    void captureAmbience();
    void showAmbienceMatch();
    void showRebalance();
    void rebalance (Rebalance::Settings);
    void showSpectralRecovery();
    void recoverSpectrum (SpectralRecovery::Settings);
    void showWowFlutter();
    void correctWowFlutter (WowFlutter::Settings);
    void showSpill();
    void reduceSpill (Spill::Settings);
    void showBreathControl();
    void showMouthDeClick (bool crackle);
    void showDePlosive();
    void showAdaptiveDenoise();
    void reduceNoiseAdaptively (AdaptiveDenoise::Settings);
    void showDialogueIsolation();
    void isolateDialogue (DialogueIsolation::Settings);
    void captureMatchReference();
    void captureMatchReferenceFromFile();
    void showMatchEq();
    void applyMatchEq (MatchEq::Settings);
    DynamicEq::Settings dynamicEqSettings = DynamicEq::defaultSettings();
    void showMultiband();
    void showDynamicEq();
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

    void applyParametricEq (const std::vector<Equalizer::Band>&, bool linearPhase);
    void previewParametricEq (const ParametricEq*);   // nullptr: stop
    ParametricEq::Settings eqSettings = ParametricEq::defaultSettings();
    bool eqLinearPhase = false;
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
    /** One level in the effect dialog, from min to max dB, with the presets under presetKey (empty: none)
        and, with `rendered`, a preview of its request for the level. */
    void askForDecibels (const juce::String& title, const juce::String& label, float initial, double min, double max,
                         const juce::String& presetKey, std::function<void (float)> onOk,
                         std::function<std::optional<EffectPreview::Request> (float)> rendered = nullptr);
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

    /** Meet Delivery Spec: the selection or file brought to a chosen spec (Delivery::deliver) as
        one step to undo, then its report, which can be saved as a text file. */
    void askForDeliverySpec();
    void meetDeliverySpec (const Delivery::Spec&);
    void showDeliveryReport (const juce::String& report, const juce::File& suggested = {});
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
    /** Adds the files as clips at the frame, the first on the track (or a new one) and the rest on
        new tracks, as one undo step; returns where the first one ends, or -1 when none could be read. */
    juce::int64 addFilesToSession (const juce::StringArray& files, int track, juce::int64 frame);
    void chooseMixDownFile();
    void askForMixDownFormat (const juce::File&);
    void mixDown (const juce::File&, int bitDepth, juce::Range<juce::int64>, std::optional<Delivery::Spec> spec = std::nullopt);
    void bounceTrack();
    void freezeOrUnfreezeTrack();
    void duplicateTrack();
    void chooseTrackTemplateToSave();
    void chooseTrackTemplateToAdd();
    void reverseSelectedClip();
    void askToStretchSelectedClip();
    void askToShiftSelectedClip();
    void processSelectedClip (const juce::String& action, const juce::String& suffix,
                              std::function<AudioDocument::Snapshot (const SessionClip&, const std::function<bool (float)>&)> process,
                              bool keepEnd = false);
    void chooseStemsFolder (bool folders);
    void exportStems (const juce::File& folder, juce::Range<juce::int64>, bool folders, std::optional<Delivery::Spec> spec = std::nullopt);

    /** "As mixed", then each delivery spec's name: the choices for a mixdown's or stems' loudness. */
    juce::StringArray loudnessChoices();
    std::optional<Delivery::Spec> loudnessChoice (int index);

    /** The delivery specs offered: Delivery::specs(), then the custom spec set in Meet Delivery
        Spec (kept in the settings; -20 LUFS +/- 1 LU at -1 dBTP until one is set). */
    std::vector<Delivery::Spec> deliverySpecs();
    static juce::String describe (const Delivery::Spec&);

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

    juce::TextButton editorButton { "Editor" }, multitrackButton { "Multitrack" };   // a two-part switch, lit on the side in use
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

    /** Play with a box, lasso or brush selection in the spectrogram plays only what is inside it:
        the audio with everything else over the selection taken out (SpectralEdit::changeOutsideBand
        or changeOutsideShape), processed in the background, then played in place of the document
        (DocumentSource::setStandIn) until playback stops, the selection moves, the document changes
        or a preview starts; the transport shows "Processing..." in place of the time meanwhile.
        Files only. */
    void playInsideSpectral (juce::Range<juce::int64>);
    void stopPlayingInside();
    AudioDocument::Snapshot insideAudio;        // what plays in place of the document, while set
    juce::Range<juce::int64> insideRange;       // the selection it was made for
    juce::uint32 insideVersion = 0;             // the document's version it was made from
    bool insidePending = false;                 // being processed: plays once it is done

    /** Nudging the selection: its start or its end, later or earlier, by a pixel's worth of time at
        this zoom (a frame at the least), and on to the next zero crossing that way with Select at
        Zero Crossings on, which also lands the ends of a selection made with the mouse on the
        nearest crossing (waveform.snapSelectionFrame). */
    void nudgeSelection (bool start, bool later);
    juce::int64 zeroCrossingReach() const;      // 5 ms, in the file's frames
    bool selectZeroCrossings = false;           // the option, kept in the settings

    /** The selection (file) or time range (session) that plays when Play is pressed. */
    juce::Range<juce::int64> playRange() const;

    /** Loops the player over the play range while looping is on and something plays; else none. */
    void updatePlaybackLoop();

    /** Previewing an effect: the selection (or the whole file) plays over and over through it,
        before the rack, until stopPreview. A preview from another owner takes over, and the old
        one's ended callback is called (so it can untick its Preview button); stopPreview calls
        it too. An owner already previewing just has its effect swapped. Files only. */
    enum class PreviewOwner { none, parametricEq, compressor, limiter, chorus, flanger, phaser, reverb, convolution, graphicEq, multiband, deEsser, rendered, dynamicEq, multibandWidth, exciter, maximizer, transientShaper };
    void startPreview (PreviewOwner, std::unique_ptr<juce::AudioPluginInstance>, std::function<void()> ended);
    void stopPreview();
    juce::AudioPluginInstance* previewEffectOf (PreviewOwner) const;
    PreviewOwner previewOwner = PreviewOwner::none;
    std::function<void()> previewEnded;

    /** Previewing an effect that can't run live (PreviewOwner::rendered): the request is processed
        in the background on the selection (or the whole file; its first EffectPreview::longestSeconds
        unless the request needs it all), and the result plays over and over in place of the audio.
        A new request replaces the one playing once it is processed; until then the last one plays.
        status gets what it is doing ("Processing...") and anything that stops it. */
    void startRenderedPreview (const EffectPreview::Request&, std::function<void()> ended, std::function<void (const juce::String&)> status);
    EffectPreview effectPreview;
    AudioDocument::Snapshot renderedAudio;      // the preview's audio, once processed
    juce::Range<juce::int64> renderedLoop, renderedPart;   // where it plays processed, and the part as it was
    juce::String renderedKey;                   // the settings processed or being processed
    juce::uint32 renderedVersion = 0;           // the document's version it was processed from

    /** Bypass in a preview: the selection plays as it is, to compare, until put back. */
    void setPreviewBypassed (bool);
    bool previewBypassed = false;

    /** An effect's dialog (EffectDialog): a slider or menu for each setting (a menu's value is the
        choice's index), the effect's presets (under presetKey, the fields' IDs naming the values),
        and Preview and Bypass. The preview plays the selection through the live effect when `make`
        is given (made by `make` for the values, set by `setLive` as the fields change; setLive
        returns false when it needs a new one), else through `rendered`'s request for the values
        once they settle (none: no preview). Apply applies the effect when the values are valid
        (else shows invalidText); the preview ends whenever the dialog closes. */
    using EffectField = EffectDialog::Field;
    std::map<juce::String, juce::String> effectTexts;   // the text fields of the effect dialog being applied, while apply runs
    using EffectValues = EffectPresets::Values;
    /** True when an effect dialog opened now should start with its Preview on: the file is playing
        (not a session, not recording) and nothing else is previewing. */
    bool previewsFromStart();
    void showEffectDialog (const juce::String& title, const juce::String& message, const std::vector<EffectField>&,
                           const juce::String& presetKey, PreviewOwner,
                           std::function<bool (const EffectValues&)> valid, const juce::String& invalidText,
                           std::function<std::unique_ptr<juce::AudioPluginInstance> (const EffectValues&)> make,
                           std::function<bool (juce::AudioPluginInstance&, const EffectValues&)> setLive,
                           std::function<void (const EffectValues&)> apply,
                           std::function<std::optional<EffectPreview::Request> (const EffectValues&)> rendered = nullptr,
                           const juce::String& applyText = "Apply");

    /** A step's request for the document's rate (nothing when the step can't run with these settings). */
    std::optional<EffectPreview::Request> previewOf (const Sequences::Step&, bool wholeRange = false) const;

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
    void setTimeFormat (TimeFormat::Kind);
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
