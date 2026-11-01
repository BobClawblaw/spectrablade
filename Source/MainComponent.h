#pragma once

#include "AudioEdits.h"
#include "AudioEngine.h"
#include "ClickRepair.h"
#include "LevelMeter.h"
#include "LoadReadout.h"
#include "TapTempo.h"
#include "TimeEntry.h"
#include "RecentFiles.h"
#include "Loudness.h"
#include "MultitrackView.h"
#include "NoiseReduction.h"
#include "RackView.h"
#include "SessionOverview.h"
#include "SessionRecording.h"
#include "WaveformView.h"

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    /** Asks about unsaved changes to the file, then calls proceed unless the user cancels. */
    void confirmDiscardChanges (std::function<void()> proceed);

    /** Asks about unsaved changes to the file and then the session, then calls proceed. */
    void confirmQuit (std::function<void()> proceed);

    std::function<void (const juce::String& title)> onTitleChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    ApplicationCommandTarget* getNextCommandTarget() override  { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int menuItemId, int) override;

private:
    class PluginEditorWindow;
    class MixerWindow;
    class ClipPropertiesWindow;
    class ClipListWindow;
    class MarkerListWindow;
    class KeyShortcutsWindow;
    class UndoHistoryWindow;
    class NotesWindow;
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
    void save();
    void saveAs (std::function<void (bool saved)> done = {});
    void saveSelectionAs();
    void chooseFileRegionsFolder();
    void saveFileRegions (const juce::File& folder);

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
    void showFadeEdges();
    double edgeFadeMs = 10.0;
    void addMarkersAtClipping();
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
    void matchLoudness (double target, double ceiling);

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
    void toggleMixer();
    void toggleClipProperties();
    void toggleClipList();
    void toggleMarkerList();
    void showKeyShortcuts();

    /** Asks for a folder, then mixes each region of the session to a file of its own there. */
    void chooseRegionsFolder();
    void exportRegions (const juce::File& folder);

    /** Writes the markers of the session (in multitrack mode) or the open file as CSV. */
    void chooseMarkersCsv();
    void chooseMarkersToImport();
    void importMarkers (const juce::String& csv, const juce::String& fileName);
    void chooseClipsCsv();
    void toggleUndoHistory();
    void toggleNotes();
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
    juce::uint32 recoveredVersion = 0;         // the session version last copied for recovery
    juce::uint32 lastRecoveryCheck = 0;        // when, in milliseconds since startup
    bool recoveryCopyKept = false;
    juce::TooltipWindow tooltips { this, 700 };   // without one, no control's tooltip ever shows
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    AudioClip clipboard;

    juce::TextButton multitrackButton { "Multitrack" };
    juce::TextButton openButton { "Open..." }, playButton { "Play" }, stopButton { "Stop" }, recordButton { "Record" },
                     pluginsButton { "Plug-ins..." }, audioButton { "Audio Settings..." },
                     saveRackButton { "Save Rack..." }, loadRackButton { "Load Rack..." };
    juce::Label outputLabel { {}, "Output" };
    juce::ComboBox outputPair;
    juce::Label inputLabel { {}, "Input" };
    juce::ComboBox inputChoice;
    LevelMeter inputMeter;
    juce::Label fileLabel, deviceLabel, timeLabel, selectionLabel;
    TapTempo tapper;
    juce::RecentlyOpenedFilesList recentFiles;
    static constexpr int recentBaseId = 0x7000;   // menu ids for File > Open Recent, clear of the command ids
    static constexpr int fadeShapeBaseId = 0x7100;   // menu ids for Effects > Fade Shape
    void noteRecent (const juce::File&);
    LoadReadout loadReadout { [this] { return LoadReadout::Reading { engine.deviceManager.getCpuUsage(), engine.deviceManager.getXRunCount() }; } };

    WaveformView waveform { engine };
    MultitrackView multitrack { engine };
    SessionOverview overview { engine, multitrack };
    bool multitrackMode = false;
    FadeShape fadeShape = FadeShape::sCurve;   // the editor's Fade In and Fade Out
    bool normalizeChannelsApart = false;   // the Normalize dialog's last choice
    float silenceThresholdDb = -60.0f;   // Trim Silence's last level, also used by Select Audible Span and Find Next Silence
    double silenceMinSeconds = 0.5;      // Find Next Silence's last time
    RackView rackView { engine };

    std::unique_ptr<juce::FileChooser> chooser;
    juce::OwnedArray<PluginEditorWindow> editorWindows;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<MixerWindow> mixerWindow;
    std::unique_ptr<ClipPropertiesWindow> clipPropertiesWindow;
    std::unique_ptr<ClipListWindow> clipListWindow;
    std::unique_ptr<MarkerListWindow> markerListWindow;
    std::unique_ptr<KeyShortcutsWindow> keyShortcutsWindow;
    std::unique_ptr<UndoHistoryWindow> undoHistoryWindow;
    std::unique_ptr<NotesWindow> notesWindow;
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
    bool inputMonitoring = false;                        // the armed tracks play their inputs
    bool countIn = false;                                // record after a bar of clicks
    SessionRecording::Rolls rolls;                       // pre-roll and post-roll, kept in the settings
    std::vector<std::pair<juce::uint32, Recorder*>> recordTracks;   // the tracks recording (0: a new one), and from what
    juce::String lastInputError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
