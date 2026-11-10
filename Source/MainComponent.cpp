#include "MainComponent.h"
#include "Navigation.h"
#include "Restoration.h"
#include "CentreChannel.h"
#include "PhaseCorrection.h"
#include "PitchCorrection.h"
#include "PitchEditor.h"
#include "Dereverb.h"
#include "Leveller.h"
#include "Generator.h"
#include "LossyFiles.h"
#include "Ffmpeg.h"
#include "Theme.h"
#include "BuiltInEffects.h"
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
#include "ExtremeStretch.h"
#include "TrackTemplate.h"
#include "SessionNotes.h"
#include "UndoHistory.h"
#include "LevelsPanel.h"
#include "Analysis.h"
#include "Surround.h"
#include "Video.h"
#include "Project.h"

namespace
{
    constexpr const char* knownPluginsKey = "knownPlugins";
    constexpr const char* audioDeviceKey  = "audioDevice";
    constexpr const char* outputPairKey   = "outputPairFirstChannel";
    constexpr const char* lastFileKey     = "lastFile";
    constexpr const char* inputFirstKey   = "inputFirstChannel";
    constexpr const char* inputCountKey   = "inputNumChannels";
    constexpr const char* displayKey      = "display";
    constexpr const char* fadeShapeKey    = "editorFadeShape";
    constexpr const char* snappingKey     = "snapping";
    constexpr const char* crossfadingKey  = "crossfading";
    constexpr const char* zeroCrossingsKey = "zeroCrossings";
    constexpr const char* selectZeroCrossingsKey = "selectZeroCrossings";
    constexpr const char* prerollKey      = "preroll";
    constexpr const char* postrollKey     = "postroll";
    constexpr const char* laneHeightKey   = "laneHeight";
    constexpr const char* loopPlaybackKey = "loopPlayback";
    constexpr const char* fileTabsKey = "fileTabs";
    constexpr const char* projectHistoryKey = "projectHistory";
    constexpr const char* rackShownKey    = "rackShown";
    constexpr const char* openPreviewKey  = "previewInOpenPanel";
    constexpr const char* waveShareKey    = "waveformShare";
    constexpr const char* workspacesKey   = "workspaces";
    constexpr const char* themeKey        = "theme";
    constexpr const char* accentKey       = "accent";
    constexpr const char* waveformColourKey = "waveformColour";
    constexpr const char* timeFormatKey   = "timeFormat";

    // The dock's panels.
    namespace Panels
    {
        const juce::String editor { "editor" }, rack { "rack" }, selection { "selection" }, mixer { "mixer" },
                           clipProperties { "clipProperties" }, clipList { "clipList" }, markers { "markers" },
                           history { "history" }, notes { "notes" }, files { "files" }, levels { "levels" },
                                spectrum { "spectrum" }, phase { "phase" }, video { "video" }, pitch { "pitch" }, tonalBalance { "tonalBalance" }, statistics { "statistics" };

        juce::StringArray all()
        {
            return { editor, rack, selection, mixer, clipProperties, clipList, markers, history, notes, files, levels, spectrum, phase, video, pitch, tonalBalance, statistics };
        }
    }

    /** The workspaces SpectraBlade comes with: Classic, the editor with the effects rack and the
        selection beside it; Editing, with the open files on the left, the undo history among the rack's
        tabs and the levels along the bottom; Multitrack, with the mixer
        under the editor and the clip panels beside it. */
    std::vector<std::pair<juce::String, Dock::Layout>> builtInWorkspaces()
    {
        using Dock::Side;
        auto classic = Dock::Layout::single (Panels::editor);
        classic.dock (Panels::rack, Panels::editor, Side::right, 0.28);
        classic.dock (Panels::selection, Panels::rack, Side::bottom, 0.2);

        auto editing = classic;
        editing.dock (Panels::files, Panels::editor, Side::left, 0.22);
        editing.dock (Panels::history, Panels::rack, Side::centre);
        editing.bringToFront (Panels::rack);
        editing.dockAtEdge (Panels::levels, Side::bottom, 0.12);

        auto multitrack = Dock::Layout::single (Panels::editor);
        multitrack.dock (Panels::rack, Panels::editor, Side::right, 0.26);
        multitrack.dock (Panels::markers, Panels::rack, Side::centre);
        multitrack.dock (Panels::clipList, Panels::rack, Side::centre);
        multitrack.bringToFront (Panels::rack);
        multitrack.dock (Panels::clipProperties, Panels::rack, Side::top, 0.4);
        multitrack.dockAtEdge (Panels::mixer, Side::bottom, 0.3);

        auto analysis = Dock::Layout::single (Panels::editor);
        analysis.dock (Panels::rack, Panels::editor, Side::right, 0.26);
        analysis.dockAtEdge (Panels::spectrum, Side::bottom, 0.34);
        analysis.dock (Panels::phase, Panels::spectrum, Side::right, 0.22);
        analysis.dockAtEdge (Panels::levels, Side::bottom, 0.1);

        return { { "Classic", classic }, { "Editing", editing }, { "Multitrack", multitrack }, { "Analysis", analysis } };
    }

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
        cmdClearRange, cmdDeleteTime, cmdInsertTime, cmdCutTime, cmdCopyTime, cmdPasteTime, cmdTempoChange, cmdMarkerList, cmdExportMarkers, cmdExportRegions, cmdQuantize, cmdLoopPlayback, cmdKeyShortcuts, cmdFreezeTrack, cmdGroupClips, cmdUngroupClips, cmdClipGainLines, cmdAddFolder, cmdExportFolderStems, cmdSessionNotes, cmdSelectBetweenMarkers, cmdSelectClipsInRange, cmdTapTempo, cmdGoToTime, cmdTrimStartToCursor, cmdTrimEndToCursor, cmdNextClip, cmdPreviousClip, cmdExportClips, cmdDuplicateTime, cmdRegionFromClips, cmdUnsoloAll, cmdUnmuteAll, cmdTrackAbove, cmdTrackBelow, cmdRemoveDc, cmdSwapChannels, cmdInsertSilence, cmdTrimSilence, cmdMixToMono, cmdMakeStereo, cmdSelectAudible, cmdBalance, cmdSetSelection, cmdPasteMix, cmdSaveSelection, cmdSaveRegions, cmdSelectRegion, cmdMarkersAtIntervals, cmdRegionsFromMarkers, cmdRenameMarkers, cmdImportMarkers, cmdFindClipping, cmdFindSilence, cmdMarkersAtSilences, cmdShortenPauses, cmdFitRegions, cmdRegionsOverClips, cmdSelectClipsInRegion, cmdLoopRegion, cmdNextRegion, cmdPreviousRegion, cmdCopySelectionTime, cmdStepSelectionLater, cmdStepSelectionEarlier, cmdDoubleSelection, cmdHalveSelection, cmdSelectionToZero, cmdCrossfadeLoop, cmdRepeatSelection, cmdPasteFill, cmdInvertLeft, cmdInvertRight, cmdSilenceLeft, cmdSilenceRight, cmdLeftToRight, cmdRightToLeft, cmdNoiseGate, cmdMarkClipping, cmdRepair, cmdFadeEdges, cmdCropMargins, cmdSavePieces, cmdDeleteMarkersInSelection, cmdMoveMarkersInSelection, cmdMarkersToZero, cmdGainRamp, cmdPanRamp, cmdStereoWidth, cmdRegionsOverSounds, cmdNextSound, cmdTremolo, cmdEcho, cmdSilenceOutside, cmdNormalizeRegions, cmdFadeRegions, cmdPreviousSound, cmdEqBand, cmdParametricEq, cmdCompressor, cmdLimiter, cmdChorus, cmdFlanger, cmdPhaser, cmdReverb, cmdConvolutionReverb, cmdGraphicEq10, cmdGraphicEq31, cmdAbout, cmdReverseRegions, cmdStop, cmdShowRack, cmdMultiband, cmdDeEsser,
        cmdShowSelectionPanel, cmdSaveWorkspace, cmdResetWorkspace, cmdDeleteWorkspace,
        cmdShowFiles, cmdCloseFile, cmdNextFile, cmdPreviousFile, cmdShowLevels, cmdShowSpectrum, cmdShowPhase,
        cmdMaximizeEditor, cmdResetVerticalZoom, cmdPreviousEdge, cmdNextEdge, cmdRepeatLast, cmdRemoveHum, cmdDeclip, cmdGenerateTone, cmdGenerateNoise,
        cmdBoxTool, cmdLassoTool, cmdBrushTool, cmdBrushLarger, cmdBrushSmaller, cmdSpectralHeal, cmdExportLossy, cmdShowFileTabs, cmdExportFfmpeg, cmdFileInfo, cmdBatchProcess, cmdRecordSequence, cmdPlaySequence,
        cmdSurroundFoldDown, cmdSurroundOutputs, cmdSaveChannels, cmdNewFromChannels,
        cmdShowVideo, cmdAttachVideo, cmdSaveIntoVideo, cmdOpenProject, cmdSaveProject, cmdSaveProjectAs, cmdKeepProjectHistory,
        cmdSurroundMaster, cmdStretch, cmdPitch, cmdDereverb, cmdLevelSpeech, cmdVideoStart, cmdDetachVideo, cmdCentreChannel, cmdPhaseCorrection, cmdGenerateTones, cmdPitchCorrection, cmdShowPitchEditor,
        cmdSevenOneMaster, cmdMeetDeliverySpec, cmdShowSoundLibrary, cmdDynamicEq, cmdMultibandWidth, cmdExciter, cmdMaximizer, cmdTransientShaper, cmdCaptureMatchReference, cmdCaptureMatchReferenceFromFile, cmdMatchEq, cmdShowTonalBalance, cmdDither, cmdDialogueIsolation, cmdAdaptiveDenoise, cmdMouthDeClick, cmdDeCrackle, cmdDePlosive, cmdBreathControl, cmdSpill, cmdWowFlutter, cmdSpectralRecovery, cmdRebalance, cmdCaptureAmbience, cmdAmbienceMatch, cmdDialogueContour, cmdAzimuth, cmdCodecPreview,
        cmdNudgeStartEarlier, cmdNudgeStartLater, cmdNudgeEndEarlier, cmdNudgeEndLater, cmdSelectZeroCrossings, cmdBrushLighter, cmdBrushHeavier, cmdShowStatistics,
        cmdTimeMinutesSeconds, cmdTimeDecimalSeconds, cmdTimeSamples, cmdTimeSmpte24, cmdTimeSmpte25, cmdTimeSmpte2997Drop, cmdTimeSmpte2997, cmdTimeSmpte30,   // one a TimeFormat::Kind, in its order
        cmdExtremeStretch, cmdNewFile,
        cmdCount   // one past the last command: every id before it is registered
    };

    /** Commands that work on the open file only, off while the session is showing. */
    bool isEditorOnly (int id)
    {
        return (id >= cmdCut && id <= cmdApplyRack) || id == cmdZoomSelection
            || (id >= cmdAddMarker && id <= cmdConvertRate) || (id >= cmdSpectralAttenuate && id <= cmdClickRepair)
            || (id >= cmdRemoveDc && id <= cmdSaveRegions) || (id >= cmdFindClipping && id <= cmdShortenPauses)
            || (id >= cmdStepSelectionLater && id <= cmdSavePieces) || id == cmdMarkersToZero || (id >= cmdGainRamp && id <= cmdGraphicEq31) || id == cmdReverseRegions || id == cmdMultiband || id == cmdDeEsser || id == cmdDynamicEq || id == cmdMultibandWidth || id == cmdExciter || id == cmdMaximizer || id == cmdTransientShaper || (id >= cmdCaptureMatchReference && id <= cmdMatchEq) || id == cmdDither || id == cmdDialogueIsolation || id == cmdAdaptiveDenoise || (id >= cmdMouthDeClick && id <= cmdCodecPreview) || (id >= cmdNudgeStartEarlier && id <= cmdBrushHeavier)
            || id == cmdRemoveHum || id == cmdDeclip || id == cmdGenerateTone || id == cmdGenerateNoise
            || (id >= cmdBoxTool && id <= cmdExportLossy) || id == cmdExportFfmpeg || id == cmdFileInfo
            || id == cmdRecordSequence || id == cmdPlaySequence || id == cmdSaveChannels
            || id == cmdStretch || id == cmdExtremeStretch || id == cmdPitch || id == cmdDereverb || id == cmdLevelSpeech || id == cmdCentreChannel || id == cmdPhaseCorrection || id == cmdGenerateTones || id == cmdPitchCorrection;
    }

    juce::String formatTime (juce::int64 frames, double sampleRate)
    {
        return TimeFormat::format (frames, juce::jmax (1.0, sampleRate));
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

    /** What Adaptive Noise Reduction says when sound stood clear of the tracked noise in under a
        tenth of the frames: the whole came down. Empty otherwise. */
    juce::String adaptiveDenoiseNote (const AdaptiveDenoise::Report& r)
    {
        if (r.signalShare >= 0.1f)
            return {};

        return "Sound stood 6 dB clear of the tracked noise in " + juce::String (juce::roundToInt (100.0f * r.signalShare)) + " % of the frames, so the typical stretch came down by "
               + juce::String (-r.loweredDb, 1) + " dB: either the selection is noise alone, or a sound that never pauses (a room full of voices, say) was itself taken for the noise.";
    }

    /** What Dialogue Isolation says when speech was found in under a tenth of the frames. */
    juce::String dialogueIsolationNote (const DialogueIsolation::Report& r)
    {
        if (r.speechShare >= 0.1f)
            return {};

        return "Speech was found in " + juce::String (juce::roundToInt (100.0f * r.speechShare)) + " % of the frames, so the typical stretch came down by "
               + juce::String (-r.loweredDb, 1) + " dB: speech is found by its pitch, and a crowd of voices, with no one pitch to follow, is not kept.";
    }
}

//==============================================================================
/** Every command and its keys, to change; the changes are saved as they are made. */
/** Keys as Adobe Audition's defaults have them, for people coming from it: the keys Adobe's own list
    of default shortcuts gives (helpx.adobe.com, "Default keyboard shortcuts", updated June 2026),
    and those its partial list leaves out where two published cheat sheets agree. Only commands
    SpectraBlade has; the rest are named in `about`. */
static KeyShortcuts::KeySet auditionStyleKeys()
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto alt = juce::ModifierKeys::altModifier;

    KeyShortcuts::KeySet set;
    set.name = "Audition-Style Keys";
    set.about = "The keys Adobe Audition's defaults give these commands, from Adobe's published list (a partial one) and, where it "
                "says nothing, keys two cheat sheets agree on. 1 switches between the file and the multitrack session; = and - zoom; "
                "Option-Cmd-Left and Right go to markers; Option-comma and Option-period nudge; Shift-T converts the sample rate. "
                "Space, Home, End, M, Shift-P, Cmd-K, Cmd-T and S already match, as do Cmd-Left and Right (going to clip and selection "
                "edges) and Shift-R (repeating the last effect, its dialog opening again).\n\n"
                "Not carried over: Cmd-R stays Apply Effects Rack; vertical zoom keeps SpectraBlade's keys (Option-Cmd-= and minus), "
                "Audition's not having been checked; and, for want of a matching command, editing one channel (Up, Down), the spectral "
                "display's scale and resolution, nudging clips between tracks, and Return to Start on Stop (Shift-X).";
    set.keys = { { cmdPlayPause, juce::KeyPress (juce::KeyPress::spaceKey) },
                 { cmdGoToStart, juce::KeyPress (juce::KeyPress::homeKey) },
                 { cmdGoToEnd, juce::KeyPress (juce::KeyPress::endKey) },
                 { cmdMultitrack, juce::KeyPress ('1') },
                 { cmdZoomIn, juce::KeyPress ('=') },
                 { cmdZoomOut, juce::KeyPress ('-') },
                 { cmdAddMarker, juce::KeyPress ('m') },
                 { cmdPreviousMarker, juce::KeyPress (juce::KeyPress::leftKey, cmd | alt, 0) },
                 { cmdNextMarker, juce::KeyPress (juce::KeyPress::rightKey, cmd | alt, 0) },
                 { cmdConvertRate, juce::KeyPress ('t', shift, 0) },
                 { cmdCaptureNoisePrint, juce::KeyPress ('p', shift, 0) },
                 { cmdNudgeEarlier, juce::KeyPress (',', alt, 0) },
                 { cmdNudgeLater, juce::KeyPress ('.', alt, 0) },
                 { cmdSplitClip, juce::KeyPress ('k', cmd, 0) },
                 { cmdCrop, juce::KeyPress ('t', cmd, 0) },
                 { cmdSnapping, juce::KeyPress ('s') },
                 { cmdPreviousEdge, juce::KeyPress (juce::KeyPress::leftKey, cmd, 0) },
                 { cmdNextEdge, juce::KeyPress (juce::KeyPress::rightKey, cmd, 0) },
                 { cmdRepeatLast, juce::KeyPress ('r', shift, 0) } };
    return set;
}

class MainComponent::KeyShortcutsWindow final : public juce::DocumentWindow
{
public:
    KeyShortcutsWindow (juce::ApplicationCommandManager& commandManager, std::function<void()> onClose)
        : DocumentWindow ("Keyboard Shortcuts", Theme::surface, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new KeyShortcuts::Editor (commandManager, { auditionStyleKeys() }), true);
        setResizable (true, false);
        setResizeLimits (520, 420, 3000, 3000);
        centreWithSize (680, 640);
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

private:
    std::function<void()> closeCallback;
};

//==============================================================================
/** The parametric EQ: its bands and curve, with Apply to filter the selection (or the whole file)
    and Flat to set every band back to 0 dB. It stays open, so it can be applied again. */
class MainComponent::EqWindow final : public juce::DocumentWindow
{
public:
    EqWindow (const ParametricEq::Settings& settings, bool linearPhase, double sampleRate, std::function<void (const ParametricEq&)> onApply,
              std::function<void (const ParametricEq::Settings&, bool)> onSettings, std::function<void (const ParametricEq*)> onPreview,
              std::function<void()> onClose)
        : DocumentWindow ("Parametric EQ", Theme::surface, DocumentWindow::closeButton),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        content = new Content (settings, linearPhase, sampleRate, std::move (onApply), std::move (onSettings), std::move (onPreview));
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (780, 320, 3000, 2000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override              { closeCallback(); }

    /** Unticks Preview without asking for anything (the preview has ended elsewhere). */
    void previewEnded()                             { content->preview.setToggleState (false, juce::dontSendNotification); }

    /** Ticks Preview and starts it, as a click on it would. */
    void startPreviewing()                          { content->preview.setToggleState (true, juce::dontSendNotification); content->previewBands (content->panel.get()); }

    /** Where the drawing's spectrum comes from: the newest output, as for the analysis panels. */
    void setReadOutput (std::function<double (juce::AudioBuffer<float>&)> read)
    {
        content->readOutput = std::move (read);
        content->panel->readOutput = content->readOutput;
    }

private:
    struct Content final : public juce::Component
    {
        Content (const ParametricEq::Settings& settings, bool linearPhase, double sampleRate, std::function<void (const ParametricEq&)> onApply,
                 std::function<void (const ParametricEq::Settings&, bool)> onSettings, std::function<void (const ParametricEq*)> onPreview)
            : rate (sampleRate), remember (std::move (onSettings)), previewBands (std::move (onPreview))
        {
            makePanel (settings, linearPhase);
            apply.onClick = [this, applyBands = std::move (onApply)] { applyBands (*panel); };   // applying ends the preview
            preview.onClick = [this] { previewBands (preview.getToggleState() ? panel.get() : nullptr); };
            presets = std::make_unique<EffectPresets::Bar> (EffectPresets::userStore(), BuiltInEffects::parametricEqId,
                                                            [this] { return BuiltInEffects::eqPresetValues (panel->getSettings(), panel->isLinearPhase()); },
                                                            [this] (const EffectPresets::Values& v)
                                                            {
                                                                const auto s = BuiltInEffects::eqSettingsFrom (v, panel->getSettings());
                                                                const auto linear = BuiltInEffects::eqLinearPhaseFrom (v, panel->isLinearPhase());
                                                                makePanel (s, linear);
                                                                remember (s, linear);
                                                                changed();
                                                                resized();
                                                            });
            addAndMakeVisible (*presets);
            flat.onClick = [this]
            {
                auto s = panel->getSettings();

                for (auto& b : s)
                    b.band.gainDb = 0.0;

                const auto linear = panel->isLinearPhase();
                makePanel (s, linear);
                remember (s, linear);
                changed();
                resized();
            };
            addAndMakeVisible (apply);
            addAndMakeVisible (flat);
            addAndMakeVisible (preview);
            setSize (juce::jmax (820, panel->getWidth()), panel->getHeight() + 40);   // wide enough for the buttons and presets
        }

        void makePanel (const ParametricEq::Settings& settings, bool linearPhase)
        {
            panel = std::make_unique<ParametricEq> (settings, rate);
            panel->setLinearPhase (linearPhase);
            panel->readOutput = readOutput;
            panel->onChange = [this]
            {
                remember (panel->getSettings(), panel->isLinearPhase());
                changed();
            };
            addAndMakeVisible (*panel);
        }

        void changed()
        {
            if (preview.getToggleState())
                previewBands (panel.get());
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto buttons = r.removeFromBottom (40).reduced (8, 6);
            apply.setBounds (buttons.removeFromRight (90));
            buttons.removeFromRight (8);
            flat.setBounds (buttons.removeFromRight (90));
            preview.setBounds (buttons.removeFromLeft (200));
            buttons.removeFromLeft (8);
            presets->setBounds (buttons);
            panel->setBounds (r);
        }

        double rate;
        std::function<void (const ParametricEq::Settings&, bool)> remember;
        std::function<void (const ParametricEq*)> previewBands;
        std::function<double (juce::AudioBuffer<float>&)> readOutput;
        std::unique_ptr<ParametricEq> panel;
        juce::TextButton apply { "Apply" }, flat { "Flat" };
        juce::ToggleButton preview { "Preview on the selection" };
        std::unique_ptr<EffectPresets::Bar> presets;
    };

    Content* content = nullptr;
    std::function<void()> closeCallback;
};

//==============================================================================
/** Shows a plug-in's own editor, or a generic parameter editor when it has none. */
class MainComponent::PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioProcessor& p, std::function<void (PluginEditorWindow*)> onClose)
        : DocumentWindow (p.getName(), Theme::surface, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
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
        // The editor must be deleted before the plug-in instance, and the plug-in told it has
        // gone: otherwise it keeps the editor as its active one and refuses to make another, and
        // the next window for it falls back to the generic parameter list.
        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            processor.editorBeingDeleted (editor);

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
        : DocumentWindow ("Plug-ins", Theme::surface, DocumentWindow::closeButton),
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

            Theme::follow (status, juce::Label::textColourId, Theme::text);
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
                owner.recordStep ({ "rack", {}, std::make_shared<const juce::XmlElement> (*rackXml) });
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
/** The dock's middle: the open files' tabs over the file's waveform, or the session's overview over
    its tracks. */
class MainComponent::EditorPanel final : public juce::Component
{
public:
    EditorPanel (FileTabs& t, WaveformView& w, EditorBar& b, SessionOverview& o, MultitrackView& m)
        : tabs (t), waveform (w), bar (b), overview (o), multitrack (m)
    {
        addAndMakeVisible (tabs);
        addAndMakeVisible (waveform);
        addAndMakeVisible (bar);
        addChildComponent (overview);
        addChildComponent (multitrack);
    }

    /** Whether the open files show as tabs over the editor (the Files panel lists them either way). */
    void setTabsWanted (bool on)        { tabsWanted = on; resized(); }
    bool areTabsWanted() const          { return tabsWanted; }

    void resized() override
    {
        auto r = getLocalBounds();
        const auto showTabs = tabsWanted && waveform.isVisible() && tabs.hasTabs();
        tabs.setVisible (showTabs);
        tabs.setBounds (showTabs ? r.removeFromTop (FileTabs::height) : juce::Rectangle<int>());
        bar.setVisible (waveform.isVisible());
        bar.setBounds (waveform.isVisible() ? r.removeFromBottom (EditorBar::height) : juce::Rectangle<int>());
        waveform.setBounds (r);
        r = getLocalBounds();
        overview.setBounds (r.removeFromTop (SessionOverview::height));
        r.removeFromTop (4);
        multitrack.setBounds (r);
    }

private:
    bool tabsWanted = false;
    FileTabs& tabs;
    WaveformView& waveform;
    EditorBar& bar;
    SessionOverview& overview;
    MultitrackView& multitrack;
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
    Theme::apply (Theme::make (settings->getValue (themeKey, "Dark"), settings->getValue (accentKey, "Blue"),
                               settings->getValue (waveformColourKey, "Theme")));
    TimeFormat::set (TimeFormat::fromKey (settings->getValue (timeFormatKey, TimeFormat::key (TimeFormat::Kind::minutesSeconds))));
    deadMansPedalFile = settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");
    places = std::make_unique<WindowPlaces> (*settings);

    engine.knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
    engine.knownPlugins.addChangeListener (this);
    engine.addChangeListener (this);
    engine.document.addChangeListener (this);
    engine.session.addChangeListener (this);

    for (auto* b : { &editorButton, &multitrackButton, &openButton, &pluginsButton, &audioButton })
        addAndMakeVisible (b);

    // The mode switch: the side in use is lit, the other is where a click goes.
    editorButton.setTooltip ("Edit the file; the lit side is the mode in use (Cmd+4 switches)");
    multitrackButton.setTooltip ("Work in the multitrack session; the lit side is the mode in use (Cmd+4 switches)");
    editorButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    multitrackButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    Theme::follow (editorButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
    Theme::follow (multitrackButton, juce::TextButton::buttonOnColourId, Theme::accentDeep);
    editorButton.onClick = [this] { setMultitrack (false); };
    multitrackButton.onClick = [this] { setMultitrack (true); };

    openButton.onClick     = [this] { commands.invokeDirectly (cmdOpen, true); };
    pluginsButton.onClick  = [this] { showPluginList(); };
    audioButton.onClick    = [this] { showAudioSettings(); };
    selectionPanel.onClick = [this] { commands.invokeDirectly (cmdSetSelection, true); };

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
    addAndMakeVisible (outputMeter);
    outputMeter.setNumChannels (2);
    outputMeter.setTooltip ("What goes to the outputs, after the effects. Click to clear the clip lights.");
    outputMeter.readPeak = [this] (int ch)
    {
        if (ch == 0)
            outputPeaks = engine.takeMasterPeaks (AudioEngine::MasterReader::mainWindow);

        return ch == 0 ? outputPeaks.first : outputPeaks.second;
    };
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

    for (auto* l : { &fileLabel, &deviceLabel, &selectionLabel })
        addAndMakeVisible (l);

    addAndMakeVisible (loadReadout);

    Theme::follow (deviceLabel, juce::Label::textColourId, Theme::dimText);
    Theme::follow (selectionLabel, juce::Label::textColourId, Theme::text);
    selectionLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));

    waveform.setDisplay ((WaveformView::Display) juce::jlimit (0, 2, settings->getIntValue (displayKey, 0)));
    waveform.setWaveformShare (settings->getDoubleValue (waveShareKey, 0.35));
    waveform.onWaveformShareChanged = [settings] (double share) { settings->setValue (waveShareKey, share); };
    fadeShape = FadeShapes::fromId (settings->getValue (fadeShapeKey));
    waveform.onFilesDropped = [this] (const juce::Array<juce::File>& files) { openDocuments (files); };
    waveform.onSelectionChanged = [this] { commands.commandStatusChanged(); timerCallback(); };
    waveform.onContextMenu = [this]
    {
        juce::PopupMenu menu;

        for (int id : std::initializer_list<int> { cmdPlayPause, cmdLoopPlayback, 0, cmdCut, cmdCopy, cmdPaste, cmdDelete, cmdCrop, cmdSelectAll })
        {
            if (id == 0)
                menu.addSeparator();
            else
                menu.addCommandItem (&commands, id);
        }

        menu.addSeparator();
        menu.addSubMenu ("Effects", getMenuForIndex (2, "Effects"));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&waveform).withMousePosition());
    };

    editorBar = std::make_unique<EditorBar> (commands, waveform, engine.document,
                                             EditorBar::Commands { cmdZoomOut, cmdZoomIn, cmdZoomFit, cmdShorterTracks, cmdTallerTracks,
                                                                   cmdResetVerticalZoom, cmdGoToTime, cmdSetSelection,
                                                                   cmdBoxTool, cmdLassoTool, cmdBrushTool, cmdSpectralHeal });
    editorPanel = std::make_unique<EditorPanel> (fileTabs, waveform, *editorBar, overview, multitrack);
    fileTabs.onChoose = [this] (int index) { switchToFile (index); };
    fileTabs.onClose = [this] (int index) { closeFile (index); };
    multitrack.setSnapping (settings->getBoolValue (snappingKey, true));
    multitrack.setCrossfading (settings->getBoolValue (crossfadingKey, true));
    multitrack.setFindingZeroCrossings (settings->getBoolValue (zeroCrossingsKey, false));
    selectZeroCrossings = settings->getBoolValue (selectZeroCrossingsKey, false);
    waveform.snapSelectionFrame = [this] (juce::int64 frame)
    {
        return selectZeroCrossings ? AudioEdits::zeroCrossingNear (engine.document.getSnapshot(), frame, zeroCrossingReach()) : frame;
    };
    loopPlayback = settings->getBoolValue (loopPlaybackKey, false);
    editorPanel->setTabsWanted (settings->getBoolValue (fileTabsKey, false));
    multitrack.setLaneHeight (settings->getIntValue (laneHeightKey, MultitrackView::trackHeight));
    rolls.preroll = juce::jlimit (0.0, SessionRecording::Rolls::longest, settings->getDoubleValue (prerollKey, rolls.preroll));
    rolls.postroll = juce::jlimit (0.0, SessionRecording::Rolls::longest, settings->getDoubleValue (postrollKey, rolls.postroll));
    multitrack.onOpenInEditor = [this] (const juce::File& f) { openFile (f); };
    multitrack.onFilesDropped = [this] (const juce::StringArray& files, int track, juce::int64 frame) { addFilesToSession (files, track, frame); };
    multitrack.onSelectionChanged = [this] { commands.commandStatusChanged(); timerCallback(); };

    // Option-dragging a clip's edge: the clip (selected by the drag) stretched to where it was let go.
    multitrack.onStretchClip = [this] (juce::uint32 clipId, double ratio, bool keepEnd)
    {
        if (multitrack.getSelectedClip() != clipId)
            return;

        processSelectedClip ("Stretch Clip", "Stretched", [ratio, rate = engine.session.getSampleRate()] (const SessionClip& c, const auto& progress)
        {
            return SessionEdits::stretchedAudio (c, ratio, rate, progress);
        }, keepEnd);
    };
    multitrack.onEditPlugin = [this] (juce::AudioPluginInstance& p) { showEditor (p); };

    waveform.getRecentFiles = [this]
    {
        juce::Array<juce::File> files;

        for (int i = 0; i < recentFiles.getNumFiles(); ++i)
            if (isThere (recentFiles.getFile (i)))
                files.add (recentFiles.getFile (i));

        return files;
    };
    fileChecks.onChange = [this]
    {
        waveform.repaint();
        menuItemsChanged();
    };
    waveform.onRecentFileClicked = [this] (const juce::File& f) { openDocument (f); };
    rackView.onEditSlot = [this] (int index) { showEditor (index); };
    engine.onPluginAboutToBeRemoved = [this] (juce::AudioProcessor* p) { closeEditorFor (p); };
    engine.onEffectsNotCopied = [] (const juce::StringArray& names)
    {
        showNote ("Surround Effects", names.joinIntoString (", ") + (names.size() == 1 ? " couldn't" : " couldn't each")
                                          + " be opened again for the channels past left and right (centre, LFE and surrounds), which play silence through it.");
    };

    // The panels as the current workspace left them. Before there were workspaces the rack could be
    // hidden; it stays hidden.
    workspaces = std::make_unique<Dock::Workspaces> (builtInWorkspaces());

    if (auto saved = settings->getXmlValue (workspacesKey))
    {
        workspaces->restore (*saved, Panels::all(), Panels::editor);
    }
    else if (! settings->getBoolValue (rackShownKey, true))
    {
        auto layout = workspaces->current().now;
        layout.remove (Panels::rack);
        workspaces->update (layout);
    }

    dock = std::make_unique<Dock::Area> (static_cast<Dock::Host&> (*this));
    dock->setLayout (workspaces->current().now);
    addAndMakeVisible (*dock);

    commands.registerAllCommandsForTarget (this);
    commands.setFirstCommandTarget (this);
    transportBar = std::make_unique<TransportBar::Bar> (commands, TransportBar::Commands { cmdGoToStart, cmdPlayPause, cmdStop, cmdRecord, cmdLoopPlayback, cmdGoToEnd,
                                                                                         cmdZoomIn, cmdZoomOut, cmdZoomSelection, cmdZoomFit });
    addAndMakeVisible (*transportBar);

    KeyShortcuts::restore (*commands.getKeyMappings(), *settings);
    editorBar->refreshTooltips();
    commands.getKeyMappings()->addChangeListener (this);
    addKeyListener (commands.getKeyMappings());
    setApplicationCommandManagerToWatch (&commands);   // menus refresh when command state changes
   #if JUCE_MAC
    juce::PopupMenu appMenu;   // at the top of the application menu
    appMenu.addCommandItem (&commands, cmdAbout);
    juce::MenuBarModel::setMacMainMenu (this, &appMenu);
   #endif

    RecentFiles::restore (recentFiles, *settings);

    // Reopening the last file is quiet, and read off the message thread, so a slow or absent volume
    // doesn't hold up the window: it comes in when read, unless something else was opened, recorded
    // or edited first. One that can no longer be read is logged and forgotten; one that isn't there
    // (its volume not mounted) is kept for next time.
    loading = std::make_unique<LoadingScreen>();
    loading->onFinished = [this] { bootFinished(); };

    if (const juce::File last (settings->getValue (lastFileKey)); last != juce::File())
    {
        loading->begin ("file", "Reopening " + last.getFileName(),
                        "The last file's volume isn't answering. Continue to use SpectraBlade now; the file opens when it can be read.");
        BackgroundLoad::start (last, [safe = juce::Component::SafePointer<MainComponent> (this), last] (AudioFileIO::Loaded loaded)
        {
            if (safe != nullptr)
                safe->loading->end ("file");

            if (safe == nullptr || (loaded.snapshot == nullptr && loaded.error.isEmpty()))
                return;

            auto& doc = safe->engine.document;

            if (doc.getFile() != juce::File() || doc.canUndo() || safe->isRecording())
                return;

            if (const auto error = safe->engine.adoptFile (last, std::move (loaded)); error.isNotEmpty())
            {
                juce::Logger::writeToLog ("Could not reopen " + last.getFullPathName() + ": " + error);
                safe->appProperties.getUserSettings()->removeValue (lastFileKey);
            }
        });
    }

    startOpeningDevice();
    loading->begin ("plug-ins", "Reading the plug-in list");
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->readPluginList();
    });

    refreshTitle();
    setWantsKeyboardFocus (true);
    setSize (1200, 680);
    startTimerHz (20);
    lastRecoveryCheck = juce::Time::getMillisecondCounter();
}

void MainComponent::readPluginList()
{
    if (auto xml = appProperties.getUserSettings()->getXmlValue (knownPluginsKey))
        engine.knownPlugins.recreateFromXml (*xml);

    BuiltInEffects::addTo (engine.knownPlugins);
    loading->end ("plug-ins");
}

void MainComponent::startOpeningDevice()
{
    // The audio system is asked on a thread of its own first: when its service doesn't answer,
    // that thread waits, not the window. Once it has answered, the device opens here as before.
    std::shared_ptr<const juce::XmlElement> saved (appProperties.getUserSettings()->getXmlValue (audioDeviceKey));
    const auto name = saved != nullptr ? saved->getStringAttribute ("audioOutputDeviceName") : juce::String();
    loading->begin ("device", name.isNotEmpty() ? "Opening " + name : juce::String ("Opening the audio device"),
                    "The audio device isn't answering. If other apps can't play sound either, the Mac's audio service has stopped: "
                    "restarting the interface, or the service (sudo killall coreaudiod in Terminal), clears it. "
                    "Continue to use SpectraBlade now; the device opens when it answers.");

    std::thread ([safe = juce::Component::SafePointer<MainComponent> (this), saved]
    {
        AudioEngine::probeDevice (saved.get());
        juce::MessageManager::callAsync ([safe, saved]
        {
            if (safe != nullptr)
                safe->finishOpeningDevice (saved.get());
        });
    }).detach();
}

void MainComponent::finishOpeningDevice (const juce::XmlElement* savedDevice)
{
    auto* settings = appProperties.getUserSettings();

    if (const auto error = engine.openDevice (savedDevice); error.isNotEmpty())
        showError ("Audio device", error);

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
        juce::Logger::writeToLog ("Audio device: " + device->getName() + ", " + juce::String (device->getCurrentSampleRate())
                                  + " Hz, " + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
    else
        juce::Logger::writeToLog ("Audio device: none");

    engine.setOutputPair (settings->getIntValue (outputPairKey, 0));
    engine.setSurroundOutputs (settings->getBoolValue ("surroundOutputs", false));

    if (const auto count = settings->getIntValue (inputCountKey, 0); count > 0)
        if (const auto error = engine.setInput (settings->getIntValue (inputFirstKey, 0), count); error.isNotEmpty())
            showError ("Recording input", error);

    // A session nobody has touched yet plays at the device's rate.
    if (auto* device = engine.deviceManager.getCurrentAudioDevice();
        device != nullptr && ! engine.session.canUndo() && engine.session.getFile() == juce::File())
    {
        SessionState empty;
        empty.sampleRate = device->getCurrentSampleRate();
        engine.clearTrackEffects();
        engine.session.reset (std::move (empty), {});
    }

    refreshOutputChoices();
    refreshInputChoices();
    commands.commandStatusChanged();
    loading->end ("device");
}

void MainComponent::showLoadingScreen (juce::Rectangle<int> windowBounds, std::function<void()> ready)
{
    whenReady = std::move (ready);

    if (loading->hasFinished())
    {
        bootFinished();
        return;
    }

    // A quarter of the window's room: half its width and height, the logo's shape kept.
    loading->setBounds (juce::Rectangle<int> (600, 380).withCentre (windowBounds.getCentre()));
    loading->addToDesktop (juce::ComponentPeer::windowHasDropShadow);
    loading->setVisible (true);
    loading->toFront (true);
}

void MainComponent::bootFinished()
{
    commands.commandStatusChanged();

    if (whenReady == nullptr)   // finished before the window was placed: it shows when it is
        return;

    whenReady();
    loading->setVisible (false);

    // Called from inside the loading screen, so it goes on the next message.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe == nullptr)
            return;

        safe->loading->removeFromDesktop();
        safe->grabKeyboardFocus();
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
    engine.onEffectsNotCopied = nullptr;
    engine.document.removeChangeListener (this);
    engine.session.removeChangeListener (this);
    engine.removeChangeListener (this);
    engine.knownPlugins.removeChangeListener (this);
    commands.getKeyMappings()->removeChangeListener (this);
    keyShortcutsWindow = nullptr;
    editorWindows.clear();
    pluginListWindow = nullptr;
    dock = nullptr;
    madePanels.clear();
    eqWindow = nullptr;
}

void MainComponent::saveSettings()
{
    auto* settings = appProperties.getUserSettings();

    // Only a device that is open is kept: with none open (its service stuck, say) the setup names
    // nothing, and the device that was chosen is asked for again next time.
    if (engine.deviceManager.getCurrentAudioDevice() != nullptr)
        if (auto xml = engine.deviceManager.createStateXml())
            settings->setValue (audioDeviceKey, xml.get());

    settings->setValue (outputPairKey, engine.getOutputPair());
    settings->setValue (inputFirstKey, engine.getInputFirstChannel());
    settings->setValue (inputCountKey, engine.getInputNumChannels());
    settings->setValue (displayKey, (int) waveform.getDisplay());
    settings->setValue (fadeShapeKey, FadeShapes::id (fadeShape));
    settings->setValue (lastFileKey, engine.document.getFile().getFullPathName());
    settings->saveIfNeeded();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (6);

    // Top: the mode and Open, the transport with the time and zoom in the middle, then the setup buttons.
    auto toolbar = r.removeFromTop (30);
    editorButton.setBounds (toolbar.removeFromLeft (64).reduced (0, 2));
    multitrackButton.setBounds (toolbar.removeFromLeft (86).reduced (0, 2));
    toolbar.removeFromLeft (6);
    openButton.setBounds (toolbar.removeFromLeft (76).reduced (0, 2));
    audioButton.setBounds (toolbar.removeFromRight (124).reduced (0, 2));
    toolbar.removeFromRight (6);
    pluginsButton.setBounds (toolbar.removeFromRight (96).reduced (0, 2));
    toolbar.removeFromRight (12);
    toolbar.removeFromLeft (12);
    const auto barWidth = juce::jmin (toolbar.getWidth(), transportBar->getIdealWidth());
    transportBar->setBounds (toolbar.withSizeKeepingCentre (barWidth, TransportBar::Bar::height));

    // Bottom: the devices and the file over its status line, the selection and view beside them.
    r.removeFromTop (4);
    auto bottom = r.removeFromBottom (46);
    auto devices = bottom.removeFromTop (24).reduced (0, 1);
    outputLabel.setBounds (devices.removeFromLeft (52));
    outputPair.setBounds (devices.removeFromLeft (juce::jmin (200, devices.getWidth() / 4)));
    devices.removeFromLeft (6);
    outputMeter.setBounds (devices.removeFromLeft (juce::jmin (140, devices.getWidth() / 4)).reduced (0, 3));
    devices.removeFromLeft (14);
    inputLabel.setBounds (devices.removeFromLeft (42));
    inputChoice.setBounds (devices.removeFromLeft (juce::jmin (170, devices.getWidth() / 2)));
    devices.removeFromLeft (6);
    inputMeter.setBounds (devices.reduced (0, 3));
    auto status = bottom;
    loadReadout.setBounds (status.removeFromRight (150));
    deviceLabel.setBounds (status.removeFromRight (status.getWidth() / 3));
    selectionLabel.setBounds (status.removeFromRight (status.getWidth() / 2));
    fileLabel.setBounds (status);
    r.removeFromBottom (4);

    if (dock != nullptr)
        dock->setBounds (r);
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (int id = cmdOpen; id < cmdCount; ++id)
        ids.add (id);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    describeCommand (id, info);

    // While loading only About works; afterwards, what plays, records or sets up the device waits for it.
    if ((booting() && id != cmdAbout) || (! engine.isDeviceOpen() && (id == cmdPlayPause || id == cmdRecord || id == cmdAudioSettings)))
        info.setActive (false);
}

void MainComponent::describeCommand (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const auto& doc = engine.document;
    const auto hasAudio = doc.hasAudio();
    const auto hasSpectral = ! waveform.getSpectralShape().isEmpty() || (! waveform.getSelection().isEmpty() && ! waveform.getSpectralBand().isEmpty());
    const auto hasSelection = ! waveform.getSelection().isEmpty();

    switch (id)
    {
        case cmdOpen:       info.setInfo ("Open...", "Open an audio file", "File", 0); info.addDefaultKeypress ('o', cmd); break;
        case cmdSave:       info.setInfo ("Save", "Save the file", "File", 0); info.addDefaultKeypress ('s', cmd);
                            info.setActive (hasAudio && doc.isDirty()); break;
        case cmdSaveAs:     info.setInfo ("Save As...", "Save to a new file or format", "File", 0); info.addDefaultKeypress ('s', cmd | shift);
                            info.setActive (hasAudio); break;

        case cmdUndo:       if (! multitrackMode && waveform.canUndoStroke())
                            {
                                info.setInfo ("Undo Stroke", "Take back the last stroke drawn in the spectrogram", "Edit", 0);
                                info.addDefaultKeypress ('z', cmd);
                                break;
                            }
                            info.setInfo (doc.canUndo() ? "Undo " + doc.getUndoDescription() : juce::String ("Undo"), "Undo", "Edit", 0);
                            info.addDefaultKeypress ('z', cmd); info.setActive (doc.canUndo()); break;
        case cmdRedo:       info.setInfo (doc.canRedo() ? "Redo " + doc.getRedoDescription() : juce::String ("Redo"), "Redo", "Edit", 0);
                            info.addDefaultKeypress ('z', cmd | shift); info.setActive (doc.canRedo()); break;
        case cmdCut:        info.setInfo ("Cut", "Cut the selection", "Edit", 0); info.addDefaultKeypress ('x', cmd); info.setActive (hasSelection); break;
        case cmdCopy:       info.setInfo ("Copy", "Copy the selection", "Edit", 0); info.addDefaultKeypress ('c', cmd); info.setActive (hasSelection); break;
        case cmdPaste:      info.setInfo ("Paste", "Paste at the cursor or over the selection", "Edit", 0); info.addDefaultKeypress ('v', cmd);
                            info.setActive (! clipboard.isEmpty()); break;
        case cmdSaveSelection: info.setInfo ("Save Selection As...", "Save the selection, and the markers in it, to a new file", "File", 0);
                            info.setActive (! waveform.getSelection().isEmpty()); break;
        case cmdSavePieces: info.setInfo ("Save Pieces Between Markers As...", "Split the file at its markers and save each piece to a file of its own, in a folder", "File", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(),
                                                                     [&doc] (const Marker& m) { return ! m.isRegion() && m.start < doc.getNumFrames(); })); break;
        case cmdSaveRegions: info.setInfo ("Save Each Region As...", "Save every region of the file to a file of its own, in a folder", "File", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(),
                                                                     [] (const Marker& m) { return m.isRegion(); })); break;
        case cmdPasteMix:   info.setInfo ("Paste Mix", "Mix the clipboard into the audio from the cursor, or the start of the selection", "Edit", 0);
                            info.addDefaultKeypress ('v', cmd | shift); info.setActive (! clipboard.isEmpty() && hasAudio); break;
        case cmdPasteFill:  info.setInfo ("Paste to Fill Selection", "Replace the selection with the clipboard repeated to the same length", "Edit", 0);
                            info.setActive (! clipboard.isEmpty() && hasSelection); break;
        case cmdDelete:     info.setInfo ("Delete", hasSpectral ? "Remove what is inside the box, lasso or brush selection in the spectrogram, keeping the file's length"
                                                             : "Delete the selection, taking its time out of the file", "Edit", 0);
                            info.addDefaultKeypress (juce::KeyPress::backspaceKey, 0); info.addDefaultKeypress (juce::KeyPress::deleteKey, 0);
                            info.setActive (hasSelection || hasSpectral); break;
        case cmdSelectAll:  info.setInfo ("Select All", "Select the whole file", "Edit", 0); info.addDefaultKeypress ('a', cmd); info.setActive (hasAudio); break;
        case cmdCrop:       info.setInfo ("Crop to Selection", "Keep only the selection", "Edit", 0); info.addDefaultKeypress ('t', cmd); info.setActive (hasSelection); break;

        case cmdGain:       info.setInfo ("Gain...", "Change the level", "Effects", 0); info.setActive (hasAudio); break;
        case cmdNormalize:  info.setInfo ("Normalize...", "Set the peak level", "Effects", 0); info.setActive (hasAudio); break;
        case cmdFadeIn:     info.setInfo ("Fade In", "Fade in over the selection", "Effects", 0); info.setActive (hasSelection); break;
        case cmdFadeOut:    info.setInfo ("Fade Out", "Fade out over the selection", "Effects", 0); info.setActive (hasSelection); break;
        case cmdCropMargins: info.setInfo ("Crop with Margins...", "Crop to the selection plus a time either side of it", "Edit", 0);
                            info.setActive (hasSelection); break;
        case cmdStereoWidth: info.setInfo ("Stereo Width...", "Narrow or widen a stereo file, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdPanRamp:    info.setInfo ("Pan Ramp...", "Move a stereo file's balance evenly from one position to another across the selection", "Effects", 0);
                            info.setActive (hasSelection && doc.getNumChannels() == 2); break;
        case cmdSilenceOutside: info.setInfo ("Silence Outside Selection", "Silence everything before and after the selection, keeping the length", "Effects", 0);
                            info.setActive (hasSelection && waveform.getSelection().getLength() < doc.getNumFrames()); break;
        case cmdFadeRegions: info.setInfo ("Fade Each Region...", "Fade in at the start and out at the end of every region", "Effects", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return m.isRegion(); }));
                            break;
        case cmdNormalizeRegions: info.setInfo ("Normalize Each Region...", "Bring every region's peak to the same level, each on its own", "Effects", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return m.isRegion(); }));
                            break;
        case cmdReverseRegions: info.setInfo ("Reverse Each Region", "Play every region backwards in place", "Effects", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return m.isRegion(); }));
                            break;
        case cmdEcho:       info.setInfo ("Echo...", "Add repeats of the audio, each quieter than the last, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdEqBand:     info.setInfo ("EQ Band...", "Boost or cut a band of frequencies, or filter above or below one, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdParametricEq: info.setInfo ("Parametric EQ", "A window of five EQ bands over their response curve, applied to the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio || eqWindow != nullptr); info.setTicked (eqWindow != nullptr); break;
        case cmdCompressor: info.setInfo ("Compressor...", "Turn down what goes over a threshold by a ratio, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdLimiter:    info.setInfo ("Limiter...", "Keep every sample under a ceiling, with gain before it to make the audio louder", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdMultiband:  info.setInfo ("Multiband Compressor...", "Compress the lows, the middle and the highs each on its own", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdDeEsser:    info.setInfo ("De-esser...", "Turn down harsh S sounds: the highs, whenever they get loud", "Effects", 0);
        case cmdMouthDeClick:
            info.setInfo ("Mouth De-click...", "Find and rebuild the small clicks of lips and tongue in speech", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdDeCrackle:
            info.setInfo ("De-crackle...", "Find and rebuild dense tiny clicks: crackle", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdCodecPreview:
            info.setInfo ("Codec Preview...", "Hear the selection as it will come back from an MP3 or AAC file at a bit rate", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && engine.document.getNumChannels() <= 2 && backgroundJob == nullptr);
            break;
        case cmdAzimuth:
            info.setInfo ("Azimuth...", "Find the drifting time offset between the channels of a tape transfer and take it out", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && engine.document.getNumChannels() == 2 && backgroundJob == nullptr);
            break;
        case cmdDialogueContour:
            info.setInfo ("Dialogue Contour...", "Reshape the pitch of a phrase's ending: a fall or a lift over its last moments", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdCaptureAmbience: info.setInfo ("Capture Room Tone", "Learn the room tone from a selection of it alone, to lay under other parts", "Effects", 0); break;
        case cmdAmbienceMatch:
            info.setInfo ("Ambience Match...", "Lay the captured room tone under the selection", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && ambiencePrint.has_value() && backgroundJob == nullptr);
            break;
        case cmdRebalance:
            info.setInfo ("Rebalance...", "Turn the voice, the bass, the drums and the rest of a mix up or down", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdSpectralRecovery:
            info.setInfo ("Spectral Recovery...", "Rebuild the highs missing from band-limited audio from the band that is there", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdWowFlutter:
            info.setInfo ("Wow and Flutter...", "Find the pitch drift of a tape or a record and take it out", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdSpill:
            info.setInfo ("Wind, Rustle and Bleed...", "Lower gusts of wind, bursts of rustle and the bleed between words in a speech recording", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdBreathControl: info.setInfo ("Breath Control...", "Find the breaths between words and lower them", "Effects", 0); break;
        case cmdDePlosive: info.setInfo ("De-plosive...", "Lower the thumps a p or a b puts into the microphone", "Effects", 0); break;
        case cmdAdaptiveDenoise:
            info.setInfo ("Adaptive Noise Reduction...", "Reduce noise with no noise print, following the noise as it changes", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdDialogueIsolation:
            info.setInfo ("Dialogue Isolation...", "Keep the speech and lower everything else, in the selection or the whole file", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && backgroundJob == nullptr);
            break;
        case cmdDither:     info.setInfo ("Dither...", "Round the audio to 16 or 24 bits with dither, the last step before saving at that depth", "Effects", 0); break;
        case cmdCaptureMatchReference: info.setInfo ("Capture Match Reference", "Learn the tone of the selection, to match other sounds to it", "Effects", 0); break;
        case cmdCaptureMatchReferenceFromFile: info.setInfo ("Capture Match Reference from File...", "Learn the tone of an audio file, to match sounds to it", "Effects", 0); break;
        case cmdMatchEq:
            info.setInfo ("Match EQ...", "Take the captured reference's tone onto the selection or the whole file", "Effects", 0);
            info.setActive (engine.document.hasAudio() && ! multitrackMode && matchReference.has_value() && backgroundJob == nullptr);
            break;
        case cmdTransientShaper: info.setInfo ("Transient Shaper...", "Turn the attacks and the sustain of hits up or down on their own, in the lows, the middle and the highs", "Effects", 0); break;
        case cmdMaximizer:  info.setInfo ("Maximizer...", "Make the sound as loud as it can be under a true-peak ceiling, with a character and the hits kept", "Effects", 0); break;
        case cmdExciter:    info.setInfo ("Exciter...", "Drive the lows, the middle and the highs each into a saturation curve of its own: tube, tape or transistor", "Effects", 0); break;
        case cmdMultibandWidth: info.setInfo ("Multiband Stereo Width...", "Narrow or widen the lows, the middle and the highs each on its own: a mono low end, wider highs", "Effects", 0); break;
        case cmdDynamicEq:  info.setInfo ("Dynamic EQ...", "EQ bands that cut or boost only while their part of the sound is over (or under) a threshold", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdChorus:     info.setInfo ("Chorus...", "Thicken the sound with a slowly swept copy of it, a few milliseconds late", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdFlanger:    info.setInfo ("Flanger...", "Sweep a comb of notches through the sound with a very short swept delay fed back on itself", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdPhaser:     info.setInfo ("Phaser...", "Sweep notches through the sound with a chain of all-pass filters", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdReverb:     info.setInfo ("Reverb...", "Add a room's reverberation, ringing on past the selection", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdConvolutionReverb: info.setInfo ("Convolution Reverb...", "Add the reverberation of a recorded space from an impulse response file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdGraphicEq10: info.setInfo ("Graphic EQ (10 Bands)...", "Boost or cut ten bands an octave apart, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdAbout: info.setInfo ("About SpectraBlade", "Show the version, the copyright and the licence", "Help", 0); break;
        case cmdGraphicEq31: info.setInfo ("Graphic EQ (31 Bands)...", "Boost or cut 31 bands a third of an octave apart, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdTremolo:    info.setInfo ("Tremolo...", "Make the level rise and fall at a chosen rate and depth, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdGainRamp:   info.setInfo ("Gain Ramp...", "Change the gain evenly from one level to another across the selection", "Effects", 0);
                            info.setActive (hasSelection); break;
        case cmdFadeEdges:  info.setInfo ("Fade Selection Edges...", "Fade in at the start of the selection and out at its end", "Effects", 0);
                            info.setActive (hasSelection); break;
        case cmdSilence:    info.setInfo ("Silence", "Replace with silence", "Effects", 0); info.setActive (hasSelection); break;
        case cmdInvert:     info.setInfo ("Invert", "Flip the polarity", "Effects", 0); info.setActive (hasAudio); break;
        case cmdReverse:    info.setInfo ("Reverse", "Play backwards", "Effects", 0); info.setActive (hasAudio); break;
        case cmdStretch:    info.setInfo ("Stretch...", "Make the selection, or the whole file, longer or shorter, keeping its pitch", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdPitch:      info.setInfo ("Shift Pitch...", "Make the selection, or the whole file, higher or lower, keeping its length", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdExtremeStretch: info.setInfo ("Extreme Stretch...", "Smear the selection, or the whole file, to many times its length: a pad that keeps the pitch and colour, not the timing", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdRemoveDc:   info.setInfo ("Remove DC Offset", "Take each channel's average level out of the selection, or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdBalance:    info.setInfo ("Balance...", "Move the stereo balance of the selection, or the whole file, left or right", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdSilenceLeft: info.setInfo ("Silence Left Channel", "Silence the left channel of the selection only", "Effects", 0);
                            info.setActive (hasSelection && doc.getNumChannels() == 2); break;
        case cmdSilenceRight: info.setInfo ("Silence Right Channel", "Silence the right channel of the selection only", "Effects", 0);
                            info.setActive (hasSelection && doc.getNumChannels() == 2); break;
        case cmdLeftToRight: info.setInfo ("Copy Left to Right", "Replace the right channel with the left, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdRightToLeft: info.setInfo ("Copy Right to Left", "Replace the left channel with the right, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdInvertLeft: info.setInfo ("Invert Left Channel", "Flip the polarity of the left channel only", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdInvertRight: info.setInfo ("Invert Right Channel", "Flip the polarity of the right channel only", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdSwapChannels: info.setInfo ("Swap Channels", "Exchange left and right in the selection, or the whole file", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() == 2); break;
        case cmdSetSelection: info.setInfo ("Set Selection...", "Type the start and length of the selection", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdRegionsOverClips: info.setInfo ("Add Regions over Clips", "Add a region over each stretch where clips run without a gap", "Markers", 0);
                            info.setActive (multitrackMode && engine.session.getState()->getLength() > 0); break;
        case cmdFitRegions: info.setInfo ("Fit Regions to Sound", "Shrink every region to the sound inside it, at the level Trim Silence last used", "Markers", 0);
                            info.setActive (hasAudio && std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return m.isRegion(); })); break;
        case cmdRepair:    info.setInfo ("Repair Selection", "Redraw a short selection as a smooth curve between the frames around it", "Effects", 0);
                            info.setActive (hasSelection && waveform.getSelection().getLength() <= AudioEdits::maxRepairFrames); break;
        case cmdNoiseGate: info.setInfo ("Noise Gate...", "Silence every stretch quieter than a level, in the selection or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdShortenPauses: info.setInfo ("Shorten Pauses...", "Cut every pause between sounds that is longer than a time down to that time", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdMarkersAtSilences: info.setInfo ("Add Markers at Silences...", "Put a marker in the middle of every pause between sounds, to split takes", "Markers", 0);
                            info.setActive (hasAudio); break;
        case cmdNextSound:  info.setInfo ("Select Next Sound", "Select the next stretch of sound between silences, at the settings Find Silence last used", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdPreviousSound: info.setInfo ("Select Previous Sound", "Select the previous stretch of sound between silences, at the settings Find Silence last used", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdFindSilence: info.setInfo ("Find Next Silence...", "Select the next stretch quieter than a level for at least a chosen time", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdRegionsOverSounds: info.setInfo ("Add Regions over Sounds...", "Put a region over every stretch of sound between silences, to split takes", "Markers", 0);
                            info.setActive (hasAudio); break;
        case cmdMarkClipping: info.setInfo ("Add Markers at Clipping", "Put a marker at the start of every run of three or more samples at full scale", "Markers", 0);
                            info.setActive (hasAudio); break;
        case cmdFindClipping: info.setInfo ("Find Next Clipping", "Select the next run of three or more samples at full scale after the cursor", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdSelectAudible: info.setInfo ("Select Audible Span", "Select from the first sound to the last, in the selection or the whole file, at the level Trim Silence last used", "Edit", 0);
                            info.setActive (hasAudio); break;
        case cmdMixToMono:  info.setInfo ("Mix to Mono", "Average the channels into one", "Effects", 0);
                            info.setActive (hasAudio && doc.getNumChannels() > 1); break;
        case cmdMakeStereo: info.setInfo ("Make Stereo", "Copy the one channel to left and right, or fold 5.1 or 7.1 down to stereo", "Effects", 0);
                            if (hasAudio && Surround::isSurround (doc.getNumChannels()))
                                info.shortName = "Fold Down to Stereo";
                            info.setActive (hasAudio && (doc.getNumChannels() == 1 || Surround::isSurround (doc.getNumChannels()))); break;
        case cmdTrimSilence: info.setInfo ("Trim Silence...", "Remove what is below a level from the start and end of the file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdGenerateTone: info.setInfo ("Generate Tone...", "Put a tone or sweep in at the cursor, or the start of the selection", "Effects", 0);
                              info.setActive (hasAudio); break;
        case cmdGenerateTones: info.setInfo ("Generate DTMF Tones...", "Put telephone signalling tones (DTMF or MF) for typed digits in at the cursor, or the start of the selection", "Effects", 0);
                               info.setActive (hasAudio); break;
        case cmdGenerateNoise: info.setInfo ("Generate Noise...", "Put white, pink or brown noise in at the cursor, or the start of the selection", "Effects", 0);
                               info.setActive (hasAudio); break;
        case cmdInsertSilence: info.setInfo ("Insert Silence...", "Put a chosen length of silence in at the cursor, or the start of the selection", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdApplyRack:  info.setInfo ("Apply Effects Rack", "Render the rack's effects into the audio", "Effects", 0);
                            info.addDefaultKeypress ('r', cmd); info.setActive (hasAudio && engine.getNumRackSlots() > 0); break;

        case cmdPlayPause:  info.setInfo ("Play/Pause", "Play the selection (with a box, lasso or brush selection in the spectrogram, only what is inside it), or from the cursor", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::spaceKey, 0); info.setActive (hasAudio); break;
        case cmdLoopPlayback: info.setInfo ("Loop Playback", "Play the selection or time range over and over", "Transport", 0);
                            info.addDefaultKeypress ('l', cmd); info.setTicked (loopPlayback); break;
        case cmdNextRegion: case cmdPreviousRegion:
        {
            const auto shown = markersShown();
            info.setInfo (id == cmdNextRegion ? "Next Region" : "Previous Region",
                          id == cmdNextRegion ? "Select the next region" : "Select the region before", "Markers", 0);
            info.addDefaultKeypress (id == cmdNextRegion ? ']' : '[', cmd | shift);
            info.setActive (std::any_of (shown.begin(), shown.end(), [] (const Marker& m) { return m.isRegion(); }));
            break;
        }
        case cmdStepSelectionLater: case cmdStepSelectionEarlier:
            info.setInfo (id == cmdStepSelectionLater ? "Step Selection Later" : "Step Selection Earlier",
                          "Move the selection by its own length, to hear or edit the next stretch of the same size", "Edit", 0);
            info.addDefaultKeypress (id == cmdStepSelectionLater ? juce::KeyPress::rightKey : juce::KeyPress::leftKey, alt | shift);
            info.setActive (! waveform.getSelection().isEmpty());
            break;
        case cmdRepeatSelection: info.setInfo ("Repeat Selection...", "Put copies of the selection right after it", "Edit", 0);
                            info.setActive (! waveform.getSelection().isEmpty()); break;
        case cmdCrossfadeLoop: info.setInfo ("Crossfade Loop...", "Blend the end of the selection into the audio before it, so it loops without a click", "Effects", 0);
                            info.setActive (! waveform.getSelection().isEmpty()); break;
        case cmdSelectionToZero: info.setInfo ("Selection to Zero Crossings", "Move both ends of the selection to the nearest zero crossings, up to 5 ms away", "Edit", 0);
                            info.setActive (! waveform.getSelection().isEmpty()); break;
        case cmdNudgeStartEarlier: case cmdNudgeStartLater: case cmdNudgeEndEarlier: case cmdNudgeEndLater:
        {
            const auto start = id == cmdNudgeStartEarlier || id == cmdNudgeStartLater;
            const auto later = id == cmdNudgeStartLater || id == cmdNudgeEndLater;
            info.setInfo (juce::String ("Nudge Selection ") + (start ? "Start" : "End") + (later ? " Later" : " Earlier"),
                          juce::String ("Move the selection's ") + (start ? "start" : "end") + (later ? " later" : " earlier")
                              + " by a pixel's worth of time at this zoom (on to the next zero crossing, with Select at Zero Crossings on)", "Edit", 0);
            // Shift and an arrow move the end that way; with Command too, the start.
            info.addDefaultKeypress (later ? juce::KeyPress::rightKey : juce::KeyPress::leftKey, start ? cmd | shift : shift);
            info.setActive (! waveform.getSelection().isEmpty());
            break;
        }
        case cmdSelectZeroCrossings: info.setInfo ("Select at Zero Crossings", "Land the ends of a selection made with the mouse, and a nudged end, on the nearest zero crossing, up to 5 ms away", "Edit", 0);
                            info.setTicked (selectZeroCrossings); break;
        case cmdDoubleSelection: case cmdHalveSelection:
            info.setInfo (id == cmdDoubleSelection ? "Double Selection" : "Halve Selection",
                          id == cmdDoubleSelection ? "Make the selection twice as long, from its start" : "Make the selection half as long, from its start", "Edit", 0);
            info.addDefaultKeypress (id == cmdDoubleSelection ? juce::KeyPress::upKey : juce::KeyPress::downKey, alt | shift);
            info.setActive (! waveform.getSelection().isEmpty());
            break;
        case cmdCopySelectionTime: info.setInfo ("Copy Selection Time", "Put the selection's start, end and length on the clipboard as text", "Edit", 0);
                            info.setActive (multitrackMode ? ! multitrack.getTimeSelection().isEmpty() : ! waveform.getSelection().isEmpty()); break;
        case cmdLoopRegion: info.setInfo ("Loop Region", "Select the region at the cursor and play it over and over", "Transport", 0);
                        {
                            const auto shown = markersShown();
                            info.setActive (! isRecording() && std::any_of (shown.begin(), shown.end(), [] (const Marker& m) { return m.isRegion(); }));
                            break;
                        }
        case cmdStop:       info.setInfo ("Stop", "Stop playing or recording", "Transport", 0);
                            info.setActive (engine.getPlayer().isPlaying() || isRecording()); break;
        case cmdGoToStart:  info.setInfo ("Go to Start", "Move the cursor to the start", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::homeKey, 0); info.setActive (hasAudio); break;
        case cmdGoToEnd:    info.setInfo ("Go to End", "Move the cursor to the end", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::endKey, 0); info.setActive (hasAudio); break;
        case cmdRecord:     info.setInfo (isRecording() ? "Stop Recording" : "Record",
                                          "Record from the input", "Transport", 0);
                            info.addDefaultKeypress (juce::KeyPress::spaceKey, shift); break;

        case cmdShowFileTabs: info.setInfo ("File Tabs", "Show the open files as tabs over the editor as well as in the Files panel", "Window", 0);
                              info.setTicked (editorPanel != nullptr && editorPanel->areTabsWanted()); break;
        case cmdShowRack:   info.setInfo ("Effects Rack", "Show or hide the effects rack panel", "Window", 0);
                            info.addDefaultKeypress ('e', cmd | alt); info.setTicked (dock->isShown (Panels::rack)); break;
        case cmdShowSpectrum: info.setInfo ("Frequency Analysis", "Show or hide the spectrum of what goes to the outputs", "Window", 0);
                            info.setTicked (dock->isShown (Panels::spectrum)); break;
        case cmdShowTonalBalance: info.setInfo ("Tonal Balance", "Show or hide the long-term spectrum of what goes to the outputs against a target range for a style", "Window", 0);
                            info.setTicked (dock->isShown (Panels::tonalBalance)); break;
        case cmdTimeMinutesSeconds: case cmdTimeDecimalSeconds: case cmdTimeSamples: case cmdTimeSmpte24:
        case cmdTimeSmpte25: case cmdTimeSmpte2997Drop: case cmdTimeSmpte2997: case cmdTimeSmpte30:
        {
            const auto kind = (TimeFormat::Kind) (id - cmdTimeMinutesSeconds);
            const auto name = TimeFormat::names()[(int) kind];
            info.setInfo (name, "Show every time as " + (TimeFormat::isSmpte (kind) ? name + " timecode" : name.toLowerCase()), "View", 0);
            info.setTicked (TimeFormat::get() == kind);
            break;
        }
        case cmdShowStatistics: info.setInfo ("Amplitude Statistics", "Show or hide the numbers of the selection or the file: peaks, RMS, DC offset, bit depth and loudness", "Window", 0);
                            info.setTicked (dock->isShown (Panels::statistics)); break;
        case cmdShowPhase:  info.setInfo ("Phase Meter", "Show or hide the goniometer and correlation of what goes to the outputs", "Window", 0);
                            info.setTicked (dock->isShown (Panels::phase)); break;
        case cmdMaximizeEditor:
            info.setInfo (dock->getMaximised().isEmpty() ? "Maximize Editor" : "Restore All Panels",
                          "Show the editor alone, filling the window, or every panel again", "Window", 0);
            info.addDefaultKeypress ('f', cmd | shift); info.setTicked (dock->getMaximised().isNotEmpty()); break;
        case cmdShowLevels: info.setInfo ("Levels", "Show or hide the panel metering what goes to the outputs", "Window", 0);
                            info.setTicked (dock->isShown (Panels::levels)); break;
        case cmdShowFiles:  info.setInfo ("Files", "Show or hide the panel listing the open files", "Window", 0);
                            info.setTicked (dock->isShown (Panels::files)); break;
        case cmdCloseFile:  info.setInfo ("Close File", "Close the file in the editor, asking about unsaved changes", "File", 0);
                            info.addDefaultKeypress ('w', cmd);
                            info.setActive (! isRecording() && (engine.document.hasAudio() || openFiles.size() > 1)); break;
        case cmdNextFile:   info.setInfo ("Next File", "Show the next open file in the editor", "File", 0);
                            info.setActive (openFiles.size() > 1 && ! isRecording()); break;
        case cmdPreviousFile: info.setInfo ("Previous File", "Show the previous open file in the editor", "File", 0);
                            info.setActive (openFiles.size() > 1 && ! isRecording()); break;
        case cmdShowSelectionPanel: info.setInfo ("Selection/View", "Show or hide the panel with the selection's and the view's start, end and length", "Window", 0);
                            info.setTicked (dock->isShown (Panels::selection)); break;
        case cmdShowSoundLibrary: info.setInfo ("Sound Library", "Show or hide the Sound Library: the sounds in the folders you choose, to search, "
                                                                 "and drag into the multitrack view, the editor or other apps", "Window", 0);
                            info.addDefaultKeypress (juce::KeyPress::F5Key, 0); info.setTicked (soundLibrary != nullptr && soundLibrary->isVisible()); break;
        case cmdSaveWorkspace: info.setInfo ("Save Workspace As...", "Keep the panels as they are now as a workspace of your own", "Window", 0); break;
        case cmdResetWorkspace: info.setInfo ("Reset Workspace", "Put the current workspace's panels back as it was saved", "Window", 0); break;
        case cmdDeleteWorkspace: info.setInfo ("Delete Workspace...", "Delete the current workspace (the built-in ones can't be deleted)", "Window", 0);
                            info.setActive (! workspaces->current().builtIn); break;
        case cmdShowWaveform: info.setInfo ("Waveform", "Show each channel's waveform", "View", 0);
                            info.addDefaultKeypress ('1', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::waveform); break;
        case cmdShowSpectrogram: info.setInfo ("Spectrogram", "Show each channel's frequencies over time", "View", 0);
                            info.addDefaultKeypress ('2', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::spectrogram); break;
        case cmdShowBoth:   info.setInfo ("Waveform and Spectrogram", "Show both, one above the other", "View", 0);
                            info.addDefaultKeypress ('3', cmd);
                            info.setTicked (waveform.getDisplay() == WaveformView::Display::both); break;
        case cmdSpectralAttenuate: info.setInfo ("Attenuate Frequencies...", "Lower the level inside the box, lasso or brush selection in the spectrogram", "Effects", 0);
                            info.setActive (hasSpectral); break;
        case cmdSpectralRemove: info.setInfo ("Remove Frequencies", "Remove what is inside the box, lasso or brush selection in the spectrogram", "Effects", 0);
                            info.addDefaultKeypress (juce::KeyPress::backspaceKey, alt);
                            info.setActive (hasSpectral); break;
        case cmdFileInfo: info.setInfo ("File Info...", "See and change the file's title, artist, album and other details, and its broadcast-wave details", "File", 0);
                          info.setActive (hasAudio); break;
        case cmdBatchProcess: info.setInfo ("Batch Process...", "Put many files through the same steps (the effects rack, sample rate, channels, level) and write each as a new file", "File", 0);
                              info.setActive (backgroundJob == nullptr); break;
        case cmdRecordSequence:
            info.setInfo (sequenceRecorder.isRecording() ? "Stop Recording Sequence..." : "Record Sequence",
                          "Record the effects you apply, with their settings, as a sequence to play on other files or in a batch", "Sequences", 0);
            info.setTicked (sequenceRecorder.isRecording());
            break;
        case cmdSurroundFoldDown:
            info.setInfo ("Play Surround Folded Down", "Play a 5.1 or 7.1 file folded down to stereo on the output pair (centre and surrounds at -3 dB, "
                                                       "LFE left out), and a session's 5.1 or 7.1 master likewise; other files past stereo play their first two channels", "Transport", 0);
            info.setTicked (! engine.getSurroundOutputs());
            break;
        case cmdSurroundOutputs:
            info.setInfo ("Play Surround to Separate Outputs", "Play a file's first two channels on the output pair and the rest on the outputs "
                                                               "after it, one each (5.1: C, LFE, Ls, Rs; 7.1: C, LFE, Lrs, Rrs, Lss, Rss), up to 8 channels; a session's 5.1 or 7.1 master likewise", "Transport", 0);
            info.setTicked (engine.getSurroundOutputs());
            break;
        case cmdSaveChannels: info.setInfo ("Save Channels as Mono Files...", "Write each channel to a mono WAV file of its own (5.1: L, R, C, LFE, Ls, Rs; 7.1: L, R, C, LFE, Lrs, Rrs, Lss, Rss)", "File", 0);
                              info.setActive (hasAudio && doc.getNumChannels() > 1 && backgroundJob == nullptr); break;
        case cmdOpenProject: info.setInfo ("Open Project...", "Open a project: its files with where you were in each, the effects rack and the session", "File", 0);
                             info.setActive (backgroundJob == nullptr); break;
        case cmdSaveProject: info.setInfo ("Save Project", "Save everything open as a project: the files, where you are in each, the effects rack and the session", "File", 0);
                             if (projectFile != juce::File()) info.shortName = "Save Project (" + projectFile.getFileNameWithoutExtension() + ")";
                             info.setActive (backgroundJob == nullptr); break;
        case cmdSaveProjectAs: info.setInfo ("Save Project As...", "Save everything open as a new project", "File", 0);
                               info.setActive (backgroundJob == nullptr); break;
        case cmdKeepProjectHistory: info.setInfo ("Keep Undo History in Projects", "Save each file's undo steps with a project (writing the audio of every version they hold)", "File", 0);
                                    info.setTicked (appProperties.getUserSettings()->getBoolValue (projectHistoryKey, true)); break;
        case cmdShowPitchEditor: info.setInfo ("Pitch Editor", "Show or hide the panel for moving the notes of a voice or solo line by hand, in a file or a session clip", "Window", 0);
                           info.setTicked (dock->isShown (Panels::pitch)); break;
        case cmdShowVideo: info.setInfo ("Video", "Show or hide the panel with the file's or session's video, following the playhead", "Window", 0);
                           info.setTicked (dock->isShown (Panels::video)); break;
        case cmdAttachVideo: info.setInfo ("Attach Video...", "Choose the video this file's sound goes with, to watch in the Video panel and to put the sound back into", "File", 0);
                             info.setActive (hasAudio); break;
        case cmdSaveIntoVideo: info.setInfo ("Save Sound into Video...", "Write a copy of the file's video with this sound in place of its own, the picture copied as it is (needs ffmpeg)", "File", 0);
                               info.setActive (hasAudio && doc.getVideo().existsAsFile() && backgroundJob == nullptr && Ffmpeg::find() != juce::File()); break;
        case cmdVideoStart: info.setInfo ("Video Start...", "Set where in the session the video's first frame plays", "File", 0);
                            info.setActive (false); break;   // a session's
        case cmdDetachVideo: info.setInfo ("Detach Video", "Stop showing a video with the file or session", "File", 0);
                             info.setActive (doc.getVideo() != juce::File()); break;
        case cmdNewFromChannels: info.setInfo ("New File from Mono Files...", "Put several files' channels side by side as one file, such as six mono files as 5.1 or eight as 7.1", "File", 0);
                                 info.setActive (backgroundJob == nullptr); break;
        case cmdPlaySequence: info.setInfo ("Play Sequence...", "Play a recorded sequence on the selection, or the whole file", "Sequences", 0);
                              info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdExportFfmpeg: info.setInfo ("Export with ffmpeg...", "Write the file, or the selection, as Opus, Apple Lossless, WavPack or AC-3 through an installed ffmpeg", "File", 0);
                              info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdExportLossy: info.setInfo ("Export as MP3 or AAC...", "Write the file, or the selection, to an MP3 or AAC (.m4a) file", "File", 0);
                             info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdSpectralHeal: info.setInfo ("Heal", "Replace what is inside the box, lasso or brush selection in the spectrogram with the sound around it", "Effects", 0);
                            info.addDefaultKeypress ('h', cmd | shift);
                            info.setActive (hasSpectral); break;
        case cmdBoxTool:
        case cmdLassoTool:
        case cmdBrushTool:
        {
            const auto t = id == cmdBoxTool ? WaveformView::SpectralTool::box : id == cmdLassoTool ? WaveformView::SpectralTool::lasso
                                                                                                 : WaveformView::SpectralTool::brush;
            info.setInfo (id == cmdBoxTool ? "Box Selection" : id == cmdLassoTool ? "Lasso Selection" : "Brush Selection",
                          id == cmdBoxTool ? "Drag to select time; Option-drag in a spectrogram selects a box of time and frequency"
                          : id == cmdLassoTool ? "Drag in a spectrogram to draw round what to select; Shift adds to it, Option takes from it"
                                               : "Drag in a spectrogram to paint over what to select; Shift adds to it, Option takes from it", "Edit", 0);
            info.addDefaultKeypress (id == cmdBoxTool ? 'e' : id == cmdLassoTool ? 'd' : 'p', 0);
            info.setTicked (waveform.getSpectralTool() == t);
            info.setActive (! multitrackMode);
            break;
        }
        case cmdBrushLarger:
        case cmdBrushSmaller:
            info.setInfo (id == cmdBrushLarger ? "Larger Brush" : "Smaller Brush", "Change the selection brush's size", "Edit", 0);
            info.addDefaultKeypress (id == cmdBrushLarger ? ']' : '[', 0);
            info.setActive (! multitrackMode && waveform.getSpectralTool() == WaveformView::SpectralTool::brush);
            break;
        case cmdBrushLighter:
        case cmdBrushHeavier:
        {
            const auto strength = juce::roundToInt (waveform.getBrushStrength() * 100.0f);
            info.setInfo (id == cmdBrushHeavier ? "Heavier Brush" : "Lighter Brush",
                          juce::String ("Change how much the selection brush selects, in steps of 10 % (now ") + juce::String (strength)
                              + " %): an effect on what only it painted applies by that much", "Edit", 0);
            info.addDefaultKeypress (id == cmdBrushHeavier ? '}' : '{', shift);   // Shift-] and Shift-[, the characters they type
            info.setActive (! multitrackMode && waveform.getSpectralTool() == WaveformView::SpectralTool::brush
                            && (id == cmdBrushHeavier ? strength < 100 : strength > 10));
            break;
        }

        case cmdLoudnessStats: info.setInfo ("Loudness Statistics...", "Measure loudness (EBU R128) and true peak of the selection or file", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdMatchLoudness: info.setInfo ("Match Loudness...", "Set the integrated loudness of the selection or file", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdMeetDeliverySpec: info.setInfo ("Meet Delivery Spec...", "Bring the selection or file to a loudness spec (EBU R128, ATSC A/85 or an online target) "
                                                                         "in one step, with a report of what it measures", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdCaptureNoisePrint: info.setInfo ("Capture Noise Print", "Learn the noise from a selection of noise alone", "Effects", 0);
            info.addDefaultKeypress ('p', juce::ModifierKeys::shiftModifier);
            info.setActive (waveform.getSelection().getLength() >= NoiseReduction::frameSize); break;
        case cmdLevelSpeech: info.setInfo ("Level Speech...", "Even out the level of speech in the selection or file, raising quiet passages and lowering loud ones", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdCentreChannel: info.setInfo ("Centre Channel...", "Remove what is panned to the middle of a stereo file, or keep it alone, in the selection or file", "Effects", 0);
                            info.setActive (hasAudio && engine.document.getNumChannels() == 2 && backgroundJob == nullptr); break;
        case cmdPitchCorrection: info.setInfo ("Pitch Correction...", "Move the pitch of a voice or solo line to the nearest notes of a scale, in the selection or file", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdPhaseCorrection: info.setInfo ("Automatic Phase Correction...", "Find a time offset between the channels of a stereo file and take it out, in the selection or file", "Effects", 0);
                            info.setActive (hasAudio && engine.document.getNumChannels() == 2 && backgroundJob == nullptr); break;
        case cmdDereverb:   info.setInfo ("Reduce Reverb...", "Lower the room's reverberation in the selection or file, keeping the direct sound", "Effects", 0);
                            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdNoiseReduction: info.setInfo ("Noise Reduction...", "Reduce the captured noise in the selection or file", "Effects", 0);
            info.addDefaultKeypress ('p', cmd | juce::ModifierKeys::shiftModifier);
            info.setActive (hasAudio && noisePrint.has_value() && backgroundJob == nullptr); break;
        case cmdDeclip:     info.setInfo ("Repair Clipping...", "Rebuild clipped peaks in the selection, or the whole file, from the audio around them", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdRemoveHum:  info.setInfo ("Remove Hum...", "Take mains hum (50 or 60 Hz and its harmonics) out of the selection, or the whole file", "Effects", 0);
                            info.setActive (hasAudio); break;
        case cmdClickRepair: info.setInfo ("Click and Pop Repair...", "Find clicks and pops in the selection or file and rebuild them", "Effects", 0);
            info.setActive (hasAudio && backgroundJob == nullptr); break;
        case cmdMultitrack: info.setInfo ("Multitrack Session", "Show the multitrack session instead of the file", "View", 0);
                            info.addDefaultKeypress ('4', cmd); info.setTicked (multitrackMode); break;
        case cmdNewFile:    info.setInfo ("New File...", "Start a new file: silence of a chosen length at a chosen sample rate, channels and bit depth, to record, paste or generate into", "File", 0);
                            info.addDefaultKeypress ('n', cmd); info.setActive (! isRecording()); break;
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
        case cmdMixer:      info.setInfo ("Mixer", "A panel with a strip for each track and bus, and the master meter", "Multitrack", 0);
                            info.addDefaultKeypress ('m', cmd | shift); info.setTicked (dock->isShown (Panels::mixer)); break;
        // In the session these size the tracks; with a file, they zoom the waveform vertically.
        case cmdTallerTracks:
            if (multitrackMode)
                info.setInfo ("Taller Tracks", "Make every track's lane taller", "View", 0);
            else
                info.setInfo ("Zoom In Vertically", "Draw the waveform taller, to see quiet audio", "View", 0);

            info.addDefaultKeypress ('=', cmd | alt);
            info.setActive (multitrackMode ? multitrack.getLaneHeight() < MultitrackView::maxLaneHeight
                                           : hasAudio && waveform.getVerticalZoom() < WaveformView::maxVerticalZoom);
            break;
        case cmdShorterTracks:
            if (multitrackMode)
                info.setInfo ("Shorter Tracks", "Make every track's lane shorter, to see more of them", "View", 0);
            else
                info.setInfo ("Zoom Out Vertically", "Draw the waveform less tall, back towards full scale", "View", 0);

            info.addDefaultKeypress ('-', cmd | alt);
            info.setActive (multitrackMode ? multitrack.getLaneHeight() > MultitrackView::minLaneHeight
                                           : hasAudio && waveform.getVerticalZoom() > 1.0);
            break;
        case cmdPreviousEdge:
        case cmdNextEdge:
        {
            const auto forwards = id == cmdNextEdge;
            info.setInfo (forwards ? "Go to Next Edge" : "Go to Previous Edge",
                          forwards ? "Move the cursor to the next clip or selection edge, or the end"
                                   : "Move the cursor to the previous clip or selection edge, or the start", "Transport", 0);
            info.addDefaultKeypress (forwards ? juce::KeyPress::rightKey : juce::KeyPress::leftKey, cmd);
            info.setActive (multitrackMode || hasAudio);
            break;
        }
        case cmdRepeatLast:
        {
            juce::ApplicationCommandInfo last (lastRepeatable);

            if (lastRepeatable != 0)
                describeCommand (lastRepeatable, last);

            info.setInfo (lastRepeatable != 0 ? "Repeat " + last.shortName.trimCharactersAtEnd (".") : juce::String ("Repeat Last Effect"),
                          "Run the last command from the Effects menu again (its dialog opens again, if it has one)", "Edit", 0);
            info.addDefaultKeypress ('r', shift);
            info.setActive (! multitrackMode && lastRepeatable != 0 && (last.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
            break;
        }
        case cmdResetVerticalZoom:
            info.setInfo ("Full Scale Vertically", "Draw the waveform at full scale again (1x)", "View", 0);
            info.addDefaultKeypress ('0', cmd | alt);
            info.setActive (! multitrackMode && waveform.getVerticalZoom() > 1.0);
            break;
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
        case cmdClipList:   info.setInfo ("Clip List", "A panel listing every clip in the session; click one to go to it", "Multitrack", 0);
                            info.setTicked (dock->isShown (Panels::clipList)); break;
        case cmdMuteClip:   info.setInfo ("Mute Clip", "Keep the selected clips in place but silent, or let them be heard again", "Multitrack", 0);
                            info.setTicked (multitrack.areSelectedClipsMuted());
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdClipProperties: info.setInfo ("Clip Properties", "A panel with the selected clip's name, start, length, gain and fades", "Multitrack", 0);
                            info.addDefaultKeypress ('i', cmd | shift); info.setTicked (dock->isShown (Panels::clipProperties)); break;
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
        case cmdSurroundMaster: info.setInfo ("5.1 Master", "Mix the session to 5.1 (L R C LFE Ls Rs) instead of stereo, placing each track "
                                                    "and bus with the surround pad on its mixer strip", "Multitrack", 0);
                                info.setTicked (engine.session.getState()->masterChannels() == 6); info.setActive (multitrackMode && ! isRecording()); break;
        case cmdSevenOneMaster: info.setInfo ("7.1 Master", "Mix the session to 7.1 (L R C LFE Lrs Rrs Lss Rss) instead of stereo, placing each track "
                                                    "and bus with the surround pad on its mixer strip", "Multitrack", 0);
                                info.setTicked (engine.session.getState()->masterChannels() == 8); info.setActive (multitrackMode && ! isRecording()); break;
        case cmdBarsAndBeats: info.setInfo ("Bars and Beats", "Count the ruler in bars and beats of the tempo, and snap to its beats", "Multitrack", 0);
                            info.addDefaultKeypress ('b', 0); info.setTicked (multitrack.isShowingBars()); info.setActive (multitrackMode); break;
        case cmdTempoChange: info.setInfo ("Tempo Change at Cursor...", "Change the tempo or beats in a bar from the bar the cursor is in, or remove a change there", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdTempo:      info.setInfo ("Tempo...", "The session's tempo and beats in a bar", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdTapTempo:   info.setInfo ("Tap Tempo", "Tap in time to set the session's tempo from the taps' spacing", "Multitrack", 0);
                            info.addDefaultKeypress ('t', 0); info.setActive (multitrackMode && ! isRecording()); break;
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
        case cmdNextClip:   info.setInfo ("Select Next Clip", "Select the next clip along the selected clip's track", "Edit", 0);
                            info.addDefaultKeypress (juce::KeyPress::tabKey, 0); info.setActive (multitrackMode); break;
        case cmdPreviousClip: info.setInfo ("Select Previous Clip", "Select the clip before along the selected clip's track", "Edit", 0);
                            info.addDefaultKeypress (juce::KeyPress::tabKey, shift); info.setActive (multitrackMode); break;
        case cmdSelectClipsInRegion: info.setInfo ("Select Clips in Region", "Select the region at the cursor and every clip that plays part of it", "Edit", 0);
                            info.setActive (multitrackMode && std::any_of (engine.session.getState()->markers.begin(), engine.session.getState()->markers.end(),
                                                                           [] (const Marker& m) { return m.isRegion(); })); break;
        case cmdSelectClipsInRange: info.setInfo ("Select Clips in Time Range", "Select every clip that plays part of the time range", "Edit", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdMoveClipsToCursor: info.setInfo ("Move Clips to Cursor", "Move the selected clips together so the earliest starts at the cursor", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdTrimStartToCursor: info.setInfo ("Trim Start to Cursor", "Make the selected clips the cursor is in start at the cursor", "Multitrack", 0);
                            info.addDefaultKeypress ('[', alt); info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdTrimEndToCursor: info.setInfo ("Trim End to Cursor", "Make the selected clips the cursor is in end at the cursor", "Multitrack", 0);
                            info.addDefaultKeypress (']', alt); info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdZeroCrossings: info.setInfo ("Cut at Zero Crossings", "Move splits and dragged clip edges to the nearest zero crossing, up to 5 ms away", "Multitrack", 0);
                            info.setTicked (multitrack.isFindingZeroCrossings()); info.setActive (multitrackMode); break;
        case cmdStretchClip: info.setInfo ("Stretch Clip...", "Make the selected clip longer or shorter without changing its pitch", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0 && backgroundJob == nullptr); break;
        case cmdSaveCopyWithAudio: info.setInfo ("Save a Copy with Audio...", "Save a copy of the session with every audio file it uses in a folder beside it", "File", 0);
                            info.setActive (multitrackMode); break;
        case cmdSaveTrackTemplate: info.setInfo ("Save Track as Template...", "Save the selected track's settings, sends, effects and automation to start new tracks from", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedTrack() >= 0
                                            && ! engine.session.getState()->tracks[(size_t) multitrack.getSelectedTrack()].isFolder); break;
        case cmdAddFromTemplate: info.setInfo ("Add Track from Template...", "Add a track or bus made from a saved template", "Multitrack", 0);
                            info.setActive (multitrackMode && ! isRecording()); break;
        case cmdSessionNotes: info.setInfo ("Session Notes", "A panel of free text kept with the session: lyrics, takes, things to do", "Multitrack", 0);
                            info.setTicked (dock->isShown (Panels::notes)); break;
        case cmdUndoHistory: info.setInfo ("Undo History", "A panel listing the undo steps of the file or the session showing; click one to go back or forward to it", "Edit", 0);
                            info.setTicked (dock->isShown (Panels::history)); break;
        case cmdShiftPitch: info.setInfo ("Shift Clip Pitch...", "Raise or lower the selected clip's pitch without changing its length", "Multitrack", 0);
                            info.setActive (multitrackMode && multitrack.getSelectedClip() != 0 && backgroundJob == nullptr); break;
        case cmdClearRange: info.setInfo ("Clear Range", "Clear the time range on every track, leaving the time empty", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdDeleteTime: info.setInfo ("Delete Time", "Take the time range out of the session, moving what is after it back", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdInsertTime: info.setInfo ("Insert Time", "Put as much empty time as the range is long in at its start, moving what is after it on", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdMarkerList: info.setInfo ("Marker List", "A panel listing the session's markers and regions", "Multitrack", 0);
                            info.setTicked (dock->isShown (Panels::markers)); break;
        case cmdImportMarkers: info.setInfo ("Import Markers from CSV...", "Add the markers and regions from a CSV file written by Export Markers", "File", 0);
                            info.setActive (multitrackMode || hasAudio); break;
        case cmdExportMarkers: info.setInfo ("Export Markers as CSV...", "Write the markers and regions of the session or file to a CSV file", "File", 0);
                            info.setActive (multitrackMode ? ! engine.session.getState()->markers.empty() : ! engine.document.getMarkers().empty()); break;
        case cmdExportClips: info.setInfo ("Export Clip List as CSV...", "Write every clip in the session, with its track, times and file, to a CSV file", "File", 0);
                            info.setActive (multitrackMode && engine.session.getState()->getLength() > 0); break;
        case cmdQuantize:   info.setInfo ("Quantize Clips to Grid", "Move each selected clip so it starts on the nearest grid line", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdCutTime:    info.setInfo ("Cut Time", "Copy the time range from every track, then take it out of the session", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdCopyTime:   info.setInfo ("Copy Time", "Copy the time range from every track: clips, envelopes and markers", "Multitrack", 0);
                            info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
        case cmdDuplicateTime: info.setInfo ("Duplicate Time", "Repeat the time range right after it on every track, moving what follows on", "Multitrack", 0);
                            info.addDefaultKeypress ('d', cmd | alt); info.setActive (multitrackMode && ! multitrack.getTimeSelection().isEmpty()); break;
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
        case cmdGoToTime:   info.setInfo ("Go to Time...", "Move the cursor to a time typed in seconds, minutes and seconds, or bar and beat", "Transport", 0);
                            info.addDefaultKeypress ('j', cmd); info.setActive (multitrackMode || hasAudio); break;
        case cmdTrackAbove: info.setInfo ("Select Track Above", "Select the track above the selected one", "Multitrack", 0);
                            info.addDefaultKeypress (juce::KeyPress::upKey, 0); info.setActive (multitrackMode); break;
        case cmdTrackBelow: info.setInfo ("Select Track Below", "Select the track below the selected one", "Multitrack", 0);
                            info.addDefaultKeypress (juce::KeyPress::downKey, 0); info.setActive (multitrackMode); break;
        case cmdUnsoloAll:  info.setInfo ("Unsolo All", "Clear the solo of every track, bus and folder", "Multitrack", 0);
                            info.setActive (multitrackMode && engine.session.getState()->anySolo()); break;
        case cmdUnmuteAll:  info.setInfo ("Unmute All", "Clear the mute of every track, bus and folder", "Multitrack", 0);
                            info.setActive (multitrackMode && [state = engine.session.getState()]
                                            { return std::any_of (state->tracks.begin(), state->tracks.end(), [] (auto& t) { return t.mute; }); }()); break;
        case cmdRegionFromClips: info.setInfo ("Add Region from Clips", "Add a region from the start of the selected clips to their end", "Markers", 0);
                            info.setActive (multitrackMode && ! multitrack.getSelectedClips().empty()); break;
        case cmdSelectBetweenMarkers: info.setInfo ("Select Between Markers", "Select from the marker at or before the cursor to the next one", "Markers", 0);
                            info.setActive (! doc.getMarkers().empty()); break;
        case cmdMarkersAtIntervals: info.setInfo ("Add Markers at Intervals...", "Put a marker every so many seconds through the selection, or all of it", "Markers", 0);
                            info.setActive (hasAudio); break;
        case cmdRegionsFromMarkers: info.setInfo ("Regions from Markers", "Turn each marker into a region running to the next marker, or the end", "Markers", 0);
                            info.setActive (std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return ! m.isRegion(); })); break;
        case cmdRenameMarkers: info.setInfo ("Rename Markers in Sequence...", "Name every marker and region in time order from a typed name and a number", "Markers", 0);
                            info.setActive (! doc.getMarkers().empty()); break;
        case cmdSelectRegion: info.setInfo ("Select Region at Cursor", "Select the region the cursor is in, the shortest where regions overlap", "Markers", 0);
                            info.setActive (std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(), [] (const Marker& m) { return m.isRegion(); })); break;
        case cmdDeleteAllMarkers: info.setInfo ("Delete All Markers", "Remove every marker and region", "Markers", 0);
                            info.setActive (! doc.getMarkers().empty()); break;
        case cmdMarkersToZero: info.setInfo ("Markers to Zero Crossings", "Move every marker and region end to the nearest zero crossing, up to 5 ms away", "Markers", 0);
                            info.setActive (hasAudio && ! doc.getMarkers().empty()); break;
        case cmdMoveMarkersInSelection: info.setInfo ("Move Markers in Selection...", "Move the markers and regions that start inside the selection by a typed time", "Markers", 0);
                            info.setActive (std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(),
                                                         [sel = waveform.getSelection()] (const Marker& m) { return sel.contains (m.start); })); break;
        case cmdDeleteMarkersInSelection: info.setInfo ("Delete Markers in Selection", "Remove the markers and regions that start inside the selection", "Markers", 0);
                            info.setActive (std::any_of (doc.getMarkers().begin(), doc.getMarkers().end(),
                                                         [sel = waveform.getSelection()] (const Marker& m) { return sel.contains (m.start); })); break;

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
            case cmdPreviousMarker: case cmdNextMarker: case cmdSelectBetweenMarkers: case cmdDeleteAllMarkers:
                            info.setActive (! session.getState()->markers.empty()); break;
            case cmdMarkersAtIntervals: info.setActive (hasClips); break;
            case cmdRegionsFromMarkers: info.setActive (std::any_of (session.getState()->markers.begin(), session.getState()->markers.end(),
                                                                     [] (const Marker& m) { return ! m.isRegion(); })); break;
            case cmdRenameMarkers: info.setActive (! session.getState()->markers.empty()); break;
            case cmdDeleteMarkersInSelection: case cmdMoveMarkersInSelection:
                            info.setActive (std::any_of (session.getState()->markers.begin(), session.getState()->markers.end(),
                                                         [sel = multitrack.getTimeSelection()] (const Marker& m) { return sel.contains (m.start); })); break;
            case cmdFitRegions: info.shortName = "Fit Regions to Clips"; info.description = "Shrink every region to the clips inside it";
                            info.setActive (std::any_of (session.getState()->markers.begin(), session.getState()->markers.end(),
                                                         [] (const Marker& m) { return m.isRegion(); })); break;
            case cmdSelectRegion: info.setActive (std::any_of (session.getState()->markers.begin(), session.getState()->markers.end(),
                                                               [] (const Marker& m) { return m.isRegion(); })); break;
            case cmdAttachVideo: info.description = "Choose the video this session goes with, to watch in the Video panel and to put the mix into";
                                 info.setActive (true); break;
            case cmdSaveIntoVideo: info.shortName = "Save Mix into Video...";
                                   info.description = "Write a copy of the session's video with the mix from the video's start in place of its sound, the picture copied as it is (needs ffmpeg)";
                                   info.setActive (session.getState()->video.existsAsFile() && backgroundJob == nullptr && Ffmpeg::find() != juce::File()); break;
            case cmdVideoStart: info.setActive (session.getState()->video != juce::File()); break;
            case cmdDetachVideo: info.setActive (session.getState()->video != juce::File()); break;
            default:        if (isEditorOnly (id)) info.setActive (false); break;
        }
    }

    // While recording, the document must stay as it is so the take lands where it was started.
    if (isRecording() && id != cmdRecord && id != cmdPlayPause && id != cmdAddMarker)
        info.setActive (false);
}

bool MainComponent::perform (const InvocationInfo& info)
{
    // The last command from the Effects menu, for Repeat.
    if (info.commandID != cmdRepeatLast && ! multitrackMode)
    {
        juce::ApplicationCommandInfo described (info.commandID);
        describeCommand (info.commandID, described);

        if (described.categoryName == "Effects")
            lastRepeatable = info.commandID;
    }

    switch (info.commandID)
    {
        case cmdOpen:       chooseFile(); break;
        case cmdSave:       if (multitrackMode) saveSession(); else save(); break;
        case cmdSaveAs:     if (multitrackMode) saveSessionAs(); else saveAs(); break;
        case cmdUndo:       if (multitrackMode) engine.session.undo(); else if (! waveform.undoStroke()) engine.document.undo(); break;
        case cmdRedo:       if (multitrackMode) engine.session.redo(); else engine.document.redo(); break;
        case cmdCut:        if (multitrackMode) multitrack.cutSelectedClip(); else cut(); break;
        case cmdCopy:       if (multitrackMode) { multitrack.copySelectedClip(); commands.commandStatusChanged(); } else copy(); break;
        case cmdPaste:      if (multitrackMode) multitrack.pasteClip(); else paste (PasteMode::insert); break;
        case cmdPasteMix:   paste (PasteMode::mix); break;
        case cmdPasteFill:  paste (PasteMode::fill); break;
        case cmdSaveSelection: saveSelectionAs(); break;
        case cmdSaveRegions: chooseFileRegionsFolder (false); break;
        case cmdSavePieces: chooseFileRegionsFolder (true); break;
        case cmdDuplicateClip: multitrack.duplicateSelectedClip(); break;
        case cmdDelete:     if (multitrackMode) multitrack.deleteSelectedClip(); else deleteSelection(); break;   // a spectral selection: its frequencies
        case cmdSelectAll:  if (multitrackMode) multitrack.selectAllClips(); else waveform.selectAll(); break;
        case cmdCrop:       cropToSelection(); break;
        case cmdGain:       applyGain(); break;
        case cmdNormalize:  applyNormalize(); break;
        case cmdFadeIn:     applyFade (false); break;
        case cmdFadeOut:    applyFade (true); break;
        case cmdFadeEdges:  showFadeEdges (false); break;
        case cmdGainRamp:   showGainRamp(); break;
        case cmdTremolo:    showTremolo(); break;
        case cmdEcho:       showEcho(); break;
        case cmdEqBand:     showEqBand(); break;
        case cmdParametricEq: toggleParametricEq(); break;
        case cmdCompressor: showCompressor(); break;
        case cmdLimiter:    showLimiter(); break;
        case cmdMultiband:  showMultiband(); break;
        case cmdDeEsser:    showDeEsser(); break;
        case cmdDynamicEq:  showDynamicEq(); break;
        case cmdMultibandWidth: showMultibandWidth(); break;
        case cmdExciter:    showExciter(); break;
        case cmdMaximizer:  showMaximizer(); break;
        case cmdTransientShaper: showTransientShaper(); break;
        case cmdDither:     showDither(); break;
        case cmdDialogueIsolation: showDialogueIsolation(); break;
        case cmdAdaptiveDenoise: showAdaptiveDenoise(); break;
        case cmdMouthDeClick: showMouthDeClick (false); break;
        case cmdDeCrackle:  showMouthDeClick (true); break;
        case cmdDePlosive:  showDePlosive(); break;
        case cmdBreathControl: showBreathControl(); break;
        case cmdSpill:      showSpill(); break;
        case cmdWowFlutter: showWowFlutter(); break;
        case cmdSpectralRecovery: showSpectralRecovery(); break;
        case cmdRebalance:  showRebalance(); break;
        case cmdCaptureAmbience: captureAmbience(); break;
        case cmdDialogueContour: showDialogueContour(); break;
        case cmdAzimuth:    showAzimuth(); break;
        case cmdCodecPreview: showCodecPreview(); break;
        case cmdAmbienceMatch: showAmbienceMatch(); break;
        case cmdCaptureMatchReference: captureMatchReference(); break;
        case cmdCaptureMatchReferenceFromFile: captureMatchReferenceFromFile(); break;
        case cmdMatchEq:    showMatchEq(); break;
        case cmdChorus:     showSweptDelay (false); break;
        case cmdFlanger:    showSweptDelay (true); break;
        case cmdPhaser:     showPhaser(); break;
        case cmdReverb:     showReverb(); break;
        case cmdConvolutionReverb: showConvolutionReverb(); break;
        case cmdGraphicEq10: showGraphicEq (10); break;
        case cmdGraphicEq31: showGraphicEq (31); break;
        case cmdAbout: showAbout(); break;
        case cmdFadeRegions: showFadeEdges (true); break;
        case cmdNormalizeRegions:
            askForDecibels ("Normalize Each Region", "Peak level (dBFS)", -0.1f, -48.0, 0.0, "spectrablade:normalize-regions", [this] (float db)
            {
                auto skipped = 0;

                if (auto next = AudioEdits::normalizeRegions (*engine.document.getSnapshot(), engine.document.getMarkers(), juce::jmin (db, 0.0f), skipped))
                    applyEdit (std::move (next), "Normalize Each Region", waveform.getSelection());

                if (skipped > 0)
                    showError ("Normalize Each Region", juce::String (skipped) + (skipped == 1 ? " region was" : " regions were")
                                                            + " left as they were: silent, or overlapping a region before.");
            },
            [markers = engine.document.getMarkers()] (float db)
            {
                // Every region of the file, heard over the selection (or the whole file).
                return std::optional<EffectPreview::Request> ({ "normalizeRegions " + juce::String (db, 3),
                    [markers, db] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double, const std::function<bool (float)>&)
                    {
                        auto skipped = 0;
                        return EffectPreview::Rendered { AudioEdits::normalizeRegions (*audio, markers, juce::jmin (db, 0.0f), skipped), start, end, {} };
                    }, true });
            });
            break;
        case cmdSilenceOutside: applySimpleEffect ("Silence Outside Selection", &AudioEdits::silenceOutside, true); break;
        case cmdPanRamp:    showPanRamp(); break;
        case cmdStereoWidth: showStereoWidth(); break;
        case cmdCropMargins: showCropMargins(); break;
        case cmdSilence:    applySimpleEffect ("Silence", &AudioEdits::silence, true); break;
        case cmdInvert:     runStep ({ "invert", {}, nullptr }); break;
        case cmdReverse:    runStep ({ "reverse", {}, nullptr }); break;
        case cmdStretch:    askToStretch(); break;
        case cmdPitch:      askToShiftPitch(); break;
        case cmdExtremeStretch: askToExtremeStretch(); break;
        case cmdReverseRegions:
        {
            auto skipped = 0;

            if (auto next = AudioEdits::reverseRegions (*engine.document.getSnapshot(), engine.document.getMarkers(), skipped))
                applyEdit (std::move (next), "Reverse Each Region", waveform.getSelection());

            if (skipped > 0)
                showError ("Reverse Each Region", juce::String (skipped) + (skipped == 1 ? " region was" : " regions were")
                                                      + " left as they were: one frame long, or overlapping a region before.");
            break;
        }
        case cmdRemoveDc:   runStep ({ "removeDc", {}, nullptr }); break;
        case cmdBalance:    showBalance(); break;
        case cmdSilenceLeft: applySimpleEffect ("Silence Left Channel", [] (const AudioSnapshot& s, juce::int64 a, juce::int64 b)
                                                   { return AudioEdits::silenceChannel (s, a, b, 0); }, true); break;
        case cmdSilenceRight: applySimpleEffect ("Silence Right Channel", [] (const AudioSnapshot& s, juce::int64 a, juce::int64 b)
                                                    { return AudioEdits::silenceChannel (s, a, b, 1); }, true); break;
        case cmdLeftToRight: runStep ({ "leftToRight", {}, nullptr }); break;
        case cmdRightToLeft: runStep ({ "rightToLeft", {}, nullptr }); break;
        case cmdInvertLeft: runStep ({ "invertLeft", {}, nullptr }); break;
        case cmdInvertRight: runStep ({ "invertRight", {}, nullptr }); break;
        case cmdSwapChannels: runStep ({ "swapChannels", {}, nullptr }); break;
        case cmdInsertSilence: showInsertSilence(); break;
        case cmdGenerateTone: showGenerateTone(); break;
        case cmdGenerateNoise: showGenerateNoise(); break;
        case cmdGenerateTones: showGenerateTones(); break;
        case cmdSelectAudible: selectAudibleSpan(); break;
        case cmdFindClipping: findNextClipping(); break;
        case cmdMarkClipping: addMarkersAtClipping(); break;
        case cmdRegionsOverSounds: askForSilence ("Add Regions over Sounds", [this] (juce::int64 frames) { addRegionsOverSounds (frames); }); break;
        case cmdNextSound:  selectNextSound(); break;
        case cmdPreviousSound: selectPreviousSound(); break;
        case cmdFindSilence: askForSilence ("Find Next Silence", [this] (juce::int64 frames) { findNextSilence (frames); }); break;
        case cmdFitRegions: fitRegionsToSound(); break;
        case cmdRegionsOverClips:
        {
            auto next = *engine.session.getState();

            if (const auto added = SessionEdits::regionsOverClips (next); added > 0)
                engine.session.apply (std::move (next), "Add " + juce::String (added) + (added == 1 ? " Region" : " Regions"));

            break;
        }
        case cmdRepair:    repairSelection(); break;
        case cmdNoiseGate:
            askForSilence ("Noise Gate", [this] (juce::int64 frames) { noiseGate (frames); }, [] (float db, juce::int64 frames)
            {
                return std::optional<EffectPreview::Request> ({ "gate " + juce::String (db, 3) + " " + juce::String (frames),
                    [db, frames] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                    {
                        return EffectPreview::Rendered { AudioEdits::gateQuiet (*audio, start, end, juce::Decibels::decibelsToGain (db, -200.0f), frames,
                                                                                juce::roundToInt (sampleRate * gateFadeSeconds)),
                                                         start, end, {} };
                    } });
            });
            break;
        case cmdShortenPauses: askForSilence ("Shorten Pauses", [this] (juce::int64 frames) { shortenPauses (frames); }); break;
        case cmdMarkersAtSilences: askForSilence ("Add Markers at Silences", [this] (juce::int64 frames) { addMarkersAtSilences (frames); }); break;
        case cmdSetSelection: showSetSelection(); break;
        case cmdMixToMono:  convertChannels (1); break;
        case cmdMakeStereo: convertChannels (2); break;
        case cmdTrimSilence: askForDecibels ("Trim Silence", "Threshold (dB)", silenceThresholdDb, -120.0, 0.0, "spectrablade:trim-silence", [this] (float db) { trimSilence (db); }); break;
        case cmdApplyRack:  applyRackToSelection(); break;
        case cmdPlayPause:  togglePlay(); break;
        case cmdLoopPlayback:
            loopPlayback = ! loopPlayback;
            appProperties.getUserSettings()->setValue (loopPlaybackKey, loopPlayback);
            updatePlaybackLoop();
            commands.commandStatusChanged();
            break;
        case cmdLoopRegion: loopRegion(); break;
        case cmdStepSelectionLater: case cmdStepSelectionEarlier:
        {
            const auto stepped = AudioEdits::stepSelection (waveform.getSelection(), engine.document.getNumFrames(), info.commandID == cmdStepSelectionLater);
            waveform.setSelection (stepped);
            waveform.showFrame (stepped.getStart());
            break;
        }
        case cmdCrossfadeLoop: showCrossfadeLoop(); break;
        case cmdRepeatSelection: showRepeatSelection(); break;
        case cmdSelectionToZero:
            waveform.setSelection (AudioEdits::toZeroCrossings (engine.document.getSnapshot(), waveform.getSelection(), zeroCrossingReach()));
            break;
        case cmdNudgeStartEarlier: case cmdNudgeStartLater: case cmdNudgeEndEarlier: case cmdNudgeEndLater:
            nudgeSelection (info.commandID == cmdNudgeStartEarlier || info.commandID == cmdNudgeStartLater,
                            info.commandID == cmdNudgeStartLater || info.commandID == cmdNudgeEndLater);
            break;
        case cmdSelectZeroCrossings:
            selectZeroCrossings = ! selectZeroCrossings;
            appProperties.getUserSettings()->setValue (selectZeroCrossingsKey, selectZeroCrossings);
            commands.commandStatusChanged();
            break;
        case cmdDoubleSelection: case cmdHalveSelection:
            waveform.setSelection (AudioEdits::resizeSelection (waveform.getSelection(), engine.document.getNumFrames(), info.commandID == cmdDoubleSelection));
            break;
        case cmdCopySelectionTime:
            juce::SystemClipboard::copyTextToClipboard (multitrackMode ? TimeEntry::describe (multitrack.getTimeSelection(), engine.session.getSampleRate())
                                                                       : TimeEntry::describe (waveform.getSelection(), engine.document.getSampleRate()));
            break;
        case cmdNextRegion: case cmdPreviousRegion: selectAdjacentRegion (info.commandID == cmdNextRegion); break;
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
        case cmdClipProperties: dock->toggle (Panels::clipProperties); break;
        case cmdClipList:   dock->toggle (Panels::clipList); break;
        case cmdRepeatClip: multitrack.repeatSelectedClip(); break;
        case cmdRecordingOptions: showRecordingOptions(); break;
        case cmdResetVerticalZoom: waveform.setVerticalZoom (1.0); break;
        case cmdRepeatLast: commands.invokeDirectly (lastRepeatable, true); break;
        case cmdPreviousEdge:
        case cmdNextEdge:
        {
            const auto forwards = info.commandID == cmdNextEdge;

            if (multitrackMode)
            {
                if (const auto to = Navigation::nextEdge (Navigation::sessionEdges (*engine.session.getState(), multitrack.getTimeSelection()),
                                                          multitrack.getCursor(), forwards))
                    multitrack.showFrame (*to);
            }
            else if (const auto to = Navigation::nextEdge (Navigation::fileEdges (engine.document.getNumFrames(), waveform.getSelection()),
                                                           waveform.getCursor(), forwards))
            {
                waveform.showFrame (*to);
            }

            break;
        }
        case cmdTallerTracks:
        case cmdShorterTracks:
            if (! multitrackMode)
            {
                if (info.commandID == cmdTallerTracks)
                    waveform.zoomInVertically();
                else
                    waveform.zoomOutVertically();

                break;
            }

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
        case cmdStop:       stopTransport(); break;
        case cmdShowRack:   dock->toggle (Panels::rack); break;
        case cmdShowFileTabs:
            editorPanel->setTabsWanted (! editorPanel->areTabsWanted());
            appProperties.getUserSettings()->setValue (fileTabsKey, editorPanel->areTabsWanted());
            commands.commandStatusChanged();
            break;
        case cmdShowFiles:  dock->toggle (Panels::files); break;
        case cmdShowLevels: dock->toggle (Panels::levels); break;
        case cmdShowSpectrum: dock->toggle (Panels::spectrum); break;
        case cmdShowPhase:  dock->toggle (Panels::phase); break;
        case cmdShowTonalBalance: dock->toggle (Panels::tonalBalance); break;
        case cmdShowStatistics: dock->toggle (Panels::statistics); break;
        case cmdTimeMinutesSeconds: case cmdTimeDecimalSeconds: case cmdTimeSamples: case cmdTimeSmpte24:
        case cmdTimeSmpte25: case cmdTimeSmpte2997Drop: case cmdTimeSmpte2997: case cmdTimeSmpte30:
            setTimeFormat ((TimeFormat::Kind) (info.commandID - cmdTimeMinutesSeconds));
            break;
        case cmdMaximizeEditor:
            if (dock->getMaximised().isEmpty())
                dock->maximise (Panels::editor);
            else
                dock->restore();
            break;
        case cmdCloseFile:  closeFile (openFiles.getActiveIndex()); break;
        case cmdNextFile:   switchToFile ((openFiles.getActiveIndex() + 1) % openFiles.size()); break;
        case cmdPreviousFile: switchToFile ((openFiles.getActiveIndex() + openFiles.size() - 1) % openFiles.size()); break;
        case cmdShowSelectionPanel: dock->toggle (Panels::selection); break;
        case cmdShowSoundLibrary: toggleSoundLibrary(); break;
        case cmdSaveWorkspace: askToSaveWorkspace(); break;
        case cmdResetWorkspace: resetWorkspace(); break;
        case cmdDeleteWorkspace: askToDeleteWorkspace(); break;
        case cmdAddMarker:  addMarker(); break;
        case cmdConvertRate: chooseSampleRate(); break;
        case cmdShowWaveform: setMultitrack (false); setDisplay (WaveformView::Display::waveform); break;
        case cmdShowSpectrogram: setMultitrack (false); setDisplay (WaveformView::Display::spectrogram); break;
        case cmdShowBoth:   setMultitrack (false); setDisplay (WaveformView::Display::both); break;
        case cmdMultitrack: setMultitrack (! multitrackMode); break;
        case cmdNewFile:    askToNewFile(); break;
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
        case cmdSelectClipsInRange: multitrack.selectClipsInRange(); break;
        case cmdSelectClipsInRegion: multitrack.selectClipsInRegion(); break;
        case cmdNextClip: multitrack.selectAdjacentClip (true); break;
        case cmdPreviousClip: multitrack.selectAdjacentClip (false); break;
        case cmdMoveClipsToCursor: multitrack.moveSelectedClipsToCursor(); break;
        case cmdTrimStartToCursor: multitrack.trimSelectedClipsToCursor (true); break;
        case cmdTrimEndToCursor: multitrack.trimSelectedClipsToCursor (false); break;
        case cmdCloseGaps:  multitrack.closeGapsOnSelectedTrack(); break;
        case cmdClearRange: multitrack.clearTimeSelection(); break;
        case cmdDeleteTime: multitrack.deleteTimeSelection(); break;
        case cmdInsertTime: multitrack.insertTimeAtSelection(); break;
        case cmdCutTime:    multitrack.cutTimeSelection(); break;
        case cmdMarkerList: dock->toggle (Panels::markers); break;
        case cmdQuantize:   multitrack.quantizeSelectedClips(); break;
        case cmdExportMarkers: chooseMarkersCsv(); break;
        case cmdImportMarkers: chooseMarkersToImport(); break;
        case cmdExportClips: chooseClipsCsv(); break;
        case cmdCopyTime:   multitrack.copyTimeSelection(); break;
        case cmdPasteTime:  multitrack.pasteTime(); break;
        case cmdDuplicateTime: multitrack.duplicateTimeSelection(); break;
        case cmdStretchClip: askToStretchSelectedClip(); break;
        case cmdShiftPitch: askToShiftSelectedClip(); break;
        case cmdUndoHistory: dock->toggle (Panels::history); break;
        case cmdSessionNotes: dock->toggle (Panels::notes); break;
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
        case cmdTapTempo:
            tapSessionTempo (engine.session, tapper, juce::Time::getMillisecondCounterHiRes() / 1000.0);
            break;
        case cmdTempoChange:
            showTempoDialog (true);
            break;
        case cmdSurroundMaster:
        case cmdSevenOneMaster:
        {
            // Each turns its own on, or back to stereo when it is on already.
            auto next = *engine.session.getState();
            const auto wanted = info.commandID == cmdSevenOneMaster ? 8 : 6;
            const auto channels = next.masterChannels() == wanted ? 2 : wanted;

            if (SessionEdits::setMasterChannels (next, channels))
                engine.session.apply (std::move (next), channels == 8 ? "7.1 Master" : channels == 6 ? "5.1 Master" : "Stereo Master");

            break;
        }
        case cmdBarsAndBeats:
            multitrack.setShowBars (! multitrack.isShowingBars());
            commands.commandStatusChanged();
            break;
        case cmdMixer:      dock->toggle (Panels::mixer); break;
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
            askForDecibels ("Attenuate Frequencies", "Change (dB)", -12.0f, -60.0, 24.0, "spectrablade:attenuate-frequencies", [this] (float db) { applySpectral (db); },
                            [sel = waveform.getSelection(), band = waveform.getSpectralBand(), shape = waveform.getSpectralShape()] (float db)
                            {
                                // What is drawn, or the selection's band, heard over the selection.
                                return std::optional<EffectPreview::Request> ({ "attenuate " + juce::String (db, 3),
                                    [sel, band, shape, db] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                                    {
                                        if (! shape.isEmpty())
                                            return EffectPreview::Rendered { SpectralEdit::changeShape (*audio, shape, sampleRate, db), start, end, {} };

                                        if (! sel.isEmpty() && ! band.isEmpty())
                                            return EffectPreview::Rendered { SpectralEdit::changeBand (*audio, sel.getStart(), sel.getEnd(), band, sampleRate, db), start, end, {} };

                                        return EffectPreview::Rendered { nullptr, start, end, "Nothing is drawn or selected in the spectrogram." };
                                    }, true });
                            });
            break;
        case cmdSpectralRemove: applySpectral (-std::numeric_limits<float>::infinity()); break;
        case cmdSpectralHeal: healSpectral(); break;
        case cmdExportLossy: exportLossy(); break;
        case cmdExportFfmpeg: exportWithFfmpeg(); break;
        case cmdFileInfo: showFileInfo(); break;
        case cmdBatchProcess: batchProcess(); break;
        case cmdRecordSequence: toggleSequenceRecording(); break;
        case cmdPlaySequence: choosePlaySequence(); break;
        case cmdSurroundFoldDown: case cmdSurroundOutputs:
            engine.setSurroundOutputs (info.commandID == cmdSurroundOutputs);
            appProperties.getUserSettings()->setValue ("surroundOutputs", engine.getSurroundOutputs());
            commands.commandStatusChanged();
            break;
        case cmdSaveChannels: saveChannels(); break;
        case cmdNewFromChannels: newFileFromChannels(); break;
        case cmdShowVideo: dock->toggle (Panels::video); break;
        case cmdShowPitchEditor: dock->toggle (Panels::pitch); break;
        case cmdOpenProject: chooseProject(); break;
        case cmdSaveProject: saveProject (false); break;
        case cmdSaveProjectAs: saveProject (true); break;
        case cmdKeepProjectHistory:
            appProperties.getUserSettings()->setValue (projectHistoryKey, ! appProperties.getUserSettings()->getBoolValue (projectHistoryKey, true));
            commands.commandStatusChanged();
            break;
        case cmdAttachVideo: attachVideo(); break;
        case cmdSaveIntoVideo: saveIntoVideo(); break;
        case cmdVideoStart: askForVideoStart(); break;
        case cmdDetachVideo: detachVideo(); break;
        case cmdBoxTool:    waveform.setSpectralTool (WaveformView::SpectralTool::box); commands.commandStatusChanged(); break;
        case cmdLassoTool:  waveform.setSpectralTool (WaveformView::SpectralTool::lasso); commands.commandStatusChanged(); break;
        case cmdBrushTool:  waveform.setSpectralTool (WaveformView::SpectralTool::brush); commands.commandStatusChanged(); break;
        case cmdBrushLarger: waveform.setBrushRadius (waveform.getBrushRadius() * 1.25f); break;
        case cmdBrushSmaller: waveform.setBrushRadius (waveform.getBrushRadius() / 1.25f); break;
        case cmdBrushLighter: waveform.setBrushStrength (waveform.getBrushStrength() - WaveformView::brushStrengthStep); commands.commandStatusChanged(); break;
        case cmdBrushHeavier: waveform.setBrushStrength (waveform.getBrushStrength() + WaveformView::brushStrengthStep); commands.commandStatusChanged(); break;
        case cmdLoudnessStats: showLoudness(); break;
        case cmdMatchLoudness: askForLoudnessTarget(); break;
        case cmdMeetDeliverySpec: askForDeliverySpec(); break;
        case cmdCaptureNoisePrint: captureNoisePrint(); break;
        case cmdNoiseReduction: askForNoiseReduction(); break;
        case cmdDereverb:   askToReduceReverb(); break;
        case cmdCentreChannel: askForCentreChannel(); break;
        case cmdPhaseCorrection: askForPhaseCorrection(); break;
        case cmdPitchCorrection: askForPitchCorrection(); break;
        case cmdLevelSpeech: askToLevelSpeech(); break;
        case cmdClickRepair: askForClickRepair(); break;
        case cmdRemoveHum:  showRemoveHum(); break;
        case cmdDeclip:     showDeclip(); break;
        case cmdPreviousMarker: goToMarker (false); break;
        case cmdNextMarker: goToMarker (true); break;
        case cmdSelectBetweenMarkers: selectBetweenMarkers(); break;
        case cmdSelectRegion: selectRegionAtCursor(); break;
        case cmdMarkersAtIntervals: showMarkersAtIntervals(); break;
        case cmdRegionsFromMarkers: regionsFromMarkers(); break;
        case cmdRenameMarkers: showRenameMarkers(); break;
        case cmdRegionFromClips: multitrack.addRegionFromSelectedClips(); break;
        case cmdUnsoloAll:  multitrack.clearAllSolos(); break;
        case cmdTrackAbove: multitrack.selectAdjacentTrack (false); break;
        case cmdTrackBelow: multitrack.selectAdjacentTrack (true); break;
        case cmdUnmuteAll:  multitrack.clearAllMutes(); break;
        case cmdGoToTime: showGoToTime(); break;
        case cmdMoveMarkersInSelection: showMoveMarkers(); break;
        case cmdMarkersToZero:
        {
            auto markers = engine.document.getMarkers();
            const auto reach = (juce::int64) std::llround (MultitrackView::zeroCrossingReach * engine.document.getSampleRate());

            if (const auto moved = AudioEdits::markersToZeroCrossings (engine.document.getSnapshot(), markers, reach); moved > 0)
                engine.document.setMarkers (std::move (markers), "Markers to Zero Crossings");
            else
                showError ("Markers to Zero Crossings", "Every marker is already on a zero crossing, or has none within 5 ms.");

            break;
        }
        case cmdDeleteMarkersInSelection:
            if (multitrackMode)
            {
                auto next = *engine.session.getState();

                if (const auto gone = MarkerEdits::removeStartingIn (next.markers, multitrack.getTimeSelection()); gone > 0)
                    engine.session.apply (std::move (next), "Delete " + juce::String (gone) + (gone == 1 ? " Marker" : " Markers"));
            }
            else
            {
                auto markers = engine.document.getMarkers();

                if (const auto gone = MarkerEdits::removeStartingIn (markers, waveform.getSelection()); gone > 0)
                    engine.document.setMarkers (std::move (markers), "Delete " + juce::String (gone) + (gone == 1 ? " Marker" : " Markers"));
            }
            break;
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

    // A submenu of commands grouped by what they do, so a long menu stays short (the Effects menu's
    // Fades group carries the Fade Shape submenu too).
    const auto group = [this, &menu] (const juce::String& name, std::initializer_list<int> ids, const juce::PopupMenu* fadeShapes = nullptr)
    {
        juce::PopupMenu sub;

        for (auto id : ids)
        {
            if (id == 0)
                sub.addSeparator();
            else
                sub.addCommandItem (&commands, id);
        }

        if (fadeShapes != nullptr)
            sub.addSubMenu ("Fade Shape", *fadeShapes, ! multitrackMode);

        menu.addSubMenu (name, sub);
    };

    switch (index)
    {
        case 0:
            add ({ cmdNewFile, cmdOpen });
            menu.addSubMenu ("Open Recent", RecentFiles::createMenu (recentFiles, recentBaseId, [this] (const juce::File& f) { return isThere (f); }), recentFiles.getNumFiles() > 0 && ! isRecording());
            add ({ cmdCloseFile, cmdNextFile, cmdPreviousFile, 0, cmdSave, cmdSaveAs, cmdSaveSelection, cmdSaveRegions, cmdSavePieces, cmdSaveChannels, cmdExportLossy, cmdExportFfmpeg, 0, cmdAttachVideo, cmdVideoStart, cmdDetachVideo, cmdSaveIntoVideo, 0, cmdBatchProcess, cmdNewFromChannels, 0, cmdFileInfo, 0, cmdOpenProject, cmdSaveProject, cmdSaveProjectAs, cmdKeepProjectHistory, 0, cmdNewSession, cmdOpenSession, cmdSaveCopyWithAudio, 0, cmdExportMarkers, cmdImportMarkers, cmdExportClips }); break;
        case 1: add ({ cmdUndo, cmdRedo, cmdUndoHistory, cmdRepeatLast, 0, cmdCut, cmdCopy, cmdPaste, cmdPasteMix, cmdPasteFill, cmdDelete, cmdRepeatSelection, 0, cmdSelectAll, cmdSelectAudible, cmdSetSelection, cmdStepSelectionEarlier, cmdStepSelectionLater, cmdDoubleSelection, cmdHalveSelection, cmdNudgeStartEarlier, cmdNudgeStartLater, cmdNudgeEndEarlier, cmdNudgeEndLater, cmdSelectionToZero, cmdSelectZeroCrossings, cmdCopySelectionTime, cmdFindClipping, cmdFindSilence, cmdNextSound, cmdPreviousSound, cmdSelectTrackClips, cmdSelectClipsAfter, cmdSelectClipsInRange, cmdSelectClipsInRegion, cmdNextClip, cmdPreviousClip, cmdCrop, cmdCropMargins, 0, cmdBoxTool, cmdLassoTool, cmdBrushTool, cmdBrushLarger, cmdBrushSmaller, cmdBrushLighter, cmdBrushHeavier, 0, cmdConvertRate }); break;
        case 2:
        {
            juce::PopupMenu shapes;

            for (size_t i = 0; i < FadeShapes::all.size(); ++i)
                shapes.addItem (fadeShapeBaseId + (int) i, FadeShapes::displayName (FadeShapes::all[i]), true, FadeShapes::all[i] == fadeShape);

            group ("Level and Loudness", { cmdGain, cmdNormalize, cmdNormalizeRegions, cmdMatchLoudness, cmdMeetDeliverySpec, cmdLevelSpeech, 0, cmdLoudnessStats , 0, cmdDither, cmdCodecPreview });
            group ("Fades", { cmdFadeIn, cmdFadeOut, cmdFadeEdges, cmdFadeRegions, cmdGainRamp, 0 }, &shapes);
            group ("EQ", { cmdEqBand, cmdParametricEq, cmdDynamicEq, cmdGraphicEq10, cmdGraphicEq31, 0, cmdCaptureMatchReference, cmdCaptureMatchReferenceFromFile, cmdMatchEq });
            group ("Dynamics", { cmdCompressor, cmdLimiter, cmdMaximizer, cmdMultiband, cmdTransientShaper, cmdDeEsser, cmdNoiseGate });
            group ("Saturation", { cmdExciter });
            group ("Modulation and Delay", { cmdTremolo, cmdEcho, cmdChorus, cmdFlanger, cmdPhaser });
            group ("Reverb", { cmdReverb, cmdConvolutionReverb });
            group ("Time and Pitch", { cmdDialogueContour, 0,  cmdStretch, cmdExtremeStretch, cmdPitch, cmdPitchCorrection, 0, cmdReverse, cmdReverseRegions, cmdCrossfadeLoop });
            group ("Stereo and Channels", { cmdBalance, cmdPanRamp, cmdStereoWidth, cmdMultibandWidth, cmdCentreChannel, cmdPhaseCorrection, cmdAzimuth, 0, cmdSwapChannels, cmdLeftToRight, cmdRightToLeft,
                                            cmdMixToMono, cmdMakeStereo, 0, cmdInvert, cmdInvertLeft, cmdInvertRight });
            group ("Silence", { cmdSilence, cmdSilenceOutside, cmdSilenceLeft, cmdSilenceRight, cmdInsertSilence, 0, cmdTrimSilence, cmdShortenPauses });
            group ("Generate", { cmdGenerateTone, cmdGenerateTones, cmdGenerateNoise });
            menu.addSeparator();
            group ("Noise Reduction and Repair", { cmdCaptureNoisePrint, cmdNoiseReduction, cmdAdaptiveDenoise, cmdDialogueIsolation, 0, cmdDereverb, cmdClickRepair, cmdMouthDeClick, cmdDeCrackle, cmdDePlosive, cmdBreathControl, cmdSpill, cmdWowFlutter, cmdSpectralRecovery, 0, cmdCaptureAmbience, cmdAmbienceMatch, 0, cmdRemoveHum, cmdDeclip, cmdRepair, cmdRemoveDc });
            group ("Spectral", { cmdRebalance, 0,  cmdSpectralAttenuate, cmdSpectralRemove, cmdSpectralHeal });
            add ({ 0, cmdApplyRack, 0, cmdRecordSequence, cmdPlaySequence });
            break;
        }
        case 3:
        {
            add ({ cmdShowWaveform, cmdShowSpectrogram, cmdShowBoth, cmdMultitrack, cmdShowRack, 0, cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdZoomSelection,
                   0, cmdTallerTracks, cmdShorterTracks, cmdResetVerticalZoom, cmdShowAllTracks });

            juce::PopupMenu themeMenu;
            const auto themes = Theme::themeNames(), accents = Theme::accentNames();

            // The dark themes, then the light ones, then High Contrast, a line between each group.
            for (int i = 0; i < themes.size(); ++i)
            {
                if (themes[i] == "Light" || themes[i] == "High Contrast")
                    themeMenu.addSeparator();

                themeMenu.addItem (themeBaseId + i, themes[i], true, themes[i] == Theme::current().name);
            }

            themeMenu.addSeparator();
            themeMenu.addSectionHeader ("Accent");

            for (int i = 0; i < accents.size(); ++i)
                themeMenu.addItem (accentBaseId + i, accents[i], true, accents[i] == appProperties.getUserSettings()->getValue (accentKey, "Blue"));

            themeMenu.addSeparator();
            themeMenu.addSectionHeader ("Waveform");
            const auto waveforms = Theme::waveformNames();

            for (int i = 0; i < waveforms.size(); ++i)
                themeMenu.addItem (waveformColourBaseId + i, i == 0 ? juce::String ("Theme's Own") : waveforms[i], true,
                                   waveforms[i] == appProperties.getUserSettings()->getValue (waveformColourKey, "Theme"));

            menu.addSeparator();
            menu.addSubMenu ("Theme", themeMenu);
            group ("Time Format", { cmdTimeMinutesSeconds, cmdTimeDecimalSeconds, cmdTimeSamples, 0,
                                    cmdTimeSmpte24, cmdTimeSmpte25, cmdTimeSmpte2997Drop, cmdTimeSmpte2997, cmdTimeSmpte30 });
            break;
        }
        case 4:
            // The panels and Import at the top; the rest in groups by what they do.
            add ({ cmdMixer, cmdClipProperties, cmdClipList, cmdMarkerList, cmdSessionNotes, 0, cmdImportToSession, 0 });
            group ("Tracks", { cmdAddTrack, cmdAddBus, cmdAddFolder, cmdAddFromTemplate, cmdSaveTrackTemplate, 0, cmdDuplicateTrack, cmdDeleteTrack, cmdMoveTrackUp, cmdMoveTrackDown, cmdHideTrack,
                               0, cmdTrackAbove, cmdTrackBelow, 0, cmdUnsoloAll, cmdUnmuteAll });
            group ("Clips", { cmdSplitClip, cmdDuplicateClip, cmdRepeatClip, cmdReverseClip, cmdStretchClip, cmdShiftPitch, 0, cmdLockClip, cmdMuteClip, cmdGroupClips, cmdUngroupClips, cmdClipGainLines });
            group ("Arrange", { cmdNudgeEarlier, cmdNudgeLater, cmdMoveClipsToCursor, cmdTrimStartToCursor, cmdTrimEndToCursor, cmdCloseGaps, cmdQuantize });
            group ("Time Range", { cmdClearRange, cmdDeleteTime, cmdInsertTime, 0, cmdCutTime, cmdCopyTime, cmdPasteTime, cmdDuplicateTime });
            group ("Editing and Recording", { cmdSnapping, cmdZeroCrossings, cmdCrossfade, 0, cmdWriteAutomation, cmdInputMonitoring, cmdLoopRecording });
            group ("Tempo", { cmdMetronome, cmdCountIn, 0, cmdTempo, cmdTapTempo, cmdTempoChange, cmdBarsAndBeats });
            group ("Surround", { cmdSurroundMaster, cmdSevenOneMaster });
            group ("Render and Export", { cmdBounceTrack, cmdFreezeTrack, 0, cmdMixDown, cmdExportStems, cmdExportFolderStems, cmdExportRegions });
            break;
        case 5: add ({ cmdPlayPause, cmdStop, cmdLoopPlayback, cmdLoopRegion, cmdRecord, cmdRecordingOptions, 0, cmdGoToStart, cmdGoToEnd, cmdPreviousEdge, cmdNextEdge, cmdGoToTime, 0, cmdSurroundFoldDown, cmdSurroundOutputs }); break;
        case 6: add ({ cmdAddMarker, cmdRegionFromClips, cmdRegionsOverClips, cmdMarkersAtIntervals, cmdMarkersAtSilences, cmdRegionsOverSounds, cmdMarkClipping, cmdRegionsFromMarkers, cmdFitRegions, cmdRenameMarkers, 0, cmdPreviousMarker, cmdNextMarker, cmdSelectBetweenMarkers, cmdSelectRegion, cmdPreviousRegion, cmdNextRegion, cmdMarkersToZero, 0, cmdMoveMarkersInSelection, cmdDeleteMarkersInSelection, cmdDeleteAllMarkers }); break;
        case 7:
        {
            add ({ cmdPlugins, cmdAudioSettings, cmdKeyShortcuts, 0, cmdSaveRack, cmdLoadRack, 0,
                   cmdMaximizeEditor, 0, cmdShowFiles, cmdShowFileTabs, cmdShowRack, cmdShowSelectionPanel, cmdShowLevels, cmdShowSpectrum, cmdShowPhase, cmdShowTonalBalance, cmdShowStatistics, cmdShowVideo, cmdShowPitchEditor, cmdUndoHistory, cmdMarkerList, cmdMixer, cmdClipProperties, cmdClipList, cmdSessionNotes, 0, cmdShowSoundLibrary });

            juce::PopupMenu workspaceMenu;

            for (size_t i = 0; i < workspaces->all().size(); ++i)
                workspaceMenu.addItem (workspaceBaseId + (int) i, workspaces->all()[i].name, true, (int) i == workspaces->getCurrentIndex());

            workspaceMenu.addSeparator();
            workspaceMenu.addCommandItem (&commands, cmdSaveWorkspace);
            workspaceMenu.addCommandItem (&commands, cmdResetWorkspace);
            workspaceMenu.addCommandItem (&commands, cmdDeleteWorkspace);
            menu.addSeparator();
            menu.addSubMenu ("Workspace", workspaceMenu);
            break;
        }
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
        // The Plug-ins window can clear the list; the built-in effects always stay in it. Adding one
        // that is already there changes nothing, so this settles at once.
        BuiltInEffects::addTo (engine.knownPlugins);

        if (auto xml = engine.knownPlugins.createXml())
        {
            appProperties.getUserSettings()->setValue (knownPluginsKey, xml.get());
            appProperties.saveIfNeeded();
        }

        return;
    }

    if (source == &engine.document || source == &engine.session)
    {
        if (source == &engine.document)
            noticeUnrecordedEdits();

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

    if ((insideAudio != nullptr || insidePending)
        && ((insideAudio != nullptr && ! player.isPlaying()) || multitrackMode || engine.document.getVersion() != insideVersion || playRange() != insideRange))
        stopPlayingInside();

    updatePlaybackLoop();

    updateInputs();

    if (multitrackMode)
    {
        const auto rate = engine.session.getSampleRate();
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

        const auto pos = player.isPlaying() ? player.getHeardFrame() : multitrack.getCursor();
        transportBar->update (player.isPlaying(), isRecording(), loopPlayback, (isRecording() ? "REC " : "") + formatTime (pos, rate));
        const auto visible = multitrack.getVisibleFrames();
        selectionPanel.set (multitrack.getTimeSelection(), { (juce::int64) juce::jmax (0.0, visible.getStart()), (juce::int64) visible.getEnd() }, rate);

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

        transportBar->update (player.isPlaying(), true, loopPlayback, "REC " + formatTime (recorder.getRecordedFrames(), rate));
    }
    else
    {
        const auto pos = player.isPlaying() ? player.getHeardFrame() : waveform.getCursor();
        transportBar->update (player.isPlaying(), false, loopPlayback, insidePending ? juce::String ("Processing...") : formatTime (pos, rate));
    }

    // The view as far as the file goes; nothing to show without audio.
    const auto visible = waveform.getVisibleFrames();
    const auto length = engine.document.hasAudio() ? engine.document.getNumFrames() : (juce::int64) 0;
    selectionPanel.set (waveform.getSelection(),
                        { juce::jlimit ((juce::int64) 0, length, (juce::int64) visible.getStart()), juce::jlimit ((juce::int64) 0, length, (juce::int64) visible.getEnd()) },
                        rate);
    selectionLabel.setText ({}, juce::dontSendNotification);
}

void MainComponent::stopTransport()
{
    if (isRecording())
        stopRecording();
    else
        engine.getPlayer().pause();

    waveform.repaint();
    multitrack.repaint();
}

void MainComponent::refreshTitle()
{
    editorButton.setToggleState (! multitrackMode, juce::dontSendNotification);
    multitrackButton.setToggleState (multitrackMode, juce::dontSendNotification);

    if (multitrackMode)
    {
        auto& session = engine.session;
        const auto state = session.getState();
        const auto name = session.getFile() == juce::File() ? juce::String ("Untitled Session") : session.getFile().getFileName();
        fileLabel.setText ("Multitrack session: " + name + "  |  " + juce::String (state->sampleRate / 1000.0, 1) + " kHz, "
                               + juce::String ((int) state->tracks.size()) + (state->tracks.size() == 1 ? " track" : " tracks"),
                           juce::dontSendNotification);

        if (onTitleChanged != nullptr)
            onTitleChanged ((session.isDirty() ? "* " : "") + name + projectSuffix() + " - SpectraBlade");

        return;
    }

    auto& doc = engine.document;
    const auto name = doc.getFile() == juce::File() ? juce::String ("Untitled") : doc.getFile().getFileName();
    fileLabel.setText (doc.hasAudio() ? "Editing file: " + name + "  |  " + juce::String (doc.getSampleRate() / 1000.0, 1) + " kHz, "
                                          + juce::String (doc.getNumChannels()) + " ch, "
                                          + (doc.getSourceBitDepth() == 32 ? juce::String ("32-bit float")
                                                                            : juce::String (doc.getSourceBitDepth()) + "-bit")
                                      : juce::String(),
                       juce::dontSendNotification);

    if (sequenceRecorder.isRecording())
        fileLabel.setText (fileLabel.getText() + "  |  Recording a sequence: " + juce::String ((int) sequenceRecorder.steps().size())
                               + (sequenceRecorder.steps().size() == 1 ? " step" : " steps"),
                           juce::dontSendNotification);

    if (onTitleChanged != nullptr)
        onTitleChanged ((doc.isDirty() ? "* " : "") + name + projectSuffix() + " - SpectraBlade");
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
        deviceLabel.setText (engine.isDeviceOpen() ? "No audio device" : "Opening the audio device", juce::dontSendNotification);
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
    confirmAllFiles ([this, then = std::move (proceed)] { confirmDiscardSession (then); });
}

//==============================================================================
void MainComponent::setMultitrack (bool on)
{
    if (on == multitrackMode || isRecording())
        return;

    stopPreview();
    engine.getPlayer().pause();
    multitrackMode = on;
    engine.getPlayer().setMode (on ? DocumentPlayerProcessor::Mode::session : DocumentPlayerProcessor::Mode::document);
    engine.useDeviceOutputs();
    waveform.setVisible (! on);
    multitrack.setVisible (on);
    overview.setVisible (on);
    editorPanel->resized();   // the file tabs show with the editor only

    if (auto it = madePanels.find (Panels::history); it != madePanels.end())
        if (auto* history = dynamic_cast<UndoHistory*> (it->second.get()))
            history->refresh();   // the file's steps, or the session's
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

    // A recovered copy isn't a file the user opened.
    if (! as.has_value())
        noteRecent (file);

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

    // Loudness: as mixed, or brought to a delivery spec (as Meet Delivery Spec does), with its report.
    box->addComboBox ("loudness", loudnessChoices(), "Loudness");
    box->getComboBoxComponent ("loudness")->setSelectedItemIndex (0);
    box->addButton ("Mix Down", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, box, file, depths, range, rate, tails] (int result)
        {
            if (result != 1)
                return;

            const auto depth = depths[box->getComboBoxComponent ("depth")->getSelectedItemIndex()];
            const auto tail = (juce::int64) std::llround (tails[(size_t) box->getComboBoxComponent ("tail")->getSelectedItemIndex()] * rate);
            mixDown (file, depth, range.withEnd (range.getEnd() + tail), loudnessChoice (box->getComboBoxComponent ("loudness")->getSelectedItemIndex()));
        }), true);
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

void MainComponent::chooseMarkersToImport()
{
    const auto from = multitrackMode ? engine.session.getFile() : engine.document.getFile();
    chooser = std::make_unique<juce::FileChooser> ("Import markers from CSV", from != juce::File() ? from.getParentDirectory()
                                                                                                 : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.csv");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (const auto file = fc.getResult(); file != juce::File())
                                  importMarkers (file.loadFileAsString(), file.getFileName());
                          });
}

void MainComponent::importMarkers (const juce::String& csv, const juce::String& fileName)
{
    const auto rate = multitrackMode ? engine.session.getSampleRate() : engine.document.getSampleRate();
    const auto length = multitrackMode ? std::numeric_limits<juce::int64>::max() : engine.document.getNumFrames();
    const auto read = MarkerEdits::fromCsv (csv, rate, length);

    if (! read)
    {
        showError ("Import Markers", fileName + " is not a marker list written by Export Markers as CSV.");
        return;
    }

    if (multitrackMode)
    {
        auto next = *engine.session.getState();

        if (MarkerEdits::merge (next.markers, *read) > 0)
            engine.session.apply (std::move (next), "Import Markers");
    }
    else
    {
        auto markers = engine.document.getMarkers();

        if (MarkerEdits::merge (markers, *read) > 0)
            engine.document.setMarkers (std::move (markers), "Import Markers");
    }
}

void MainComponent::chooseClipsCsv()
{
    // Taken now: the session as it is when the command is chosen.
    const auto csv = ClipList::toCsv (*engine.session.getState());
    const auto from = engine.session.getFile();
    const auto start = from != juce::File() ? from.getSiblingFile (from.getFileNameWithoutExtension() + " Clips.csv")
                                            : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Clips.csv");
    chooser = std::make_unique<juce::FileChooser> ("Export clip list as CSV", start, "*.csv");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [csv] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file != juce::File() && ! file.replaceWithText (csv))
                                  showError ("Export Clip List", "Couldn't write " + file.getFullPathName() + ".");
                          });
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
                showError ("Tempo", "The tempo must be from 20 to 400 beats a minute, with 1 to 16 beats in a bar.");
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

std::vector<Delivery::Spec> MainComponent::deliverySpecs()
{
    auto* settings = appProperties.getUserSettings();
    auto all = Delivery::specs();
    auto own = Delivery::specFor (settings->getDoubleValue ("deliveryOwnLufs", -20.0), settings->getDoubleValue ("deliveryOwnTolerance", 1.0),
                                  settings->getDoubleValue ("deliveryOwnCeiling", -1.0));

    if (! Delivery::isValid (own.targetLufs, own.toleranceLu, own.maxTruePeak))
        own = Delivery::specFor (-20.0, 1.0, -1.0);

    own.name = "Custom spec";
    all.push_back (own);
    return all;
}

juce::String MainComponent::describe (const Delivery::Spec& spec)
{
    return spec.name + ": " + Loudness::format (spec.targetLufs, "LUFS") + " +/- " + juce::String (spec.toleranceLu, 1) + " LU, true peak "
           + Loudness::format (spec.maxTruePeak, "dBTP");
}

juce::StringArray MainComponent::loudnessChoices()
{
    juce::StringArray names { "As mixed" };

    for (auto& spec : deliverySpecs())
        names.add (describe (spec));

    return names;
}

std::optional<Delivery::Spec> MainComponent::loudnessChoice (int index)
{
    const auto all = deliverySpecs();

    if (! juce::isPositiveAndBelow (index - 1, (int) all.size()))
        return std::nullopt;

    return all[(size_t) index - 1];
}

void MainComponent::mixDown (const juce::File& file, int bitDepth, juce::Range<juce::int64> range, std::optional<Delivery::Spec> spec)
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
    auto report = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Mixing down...",
        [this, state, rate, effects, inserts, error, warnings, report, file, bitDepth, range, spec] (const std::function<bool (float)>& progress)
        {
            *warnings = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts);
            juce::AudioBuffer<float> mix;
            const auto share = spec.has_value() ? 0.5f : 1.0f;

            if (! OfflineRender::mixSession (*state, range, mix, inserts.get(), [&progress, share] (float p) { return progress (share * p); }))
                return false;

            auto audio = AudioEdits::fromBuffer (std::move (mix));

            // To the spec, as Meet Delivery Spec brings a file to it.
            if (spec.has_value())
            {
                const auto d = Delivery::deliver (*audio, 0, audio->getNumFrames(), rate, *spec, [&progress] (float p) { return progress (0.5f + 0.5f * p); });

                if (d.snapshot == nullptr && d.error.isEmpty())
                    return false;

                if (d.snapshot == nullptr)
                {
                    *error = d.error;
                    return true;
                }

                audio = d.snapshot;
                *report = Delivery::report (*spec, d, file.getFileName());
            }

            // The session's markers go with it, where the format holds them.
            *error = AudioFileIO::save (*audio, rate, file, bitDepth, engine.formatManager,
                                        MarkerEdits::afterCrop (state->markers, range.getStart(), range.getEnd()));
            return true;
        },
        [this, error, warnings, report, file]
        {
            if (error->isNotEmpty())
                showError ("Mix Down", *error);
            else if (warnings->isNotEmpty())
                showError ("Mix Down", file.getFileName() + " was written without these effects, which could not be loaded:\n" + *warnings);

            if (error->isEmpty() && report->isNotEmpty())
                showDeliveryReport (*report, file);
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
    // The track's own stereo sound, which the new track's surround pan places; a 5.1 or 7.1
    // track's own channels in a surround master (whichever that is).
    alone->surround = state->surround && track.isWide();
    alone->sevenOne = alone->surround && track.sevenOne;
    auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

    for (auto& t : alone->tracks)   // the track's, and its keys'
        if (auto xml = engine.getTrackEffects().createXml (t.id))
            (*effects)[t.id] = std::move (xml);

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

    for (auto& t : alone->tracks)   // the track's, and its keys'
        if (auto xml = engine.getTrackEffects().createXml (t.id))
            (*effects)[t.id] = std::move (xml);

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

    // Its other takes reversed alike, each to a file of its own.
    std::vector<SessionClip::Take> takes (clip.takes.size());

    for (int i = 0; i < (int) takes.size() && error.isEmpty(); ++i)
    {
        if (i == clip.activeTake)
            continue;

        auto audio = SessionEdits::reversedAudio (SessionEdits::takeAsPlayed (clip, i));
        const auto takeFile = folder.getNonexistentChildFile (juce::File::createLegalFileName (clip.name) + " Reversed Take " + juce::String (i + 1),
                                                              ".wav", false);
        error = audio == nullptr ? juce::String ("Take " + juce::String (i + 1) + " has no audio.")
                                 : AudioFileIO::save (*audio, state->sampleRate, takeFile, 32, engine.formatManager);
        takes[(size_t) i] = { takeFile, std::move (audio), 0 };
    }

    if (error.isNotEmpty())
    {
        showError ("Reverse Clip", error);
        return;
    }

    auto next = *state;
    SessionEdits::reverseClip (next, clipId, file, reversed, std::move (takes));
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
            showError ("Stretch Clip", "The length must be from 50 % to 200 % of what it is now.");
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
                                         std::function<AudioDocument::Snapshot (const SessionClip&, const std::function<bool (float)>&)> process,
                                         bool keepEnd)
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
    auto takes = std::make_shared<std::vector<SessionClip::Take>> (clip.takes.size());
    auto error = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, action + ": " + clip.name + "...",
        [this, clip, process, rate, folder, file, stretched, takes, error, suffix] (const std::function<bool (float)>& progress)
        {
            // The active take, then each other take alike, from the part the clip plays.
            const auto count = (float) juce::jmax ((size_t) 1, takes->size());
            const auto share = [&progress, count] (int i) { return [&progress, count, i] (float p) { return progress (((float) i + p) / count); }; };
            *stretched = process (clip, share (0));

            if (*stretched == nullptr)
                return false;

            if (! folder.createDirectory())
            {
                *error = "Could not make the folder " + folder.getFullPathName();
                return true;
            }

            *error = AudioFileIO::save (**stretched, rate, file, 32, engine.formatManager);
            auto done = 1;

            for (int i = 0; i < (int) takes->size() && error->isEmpty(); ++i)
            {
                if (i == clip.activeTake)
                    continue;

                auto audio = process (SessionEdits::takeAsPlayed (clip, i), share (done++));

                if (audio == nullptr)
                    return false;

                const auto takeFile = folder.getNonexistentChildFile (juce::File::createLegalFileName (clip.name) + " " + suffix
                                                                          + " Take " + juce::String (i + 1), ".wav", false);
                *error = AudioFileIO::save (*audio, rate, takeFile, 32, engine.formatManager);
                (*takes)[(size_t) i] = { takeFile, std::move (audio), 0 };
            }

            return true;
        },
        [this, clipId, file, stretched, takes, error, action, keepEnd]
        {
            if (error->isNotEmpty())
            {
                showError (action, *error);
                return;
            }

            auto next = *engine.session.getState();

            if (SessionEdits::stretchClip (next, clipId, file, *stretched, keepEnd, *takes))
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
                              const auto range = ! timeRange.isEmpty() ? timeRange
                                                                       : juce::Range<juce::int64> (0, engine.session.getState()->getLength());

                              // Loudness: as mixed, or each stem given the one gain that brings the whole mix to a spec.
                              auto* box = new juce::AlertWindow (folders ? "Export Folder Stems" : "Export Stems",
                                                                 "To a delivery spec, every stem gets the same gain: the one that brings the "
                                                                 "whole mix to the spec's loudness, so the stems still add up to that mix. "
                                                                 "No limiter is used, and the report says when the mix would need one.",
                                                                 juce::MessageBoxIconType::NoIcon, this);
                              box->addComboBox ("loudness", loudnessChoices(), "Loudness");
                              box->getComboBoxComponent ("loudness")->setSelectedItemIndex (0);
                              box->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
                              box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                              box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, folder, range, folders] (int result)
                              {
                                  if (result == 1)
                                      exportStems (folder, range, folders, loudnessChoice (box->getComboBoxComponent ("loudness")->getSelectedItemIndex()));
                              }), true);
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

void MainComponent::exportStems (const juce::File& folder, juce::Range<juce::int64> range, bool folders, std::optional<Delivery::Spec> spec)
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

    // To a delivery spec: the whole mix too, measured for the one gain they all get.
    std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> mixEffects;

    if (spec.has_value())
        for (auto& t : state->tracks)
            if (auto xml = engine.getTrackEffects().createXml (t.id))
                mixEffects[t.id] = std::move (xml);

    auto mixChains = std::make_shared<decltype (mixEffects)> (std::move (mixEffects));

    // Fresh plug-ins for each stem, kept until the job is deleted on the message thread.
    auto inserts = std::make_shared<std::vector<std::unique_ptr<TrackEffects>>>();
    auto errors = std::make_shared<juce::StringArray>();
    auto warnings = std::make_shared<juce::String>();
    auto report = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Exporting stems...",
        [this, state, stems, inserts, errors, warnings, report, mixChains, rate, range, spec, sessionName] (const std::function<bool (float)>& progress)
        {
            const auto count = (float) stems->size() + (spec.has_value() ? 1.0f : 0.0f);
            auto gainDb = 0.0;

            if (spec.has_value())
            {
                inserts->push_back (std::make_unique<TrackEffects>());
                *warnings += OfflineRender::loadTrackEffects (*mixChains, engine.pluginFormatManager, rate, *inserts->back());
                juce::AudioBuffer<float> mix;

                if (! OfflineRender::mixSession (*state, range, mix, inserts->back().get(), [&progress, count] (float p) { return progress (p / count); }))
                    return false;

                const auto plan = Delivery::planStems (*spec, mix, rate, (int) stems->size(), sessionName + " stems");

                if (plan.error.isNotEmpty())
                {
                    errors->add (plan.error + " The stems were not written.");
                    return true;
                }

                gainDb = plan.gainDb;
                *report = plan.report;
            }

            const auto first = spec.has_value() ? 1.0f : 0.0f;

            for (size_t i = 0; i < stems->size(); ++i)
            {
                auto& stem = (*stems)[i];
                inserts->push_back (std::make_unique<TrackEffects>());
                *warnings += OfflineRender::loadTrackEffects (stem.effects, engine.pluginFormatManager, rate, *inserts->back());
                juce::AudioBuffer<float> mix;
                const auto part = [&progress, i, count, first] (float p) { return progress ((first + (float) i + p) / count); };

                if (! OfflineRender::mixSession (*stem.alone, range, mix, inserts->back().get(), part))
                    return false;

                if (spec.has_value())
                    mix.applyGain ((float) juce::Decibels::decibelsToGain (gainDb, -1000.0));

                if (const auto error = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (mix)), rate, stem.file, 32, engine.formatManager);
                    error.isNotEmpty())
                    errors->add (error);
            }

            return true;
        },
        [this, errors, warnings, report, folder, sessionName]
        {
            if (! errors->isEmpty())
                showError ("Export Stems", errors->joinIntoString ("\n"));
            else if (warnings->isNotEmpty())
                showError ("Export Stems", "The stems were written to " + folder.getFullPathName()
                                               + " without these effects, which could not be loaded:\n" + *warnings);

            if (errors->isEmpty() && report->isNotEmpty())
                showDeliveryReport (*report, folder.getChildFile (sessionName + ".wav"));
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
    auto* preview = makeOpenPreview();
    chooser = std::make_unique<juce::FileChooser> ("Import audio", engine.document.getFile().getParentDirectory(),
                                                   openWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              engine.auditionFile ({});
                              juce::StringArray paths;

                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());

                              if (! paths.isEmpty())
                                  addFilesToSession (paths, -1, multitrack.getCursor());
                          },
                          preview);
}

juce::int64 MainComponent::addFilesToSession (const juce::StringArray& files, int track, juce::int64 frame)
{
    auto next = *engine.session.getState();
    juce::StringArray errors;
    int added = 0;
    juce::int64 firstEnd = -1;

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
        if (added == 0)
            firstEnd = frame + loaded.snapshot->getNumFrames();

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

    return firstEnd;
}

void MainComponent::openDocument (const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    if (isRecording())
    {
        showError ("Can't open " + file.getFileName(), "Stop recording first.");
        return;
    }

    if (file.hasFileExtension ("sbsession"))
        confirmDiscardSession ([this, file] { openSession (file); });
    else if (file.hasFileExtension (Project::extension))
        openProject (file);
    else
        openFile (file);
}

void MainComponent::openDocuments (const juce::Array<juce::File>& chosen)
{
    juce::Array<juce::File> audio;

    for (auto& file : chosen)
    {
        if (! file.existsAsFile())
            continue;

        if (file.hasFileExtension ("sbsession") || file.hasFileExtension (Project::extension))
        {
            openDocument (file);   // a session or project takes the place of the rest
            return;
        }

        audio.add (file);
    }

    if (audio.isEmpty())
        return;

    if (isRecording())
    {
        showError ("Can't open files", "Stop recording first.");
        return;
    }

    // All but the last are read and set aside unseen, so each is fitted to the window when first
    // shown; the one showing now keeps its place. The ones that can't be read are listed once.
    juce::StringArray errors;
    auto opened = 0;
    auto showingUnseen = false;   // the editor holds a file adopted here that hasn't been laid out yet
    stopPreview();

    for (auto& file : audio)
    {
        if (const auto index = openFiles.indexOf (file); index >= 0)
        {
            switchToFile (index);
            showingUnseen = false;
            ++opened;
            continue;
        }

        auto loaded = AudioFileIO::load (file, engine.formatManager);

        if (loaded.error.isNotEmpty())
        {
            errors.add (file.getFileName() + ": " + loaded.error);
            continue;
        }

        openFiles.prepareForAnother (showingUnseen ? FilePlace {} : waveform.getPlace());
        engine.adoptFile (file, std::move (loaded));
        showingUnseen = true;

        if (Video::isVideoFile (file))
            showVideoPanel();
        noteRecent (file);
        ++opened;
    }

    if (opened > 0)
    {
        setMultitrack (false);
        refreshTitle();
    }

    if (! errors.isEmpty())
        showError (errors.size() == 1 ? juce::String ("Can't open file") : juce::String ("Can't open some files"), errors.joinIntoString ("\n"));
}

void MainComponent::openFile (const juce::File& file)
{
    if (isRecording())
        return;

    // A file already open is switched to; another opens beside the files open.
    if (const auto index = openFiles.indexOf (file); index >= 0)
    {
        switchToFile (index);
        setMultitrack (false);
        return;
    }

    stopPreview();
    auto loaded = AudioFileIO::load (file, engine.formatManager);

    if (loaded.error.isNotEmpty())
    {
        showError ("Can't open file", file.getFullPathName() + "\n\n" + loaded.error);
        return;
    }

    openFiles.prepareForAnother (waveform.getPlace());
    engine.adoptFile (file, std::move (loaded));
    setMultitrack (false);
    noteRecent (file);

    if (Video::isVideoFile (file))
        showVideoPanel();
    refreshTitle();
}

void MainComponent::switchToFile (int index)
{
    if (isRecording())
        return;

    stopPreview();

    if (const auto place = openFiles.activate (index, waveform.getPlace()))
        waveform.setPlace (*place);

    setMultitrack (false);
    refreshTitle();
    timerCallback();
    commands.commandStatusChanged();
}

void MainComponent::closeFile (int index)
{
    if (isRecording() || ! juce::isPositiveAndBelow (index, openFiles.size()))
        return;

    const auto finish = [this]
    {
        stopPreview();

        if (const auto place = openFiles.close (openFiles.getActiveIndex()))
            waveform.setPlace (*place);

        refreshTitle();
        timerCallback();
        commands.commandStatusChanged();
    };

    // One without changes closes where it is; one with them is shown first, to be asked about.
    const auto& doc = openFiles.documentAt (index);

    if (doc.isDirty() && doc.hasAudio())
    {
        switchToFile (index);
        confirmDiscardChanges (finish);
    }
    else if (index == openFiles.getActiveIndex())
    {
        finish();
    }
    else
    {
        openFiles.close (index);
        commands.commandStatusChanged();
    }
}

void MainComponent::confirmAllFiles (std::function<void()> proceed, int from)
{
    for (auto index : openFiles.unsaved())
    {
        if (index < from)
            continue;

        switchToFile (index);
        confirmDiscardChanges ([this, proceed, index] { confirmAllFiles (proceed, index + 1); });
        return;
    }

    proceed();
}

void MainComponent::noteRecent (const juce::File& file)
{
    RecentFiles::note (recentFiles, file, *appProperties.getUserSettings());
    menuItemsChanged();
}

void MainComponent::setTheme (const juce::String& theme, const juce::String& accentName, const juce::String& waveformName)
{
    auto* settings = appProperties.getUserSettings();
    settings->setValue (themeKey, theme);
    settings->setValue (accentKey, accentName);
    settings->setValue (waveformColourKey, waveformName);
    Theme::apply (Theme::make (theme, accentName, waveformName));
    menuItemsChanged();
}

void MainComponent::menuItemSelected (int menuItemId, int)
{
    if (const auto themes = Theme::themeNames(); juce::isPositiveAndBelow (menuItemId - themeBaseId, themes.size()))
    {
        setTheme (themes[menuItemId - themeBaseId], appProperties.getUserSettings()->getValue (accentKey, "Blue"),
                  appProperties.getUserSettings()->getValue (waveformColourKey, "Theme"));
        return;
    }

    if (const auto accents = Theme::accentNames(); juce::isPositiveAndBelow (menuItemId - accentBaseId, accents.size()))
    {
        setTheme (Theme::current().name, accents[menuItemId - accentBaseId], appProperties.getUserSettings()->getValue (waveformColourKey, "Theme"));
        return;
    }

    if (const auto waveforms = Theme::waveformNames(); juce::isPositiveAndBelow (menuItemId - waveformColourBaseId, waveforms.size()))
    {
        setTheme (Theme::current().name, appProperties.getUserSettings()->getValue (accentKey, "Blue"), waveforms[menuItemId - waveformColourBaseId]);
        return;
    }

    if (juce::isPositiveAndBelow (menuItemId - workspaceBaseId, (int) workspaces->all().size()))
    {
        selectWorkspace (workspaces->all()[(size_t) (menuItemId - workspaceBaseId)].name);
        return;
    }

    if (juce::isPositiveAndBelow (menuItemId - fadeShapeBaseId, (int) FadeShapes::all.size()))
    {
        fadeShape = FadeShapes::all[(size_t) (menuItemId - fadeShapeBaseId)];
        saveSettings();
        menuItemsChanged();
        return;
    }

    if (menuItemId == recentBaseId + RecentFiles::maxFiles)
    {
        RecentFiles::clear (recentFiles, *appProperties.getUserSettings());
        menuItemsChanged();
        return;
    }

    if (! juce::isPositiveAndBelow (menuItemId - recentBaseId, recentFiles.getNumFiles()))
        return;

    const auto file = recentFiles.getFile (menuItemId - recentBaseId);

    if (! file.existsAsFile())
        showError ("Can't open", file.getFullPathName() + " is no longer there.");
    else if (RecentFiles::isSession (file))
        confirmDiscardSession ([this, file] { openSession (file); });
    else if (file.hasFileExtension (Project::extension))
        openProject (file);
    else
        openFile (file);
}

AuditionPreview* MainComponent::makeOpenPreview()
{
    chooser.reset();   // the last panel lets go of its preview first
    auto* settings = appProperties.getUserSettings();
    openPreview = std::make_unique<AuditionPreview> (engine.formatManager,
                                                     AuditionPreview::Player { [this] (const juce::File& f) { return engine.auditionFile (f); },
                                                                               [this] { return engine.getPlayer().isAuditioning(); } },
                                                     settings->getBoolValue (openPreviewKey, false));
    openPreview->onPreviewChanged = [settings] (bool on) { settings->setValue (openPreviewKey, on); };
    return openPreview.get();
}

juce::String MainComponent::openWildcard()
{
    auto wildcard = engine.formatManager.getWildcardForAllFormats();

    // With ffmpeg installed, the formats it reads too.
    if (Ffmpeg::find() != juce::File())
        for (const auto& e : Ffmpeg::importExtensions())
            if (! wildcard.containsIgnoreCase ("*." + e + ";") && ! wildcard.endsWithIgnoreCase ("*." + e))
                wildcard << ";*." << e;

    return wildcard;
}

void MainComponent::chooseFile()
{
    auto* preview = makeOpenPreview();
    chooser = std::make_unique<juce::FileChooser> ("Open audio file", engine.document.getFile().getParentDirectory(),
                                                   openWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              engine.auditionFile ({});
                              openDocuments (fc.getResults());
                          },
                          preview);
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

    askForAudioTarget ("Save As", suggested, [this, done] (const juce::File& file, int depth)
    {
        if (file == juce::File())
        {
            if (done != nullptr) done (false);
            return;
        }

        writeFile (file, depth, done);
    });
}

void MainComponent::saveSelectionAs()
{
    const auto sel = waveform.getSelection();

    if (sel.isEmpty())
        return;

    const auto& doc = engine.document;
    const auto suggested = doc.getFile().getSiblingFile (doc.getFile().getFileNameWithoutExtension() + " selection").withFileExtension ("wav");

    askForAudioTarget ("Save Selection As", suggested, [this, sel] (const juce::File& file, int depth)
    {
        auto& d = engine.document;

        if (file == juce::File())
            return;

        if (const auto error = AudioFileIO::saveRange (*d.getSnapshot(), d.getSampleRate(), sel.getStart(), sel.getEnd(), file, depth,
                                                        engine.formatManager, d.getMarkers(), d.getInfo()); error.isNotEmpty())
            showError ("Save failed", error);
    });
}

void MainComponent::chooseFileRegionsFolder (bool betweenMarkers)
{
    const auto file = engine.document.getFile();
    chooser = std::make_unique<juce::FileChooser> (betweenMarkers ? "Save pieces to" : "Save regions to", file != juce::File() ? file.getParentDirectory()
                                                                                          : juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, betweenMarkers] (const juce::FileChooser& fc)
                          {
                              if (const auto folder = fc.getResult(); folder != juce::File())
                                  saveFileRegions (folder, betweenMarkers);
                          });
}

void MainComponent::saveFileRegions (const juce::File& folder, bool betweenMarkers)
{
    if (backgroundJob != nullptr)
        return;

    const auto& doc = engine.document;
    const auto snapshot = doc.getSnapshot();
    const auto markers = doc.getMarkers();
    const auto rate = doc.getSampleRate();
    const auto name = doc.getFile() != juce::File() ? doc.getFile().getFileNameWithoutExtension() : juce::String ("Untitled");
    const auto regions = Bounce::regionFiles (betweenMarkers ? MarkerEdits::piecesBetweenPoints (markers, doc.getNumFrames()) : markers, name, folder);
    const auto title = juce::String (betweenMarkers ? "Save Pieces Between Markers As" : "Save Each Region As");
    const auto depths = AudioFileIO::bitDepthsFor (folder.getChildFile ("x.wav"), engine.formatManager);
    const auto depth = depths.contains (doc.getSourceBitDepth()) ? doc.getSourceBitDepth() : 32;
    auto errors = std::make_shared<juce::StringArray>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, betweenMarkers ? "Saving pieces..." : "Saving regions...",
        [this, snapshot, markers, rate, regions, depth, errors] (const std::function<bool (float)>& progress)
        {
            for (size_t i = 0; i < regions.size(); ++i)
            {
                if (! progress ((float) i / (float) regions.size()))
                    return false;

                const auto& [range, file] = regions[i];

                if (const auto error = AudioFileIO::saveRange (*snapshot, rate, range.getStart(), range.getEnd(), file, depth,
                                                               engine.formatManager, markers); error.isNotEmpty())
                    errors->add (error);
            }

            return true;
        },
        [errors, title]
        {
            if (! errors->isEmpty())
                showError (title, errors->joinIntoString ("\n"));
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::exportLossy()
{
    const auto& doc = engine.document;

    if (doc.getSnapshot() == nullptr || doc.getNumFrames() == 0 || backgroundJob != nullptr)
        return;

    const auto selection = waveform.getSelection();
    const auto base = doc.getFile() != juce::File() ? doc.getFile() : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Untitled");
    chooser = std::make_unique<juce::FileChooser> ("Export as MP3 or AAC", base.withFileExtension ("mp3"), "*.mp3;*.m4a");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, selection] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (! LossyFiles::isLossy (file))
                                  file = file.withFileExtension ("mp3");

                              askLossySettings (file, selection);
                          });
}

void MainComponent::askLossySettings (const juce::File& file, juce::Range<juce::int64> selection)
{
    const auto kind = LossyFiles::kindFor (file);
    const auto isMp3 = kind == LossyFiles::Kind::mp3;
    const auto& doc = engine.document;
    std::vector<LossyFiles::Settings> choices;
    juce::StringArray names;

    if (isMp3)
        for (auto q : { 0, 2, 4 })
        {
            choices.push_back ({ kind, 0, true, q });
            names.add ("Variable, V" + juce::String (q) + (q == 0 ? " (best)" : ""));
        }

    for (auto kbps : LossyFiles::bitRates())
    {
        choices.push_back ({ kind, kbps });
        names.add ((isMp3 ? "Constant, " : "") + juce::String (kbps) + " kbps");
    }

    const auto rate = doc.getSampleRate();
    const auto written = LossyFiles::outputRate (rate);
    juce::String about = (isMp3 ? "MP3" : "AAC") + juce::String (" file: ") + file.getFileName() + ".";

    if (! juce::exactlyEqual (written, rate))
        about << "\nWritten at " << juce::String (written / 1000.0, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " kHz.";

    if (! doc.getMarkers().empty())
        about << "\nMarkers are not kept.";

    auto* box = new juce::AlertWindow ("Export as MP3 or AAC", about, juce::MessageBoxIconType::NoIcon);
    box->addComboBox ("quality", names, isMp3 ? "Bit rate" : "Average bit rate");
    box->getComboBoxComponent ("quality")->setSelectedItemIndex (juce::jlimit (0, names.size() - 1, isMp3 ? lossyMp3Choice : lossyAacChoice));

    if (! selection.isEmpty())
    {
        box->addComboBox ("range", { "The selection", "The whole file" }, "What");
        box->getComboBoxComponent ("range")->setSelectedItemIndex (0);
    }

    box->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, file, selection, choices, isMp3] (int result)
    {
        if (result != 1 || backgroundJob != nullptr)
            return;

        const auto index = box->getComboBoxComponent ("quality")->getSelectedItemIndex();
        (isMp3 ? lossyMp3Choice : lossyAacChoice) = index;
        const auto settings = choices[(size_t) index];
        const auto onlySelection = ! selection.isEmpty() && box->getComboBoxComponent ("range")->getSelectedItemIndex() == 0;
        const auto& d = engine.document;
        const auto snapshot = onlySelection ? AudioEdits::crop (*d.getSnapshot(), selection.getStart(), selection.getEnd()) : d.getSnapshot();
        const auto sampleRate = d.getSampleRate();
        auto error = std::make_shared<juce::String>();
        const auto info = d.getInfo();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Exporting...",
            [snapshot, sampleRate, file, settings, error, info] (const std::function<bool (float)>& progress)
            {
                *error = LossyFiles::write (*snapshot, sampleRate, file, settings, progress, info);
                return *error != "Stopped.";
            },
            [error]
            {
                if (error->isNotEmpty() && *error != "Stopped.")
                    showError ("Export as MP3 or AAC", *error);
            });
        backgroundJob->launchThread();
        commands.commandStatusChanged();
    }), true);
}

void MainComponent::exportWithFfmpeg()
{
    const auto& doc = engine.document;

    if (doc.getSnapshot() == nullptr || doc.getNumFrames() == 0 || backgroundJob != nullptr)
        return;

    const auto ffmpeg = Ffmpeg::find();

    if (ffmpeg == juce::File())
    {
        showError ("Export with ffmpeg", "This needs ffmpeg, which isn't installed. With Homebrew: brew install ffmpeg");
        return;
    }

    const auto selection = waveform.getSelection();
    const auto& targets = Ffmpeg::targets();
    juce::StringArray names;

    for (const auto& t : targets)
        names.add (t.name + " (." + t.extension + ")");

    auto* box = new juce::AlertWindow ("Export with ffmpeg", "Written by " + ffmpeg.getFullPathName() + ".", juce::MessageBoxIconType::NoIcon);
    box->addComboBox ("format", names, "Format");
    box->addComboBox ("rate", {}, "Bit rate");
    auto* format = box->getComboBoxComponent ("format");
    auto* bitRate = box->getComboBoxComponent ("rate");

    format->onChange = [format, bitRate, &targets]
    {
        const auto& t = targets[(size_t) juce::jmax (0, format->getSelectedItemIndex())];
        bitRate->clear (juce::dontSendNotification);

        for (auto kbps : t.bitRates)
            bitRate->addItem (juce::String (kbps) + " kbps", kbps);

        if (! t.hasBitRate)
            bitRate->addItem ("Lossless", 1);

        bitRate->setSelectedId (t.hasBitRate ? t.defaultBitRate : 1, juce::dontSendNotification);
        bitRate->setEnabled (t.hasBitRate);
    };
    format->setSelectedItemIndex (juce::jlimit (0, (int) targets.size() - 1, ffmpegTargetChoice), juce::sendNotificationSync);

    if (! selection.isEmpty())
    {
        box->addComboBox ("range", { "The selection", "The whole file" }, "What");
        box->getComboBoxComponent ("range")->setSelectedItemIndex (0);
    }

    box->addButton ("Export...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, ffmpeg, selection] (int result)
    {
        if (result != 1)
            return;

        ffmpegTargetChoice = box->getComboBoxComponent ("format")->getSelectedItemIndex();
        const auto target = Ffmpeg::targets()[(size_t) ffmpegTargetChoice];
        const auto kbps = box->getComboBoxComponent ("rate")->getSelectedId();
        const auto onlySelection = ! selection.isEmpty() && box->getComboBoxComponent ("range")->getSelectedItemIndex() == 0;
        const auto& d = engine.document;
        const auto base = d.getFile() != juce::File() ? d.getFile() : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Untitled");

        chooser = std::make_unique<juce::FileChooser> ("Export as " + target.name, base.withFileExtension (target.extension), "*." + target.extension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, ffmpeg, target, kbps, onlySelection, selection] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();

            if (file == juce::File() || backgroundJob != nullptr)
                return;

            file = file.withFileExtension (target.extension);
            const auto& current = engine.document;
            const auto snapshot = onlySelection ? AudioEdits::crop (*current.getSnapshot(), selection.getStart(), selection.getEnd()) : current.getSnapshot();
            const auto sampleRate = current.getSampleRate();
            auto error = std::make_shared<juce::String>();
            const auto info = current.getInfo();

            backgroundJob = std::make_unique<BackgroundJob> (*this, "Exporting with ffmpeg...",
                [this, snapshot, sampleRate, ffmpeg, file, target, kbps, error, info] (const std::function<bool (float)>& progress)
                {
                    juce::TemporaryFile wav (".wav");
                    *error = AudioFileIO::save (*snapshot, sampleRate, wav.getFile(), 32, engine.formatManager);

                    if (error->isEmpty() && progress (0.3f))
                        *error = Ffmpeg::encode (ffmpeg, wav.getFile(), file, target, kbps, [&progress] { return progress (0.3f); }, info);

                    return *error != "Stopped.";
                },
                [error]
                {
                    if (error->isNotEmpty() && *error != "Stopped.")
                        showError ("Export with ffmpeg", *error);
                });
            backgroundJob->launchThread();
            commands.commandStatusChanged();
        });
    }), true);
}

namespace
{
    /** The File Info dialog's cover: a thumbnail, its size and type, and buttons to choose an image or
        remove it. */
    class CoverPicker final : public juce::Component
    {
    public:
        explicit CoverPicker (std::shared_ptr<const juce::MemoryBlock> initial) : cover (std::move (initial))
        {
            choose.onClick = [this] { chooseImage(); };
            remove.onClick = [this] { set (nullptr); };
            addAndMakeVisible (choose);
            addAndMakeVisible (remove);
            addAndMakeVisible (details);
            Theme::follow (details, juce::Label::textColourId, Theme::dimText);
            setSize (360, 120);
            set (cover);
        }

        std::shared_ptr<const juce::MemoryBlock> getCover() const  { return cover; }

        void paint (juce::Graphics& g) override
        {
            const auto area = thumbArea().toFloat();
            g.setColour (Theme::lineStrong);
            g.drawRect (area);

            if (thumbnail.isValid())
            {
                g.drawImage (thumbnail, area.reduced (2.0f), juce::RectanglePlacement::centred);
                return;
            }

            g.setColour (Theme::dimText);
            g.setFont (12.0f);
            g.drawText ("No cover", area, juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromLeft (thumbArea().getRight() + 12);
            details.setBounds (r.removeFromTop (44));
            r.removeFromTop (6);
            choose.setBounds (r.removeFromTop (26).removeFromLeft (130));
            r.removeFromTop (6);
            remove.setBounds (r.removeFromTop (26).removeFromLeft (130));
        }

    private:
        std::shared_ptr<const juce::MemoryBlock> cover;
        juce::Image thumbnail;
        juce::Label details;
        juce::TextButton choose { "Choose Image..." }, remove { "Remove" };
        std::unique_ptr<juce::FileChooser> chooser;

        juce::Rectangle<int> thumbArea() const  { return { 0, 0, 120, 120 }; }

        void set (std::shared_ptr<const juce::MemoryBlock> next)
        {
            cover = std::move (next);
            thumbnail = cover != nullptr ? juce::ImageFileFormat::loadFrom (cover->getData(), cover->getSize()) : juce::Image();
            FileInfo probe;
            probe.cover = cover;
            details.setText (thumbnail.isValid() ? "Front cover: " + juce::String (thumbnail.getWidth()) + " x " + juce::String (thumbnail.getHeight())
                                                       + (probe.coverMime() == "image/png" ? " PNG, " : " JPEG, ")
                                                       + juce::File::descriptionOfSizeInBytes ((juce::int64) cover->getSize())
                                                 : juce::String ("Front cover: none. A JPEG or PNG image, up to 8 MB."),
                             juce::dontSendNotification);
            remove.setEnabled (cover != nullptr);
            repaint();
        }

        void chooseImage()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a cover image", juce::File(), "*.jpg;*.jpeg;*.png");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [safe = juce::Component::SafePointer<CoverPicker> (this)] (const juce::FileChooser& fc)
            {
                if (safe == nullptr || fc.getResult() == juce::File())
                    return;

                juce::MemoryBlock data;
                const auto file = fc.getResult();

                if (! file.loadFileAsData (data) || ! FileInfoIO::isCoverImage (data.getData(), data.getSize())
                    || ! juce::ImageFileFormat::loadFrom (data.getData(), data.getSize()).isValid())
                {
                    showError ("Not a cover image", file.getFileName() + " isn't a JPEG or PNG image of up to 8 MB.");
                    return;
                }

                safe->set (std::make_shared<const juce::MemoryBlock> (std::move (data)));
            });
        }
    };
}

void MainComponent::batchProcess()
{
    if (backgroundJob != nullptr)
        return;

    auto* settings = appProperties.getUserSettings();
    const juce::File start (settings->getValue ("batchSources", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getFullPathName()));
    chooser = std::make_unique<juce::FileChooser> ("Batch Process: choose files or folders", start, openWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this, settings] (const juce::FileChooser& fc)
                          {
                              const auto chosen = fc.getResults();

                              if (chosen.isEmpty())
                                  return;

                              settings->setValue ("batchSources", (chosen[0].isDirectory() ? chosen[0] : chosen[0].getParentDirectory()).getFullPathName());
                              const auto files = Batch::collect (chosen, engine.formatManager);

                              if (files.isEmpty())
                                  showError ("Batch Process", "There are no audio files there.");
                              else
                                  askForBatchSettings (files);
                          });
}

void MainComponent::askForBatchSettings (const juce::Array<juce::File>& files)
{
    auto* settings = appProperties.getUserSettings();
    const auto withFfmpeg = Ffmpeg::find() != juce::File();
    const auto types = Batch::types (withFfmpeg);
    const auto rackSlots = engine.getNumRackSlots();
    static const juce::Array<double> rates { 0.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    static const juce::Array<int> depths { 0, 16, 24, 32 };

    juce::String what = juce::String (files.size()) + (files.size() == 1 ? " file: " : " files: ") + files[0].getFileName();

    if (files.size() > 1)
        what << (files.size() == 2 ? " and " : ", ") << files[1].getFileName() << (files.size() > 2 ? ", ..." : "");

    auto* box = new juce::AlertWindow ("Batch Process",
                                       what + "\nEach file goes through these steps in order and is written as a new file; the files "
                                              "themselves are left as they are.",
                                       juce::MessageBoxIconType::NoIcon, this);

    juce::StringArray rackChoices { "Leave out the effects rack" };

    if (rackSlots > 0)
        rackChoices.add ("Through the effects rack (" + juce::String (rackSlots) + (rackSlots == 1 ? " effect)" : " effects)"));

    box->addComboBox ("rack", rackChoices, "Effects");

    juce::StringArray sequenceChoices { "None" };
    sequenceChoices.addArray (Sequences::userStore().names());
    box->addComboBox ("sequence", sequenceChoices, "Recorded sequence (on the whole file)");
    box->addComboBox ("rate", { "Keep each file's", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz" }, "Sample rate");
    box->addComboBox ("channels", { "Keep each file's", "Mono", "Stereo" }, "Channels");
    // Then each delivery spec, as Meet Delivery Spec does it.
    juce::StringArray levels { "Leave it", "Normalize the peak", "Match loudness, stopping at the ceiling", "Match loudness, limiting at the ceiling" };

    for (auto& spec : deliverySpecs())
        levels.add ("Meet " + describe (spec));

    box->addComboBox ("level", levels, "Level");
    box->addTextEditor ("peak", juce::String (settings->getDoubleValue ("batchPeak", -1.0), 1), "Peak (dBFS)");
    box->addTextEditor ("lufs", juce::String (settings->getDoubleValue ("batchLufs", -16.0), 1), "Loudness (LUFS)");
    box->addTextEditor ("ceiling", juce::String (settings->getDoubleValue ("batchCeiling", -1.0), 1), "True peak ceiling (dBTP)");

    juce::StringArray typeNames;

    for (auto& t : types)
        typeNames.add (Batch::nameOf (t));

    box->addComboBox ("type", typeNames, "File type");
    box->addComboBox ("depth", { "Keep each file's", "16-bit", "24-bit", "32-bit float" }, "Bit depth (WAV, AIFF, FLAC)");

    juce::StringArray rateNames;

    for (auto r : LossyFiles::bitRates())
        rateNames.add (juce::String (r) + " kbps");

    box->addComboBox ("kbps", rateNames, "Bit rate (MP3, AAC, Opus, AC-3)");
    box->addTextEditor ("suffix", settings->getValue ("batchSuffix"), "Added to each name");
    box->addComboBox ("folder", { "Beside each file", "In a folder, chosen next" }, "Write them");

    const auto pick = [box] (const char* name, int index) { box->getComboBoxComponent (name)->setSelectedItemIndex (juce::jmax (0, index)); };
    pick ("rack", juce::jmin (rackChoices.size() - 1, settings->getIntValue ("batchRack", 0)));
    pick ("sequence", sequenceChoices.indexOf (settings->getValue ("batchSequence")));
    pick ("rate", rates.indexOf (settings->getDoubleValue ("batchRate", 0.0)));
    pick ("channels", settings->getIntValue ("batchChannels", 0));
    pick ("level", settings->getIntValue ("batchLevel", 0));
    pick ("type", types.indexOf (settings->getValue ("batchType", "wav")));
    pick ("depth", depths.indexOf (settings->getIntValue ("batchDepth", 0)));
    pick ("kbps", LossyFiles::bitRates().indexOf (settings->getIntValue ("batchKbps", 192)));
    pick ("folder", settings->getIntValue ("batchFolderMode", 0));

    for (auto* name : { "peak", "lufs", "ceiling" })
        box->getTextEditor (name)->setInputRestrictions (8, "-.0123456789");

    box->addButton ("Process", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, settings, files, types] (int result)
    {
        if (result != 1)
            return;

        const auto index = [box] (const char* name) { return box->getComboBoxComponent (name)->getSelectedItemIndex(); };
        Batch::Settings s;
        s.applyRack = index ("rack") == 1;
        s.sampleRate = rates[index ("rate")];
        s.channels = index ("channels");
        const auto level = index ("level");
        s.level = level == 0 ? Batch::Level::none : level == 1 ? Batch::Level::peak : level <= 3 ? Batch::Level::loudness : Batch::Level::spec;

        if (s.level == Batch::Level::spec)
        {
            const auto specs = deliverySpecs();
            const auto& spec = specs[(size_t) juce::jlimit (0, (int) specs.size() - 1, level - 4)];
            s.specLufs = spec.targetLufs;
            s.specToleranceLu = spec.toleranceLu;
            s.specCeilingDbtp = spec.maxTruePeak;
        }
        s.limit = index ("level") == 3;
        s.peakDb = (float) box->getTextEditorContents ("peak").getDoubleValue();
        s.lufs = box->getTextEditorContents ("lufs").getDoubleValue();
        s.ceilingDbtp = box->getTextEditorContents ("ceiling").getDoubleValue();
        s.type = types[index ("type")];
        s.bitDepth = depths[index ("depth")];
        s.lossy.kbps = LossyFiles::bitRates()[index ("kbps")];
        s.ffmpegKbps = s.lossy.kbps;
        s.suffix = box->getTextEditorContents ("suffix");

        if (index ("sequence") > 0)
        {
            const auto name = box->getComboBoxComponent ("sequence")->getText();
            const auto sequence = Sequences::userStore().load (name);

            if (! sequence.has_value())
            {
                showError ("Batch Process", name + " can't be read: it may hold a step from a later version.");
                return;
            }

            s.sequence = std::make_shared<const Sequences::Sequence> (*sequence);
        }

        settings->setValue ("batchSequence", index ("sequence") > 0 ? box->getComboBoxComponent ("sequence")->getText() : juce::String());
        settings->setValue ("batchRack", index ("rack"));
        settings->setValue ("batchRate", s.sampleRate);
        settings->setValue ("batchChannels", s.channels);
        settings->setValue ("batchLevel", index ("level"));
        settings->setValue ("batchPeak", s.peakDb);
        settings->setValue ("batchLufs", s.lufs);
        settings->setValue ("batchCeiling", s.ceilingDbtp);
        settings->setValue ("batchType", s.type);
        settings->setValue ("batchDepth", s.bitDepth);
        settings->setValue ("batchKbps", s.lossy.kbps);
        settings->setValue ("batchSuffix", s.suffix);
        settings->setValue ("batchFolderMode", index ("folder"));

        if (index ("folder") == 0)
        {
            runBatch (files, s);
            return;
        }

        const juce::File start (settings->getValue ("batchFolder", files[0].getParentDirectory().getFullPathName()));
        chooser = std::make_unique<juce::FileChooser> ("Batch Process: the folder to write into", start);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, settings, files, s] (const juce::FileChooser& fc) mutable
                              {
                                  if (! fc.getResult().isDirectory())
                                      return;

                                  settings->setValue ("batchFolder", fc.getResult().getFullPathName());
                                  s.folder = fc.getResult();
                                  runBatch (files, s);
                              });
    }), true);
}

void MainComponent::runBatch (const juce::Array<juce::File>& files, const Batch::Settings& settings)
{
    if (const auto problem = settings.problem(); problem.isNotEmpty())
    {
        showError ("Batch Process", problem);
        return;
    }

    if (backgroundJob != nullptr)
        return;

    std::shared_ptr<const juce::XmlElement> rack;

    if (settings.applyRack)
        rack = engine.createRackXml();

    auto outcomes = std::make_shared<std::vector<Batch::Outcome>>();
    backgroundJob = std::make_unique<BackgroundJob> (*this, "Batch processing...",
        [this, files, settings, rack, outcomes] (const std::function<bool (float)>& progress)
        {
            for (int i = 0; i < files.size(); ++i)
            {
                auto outcome = Batch::process (files[i], settings, engine.formatManager, rack.get(), &engine.pluginFormatManager,
                                               [&progress, i, n = files.size()] (float p) { return progress (((float) i + p) / (float) n); });

                if (outcome.error == "Stopped.")
                    return false;

                outcomes->push_back (std::move (outcome));
            }

            return true;
        },
        [this, outcomes, settings]
        {
            // With a delivery spec, every file's report goes in a text file beside them.
            const auto delivery = Batch::writeDeliveryReport (*outcomes, settings, juce::Time::getCurrentTime());
            showBatchReport (*outcomes, delivery);
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::showSoundLibrary()
{
    if (soundLibrary == nullptr)
    {
        // The index lives with the app's settings; only folders added in the window are scanned.
        soundLibrary = std::make_unique<SoundLibraryWindow> (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                                                 .getChildFile ("SpectraBlade").getChildFile ("Sound Library").getChildFile ("Index.sqlite"));
        soundLibrary->getView().onOpen = [this] (const juce::File& file) { openFile (file); };
        soundLibrary->getView().onInsert = [this] (const juce::StringArray& files)
        {
            const auto end = addFilesToSession (files, multitrack.getSelectedTrack(), multitrack.getCursor());

            if (end >= 0)
                multitrack.setCursor (end);

            return end >= 0;
        };
        soundLibrary->getView().setAudioDevice (&engine.deviceManager);
        soundLibrary->getView().setPaneWidth (appProperties.getUserSettings()->getIntValue ("libraryPaneWidth", 0));
        soundLibrary->getView().onPaneWidthChanged = [this] (int width) { appProperties.getUserSettings()->setValue ("libraryPaneWidth", width); };
        rememberPlace (*soundLibrary, "Sound Library");
    }

    soundLibrary->setVisible (true);
    soundLibrary->toFront (true);
}

void MainComponent::toggleSoundLibrary()
{
    if (soundLibrary != nullptr && soundLibrary->isVisible())
    {
        soundLibrary->setVisible (false);
        commands.commandStatusChanged();
        return;
    }

    showSoundLibrary();
    commands.commandStatusChanged();
}

void MainComponent::showBatchReport (const std::vector<Batch::Outcome>& outcomes, const juce::File& deliveryReport)
{
    juce::File firstWritten;

    for (auto& o : outcomes)
        if (o.written != juce::File() && firstWritten == juce::File())
            firstWritten = o.written;

    auto* box = new juce::AlertWindow ("Batch Process", {}, juce::MessageBoxIconType::NoIcon, this);
    auto* text = new juce::TextEditor();
    text->setMultiLine (true);
    text->setReadOnly (true);
    text->setScrollbarsShown (true);
    text->setText (Batch::report (outcomes)
                   + (deliveryReport != juce::File() ? "\n\nThe delivery report, with each file's measurements against the spec, is in "
                                                           + deliveryReport.getFullPathName() + "."
                                                     : juce::String()));
    text->setSize (560, 300);
    box->addCustomComponent (text);
    box->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));

    if (firstWritten != juce::File())
        box->addButton ("Show in Finder", 1);

    box->enterModalState (true, juce::ModalCallbackFunction::create ([text, firstWritten] (int result)
    {
        delete text;

        if (result == 1)
            firstWritten.revealToUser();
    }), true);
}

Sequences::Result MainComponent::runStep (const Sequences::Step& step)
{
    auto& doc = engine.document;
    const auto r = selectionOrAll();
    auto result = Sequences::apply (step, doc.getSnapshot(), r.getStart(), r.getEnd(), doc.getSampleRate(), doc.getMarkers(), nullptr);

    if (result.error.isNotEmpty())
    {
        showError (step.name(), result.error);
        return result;
    }

    if (result.snapshot != nullptr)
    {
        applyEdit (result.snapshot, step.name(), waveform.getSelection());
        recordStep (step);
    }

    return result;
}

void MainComponent::runStepInBackground (const Sequences::Step& step, const juce::String& busy)
{
    auto& doc = engine.document;

    if (! doc.hasAudio() || backgroundJob != nullptr)
        return;

    engine.getPlayer().pause();

    const auto range = selectionOrAll();
    const auto selected = ! waveform.getSelection().isEmpty();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto markers = doc.getMarkers();
    const auto version = doc.getVersion();
    auto result = std::make_shared<Sequences::Result>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, busy,
        [result, step, snapshot, range, rate, markers] (const std::function<bool (float)>& progress)
        {
            *result = Sequences::apply (step, snapshot, range.getStart(), range.getEnd(), rate, markers, nullptr, progress);
            return result->error.isEmpty();
        },
        [this, result, step, selected, version]
        {
            if (engine.document.getVersion() != version)
            {
                showError (step.name(), "The audio changed meanwhile, so the result was discarded.");
                return;
            }

            if (result->snapshot != nullptr)
            {
                applyEdit (result->snapshot, step.name(), selected ? juce::Range<juce::int64> (result->start, result->end) : juce::Range<juce::int64>(),
                           result->markers);
                recordStep (step);
            }

            // What the step found or left, each as a sentence.
            if (! result->notes.isEmpty())
            {
                juce::StringArray lines;

                for (auto& note : result->notes)
                    lines.add (note.substring (0, 1).toUpperCase() + note.substring (1) + ".");

                showNote (step.name(), lines.joinIntoString ("\n"));
            }
        });
    backgroundJob->launchThread();
}

juce::StringArray MainComponent::stretchMethods()
{
    return { "Signalsmith Stretch", "SpectraBlade Stretch (sharper attacks)" };
}

void MainComponent::askToStretch()
{
    const auto range = selectionOrAll();
    const auto seconds = (double) range.getLength() / engine.document.getSampleRate();

    showEffectDialog ("Stretch", juce::String (waveform.getSelection().isEmpty() ? "The whole file" : "The selection") + " ("
                                     + juce::String (seconds, 2) + " s) longer or shorter, keeping its pitch. Markers in it keep their place in it.",
                      { { "percent", "Length (% of now)", stretchPercent, 2, {}, TimeStretch::shortest * 100.0, TimeStretch::longest * 100.0, 1.0 },
                        { "method", "Method", stretchMethod, 0, stretchMethods() } },
                      "spectrablade:stretch", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          stretchPercent = v.at ("percent");
                          stretchMethod = v.at ("method");

                          if (std::abs (stretchPercent - 100.0) >= 0.005)
                              runStepInBackground ({ "stretch", v, nullptr }, "Stretching...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "stretch", v, nullptr }); },
                      "Stretch");
}

void MainComponent::askToExtremeStretch()
{
    const auto range = selectionOrAll();
    const auto seconds = (double) range.getLength() / engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v) { return ExtremeStretch::Settings { v.at ("factor"), v.at ("window") }; };

    showEffectDialog ("Extreme Stretch", juce::String (waveform.getSelection().isEmpty() ? "The whole file" : "The selection") + " ("
                                             + juce::String (seconds, 2) + " s) smeared to many times its length. Each window of the sound keeps "
                                             "its pitch and colour and loses its timing, so a texture or a chord becomes a pad and attacks, words "
                                             "and rhythm melt away. A longer window smears more smoothly and loses more of the movement; a "
                                             "shorter one keeps more of it and sounds grainier. Markers in it keep their place in it.",
                      { { "factor", "Stretch (times)", extremeFactor, 1, {}, ExtremeStretch::shortestFactor, ExtremeStretch::longestFactor, 1.0 },
                        { "window", "Window (s)", extremeWindow, 2, {}, ExtremeStretch::shortestWindow, ExtremeStretch::longestWindow, 0.05 } },
                      "spectrablade:extreme-stretch", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return ExtremeStretch::isValid (toSettings (v)); },
                      "The stretch must be from 1 to 1,000 times, and the window from 0.05 to 2 seconds.", nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          extremeFactor = v.at ("factor");
                          extremeWindow = v.at ("window");
                          runStepInBackground ({ "extremeStretch", v, nullptr }, "Stretching...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "extremeStretch", v, nullptr }); },
                      "Stretch");
}

void MainComponent::askToShiftPitch()
{
    showEffectDialog ("Shift Pitch", juce::String (waveform.getSelection().isEmpty() ? "The whole file" : "The selection")
                                         + " higher or lower, keeping its length (12 semitones is an octave).",
                      { { "semitones", "Semitones", pitchSemitones, 2, {}, -TimeStretch::widestShift, TimeStretch::widestShift, 1.0 },
                        { "method", "Method", stretchMethod, 0, stretchMethods() } },
                      "spectrablade:shift-pitch", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          pitchSemitones = v.at ("semitones");
                          stretchMethod = v.at ("method");

                          if (std::abs (pitchSemitones) >= 0.005)
                              runStepInBackground ({ "pitch", v, nullptr }, "Shifting the pitch...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "pitch", v, nullptr }); },
                      "Shift");
}

void MainComponent::askToLevelSpeech()
{
    auto* settings = appProperties.getUserSettings();
    const auto toSettings = [] (const EffectValues& v)
    {
        return Leveller::Settings { (float) v.at ("target"), (float) v.at ("boost"), (float) v.at ("cut"), (float) v.at ("silence"), (float) v.at ("speed") };
    };

    showEffectDialog ("Level Speech",
                      "Raises quiet passages and lowers loud ones towards the target, smoothly over about a second. Pauses "
                      "quieter than the silence threshold take the gain of the speech around them rather than being raised "
                      "themselves, and no sample comes out above -1 dBFS. The fastest change holds the gain to that many dB a "
                      "second (at 30 it moves as fast as the smoothing lets it, 5 to 13 dB a second on real speech, at pauses and "
                      "changes of speaker): lower it if the levelling is heard to pump.",
                      { { "target", "Target (LUFS)", settings->getDoubleValue ("levelTarget", -20.0), 1, {}, -40.0, -5.0, 0.5 },
                        { "boost", "Largest boost (dB)", settings->getDoubleValue ("levelBoost", 12.0), 1, {}, 0.0, 30.0, 0.5 },
                        { "cut", "Largest cut (dB)", settings->getDoubleValue ("levelCut", 12.0), 1, {}, 0.0, 30.0, 0.5 },
                        { "silence", "Silence below (LUFS)", settings->getDoubleValue ("levelSilence", -50.0), 1, {}, -80.0, -20.0, 1.0 },
                        { "speed", "Fastest change (dB/s)", settings->getDoubleValue ("levelSpeed", 30.0), 0, {}, 1.0, 30.0, 1.0 } },
                      "spectrablade:level-speech", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return Leveller::isValid (toSettings (v)); },
                      "The target must be from -40 to -5 LUFS, the boost and cut from 0 to 30 dB, the silence threshold from -80 to -20 LUFS, and the fastest change from 1 to 30 dB a second.",
                      nullptr, nullptr,
                      [this, settings] (const EffectValues& v)
                      {
                          settings->setValue ("levelTarget", v.at ("target"));
                          settings->setValue ("levelBoost", v.at ("boost"));
                          settings->setValue ("levelCut", v.at ("cut"));
                          settings->setValue ("levelSilence", v.at ("silence"));
                          settings->setValue ("levelSpeed", v.at ("speed"));
                          runStepInBackground ({ "levelSpeech", v, nullptr }, "Levelling speech...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "levelSpeech", v, nullptr }); },
                      "Level");
}

void MainComponent::moveNotesInBackground (const PitchEditor::Source& source, const PitchCorrection::Track& track,
                                           const std::vector<PitchCorrection::Note>& notes)
{
    auto& doc = engine.document;

    if (multitrackMode || ! doc.hasAudio() || backgroundJob != nullptr)
        return;

    engine.getPlayer().pause();
    const auto selected = ! waveform.getSelection().isEmpty();
    auto result = std::make_shared<AudioDocument::Snapshot>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Moving the notes...",
        [result, source, track, notes] (const std::function<bool (float)>& progress)
        {
            *result = PitchCorrection::moveNotes (*source.audio, source.start, source.end, source.sampleRate, track, notes, progress);
            return *result != nullptr;
        },
        [this, result, source, selected]
        {
            auto* panel = dynamic_cast<PitchEditor*> (madePanels.count (Panels::pitch) > 0 ? madePanels[Panels::pitch].get() : nullptr);

            if (engine.document.getVersion() != source.version)
            {
                showError ("Pitch Editor", "The audio changed meanwhile, so the result was discarded.");
                return;
            }

            if (*result == nullptr)
                return;

            applyEdit (*result, "Move Notes", selected ? juce::Range<juce::int64> (source.start, source.end) : juce::Range<juce::int64>());

            if (panel != nullptr)
                panel->clear ("Applied. Analyse again to move more.");
        });
    backgroundJob->launchThread();
}

juce::uint32 MainComponent::clipPitchVersion (juce::uint32 clipId) const
{
    const auto state = engine.session.getState();
    const auto where = state->findClip (clipId);

    if (! where.has_value())
        return 0;

    const auto& c = state->tracks[(size_t) where->first].clips[(size_t) where->second];
    auto h = std::hash<const void*>() (c.audio.get());
    h = h * 1000003u ^ std::hash<juce::int64>() (c.offset);
    h = h * 1000003u ^ std::hash<juce::int64>() (c.length);
    h = h * 1000003u ^ clipId;
    return (juce::uint32) (h ^ (h >> 32)) | 1u;
}

void MainComponent::moveClipNotesInBackground (const PitchEditor::Source& source, const PitchCorrection::Track& track,
                                               const std::vector<PitchCorrection::Note>& notes)
{
    const auto state = engine.session.getState();
    const auto clipId = pitchClipId;
    const auto where = state->findClip (clipId);

    if (! where.has_value() || backgroundJob != nullptr || clipPitchVersion (clipId) != source.version)
        return;

    engine.getPlayer().pause();
    const auto clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];
    const auto folder = recordingsFolder();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (clip.name) + " Notes", ".wav", false);
    auto part = std::make_shared<AudioDocument::Snapshot>();
    auto error = std::make_shared<juce::String>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Moving the notes...",
        [this, source, track, notes, clip, folder, file, part, error] (const std::function<bool (float)>& progress)
        {
            const auto moved = PitchCorrection::moveNotes (*source.audio, source.start, source.end, source.sampleRate, track, notes, progress);
            *part = SessionEdits::playedPart (clip, moved);

            if (*part == nullptr)
                return false;

            *error = ! folder.createDirectory() ? "Could not make the folder " + folder.getFullPathName()
                                                : AudioFileIO::save (**part, source.sampleRate, file, 32, engine.formatManager);
            return true;
        },
        [this, clipId, source, file, part, error]
        {
            auto* panel = dynamic_cast<PitchEditor*> (madePanels.count (Panels::pitch) > 0 ? madePanels[Panels::pitch].get() : nullptr);

            if (error->isNotEmpty())
            {
                showError ("Pitch Editor", *error);
                return;
            }

            if (clipPitchVersion (clipId) != source.version)
            {
                showError ("Pitch Editor", "The clip changed meanwhile, so the result was discarded.");
                return;
            }

            auto next = *engine.session.getState();

            // The other takes stay as they were: the notes moved are the active take's.
            if (*part == nullptr || ! SessionEdits::replaceActiveTake (next, clipId, file, *part))
                return;

            engine.session.apply (std::move (next), "Move Notes");

            if (panel != nullptr)
                panel->clear ("Applied. Analyse again to move more.");
        });
    backgroundJob->launchThread();
}

void MainComponent::askForPitchCorrection()
{
    auto* settings = appProperties.getUserSettings();

    showEffectDialog ("Pitch Correction",
                      "For one voice or instrument at a time: finds its pitch every 5 ms (60 to 1,000 Hz) and moves it to the "
                      "nearest note of the scale (A = 440 Hz), keeping its length. The speed is how long a correction takes "
                      "to settle: 0 moves it at once, slower keeps more of the slides and vibrato. Breaths, consonants and "
                      "noise are left as they are.",
                      { { "key", "Key", (double) juce::jlimit (0, 11, settings->getIntValue ("tuneKey", 0)), 0,
                          { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },
                        { "scale", "Scale", (double) juce::jlimit (0, 2, settings->getIntValue ("tuneScale", 0)), 0, { "Chromatic (every note)", "Major", "Minor" } },
                        { "speed", "Speed (ms)", settings->getDoubleValue ("tuneSpeed", 20.0), 0, {}, 0.0, 500.0, 5.0 },
                        { "strength", "Strength (%)", settings->getDoubleValue ("tuneStrength", 100.0), 0, {}, 0.0, 100.0, 1.0 } },
                      "spectrablade:pitch-correction", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings] (const EffectValues& v)
                      {
                          settings->setValue ("tuneKey", (int) v.at ("key"));
                          settings->setValue ("tuneScale", (int) v.at ("scale"));
                          settings->setValue ("tuneSpeed", v.at ("speed"));
                          settings->setValue ("tuneStrength", v.at ("strength"));

                          if (v.at ("strength") > 0.0)
                              runStepInBackground ({ "pitchCorrect", v, nullptr }, "Correcting the pitch...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "pitchCorrect", v, nullptr }); },
                      "Correct");
}

void MainComponent::askForPhaseCorrection()
{
    auto* settings = appProperties.getUserSettings();
    const auto toStep = [] (const EffectValues& v)
    {
        return Sequences::Step { "phase", { { "most", v.at ("most") }, { "turnOver", v.at ("polarity") < 0.5 ? 1.0 : 0.0 } }, nullptr };
    };

    // The offset is found across the whole selection, so the preview processes all of it.
    showEffectDialog ("Automatic Phase Correction",
                      "Finds how far the right channel is late or early on the left (as with two microphones at different "
                      "distances, or a tape head out of line), to a small fraction of a frame, and moves each channel half of "
                      "that so they line up. It looks up to the most offset either way.",
                      { { "most", "Most offset (ms)", settings->getDoubleValue ("phaseMost", 5.0), 1, {}, PhaseCorrection::shortestMs, PhaseCorrection::longestMs, 0.1 },
                        { "polarity", "Polarity", settings->getBoolValue ("phaseTurnOver", true) ? 0.0 : 1.0, 0,
                          { "Turn over a channel found upside down", "Leave a channel found upside down" } } },
                      "spectrablade:phase-correction", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings, toStep] (const EffectValues& v)
                      {
                          settings->setValue ("phaseMost", v.at ("most"));
                          settings->setValue ("phaseTurnOver", v.at ("polarity") < 0.5);
                          runStepInBackground (toStep (v), "Lining up the channels...");
                      },
                      [this, toStep] (const EffectValues& v) { return previewOf (toStep (v), true); },
                      "Correct");
}

void MainComponent::askForCentreChannel()
{
    auto* settings = appProperties.getUserSettings();
    const auto toSettings = [] (const EffectValues& v)
    {
        return CentreChannel::Settings { v.at ("keep") > 0.5, (float) v.at ("low"), (float) v.at ("high"), (float) v.at ("reduction") };
    };

    showEffectDialog ("Centre Channel",
                      "Finds what is panned to the middle of the mix (a voice, dialogue, a lead instrument) band by band, "
                      "from what the two channels share, and either removes it or keeps it alone, lowering the rest. "
                      "Only the frequencies between the low and high are changed. A sound off to the side at the same "
                      "moment and pitch can't be told apart fully, so some of each stays with the other.",
                      { { "keep", "Centre", settings->getBoolValue ("centreKeep", false) ? 1.0 : 0.0, 0, { "Remove the centre", "Keep the centre alone" } },
                        { "low", "Low (Hz)", settings->getDoubleValue ("centreLow", 100.0), 0, {}, 20.0, 20000.0, 10.0 },
                        { "high", "High (Hz)", settings->getDoubleValue ("centreHigh", 8000.0), 0, {}, 20.0, 20000.0, 10.0 },
                        { "reduction", "Reduction (dB)", settings->getDoubleValue ("centreReduction", 20.0), 1, {}, 0.0, 40.0, 0.5 } },
                      "spectrablade:centre-channel", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return CentreChannel::isValid (toSettings (v)); },
                      "The low must be below the high.", nullptr, nullptr,
                      [this, settings, toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          settings->setValue ("centreKeep", s.keepCentre);
                          settings->setValue ("centreLow", s.lowHz);
                          settings->setValue ("centreHigh", s.highHz);
                          settings->setValue ("centreReduction", s.reductionDb);

                          if (s.reductionDb > 0.0f)
                              runStepInBackground ({ "centre", v, nullptr }, s.keepCentre ? "Extracting the centre..." : "Removing the centre...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "centre", v, nullptr }); });
}

void MainComponent::askToReduceReverb()
{
    auto* settings = appProperties.getUserSettings();

    showEffectDialog ("Reduce Reverb",
                      "Lowers the room's reverberation after each sound by up to the reduction, keeping the sound itself and "
                      "notes held steady. The reverb time is how long the room takes to die away: set it shorter to leave more "
                      "of the tail. The recording's own time is measured first (how its sounds typically fall over 80 ms after "
                      "they stop), and a time set longer than that is taken as that: with a longer one, every sound that stops "
                      "faster, a dry voice's syllables included, would read as a tail and be cut.",
                      { { "time", "Reverb time (s)", settings->getDoubleValue ("dereverbTime", 1.0), 2, {}, 0.1, 10.0, 0.05 },
                        { "reduction", "Reduction (dB)", settings->getDoubleValue ("dereverbReduction", 12.0), 1, {}, 0.0, 30.0, 0.5 } },
                      "spectrablade:reduce-reverb", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings] (const EffectValues& v)
                      {
                          settings->setValue ("dereverbTime", v.at ("time"));
                          settings->setValue ("dereverbReduction", v.at ("reduction"));

                          if (v.at ("reduction") > 0.0)
                              runStepInBackground ({ "dereverb", v, nullptr }, "Reducing reverb...");
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "dereverb", v, nullptr }); },
                      "Reduce");
}

void MainComponent::recordStep (const Sequences::Step& step)
{
    if (! sequenceRecorder.isRecording())
        return;

    sequenceRecorder.add (step);
    sequencePosition = engine.document.getPosition();
    sequenceResetCount = engine.document.getResetCount();
    refreshTitle();
}

void MainComponent::noticeUnrecordedEdits()
{
    auto& doc = engine.document;

    if (! sequenceRecorder.isRecording())
        return;

    // Another file: what it has done before isn't part of the recording.
    if (doc.getResetCount() != sequenceResetCount)
    {
        sequenceResetCount = doc.getResetCount();
        sequencePosition = doc.getPosition();
        return;
    }

    const auto position = doc.getPosition();

    if (position > sequencePosition)
    {
        const auto history = doc.getHistory();

        for (int i = sequencePosition + 1; i <= position && i < history.size(); ++i)
            sequenceRecorder.skip (history[i]);
    }
    else if (position < sequencePosition)
    {
        sequenceRecorder.skip ("Undo " + doc.getRedoDescription());
    }

    sequencePosition = position;
}

void MainComponent::toggleSequenceRecording()
{
    if (! sequenceRecorder.isRecording())
    {
        sequenceRecorder.start();
        sequencePosition = engine.document.getPosition();
        sequenceResetCount = engine.document.getResetCount();
        refreshTitle();
        commands.commandStatusChanged();
        return;
    }

    const auto skipped = sequenceRecorder.skipped();
    auto sequence = sequenceRecorder.stop();
    refreshTitle();
    commands.commandStatusChanged();

    juce::String unrecorded;

    if (! skipped.isEmpty())
        unrecorded = "\n\nNot recorded (these commands can't be): " + skipped.joinIntoString (", ") + ".";

    if (sequence.steps.empty())
    {
        showNote ("Record Sequence", "No steps were recorded." + unrecorded);
        return;
    }

    juce::StringArray lines;

    for (auto& step : sequence.steps)
        lines.add (step.name() + (step.settingsText().isNotEmpty() ? " (" + step.settingsText() + ")" : juce::String()));

    auto* box = new juce::AlertWindow ("Save Sequence", juce::String ((int) sequence.steps.size()) + (sequence.steps.size() == 1 ? " step: " : " steps: ")
                                                             + lines.joinIntoString ("; ") + "." + unrecorded,
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("name", "Sequence " + juce::String (Sequences::userStore().names().size() + 1), "Name");
    box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Discard", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, sequence] (int result) mutable
    {
        if (result != 1)
            return;

        sequence.name = box->getTextEditorContents ("name").trim();

        if (sequence.name.isEmpty())
        {
            showError ("Save Sequence", "A sequence needs a name.");
            return;
        }

        const auto save = [sequence]
        {
            if (! Sequences::userStore().save (sequence))
                showError ("Save Sequence", "The sequence couldn't be saved.");
        };

        if (! Sequences::userStore().contains (sequence.name))
        {
            save();
            return;
        }

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Save Sequence",
                                            "There is already a sequence called " + sequence.name + ". Replace it?", "Replace", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([save] (int replace) { if (replace == 1) save(); }));
    }), true);
}

void MainComponent::choosePlaySequence()
{
    auto& store = Sequences::userStore();
    const auto names = store.names();

    if (names.isEmpty())
    {
        showNote ("Play Sequence", "There are no sequences yet. Effects > Record Sequence records one: the effects you apply until you "
                                   "stop it, with their settings.");
        return;
    }

    auto* box = new juce::AlertWindow ("Play Sequence", "It plays on the selection, or on the whole file without one, as one step to undo.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addComboBox ("sequence", names, "Sequence");
    auto* steps = new juce::TextEditor();
    steps->setMultiLine (true);
    steps->setReadOnly (true);
    steps->setSize (460, 160);
    box->addCustomComponent (steps);

    auto* combo = box->getComboBoxComponent ("sequence");
    const auto show = [combo, steps]
    {
        juce::StringArray lines;

        if (const auto seq = Sequences::userStore().load (combo->getText()))
            for (auto& step : seq->steps)
                lines.add (step.name() + (step.settingsText().isNotEmpty() ? " (" + step.settingsText() + ")" : juce::String()));
        else
            lines.add ("This sequence can't be read.");

        steps->setText (lines.joinIntoString ("\n"));
    };
    combo->onChange = show;
    combo->setSelectedItemIndex (juce::jmax (0, names.indexOf (appProperties.getUserSettings()->getValue ("lastSequence"))));
    show();

    box->addButton ("Play", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Delete", 2);
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, steps] (int result)
    {
        const auto name = box->getComboBoxComponent ("sequence")->getText();
        delete steps;

        if (result == 2)
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete Sequence", "Delete " + name + "?", "Delete", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([name] (int sure)
                                                {
                                                    if (sure == 1)
                                                        Sequences::userStore().remove (name);
                                                }));
            return;
        }

        if (result != 1)
            return;

        appProperties.getUserSettings()->setValue ("lastSequence", name);

        if (const auto seq = Sequences::userStore().load (name))
            playSequence (*seq);
        else
            showError ("Play Sequence", name + " can't be read: it may hold a step from a later version.");
    }), true);
}

void MainComponent::playSequence (const Sequences::Sequence& sequence)
{
    auto& doc = engine.document;

    if (! doc.hasAudio() || backgroundJob != nullptr || sequence.steps.empty())
        return;

    engine.getPlayer().pause();
    const auto range = selectionOrAll();
    const auto hadSelection = ! waveform.getSelection().isEmpty();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto version = doc.getVersion();
    const auto markers = doc.getMarkers();
    auto result = std::make_shared<Sequences::Result>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Playing " + sequence.name + "...",
        [this, result, sequence, snapshot, range, rate, markers] (const std::function<bool (float)>& progress)
        {
            *result = Sequences::run (sequence, snapshot, range.getStart(), range.getEnd(), rate, markers, &engine.pluginFormatManager, progress);
            return result->error != "Stopped.";
        },
        [this, result, sequence, version, hadSelection, rate, snapshot]
        {
            auto& d = engine.document;

            if (result->error.isNotEmpty())
            {
                showError ("Play Sequence", result->error + " Nothing was changed.");
                return;
            }

            if (d.getVersion() != version)
            {
                showError ("Play Sequence", "The audio changed while the sequence played, so the result was discarded.");
                return;
            }

            if (result->snapshot == snapshot)
            {
                showNote ("Play Sequence", "The sequence left the audio as it was." + (result->notes.isEmpty() ? juce::String()
                                                                                                                 : "\n\n" + result->notes.joinIntoString ("\n")));
                return;
            }

            const auto description = "Sequence: " + sequence.name;
            stopPreview();

            if (! juce::exactlyEqual (result->sampleRate, rate))
                d.applyNewRate (result->snapshot, result->sampleRate, result->markers, description);
            else
                d.apply (result->snapshot, description, result->markers);

            waveform.setSelection (hadSelection ? juce::Range<juce::int64> (result->start, result->end) : juce::Range<juce::int64>());

            // Played while recording, its steps go into the recording.
            for (auto& step : sequence.steps)
                recordStep (step);

            if (! result->notes.isEmpty())
                showNote ("Play Sequence", result->notes.joinIntoString ("\n"));
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

juce::String MainComponent::projectSuffix() const
{
    return projectFile != juce::File() ? " (" + projectFile.getFileNameWithoutExtension() + ")" : juce::String();
}

void MainComponent::chooseProject()
{
    const auto start = projectFile != juce::File() ? projectFile.getParentDirectory() : engine.document.getFile().getParentDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Open Project", start, juce::String ("*.") + Project::extension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  openProject (fc.getResult());
                          });
}

void MainComponent::openProject (const juce::File& file)
{
    if (isRecording() || backgroundJob != nullptr)
        return;

    if (const auto loaded = Project::load (file); loaded.error.isNotEmpty())
    {
        showError ("Can't open project", file.getFullPathName() + "\n\n" + loaded.error);
        return;
    }

    // Everything open is replaced: changes are asked about first.
    confirmAllFiles ([this, file] { confirmDiscardSession ([this, file] { restoreProject (file); }); });
}

void MainComponent::restoreProject (const juce::File& file)
{
    auto loaded = Project::load (file);

    if (loaded.error.isNotEmpty())
    {
        showError ("Can't open project", file.getFullPathName() + "\n\n" + loaded.error);
        return;
    }

    stopPreview();
    engine.getPlayer().pause();
    const auto restored = Project::restoreFiles (loaded.state, openFiles, engine);
    waveform.setPlace (restored.place);

    if (loaded.state.session != juce::File())
    {
        openSession (loaded.state.session);
    }
    else
    {
        // No session in the project: an empty one, as New Session leaves.
        SessionState empty;
        empty.sampleRate = engine.deviceManager.getCurrentAudioDevice() != nullptr
                               ? engine.deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                               : engine.session.getSampleRate();
        engine.clearTrackEffects();
        engine.session.reset (std::move (empty), {});
        multitrack.setCursor (0);
    }

    setMultitrack (loaded.state.multitrack);
    engine.clearRack();

    if (loaded.state.rack != nullptr)
        engine.loadRackXml (*loaded.state.rack, [] (const juce::String& errors)
        {
            if (errors.isNotEmpty())
                showError ("Some rack effects did not load", errors);
        });

    projectFile = file;
    noteRecent (file);
    refreshTitle();
    commands.commandStatusChanged();

    auto missing = loaded.missing;
    missing.addArray (restored.missing);

    if (! missing.isEmpty())
        showError ("Missing from the project", "These could not be opened, so they were left out:\n\n" + missing.joinIntoString ("\n"));
}

void MainComponent::saveProject (bool chooseFile)
{
    if (isRecording() || backgroundJob != nullptr)
        return;

    if (! chooseFile && projectFile != juce::File())
    {
        writeProject (projectFile);
        return;
    }

    const auto& doc = engine.document;
    const auto suggested = projectFile != juce::File() ? projectFile
                         : (doc.getFile() != juce::File() ? doc.getFile().withFileExtension (Project::extension)
                                                          : juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                                                .getChildFile (juce::String ("Untitled.") + Project::extension));
    chooser = std::make_unique<juce::FileChooser> ("Save Project", suggested, juce::String ("*.") + Project::extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() != juce::File())
                                  writeProject (fc.getResult().withFileExtension (Project::extension));
                          });
}

void MainComponent::writeProject (const juce::File& project)
{
    if (backgroundJob != nullptr)
        return;

    const auto copies = Project::copiesNeeded (openFiles, project);

    // Each file's undo history, unless turned off: its steps as they are now.
    const auto keepHistory = appProperties.getUserSettings()->getBoolValue (projectHistoryKey, true);
    std::vector<std::shared_ptr<Project::History>> histories ((size_t) openFiles.size());

    if (keepHistory)
        for (int i = 0; i < openFiles.size(); ++i)
            histories[(size_t) i] = Project::historyOf (openFiles.documentAt (i));

    const auto finish = [this, project, copies, histories]
    {
        Project::adoptCopies (openFiles, copies);
        const auto [session, sessionError] = Project::saveSession (engine, project);

        if (sessionError.isNotEmpty())
        {
            showError ("Save Project", "The session could not be saved: " + sessionError);
            return;
        }

        auto state = Project::gather (openFiles, waveform.getPlace(), engine, multitrackMode, session);

        Project::attachHistories (state, openFiles, histories);

        if (const auto error = Project::save (state, project); error.isNotEmpty())
        {
            showError ("Save Project", error);
            return;
        }

        projectFile = project;
        noteRecent (project);
        refreshTitle();
        commands.commandStatusChanged();
    };

    const auto anyHistory = std::any_of (histories.begin(), histories.end(), [] (auto& h) { return h != nullptr; });

    if (copies.empty() && ! anyHistory)
    {
        Project::historyFolder (project).deleteRecursively();   // none kept now
        finish();
        return;
    }

    auto error = std::make_shared<juce::String>();
    backgroundJob = std::make_unique<BackgroundJob> (*this, "Saving project...",
        [this, copies, histories, project, error] (const std::function<bool (float)>& progress)
        {
            *error = Project::writeCopies (copies, engine.formatManager, [&progress] (float p) { return progress (0.5f * p); });

            // The histories written afresh each time.
            const auto folder = Project::historyFolder (project);
            folder.deleteRecursively();

            for (size_t i = 0; i < histories.size() && error->isEmpty(); ++i)
                if (auto h = histories[i])
                    *error = Project::writeHistory (*h, folder.getChildFile (juce::String ((int) i + 1)), engine.formatManager,
                                                    [&progress, i, n = histories.size()] { return progress (0.5f + 0.5f * (float) i / (float) n); });

            return *error != "Stopped.";
        },
        [error, finish]
        {
            if (error->isNotEmpty())
                showError ("Save Project", *error);
            else
                finish();
        });
    backgroundJob->launchThread();
    commands.commandStatusChanged();
}

void MainComponent::showVideoPanel()
{
    if (! dock->isShown (Panels::video))
        dock->toggle (Panels::video);
}

void MainComponent::attachVideo()
{
    if (multitrackMode)
    {
        const auto state = engine.session.getState();
        const auto start = state->video != juce::File() ? state->video
                                                        : engine.session.getFile() != juce::File() ? engine.session.getFile().getParentDirectory()
                                                                                                   : juce::File::getSpecialLocation (juce::File::userMoviesDirectory);
        chooser = std::make_unique<juce::FileChooser> ("Attach Video to the Session", start, "*." + Video::extensions().joinIntoString (";*."));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            if (const auto video = fc.getResult(); video.existsAsFile() && Video::isVideoFile (video))
            {
                auto next = *engine.session.getState();
                next.video = video;
                engine.session.apply (std::move (next), "Attach Video");
                showVideoPanel();
                commands.commandStatusChanged();
            }
        });
        return;
    }

    if (! engine.document.hasAudio())
        return;

    const auto& doc = engine.document;
    const auto start = doc.getVideo() != juce::File() ? doc.getVideo() : doc.getFile().getParentDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Attach Video", start, "*." + Video::extensions().joinIntoString (";*."));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        if (const auto video = fc.getResult(); video.existsAsFile() && Video::isVideoFile (video))
        {
            engine.document.setVideo (video);
            showVideoPanel();
            commands.commandStatusChanged();
        }
    });
}

void MainComponent::askForVideoStart()
{
    const auto state = engine.session.getState();

    if (! multitrackMode || state->video == juce::File())
        return;

    const auto cursor = (double) multitrack.getCursor() / engine.session.getSampleRate();
    auto* box = new juce::AlertWindow ("Video Start",
                                       "Where in the session " + state->video.getFileName() + " starts: its first frame plays at this time, and "
                                           "Save Mix into Video puts the mix from here into it. The cursor is at " + juce::String (cursor, 3) + " s.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("start", juce::String (state->videoStart, 3), "Start (seconds into the session)");
    box->getTextEditor ("start")->setInputRestrictions (12, ".0123456789");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("At the Cursor", 2);
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, cursor] (int result)
    {
        if (result == 0)
            return;

        const auto start = result == 2 ? cursor : box->getTextEditorContents ("start").getDoubleValue();

        if (! std::isfinite (start) || start < 0.0)
            return;

        auto next = *engine.session.getState();
        next.videoStart = start;
        engine.session.apply (std::move (next), "Video Start");
    }), true);
}

void MainComponent::detachVideo()
{
    if (multitrackMode)
    {
        auto next = *engine.session.getState();

        if (next.video == juce::File())
            return;

        next.video = juce::File();
        next.videoStart = 0.0;
        engine.session.apply (std::move (next), "Detach Video");
    }
    else
        engine.document.setVideo ({});

    commands.commandStatusChanged();
}

void MainComponent::saveMixIntoVideo()
{
    const auto video = engine.session.getState()->video;
    const auto ffmpeg = Ffmpeg::find();

    if (! video.existsAsFile() || ffmpeg == juce::File() || backgroundJob != nullptr)
        return;

    const auto extension = video.getFileExtension();
    const auto suggested = video.getSiblingFile (video.getFileNameWithoutExtension() + " (mix)" + extension);
    chooser = std::make_unique<juce::FileChooser> ("Save Mix into Video", suggested, "*" + extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, video, ffmpeg, extension] (const juce::FileChooser& fc)
    {
        auto target = fc.getResult();

        if (target == juce::File() || backgroundJob != nullptr)
            return;

        target = target.withFileExtension (extension);

        if (target == video)
        {
            showError ("Save Mix into Video", "The new video needs a name of its own: the original is read while it is written.");
            return;
        }

        const auto state = engine.session.getState();
        const auto rate = engine.session.getSampleRate();
        auto effects = std::make_shared<std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>>();

        for (auto& track : state->tracks)
            if (auto xml = engine.getTrackEffects().createXml (track.id))
                (*effects)[track.id] = std::move (xml);

        auto inserts = std::make_shared<TrackEffects>();
        auto error = std::make_shared<juce::String>();
        auto warnings = std::make_shared<juce::String>();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Saving mix into video...",
            [this, state, rate, effects, inserts, error, warnings, ffmpeg, target] (const std::function<bool (float)>& progress)
            {
                *warnings = OfflineRender::loadTrackEffects (*effects, engine.pluginFormatManager, rate, *inserts);
                *error = Video::saveMixInto (*state, inserts.get(), ffmpeg, target, engine.formatManager, progress);
                return *error != "Stopped.";
            },
            [error, warnings, target, video]
            {
                const auto audio = Ffmpeg::audioForVideo (video);

                if (error->isNotEmpty())
                    showError ("Save Mix into Video", *error);
                else
                    showNote ("Save Mix into Video", "Written: " + target.getFileName() + ", with the picture of " + video.getFileName()
                                                         + " as it was and the mix from the video's start as " + audio.description
                                                         + (audio.kbps > 0 ? " at " + juce::String (audio.kbps) + " kbps." : juce::String ("."))
                                                         + (warnings->isNotEmpty() ? "\n\nThese effects could not be loaded and were left out:\n" + *warnings
                                                                                   : juce::String()));
            });
        backgroundJob->launchThread();
        commands.commandStatusChanged();
    });
}

void MainComponent::saveIntoVideo()
{
    if (multitrackMode)
    {
        saveMixIntoVideo();
        return;
    }

    const auto& doc = engine.document;
    const auto video = doc.getVideo();
    const auto ffmpeg = Ffmpeg::find();

    if (! doc.hasAudio() || ! video.existsAsFile() || ffmpeg == juce::File() || backgroundJob != nullptr)
        return;

    const auto extension = video.getFileExtension();
    const auto suggested = video.getSiblingFile (video.getFileNameWithoutExtension() + " (new sound)" + extension);
    chooser = std::make_unique<juce::FileChooser> ("Save Sound into Video", suggested, "*" + extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, video, ffmpeg, extension] (const juce::FileChooser& fc)
    {
        auto target = fc.getResult();
        const auto& d = engine.document;

        if (target == juce::File() || ! d.hasAudio() || backgroundJob != nullptr)
            return;

        target = target.withFileExtension (extension);   // the picture is copied into the same type of file

        if (target == video)
        {
            showError ("Save Sound into Video", "The new video needs a name of its own: the original is read while it is written.");
            return;
        }

        const auto snapshot = d.getSnapshot();
        const auto rate = d.getSampleRate();
        const auto map = d.getPictureMap();
        auto error = std::make_shared<juce::String>();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Saving sound into video...",
            [this, snapshot, rate, map, ffmpeg, video, target, error] (const std::function<bool (float)>& progress)
            {
                juce::TemporaryFile wav (".wav");
                *error = AudioFileIO::save (*snapshot, rate, wav.getFile(), 32, engine.formatManager);

                if (error->isEmpty() && progress (0.3f))
                    *error = Ffmpeg::replaceAudio (ffmpeg, video, wav.getFile(), target, [&progress] { return progress (0.3f); },
                                                   map, (double) snapshot->getNumFrames() / rate);

                return *error != "Stopped.";
            },
            [error, target, video, recut = ! map.isIdentity()]
            {
                const auto audio = Ffmpeg::audioForVideo (video);

                if (error->isNotEmpty())
                    showError ("Save Sound into Video", *error);
                else
                    showNote ("Save Sound into Video", "Written: " + target.getFileName() + ", with the picture of " + video.getFileName()
                                                         + (recut ? " cut to follow the edits (and encoded again)" : " as it was")
                                                         + " and this sound as " + audio.description
                                                         + (audio.kbps > 0 ? " at " + juce::String (audio.kbps) + " kbps." : juce::String (".")));
            });
        backgroundJob->launchThread();
        commands.commandStatusChanged();
    });
}

void MainComponent::saveChannels()
{
    const auto& doc = engine.document;

    if (! doc.hasAudio() || doc.getNumChannels() < 2 || backgroundJob != nullptr)
        return;

    const auto start = doc.getFile() != juce::File() ? doc.getFile().getParentDirectory()
                                                     : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Save Channels as Mono Files: the folder to write into", start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();
        const auto& d = engine.document;

        if (! folder.isDirectory() || ! d.hasAudio() || backgroundJob != nullptr)
            return;

        const auto snapshot = d.getSnapshot();
        const auto rate = d.getSampleRate();
        const auto markers = d.getMarkers();
        const auto info = d.getInfo();
        const auto stem = d.getFile() != juce::File() ? d.getFile().getFileNameWithoutExtension() : juce::String ("Untitled");
        const auto depths = AudioFileIO::bitDepthsFor (folder.getChildFile ("x.wav"), engine.formatManager);
        const auto depth = depths.contains (d.getSourceBitDepth()) ? d.getSourceBitDepth() : 24;
        auto written = std::make_shared<juce::StringArray>();
        auto error = std::make_shared<juce::String>();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Saving channels...",
            [this, snapshot, rate, markers, info, stem, depth, folder, written, error] (const std::function<bool (float)>& progress)
            {
                const auto channels = snapshot->getNumChannels();

                for (int ch = 0; ch < channels; ++ch)
                {
                    if (! progress ((float) ch / (float) channels))
                        return false;

                    const auto name = Surround::channelFileName (stem, channels, ch);
                    auto target = folder.getChildFile (name + ".wav");

                    for (int n = 2; target.exists(); ++n)
                        target = folder.getChildFile (name + " " + juce::String (n) + ".wav");

                    *error = AudioFileIO::save (*AudioEdits::extractChannel (*snapshot, ch), rate, target, depth, engine.formatManager, markers, info);

                    if (error->isNotEmpty())
                        return true;

                    written->add (target.getFileName());
                }

                return true;
            },
            [written, error, folder]
            {
                if (error->isNotEmpty())
                    showError ("Save Channels as Mono Files", *error + (written->isEmpty() ? juce::String() : "\n\nWritten before that: " + written->joinIntoString (", ")));
                else
                    showNote ("Save Channels as Mono Files", "Written in " + folder.getFileName() + ": " + written->joinIntoString (", ") + ".");
            });
        backgroundJob->launchThread();
        commands.commandStatusChanged();
    });
}

void MainComponent::newFileFromChannels()
{
    if (backgroundJob != nullptr || isRecording())
        return;

    const auto start = engine.document.getFile() != juce::File() ? engine.document.getFile().getParentDirectory()
                                                                 : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("New File from Mono Files: choose the files", start, openWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        const auto files = Surround::orderForCombining (fc.getResults());

        if (files.isEmpty() || backgroundJob != nullptr)
            return;

        if (files.size() < 2)
        {
            showError ("New File from Mono Files", "Choose two or more files: their channels go side by side, in name order "
                                                   "(or 5.1 order, for six named ... L, R, C, LFE, Ls and Rs, or 7.1 order, for eight named ... L, R, C, LFE, Lrs, Rrs, Lss and Rss).");
            return;
        }

        auto loaded = std::make_shared<std::vector<AudioFileIO::Loaded>>();

        backgroundJob = std::make_unique<BackgroundJob> (*this, "Reading files...",
            [this, files, loaded] (const std::function<bool (float)>& progress)
            {
                for (int i = 0; i < files.size(); ++i)
                {
                    if (! progress ((float) i / (float) files.size()))
                        return false;

                    loaded->push_back (AudioFileIO::load (files[i], engine.formatManager));
                }

                return true;
            },
            [this, files, loaded] { combineLoaded (files, *loaded); });
        backgroundJob->launchThread();
        commands.commandStatusChanged();
    });
}

void MainComponent::combineLoaded (const juce::Array<juce::File>& files, const std::vector<AudioFileIO::Loaded>& loaded)
{
    std::vector<const AudioSnapshot*> parts;
    int channels = 0, depth = 0;
    juce::int64 shortest = std::numeric_limits<juce::int64>::max(), longest = 0;

    for (size_t i = 0; i < loaded.size(); ++i)
    {
        const auto& l = loaded[i];
        const auto name = files[(int) i].getFileName();

        if (l.snapshot == nullptr)
        {
            showError ("New File from Mono Files", name + ": " + l.error);
            return;
        }

        if (! juce::exactlyEqual (l.sampleRate, loaded[0].sampleRate))
        {
            showError ("New File from Mono Files", name + " is at " + juce::String (l.sampleRate / 1000.0, 1) + " kHz and "
                                                       + files[0].getFileName() + " at " + juce::String (loaded[0].sampleRate / 1000.0, 1)
                                                       + " kHz. Convert them to one rate first.");
            return;
        }

        parts.push_back (l.snapshot.get());
        channels += l.snapshot->getNumChannels();
        depth = juce::jmax (depth, l.bitDepth);
        shortest = juce::jmin (shortest, l.snapshot->getNumFrames());
        longest = juce::jmax (longest, l.snapshot->getNumFrames());
    }

    if (channels > SessionTrack::maxDeviceOutputs)
    {
        showError ("New File from Mono Files", "That makes " + juce::String (channels) + " channels; a file here has at most "
                                                   + juce::String (SessionTrack::maxDeviceOutputs) + ".");
        return;
    }

    auto combined = AudioEdits::combineChannels (parts);

    if (combined == nullptr)
        return;

    // An empty file first, then the channels as one step, so the new file has changes to save.
    AudioFileIO::Loaded blank;
    blank.snapshot = AudioEdits::fromBuffer (juce::AudioBuffer<float> (channels, 0));
    blank.sampleRate = loaded[0].sampleRate;
    blank.bitDepth = depth;
    stopPreview();
    openFiles.prepareForAnother (waveform.getPlace());
    engine.adoptFile ({}, std::move (blank));
    engine.document.apply (std::move (combined), "New File from " + juce::String (files.size()) + " Files");
    setMultitrack (false);
    refreshTitle();

    if (shortest != longest)
        showNote ("New File from Mono Files", "The files aren't the same length: the shorter ones end in silence.");
}

void MainComponent::showFileInfo()
{
    const auto& doc = engine.document;
    const auto current = doc.getInfo();

    struct Field { const char* key; const char* label; juce::String FileInfo::* field; int limit; };
    static const Field fields[] {
        { "title", "Title", &FileInfo::title, 0 }, { "artist", "Artist", &FileInfo::artist, 0 }, { "album", "Album", &FileInfo::album, 0 },
        { "genre", "Genre", &FileInfo::genre, 0 }, { "year", "Year", &FileInfo::year, 0 }, { "track", "Track (3 or 3/12)", &FileInfo::track, 0 },
        { "comment", "Comment", &FileInfo::comment, 0 },
        { "description", "Broadcast: description", &FileInfo::description, 256 }, { "originator", "Broadcast: originator", &FileInfo::originator, 32 },
        { "reference", "Broadcast: originator reference", &FileInfo::originatorReference, 32 },
        { "date", "Broadcast: origination date (yyyy-mm-dd)", &FileInfo::originationDate, 10 },
        { "time", "Broadcast: origination time (hh:mm:ss)", &FileInfo::originationTime, 8 } };

    auto* box = new juce::AlertWindow ("File Info", "WAV, AIFF and FLAC files, and MP3 and AAC exports, keep the cover and the first seven; "
                                                    "WAV files keep the broadcast details too (in their bext chunk); exports through ffmpeg keep "
                                                    "the first seven. Time reference: " + juce::String (current.timeReference) + " frames since midnight.",
                                       juce::MessageBoxIconType::NoIcon);
    auto picker = std::make_shared<CoverPicker> (current.cover);
    box->addCustomComponent (picker.get());

    for (const auto& f : fields)
    {
        box->addTextEditor (f.key, current.*f.field, f.label);

        if (f.limit > 0)
            box->getTextEditor (f.key)->setInputRestrictions (f.limit);
    }

    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, current, picker] (int result)
    {
        if (result != 1)
            return;

        auto next = current;

        for (const auto& f : fields)
            next.*f.field = box->getTextEditorContents (f.key).trim();

        next.cover = picker->getCover();

        if (next != current)
            engine.document.setInfo (next, "File Info");
    }), true);
}

void MainComponent::askForAudioTarget (const juce::String& title, const juce::File& suggested,
                                       std::function<void (const juce::File&, int)> chosen)
{
    chooser = std::make_unique<juce::FileChooser> (title, suggested, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, title, chosen] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File())
                              {
                                  chosen ({}, 0);
                                  return;
                              }

                              if (! file.hasFileExtension ("wav;aif;aiff;flac"))
                                  file = file.withFileExtension ("wav");

                              const auto depths = AudioFileIO::bitDepthsFor (file, engine.formatManager);

                              if (depths.isEmpty())
                              {
                                  showError (title, "SpectraBlade can't write " + file.getFileExtension() + " files.");
                                  chosen ({}, 0);
                                  return;
                              }

                              auto* box = new juce::AlertWindow (title, "Sample format for " + file.getFileName(),
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
                                  [box, file, depths, chosen] (int result)
                                  {
                                      if (result != 1)
                                          chosen ({}, 0);
                                      else
                                          chosen (file, depths[box->getComboBoxComponent ("depth")->getSelectedItemIndex()]);
                                  }), true);
                          });
}

void MainComponent::writeFile (const juce::File& file, int bitDepth, std::function<void (bool)> done)
{
    auto& doc = engine.document;
    const auto error = AudioFileIO::save (*doc.getSnapshot(), doc.getSampleRate(), file, bitDepth, engine.formatManager,
                                          doc.getMarkers(), doc.getInfo());

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
        else if (doc.getInfo().hasTags() && ! AudioFileIO::canStoreInfo (file))
            showError ("File info not saved", file.getFileExtension().toUpperCase().trimCharactersAtStart (".")
                                                  + " files here don't keep the title, artist and so on. Save as WAV, AIFF or FLAC, or export as MP3 or AAC, to keep them.");
        else if (doc.getInfo().hasBroadcast() && ! AudioFileIO::canStoreBroadcast (file))
            showError ("Broadcast details not saved", file.getFileExtension().toUpperCase().trimCharactersAtStart (".")
                                                          + " files don't keep the broadcast-wave details (description, originator and so on); "
                                                            "the tags were saved. Save as WAV to keep them too.");
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

    stopPreview();   // an edit ends any preview, so it isn't heard through the effect twice
    engine.document.apply (std::move (next), description);
    waveform.setSelection (newSelection);
}

void MainComponent::applyEdit (AudioDocument::Snapshot next, const juce::String& description,
                               juce::Range<juce::int64> newSelection, Markers markers)
{
    if (next == nullptr)
        return;

    stopPreview();   // an edit ends any preview, so it isn't heard through the effect twice
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
    const auto& shape = waveform.getSpectralShape();
    auto& doc = engine.document;
    const auto name = std::isinf (gainDb) ? juce::String ("Remove Frequencies")
                                          : "Attenuate Frequencies " + juce::String (gainDb, 1) + " dB";

    if (! shape.isEmpty())
        applyEdit (SpectralEdit::changeShape (*doc.getSnapshot(), shape, doc.getSampleRate(), gainDb), name, sel);
    else if (! sel.isEmpty() && ! band.isEmpty())
        applyEdit (SpectralEdit::changeBand (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), band, doc.getSampleRate(), gainDb),
                   name, sel);
    else
        return;

    // Removed, there is nothing inside the drawing to play or remove again: the time selection
    // stays, the drawing goes, so Space plays the selection as it now sounds. Attenuated, the
    // drawing stays for another step or a listen to what is inside.
    if (std::isinf (gainDb))
        waveform.clearSpectralSelection();
}

void MainComponent::healSpectral()
{
    const auto sel = waveform.getSelection();
    const auto band = waveform.getSpectralBand();
    auto shape = waveform.getSpectralShape();

    // A box heals as the outline of itself.
    if (shape.isEmpty() && ! sel.isEmpty() && ! band.isEmpty())
    {
        const auto lo = std::log2 (juce::jmax (1.0, band.getStart())), hi = std::log2 (band.getEnd());
        SpectralEdit::Stroke outline;
        outline.lasso = { { (double) sel.getStart(), lo }, { (double) sel.getEnd(), lo }, { (double) sel.getEnd(), hi }, { (double) sel.getStart(), hi } };
        shape.strokes.push_back (std::move (outline));
    }

    if (shape.isEmpty())
        return;

    auto& doc = engine.document;
    const auto version = doc.getVersion();
    applyEdit (SpectralEdit::heal (*doc.getSnapshot(), shape, doc.getSampleRate()), "Heal", sel);

    if (doc.getVersion() != version)   // healed: the drawing has done its work
        waveform.clearSpectralSelection();
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
        m.start = juce::jlimit ((juce::int64) 0, doc.getNumFrames(), player.getHeardFrame());
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

void MainComponent::showGoToTime()
{
    const auto rate = multitrackMode ? engine.session.getSampleRate() : engine.document.getSampleRate();
    const auto cursor = multitrackMode ? multitrack.getCursor() : waveform.getCursor();
    auto* box = new juce::AlertWindow ("Go to Time",
                                       multitrackMode ? "Seconds (83.5), minutes and seconds (1:23.5), or bar and beat (5|3)."
                                                      : "Seconds (83.5) or minutes and seconds (1:23.5).",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("time", formatTime (cursor, rate));
    box->addButton ("Go", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, rate] (int result)
    {
        if (result != 1)
            return;

        const auto typed = box->getTextEditorContents ("time");
        const auto state = engine.session.getState();
        const auto map = state->tempoMap();
        const auto frame = TimeEntry::parse (typed, rate, multitrackMode ? &map : nullptr);

        if (! frame)
            showError ("Go to Time", "\"" + typed.trim() + "\" is not a time this can go to.");
        else if (multitrackMode)
            multitrack.showFrame (*frame);
        else
            waveform.showFrame (juce::jmin (*frame, engine.document.getNumFrames()));
    }), true);
}

void MainComponent::showInsertSilence()
{
    showEffectDialog ("Insert Silence", "Length in seconds (2.5) or minutes and seconds (1:30).",
                      { EffectField::textField ("length", "Length", "1.0", "0123456789.:", 12) },
                      {}, PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues&)
                      {
                          const auto typed = effectTexts["length"];
                          const auto frames = TimeEntry::parse (typed, engine.document.getSampleRate(), nullptr);

                          if (! frames || *frames <= 0 || *frames >= std::numeric_limits<int>::max())
                              showError ("Insert Silence", "\"" + typed + "\" is not a length that can be inserted.");
                          else
                              insertSilence (*frames);
                      },
                      nullptr, "Insert");
}

void MainComponent::insertGenerated (const juce::AudioBuffer<float>& audio, const juce::String& name)
{
    auto& doc = engine.document;

    if (! doc.hasAudio() || audio.getNumSamples() == 0)
        return;

    const auto sel = waveform.getSelection();
    const auto at = sel.isEmpty() ? waveform.getCursor() : sel.getStart();
    const auto frames = (juce::int64) audio.getNumSamples();

    if (auto next = Generator::insert (*doc.getSnapshot(), at, audio))
        applyEdit (std::move (next), name, { at, at + frames }, MarkerEdits::afterReplace (doc.getMarkers(), at, at, frames));
}

std::optional<EffectPreview::Request> MainComponent::generatedPreview (const juce::String& kind, const EffectValues& values,
                                                                      std::function<juce::AudioBuffer<float>()> make)
{
    if (! engine.document.hasAudio())
        return std::nullopt;

    const auto sel = waveform.getSelection();
    const auto at = sel.isEmpty() ? waveform.getCursor() : sel.getStart();
    auto key = kind + " at=" + juce::String (at);

    for (const auto& [id, value] : values)
        key << " " << id << "=" << juce::String (value, 9);

    for (const auto& [id, text] : effectTexts)
        key << " " << id << "=" << text;

    return EffectPreview::generated (key, at, std::move (make));
}

void MainComponent::showGenerateTone()
{
    const auto rate = engine.document.getSampleRate();
    const auto channels = engine.document.getSnapshot()->getNumChannels();
    const auto toTone = [] (const EffectValues& v)
    {
        Generator::Tone t;
        t.wave = (Generator::Wave) juce::jlimit (0, 3, (int) v.at ("wave"));
        t.frequency = v.at ("frequency");
        t.endFrequency = v.at ("end");
        t.levelDb = v.at ("level");
        t.fadeMs = v.at ("fade");
        return t;
    };

    showEffectDialog ("Generate Tone", "Puts a tone in at the cursor, or the start of the selection, the same on every channel, moving "
                                       "what follows on. An end frequency unlike the start sweeps evenly in octaves from one to the other. "
                                       "The level is the peak, from -96 to 0 dBFS; the ends fade over the time given (0 for none, up to "
                                       "1,000 ms). Length up to ten minutes.",
                      { { "wave", "Wave", (double) toneWave, 0, { "Sine", "Square", "Triangle", "Sawtooth" } },
                        { "frequency", "Frequency (Hz)", toneHz, 1, {}, 1, juce::jmin (20000.0, 0.5 * rate - 1.0), 1 }, { "end", "End frequency (Hz)", toneEndHz, 1, {}, 1, juce::jmin (20000.0, 0.5 * rate - 1.0), 1 },
                        { "level", "Level (dBFS peak)", toneLevelDb, 1, {}, -96, 0, 0.5 }, { "length", "Length (s)", toneSeconds, 3, {}, 0.001, 600, 0.1 },
                        { "fade", "Fade ends (ms)", toneFadeMs, 1, {}, 0, 1000, 1 } },
                      "spectrablade:generate-tone", PreviewOwner::rendered,
                      [toTone, rate] (const EffectValues& v) { return Generator::isValid (toTone (v), rate) && v.at ("length") > 0.0 && v.at ("length") <= 600.0; },
                      "The frequencies must be from 1 Hz to below " + juce::String (0.5 * rate, 0) + " Hz, the level from -96 to 0 dBFS, "
                          "the length above 0 and up to 600 s, and the fade from 0 to 1000 ms.",
                      nullptr, nullptr,
                      [this, toTone, rate, channels] (const EffectValues& v)
                      {
                          const auto t = toTone (v);
                          toneWave = (int) t.wave;
                          toneHz = t.frequency;
                          toneEndHz = t.endFrequency;
                          toneLevelDb = t.levelDb;
                          toneSeconds = v.at ("length");
                          toneFadeMs = t.fadeMs;
                          insertGenerated (Generator::tone (t, channels, (int) std::round (toneSeconds * rate), rate), "Generate Tone");
                      },
                      [this, toTone, rate, channels] (const EffectValues& v)
                      {
                          const auto t = toTone (v);
                          const auto frames = (int) std::round (v.at ("length") * rate);
                          return generatedPreview ("tone", v, [t, channels, frames, rate] { return Generator::tone (t, channels, frames, rate); });
                      });
}

void MainComponent::showGenerateTones()
{
    auto* settings = appProperties.getUserSettings();

    showEffectDialog ("Generate DTMF Tones",
                      "Puts a pair of tones for each digit in at the cursor, or the start of the selection, the same on every "
                      "channel, moving what follows on. DTMF (touch-tone) has 0-9, * and #, and A-D; MF (R1, between "
                      "exchanges) has 0-9, K for KP and S for ST. A comma is a pause as long as a tone. The level is the "
                      "pair's peak, each tone at half of it; each pair rises and falls over 2 ms.",
                      { { "kind", "Signalling", settings->getIntValue ("tonesKind", 0) == 1 ? 1.0 : 0.0, 0, { "DTMF", "MF (R1)" } },
                        EffectField::textField ("characters", "Digits", settings->getValue ("tonesCharacters", "5551234"),
                                                "0123456789*#ABCDabcdKSks,", Generator::ToneSequence::maxCharacters),
                        { "tone", "Each tone (ms)", settings->getDoubleValue ("tonesToneMs", 100.0), 0, {}, 20.0, 2000.0, 10.0 },
                        { "gap", "Between (ms)", settings->getDoubleValue ("tonesGapMs", 100.0), 0, {}, 0.0, 2000.0, 10.0 },
                        { "level", "Level (dBFS peak)", settings->getDoubleValue ("tonesLevel", -6.0), 1, {}, -96.0, 0.0, 0.5 } },
                      "spectrablade:dtmf-tones", PreviewOwner::rendered, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings] (const EffectValues& v)
                      {
                          if (! engine.document.hasAudio())
                              return;

                          const auto rate = engine.document.getSampleRate();
                          Generator::ToneSequence t;
                          t.kind = v.at ("kind") > 0.5 ? Generator::Signalling::mf : Generator::Signalling::dtmf;
                          t.characters = effectTexts["characters"].removeCharacters (" ");
                          t.toneMs = v.at ("tone");
                          t.gapMs = v.at ("gap");
                          t.levelDb = v.at ("level");

                          if (! Generator::isValid (t, rate))
                          {
                              showError ("Generate DTMF Tones", juce::String (t.kind == Generator::Signalling::dtmf ? "DTMF takes 0-9, *, # and A-D" : "MF takes 0-9, K and S")
                                                                    + " (and commas for pauses), at least one digit.");
                              return;
                          }

                          settings->setValue ("tonesKind", t.kind == Generator::Signalling::mf ? 1 : 0);
                          settings->setValue ("tonesCharacters", t.characters);
                          settings->setValue ("tonesToneMs", t.toneMs);
                          settings->setValue ("tonesGapMs", t.gapMs);
                          settings->setValue ("tonesLevel", t.levelDb);
                          insertGenerated (Generator::toneSequence (t, engine.document.getSnapshot()->getNumChannels(), rate), "Generate DTMF Tones");
                      },
                      [this] (const EffectValues& v) -> std::optional<EffectPreview::Request>
                      {
                          if (! engine.document.hasAudio())
                              return std::nullopt;

                          const auto rate = engine.document.getSampleRate();
                          const auto channels = engine.document.getSnapshot()->getNumChannels();
                          Generator::ToneSequence t;
                          t.kind = v.at ("kind") > 0.5 ? Generator::Signalling::mf : Generator::Signalling::dtmf;
                          t.characters = effectTexts["characters"].removeCharacters (" ");
                          t.toneMs = v.at ("tone");
                          t.gapMs = v.at ("gap");
                          t.levelDb = v.at ("level");

                          if (! Generator::isValid (t, rate))
                              return std::nullopt;

                          return generatedPreview ("dtmf", v, [t, channels, rate] { return Generator::toneSequence (t, channels, rate); });
                      },
                      "Generate");
}

void MainComponent::showGenerateNoise()
{
    const auto rate = engine.document.getSampleRate();
    const auto channels = engine.document.getSnapshot()->getNumChannels();
    const auto toNoise = [] (const EffectValues& v)
    {
        Generator::Noise n;
        n.colour = (Generator::Colour) juce::jlimit (0, 2, (int) v.at ("colour"));
        n.levelDb = v.at ("level");
        n.fadeMs = v.at ("fade");
        return n;
    };

    // One seed for the dialog, so what is put in starts as the preview did.
    const auto seed = juce::Random::getSystemRandom().nextInt64();

    showEffectDialog ("Generate Noise", "Puts noise in at the cursor, or the start of the selection, moving what follows on; each channel "
                                        "gets its own. White is even across frequencies, pink falls 3 dB an octave (as much in every "
                                        "octave) and brown 6 dB. The level is the RMS: white up to -6 dBFS, pink and brown up to -20, "
                                        "so the peaks stay below full scale. The ends fade over the time given. Length up to ten minutes.",
                      { { "colour", "Colour", (double) noiseColour, 0, { "White", "Pink", "Brown" } },
                        { "level", "Level (dBFS RMS)", noiseLevelDb, 1, {}, -96, -6, 0.5 }, { "length", "Length (s)", noiseSeconds, 3, {}, 0.001, 600, 0.1 },
                        { "fade", "Fade ends (ms)", noiseFadeMs, 1, {}, 0, 1000, 1 } },
                      "spectrablade:generate-noise", PreviewOwner::rendered,
                      [toNoise] (const EffectValues& v) { return Generator::isValid (toNoise (v)) && v.at ("length") > 0.0 && v.at ("length") <= 600.0; },
                      "The level must be from -96 dBFS to -6 for white noise or -20 for pink and brown, the length above 0 and up to "
                      "600 s, and the fade from 0 to 1000 ms.",
                      nullptr, nullptr,
                      [this, toNoise, rate, channels, seed] (const EffectValues& v)
                      {
                          auto n = toNoise (v);
                          n.seed = seed;
                          noiseColour = (int) n.colour;
                          noiseLevelDb = n.levelDb;
                          noiseSeconds = v.at ("length");
                          noiseFadeMs = n.fadeMs;
                          insertGenerated (Generator::noise (n, channels, (int) std::round (noiseSeconds * rate), rate), "Generate Noise");
                      },
                      [this, toNoise, rate, channels, seed] (const EffectValues& v)
                      {
                          auto n = toNoise (v);
                          n.seed = seed;
                          const auto frames = (int) std::round (v.at ("length") * rate);
                          return generatedPreview ("noise", v, [n, channels, frames, rate] { return Generator::noise (n, channels, frames, rate); });
                      });
}

void MainComponent::insertSilence (juce::int64 frames)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    const auto sel = waveform.getSelection();
    const auto at = sel.isEmpty() ? waveform.getCursor() : sel.getStart();
    applyEdit (AudioEdits::insertSilence (*doc.getSnapshot(), at, frames), "Insert Silence", { at, at + frames },
               MarkerEdits::afterReplace (doc.getMarkers(), at, at, frames));
}

void MainComponent::trimSilence (float thresholdDb)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    silenceThresholdDb = thresholdDb;
    const auto snapshot = doc.getSnapshot();
    const auto audible = AudioEdits::audibleRange (*snapshot, juce::Decibels::decibelsToGain (thresholdDb, -200.0f));
    const auto level = juce::String (thresholdDb, 1) + " dB";

    if (! audible)
        showError ("Trim Silence", "The whole file is at or below " + level + ", so nothing was trimmed.");
    else if (audible->getLength() == snapshot->getNumFrames())
        showError ("Trim Silence", "Neither end is at or below " + level + ", so nothing was trimmed.");
    else
        applyEdit (AudioEdits::crop (*snapshot, audible->getStart(), audible->getEnd()), "Trim Silence", {},
                   MarkerEdits::afterCrop (doc.getMarkers(), audible->getStart(), audible->getEnd()));
}

void MainComponent::askForSilence (const juce::String& title, std::function<void (juce::int64)> found,
                                   std::function<std::optional<EffectPreview::Request> (float, juce::int64)> rendered)
{
    const auto framesOf = [this] (const EffectValues& v)
    {
        return (juce::int64) std::llround (v.at ("length") * engine.document.getSampleRate());
    };

    showEffectDialog (title, "Silence is every channel at or below the level for at least the time.",
                      { { "level", "Level (dB)", (double) silenceThresholdDb, 1, {}, -120.0, 0.0, 1.0 },
                        { "length", "At least (s)", silenceMinSeconds, 3, {}, 0.001, 600.0, 0.05 } },
                      "spectrablade:silence", PreviewOwner::none,
                      [framesOf] (const EffectValues& v) { return framesOf (v) > 0; }, "The time must be at least one frame.", nullptr, nullptr,
                      [this, found, framesOf] (const EffectValues& v)
                      {
                          silenceThresholdDb = (float) v.at ("level");
                          silenceMinSeconds = v.at ("length");
                          found (framesOf (v));
                      },
                      rendered == nullptr ? std::function<std::optional<EffectPreview::Request> (const EffectValues&)>()
                                          : [rendered, framesOf] (const EffectValues& v) { return rendered ((float) v.at ("level"), framesOf (v)); },
                      title == "Noise Gate" ? "Apply" : "Find");
}

void MainComponent::fitRegionsToSound()
{
    if (multitrackMode)
    {
        auto next = *engine.session.getState();

        if (SessionEdits::fitRegionsToClips (next) > 0)
            engine.session.apply (std::move (next), "Fit Regions to Clips");
        else
            showError ("Fit Regions to Clips", "Every region already starts and ends with its clips, or holds none.");

        return;
    }

    auto& doc = engine.document;
    auto markers = doc.getMarkers();

    if (AudioEdits::fitRegionsToSound (*doc.getSnapshot(), markers, juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f)) > 0)
        doc.setMarkers (std::move (markers), "Fit Regions to Sound");
    else
        showError ("Fit Regions to Sound", "Every region already starts and ends with sound above " + juce::String (silenceThresholdDb, 1)
                                               + " dB, or holds none.");
}

void MainComponent::repairSelection()
{
    const auto sel = waveform.getSelection();

    if (auto next = AudioEdits::repair (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd()))
        applyEdit (std::move (next), "Repair Selection", sel);
    else
        showError ("Repair Selection", "Select from 1 to " + juce::String (AudioEdits::maxRepairFrames)
                                         + " frames with at least two frames of audio on each side.");
}

void MainComponent::noiseGate (juce::int64 minFrames)
{
    auto& doc = engine.document;
    const auto r = selectionOrAll();

    if (r.isEmpty())
        return;

    const auto fade = juce::roundToInt (doc.getSampleRate() * gateFadeSeconds);

    if (auto next = AudioEdits::gateQuiet (*doc.getSnapshot(), r.getStart(), r.getEnd(), juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f),
                                           minFrames, fade))
        applyEdit (std::move (next), "Noise Gate", waveform.getSelection());
    else
        showError ("Noise Gate", "Nothing there stays at or below " + juce::String (silenceThresholdDb, 1) + " dB for that long.");
}

void MainComponent::shortenPauses (juce::int64 keepFrames)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    // Pauses longer than the time are found, then each cut down to it, last first so earlier
    // frames keep their places while the later cuts are made.
    auto snapshot = doc.getSnapshot();
    const auto cuts = AudioEdits::pauseCuts (AudioEdits::pauses (*snapshot, juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f),
                                                                 keepFrames + 1, std::numeric_limits<size_t>::max()),
                                             keepFrames);

    if (cuts.empty())
    {
        showError ("Shorten Pauses", "No pause between sounds stays at or below " + juce::String (silenceThresholdDb, 1) + " dB for longer than that.");
        return;
    }

    auto markers = doc.getMarkers();

    for (auto cut = cuts.rbegin(); cut != cuts.rend(); ++cut)
    {
        snapshot = AudioEdits::remove (*snapshot, cut->getStart(), cut->getEnd());
        markers = MarkerEdits::afterReplace (markers, cut->getStart(), cut->getEnd(), 0);
    }

    applyEdit (std::move (snapshot), "Shorten Pauses", {}, std::move (markers));
}

void MainComponent::addMarkersAtSilences (juce::int64 minFrames)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    addMarkersAt (AudioEdits::quietMiddles (*doc.getSnapshot(), juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f), minFrames,
                                            (size_t) MarkerEdits::maxIntervalMarkers),
                  "Add Markers at Silences", "No pause between sounds stays at or below " + juce::String (silenceThresholdDb, 1) + " dB for that long.");
}

void MainComponent::addRegionsOverSounds (juce::int64 minFrames)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    const auto sounds = AudioEdits::soundsBetween (*doc.getSnapshot(), juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f), minFrames,
                                                   (size_t) MarkerEdits::maxIntervalMarkers);
    auto markers = doc.getMarkers();
    auto added = 0;

    for (auto sound : sounds)
    {
        if (std::any_of (markers.begin(), markers.end(), [sound] (const Marker& m) { return m.start == sound.getStart() && m.getEnd() == sound.getEnd(); }))
            continue;

        markers.push_back ({ sound.getStart(), sound.getLength(), MarkerEdits::nextName (markers, true) });
        ++added;
    }

    if (added == 0)
    {
        showError ("Add Regions over Sounds", sounds.empty() ? "The whole file is at or below " + juce::String (silenceThresholdDb, 1) + " dB."
                                                             : juce::String ("Every sound already has a region."));
        return;
    }

    MarkerEdits::sort (markers);
    doc.setMarkers (std::move (markers), "Add " + juce::String (added) + (added == 1 ? " Region" : " Regions"));
}

void MainComponent::addMarkersAtClipping()
{
    auto& doc = engine.document;

    if (doc.hasAudio())
        addMarkersAt (AudioEdits::clippingStarts (*doc.getSnapshot(), clipLevel, clipRun, (size_t) MarkerEdits::maxIntervalMarkers),
                      "Add Markers at Clipping", "No run of " + juce::String (clipRun) + " or more samples at full scale, or each already has a marker.");
}

void MainComponent::addMarkersAt (const std::vector<juce::int64>& frames, const juce::String& title, const juce::String& noneFound)
{
    auto& doc = engine.document;
    auto markers = doc.getMarkers();
    auto added = 0;

    for (auto frame : frames)
    {
        if (std::any_of (markers.begin(), markers.end(), [frame] (const Marker& m) { return ! m.isRegion() && m.start == frame; }))
            continue;

        markers.push_back ({ frame, 0, MarkerEdits::nextName (markers, false) });
        ++added;
    }

    if (added == 0)
    {
        showError (title, noneFound);
        return;
    }

    MarkerEdits::sort (markers);
    doc.setMarkers (std::move (markers), "Add " + juce::String (added) + (added == 1 ? " Marker" : " Markers"));
}

void MainComponent::findNextSilence (juce::int64 minFrames)
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    const auto sel = waveform.getSelection();
    const auto from = sel.isEmpty() ? waveform.getCursor() : sel.getEnd();
    const auto level = juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f);

    if (const auto run = AudioEdits::nextQuiet (*doc.getSnapshot(), from, level, minFrames))
    {
        waveform.setSelection (*run);
        waveform.showFrame (run->getStart());
    }
    else
    {
        showError ("Find Next Silence", "Nothing after the cursor stays at or below " + juce::String (silenceThresholdDb, 1) + " dB for that long.");
    }
}

void MainComponent::selectNextSound()
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    // From the end of the selection, so choosing it again moves on to the sound after.
    const auto sel = waveform.getSelection();
    const auto from = sel.isEmpty() ? waveform.getCursor() : sel.getEnd();
    const auto minFrames = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (silenceMinSeconds * doc.getSampleRate()));

    if (const auto sound = AudioEdits::nextSound (*doc.getSnapshot(), from, juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f), minFrames))
    {
        waveform.setSelection (*sound);
        waveform.showFrame (sound->getStart());
    }
    else
    {
        showError ("Select Next Sound", "No sound follows a silence (at or below " + juce::String (silenceThresholdDb, 1) + " dB for "
                                          + juce::String (silenceMinSeconds, 2) + " s) after the cursor.");
    }
}

void MainComponent::selectPreviousSound()
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    // Up to the start of the selection, so choosing it again moves back to the sound before.
    const auto sel = waveform.getSelection();
    const auto to = sel.isEmpty() ? waveform.getCursor() : sel.getStart();
    const auto minFrames = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (silenceMinSeconds * doc.getSampleRate()));

    if (const auto sound = AudioEdits::previousSound (*doc.getSnapshot(), to, juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f), minFrames))
    {
        waveform.setSelection (*sound);
        waveform.showFrame (sound->getStart());
    }
    else
    {
        showError ("Select Previous Sound", "No sound ends at a silence (at or below " + juce::String (silenceThresholdDb, 1) + " dB for "
                                              + juce::String (silenceMinSeconds, 2) + " s) before the cursor.");
    }
}

void MainComponent::findNextClipping()
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    // From just past the current selection, so choosing it again moves on to the next run.
    const auto sel = waveform.getSelection();
    const auto from = sel.isEmpty() ? waveform.getCursor() : sel.getEnd();

    if (const auto run = AudioEdits::nextClipping (*doc.getSnapshot(), from, clipLevel, clipRun))
    {
        waveform.setSelection (*run);
        waveform.showFrame (run->getStart());
    }
    else
    {
        showError ("Find Next Clipping", "No run of " + juce::String (clipRun) + " or more samples at full scale after the cursor.");
    }
}

void MainComponent::selectAudibleSpan()
{
    auto& doc = engine.document;

    if (! doc.hasAudio())
        return;

    // Inside the selection when there is one, so choosing it again keeps the same span.
    const auto r = selectionOrAll();

    if (const auto audible = AudioEdits::audibleRange (*doc.getSnapshot(), r.getStart(), r.getEnd(), juce::Decibels::decibelsToGain (silenceThresholdDb, -200.0f)))
        waveform.setSelection (*audible);
    else
        showError ("Select Audible Span", (waveform.getSelection().isEmpty() ? "The whole file" : "The whole selection")
                                            + juce::String (" is at or below ") + juce::String (silenceThresholdDb, 1) + " dB.");
}

void MainComponent::showSetSelection()
{
    const auto rate = engine.document.getSampleRate();
    const auto sel = waveform.getSelection();
    auto* box = new juce::AlertWindow ("Set Selection", "Seconds (83.5) or minutes and seconds (1:23.5).",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("start", formatTime (sel.isEmpty() ? waveform.getCursor() : sel.getStart(), rate), "Start");
    box->addTextEditor ("length", formatTime (sel.getLength(), rate), "Length");
    box->addButton ("Select", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, rate] (int result)
    {
        if (result != 1)
            return;

        if (const auto range = TimeEntry::parseRange (box->getTextEditorContents ("start"), box->getTextEditorContents ("length"),
                                                      rate, engine.document.getNumFrames()))
        {
            waveform.setSelection (*range);
            waveform.showFrame (range->getStart());
        }
        else
        {
            showError ("Set Selection", "That start and length don't name part of the file: the start must be before its end and the length more than 0.");
        }
    }), true);
}

void MainComponent::showBalance()
{
    showEffectDialog ("Balance", "From -100 (all left) to 100 (all right). The side moved away from is lowered.",
                      { { "pan", "Balance", 0.0, 0, {}, -100.0, 100.0, 1.0 } },
                      "spectrablade:balance", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v) { runStep ({ "balance", v, nullptr }); },
                      [this] (const EffectValues& v) { return previewOf ({ "balance", v, nullptr }); });
}

void MainComponent::showStereoWidth()
{
    // Two channels the same over the range have no difference to scale: say so rather than do nothing.
    const auto alike = [this]
    {
        const auto r = selectionOrAll();
        const auto snapshot = engine.document.getSnapshot();
        juce::AudioBuffer<float> block (2, 65536);

        for (auto at = r.getStart(); at < r.getEnd(); at += block.getNumSamples())
        {
            const auto n = (int) juce::jmin ((juce::int64) block.getNumSamples(), r.getEnd() - at);
            snapshot->read (block, 0, at, n);

            if (std::memcmp (block.getReadPointer (0), block.getReadPointer (1), (size_t) n * sizeof (float)) != 0)
                return false;
        }

        return true;
    }();

    showEffectDialog ("Stereo Width", juce::String ("0 % makes it mono, 100 % leaves it as it is, and up to 200 % widens it, "
                                                    "by scaling the difference between the channels.")
                                          + (alike ? " The two channels here are the same, so there is no difference to scale: nothing would change." : ""),
                      { { "width", "Width (%)", (double) stereoWidthPercent, 0, {}, 0.0, 200.0, 1.0 } },
                      "spectrablade:stereo-width", PreviewOwner::none, [alike] (const EffectValues&) { return ! alike; },
                      "The two channels are the same here (a mono sound in a stereo file), so there is no stereo width to change.", nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          stereoWidthPercent = juce::roundToInt (v.at ("width"));
                          runStep ({ "stereoWidth", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "stereoWidth", v, nullptr }); });
}

void MainComponent::showPanRamp()
{
    // The ramp spans the whole selection, so the preview processes all of it.
    showEffectDialog ("Pan Ramp", "The balance moves evenly from the first position at the start of the selection to the second "
                                  "at its end, each from -100 (all left) to 100 (all right). The side moved away from is lowered.",
                      { { "from", "Start", -100.0, 0, {}, -100.0, 100.0, 1.0 }, { "to", "End", 100.0, 0, {}, -100.0, 100.0, 1.0 } },
                      "spectrablade:pan-ramp", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          const auto sel = waveform.getSelection();

                          if (! sel.isEmpty())
                              if (auto next = AudioEdits::balanceRamp (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd(),
                                                                       (float) v.at ("from") / 100.0f, (float) v.at ("to") / 100.0f))
                                  applyEdit (std::move (next), "Pan Ramp", sel);
                      },
                      [] (const EffectValues& v)
                      {
                          const auto from = (float) v.at ("from") / 100.0f, to = (float) v.at ("to") / 100.0f;
                          return std::optional<EffectPreview::Request> ({ "panRamp " + juce::String (from, 3) + " " + juce::String (to, 3),
                              [from, to] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { AudioEdits::balanceRamp (*audio, start, end, from, to), start, end, {} };
                              }, true });
                      });
}

void MainComponent::showRepeatSelection()
{
    auto* box = new juce::AlertWindow ("Repeat Selection", "How many copies to put after the selection (1 to " + juce::String (maxRepeats) + ").",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("times", "1", "Copies");
    box->getTextEditor ("times")->setInputRestrictions (3, "0123456789");
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        const auto times = juce::jlimit (1, maxRepeats, box->getTextEditorContents ("times").getIntValue());
        const auto sel = waveform.getSelection();
        auto& doc = engine.document;

        if (auto next = AudioEdits::repeatRange (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), times))
            applyEdit (std::move (next), "Repeat Selection", { sel.getEnd(), sel.getEnd() + sel.getLength() * times },
                       MarkerEdits::afterReplace (doc.getMarkers(), sel.getEnd(), sel.getEnd(), sel.getLength() * times));
    }), true);
}

void MainComponent::showCrossfadeLoop()
{
    const auto rate = engine.document.getSampleRate();
    const auto framesOf = [rate] (const EffectValues& v) { return (juce::int64) std::llround (v.at ("ms") / 1000.0 * rate); };
    const auto invalid = juce::String ("The crossfade must be longer than 0, at most half the selection, and no longer than the audio before it.");

    // The preview loops the selection as it would be, seam and all.
    showEffectDialog ("Crossfade Loop", "The end of the selection fades into the audio just before it, over this many "
                                        "milliseconds, so the selection loops seamlessly. It needs that much audio before the selection.",
                      { { "ms", "Crossfade (ms)", 50.0, 1, {}, 0.1, 10000.0, 5.0 } },
                      "spectrablade:crossfade-loop", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, framesOf, invalid] (const EffectValues& v)
                      {
                          const auto sel = waveform.getSelection();

                          if (auto next = AudioEdits::crossfadeLoop (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd(), framesOf (v)))
                              applyEdit (std::move (next), "Crossfade Loop", sel);
                          else
                              showError ("Crossfade Loop", invalid);
                      },
                      [framesOf, invalid] (const EffectValues& v)
                      {
                          const auto fade = framesOf (v);
                          return std::optional<EffectPreview::Request> ({ "crossfadeLoop " + juce::String (fade),
                              [fade, invalid] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double, const std::function<bool (float)>&)
                              {
                                  auto next = AudioEdits::crossfadeLoop (*audio, start, end, fade);
                                  return EffectPreview::Rendered { next, start, end, next == nullptr ? invalid : juce::String() };
                              }, true });
                      },
                      "Crossfade");
}

void MainComponent::showMoveMarkers()
{
    auto* box = new juce::AlertWindow ("Move Markers in Selection", "Move the markers and regions that start inside the selection by this time: "
                                       "seconds (0.5) or minutes and seconds (1:30), with a minus sign to move them earlier.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("offset", "0.1", "Move by");
    box->addButton ("Move", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        const auto title = juce::String ("Move Markers in Selection");
        const auto rate = multitrackMode ? engine.session.getState()->sampleRate : engine.document.getSampleRate();
        const auto offset = TimeEntry::parseOffset (box->getTextEditorContents ("offset"), rate);

        if (! offset || *offset == 0)
        {
            showError (title, "Type a time other than 0, such as 0.5 or -0.5.");
            return;
        }

        const auto describe = [] (int moved) { return "Move " + juce::String (moved) + (moved == 1 ? " Marker" : " Markers"); };
        const auto refused = "That would move a marker or region past the start or the end.";

        if (multitrackMode)
        {
            auto next = *engine.session.getState();
            const auto total = std::numeric_limits<juce::int64>::max() / 2;

            if (const auto moved = MarkerEdits::moveStartingIn (next.markers, multitrack.getTimeSelection(), *offset, total); moved > 0)
                engine.session.apply (std::move (next), describe (moved));
            else
                showError (title, refused);
        }
        else
        {
            auto markers = engine.document.getMarkers();

            if (const auto moved = MarkerEdits::moveStartingIn (markers, waveform.getSelection(), *offset, engine.document.getNumFrames()); moved > 0)
                engine.document.setMarkers (std::move (markers), describe (moved));
            else
                showError (title, refused);
        }
    }), true);
}

void MainComponent::showCropMargins()
{
    auto* box = new juce::AlertWindow ("Crop with Margins", "Keep the selection and this much audio either side of it (as far as the file goes). "
                                       "Seconds (0.5) or minutes and seconds (1:30).",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("margin", juce::String (cropMarginSeconds, 3), "Margin");
    box->addButton ("Crop", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        auto& doc = engine.document;
        const auto rate = doc.getSampleRate();
        const auto margin = TimeEntry::parse (box->getTextEditorContents ("margin"), rate, nullptr);

        if (! margin || *margin < 0)
        {
            showError ("Crop with Margins", "Type a time of 0 or more.");
            return;
        }

        cropMarginSeconds = (double) *margin / rate;
        const auto sel = waveform.getSelection();
        const auto keep = AudioEdits::withMargins (sel, *margin, doc.getNumFrames());
        applyEdit (AudioEdits::crop (*doc.getSnapshot(), keep.getStart(), keep.getEnd()), "Crop with Margins", sel - keep.getStart(),
                   MarkerEdits::afterCrop (doc.getMarkers(), keep.getStart(), keep.getEnd()));
    }), true);
}

void MainComponent::showEcho()
{
    showEffectDialog ("Echo", "Each repeat comes this long after the one before at this share of its level. The repeats stop at "
                              "the end of the selection, so leave room after the sound.",
                      { { "delay", "Delay (ms)", (double) echoDelayMs, 0, {}, 1.0, 5000.0, 10.0 },
                        { "feedback", "Feedback (%)", (double) echoFeedbackPercent, 0, {}, 0.0, 95.0, 1.0 } },
                      "spectrablade:echo", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          echoDelayMs = juce::roundToInt (v.at ("delay"));
                          echoFeedbackPercent = juce::roundToInt (v.at ("feedback"));
                          runStep ({ "echo", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "echo", v, nullptr }); });
}

void MainComponent::showRemoveHum()
{
    const auto rate = engine.document.getSampleRate();
    const auto toHum = [] (const EffectValues& v)
    {
        Restoration::Hum h;
        h.frequency = v.at ("mains") < 0.5 ? 50.0 : 60.0;
        h.harmonics = juce::roundToInt (v.at ("harmonics"));
        h.q = v.at ("q");
        return h;
    };

    showEffectDialog ("Remove Hum", "Takes out mains hum: a narrow notch at the mains frequency and at each of its harmonics up to the "
                                    "number given, each as wide in hertz as the first (the mains frequency divided by the narrowness: "
                                    "1.7 Hz at 50 Hz and 30). Harmonics 1 to 40, narrowness 5 (wide) to 200 (narrow).",
                      { { "mains", "Mains", humIs60 ? 1.0 : 0.0, 0, { "50 Hz", "60 Hz" } },
                        { "harmonics", "Harmonics", (double) humHarmonics, 0, {}, 1, 40, 1 }, { "q", "Narrowness (Q)", humQ, 0, {}, 5, 200, 1 } },
                      "spectrablade:remove-hum", PreviewOwner::none,
                      [toHum, rate] (const EffectValues& v) { return Restoration::isValid (toHum (v), rate); },
                      "The harmonics must be from 1 to 40 and the narrowness from 5 to 200.",
                      nullptr, nullptr,
                      [this, toHum, rate] (const EffectValues& v)
                      {
                          const auto hum = toHum (v);
                          humIs60 = hum.frequency > 55.0;
                          humHarmonics = hum.harmonics;
                          humQ = hum.q;
                          const auto r = selectionOrAll();

                          if (auto next = Restoration::removeHum (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), rate, hum))
                              applyEdit (std::move (next), "Remove Hum", waveform.getSelection());
                      },
                      [toHum] (const EffectValues& v)
                      {
                          const auto hum = toHum (v);
                          return std::optional<EffectPreview::Request> ({ "hum " + juce::String (hum.frequency) + " " + juce::String (hum.harmonics) + " " + juce::String (hum.q),
                              [hum] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { Restoration::removeHum (*audio, start, end, sampleRate, hum), start, end, {} };
                              } });
                      });
}

void MainComponent::showDeclip()
{
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v)
    {
        Restoration::Declip d;
        d.threshold = v.at ("threshold") / 100.0;
        d.keepPeak = v.at ("keep") < 0.5;
        return d;
    };

    showEffectDialog ("Repair Clipping", "Finds clipped peaks, two or more samples in a row held level at or above the share of each "
                                         "channel's peak given, and rebuilds each as the curve that best fits the samples on either side. "
                                         "Rebuilt peaks rise above the clipped level; Keep the peak level lowers the selection so they "
                                         "reach it, and no higher. Clipping longer than 5 ms is left as it is.",
                      { { "threshold", "Clipped at or above (% of the peak)", declipThreshold * 100.0, 1, {}, 50, 100, 0.5 },
                        { "keep", "Keep the peak level", declipKeepPeak ? 0.0 : 1.0, 0, { "Yes", "No" } } },
                      "spectrablade:declip", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { const auto d = toSettings (v); return d.threshold >= 0.5 && d.threshold <= 1.0; },
                      "The share of the peak must be from 50 to 100 %.",
                      nullptr, nullptr,
                      [this, toSettings, rate] (const EffectValues& v)
                      {
                          const auto d = toSettings (v);
                          declipThreshold = d.threshold;
                          declipKeepPeak = d.keepPeak;
                          const auto r = selectionOrAll();
                          auto result = Restoration::declip (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), rate, d);
                          const auto tooLong = result.tooLong > 0 ? "\n\n" + juce::String (result.tooLong) + " clipped stretches longer than 5 ms were left as they were."
                                                                  : juce::String();

                          if (result.audio == nullptr)
                          {
                              showNote ("Repair Clipping", "No clipped peaks were found that could be rebuilt." + tooLong);
                              return;
                          }

                          applyEdit (std::move (result.audio), "Repair Clipping", waveform.getSelection());

                          if (tooLong.isNotEmpty())
                              showNote ("Repair Clipping", juce::String (result.repaired) + " clipped peaks were rebuilt." + tooLong);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          // Keeping the peak level lowers the whole selection by what its loudest rebuilt peak needs.
                          const auto d = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "declip " + juce::String (d.threshold, 9) + (d.keepPeak ? " keep" : ""),
                              [d] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { Restoration::declip (*audio, start, end, sampleRate, d).audio, start, end, {} };
                              }, d.keepPeak });
                      });
}

void MainComponent::showEqBand()
{
    const auto nyquist = engine.document.getSampleRate() / 2.0;
    const auto toBand = [] (const EffectValues& v)
    {
        return Equalizer::Band { (Equalizer::BandType) juce::jlimit (0, 4, (int) v.at ("type")), v.at ("frequency"), v.at ("gain"), v.at ("q") };
    };

    showEffectDialog ("EQ Band", "A bell boosts or cuts around the frequency (a higher Q is narrower); a shelf boosts or cuts "
                                 "everything below or above it, half as much at the frequency; a high-pass or low-pass removes what is "
                                 "below or above it, 3 dB down at the frequency with a Q of 0.71. Gain -48 to +48 dB, Q 0.05 to 50.",
                      { { "type", "Type", (double) eqBandType, 0, { "Bell", "Low Shelf", "High Shelf", "High-Pass", "Low-Pass" } },
                        { "frequency", "Frequency (Hz)", eqBandHz, 1, {}, 20, juce::jmin (20000.0, nyquist - 1.0), 1 }, { "gain", "Gain (dB)", eqBandGainDb, 1, {}, -48, 48, 0.5 }, { "q", "Q", eqBandQ, 2, {}, 0.05, 50, 0.05 } },
                      "spectrablade:eq-band", PreviewOwner::none,
                      [this, toBand] (const EffectValues& v) { return Equalizer::isValid (toBand (v), engine.document.getSampleRate()); },
                      "The frequency must be above 0 and below " + juce::String (nyquist, 0) + " Hz, the gain within 48 dB either way, and the Q from 0.05 to 50.",
                      nullptr, nullptr,
                      [this, toBand] (const EffectValues& v)
                      {
                          const auto band = toBand (v);
                          eqBandType = (int) band.type;
                          eqBandHz = band.frequency;
                          eqBandGainDb = band.gainDb;
                          eqBandQ = band.q;
                          runStep ({ "eqBand", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "eqBand", v, nullptr }); });
}

void MainComponent::toggleParametricEq()
{
    // Deleted later, not inside its own close button's callback.
    if (eqWindow != nullptr)
    {
        if (previewOwner == PreviewOwner::parametricEq)
            stopPreview();

        eqWindow = nullptr;
        return;
    }

    const auto rate = engine.document.hasAudio() ? engine.document.getSampleRate() : 48000.0;
    eqWindow = std::make_unique<EqWindow> (eqSettings, eqLinearPhase, rate, [this] (const ParametricEq& eq) { applyParametricEq (eq.getActiveBands(), eq.isLinearPhase()); },
                                           [this] (const ParametricEq::Settings& s, bool linear) { eqSettings = s; eqLinearPhase = linear; },
                                           [this] (const ParametricEq* eq) { previewParametricEq (eq); },
                                           [this]
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr)
                safe->toggleParametricEq();
        });
    });
    eqWindow->setReadOutput ([this] (juce::AudioBuffer<float>& b) { engine.copyRecentOutput (b); return engine.getOutputSampleRate(); });
    places->watch (*eqWindow, "Parametric EQ");

    if (previewsFromStart())   // opened while the file plays: heard from the start
        eqWindow->startPreviewing();
}

void MainComponent::previewParametricEq (const ParametricEq* eq)
{
    if (eq == nullptr)
    {
        if (previewOwner == PreviewOwner::parametricEq)
            stopPreview();

        return;
    }

    // The live EQ with the window's bands: a new one to start, else the one playing, set again.
    const auto settings = eq->getSettings();
    auto* live = dynamic_cast<BuiltInEffects::ParametricEqProcessor*> (previewEffectOf (PreviewOwner::parametricEq));
    std::unique_ptr<BuiltInEffects::ParametricEqProcessor> made;

    if (live == nullptr)
    {
        made = std::make_unique<BuiltInEffects::ParametricEqProcessor>();
        live = made.get();
    }

    for (int i = 0; i < BuiltInEffects::ParametricEqProcessor::numBands; ++i)
        live->setBand (i, settings[(size_t) i].on, settings[(size_t) i].band);

    live->linearPhaseParameter().setValueNotifyingHost (eq->isLinearPhase() ? 1.0f : 0.0f);

    if (made != nullptr)
    {
        startPreview (PreviewOwner::parametricEq, std::move (made), [this]
        {
            if (eqWindow != nullptr)
                eqWindow->previewEnded();
        });

        if (previewOwner != PreviewOwner::parametricEq && eqWindow != nullptr)
            eqWindow->previewEnded();   // nothing to preview (no file, or the session showing)
    }
}

void MainComponent::applyParametricEq (const std::vector<Equalizer::Band>& bands, bool linearPhase)
{
    auto& doc = engine.document;

    if (! doc.hasAudio() || multitrackMode)
        return;

    if (bands.empty())
    {
        showError ("Parametric EQ", "Every band is off or at 0 dB, so there is nothing to apply.");
        return;
    }

    for (auto& band : bands)
        if (! Equalizer::isValid (band, doc.getSampleRate()))
        {
            showError ("Parametric EQ", "A band at " + juce::String (band.frequency, 0) + " Hz is at or past half this file's sample rate. "
                                        "Close and reopen the window to fit the bands to this file.");
            return;
        }

    const auto r = selectionOrAll();

    auto next = linearPhase ? Equalizer::applyLinearPhase (*doc.getSnapshot(), r.getStart(), r.getEnd(), doc.getSampleRate(), bands)
                            : Equalizer::apply (*doc.getSnapshot(), r.getStart(), r.getEnd(), doc.getSampleRate(), bands);

    if (next != nullptr)
        applyEdit (std::move (next), linearPhase ? "Parametric EQ (Linear Phase)" : "Parametric EQ", waveform.getSelection());
}

void MainComponent::showCompressor()
{
    const auto& s = compressorSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Dynamics::Compressor { v.at ("threshold"), v.at ("ratio"), v.at ("knee"), v.at ("attack"), v.at ("release"), v.at ("makeup") };
    };

    showEffectDialog ("Compressor", "Above the threshold, each ratio dB in comes out as 1 dB, eased in over the knee's width. "
                                    "The gain falls with the attack time and recovers with the release time, the same on every channel. "
                                    "Threshold -80 to 0 dB, ratio 1 to 100, knee 0 to 24 dB, attack 0.01 to 1,000 ms, release 1 to 5,000 ms, makeup within 48 dB.",
                      { { "threshold", "Threshold (dB)", s.thresholdDb, 1, {}, -80, 0, 0.5 }, { "ratio", "Ratio (to 1)", s.ratio, 1, {}, 1, 100, 0.1 }, { "knee", "Knee (dB)", s.kneeDb, 1, {}, 0, 24, 0.5 },
                        { "attack", "Attack (ms)", s.attackMs, 2, {}, 0.01, 1000, 0.1 }, { "release", "Release (ms)", s.releaseMs, 0, {}, 1, 5000, 5 }, { "makeup", "Makeup gain (dB)", s.makeupDb, 1, {}, -48, 48, 0.5 } },
                      BuiltInEffects::compressorId, PreviewOwner::compressor,
                      [toSettings] (const EffectValues& v) { return Dynamics::isValid (toSettings (v)); },
                      "A setting is out of range: threshold -80 to 0 dB, ratio 1 to 100, knee 0 to 24 dB, attack 0.01 to 1,000 ms, release 1 to 5,000 ms, makeup within 48 dB.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::CompressorProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::CompressorProcessor*> (&p);

                          if (live != nullptr)
                              live->setSettings (toSettings (v));

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          compressorSettings = toSettings (v);
                          runStep ({ "compressor", v, nullptr });
                      });
}

void MainComponent::showMultiband()
{
    auto* box = new juce::AlertWindow ("Multiband Compressor",
                                       "The sound is split into lows, middle and highs at the two crossovers (24 dB an octave), each band "
                                       "compressed on its own as the Compressor does (6 dB knee), and the bands added back up: at a ratio of 1 "
                                       "everywhere, every frequency keeps its level. The high crossover stays at least an octave above the low one. The "
                                       "band split is minimum phase to start: no delay, but it turns the phase, which can lift the peaks of a sound already "
                                       "at its ceiling (6 dB on a limited master). Linear phase splits with symmetric impulses instead, so the bands add back "
                                       "to the sound exactly as it was and its peaks hold, at the cost of 8,703 frames of delay when played live (181 ms at "
                                       "48 kHz), as the linear-phase EQ.",
                                       juce::MessageBoxIconType::NoIcon, this);
    auto panel = std::make_shared<Multiband::Panel>();
    panel->setSettings (multibandSettings);
    box->addCustomComponent (panel.get());

    auto preview = std::make_shared<juce::ToggleButton> ("Preview on the selection");
    preview->setSize (260, 24);
    box->addCustomComponent (preview.get());

    // The live multiband compressor with the panel's settings, on the selection over and over.
    const auto update = [this, p = panel.get(), toggle = preview.get()]
    {
        if (! toggle->getToggleState())
        {
            if (previewOwner == PreviewOwner::multiband)
                stopPreview();

            return;
        }

        if (auto* live = dynamic_cast<BuiltInEffects::MultibandProcessor*> (previewEffectOf (PreviewOwner::multiband)))
        {
            live->setSettings (p->getSettings());
            return;
        }

        auto made = std::make_unique<BuiltInEffects::MultibandProcessor>();
        made->setSettings (p->getSettings());
        startPreview (PreviewOwner::multiband, std::move (made), [toggle] { toggle->setToggleState (false, juce::dontSendNotification); });

        if (previewOwner != PreviewOwner::multiband)
            toggle->setToggleState (false, juce::dontSendNotification);
    };
    preview->onClick = update;
    panel->onChange = update;

    if (previewsFromStart())   // opened while the file plays: heard from the start
    {
        preview->setToggleState (true, juce::dontSendNotification);
        update();
    }

    // Presets under the live effect's parameter IDs, shared with it.
    auto presets = std::make_shared<EffectPresets::Bar> (EffectPresets::userStore(), BuiltInEffects::multibandId,
                                                         [p = panel.get()]
                                                         {
                                                             BuiltInEffects::MultibandProcessor holder;
                                                             holder.setSettings (p->getSettings());
                                                             return holder.getPresetValues();
                                                         },
                                                         [p = panel.get(), update] (const EffectPresets::Values& v)
                                                         {
                                                             BuiltInEffects::MultibandProcessor holder;
                                                             holder.setSettings (p->getSettings());
                                                             holder.setPresetValues (v);
                                                             p->setSettings (holder.getSettings());
                                                             update();
                                                         });
    box->addCustomComponent (presets.get());
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, panel, preview, presets] (int result)
    {
        if (previewOwner == PreviewOwner::multiband)
            stopPreview();

        if (result != 1)
            return;

        const auto rate = engine.document.getSampleRate();
        multibandSettings = Multiband::spaced (panel->getSettings(), rate);
        const auto r = selectionOrAll();

        if (auto next = Multiband::compress (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), rate, multibandSettings))
            applyEdit (std::move (next), "Multiband Compressor", waveform.getSelection());
    }), true);
}

void MainComponent::showDynamicEq()
{
    auto* box = new juce::AlertWindow ("Dynamic EQ",
                                       "Five bands, each a bell or a shelf that moves only while its own part of the sound is over (or under) its "
                                       "threshold, by as much as that part is past it (at the ratio), up to the band's gain: a cut at 3 kHz over "
                                       "-30 dB leaves the sound alone until the 3 kHz part gets loud. Drag a handle for its frequency and gain, the "
                                       "wheel for a bell's Q, the line on the meter for the picked band's threshold. A band at 0 dB changes nothing.",
                                       juce::MessageBoxIconType::NoIcon, this);
    const auto rate = engine.document.getSampleRate();
    auto panel = std::make_shared<DynamicEq::Panel> (dynamicEqSettings, rate);
    panel->readOutput = [this] (juce::AudioBuffer<float>& b) { engine.copyRecentOutput (b); return engine.getOutputSampleRate(); };
    box->addCustomComponent (panel.get());

    auto preview = std::make_shared<juce::ToggleButton> ("Preview on the selection");
    preview->setSize (260, 24);
    box->addCustomComponent (preview.get());

    // The live dynamic EQ with the panel's settings, on the selection over and over; its gains and
    // levels drawn on the panel as it plays.
    const auto update = [this, p = panel.get(), toggle = preview.get()]
    {
        if (! toggle->getToggleState())
        {
            if (previewOwner == PreviewOwner::dynamicEq)
                stopPreview();

            p->readLive = nullptr;
            p->setReadings ({});
            return;
        }

        if (auto* live = dynamic_cast<BuiltInEffects::DynamicEqProcessor*> (previewEffectOf (PreviewOwner::dynamicEq)))
        {
            live->setSettings (p->getSettings());
            return;
        }

        auto made = std::make_unique<BuiltInEffects::DynamicEqProcessor>();
        made->setSettings (p->getSettings());
        auto* live = made.get();
        startPreview (PreviewOwner::dynamicEq, std::move (made), [toggle, p] { toggle->setToggleState (false, juce::dontSendNotification); p->readLive = nullptr; });

        if (previewOwner != PreviewOwner::dynamicEq)
            toggle->setToggleState (false, juce::dontSendNotification);
        else
            p->readLive = [live] { return live->getReadings(); };
    };
    preview->onClick = update;
    panel->onChange = update;

    if (previewsFromStart())   // opened while the file plays: heard from the start
    {
        preview->setToggleState (true, juce::dontSendNotification);
        update();
    }

    auto presets = std::make_shared<EffectPresets::Bar> (EffectPresets::userStore(), BuiltInEffects::dynamicEqId,
                                                         [p = panel.get()]
                                                         {
                                                             BuiltInEffects::DynamicEqProcessor holder;
                                                             holder.setSettings (p->getSettings());
                                                             return holder.getPresetValues();
                                                         },
                                                         [p = panel.get(), update] (const EffectPresets::Values& v)
                                                         {
                                                             BuiltInEffects::DynamicEqProcessor holder;
                                                             holder.setSettings (p->getSettings());
                                                             holder.setPresetValues (v);
                                                             p->setSettings (holder.getParameterSettings());
                                                             update();
                                                         });
    box->addCustomComponent (presets.get());
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, panel, preview, presets] (int result)
    {
        if (previewOwner == PreviewOwner::dynamicEq)
            stopPreview();

        panel->readLive = nullptr;

        if (result != 1)
            return;

        const auto fileRate = engine.document.getSampleRate();
        dynamicEqSettings = DynamicEq::withinRate (panel->getSettings(), fileRate);
        const auto r = selectionOrAll();

        if (auto next = DynamicEq::apply (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), fileRate, dynamicEqSettings))
            applyEdit (std::move (next), "Dynamic EQ", waveform.getSelection());
    }), true);
}

void MainComponent::showMouthDeClick (bool crackle)
{
    // Click and Pop Repair with settings for the job: mouth clicks are faint and up to 8 ms long,
    // crackle fainter still and under a millisecond; remembered on their own.
    auto* settings = appProperties.getUserSettings();
    const juce::String prefix = crackle ? "crackle" : "mouthClick";
    const auto toSettings = [] (const EffectValues& v)
    {
        ClickRepair::Settings s;
        s.voice = true;
        s.thresholdDb = (float) v.at ("threshold");
        s.maxLengthMs = (float) v.at ("length");
        return s;
    };

    showEffectDialog (crackle ? "De-crackle" : "Mouth De-click",
                      crackle ? juce::String ("Finds dense tiny clicks (crackle from a worn record, a dirty contact, a dry mouth) and rebuilds those samples "
                                              "from the audio around them, as Click and Pop Repair does with a lower threshold and a shorter longest click. "
                                              "Lower the threshold to catch fainter crackle; raise it if the sound's own detail is being taken for it.")
                              : juce::String ("Finds the small clicks lips and tongue make in speech and rebuilds those samples from the audio around them, "
                                              "as Click and Pop Repair does with a lower threshold and a longer longest click. Lower the threshold to "
                                              "catch fainter clicks; raise it if consonants are being taken for clicks."),
                      { { "threshold", "Threshold (dB above the background)", settings->getDoubleValue (prefix + "Threshold", crackle ? 8.0 : 10.0), 1, {}, 3.0, 60.0, 0.5 },
                        { "length", "Longest click (ms)", settings->getDoubleValue (prefix + "MaxLength", crackle ? 0.8 : 8.0), 1, {}, 0.1, 20.0, 0.1 } },
                      crackle ? "spectrablade:de-crackle" : "spectrablade:mouth-declick", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings, prefix, toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          settings->setValue (prefix + "Threshold", s.thresholdDb);
                          settings->setValue (prefix + "MaxLength", s.maxLengthMs);
                          repairClicks (s);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "clicks " + juce::String (s.thresholdDb, 3) + " " + juce::String (s.maxLengthMs, 3),
                              [s] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  const auto r = ClickRepair::repair (*audio, start, end, sampleRate, s, progress);
                                  return EffectPreview::Rendered { r.has_value() ? r->snapshot : nullptr, start, end,
                                                                   r.has_value() ? juce::String() : juce::String ("Stopped.") };
                              } });
                      },
                      "Repair");
}

void MainComponent::showCodecPreview()
{
    const auto& s = codecPreviewSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        LossyFiles::Settings settings;
        settings.kind = juce::roundToInt (v.at ("kind")) == 1 ? LossyFiles::Kind::aac : LossyFiles::Kind::mp3;
        settings.kbps = juce::roundToInt (v.at ("kbps"));
        return settings;
    };
    juce::StringArray rates;

    for (auto r : LossyFiles::bitRates())
        rates.add (juce::String (r) + " kbps");

    const auto rateIndex = juce::jmax (0, LossyFiles::bitRates().indexOf (s.kbps));
    const auto toSettingsFromIndex = [toSettings] (const EffectValues& v)
    {
        auto values = v;
        values["kbps"] = (double) LossyFiles::bitRates()[juce::jlimit (0, LossyFiles::bitRates().size() - 1, juce::roundToInt (v.at ("rate")))];
        return toSettings (values);
    };

    showEffectDialog ("Codec Preview",
                      "The selection as it will come back from an MP3 or an AAC file at the bit rate: written through the real encoder to a temporary "
                      "file, read back, lined up with the original and put in its place, so what plays is the codec's own output. Apply keeps "
                      "that output in the file, for an A/B or to work on what a listener will hear.",
                      { { "kind", "Format", s.kind == LossyFiles::Kind::aac ? 1.0 : 0.0, 0, { "MP3", "AAC" } },
                        { "rate", "Bit rate", (double) rateIndex, 0, rates } },
                      "spectrablade:codec-preview", PreviewOwner::none,
                      [toSettingsFromIndex] (const EffectValues& v) { return toSettingsFromIndex (v).isValid(); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettingsFromIndex] (const EffectValues& v)
                      {
                          codecPreviewSettings = toSettingsFromIndex (v);
                          applyCodecPreview (codecPreviewSettings);
                      },
                      [toSettingsFromIndex] (const EffectValues& v)
                      {
                          const auto settings = toSettingsFromIndex (v);
                          return std::optional<EffectPreview::Request> ({ "codec " + juce::String (settings.kind == LossyFiles::Kind::aac ? "aac" : "mp3") + " " + juce::String (settings.kbps),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  juce::String error;
                                  const auto r = CodecPreview::roundTrip (*audio, start, end, sampleRate, settings, error, progress);
                                  return EffectPreview::Rendered { r.has_value() ? r->snapshot : nullptr, start, end, r.has_value() ? juce::String() : error };
                              } });
                      },
                      "Keep");
}

void MainComponent::applyCodecPreview (LossyFiles::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<std::optional<CodecPreview::Result>>();
    auto error = std::make_shared<juce::String>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Encoding and decoding...",
        [result, error, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = CodecPreview::roundTrip (*snapshot, range.getStart(), range.getEnd(), rate, s, *error, progress);
            return result->has_value();
        },
        [this, result, error, version, s]
        {
            if (engine.document.getVersion() != version)
                showError ("Codec Preview", "The audio changed during processing, so the result was discarded.");
            else if (! result->has_value())
            {
                if (error->isNotEmpty())
                    showError ("Codec Preview", *error);
            }
            else
                applyEdit ((*result)->snapshot, juce::String (s.kind == LossyFiles::Kind::aac ? "AAC" : "MP3") + " " + juce::String (s.kbps) + " kbps", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showAzimuth()
{
    const auto& s = azimuthSettings;
    const auto toSettings = [] (const EffectValues& v) { return Azimuth::Settings { v.at ("most"), v.at ("amount") }; };

    showEffectDialog ("Azimuth",
                      "The time offset between the two channels of a tape transfer (a head out of line with the tracks) found as it drifts and taken "
                      "out: measured over windows of two seconds, a second apart, as Automatic Phase Correction measures it, with unclear windows "
                      "filled from their neighbours and the readings smoothed; the channels are then moved by half the offset each, the late one "
                      "earlier and the other later, by a fractional delay that follows the drift sample by sample. For a transfer of a tape: on a "
                      "recording made in stereo the readings follow whatever is loudest and move with it, which is not azimuth.",
                      { { "most", "Most offset looked for (ms)", s.mostMs, 1, {}, 0.5, 50, 0.1 }, { "amount", "Amount (%)", s.amountPercent, 0, {}, 0, 100, 1 } },
                      "spectrablade:azimuth", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return Azimuth::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          azimuthSettings = toSettings (v);
                          correctAzimuth (azimuthSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "azimuth " + juce::String (settings.mostMs, 3) + " " + juce::String (settings.amountPercent, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  auto result = Azimuth::correct (*audio, start, end, sampleRate, settings, progress);
                                  return EffectPreview::Rendered { result, start, end, result == nullptr ? juce::String ("No clear offset read: two channels and at least two seconds are needed.") : juce::String() };
                              } });
                      },
                      "Correct");
}

void MainComponent::correctAzimuth (Azimuth::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Correcting the azimuth...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = Azimuth::correct (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Azimuth", "The audio changed during processing, so the result was discarded.");
            else if (*result == nullptr)
                showError ("Azimuth", "No clear offset was read: the audio must have two channels and the selection be at least two seconds, with the channels related.");
            else
                applyEdit (*result, "Azimuth", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showDialogueContour()
{
    const auto& s = contourSettings;
    const auto toSettings = [] (const EffectValues& v) { return DialogueContour::Settings { (float) v.at ("semitones"), (float) v.at ("ending"), (float) v.at ("curve") }; };

    showEffectDialog ("Dialogue Contour",
                      "The pitch of a phrase's ending reshaped: over the last stretch of pitched sound before the end of the selection, as long as "
                      "the ending asked for, the pitch is moved by a ramp from nothing at the ending's start to the full move at its end, so a line "
                      "that trails up can be made to land, or a flat ending given a lift. The curve bends the ramp: most of the move early (under 0) "
                      "or late (over 0). Everything before the ending is left as it is.",
                      { { "semitones", "Move (semitones)", s.semitones, 1, {}, -12, 12, 0.5 }, { "ending", "Ending (s)", s.endingSeconds, 2, {}, 0.1, 1.5, 0.05 },
                        { "curve", "Curve", s.curve, 2, {}, -1, 1, 0.1 } },
                      "spectrablade:dialogue-contour", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return DialogueContour::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          contourSettings = toSettings (v);
                          reshapeContour (contourSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "contour " + juce::String (settings.semitones, 3) + " " + juce::String (settings.endingSeconds, 3) + " " + juce::String (settings.curve, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  auto result = DialogueContour::apply (*audio, start, end, sampleRate, settings, progress);
                                  return EffectPreview::Rendered { result, start, end, result == nullptr ? juce::String ("No pitched sound found to reshape.") : juce::String() };
                              } });
                      },
                      "Reshape");
}

void MainComponent::reshapeContour (DialogueContour::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Reshaping the ending...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = DialogueContour::apply (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Dialogue Contour", "The audio changed during processing, so the result was discarded.");
            else if (*result == nullptr)
                showError ("Dialogue Contour", "No pitched sound was found in the selection to reshape.");
            else
                applyEdit (*result, "Dialogue Contour", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::captureAmbience()
{
    const auto sel = selectionOrAll();
    auto& doc = engine.document;
    const auto print = Ambience::capture (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), doc.getSampleRate());

    if (! print.has_value())
    {
        showError ("Capture Room Tone", "Select at least " + formatTime (Ambience::frameSize, doc.getSampleRate()) + " of room tone alone, then capture it.");
        return;
    }

    ambiencePrint = print;
    ++ambienceCaptures;
    commands.commandStatusChanged();
}

void MainComponent::showAmbienceMatch()
{
    if (! ambiencePrint.has_value())
        return;

    if (! juce::exactlyEqual (ambiencePrint->sampleRate, engine.document.getSampleRate()))
    {
        showError ("Ambience Match", "The room tone was captured at " + juce::String (ambiencePrint->sampleRate / 1000.0, 1) + " kHz. Capture it again from this file.");
        return;
    }

    const auto& s = ambienceSettings;
    const auto toSettings = [] (const EffectValues& v) { return Ambience::Settings { (float) v.at ("level"), juce::roundToInt (v.at ("where")) == 0 }; };

    showEffectDialog ("Ambience Match",
                      "The captured room tone laid under the selection: new tone made from the print (its magnitudes with random phase, so it is "
                      "steady and never repeats), scaled by the level, added and eased in and out at the edges. Fill only the quiet adds tone only "
                      "where the selection is quieter than the print, by the shortfall, so a background is evened out rather than doubled.",
                      { { "level", "Level (dB against the print)", s.levelDb, 1, {}, -24, 12, 0.5 },
                        { "where", "Where", s.fillOnlyTheQuiet ? 0.0 : 1.0, 0, { "Fill only the quiet", "Add regardless" } } },
                      "spectrablade:ambience-match", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return Ambience::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          ambienceSettings = toSettings (v);
                          const auto r = selectionOrAll();

                          if (auto next = Ambience::apply (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), engine.document.getSampleRate(), *ambiencePrint, ambienceSettings))
                              applyEdit (std::move (next), "Ambience Match", waveform.getSelection());
                      },
                      [print = *ambiencePrint, captures = ambienceCaptures, toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "ambience " + juce::String (captures) + " " + juce::String (settings.levelDb, 3) + (settings.fillOnlyTheQuiet ? " quiet" : " all"),
                              [print, settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { Ambience::apply (*audio, start, end, sampleRate, print, settings), start, end, {} };
                              } });
                      },
                      "Lay Under");
}

void MainComponent::showRebalance()
{
    const auto& s = rebalanceSettings;
    const auto toSettings = [] (const EffectValues& v) { return Rebalance::Settings { v.at ("voice"), v.at ("bass"), v.at ("drums"), v.at ("rest"), v.at ("corner") }; };

    showEffectDialog ("Rebalance",
                      "The voice, the bass, the drums and the rest of a mix each turned up or down. Each bin of each frame is shared among the four "
                      "by soft masks that add up to one: drums are what is percussive (standing out across frequency more than across time); of the "
                      "rest, the bass is what lies below the corner, the voice is the mid's harmonics in a frame that reads as voiced, and the rest "
                      "is what is left, the sides included. A reading of the mix, not a separation: a steady chord in the middle reads as voice, a "
                      "kick's body as bass.",
                      { { "voice", "Voice (dB)", s.voiceDb, 1, {}, -40, 12, 0.5 }, { "bass", "Bass (dB)", s.bassDb, 1, {}, -40, 12, 0.5 },
                        { "drums", "Drums (dB)", s.drumsDb, 1, {}, -40, 12, 0.5 }, { "rest", "The rest (dB)", s.restDb, 1, {}, -40, 12, 0.5 },
                        { "corner", "Bass corner (Hz)", s.bassCornerHz, 0, {}, 60, 300, 1 } },
                      "spectrablade:rebalance", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return Rebalance::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          rebalanceSettings = toSettings (v);
                          rebalance (rebalanceSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "rebalance " + juce::String (settings.voiceDb, 3) + " " + juce::String (settings.bassDb, 3) + " "
                                                                              + juce::String (settings.drumsDb, 3) + " " + juce::String (settings.restDb, 3) + " " + juce::String (settings.bassCornerHz, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  return EffectPreview::Rendered { Rebalance::apply (*audio, start, end, sampleRate, settings, progress), start, end, {} };
                              } });
                      });
}

void MainComponent::rebalance (Rebalance::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Rebalancing...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = Rebalance::apply (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Rebalance", "The audio changed during processing, so the result was discarded.");
            else if (*result != nullptr)
                applyEdit (*result, "Rebalance", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showSpectralRecovery()
{
    const auto& s = recoverySettings;
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v)
    {
        return SpectralRecovery::Settings { juce::roundToInt (v.at ("auto")) == 1 ? 0.0 : v.at ("cutoff"), v.at ("level"), v.at ("highest") };
    };

    showEffectDialog ("Spectral Recovery",
                      "The highs missing from band-limited audio (a phone line, a low-rate MP3, an old transfer) rebuilt from the band that is there: "
                      "the octave below the cutoff copied up an octave (a partial's harmonics where they would lie), continuing the spectrum's slope "
                      "just below the cutoff, then again an octave higher, up to the top asked for. The cutoff is found from the audio or given. "
                      "Nothing below the cutoff changes. Rebuilt highs are a likeness, not the original.",
                      { { "auto", "Cutoff", juce::exactlyEqual (s.cutoffHz, 0.0) ? 1.0 : 0.0, 0, { "As given below", "Found from the audio" } },
                        { "cutoff", "Cutoff if given (Hz)", s.cutoffHz > 0.0 ? s.cutoffHz : 6000.0, 0, {}, 1000, 16000, 10 },
                        { "level", "Level (dB against the continued slope)", s.levelDb, 1, {}, -24, 6, 0.5 },
                        { "highest", "Rebuild up to (Hz)", s.highestHz, 0, {}, 2000, 20000, 100 } },
                      "spectrablade:spectral-recovery", PreviewOwner::none,
                      [toSettings, rate] (const EffectValues& v) { return SpectralRecovery::isValid (toSettings (v), rate); }, "A setting is out of range (the top must be under half the sample rate).", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          recoverySettings = toSettings (v);
                          recoverSpectrum (recoverySettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "recover " + juce::String (settings.cutoffHz, 1) + " " + juce::String (settings.levelDb, 3) + " " + juce::String (settings.highestHz, 1),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  auto result = SpectralRecovery::recover (*audio, start, end, sampleRate, settings, progress);
                                  return EffectPreview::Rendered { result, start, end, result == nullptr ? juce::String ("No cutoff found: the audio reaches the top.") : juce::String() };
                              } });
                      },
                      "Rebuild");
}

void MainComponent::recoverSpectrum (SpectralRecovery::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Rebuilding the highs...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = SpectralRecovery::recover (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Spectral Recovery", "The audio changed during processing, so the result was discarded.");
            else if (*result == nullptr)
                showError ("Spectral Recovery", "No cutoff was found: the audio reaches the top, or the selection is shorter than a window. Give a cutoff to rebuild above anyway.");
            else
                applyEdit (*result, "Spectral Recovery", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showWowFlutter()
{
    const auto& s = wowSettings;
    const auto toSettings = [] (const EffectValues& v) { return WowFlutter::Settings { v.at ("fastest"), v.at ("amount") }; };

    showEffectDialog ("Wow and Flutter",
                      "The pitch drift of a tape or a record found and taken out. The drift is read from the partials, each against its own "
                      "course: what every held note does together is the machine's speed, what one voice does on its own (vibrato, a slide) is "
                      "not, and where they don't agree, or only a voice is moving, nothing is read. The speed curve is low-passed to what moves "
                      "no faster than the fastest drift, and the audio is played back at its inverse, pitch and time together. The length is kept. "
                      "Works on music with sustained notes; little can be read from speech or drums alone.",
                      { { "fastest", "Fastest drift (Hz)", s.fastestHz, 1, {}, 0.5, 20, 0.1 }, { "amount", "Amount (%)", s.amountPercent, 0, {}, 0, 100, 1 } },
                      "spectrablade:wow-flutter", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return WowFlutter::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          wowSettings = toSettings (v);
                          correctWowFlutter (wowSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "wow " + juce::String (settings.fastestHz, 3) + " " + juce::String (settings.amountPercent, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  return EffectPreview::Rendered { WowFlutter::correct (*audio, start, end, sampleRate, settings, progress), start, end, {} };
                              } });
                      },
                      "Correct");
}

void MainComponent::correctWowFlutter (WowFlutter::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Correcting wow and flutter...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = WowFlutter::correct (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Wow and Flutter", "The audio changed during processing, so the result was discarded.");
            else if (*result == nullptr)
                showError ("Wow and Flutter", "The selection is under half a second, too short to read the drift from.");
            else
                applyEdit (*result, "Wow and Flutter", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showSpill()
{
    const auto& s = spillSettings;
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v)
    {
        return Spill::Settings { v.at ("wind"), v.at ("corner"), v.at ("rustle"), v.at ("bleed"), v.at ("bleedThreshold") };
    };

    showEffectDialog ("Wind, Rustle and Bleed",
                      "Three jobs on a speech recording. Wind: the lows below the corner lowered while they gust (over their typical level in a "
                      "block without voice). Rustle: the highs above 1 kHz lowered during a burst of them (20 to 400 ms, without voice and away "
                      "from it; a fricative sits next to a vowel, rustle does not). Bleed: everything lowered between the words, where the sound "
                      "lies more than the bleed threshold under the speech round it and away from voice. A reduction at 0 leaves that job out.",
                      { { "wind", "Wind reduction (dB)", s.windReductionDb, 1, {}, 0, 40, 0.5 }, { "corner", "Wind corner (Hz)", s.windCornerHz, 0, {}, 60, 400, 1 },
                        { "rustle", "Rustle reduction (dB)", s.rustleReductionDb, 1, {}, 0, 40, 0.5 },
                        { "bleed", "Bleed reduction (dB)", s.bleedReductionDb, 1, {}, 0, 40, 0.5 }, { "bleedThreshold", "Bleed threshold (dB under the speech)", s.bleedThresholdDb, 1, {}, 6, 40, 0.5 } },
                      "spectrablade:wind-rustle-bleed", PreviewOwner::none,
                      [toSettings, rate] (const EffectValues& v) { return Spill::isValid (toSettings (v), rate); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          spillSettings = toSettings (v);
                          reduceSpill (spillSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "spill " + juce::String (settings.windReductionDb, 3) + " " + juce::String (settings.windCornerHz, 3) + " "
                                                                              + juce::String (settings.rustleReductionDb, 3) + " " + juce::String (settings.bleedReductionDb, 3) + " "
                                                                              + juce::String (settings.bleedThresholdDb, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  return EffectPreview::Rendered { Spill::apply (*audio, start, end, sampleRate, settings, progress), start, end, {} };
                              } });
                      },
                      "Reduce");
}

void MainComponent::reduceSpill (Spill::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Reducing wind, rustle and bleed...",
        [result, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = Spill::apply (*snapshot, range.getStart(), range.getEnd(), rate, s, progress);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Wind, Rustle and Bleed", "The audio changed during processing, so the result was discarded.");
            else if (*result != nullptr)
                applyEdit (*result, "Wind, Rustle and Bleed", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showBreathControl()
{
    const auto& s = breathSettings;
    const auto toSettings = [] (const EffectValues& v) { return Breath::Settings { (float) v.at ("threshold"), (float) v.at ("reduction") }; };

    showEffectDialog ("Breath Control",
                      "The breaths between words found and lowered: a stretch of 80 to 800 ms that lies more than the threshold under the speech "
                      "round it and over the room noise, and sounds like air rather than voice (its zero-crossing rate), is lowered by the "
                      "reduction, eased in and out over 20 ms. Words and pauses that are only room noise are left alone.",
                      { { "threshold", "Threshold (dB under the speech)", s.thresholdDb, 1, {}, 6, 40, 0.5 }, { "reduction", "Reduction (dB)", s.reductionDb, 1, {}, 0, 40, 0.5 } },
                      "spectrablade:breath-control", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return Breath::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          breathSettings = toSettings (v);
                          const auto r = selectionOrAll();

                          if (Breath::find (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), engine.document.getSampleRate(), breathSettings).empty())
                          {
                              showError ("Breath Control", "No breaths were found in the selection. Lower the threshold to catch quieter ones.");
                              return;
                          }

                          runStep ({ "breath", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "breath", v, nullptr }); });
}

void MainComponent::showDePlosive()
{
    const auto& s = plosiveSettings;
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v) { return Plosive::Settings { v.at ("corner"), v.at ("threshold"), v.at ("reduction") }; };

    showEffectDialog ("De-plosive",
                      "The thumps a p or a b puts into a microphone (a burst of air, mostly below the corner, a few tens of milliseconds long) found "
                      "and lowered, the rest left alone. Where the lows stand over their own typical level by the threshold while the sound above "
                      "the corner does not rise with them, the lows are lowered by the reduction for as long as it lasts. A steady bass, however "
                      "loud, is left as it is.",
                      { { "corner", "Corner (Hz)", s.cornerHz, 0, {}, 60, 400, 1 }, { "threshold", "Threshold (dB over the typical lows)", s.thresholdDb, 1, {}, 3, 30, 0.5 },
                        { "reduction", "Reduction (dB)", s.reductionDb, 1, {}, 0, 40, 0.5 } },
                      "spectrablade:de-plosive", PreviewOwner::none,
                      [toSettings, rate] (const EffectValues& v) { return Plosive::isValid (toSettings (v), rate); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          plosiveSettings = toSettings (v);
                          runStep ({ "deplosive", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "deplosive", v, nullptr }); });
}

void MainComponent::showAdaptiveDenoise()
{
    const auto& s = adaptiveSettings;
    const auto toSettings = [] (const EffectValues& v) { return AdaptiveDenoise::Settings { (float) v.at ("reduction"), (float) v.at ("threshold"), (float) v.at ("tracking") }; };

    showEffectDialog ("Adaptive Noise Reduction",
                      "Noise reduced with no noise print: each frequency's noise is taken as the least it has been over the tracking span (what is "
                      "always there, even under speech), so the estimate follows the noise as it changes. The reduction is how far noise is lowered at "
                      "most; the threshold how far above the tracked noise a sound must be to count as signal. A longer span is steadier, a shorter one "
                      "follows faster. It needs moments when the noise is there alone: a sound that never pauses (a room full of voices, say) is "
                      "itself taken as the noise and lowered as a whole; when sound stands clear of the noise in under a tenth of the frames, the "
                      "preview and the result say so.",
                      { { "reduction", "Reduction (dB)", s.reductionDb, 1, {}, 0, 40, 0.5 }, { "threshold", "Threshold (dB)", s.thresholdDb, 1, {}, 0, 12, 0.5 },
                        { "tracking", "Tracking span (s)", s.trackingSeconds, 1, {}, 0.5, 5, 0.1 } },
                      "spectrablade:adaptive-noise-reduction", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return AdaptiveDenoise::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          adaptiveSettings = toSettings (v);
                          reduceNoiseAdaptively (adaptiveSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "adaptive " + juce::String (settings.reductionDb, 3) + " " + juce::String (settings.thresholdDb, 3) + " " + juce::String (settings.trackingSeconds, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  AdaptiveDenoise::Report report;
                                  const auto out = AdaptiveDenoise::reduce (*audio, start, end, sampleRate, settings, progress, &report);
                                  return EffectPreview::Rendered { out, start, end, {}, out != nullptr ? adaptiveDenoiseNote (report) : juce::String() };
                              } });
                      },
                      "Reduce");
}

void MainComponent::reduceNoiseAdaptively (AdaptiveDenoise::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    auto report = std::make_shared<AdaptiveDenoise::Report>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Reducing noise...",
        [result, report, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = AdaptiveDenoise::reduce (*snapshot, range.getStart(), range.getEnd(), rate, s, progress, report.get());
            return *result != nullptr;
        },
        [this, result, report, version, s]
        {
            if (engine.document.getVersion() != version)
                showError ("Adaptive Noise Reduction", "The audio changed during processing, so the result was discarded.");
            else if (*result != nullptr)
            {
                applyEdit (*result, "Adaptive Noise Reduction " + juce::String (s.reductionDb, 1) + " dB", waveform.getSelection());

                if (const auto note = adaptiveDenoiseNote (*report); note.isNotEmpty())
                    showNote ("Adaptive Noise Reduction", note);
            }
        });
    backgroundJob->launchThread();
}

void MainComponent::showDialogueIsolation()
{
    const auto& s = dialogueSettings;
    const auto toSettings = [] (const EffectValues& v) { return DialogueIsolation::Settings { (float) v.at ("reduction"), (float) v.at ("sensitivity") }; };

    showEffectDialog ("Dialogue Isolation",
                      "Speech kept and everything else lowered. Each frame is judged for speech by its pitch and how much of its power sits on that "
                      "pitch's harmonics (and fricatives by their highs standing over the background); what is kept is the harmonics and the highs "
                      "over the background, and all else comes down by the reduction. The background is what is always there, bin by bin, over the "
                      "last second and a half. A higher sensitivity counts frames as speech more readily (more of the voice kept, more of the rest with it). "
                      "Speech is found by its pitch, so a crowd of voices, with no one pitch to follow, is not kept; when speech is found in under a "
                      "tenth of the frames, the preview and the result say so.",
                      { { "reduction", "Reduction (dB)", s.reductionDb, 1, {}, 0, 40, 0.5 }, { "sensitivity", "Sensitivity (%)", s.sensitivity, 0, {}, 0, 100, 1 } },
                      "spectrablade:dialogue-isolation", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return DialogueIsolation::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          dialogueSettings = toSettings (v);
                          isolateDialogue (dialogueSettings);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "dialogue " + juce::String (settings.reductionDb, 3) + " " + juce::String (settings.sensitivity, 3),
                              [settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  DialogueIsolation::Report report;
                                  const auto out = DialogueIsolation::isolate (*audio, start, end, sampleRate, settings, progress, &report);
                                  return EffectPreview::Rendered { out, start, end, {}, out != nullptr ? dialogueIsolationNote (report) : juce::String() };
                              } });
                      },
                      "Isolate");
}

void MainComponent::isolateDialogue (DialogueIsolation::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr)
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    auto report = std::make_shared<DialogueIsolation::Report>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Isolating dialogue...",
        [result, report, snapshot, rate, range, s] (const std::function<bool (float)>& progress)
        {
            *result = DialogueIsolation::isolate (*snapshot, range.getStart(), range.getEnd(), rate, s, progress, report.get());
            return *result != nullptr;
        },
        [this, result, report, version, s]
        {
            if (engine.document.getVersion() != version)
                showError ("Dialogue Isolation", "The audio changed during processing, so the result was discarded.");
            else if (*result != nullptr)
            {
                applyEdit (*result, "Dialogue Isolation " + juce::String (s.reductionDb, 1) + " dB", waveform.getSelection());

                if (const auto note = dialogueIsolationNote (*report); note.isNotEmpty())
                    showNote ("Dialogue Isolation", note);
            }
        });
    backgroundJob->launchThread();
}

void MainComponent::showDither()
{
    const auto& s = ditherSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Dither::Settings { juce::roundToInt (v.at ("bits")) == 1 ? 24 : 16, (Dither::Shaping) juce::jlimit (0, 2, juce::roundToInt (v.at ("shaping"))) };
    };

    showEffectDialog ("Dither",
                      "The audio rounded to the depth with triangular dither, so the rounding's error is noise rather than distortion that follows "
                      "the sound: the last step before saving at that depth, after every other change. Shaping moves the noise up where hearing "
                      "is least sensitive: Light 12 dB an octave, Strong 18; shaped noise is louder in all but quieter below a few kilohertz.",
                      { { "bits", "Depth", s.bits == 24 ? 1.0 : 0.0, 0, { "16 bits", "24 bits" } },
                        { "shaping", "Noise shaping", (double) (int) s.shaping, 0, Dither::shapingNames() } },
                      "spectrablade:dither", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          ditherSettings = toSettings (v);
                          runStep ({ "dither", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "dither", v, nullptr }); });
}

void MainComponent::captureMatchReference()
{
    const auto sel = selectionOrAll();
    auto& doc = engine.document;
    const auto profile = MatchEq::measure (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), doc.getSampleRate());

    if (! profile.has_value())
    {
        showError ("Capture Match Reference", "Select at least " + formatTime (Analysis::fftSize, doc.getSampleRate()) + " of sound (not silence), then capture it.");
        return;
    }

    matchReference = profile;
    ++matchReferenceCaptures;
    commands.commandStatusChanged();
}

void MainComponent::captureMatchReferenceFromFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a reference", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              std::unique_ptr<juce::AudioFormatReader> reader (engine.formatManager.createReaderFor (file));

                              if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples < 1)
                              {
                                  showError ("Capture Match Reference", file.getFileName() + " isn't an audio file SpectraBlade can read.");
                                  return;
                              }

                              // At most ten minutes of it, at this file's rate.
                              const auto rate = engine.document.hasAudio() ? engine.document.getSampleRate() : reader->sampleRate;
                              const auto frames = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (600.0 * reader->sampleRate));
                              juce::AudioBuffer<float> read ((int) reader->numChannels, frames);
                              reader->read (&read, 0, frames, 0, true, true);
                              juce::AudioBuffer<float> atRate;

                              if (! juce::exactlyEqual (reader->sampleRate, rate))
                                  SampleRateConverter::convert (read, reader->sampleRate, rate, atRate);
                              else
                                  atRate = std::move (read);

                              const auto snapshot = AudioEdits::fromBuffer (std::move (atRate));
                              const auto profile = MatchEq::measure (*snapshot, 0, snapshot->getNumFrames(), rate);

                              if (! profile.has_value())
                              {
                                  showError ("Capture Match Reference", file.getFileName() + " is too short (under " + formatTime (Analysis::fftSize, rate) + ") or silent.");
                                  return;
                              }

                              matchReference = profile;
                              ++matchReferenceCaptures;
                              commands.commandStatusChanged();
                          });
}

void MainComponent::showMatchEq()
{
    if (! matchReference.has_value())
        return;

    if (! juce::exactlyEqual (matchReference->sampleRate, engine.document.getSampleRate()))
    {
        showError ("Match EQ", "The reference was captured at " + juce::String (matchReference->sampleRate / 1000.0, 1) + " kHz. Capture it again for this file.");
        return;
    }

    const auto& s = matchSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return MatchEq::Settings { v.at ("amount"), v.at ("smoothing") / 12.0, v.at ("most") };
    };

    showEffectDialog ("Match EQ",
                      "The tone of the captured reference taken onto the sound: the reference's long-term spectrum less the sound's, smoothed over "
                      "the width, with the overall level left as it is and no frequency moved by more than the most, scaled by the amount, and "
                      "applied in linear phase. Capture a reference from a selection or a file first.",
                      { { "amount", "Amount (%)", s.amountPercent, 0, {}, 0, 100, 1 }, { "smoothing", "Smoothing (twelfths of an octave)", s.smoothingOctaves * 12.0, 0, {}, 1, 24, 1 },
                        { "most", "Most (dB)", s.mostDb, 1, {}, 0, 24, 0.5 } },
                      "spectrablade:match-eq", PreviewOwner::none,
                      [toSettings] (const EffectValues& v) { return MatchEq::isValid (toSettings (v)); }, "A setting is out of range.", nullptr, nullptr,
                      [this, toSettings] (const EffectValues& v)
                      {
                          matchSettings = toSettings (v);
                          applyMatchEq (matchSettings);
                      },
                      [reference = *matchReference, captures = matchReferenceCaptures, toSettings] (const EffectValues& v)
                      {
                          const auto settings = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "match " + juce::String (captures) + " " + juce::String (settings.amountPercent, 3) + " "
                                                                              + juce::String (settings.smoothingOctaves, 4) + " " + juce::String (settings.mostDb, 3),
                              [reference, settings] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { MatchEq::apply (*audio, start, end, sampleRate, reference, settings), start, end, {} };
                              } });
                      },
                      "Match");
}

void MainComponent::applyMatchEq (MatchEq::Settings s)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr || ! matchReference.has_value())
        return;

    auto result = std::make_shared<AudioDocument::Snapshot>();
    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Matching...",
        [result, snapshot, rate, range, reference = *matchReference, s] (const std::function<bool (float)>&)
        {
            *result = MatchEq::apply (*snapshot, range.getStart(), range.getEnd(), rate, reference, s);
            return *result != nullptr;
        },
        [this, result, version]
        {
            if (engine.document.getVersion() != version)
                showError ("Match EQ", "The audio changed during processing, so the result was discarded.");
            else if (*result == nullptr)
                showError ("Match EQ", "The selection is too short to measure (under " + formatTime (Analysis::fftSize, engine.document.getSampleRate()) + ") or silent.");
            else
                applyEdit (*result, "Match EQ", waveform.getSelection());
        });
    backgroundJob->launchThread();
}

void MainComponent::showTransientShaper()
{
    const auto& s = transientShaperSettings;
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v)
    {
        TransientShaper::Settings t;
        t.lowCrossover = v.at ("lowCrossover");
        t.highCrossover = v.at ("highCrossover");
        t.outputDb = v.at ("output");
        t.linearPhase = juce::roundToInt (v.at ("linearPhase")) == 1;
        const std::array<const char*, 3> ids { "low", "mid", "high" };

        for (size_t b = 0; b < 3; ++b)
            t.bands[b] = { v.at (ids[b] + juce::String ("Attack")), v.at (ids[b] + juce::String ("Sustain")) };

        return t;
    };
    std::vector<EffectField> fields { { "lowCrossover", "Low crossover (Hz)", s.lowCrossover, 0, {}, 20, 10000, 1 },
                                      { "highCrossover", "High crossover (Hz)", s.highCrossover, 0, {}, 40, 20000, 1 },
                                      { "linearPhase", "Band split", s.linearPhase ? 1.0 : 0.0, 0, { "Minimum phase (no delay)", "Linear phase (peaks held)" } } };
    const std::array<const char*, 3> ids { "low", "mid", "high" }, names { "Lows", "Middle", "Highs" };

    for (size_t b = 0; b < 3; ++b)
    {
        const juce::String id (ids[b]), name (names[b]);
        fields.push_back ({ id + "Attack", name + ": attack (dB)", s.bands[b].attackDb, 1, {}, -12, 12, 0.5 });
        fields.push_back ({ id + "Sustain", name + ": sustain (dB)", s.bands[b].sustainDb, 1, {}, -12, 12, 0.5 });
    }

    fields.push_back ({ "output", "Output (dB)", s.outputDb, 1, {}, -24, 24, 0.5 });

    showEffectDialog ("Transient Shaper",
                      "The sound is split into lows, middle and highs at the two crossovers (24 dB an octave), and in each band the attacks (the "
                      "first moments of a hit, where a fast envelope stands over a slow one) and the sustain (what rings on after, where the slow "
                      "stands over the fast) are turned up or down on their own, by up to 12 dB. A steady sound is left as it is. The high "
                      "crossover stays at least an octave above the low one. The band split is minimum phase to start: no delay, but it turns the phase, which can lift the peaks of a sound already at its ceiling (6 dB on a limited master). Linear phase splits with symmetric impulses instead, so the bands add back to the sound exactly as it was and its peaks hold, at the cost of 8,703 frames of delay when played live (181 ms at 48 kHz), as the linear-phase EQ.",
                      fields, BuiltInEffects::transientShaperId, PreviewOwner::transientShaper,
                      [toSettings, rate] (const EffectValues& v) { return TransientShaper::isValid (toSettings (v), rate); },
                      "A setting is out of range: crossovers from 20 Hz to 20 kHz (below half the rate), the high one at least an octave above the low; attack and sustain within 12 dB, output within 24 dB.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::TransientShaperProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::TransientShaperProcessor*> (&p);

                          if (live != nullptr)
                              live->setSettings (toSettings (v));

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          transientShaperSettings = toSettings (v);
                          runStep ({ "transientShaper", v, nullptr });
                      });
}

void MainComponent::showMaximizer()
{
    const auto& s = maximizerSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Maximizer::Settings { v.at ("gain"), v.at ("ceiling"), (Maximizer::Character) juce::jlimit (0, 2, juce::roundToInt (v.at ("character"))), v.at ("emphasis") };
    };

    showEffectDialog ("Maximizer",
                      "The gain drives the sound into the ceiling, and nothing comes out above it, the peaks between samples held under it too. "
                      "Character sets how the limiter moves: Transparent looks 10 ms ahead and recovers slowly, so little pumps or distorts; Loud "
                      "looks 2 ms ahead and recovers fast, louder and rougher; Balanced sits between. Transient Emphasis lifts the attacks before "
                      "the limiter (by up to 6 dB), so hits keep their edge through it.",
                      { { "gain", "Gain (dB)", s.gainDb, 1, {}, 0, 24, 0.1 }, { "ceiling", "Ceiling (dBTP)", s.ceilingDb, 1, {}, -24, 0, 0.1 },
                        { "character", "Character", (double) (int) s.character, 0, Maximizer::characterNames() },
                        { "emphasis", "Transient emphasis (%)", s.emphasisPercent, 0, {}, 0, 100, 1 } },
                      BuiltInEffects::maximizerId, PreviewOwner::maximizer,
                      [toSettings] (const EffectValues& v) { return Maximizer::isValid (toSettings (v)); },
                      "A setting is out of range: gain 0 to 24 dB, ceiling -24 to 0 dBTP, emphasis 0 to 100 %.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::MaximizerProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::MaximizerProcessor*> (&p);

                          if (live != nullptr)
                              live->setSettings (toSettings (v));

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          maximizerSettings = toSettings (v);
                          runStep ({ "maximizer", v, nullptr });
                      });
}

void MainComponent::showExciter()
{
    const auto& s = exciterSettings;
    const auto rate = engine.document.getSampleRate();
    const auto toSettings = [] (const EffectValues& v)
    {
        Exciter::Settings e;
        e.lowCrossover = v.at ("lowCrossover");
        e.highCrossover = v.at ("highCrossover");
        e.outputDb = v.at ("output");
        e.linearPhase = juce::roundToInt (v.at ("linearPhase")) == 1;
        const std::array<const char*, 3> ids { "low", "mid", "high" };

        for (size_t b = 0; b < 3; ++b)
            e.bands[b] = { v.at (ids[b] + juce::String ("Drive")), (Exciter::Curve) juce::jlimit (0, 2, juce::roundToInt (v.at (ids[b] + juce::String ("Curve")))),
                           v.at (ids[b] + juce::String ("Mix")) };

        return e;
    };
    std::vector<EffectField> fields { { "lowCrossover", "Low crossover (Hz)", s.lowCrossover, 0, {}, 20, 10000, 1 },
                                      { "highCrossover", "High crossover (Hz)", s.highCrossover, 0, {}, 40, 20000, 1 },
                                      { "linearPhase", "Band split", s.linearPhase ? 1.0 : 0.0, 0, { "Minimum phase (no delay)", "Linear phase (peaks held)" } } };
    const std::array<const char*, 3> ids { "low", "mid", "high" }, names { "Lows", "Middle", "Highs" };

    for (size_t b = 0; b < 3; ++b)
    {
        const juce::String id (ids[b]), name (names[b]);
        fields.push_back ({ id + "Drive", name + ": drive (dB)", s.bands[b].driveDb, 1, {}, 0, 36, 0.5 });
        fields.push_back ({ id + "Curve", name + ": curve", (double) (int) s.bands[b].curve, 0, Exciter::curveNames() });
        fields.push_back ({ id + "Mix", name + ": mix (%)", s.bands[b].mixPercent, 0, {}, 0, 100, 1 });
    }

    fields.push_back ({ "output", "Output (dB)", s.outputDb, 1, {}, -24, 24, 0.5 });

    showEffectDialog ("Exciter",
                      "The sound is split into lows, middle and highs at the two crossovers (24 dB an octave), each band driven into a saturation "
                      "curve of its own and mixed back with the band as it was. Tube rounds the halves unevenly (even and odd harmonics), Tape rounds "
                      "them alike (odd harmonics), Transistor clips harder. The drive sets how far the band goes into the curve; the level stays "
                      "about where it was. The shaping runs at four times the rate, so what it makes above the audible band is filtered off rather "
                      "than folded back. The high crossover stays at least an octave above the low one. The band split is minimum phase to start: no delay, but it turns the phase, which can lift the peaks of a sound already at its ceiling (6 dB on a limited master). Linear phase splits with symmetric impulses instead, so the bands add back to the sound exactly as it was and its peaks hold, at the cost of 8,703 frames of delay when played live (181 ms at 48 kHz), as the linear-phase EQ.",
                      fields, BuiltInEffects::exciterId, PreviewOwner::exciter,
                      [toSettings, rate] (const EffectValues& v) { return Exciter::isValid (toSettings (v), rate); },
                      "A setting is out of range: crossovers from 20 Hz to 20 kHz (below half the rate), the high one at least an octave above the low; drive 0 to 36 dB, mix 0 to 100 %, output within 24 dB.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::ExciterProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::ExciterProcessor*> (&p);

                          if (live != nullptr)
                              live->setSettings (toSettings (v));

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          exciterSettings = toSettings (v);
                          runStep ({ "exciter", v, nullptr });
                      });
}

void MainComponent::showMultibandWidth()
{
    const auto& s = multibandWidthSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Multiband::Width { v.at ("lowCrossover"), v.at ("highCrossover"), { v.at ("low"), v.at ("mid"), v.at ("high") }, juce::roundToInt (v.at ("linearPhase")) == 1 };
    };
    const auto rate = engine.document.getSampleRate();

    showEffectDialog ("Multiband Stereo Width",
                      "The sound is split into lows, middle and highs at the two crossovers (24 dB an octave), and each band's width set on its "
                      "own: 0 % makes the band mono (its side taken out: the low band at 0 % is a mono low end), 100 % leaves it, and up to 200 % "
                      "doubles its side. What the channels share is never touched. The high crossover stays at least an octave above the low one. The band split is minimum phase to start: no delay, but it turns the phase, which can lift the peaks of a sound already at its ceiling (6 dB on a limited master). Linear phase splits with symmetric impulses instead, so the bands add back to the sound exactly as it was and its peaks hold, at the cost of 8,703 frames of delay when played live (181 ms at 48 kHz), as the linear-phase EQ.",
                      { { "lowCrossover", "Low crossover (Hz)", s.lowCrossover, 0, {}, 20, 10000, 1 }, { "highCrossover", "High crossover (Hz)", s.highCrossover, 0, {}, 40, 20000, 1 },
                        { "linearPhase", "Band split", s.linearPhase ? 1.0 : 0.0, 0, { "Minimum phase (no delay)", "Linear phase (peaks held)" } },
                        { "low", "Lows (%)", s.percent[0], 0, {}, 0, 200, 1 }, { "mid", "Middle (%)", s.percent[1], 0, {}, 0, 200, 1 },
                        { "high", "Highs (%)", s.percent[2], 0, {}, 0, 200, 1 } },
                      BuiltInEffects::multibandWidthId, PreviewOwner::multibandWidth,
                      [toSettings, rate] (const EffectValues& v) { return Multiband::isValid (toSettings (v), rate); },
                      "A setting is out of range: crossovers from 20 Hz to 20 kHz (below half the rate), the high one at least an octave above the low; widths 0 to 200 %.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::MultibandWidthProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::MultibandWidthProcessor*> (&p);

                          if (live != nullptr)
                              live->setSettings (toSettings (v));

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          multibandWidthSettings = toSettings (v);
                          runStep ({ "multibandWidth", v, nullptr });
                      });
}

void MainComponent::showDeEsser()
{
    const auto& s = deEsserSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Multiband::DeEsser { v.at ("frequency"), v.at ("threshold"), v.at ("reduction"), juce::roundToInt (v.at ("mode")) == 1, juce::roundToInt (v.at ("linearPhase")) == 1 };
    };
    const auto rate = engine.document.getSampleRate();

    showEffectDialog ("De-esser",
                      "Whenever the highs above the frequency are louder than the threshold, they are turned down by however far they are over, "
                      "up to the reduction, within a millisecond, coming back over 60 ms. Turn down the high band alone (the rest of the voice "
                      "untouched) or the whole sound. Frequency 1 to 16 kHz, threshold -60 to 0 dB, reduction 0 to 24 dB. "
                      "Listen plays only what it takes out, to set the frequency and threshold by ear; OK always applies the result. The band split is minimum phase to start: no delay, but it turns the phase, which can lift the peaks of a sound already at its ceiling (6 dB on a limited master). Linear phase splits with symmetric impulses instead, so the bands add back to the sound exactly as it was and its peaks hold, at the cost of 8,703 frames of delay when played live (181 ms at 48 kHz), as the linear-phase EQ.",
                      { { "frequency", "Frequency (Hz)", s.frequency, 0, {}, 1000, 16000, 50 }, { "threshold", "Threshold (dB)", s.thresholdDb, 1, {}, -60, 0, 0.5 },
                        { "reduction", "Reduction (dB)", s.maxReductionDb, 1, {}, 0, 24, 0.5 },
                        { "mode", "Turn down", s.wholeSound ? 1.0 : 0.0, 0, { "The high band", "The whole sound" } },
                        { "linearPhase", "Band split", s.linearPhase ? 1.0 : 0.0, 0, { "Minimum phase (no delay)", "Linear phase (peaks held)" } },
                        { "listen", "Listen", 0.0, 0, { "To the result", "Only to what it takes out" } } },
                      BuiltInEffects::deEsserId, PreviewOwner::deEsser,
                      [toSettings, rate] (const EffectValues& v) { return Multiband::isValid (toSettings (v), rate); },
                      "A setting is out of range: frequency 1 to 16 kHz (below half the rate), threshold -60 to 0 dB, reduction 0 to 24 dB.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::DeEsserProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* live = dynamic_cast<BuiltInEffects::DeEsserProcessor*> (&p);

                          if (live != nullptr)
                          {
                              live->setSettings (toSettings (v));
                              *live->listen = juce::roundToInt (v.at ("listen")) == 1;
                          }

                          return live != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          deEsserSettings = toSettings (v);
                          runStep ({ "deEsser", v, nullptr });
                      });
}

void MainComponent::showLimiter()
{
    const auto& s = limiterSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return Dynamics::Limiter { v.at ("gain"), v.at ("ceiling"), v.at ("release"), v.at ("lookahead"), v.at ("peaks") > 0.5 };
    };

    showEffectDialog ("Limiter", "No sample comes out above the ceiling, or with true peaks, nothing between samples either. "
                                 "Gain before the limiter makes the audio louder into it. "
                                 "The gain starts falling the look-ahead time before each peak and recovers with the release time. "
                                 "Gain within 48 dB, ceiling -48 to 0 dBFS, release 1 to 5,000 ms, look-ahead 0.1 to 50 ms. "
                                 "The preview holds sample peaks.",
                      { { "gain", "Gain before (dB)", s.inputGainDb, 1, {}, -48, 48, 0.5 }, { "ceiling", "Ceiling (dBFS)", s.ceilingDb, 1, {}, -48, 0, 0.1 },
                        { "release", "Release (ms)", s.releaseMs, 0, {}, 1, 5000, 5 }, { "lookahead", "Look-ahead (ms)", s.lookaheadMs, 1, {}, 0.1, 50, 0.1 },
                        { "peaks", "Hold under the ceiling", s.truePeak ? 1.0 : 0.0, 0, { "Sample peaks", "True peaks (between samples)" } } },
                      BuiltInEffects::limiterId, PreviewOwner::limiter,
                      [toSettings] (const EffectValues& v) { return Dynamics::isValid (toSettings (v)); },
                      "A setting is out of range: gain within 48 dB, ceiling -48 to 0 dBFS, release 1 to 5,000 ms, look-ahead 0.1 to 50 ms.",
                      [toSettings] (const EffectValues& v) { return std::make_unique<BuiltInEffects::LimiterProcessor> (toSettings (v).lookaheadMs); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          // A limiter is made with its look-ahead; another look-ahead needs a new one.
                          const auto settings = toSettings (v);
                          auto* live = dynamic_cast<BuiltInEffects::LimiterProcessor*> (&p);

                          if (live == nullptr || ! juce::exactlyEqual (live->getSettings().lookaheadMs, settings.lookaheadMs))
                              return false;

                          live->setSettings (settings);
                          return true;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          limiterSettings = toSettings (v);
                          runStep ({ "limiter", v, nullptr });
                      });
}

bool MainComponent::previewsFromStart()
{
    return ! multitrackMode && ! isRecording() && previewOwner == PreviewOwner::none && engine.getPlayer().isPlaying();
}

void MainComponent::showEffectDialog (const juce::String& title, const juce::String& message, const std::vector<EffectField>& fields,
                                      const juce::String& presetKey, PreviewOwner owner,
                                      std::function<bool (const EffectValues&)> valid, const juce::String& invalidText,
                                      std::function<std::unique_ptr<juce::AudioPluginInstance> (const EffectValues&)> make,
                                      std::function<bool (juce::AudioPluginInstance&, const EffectValues&)> setLive,
                                      std::function<void (const EffectValues&)> apply,
                                      std::function<std::optional<EffectPreview::Request> (const EffectValues&)> rendered,
                                      const juce::String& applyText)
{
    const auto live = make != nullptr && owner != PreviewOwner::none;
    auto dialog = std::make_unique<EffectDialog> (EffectDialog::Options { message, fields, presetKey, live || rendered != nullptr, applyText });
    juce::Component::SafePointer<EffectDialog> safe (dialog.get());

    const auto previewEndedHere = [safe]
    {
        if (safe != nullptr)
            safe->setPreviewOn (false, juce::dontSendNotification);
    };

    // The live effect with the dialog's settings (the last valid ones while they are out of range);
    // a new one when setLive can't take the settings (the limiter's look-ahead).
    const auto updateLive = [this, owner, valid, make, setLive, safe, previewEndedHere] (const EffectValues& values)
    {
        if (safe == nullptr || ! safe->isPreviewOn())
        {
            if (previewOwner == owner)
                stopPreview();

            return;
        }

        if (! valid (values))
            return;

        if (auto* effect = previewEffectOf (owner); effect != nullptr && setLive (*effect, values))
            return;

        auto made = make (values);
        setLive (*made, values);
        startPreview (owner, std::move (made), previewEndedHere);

        if (previewOwner != owner)
            safe->setPreviewOn (false, juce::dontSendNotification);
    };

    // The effect processed with the settings once they settle; the last ones play meanwhile.
    const auto updateRendered = [this, valid, invalidText, rendered, safe, previewEndedHere] (const EffectValues& values)
    {
        if (safe == nullptr || ! safe->isPreviewOn())
        {
            if (previewOwner == PreviewOwner::rendered)
                stopPreview();

            if (safe != nullptr)
                safe->setStatus ({});

            return;
        }

        effectTexts = safe->getTexts();   // a generator's typed digits, as applying sees them
        const auto request = valid (values) ? rendered (values) : std::nullopt;
        effectTexts.clear();

        if (! request.has_value())
        {
            safe->setStatus (invalidText.isNotEmpty() ? invalidText : juce::String ("These settings can't be previewed."));
            return;
        }

        if (previewOwner == PreviewOwner::rendered && request->key == renderedKey)
            return;

        startRenderedPreview (*request, previewEndedHere, [safe] (const juce::String& text)
        {
            if (safe != nullptr)
                safe->setStatus (text);
        });
    };

    if (live)
    {
        dialog->onPreview = [updateLive, safe] (bool) { if (safe != nullptr) updateLive (safe->getValues()); };
        dialog->onChange = updateLive;
    }
    else if (rendered != nullptr)
    {
        dialog->onPreview = [updateRendered, safe] (bool) { if (safe != nullptr) updateRendered (safe->getValues()); };
        dialog->onSettled = updateRendered;
    }

    dialog->onBypass = [this] (bool bypassed) { setPreviewBypassed (bypassed); };
    dialog->onApply = [this, valid, invalidText, apply, title, safe] (const EffectValues& values)
    {
        previewEnded = {};
        stopPreview();
        effectTexts = safe != nullptr ? safe->getTexts() : std::map<juce::String, juce::String>();

        if (! valid (values))
        {
            juce::MessageManager::callAsync ([title, invalidText] { showError (title, invalidText); });
            return;
        }

        if (apply != nullptr)
            apply (values);

        effectTexts.clear();
    };
    dialog->onClose = [this]
    {
        previewEnded = {};
        stopPreview();
    };

    // Opened while the file plays, the dialog previews from the start, so its settings are heard
    // as they move (Preview off, the play underneath would go on unchanged).
    if (previewsFromStart())
        dialog->setPreviewOn (true, juce::sendNotification);

    EffectDialog::show (std::move (dialog), title, this);
}

void MainComponent::showSweptDelay (bool flanger)
{
    const auto& s = flanger ? flangerSettings : chorusSettings;
    const auto title = flanger ? juce::String ("Flanger") : juce::String ("Chorus");
    const auto toDelay = [] (const EffectValues& v)
    {
        return Modulation::Delay { v.at ("rate"), v.at ("delay"), v.at ("depth"), v.at ("feedback"), v.at ("mix"), v.at ("spread") };
    };
    const auto kind = flanger ? BuiltInEffects::SweptDelayProcessor::Kind::flanger : BuiltInEffects::SweptDelayProcessor::Kind::chorus;

    showEffectDialog (title,
                      juce::String (flanger ? "A very short delay, fed back on itself, sweeps a comb of notches through the sound. "
                                            : "A copy of the sound a few milliseconds late, its delay slowly swept, thickens it. ")
                          + "The delay moves between its shortest and that plus the depth at the rate; the second channel's sweep runs the spread behind. "
                            "Rate 0.01 to 10 Hz, delay and depth 0 to 50 ms, feedback -0.95 to 0.95, mix 0 to 100 %, spread 0 to 180 degrees.",
                      { { "rate", "Rate (Hz)", s.rateHz, 2, {}, 0.01, 10, 0.01 }, { "delay", "Delay (ms)", s.delayMs, 1, {}, 0, 50, 0.1 }, { "depth", "Depth (ms)", s.depthMs, 1, {}, 0, 50, 0.1 },
                        { "feedback", "Feedback (-0.95 to 0.95)", s.feedback, 2, {}, -0.95, 0.95, 0.01 }, { "mix", "Mix (%)", s.mixPercent, 0, {}, 0, 100, 1 },
                        { "spread", "Stereo spread (degrees)", s.spreadDegrees, 0, {}, 0, 180, 1 } },
                      flanger ? BuiltInEffects::flangerId : BuiltInEffects::chorusId, flanger ? PreviewOwner::flanger : PreviewOwner::chorus,
                      [toDelay] (const EffectValues& v) { return Modulation::isValid (toDelay (v)); },
                      "A setting is out of range: rate 0.01 to 10 Hz, delay and depth 0 to 50 ms, feedback -0.95 to 0.95, mix 0 to 100 %, spread 0 to 180 degrees.",
                      [kind] (const EffectValues&) { return std::make_unique<BuiltInEffects::SweptDelayProcessor> (kind); },
                      [toDelay] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* d = dynamic_cast<BuiltInEffects::SweptDelayProcessor*> (&p);

                          if (d != nullptr)
                              d->setSettings (toDelay (v));

                          return d != nullptr;
                      },
                      [this, flanger, toDelay, title] (const EffectValues& v)
                      {
                          const auto settings = toDelay (v);
                          (flanger ? flangerSettings : chorusSettings) = settings;
                          runStep ({ flanger ? "flanger" : "chorus", v, nullptr });
                      });
}

void MainComponent::showPhaser()
{
    const auto& s = phaserSettings;
    const auto toPhaser = [] (const EffectValues& v)
    {
        return Modulation::Phaser { v.at ("rate"), v.at ("low"), v.at ("high"), 2 * ((int) std::lround (v.at ("stages")) + 1),
                                    v.at ("feedback"), v.at ("mix"), v.at ("spread") };
    };

    showEffectDialog ("Phaser",
                      "A chain of all-pass filters, swept between the low and high frequencies at the rate and mixed with the sound, "
                      "sweeps notches through it (one for every two stages; deepest at a 50 % mix). The second channel's sweep runs the spread behind. "
                      "Rate 0.01 to 10 Hz, frequencies 20 Hz to 20 kHz (low up to high), 2 to 12 stages, feedback -0.95 to 0.95, mix 0 to 100 %, spread 0 to 180 degrees.",
                      { { "rate", "Rate (Hz)", s.rateHz, 2, {}, 0.01, 10, 0.01 }, { "low", "Low frequency (Hz)", s.lowHz, 0, {}, 20, 20000, 10 }, { "high", "High frequency (Hz)", s.highHz, 0, {}, 20, 20000, 10 },
                        { "stages", "Stages", (double) (s.stages / 2 - 1), 0, { "2", "4", "6", "8", "10", "12" } }, { "feedback", "Feedback (-0.95 to 0.95)", s.feedback, 2, {}, -0.95, 0.95, 0.01 },
                        { "mix", "Mix (%)", s.mixPercent, 0, {}, 0, 100, 1 }, { "spread", "Stereo spread (degrees)", s.spreadDegrees, 0, {}, 0, 180, 1 } },
                      BuiltInEffects::phaserId, PreviewOwner::phaser,
                      [toPhaser] (const EffectValues& v) { return Modulation::isValid (toPhaser (v)); },
                      "A setting is out of range: rate 0.01 to 10 Hz, frequencies 20 Hz to 20 kHz with the low one no higher than the high, "
                      "feedback -0.95 to 0.95, mix 0 to 100 %, spread 0 to 180 degrees.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::PhaserProcessor>(); },
                      [toPhaser] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* ph = dynamic_cast<BuiltInEffects::PhaserProcessor*> (&p);

                          if (ph != nullptr)
                              ph->setSettings (toPhaser (v));

                          return ph != nullptr;
                      },
                      [this, toPhaser] (const EffectValues& v)
                      {
                          phaserSettings = toPhaser (v);
                          runStep ({ "phaser", v, nullptr });
                      });
}

void MainComponent::showReverb()
{
    const auto& s = reverbSettings;
    const auto toSettings = [] (const EffectValues& v)
    {
        return RoomReverb::Settings { v.at ("preDelay"), v.at ("decay"), v.at ("size"), v.at ("damping"), v.at ("width"), v.at ("mix") };
    };

    showEffectDialog ("Reverb",
                      "The sound rings on as in a room, falling 60 dB over the decay time after the pre-delay, and carries on past the "
                      "selection (added to what follows) for the pre-delay plus the decay. Size scales the room; damping takes the highs away "
                      "faster; width spreads the reverb across the sides. Pre-delay 0 to 200 ms, decay 0.1 to 20 s, size 50 to 150 %, "
                      "damping, width and mix 0 to 100 %.",
                      { { "preDelay", "Pre-delay (ms)", s.preDelayMs, 0, {}, 0, 200, 1 }, { "decay", "Decay (s)", s.decaySeconds, 2, {}, 0.1, 20, 0.05 }, { "size", "Size (%)", s.sizePercent, 0, {}, 50, 150, 1 },
                        { "damping", "Damping (%)", s.dampingPercent, 0, {}, 0, 100, 1 }, { "width", "Width (%)", s.widthPercent, 0, {}, 0, 100, 1 }, { "mix", "Mix (%)", s.mixPercent, 0, {}, 0, 100, 1 } },
                      BuiltInEffects::reverbId, PreviewOwner::reverb,
                      [toSettings] (const EffectValues& v) { return RoomReverb::isValid (toSettings (v)); },
                      "A setting is out of range: pre-delay 0 to 200 ms, decay 0.1 to 20 s, size 50 to 150 %, damping, width and mix 0 to 100 %.",
                      [] (const EffectValues&) { return std::make_unique<BuiltInEffects::ReverbProcessor>(); },
                      [toSettings] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* r = dynamic_cast<BuiltInEffects::ReverbProcessor*> (&p);

                          if (r != nullptr)
                              r->setSettings (toSettings (v));

                          return r != nullptr;
                      },
                      [this, toSettings] (const EffectValues& v)
                      {
                          reverbSettings = toSettings (v);
                          runStep ({ "reverb", v, nullptr });
                      });
}

void MainComponent::showConvolutionReverb()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose an impulse response", lastImpulseFile, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (! file.existsAsFile())
                                  return;

                              juce::String error;
                              auto impulse = Convolver::read (file, engine.formatManager, error);

                              if (error.isNotEmpty())
                              {
                                  showError ("Convolution Reverb", error);
                                  return;
                              }

                              lastImpulseFile = file;
                              showConvolutionDialog (std::make_shared<const Convolver::Impulse> (std::move (impulse)));
                          });
}

void MainComponent::showConvolutionDialog (std::shared_ptr<const Convolver::Impulse> impulse)
{
    const auto seconds = impulse->samples.getNumSamples() / impulse->sampleRate;

    showEffectDialog ("Convolution Reverb",
                      "The sound convolved with " + impulse->name.quoted() + " (" + juce::String (seconds, 2) + " s, "
                          + (impulse->samples.getNumChannels() == 1 ? "mono" : "stereo") + "), scaled so noise comes out about as loud, "
                          "ringing on past the selection (added to what follows) for the impulse's length. Mix 0 to 100 %.",
                      { { "mix", "Mix (%)", convolutionMix, 0, {}, 0, 100, 1 } },
                      BuiltInEffects::convolutionId, PreviewOwner::convolution,
                      [] (const EffectValues& v) { return v.at ("mix") >= 0.0 && v.at ("mix") <= 100.0; },
                      "The mix must be from 0 to 100 %.",
                      [impulse] (const EffectValues&)
                      {
                          auto c = std::make_unique<BuiltInEffects::ConvolutionProcessor>();
                          c->setImpulse (*impulse);
                          return c;
                      },
                      [] (juce::AudioPluginInstance& p, const EffectValues& v)
                      {
                          auto* c = dynamic_cast<BuiltInEffects::ConvolutionProcessor*> (&p);

                          if (c != nullptr)
                              BuiltInEffects::setExactly (*c->mix, (float) v.at ("mix"));

                          return c != nullptr;
                      },
                      [this, impulse] (const EffectValues& v)
                      {
                          convolutionMix = v.at ("mix");
                          const auto r = selectionOrAll();

                          if (auto next = Convolver::apply (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), engine.document.getSampleRate(), *impulse, convolutionMix))
                              applyEdit (std::move (next), "Convolution Reverb", waveform.getSelection());
                      });
}

void MainComponent::showAbout()
{
    const auto version = juce::JUCEApplication::getInstance() != nullptr ? juce::JUCEApplication::getInstance()->getApplicationVersion() : juce::String();
    showNote ("About SpectraBlade",
              "SpectraBlade " + version + "\n"
              "Copyright (C) 2026 BobClawblaw\n\n"
              "This program is free software: you can redistribute it and/or modify it under the terms of the "
              "GNU Affero General Public License, version 3, as published by the Free Software Foundation.\n\n"
              "It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied "
              "warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the licence for more details: "
              "LICENSE.txt in the disk image, or https://www.gnu.org/licenses/agpl-3.0.html\n\n"
              "Source code: https://github.com/BobClawblaw/spectrablade\n"
              "Built with JUCE, used under the AGPLv3.");
}

void MainComponent::showGraphicEq (int numBands)
{
    auto& remembered = numBands == 31 ? graphicEq31Gains : graphicEq10Gains;
    const auto title = "Graphic EQ (" + juce::String (numBands) + " Bands)";
    auto* box = new juce::AlertWindow (title, "Each slider boosts or cuts a band around its frequency by up to 12 dB, "
                                              "as wide as the spacing; neighbouring bands overlap, so moving several together adds up. "
                                              "Double-click a slider for 0 dB.",
                                       juce::MessageBoxIconType::NoIcon, this);
    auto panel = std::make_shared<GraphicEq::Panel> (numBands);
    panel->setGains (remembered);
    box->addCustomComponent (panel.get());

    auto preview = std::make_shared<juce::ToggleButton> ("Preview on the selection");
    preview->setSize (260, 24);
    box->addCustomComponent (preview.get());

    // The live graphic EQ with the sliders' gains, on the selection over and over.
    const auto update = [this, numBands, p = panel.get(), toggle = preview.get()]
    {
        if (! toggle->getToggleState())
        {
            if (previewOwner == PreviewOwner::graphicEq)
                stopPreview();

            return;
        }

        if (auto* live = dynamic_cast<BuiltInEffects::GraphicEqProcessor*> (previewEffectOf (PreviewOwner::graphicEq)))
        {
            live->setGains (p->getGains());
            return;
        }

        auto made = std::make_unique<BuiltInEffects::GraphicEqProcessor> (numBands);
        made->setGains (p->getGains());
        startPreview (PreviewOwner::graphicEq, std::move (made), [toggle] { toggle->setToggleState (false, juce::dontSendNotification); });

        if (previewOwner != PreviewOwner::graphicEq)
            toggle->setToggleState (false, juce::dontSendNotification);
    };
    preview->onClick = update;
    panel->onChange = update;

    if (previewsFromStart())   // opened while the file plays: heard from the start
    {
        preview->setToggleState (true, juce::dontSendNotification);
        update();
    }

    // Presets under the live effect's IDs (band1 ... bandN), shared with it.
    auto presets = std::make_shared<EffectPresets::Bar> (EffectPresets::userStore(), numBands == 31 ? BuiltInEffects::graphicEq31Id : BuiltInEffects::graphicEq10Id,
                                                         [p = panel.get()]
                                                         {
                                                             EffectPresets::Values v;
                                                             const auto g = p->getGains();

                                                             for (size_t i = 0; i < g.size(); ++i)
                                                                 v["band" + juce::String ((int) i + 1)] = g[i];

                                                             return v;
                                                         },
                                                         [p = panel.get(), update] (const EffectPresets::Values& v)
                                                         {
                                                             auto g = p->getGains();

                                                             for (size_t i = 0; i < g.size(); ++i)
                                                                 if (const auto f = v.find ("band" + juce::String ((int) i + 1)); f != v.end() && std::isfinite (f->second))
                                                                     g[i] = f->second;

                                                             p->setGains (g);
                                                             update();
                                                         });
    box->addCustomComponent (presets.get());
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, numBands, title, panel, preview, presets] (int result)
    {
        if (previewOwner == PreviewOwner::graphicEq)
            stopPreview();

        if (result != 1)
            return;

        const auto gains = panel->getGains();
        (numBands == 31 ? graphicEq31Gains : graphicEq10Gains) = gains;
        const auto r = selectionOrAll();

        if (auto next = GraphicEq::apply (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), engine.document.getSampleRate(), gains))
            applyEdit (std::move (next), title, waveform.getSelection());
    }), true);
}

void MainComponent::showTremolo()
{
    showEffectDialog ("Tremolo", "The level dips by the depth and comes back this many times a second, starting at full level.",
                      { { "rate", "Rate (Hz)", tremoloRateHz, 2, {}, 0.1, 50.0, 0.1 },
                        { "depth", "Depth (%)", (double) tremoloDepthPercent, 0, {}, 0.0, 100.0, 1.0 } },
                      "spectrablade:tremolo", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          tremoloRateHz = v.at ("rate");
                          tremoloDepthPercent = juce::roundToInt (v.at ("depth"));
                          runStep ({ "tremolo", v, nullptr });
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "tremolo", v, nullptr }); });
}

void MainComponent::showGainRamp()
{
    // The ramp spans the whole selection, so the preview processes all of it.
    showEffectDialog ("Gain Ramp", "The gain moves evenly in decibels from the first level at the start of the selection "
                                   "to the second at its end.",
                      { { "from", "Start (dB)", rampFromDb, 1, {}, -96.0, 24.0, 0.5 }, { "to", "End (dB)", rampToDb, 1, {}, -96.0, 24.0, 0.5 } },
                      "spectrablade:gain-ramp", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v)
                      {
                          rampFromDb = (float) v.at ("from");
                          rampToDb = (float) v.at ("to");
                          const auto sel = waveform.getSelection();

                          if (! sel.isEmpty())
                              applyEdit (AudioEdits::gainRamp (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd(), rampFromDb, rampToDb), "Gain Ramp", sel);
                      },
                      [] (const EffectValues& v)
                      {
                          const auto from = (float) v.at ("from"), to = (float) v.at ("to");
                          return std::optional<EffectPreview::Request> ({ "gainRamp " + juce::String (from, 3) + " " + juce::String (to, 3),
                              [from, to] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double, const std::function<bool (float)>&)
                              {
                                  return EffectPreview::Rendered { AudioEdits::gainRamp (*audio, start, end, from, to), start, end, {} };
                              }, true });
                      });
}

void MainComponent::showFadeEdges (bool eachRegion)
{
    const auto title = juce::String (eachRegion ? "Fade Each Region" : "Fade Selection Edges");
    const auto what = juce::String (eachRegion ? "Every region" : "The selection");
    const auto rate = engine.document.getSampleRate();
    const auto framesOf = [rate] (const EffectValues& v) { return (juce::int64) std::llround (v.at ("ms") / 1000.0 * rate); };

    showEffectDialog (title, what + " fades in over this many milliseconds at its start and out over as many at its end "
                                    "(each at most half its length), with the fade shape chosen in Effects.",
                      { { "ms", "Fade (ms)", edgeFadeMs, 1, {}, 0.1, 10000.0, 5.0 } },
                      "spectrablade:fade-edges", PreviewOwner::none,
                      [framesOf] (const EffectValues& v) { return framesOf (v) > 0; }, "The fade must be at least one frame.", nullptr, nullptr,
                      [this, title, eachRegion, framesOf] (const EffectValues& v)
                      {
                          const auto sel = waveform.getSelection();
                          const auto fade = framesOf (v);

                          if (eachRegion)
                          {
                              auto skipped = 0;

                              if (auto next = AudioEdits::fadeRegions (*engine.document.getSnapshot(), engine.document.getMarkers(), fade, fadeShape, skipped))
                              {
                                  edgeFadeMs = v.at ("ms");
                                  applyEdit (std::move (next), title, sel);
                              }

                              if (skipped > 0)
                                  showError (title, juce::String (skipped) + (skipped == 1 ? " region was" : " regions were")
                                                        + " left as they were: one frame long, or overlapping a region before.");

                              return;
                          }

                          if (auto next = AudioEdits::fadeEdges (*engine.document.getSnapshot(), sel.getStart(), sel.getEnd(), fade, fadeShape))
                          {
                              edgeFadeMs = v.at ("ms");
                              applyEdit (std::move (next), "Fade Selection Edges", sel);
                          }
                          else
                          {
                              showError ("Fade Selection Edges", "The fade must be at least one frame, and the selection at least two frames long.");
                          }
                      },
                      [eachRegion, framesOf, shape = fadeShape, markers = engine.document.getMarkers()] (const EffectValues& v)
                      {
                          // The fades reach from the range's ends (or every region's), so the preview processes all of it.
                          const auto fade = framesOf (v);
                          return std::optional<EffectPreview::Request> ({ "fade " + juce::String (fade),
                              [eachRegion, fade, shape, markers] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double, const std::function<bool (float)>&)
                              {
                                  auto skipped = 0;
                                  return EffectPreview::Rendered { eachRegion ? AudioEdits::fadeRegions (*audio, markers, fade, shape, skipped)
                                                                              : AudioEdits::fadeEdges (*audio, start, end, fade, shape),
                                                                   start, end, {} };
                              }, true });
                      });
}

void MainComponent::applyFade (bool out)
{
    const auto r = waveform.getSelection();

    if (! r.isEmpty())
        applyEdit ((out ? AudioEdits::fadeOut : AudioEdits::fadeIn) (*engine.document.getSnapshot(), r.getStart(), r.getEnd(), fadeShape),
                   out ? "Fade Out" : "Fade In", r);
}

void MainComponent::convertChannels (int numChannels)
{
    auto& doc = engine.document;

    if (doc.hasAudio() && doc.getNumChannels() != numChannels)
        runStep ({ numChannels == 1 ? "mono" : "stereo", {}, nullptr });
}

void MainComponent::selectBetweenMarkers()
{
    if (multitrackMode)
    {
        multitrack.selectBetweenMarkers();
        return;
    }

    const auto& doc = engine.document;

    if (const auto span = MarkerEdits::spanAround (doc.getMarkers(), waveform.getCursor(), doc.getSnapshot()->getNumFrames()))
        waveform.setSelection (*span);
}

void MainComponent::showMarkersAtIntervals()
{
    auto* box = new juce::AlertWindow ("Add Markers at Intervals", "A marker every so many seconds (30) or minutes and seconds (1:30), "
                                       "through the selection or all of it. At most " + juce::String (MarkerEdits::maxIntervalMarkers) + ".",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("step", "30");
    box->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        const auto rate = multitrackMode ? engine.session.getSampleRate() : engine.document.getSampleRate();
        const auto step = TimeEntry::parse (box->getTextEditorContents ("step"), rate, nullptr);
        auto added = 0;

        if (step && *step > 0)
        {
            if (multitrackMode)
            {
                added = multitrack.addMarkersAtIntervals (*step);
            }
            else
            {
                auto& doc = engine.document;
                auto markers = doc.getMarkers();
                added = MarkerEdits::addAtIntervals (markers, selectionOrAll(), *step);

                if (added > 0)
                    doc.setMarkers (std::move (markers), "Add " + juce::String (added) + (added == 1 ? " Marker" : " Markers"));
            }
        }

        if (added == 0)
            showError ("Add Markers at Intervals", "No markers were added: the interval must be a time more than 0, give at most "
                                                       + juce::String (MarkerEdits::maxIntervalMarkers) + " markers, and reach a frame without one.");
    }), true);
}

void MainComponent::showRenameMarkers()
{
    auto* box = new juce::AlertWindow ("Rename Markers in Sequence", "Every marker and region, in time order, gets this name and a number: Take 01, Take 02, ...",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("base", "Take");
    box->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box] (int result)
    {
        if (result != 1)
            return;

        const auto base = box->getTextEditorContents ("base");

        if (multitrackMode)
        {
            multitrack.renameMarkersInSequence (base);
            return;
        }

        auto& doc = engine.document;
        auto markers = doc.getMarkers();

        if (MarkerEdits::renameInSequence (markers, base) && markers != doc.getMarkers())
            doc.setMarkers (std::move (markers), "Rename Markers");
    }), true);
}

void MainComponent::regionsFromMarkers()
{
    if (multitrackMode)
    {
        multitrack.regionsFromMarkers();
        return;
    }

    auto& doc = engine.document;
    auto markers = doc.getMarkers();

    if (MarkerEdits::regionsFromPoints (markers, doc.getNumFrames()) > 0)
        doc.setMarkers (std::move (markers), "Regions from Markers");
}

Markers MainComponent::markersShown() const
{
    return multitrackMode ? engine.session.getState()->markers : engine.document.getMarkers();
}

void MainComponent::selectAdjacentRegion (bool forwards)
{
    if (multitrackMode)
    {
        multitrack.selectAdjacentRegion (forwards);
        return;
    }

    const auto sel = waveform.getSelection();

    if (const auto region = MarkerEdits::adjacentRegion (engine.document.getMarkers(), sel.isEmpty() ? waveform.getCursor() : sel.getStart(), forwards))
    {
        waveform.setSelection (*region);
        waveform.showFrame (region->getStart());
    }
}

void MainComponent::loopRegion()
{
    if (isRecording())
        return;

    const auto cursor = multitrackMode ? multitrack.getCursor() : waveform.getCursor();
    const auto region = MarkerEdits::regionAt (markersShown(), cursor);

    if (! region)
    {
        showError ("Loop Region", "The cursor is not in a region.");
        return;
    }

    if (multitrackMode)
        multitrack.setTimeSelection (*region);
    else
        waveform.setSelection (*region);

    if (! loopPlayback)
    {
        loopPlayback = true;
        appProperties.getUserSettings()->setValue (loopPlaybackKey, loopPlayback);
    }

    // Start again from the region's start, whatever was playing.
    if (engine.getPlayer().isPlaying())
        togglePlay();

    updatePlaybackLoop();
    togglePlay();
    commands.commandStatusChanged();
}

void MainComponent::selectRegionAtCursor()
{
    if (multitrackMode)
        multitrack.selectRegionAtCursor();
    else if (const auto region = MarkerEdits::regionAt (engine.document.getMarkers(), waveform.getCursor()))
        waveform.setSelection (*region);
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

void MainComponent::paste (PasteMode mode)
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
            [this, converted, version, mode]
            {
                if (engine.document.getVersion() != version)
                    showError ("Paste", "The audio changed while the clipboard was converted, so nothing was pasted.");
                else
                    pasteClip (*converted, mode);
            });
        backgroundJob->launchThread();
        return;
    }

    pasteClip (clipboard, mode);
}

void MainComponent::pasteClip (const AudioClip& clip, PasteMode mode)
{
    auto& doc = engine.document;
    auto sel = waveform.getSelection();

    if (mode == PasteMode::fill)
    {
        if (auto next = AudioEdits::fillWithClip (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), clip))
            applyEdit (std::move (next), "Paste to Fill Selection", sel);
        else if (! sel.isEmpty())
            showError ("Paste to Fill Selection", "The clipboard is too short to fill this selection: it would take more than "
                                                   + juce::String (AudioEdits::maxFillCopies) + " copies.");

        return;
    }

    if (mode == PasteMode::mix)
    {
        const auto at = sel.isEmpty() ? waveform.getCursor() : sel.getStart();

        if (auto next = AudioEdits::mixClip (*doc.getSnapshot(), at, clip))
            applyEdit (std::move (next), "Paste Mix", { at, at + clip.getNumFrames() });
        else
            showError ("Paste Mix", "The clipboard is too long to mix in one pass.");

        return;
    }

    if (sel.isEmpty())
        sel = { waveform.getCursor(), waveform.getCursor() };

    applyEdit (AudioEdits::replaceWithClip (*doc.getSnapshot(), sel.getStart(), sel.getEnd(), clip),
               "Paste", { sel.getStart(), sel.getStart() + clip.getNumFrames() },
               MarkerEdits::afterReplace (doc.getMarkers(), sel.getStart(), sel.getEnd(), clip.getNumFrames()));
}

void MainComponent::deleteSelection()
{
    const auto sel = waveform.getSelection();

    // With a box, lasso or brush selection in the spectrogram, Delete removes what is inside it
    // (as Remove Frequencies does), keeping the file's length; a plain selection takes its time out.
    if (! waveform.getSpectralShape().isEmpty() || (! sel.isEmpty() && ! waveform.getSpectralBand().isEmpty()))
    {
        applySpectral (-std::numeric_limits<float>::infinity());
        return;
    }

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
void MainComponent::askForDecibels (const juce::String& title, const juce::String& label, float initial, double min, double max,
                                    const juce::String& presetKey, std::function<void (float)> onOk,
                                    std::function<std::optional<EffectPreview::Request> (float)> rendered)
{
    showEffectDialog (title, {}, { { "db", label, juce::jlimit (min, max, (double) initial), 1, {}, min, max, 0.5 } },
                      presetKey, PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [onOk] (const EffectValues& v) { onOk ((float) v.at ("db")); },
                      rendered == nullptr ? std::function<std::optional<EffectPreview::Request> (const EffectValues&)>()
                                          : [rendered] (const EffectValues& v) { return rendered ((float) v.at ("db")); });
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
        askForDecibels ("Clip Gain", "Gain (dB)", state->tracks[(size_t) where->first].clips[(size_t) where->second].gainDb,
                        SessionClip::minGainDb, SessionClip::maxGainDb, {}, [this, clipId] (float db)
        {
            auto next = *engine.session.getState();

            if (SessionEdits::setClipGain (next, clipId, db))
                engine.session.apply (std::move (next), "Clip Gain");
        });
        return;
    }

    showEffectDialog ("Gain", "Raises or lowers the selection, or the whole file, by this many decibels.",
                      { { "db", "Gain (dB)", 0.0, 1, {}, -48.0, 48.0, 0.5 } },
                      "spectrablade:gain", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this] (const EffectValues& v) { runStep ({ "gain", v, nullptr }); },
                      [this] (const EffectValues& v) { return previewOf ({ "gain", v, nullptr }); });
}

void MainComponent::applyNormalize()
{
    if (multitrackMode)
    {
        const auto clipId = multitrack.getSelectedClip();
        askForDecibels ("Normalize Clip", "Peak level (dBFS)", -0.1f, -48.0, 0.0, "spectrablade:normalize-regions", [this, clipId] (float db)
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

    // Its peak is found across the whole selection, so the preview processes all of it.
    const auto toStep = [] (const EffectValues& v)
    {
        return Sequences::Step { "normalize", { { "db", juce::jmin (v.at ("db"), 0.0) }, { "apart", v.count ("apart") > 0 ? v.at ("apart") : 0.0 } }, nullptr };
    };

    std::vector<EffectField> fields { { "db", "Peak level (dBFS)", -0.1, 2, {}, -48.0, 0.0, 0.1 } };

    if (engine.document.getNumChannels() > 1)
        fields.push_back ({ "apart", "Channels", normalizeChannelsApart ? 1.0 : 0.0, 0, { "All together", "Each on its own" } });

    showEffectDialog ("Normalize", "Raises or lowers the selection, or the whole file, so its highest peak reaches the level; with "
                                   "Each on its own, each channel's peak does.",
                      fields, "spectrablade:normalize", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, toStep] (const EffectValues& v)
                      {
                          const auto step = toStep (v);
                          normalizeChannelsApart = step.values.at ("apart") > 0.5;

                          if (runStep (step).snapshot == nullptr)
                              showError ("Normalize", "The selection is silent.");
                      },
                      [this, toStep] (const EffectValues& v) { return previewOf (toStep (v), true); });
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

        auto all = rows;
        const auto levels = AudioEdits::channelLevels (*engine.document.getSnapshot(), range.getStart(), range.getEnd());
        const auto db = [] (double gain) { return gain > 0.0 ? juce::String (juce::Decibels::gainToDecibels (gain), 2) + " dB" : juce::String ("-inf dB"); };

        for (size_t ch = 0; ch < levels.size(); ++ch)
        {
            const auto label = levels.size() == 2 ? juce::String (ch == 0 ? "Left" : "Right") : "Channel " + juce::String ((int) ch + 1);
            all.add ({});
            all.add (label + " peak:" + juce::String::repeatedString (" ", juce::jmax (1, 17 - label.length())) + db (levels[ch].peak));
            all.add (label + " RMS:" + juce::String::repeatedString (" ", juce::jmax (1, 18 - label.length())) + db (levels[ch].rms));
            all.add (label + " DC offset:" + juce::String::repeatedString (" ", juce::jmax (1, 12 - label.length()))
                     + juce::String (levels[ch].mean * 100.0, 4) + " %");
        }

        auto* box = new juce::AlertWindow ("Loudness Statistics",
                                           (whole ? juce::String ("Whole file") : juce::String ("Selection")) + ", "
                                               + formatTime (range.getLength(), engine.document.getSampleRate())
                                               + ". EBU R128 / ITU-R BS.1770-4.",
                                           juce::MessageBoxIconType::NoIcon, this);
        auto* text = new juce::TextEditor();
        text->setMultiLine (true);
        text->setReadOnly (true);
        text->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
        if (const auto correlation = AudioEdits::stereoCorrelation (*engine.document.getSnapshot(), range.getStart(), range.getEnd()))
        {
            all.add ({});
            all.add ("Stereo correlation:    " + juce::String (*correlation >= 0.0 ? "+" : "") + juce::String (*correlation, 3));
        }

        text->setText (all.joinIntoString ("\n"));
        text->setSize (360, 150 + 72 * (int) levels.size());
        box->addCustomComponent (text);
        box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([text] (int) { delete text; }), true);
    });
}

void MainComponent::askForLoudnessTarget()
{
    auto* settings = appProperties.getUserSettings();

    // The loudness is measured across the whole selection, so the preview processes all of it.
    showEffectDialog ("Match Loudness",
                      "Common targets: EBU R128 -23, ATSC A/85 -24, podcasts -16, streaming -14 LUFS.\n"
                      "When the true peak would pass the ceiling, the gain either stops short of the target, or goes all "
                      "the way with a true-peak limiter holding the peaks at the ceiling.",
                      { { "target", "Target (LUFS)", settings->getDoubleValue ("loudnessTarget", -23.0), 1, {}, -70.0, 0.0, 0.5 },
                        { "ceiling", "True peak ceiling (dBTP)", settings->getDoubleValue ("truePeakCeiling", -1.0), 1, {}, -30.0, 0.0, 0.1 },
                        { "limit", "When the ceiling stops the gain", settings->getBoolValue ("loudnessLimit", false) ? 1.0 : 0.0, 0,
                          { "Stop short of the target", "Limit to reach the target" } } },
                      "spectrablade:match-loudness", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings] (const EffectValues& v)
                      {
                          settings->setValue ("loudnessTarget", v.at ("target"));
                          settings->setValue ("truePeakCeiling", v.at ("ceiling"));
                          settings->setValue ("loudnessLimit", v.at ("limit") > 0.5);
                          matchLoudness (v.at ("target"), v.at ("ceiling"), v.at ("limit") > 0.5);
                      },
                      [this] (const EffectValues& v) { return previewOf ({ "matchLoudness", v, nullptr }, true); },
                      "Match");
}

void MainComponent::askForDeliverySpec()
{
    auto* settings = appProperties.getUserSettings();
    const auto specs = deliverySpecs();
    const auto& own = specs.back();
    juce::StringArray names;

    for (auto& spec : specs)
        names.add (&spec == &own ? juce::String ("Custom spec, as set below") : describe (spec));

    // The spec chosen, the custom one with the values set below.
    const auto specOf = [specs] (const EffectValues& v)
    {
        const auto index = juce::jlimit (0, (int) specs.size() - 1, (int) v.at ("spec"));
        auto spec = specs[(size_t) index];

        if (index == (int) specs.size() - 1)
        {
            spec.targetLufs = v.at ("target");
            spec.toleranceLu = v.at ("tolerance");
            spec.maxTruePeak = v.at ("ceiling");
        }

        return spec;
    };

    // The loudness is measured across the whole selection, so the preview processes all of it.
    showEffectDialog ("Meet Delivery Spec",
                      "Brings the selection (or the whole file) to the spec's loudness in one step: gain, and a true-peak "
                      "limiter only where the gain alone would take the true peak past the ceiling. Then it is measured "
                      "again and checked against the spec. The custom spec is kept for Mix Down, Export Stems and Batch Process.",
                      { { "spec", "Spec", (double) juce::jlimit (0, names.size() - 1, settings->getIntValue ("deliverySpec", 0)), 0, names },
                        { "target", "Custom: target (LUFS)", own.targetLufs, 1, {}, -70.0, 0.0, 0.5 },
                        { "tolerance", "Custom: tolerance (LU)", own.toleranceLu, 1, {}, 0.1, 10.0, 0.1 },
                        { "ceiling", "Custom: true peak at most (dBTP)", own.maxTruePeak, 1, {}, -30.0, 0.0, 0.1 } },
                      {}, PreviewOwner::none,
                      [specOf] (const EffectValues& v) { const auto s = specOf (v); return Delivery::isValid (s.targetLufs, s.toleranceLu, s.maxTruePeak); },
                      "The custom spec needs a target from -70 to 0 LUFS, a tolerance from 0.1 to 10 LU and a true-peak ceiling from -30 to 0 dBTP.",
                      nullptr, nullptr,
                      [this, settings, specOf] (const EffectValues& v)
                      {
                          if (Delivery::isValid (v.at ("target"), v.at ("tolerance"), v.at ("ceiling")))
                          {
                              settings->setValue ("deliveryOwnLufs", v.at ("target"));
                              settings->setValue ("deliveryOwnTolerance", v.at ("tolerance"));
                              settings->setValue ("deliveryOwnCeiling", v.at ("ceiling"));
                          }

                          settings->setValue ("deliverySpec", (int) v.at ("spec"));
                          meetDeliverySpec (specOf (v));
                      },
                      [this, specOf] (const EffectValues& v)
                      {
                          const auto s = specOf (v);
                          return previewOf ({ "deliverySpec", { { "target", s.targetLufs }, { "tolerance", s.toleranceLu }, { "ceiling", s.maxTruePeak } }, nullptr }, true);
                      },
                      "Meet");
}

void MainComponent::meetDeliverySpec (const Delivery::Spec& spec)
{
    auto& doc = engine.document;

    if (backgroundJob != nullptr || doc.getNumFrames() == 0)
        return;

    const auto snapshot = doc.getSnapshot();
    const auto rate = doc.getSampleRate();
    const auto range = selectionOrAll();
    const auto version = doc.getVersion();
    auto delivered = std::make_shared<Delivery::Delivered>();

    backgroundJob = std::make_unique<BackgroundJob> (*this, "Meeting " + spec.name + "...",
        [delivered, snapshot, rate, range, spec] (const std::function<bool (float)>& progress)
        {
            *delivered = Delivery::deliver (*snapshot, range.getStart(), range.getEnd(), rate, spec, progress);
            return delivered->snapshot != nullptr || delivered->error.isNotEmpty();
        },
        [this, delivered, version, spec]
        {
            if (delivered->error.isNotEmpty())
            {
                showError ("Meet Delivery Spec", delivered->error);
                return;
            }

            if (engine.document.getVersion() != version)
            {
                showError ("Meet Delivery Spec", "The audio changed meanwhile, so the result was discarded.");
                return;
            }

            const auto name = engine.document.getFile().getFileName();
            applyEdit (std::move (delivered->snapshot), "Meet " + spec.name, waveform.getSelection());
            recordStep ({ "deliverySpec", { { "target", spec.targetLufs }, { "tolerance", spec.toleranceLu }, { "ceiling", spec.maxTruePeak } }, nullptr });
            showDeliveryReport (Delivery::report (spec, *delivered, name.isNotEmpty() ? name : juce::String ("Untitled")));
        });
    backgroundJob->launchThread();
}

void MainComponent::showDeliveryReport (const juce::String& report, const juce::File& suggested)
{
    auto* box = new juce::AlertWindow ("Delivery Report", {}, juce::MessageBoxIconType::NoIcon, this);
    box->addTextBlock (report);
    box->addButton ("Save Report...", 1);
    box->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, report, suggested] (int result)
    {
        if (result != 1)
            return;

        const auto from = suggested != juce::File() ? suggested : engine.document.getFile();
        const auto start = from != juce::File() ? from.getSiblingFile (from.getFileNameWithoutExtension() + " Delivery Report.txt")
                                                : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Delivery Report.txt");
        chooser = std::make_unique<juce::FileChooser> ("Save the delivery report", start, "*.txt");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [report] (const juce::FileChooser& fc)
                              {
                                  const auto file = fc.getResult();

                                  if (file != juce::File() && ! file.replaceWithText (report + "\n"))
                                      showError ("Delivery Report", "Could not write " + file.getFullPathName());
                              });
    }), true);
}

void MainComponent::matchLoudness (double target, double ceiling, bool limitToTarget)
{
    measureLoudness ([this, target, ceiling, limitToTarget] (const Loudness::Result& r, juce::Range<juce::int64> range)
    {
        if (! std::isfinite (r.integrated))
        {
            showError ("Match Loudness", "There is nothing above the -70 LUFS gate to measure.");
            return;
        }

        const auto wanted = target - r.integrated;
        const auto gain = juce::jmin (wanted, ceiling - r.truePeak);

        if (limitToTarget && gain < wanted)
        {
            auto& doc = engine.document;
            auto matched = Dynamics::matchLoudness (*doc.getSnapshot(), range.getStart(), range.getEnd(), doc.getSampleRate(), r.integrated, target, ceiling);

            if (matched.snapshot == nullptr)
                return;

            applyEdit (std::move (matched.snapshot), "Match Loudness " + Loudness::format (target, "LUFS"), waveform.getSelection());
            recordStep ({ "matchLoudness", { { "target", target }, { "ceiling", ceiling }, { "limit", limitToTarget ? 1.0 : 0.0 } }, nullptr });

            if (std::abs (matched.integrated - target) > 0.05)
                showError ("Match Loudness", "With " + juce::String (matched.gainDb, 1) + " dB of gain into the limiter, the result is "
                                                 + Loudness::format (matched.integrated, "LUFS") + " instead of " + Loudness::format (target, "LUFS") + ".");

            return;
        }

        applyEdit (AudioEdits::gain (*engine.document.getSnapshot(), range.getStart(), range.getEnd(), (float) gain),
                   "Match Loudness " + Loudness::format (target, "LUFS"), waveform.getSelection());
        recordStep ({ "matchLoudness", { { "target", target }, { "ceiling", ceiling }, { "limit", limitToTarget ? 1.0 : 0.0 } }, nullptr });

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
    const auto toSettings = [] (const EffectValues& v)
    {
        NoiseReduction::Settings s;
        s.reductionDb = (float) v.at ("reduction");
        s.thresholdDb = (float) v.at ("threshold");
        return s;
    };

    showEffectDialog ("Noise Reduction",
                      "Lowers whatever matches the captured noise print by up to the reduction. Raise the "
                      "threshold if noise is left between sounds; lower it if quiet sounds are thinned.",
                      { { "reduction", "Reduction (dB)", settings->getDoubleValue ("noiseReduction", 12.0), 1, {}, 0.0, 60.0, 0.5 },
                        { "threshold", "Threshold above the print (dB)", settings->getDoubleValue ("noiseThreshold", 3.0), 1, {}, -10.0, 20.0, 0.5 } },
                      "spectrablade:noise-reduction", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings, toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          settings->setValue ("noiseReduction", s.reductionDb);
                          settings->setValue ("noiseThreshold", s.thresholdDb);
                          reduceNoise (s);
                      },
                      [print = *noisePrint, toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "noise " + juce::String (s.reductionDb, 3) + " " + juce::String (s.thresholdDb, 3),
                              [print, s] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  return EffectPreview::Rendered { NoiseReduction::reduce (*audio, start, end, sampleRate, print, s, progress), start, end, {} };
                              } });
                      },
                      "Reduce");
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
    const auto toSettings = [] (const EffectValues& v)
    {
        ClickRepair::Settings s;
        s.thresholdDb = (float) v.at ("threshold");
        s.maxLengthMs = (float) v.at ("length");
        return s;
    };

    showEffectDialog ("Click and Pop Repair",
                      "Finds clicks and pops and rebuilds those samples from the audio around them; nothing else "
                      "changes. Lower the threshold to catch fainter clicks; raise it if drums or plucks are "
                      "being taken for clicks.",
                      { { "threshold", "Threshold (dB above the background)", settings->getDoubleValue ("clickThreshold", 18.0), 1, {}, 6.0, 60.0, 0.5 },
                        { "length", "Longest click (ms)", settings->getDoubleValue ("clickMaxLength", 3.0), 1, {}, 0.1, 20.0, 0.1 } },
                      "spectrablade:click-repair", PreviewOwner::none, [] (const EffectValues&) { return true; }, {}, nullptr, nullptr,
                      [this, settings, toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          settings->setValue ("clickThreshold", s.thresholdDb);
                          settings->setValue ("clickMaxLength", s.maxLengthMs);
                          repairClicks (s);
                      },
                      [toSettings] (const EffectValues& v)
                      {
                          const auto s = toSettings (v);
                          return std::optional<EffectPreview::Request> ({ "clicks " + juce::String (s.thresholdDb, 3) + " " + juce::String (s.maxLengthMs, 3),
                              [s] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
                              {
                                  const auto r = ClickRepair::repair (*audio, start, end, sampleRate, s, progress);
                                  return EffectPreview::Rendered { r.has_value() ? r->snapshot : nullptr, start, end,
                                                                   r.has_value() ? juce::String() : juce::String ("Stopped.") };
                              } });
                      },
                      "Repair");
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
    if (! engine.isDeviceOpen())
        return;

    if (isRecording())
    {
        stopRecording();
        return;
    }

    auto& player = engine.getPlayer();

    if (player.isPlaying())
    {
        player.pause();
        stopPlayingInside();
        return;
    }

    if (insidePending)
    {
        // Play again while the inside of a spectral selection is still being processed: don't.
        stopPlayingInside();
        return;
    }

    // A selection or time range plays and stops at its end (or, looping, starts again);
    // otherwise play from the cursor to the end.
    const auto range = playRange();
    const auto cursor = multitrackMode ? multitrack.getCursor() : waveform.getCursor();
    const auto length = multitrackMode ? engine.session.getState()->getLength() : engine.document.getNumFrames();

    if (! range.isEmpty())
    {
        // A spectral selection plays only what is inside it, once that is processed.
        if (! multitrackMode && previewOwner == PreviewOwner::none
            && (! waveform.getSpectralShape().isEmpty() || ! waveform.getSpectralBand().isEmpty()))
        {
            playInsideSpectral (range);
            return;
        }

        player.play (range.getStart(), range.getEnd());
    }
    else
    {
        player.play (cursor >= length ? 0 : cursor, -1);
    }

    playingLoop = {};
    updatePlaybackLoop();
}

void MainComponent::playInsideSpectral (juce::Range<juce::int64> range)
{
    auto& doc = engine.document;
    insideRange = range;
    insideVersion = doc.getVersion();
    insidePending = true;

    const EffectPreview::Request request { "inside",
        [shape = waveform.getSpectralShape(), band = waveform.getSpectralBand(), sel = range]
        (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>&)
        {
            constexpr auto gone = -std::numeric_limits<float>::infinity();
            return EffectPreview::Rendered { ! shape.isEmpty() ? SpectralEdit::changeOutsideShape (*audio, shape, sampleRate, gone)
                                                                : SpectralEdit::changeOutsideBand (*audio, sel.getStart(), sel.getEnd(), band, sampleRate, gone),
                                             start, end, {} };
        }, true };

    effectPreview.render (request, doc.getSnapshot(), range, doc.getSampleRate(), [this, range] (const EffectPreview::Rendered& r)
    {
        if (! insidePending || range != insideRange || engine.document.getVersion() != insideVersion || previewOwner != PreviewOwner::none || multitrackMode)
            return;

        insidePending = false;
        insideAudio = r.snapshot;   // null (nothing to take out): the document plays as it is
        auto& player = engine.getPlayer();
        player.setStandIn (insideAudio);
        player.play (range.getStart(), range.getEnd());
        playingLoop = {};
        updatePlaybackLoop();
    });
}

void MainComponent::stopPlayingInside()
{
    if (insidePending && previewOwner == PreviewOwner::none)
        effectPreview.cancel();

    insidePending = false;

    if (insideAudio != nullptr)
    {
        insideAudio = nullptr;

        if (previewOwner == PreviewOwner::none)
            engine.getPlayer().setStandIn (nullptr);
    }
}

juce::Range<juce::int64> MainComponent::playRange() const
{
    return multitrackMode ? multitrack.getTimeSelection() : waveform.getSelection();
}

juce::int64 MainComponent::zeroCrossingReach() const
{
    return (juce::int64) std::llround (MultitrackView::zeroCrossingReach * engine.document.getSampleRate());
}

void MainComponent::nudgeSelection (bool start, bool later)
{
    const auto sel = waveform.getSelection();

    if (multitrackMode || sel.isEmpty())
        return;

    // A pixel's worth of time at this zoom, a frame at the least.
    const auto step = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (waveform.getVisibleFrames().getLength() / juce::jmax (1, waveform.getWidth())));
    auto r = AudioEdits::nudgeSelection (sel, engine.document.getNumFrames(), start, later, step);

    if (selectZeroCrossings)
    {
        // On to the next crossing the way it moved, stopping a frame short of the other end.
        const auto moved = AudioEdits::zeroCrossingFrom (engine.document.getSnapshot(), start ? r.getStart() : r.getEnd(), later, zeroCrossingReach());
        r = start ? juce::Range<juce::int64> (juce::jmin (moved, r.getEnd() - 1), r.getEnd())
                  : juce::Range<juce::int64> (r.getStart(), juce::jmax (moved, r.getStart() + 1));
    }

    waveform.setSelection (r);
}

void MainComponent::updatePlaybackLoop()
{
    auto& player = engine.getPlayer();

    // Loop recording keeps its own loop, and a preview its own.
    if (isRecording() || previewOwner != PreviewOwner::none)
        return;

    const auto wanted = loopPlayback && player.isPlaying() ? playRange() : juce::Range<juce::int64>();

    if (wanted == playingLoop)
        return;

    playingLoop = wanted;

    if (wanted.isEmpty())
    {
        // Loop off, or no range: the play goes on to its stop (the end of the last loop, or the end).
        player.clearLoop();
        return;
    }

    // The play stops by itself where it started, the selection's end then; a selection changed
    // while looping moves the stop to its new end first, so the sound carries on past the old one
    // (and the view does not stop the play there). A play now outside the loop (the selection
    // shortened behind it, or moved) starts the loop again from its start.
    player.setStopFrame (wanted.getEnd());
    player.setLoop (wanted.getStart(), wanted.getEnd());

    if (! wanted.contains (player.getPositionFrames()))
        player.setPositionFrames (wanted.getStart());
}

void MainComponent::startPreview (PreviewOwner owner, std::unique_ptr<juce::AudioPluginInstance> effect, std::function<void()> ended)
{
    if (multitrackMode || isRecording() || ! engine.document.hasAudio() || owner == PreviewOwner::none || owner == PreviewOwner::rendered)
        return;

    stopPlayingInside();

    if (previewOwner == PreviewOwner::rendered)
        stopPreview();

    const auto already = previewOwner != PreviewOwner::none;

    if (already && owner != previewOwner)
        if (auto old = std::exchange (previewEnded, {}))
            old();

    previewOwner = owner;
    previewEnded = std::move (ended);
    engine.setPreviewEffect (std::move (effect));
    engine.setPreviewBypassed (previewBypassed);

    if (already)
        return;

    auto r = selectionOrAll();
    auto& player = engine.getPlayer();
    player.setLoop (r.getStart(), r.getEnd());
    player.play (r.getStart(), -1);
    playingLoop = r;
}

void MainComponent::startRenderedPreview (const EffectPreview::Request& request, std::function<void()> ended,
                                          std::function<void (const juce::String&)> status)
{
    auto& doc = engine.document;

    if (multitrackMode || isRecording() || ! doc.hasAudio())
        return;

    stopPlayingInside();

    if (previewOwner != PreviewOwner::rendered)
    {
        stopPreview();
        previewOwner = PreviewOwner::rendered;
    }

    previewEnded = std::move (ended);
    renderedKey = request.key;
    renderedVersion = doc.getVersion();

    const auto range = selectionOrAll();
    const auto rate = doc.getSampleRate();
    const auto part = EffectPreview::partOf (range, rate, request.wholeRange);
    const auto shortened = part.getLength() < range.getLength();

    if (status != nullptr)
        status (request.generates ? "Generating..."
                : request.wholeRange && range.getLength() > (juce::int64) (EffectPreview::longestSeconds * rate)
                    ? "Processing all of it (as long as applying)..." : "Processing...");

    const auto generates = request.generates;
    effectPreview.render (request, doc.getSnapshot(), range, rate, [this, part, shortened, status, generates] (const EffectPreview::Rendered& r)
    {
        if (previewOwner != PreviewOwner::rendered || engine.document.getVersion() != renderedVersion)
            return;

        if (r.error.isNotEmpty())
        {
            if (status != nullptr)
                status ("Can't preview: " + r.error);

            return;
        }

        renderedAudio = r.snapshot != nullptr ? r.snapshot : engine.document.getSnapshot();
        renderedLoop = { r.start, juce::jmax (r.start + 1, r.end) };
        renderedPart = generates ? renderedLoop : part;   // bypassed, a generator plays what is there now

        auto& player = engine.getPlayer();
        const auto loop = previewBypassed ? renderedPart : renderedLoop;
        player.setStandIn (previewBypassed ? nullptr : renderedAudio);
        player.setLoop (loop.getStart(), loop.getEnd());
        playingLoop = loop;

        // A preview loops until the dialog ends it: the play's own stop (the selection's end when
        // Play was pressed) would stop it where the rendered audio runs past that, and a play
        // outside the loop would never come round to it.
        if (! player.isPlaying())
            player.play (loop.getStart(), -1);
        else
        {
            player.setStopFrame (-1);

            if (! loop.contains (player.getPositionFrames()))
                player.setPositionFrames (loop.getStart());
        }

        if (status != nullptr)
            status (r.note.isNotEmpty() ? r.note : ! generates && shortened ? "Playing the first " + juce::String ((int) EffectPreview::longestSeconds) + " s" : juce::String());
    });
}

void MainComponent::setPreviewBypassed (bool bypassed)
{
    previewBypassed = bypassed;

    if (previewOwner == PreviewOwner::rendered)
    {
        if (renderedAudio == nullptr)
            return;

        auto& player = engine.getPlayer();
        const auto loop = bypassed ? renderedPart : renderedLoop;
        player.setStandIn (bypassed ? nullptr : renderedAudio);
        player.setLoop (loop.getStart(), loop.getEnd());
        playingLoop = loop;

        if (! loop.contains (player.getPositionFrames()))
            player.setPositionFrames (loop.getStart());
    }
    else if (previewOwner != PreviewOwner::none)
    {
        engine.setPreviewBypassed (bypassed);
    }
}

void MainComponent::stopPreview()
{
    if (previewOwner == PreviewOwner::none)
        return;

    auto& player = engine.getPlayer();
    player.pause();
    player.clearLoop();
    playingLoop = {};

    if (previewOwner == PreviewOwner::rendered)
    {
        effectPreview.cancel();
        player.setStandIn (nullptr);
        renderedAudio = nullptr;
        renderedKey = {};
    }
    else
    {
        engine.setPreviewEffect (nullptr);
    }

    previewOwner = PreviewOwner::none;
    previewBypassed = false;

    if (auto ended = std::exchange (previewEnded, {}))
        ended();
}

std::optional<EffectPreview::Request> MainComponent::previewOf (const Sequences::Step& step, bool wholeRange) const
{
    return EffectPreview::ofStep (step, engine.document.getSampleRate(), wholeRange);
}

juce::AudioPluginInstance* MainComponent::previewEffectOf (PreviewOwner owner) const
{
    return owner != PreviewOwner::none && owner == previewOwner ? engine.getPreviewEffect() : nullptr;
}

void MainComponent::askToNewFile()
{
    if (isRecording())
        return;

    const juce::Array<int> rates { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    const juce::Array<int> channelCounts { 1, 2, 6, 8 }, depths { 16, 24, 32 };
    constexpr double shortest = 0.1, longest = 3600.0;

    auto* box = new juce::AlertWindow ("New File", "A new file of silence to record, paste or generate into. It opens beside the files open, "
                                                   "unsaved until Save As. Length " + juce::String (shortest, 1) + " to 3,600 s.",
                                       juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray rateNames, channelNames { "Mono", "Stereo", "5.1 (6 channels)", "7.1 (8 channels)" }, depthNames { "16-bit", "24-bit", "32-bit float" };

    for (auto r : rates)
        rateNames.add (juce::String (r) + " Hz");

    // The interface's rate to start with, as a recording would take it.
    auto rate = newFileRate;

    if (rate == 0)
        if (auto* device = engine.deviceManager.getCurrentAudioDevice())
            rate = (int) std::llround (device->getCurrentSampleRate());

    box->addComboBox ("rate", rateNames, "Sample rate");
    box->addComboBox ("channels", channelNames, "Channels");
    box->addComboBox ("depth", depthNames, "Bit depth");
    box->addTextEditor ("seconds", juce::String (newFileSeconds, 1), "Length (s)");
    box->getComboBoxComponent ("rate")->setSelectedItemIndex (juce::jmax (0, rates.indexOf (rates.contains (rate) ? rate : 48000)));
    box->getComboBoxComponent ("channels")->setSelectedItemIndex (juce::jmax (0, channelCounts.indexOf (newFileChannels)));
    box->getComboBoxComponent ("depth")->setSelectedItemIndex (juce::jmax (0, depths.indexOf (newFileDepth)));
    box->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([this, box, rates, channelCounts, depths] (int result)
    {
        if (result != 1)
            return;

        const auto seconds = box->getTextEditorContents ("seconds").getDoubleValue();

        if (! (seconds >= shortest && seconds <= longest))
        {
            juce::MessageManager::callAsync ([] { showError ("New File", "The length must be from 0.1 to 3,600 s."); });
            return;
        }

        newFileRate = rates[box->getComboBoxComponent ("rate")->getSelectedItemIndex()];
        newFileChannels = channelCounts[box->getComboBoxComponent ("channels")->getSelectedItemIndex()];
        newFileDepth = depths[box->getComboBoxComponent ("depth")->getSelectedItemIndex()];
        newFileSeconds = seconds;
        newFile (newFileRate, newFileChannels, newFileDepth, seconds);
    }), true);
}

void MainComponent::newFile (double sampleRate, int channels, int bitDepth, double seconds)
{
    if (isRecording())
        return;

    // Beside the files open, as an opened file goes; the editor starts at the file's start.
    stopPreview();
    openFiles.prepareForAnother (waveform.getPlace());
    engine.newDocument (channels, sampleRate, bitDepth, (juce::int64) std::llround (seconds * sampleRate));
    setMultitrack (false);
    waveform.setPlace ({});
    waveform.zoomToFit();
    refreshTitle();
    timerCallback();
    commands.commandStatusChanged();
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
            recordStep ({ "convertRate", { { "rate", newRate } }, nullptr });
        });
    backgroundJob->launchThread();
}

void MainComponent::toggleRecord()
{
    if (! engine.isDeviceOpen())
        return;

    if (isRecording())
        stopRecording();
    else
        startRecording();
}

void MainComponent::startRecording()
{
    stopPreview();
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
    if (! engine.isDeviceOpen())
        return;

    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 0, 1, 256,
                                                             false, false, true, false);
    selector->setSize (520, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = Theme::surface;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showKeyShortcuts()
{
    // Deleted later, not inside its own close button's callback.
    if (keyShortcutsWindow == nullptr)
    {
        keyShortcutsWindow = std::make_unique<KeyShortcutsWindow> (commands, [this]
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->keyShortcutsWindow = nullptr;
            });
        });
        places->watch (*keyShortcutsWindow, "Keyboard Shortcuts");
    }
    else
    {
        keyShortcutsWindow->toFront (true);
    }
}

void MainComponent::showPluginList()
{
    if (pluginListWindow == nullptr)
    {
        pluginListWindow = std::make_unique<PluginListWindow> (engine, deadMansPedalFile, appProperties.getUserSettings(),
                                                              [this] { pluginListWindow = nullptr; });
        places->watch (*pluginListWindow, "Plug-ins");
    }
    else
    {
        pluginListWindow->toFront (true);
    }
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
            {
                // A 5.1 track's copies of the effect take what was changed in its window.
                engine.refreshEffectCopies (safe->processor);
                editorWindows.removeObject (safe.getComponent());
            }
        });
    }));

    const auto* instance = dynamic_cast<juce::AudioPluginInstance*> (plugin);
    places->watch (*editorWindows.getLast(), "Plug-in " + (instance != nullptr ? instance->getPluginDescription().createIdentifierString()
                                                                                : plugin->getName()), true);
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

//==============================================================================
juce::Component* MainComponent::getPanel (const juce::String& id)
{
    if (id == Panels::editor)       return editorPanel.get();
    if (id == Panels::rack)         return &rackView;
    if (id == Panels::selection)    return &selectionPanel;

    if (! Panels::all().contains (id))
        return nullptr;

    auto& made = madePanels[id];

    if (made != nullptr)
        return made.get();

    const auto selectedClip = [this] { return multitrack.getSelectedClip(); };

    if (id == Panels::files)
    {
        auto panel = std::make_unique<FilesPanel> (openFiles);
        panel->onChoose = [this] (int index) { switchToFile (index); };
        panel->onClose = [this] (int index) { closeFile (index); };
        panel->onOpen = [this] { chooseFile(); };
        made = std::move (panel);
    }
    else if (id == Panels::mixer)
    {
        auto mixer = std::make_unique<MixerView> (engine);
        mixer->setAutomationWriter (&multitrack);
        made = std::move (mixer);
    }
    else if (id == Panels::clipProperties)
        made = std::make_unique<ClipProperties> (engine, selectedClip);
    else if (id == Panels::clipList)
        made = std::make_unique<ClipList> (engine, selectedClip, [this] (juce::uint32 clip) { multitrack.showClip (clip); });
    else if (id == Panels::markers)
        made = std::make_unique<MarkerTable> (engine.session, MarkerTable::Actions { [this] (int i) { multitrack.showMarker (i); },
                                                                                    [this] (int i) { multitrack.renameMarker (i); },
                                                                                    [this] (int i) { multitrack.deleteMarker (i); } });
    else if (id == Panels::history)
        made = std::make_unique<UndoHistory> (engine.session, engine.document, [this] { return multitrackMode; });
    else if (id == Panels::levels)
    {
        auto panel = std::make_unique<LevelsPanel>();
        panel->readPeaks = [this] { return engine.takeMasterPeaks (AudioEngine::MasterReader::levels); };
        made = std::move (panel);
    }
    else if (id == Panels::spectrum)
    {
        auto panel = std::make_unique<Analysis::SpectrumPanel>();
        panel->readOutput = [this] (juce::AudioBuffer<float>& b) { engine.copyRecentOutput (b); return engine.getOutputSampleRate(); };
        made = std::move (panel);
    }
    else if (id == Panels::phase)
    {
        auto panel = std::make_unique<Analysis::PhasePanel>();
        panel->readOutput = [this] (juce::AudioBuffer<float>& b) { engine.copyRecentOutput (b); return engine.getOutputSampleRate(); };
        made = std::move (panel);
    }
    else if (id == Panels::tonalBalance)
    {
        auto panel = std::make_unique<TonalBalance::Panel>();
        panel->readOutput = [this] (juce::AudioBuffer<float>& b) { engine.copyRecentOutput (b); return engine.getOutputSampleRate(); };
        made = std::move (panel);
    }
    else if (id == Panels::notes)
        made = std::make_unique<SessionNotes> (engine.session);
    else if (id == Panels::statistics)
    {
        auto panel = std::make_unique<Statistics::Panel>();
        panel->readSource = [this]() -> std::optional<Statistics::Panel::Source>
        {
            auto& doc = engine.document;

            // In a session: the selected clip, under the time selection or all it plays.
            if (multitrackMode)
            {
                const auto state = engine.session.getState();
                const auto where = state->findClip (multitrack.getSelectedClip());

                if (! where.has_value())
                    return std::nullopt;

                const auto& clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];

                if (clip.audio == nullptr)
                    return std::nullopt;

                const auto range = SessionEdits::pitchRange (clip, multitrack.getTimeSelection());
                return Statistics::Panel::Source { clip.audio, range.getStart(), range.getEnd(), state->sampleRate };
            }

            if (! doc.hasAudio())
                return std::nullopt;

            const auto range = selectionOrAll();
            return Statistics::Panel::Source { doc.getSnapshot(), range.getStart(), range.getEnd(), doc.getSampleRate() };
        };
        panel->formatTime = [this] (juce::int64 frame)
        {
            return TransportBar::SelectionPanel::format (frame, multitrackMode ? engine.session.getSampleRate() : engine.document.getSampleRate());
        };
        made = std::move (panel);
    }
    else if (id == Panels::pitch)
    {
        auto panel = std::make_unique<PitchEditor>();
        panel->readSource = [this]() -> std::optional<PitchEditor::Source>
        {
            auto& doc = engine.document;

            // In a session: the selected clip, under the time selection or all it plays.
            if (multitrackMode)
            {
                const auto state = engine.session.getState();
                const auto clipId = multitrack.getSelectedClip();
                const auto where = state->findClip (clipId);

                if (! where.has_value())
                    return std::nullopt;

                const auto& clip = state->tracks[(size_t) where->first].clips[(size_t) where->second];

                if (clip.audio == nullptr || clip.locked)
                    return std::nullopt;

                pitchClipId = clipId;
                const auto range = SessionEdits::pitchRange (clip, multitrack.getTimeSelection());
                return PitchEditor::Source { clip.audio, range.getStart(), range.getEnd(), state->sampleRate, clipPitchVersion (clipId) };
            }

            if (! doc.hasAudio())
                return std::nullopt;

            const auto range = selectionOrAll();
            return PitchEditor::Source { doc.getSnapshot(), range.getStart(), range.getEnd(), doc.getSampleRate(), doc.getVersion() };
        };
        panel->readVersion = [this] { return multitrackMode ? clipPitchVersion (pitchClipId) : engine.document.getVersion(); };
        panel->apply = [this] (const PitchEditor::Source& source, const PitchCorrection::Track& track, const std::vector<PitchCorrection::Note>& notes)
        {
            if (multitrackMode)
                moveClipNotesInBackground (source, track, notes);
            else
                moveNotesInBackground (source, track, notes);
        };
        made = std::move (panel);
    }
    else if (id == Panels::video)
    {
        auto panel = std::make_unique<Video::Panel>();
        panel->readVideo = [this] { return multitrackMode ? engine.session.getState()->video : engine.document.getVideo(); };
        panel->readPlayhead = [this]() -> std::optional<Video::Panel::Playhead>
        {
            if (multitrackMode)
            {
                const auto state = engine.session.getState();
                const auto rate = engine.session.getSampleRate();

                if (rate <= 0.0)
                    return std::nullopt;

                auto& player = engine.getPlayer();
                const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::session;
                const auto at = Video::sessionPicture ((double) (playing ? player.getHeardFrame() : multitrack.getCursor()) / rate, state->videoStart);
                return Video::Panel::Playhead { at.picture, playing && ! at.blank, at.blank };
            }

            const auto rate = engine.document.getSampleRate();

            if (rate <= 0.0)
                return std::nullopt;

            auto& player = engine.getPlayer();
            const auto playing = player.isPlaying() && player.getMode() == DocumentPlayerProcessor::Mode::document;
            const auto sound = (double) (playing ? player.getHeardFrame() : waveform.getCursor()) / rate;
            const auto picture = engine.document.getPictureMap().pictureAt (sound);
            return Video::Panel::Playhead { picture.value_or (0.0), playing, ! picture.has_value() };
        };
        made = std::move (panel);
    }

    return made.get();
}

void MainComponent::panelHidden (const juce::String& id)
{
    madePanels.erase (id);
}

juce::String MainComponent::getTitle (const juce::String& id)
{
    if (id == Panels::editor)          return "Editor";
    if (id == Panels::rack)            return "Effects Rack";
    if (id == Panels::selection)       return "Selection/View";
    if (id == Panels::mixer)           return "Mixer";
    if (id == Panels::clipProperties)  return "Clip Properties";
    if (id == Panels::clipList)        return "Clip List";
    if (id == Panels::markers)         return "Markers";
    if (id == Panels::history)         return "Undo History";
    if (id == Panels::notes)           return "Session Notes";
    if (id == Panels::files)           return "Files";
    if (id == Panels::levels)          return "Levels";
    if (id == Panels::spectrum)        return "Frequency Analysis";
    if (id == Panels::phase)           return "Phase Meter";
    if (id == Panels::tonalBalance)    return "Tonal Balance";
    if (id == Panels::statistics)      return "Amplitude Statistics";
    if (id == Panels::video)           return "Video";
    if (id == Panels::pitch)           return "Pitch Editor";
    return id;
}

bool MainComponent::isMovable (const juce::String& id)
{
    return id != Panels::editor;
}

void MainComponent::layoutChanged()
{
    workspaces->update (dock->getLayout());

    if (auto xml = workspaces->toXml())
        appProperties.getUserSettings()->setValue (workspacesKey, xml.get());

    commands.commandStatusChanged();
}

double MainComponent::defaultShare (const juce::String& id)
{
    return id == Panels::levels ? 0.12 : id == Panels::files ? 0.2 : id == Panels::video ? 0.45 : 0.3;
}

std::pair<juce::String, Dock::Side> MainComponent::defaultPlace (const juce::String& id)
{
    if (id == Panels::mixer)       return { Panels::editor, Dock::Side::bottom };
    if (id == Panels::rack)        return { Panels::editor, Dock::Side::right };
    if (id == Panels::selection)   return { Panels::rack, Dock::Side::bottom };
    if (id == Panels::files)       return { Panels::editor, Dock::Side::left };
    if (id == Panels::levels)      return { {}, Dock::Side::bottom };
    if (id == Panels::video)       return { Panels::rack, Dock::Side::top };
    if (id == Panels::spectrum)    return { Panels::editor, Dock::Side::bottom };
    if (id == Panels::statistics)  return { Panels::editor, Dock::Side::bottom };
    if (id == Panels::phase)       return { dock->getLayout().isDocked (Panels::spectrum) ? Panels::spectrum : Panels::editor,
                                            dock->getLayout().isDocked (Panels::spectrum) ? Dock::Side::right : Dock::Side::bottom };
    if (id == Panels::tonalBalance) return { dock->getLayout().isDocked (Panels::spectrum) ? Panels::spectrum : Panels::editor,
                                             dock->getLayout().isDocked (Panels::spectrum) ? Dock::Side::right : Dock::Side::bottom };

    // Lists and the rest join the rack's tabs, or sit at the right without it.
    return { Panels::rack, Dock::Side::centre };
}

void MainComponent::windowCreated (juce::DocumentWindow& window)
{
    window.addKeyListener (commands.getKeyMappings());   // the keys work with a floating panel in front
}

void MainComponent::setTimeFormat (TimeFormat::Kind kind)
{
    TimeFormat::set (kind);
    appProperties.getUserSettings()->setValue (timeFormatKey, TimeFormat::key (kind));

    // Everything that shows a time draws it from the format when it paints: every window again.
    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* c = desktop.getComponent (i))
            c->repaint();

    commands.commandStatusChanged();
}

void MainComponent::selectWorkspace (const juce::String& name)
{
    if (! workspaces->select (name))
        return;

    dock->setLayout (workspaces->current().now);
    layoutChanged();
    menuItemsChanged();
}

void MainComponent::resetWorkspace()
{
    workspaces->reset();
    dock->setLayout (workspaces->current().now);
    layoutChanged();
}

void MainComponent::askToSaveWorkspace()
{
    auto* box = new juce::AlertWindow ("Save Workspace", "Keep the panels as they are now under a name of your own. "
                                                         "One of yours with the same name is replaced.",
                                       juce::MessageBoxIconType::NoIcon, this);
    box->addTextEditor ("name", workspaces->current().builtIn ? juce::String() : workspaces->current().name, "Name");
    box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this), box] (int result)
    {
        if (safe == nullptr || result != 1)
            return;

        const auto name = box->getTextEditorContents ("name").trim();

        if (! safe->workspaces->saveAs (name, safe->dock->getLayout()))
        {
            showError ("Save Workspace", name.isEmpty() ? "The workspace needs a name."
                                                              : "\"" + name + "\" is a built-in workspace; choose another name.");
            return;
        }

        safe->layoutChanged();
        safe->menuItemsChanged();
    }), true);
}

void MainComponent::askToDeleteWorkspace()
{
    const auto name = workspaces->current().name;

    if (workspaces->current().builtIn)
        return;

    auto* box = new juce::AlertWindow ("Delete Workspace", "Delete the workspace \"" + name + "\"?", juce::MessageBoxIconType::QuestionIcon, this);
    box->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this), name] (int result)
    {
        if (safe == nullptr || result != 1 || ! safe->workspaces->remove (name))
            return;

        safe->dock->setLayout (safe->workspaces->current().now);
        safe->layoutChanged();
        safe->menuItemsChanged();
    }), true);
}
