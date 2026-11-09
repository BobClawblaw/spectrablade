cmake_minimum_required(VERSION 3.22)

project(SpectraBlade VERSION 0.14.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

# Use a local JUCE checkout with -DSPECTRABLADE_JUCE_DIR=/path/to/JUCE, otherwise fetch the pinned release.
set(SPECTRABLADE_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(SPECTRABLADE_JUCE_DIR)
    add_subdirectory("${SPECTRABLADE_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# LAME's MP3 encoder, built from its pinned source release. Use a local copy of that release with
# -DSPECTRABLADE_LAME_DIR=/path/to/lame-4.0.
set(SPECTRABLADE_LAME_DIR "" CACHE PATH "Local LAME 4.0 source (optional)")

if(SPECTRABLADE_LAME_DIR)
    set(spectrablade_lame_src "${SPECTRABLADE_LAME_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(lame
        URL      https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz
        URL_HASH SHA256=3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(lame)
    set(spectrablade_lame_src "${lame_SOURCE_DIR}")
endif()

set(spectrablade_lame_files VbrTag bitstream encoder fft gain_analysis id3tag lame newmdct presets psymodel
    quantize quantize_pvt reservoir set_get tables takehiro util vbrquantize version)
list(TRANSFORM spectrablade_lame_files PREPEND "${spectrablade_lame_src}/libmp3lame/")
list(TRANSFORM spectrablade_lame_files APPEND ".c")
add_library(SpectraBladeLame STATIC ${spectrablade_lame_files})
target_include_directories(SpectraBladeLame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/lame" "${spectrablade_lame_src}/libmp3lame")
target_include_directories(SpectraBladeLame SYSTEM PUBLIC "${spectrablade_lame_src}/include")
target_compile_definitions(SpectraBladeLame PRIVATE HAVE_CONFIG_H=1)
# LAME's own code, compiled as it is: its warnings are not ours to fix.
target_compile_options(SpectraBladeLame PRIVATE -w -Wno-implicit-function-declaration)
set_target_properties(SpectraBladeLame PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# Signalsmith Stretch (time stretching and pitch shifting) and the Signalsmith Linear FFTs it uses,
# both MIT-licensed headers, each fetched at its pinned release commit (Stretch 1.4.0, Linear 0.6.4).
# Use local copies of those releases with -DSPECTRABLADE_STRETCH_DIR and -DSPECTRABLADE_LINEAR_DIR.
set(SPECTRABLADE_STRETCH_DIR "" CACHE PATH "Local Signalsmith Stretch 1.4.0 (optional)")
set(SPECTRABLADE_LINEAR_DIR "" CACHE PATH "Local Signalsmith Linear 0.6.4 (optional)")

if(SPECTRABLADE_STRETCH_DIR)
    set(spectrablade_stretch_src "${SPECTRABLADE_STRETCH_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(signalsmith_stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG        a670068d9aeb64913331d5cc29337b19a457a7df
        SOURCE_SUBDIR  not-built)
    FetchContent_MakeAvailable(signalsmith_stretch)
    set(spectrablade_stretch_src "${signalsmith_stretch_SOURCE_DIR}")
endif()

if(SPECTRABLADE_LINEAR_DIR)
    set(spectrablade_linear_src "${SPECTRABLADE_LINEAR_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(signalsmith_linear
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
        GIT_TAG        de55e6a50ffcf6f8f43f649692d94691c7025151
        SOURCE_SUBDIR  not-built)
    FetchContent_MakeAvailable(signalsmith_linear)
    set(spectrablade_linear_src "${signalsmith_linear_SOURCE_DIR}")
endif()

# The headers include "signalsmith-linear/stft.h": a folder of that name pointing at Linear.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/signalsmith-include")
file(CREATE_LINK "${spectrablade_linear_src}" "${CMAKE_BINARY_DIR}/signalsmith-include/signalsmith-linear" SYMBOLIC)
add_library(SpectraBladeStretch INTERFACE)
target_include_directories(SpectraBladeStretch SYSTEM INTERFACE "${spectrablade_stretch_src}" "${CMAKE_BINARY_DIR}/signalsmith-include")

juce_add_gui_app(SpectraBlade
    PRODUCT_NAME                 "SpectraBlade"
    COMPANY_NAME                 "SpectraBlade"
    COMPANY_COPYRIGHT            "Copyright (C) 2026 BobClawblaw. Licensed under the GNU AGPLv3."
    BUNDLE_ID                    org.spectrablade.editor
    ICON_BIG                     "${CMAKE_CURRENT_SOURCE_DIR}/Art/spectrablade-icon.png"
    PLUGINHOST_AU                TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT   "SpectraBlade records from your audio interface."
    HARDENED_RUNTIME_ENABLED     TRUE
    # Third-party plug-ins are signed by other teams, so library validation must be off to load them.
    HARDENED_RUNTIME_OPTIONS     com.apple.security.cs.disable-library-validation
                                 com.apple.security.device.audio-input
    # Finder's Open With and double-clicks: the audio types AudioFileIO::openedFromFinder names, and sessions.
    DOCUMENT_EXTENSIONS          wav aif aiff aifc flac ogg mp3 m4a caf sbsession sbproject
    NEEDS_WEB_BROWSER            FALSE
    NEEDS_CURL                   FALSE)

juce_generate_juce_header(SpectraBlade)

target_sources(SpectraBlade PRIVATE
    Source/Main.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/Metronome.cpp
    Source/AudioEdits.cpp
    Source/Batch.cpp
    Source/Sequences.cpp
    Source/EffectPreview.cpp
    Source/EffectDialog.cpp
    Source/Surround.cpp
    Source/Video.cpp
    Source/Project.cpp
    Source/PictureMap.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/TimeStretch.cpp
    Source/ExtremeStretch.cpp
    Source/PhaseVocoder.cpp
    Source/TrackTemplate.cpp
    Source/UndoHistory.cpp
    Source/Spectrogram.cpp
    Source/SpectralEdit.cpp
    Source/Loudness.cpp
    Source/Delivery.cpp
    Source/SoundLibrary.cpp
    Source/SoundLibraryView.cpp
    Source/SoundLibraryPreview.cpp
    Source/SoundCategories.cpp
    Source/SoundMetadata.cpp
    Source/Equalizer.cpp
    Source/GraphicEq.cpp
    Source/Dynamics.cpp
    Source/Multiband.cpp
    Source/Modulation.cpp
    Source/Reverb.cpp
    Source/Convolver.cpp
    Source/BuiltInEffects.cpp
    Source/EffectPresets.cpp
    Source/FactoryPresets.cpp
    Source/ParametricEq.cpp
    Source/EqDrawing.cpp
    Source/DynamicEq.cpp
    Source/Oversampling.cpp
    Source/Exciter.cpp
    Source/Maximizer.cpp
    Source/TransientShaper.cpp
    Source/MatchEq.cpp
    Source/TonalBalance.cpp
    Source/Statistics.cpp
    Source/TimeFormat.cpp
    Source/Dither.cpp
    Source/DialogueIsolation.cpp
    Source/AdaptiveDenoise.cpp
    Source/Plosive.cpp
    Source/Breath.cpp
    Source/Spill.cpp
    Source/WowFlutter.cpp
    Source/SpectralRecovery.cpp
    Source/Rebalance.cpp
    Source/Ambience.cpp
    Source/DialogueContour.cpp
    Source/Azimuth.cpp
    Source/CodecPreview.cpp
    Source/NoiseReduction.cpp
    Source/Dereverb.cpp
    Source/CentreChannel.cpp
    Source/PhaseCorrection.cpp
    Source/PitchCorrection.cpp
    Source/Leveller.cpp
    Source/Scrubber.cpp
    Source/ClickRepair.cpp
    Source/Session.cpp
    Source/SessionMixer.cpp
    Source/TrackEffects.cpp
    Source/RackCopy.cpp
    Source/Recorder.cpp
    Source/SessionCollect.cpp
    Source/SessionNotes.cpp
    Source/LoadReadout.cpp
    Source/SessionRecovery.cpp
    Source/KeyShortcuts.cpp
    Source/AuditionPreview.cpp
    Source/WindowPlaces.cpp
    Source/Dock.cpp
    Source/EditorBar.cpp
    Source/Navigation.cpp
    Source/Restoration.cpp
    Source/Generator.cpp
    Source/LossyFiles.cpp
    Source/AacWriter.cpp
    Source/Ffmpeg.cpp
    Source/FileInfo.cpp
    Source/OpenFiles.cpp
    Source/LevelsPanel.cpp
    Source/Analysis.cpp
    Source/Theme.cpp
    Source/FileFades.cpp
    Source/FileChecks.cpp
    Source/LoadingScreen.cpp
    Source/TransportBar.cpp
    Source/SessionOverview.cpp
    Source/SessionRecording.cpp
    Source/Bounce.cpp
    Source/ClipList.cpp
    Source/MarkerTable.cpp
    Source/ClipProperties.cpp
    Source/PluginScanner.cpp
    Source/MainComponent.cpp
    Source/PitchEditor.cpp
    Source/WaveformView.cpp
    Source/MixerView.cpp
    Source/SurroundPad.cpp
    Source/MultitrackView.cpp
    Source/RackView.cpp)

target_compile_definitions(SpectraBlade PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_ARA=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SpectraBlade,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SpectraBlade,JUCE_VERSION>")

# The logo on the loading screen.
juce_add_binary_data(SpectraBladeArt SOURCES Art/spectrablade-banner.png ThirdParty/UCS/ucs_v8_2_1.tsv)

target_link_libraries(SpectraBlade PRIVATE
    SpectraBladeArt
    SpectraBladeLame
    SpectraBladeStretch
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_video
    sqlite3
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Integration checks: plug-in scanning, device routing and rack restore on this machine.
juce_add_console_app(SpectraBladeTests PRODUCT_NAME "SpectraBladeTests" PLUGINHOST_AU TRUE)
juce_generate_juce_header(SpectraBladeTests)
target_sources(SpectraBladeTests PRIVATE
    Tests/EngineTests.cpp
    Tests/DocumentTests.cpp
    Tests/RecorderTests.cpp
    Tests/MarkerTests.cpp
    Tests/ConverterTests.cpp
    Tests/SpectralTests.cpp
    Tests/CommandAuditTests.cpp
    Tests/PlayheadTests.cpp
    Tests/LoudnessTests.cpp
    Tests/SoundLibraryTests.cpp
    Tests/EqTests.cpp
    Tests/GraphicEqTests.cpp
    Tests/DynamicsTests.cpp
    Tests/MultibandTests.cpp
    Tests/DynamicEqTests.cpp
    Tests/ExciterTests.cpp
    Tests/MaximizerTests.cpp
    Tests/TransientShaperTests.cpp
    Tests/MatchEqTests.cpp
    Tests/TonalBalanceTests.cpp
    Tests/StatisticsTests.cpp
    Tests/ExtremeStretchTests.cpp
    Tests/TimeFormatTests.cpp
    Tests/DitherTests.cpp
    Tests/DialogueIsolationTests.cpp
    Tests/AdaptiveDenoiseTests.cpp
    Tests/PlosiveTests.cpp
    Tests/BreathTests.cpp
    Tests/SpillTests.cpp
    Tests/WowFlutterTests.cpp
    Tests/SpectralRecoveryTests.cpp
    Tests/RebalanceTests.cpp
    Tests/AmbienceTests.cpp
    Tests/DialogueContourTests.cpp
    Tests/AzimuthTests.cpp
    Tests/CodecPreviewTests.cpp
    Tests/ModulationTests.cpp
    Tests/ReverbTests.cpp
    Tests/ConvolverTests.cpp
    Tests/EffectPresetsTests.cpp
    Tests/BuiltInEffectsTests.cpp
    Tests/AuditionTests.cpp
    Tests/NoiseTests.cpp
    Tests/DereverbTests.cpp
    Tests/CentreChannelTests.cpp
    Tests/PhaseCorrectionTests.cpp
    Tests/PitchCorrectionTests.cpp
    Tests/LevellerTests.cpp
    Tests/ScrubberTests.cpp
    Tests/ClickTests.cpp
    Tests/SessionTests.cpp
    Tests/TrackEffectsTests.cpp
    Tests/BusTests.cpp
    Tests/SidechainTests.cpp
    Tests/BatchTests.cpp
    Tests/SequencesTests.cpp
    Tests/EffectPreviewTests.cpp
    Tests/SurroundTests.cpp
    Tests/SurroundSessionTests.cpp
    Tests/SurroundEffectsTests.cpp
    Tests/SessionScrubTests.cpp
    Tests/VideoTests.cpp
    Tests/ProjectTests.cpp
    Tests/AutomationTests.cpp
    Tests/MultitrackViewTests.cpp
    Tests/MixDownTests.cpp
    Tests/EffectAutomationTests.cpp
    Tests/TrackRecordingTests.cpp
    Tests/MetronomeTests.cpp
    Tests/MixerTests.cpp
    Tests/BounceTests.cpp
    Tests/PluginTests.cpp
    Tests/DockTests.cpp
    Tests/OpenFilesTests.cpp
    Tests/PanelTests.cpp
    Tests/ThemeTests.cpp
    Tests/EditorBarTests.cpp
    Tests/NavigationTests.cpp
    Tests/RestorationTests.cpp
    Tests/GeneratorTests.cpp
    Tests/LossyFilesTests.cpp
    Tests/FfmpegTests.cpp
    Tests/FileInfoTests.cpp
    Tests/SpectralShapeTests.cpp
    Tests/DuckingTests.cpp
    Tests/FadeHandleTests.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/Metronome.cpp
    Source/AudioEdits.cpp
    Source/Batch.cpp
    Source/Sequences.cpp
    Source/EffectPreview.cpp
    Source/EffectDialog.cpp
    Source/Surround.cpp
    Source/Video.cpp
    Source/Project.cpp
    Source/PictureMap.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/TimeStretch.cpp
    Source/ExtremeStretch.cpp
    Source/PhaseVocoder.cpp
    Source/TrackTemplate.cpp
    Source/UndoHistory.cpp
    Source/Spectrogram.cpp
    Source/SpectralEdit.cpp
    Source/Loudness.cpp
    Source/Delivery.cpp
    Source/SoundLibrary.cpp
    Source/SoundLibraryView.cpp
    Source/SoundLibraryPreview.cpp
    Source/SoundCategories.cpp
    Source/SoundMetadata.cpp
    Source/Equalizer.cpp
    Source/GraphicEq.cpp
    Source/Dynamics.cpp
    Source/Multiband.cpp
    Source/Modulation.cpp
    Source/Reverb.cpp
    Source/Convolver.cpp
    Source/BuiltInEffects.cpp
    Source/EffectPresets.cpp
    Source/FactoryPresets.cpp
    Source/ParametricEq.cpp
    Source/EqDrawing.cpp
    Source/DynamicEq.cpp
    Source/Oversampling.cpp
    Source/Exciter.cpp
    Source/Maximizer.cpp
    Source/TransientShaper.cpp
    Source/MatchEq.cpp
    Source/TonalBalance.cpp
    Source/Statistics.cpp
    Source/TimeFormat.cpp
    Source/Dither.cpp
    Source/DialogueIsolation.cpp
    Source/AdaptiveDenoise.cpp
    Source/Plosive.cpp
    Source/Breath.cpp
    Source/Spill.cpp
    Source/WowFlutter.cpp
    Source/SpectralRecovery.cpp
    Source/Rebalance.cpp
    Source/Ambience.cpp
    Source/DialogueContour.cpp
    Source/Azimuth.cpp
    Source/CodecPreview.cpp
    Source/NoiseReduction.cpp
    Source/Dereverb.cpp
    Source/CentreChannel.cpp
    Source/PhaseCorrection.cpp
    Source/PitchCorrection.cpp
    Source/PitchEditor.cpp
    Source/Leveller.cpp
    Source/Scrubber.cpp
    Source/ClickRepair.cpp
    Source/Session.cpp
    Source/SessionMixer.cpp
    Source/TrackEffects.cpp
    Source/RackCopy.cpp
    Source/Recorder.cpp
    Source/SessionCollect.cpp
    Source/SessionNotes.cpp
    Source/LoadReadout.cpp
    Source/SessionRecovery.cpp
    Source/KeyShortcuts.cpp
    Source/AuditionPreview.cpp
    Source/WindowPlaces.cpp
    Source/Dock.cpp
    Source/EditorBar.cpp
    Source/Navigation.cpp
    Source/Restoration.cpp
    Source/Generator.cpp
    Source/LossyFiles.cpp
    Source/AacWriter.cpp
    Source/Ffmpeg.cpp
    Source/FileInfo.cpp
    Source/OpenFiles.cpp
    Source/LevelsPanel.cpp
    Source/Analysis.cpp
    Source/Theme.cpp
    Source/FileFades.cpp
    Source/FileChecks.cpp
    Source/LoadingScreen.cpp
    Source/TransportBar.cpp
    Source/WaveformView.cpp
    Source/SessionOverview.cpp
    Source/SessionRecording.cpp
    Source/Bounce.cpp
    Source/ClipList.cpp
    Source/MarkerTable.cpp
    Source/ClipProperties.cpp
    Source/PluginScanner.cpp
    Source/MixerView.cpp
    Source/SurroundPad.cpp
    Source/RackView.cpp
    Source/MultitrackView.cpp)
# Plug-in bundles that misbehave on purpose, for the scan tests (see Tests/TestBundle.cpp).
foreach(bundle IntelOnlyBundle HangingBundle CrashingBundle)
    add_library(${bundle} MODULE Tests/TestBundle.cpp)
    set_target_properties(${bundle} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/TestBundles")
    add_dependencies(SpectraBladeTests ${bundle})
endforeach()
set_target_properties(IntelOnlyBundle PROPERTIES OSX_ARCHITECTURES x86_64)
target_compile_definitions(HangingBundle PRIVATE TEST_BUNDLE_HANGS=1)
target_compile_definitions(CrashingBundle PRIVATE TEST_BUNDLE_CRASHES=1)
target_compile_definitions(SpectraBladeTests PRIVATE SPECTRABLADE_TEST_BUNDLES="${CMAKE_BINARY_DIR}/TestBundles")

get_target_property(spectrablade_defs SpectraBlade COMPILE_DEFINITIONS)
target_compile_definitions(SpectraBladeTests PRIVATE ${spectrablade_defs} JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(SpectraBladeTests SpectraBlade)
target_compile_definitions(SpectraBladeTests PRIVATE SPECTRABLADE_APP_EXECUTABLE="$<TARGET_FILE:SpectraBlade>")
target_compile_definitions(SpectraBladeTests PRIVATE SPECTRABLADE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(SpectraBladeTests PRIVATE
    SpectraBladeArt
    SpectraBladeLame
    SpectraBladeStretch
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_video
    sqlite3
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
