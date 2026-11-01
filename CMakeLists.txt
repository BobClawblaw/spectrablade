cmake_minimum_required(VERSION 3.22)

project(SpectraBlade VERSION 0.1.0 LANGUAGES C CXX)

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

juce_add_gui_app(SpectraBlade
    PRODUCT_NAME                 "SpectraBlade"
    COMPANY_NAME                 "SpectraBlade"
    BUNDLE_ID                    org.spectrablade.editor
    ICON_BIG                     "${CMAKE_CURRENT_SOURCE_DIR}/Art/spectrablade-icon.png"
    PLUGINHOST_AU                TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT   "SpectraBlade records from your audio interface."
    HARDENED_RUNTIME_ENABLED     TRUE
    # Third-party plug-ins are signed by other teams, so library validation must be off to load them.
    HARDENED_RUNTIME_OPTIONS     com.apple.security.cs.disable-library-validation
                                 com.apple.security.device.audio-input
    NEEDS_WEB_BROWSER            FALSE
    NEEDS_CURL                   FALSE)

juce_generate_juce_header(SpectraBlade)

target_sources(SpectraBlade PRIVATE
    Source/Main.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/AudioEdits.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/Spectrogram.cpp
    Source/SpectralEdit.cpp
    Source/Loudness.cpp
    Source/NoiseReduction.cpp
    Source/ClickRepair.cpp
    Source/Session.cpp
    Source/SessionMixer.cpp
    Source/TrackEffects.cpp
    Source/Recorder.cpp
    Source/SessionRecording.cpp
    Source/PluginScanner.cpp
    Source/MainComponent.cpp
    Source/WaveformView.cpp
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

target_link_libraries(SpectraBlade PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
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
    Tests/LoudnessTests.cpp
    Tests/NoiseTests.cpp
    Tests/ClickTests.cpp
    Tests/SessionTests.cpp
    Tests/TrackEffectsTests.cpp
    Tests/BusTests.cpp
    Tests/AutomationTests.cpp
    Tests/MultitrackViewTests.cpp
    Tests/MixDownTests.cpp
    Tests/EffectAutomationTests.cpp
    Tests/TrackRecordingTests.cpp
    Tests/PluginTests.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/AudioEdits.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/Spectrogram.cpp
    Source/SpectralEdit.cpp
    Source/Loudness.cpp
    Source/NoiseReduction.cpp
    Source/ClickRepair.cpp
    Source/Session.cpp
    Source/SessionMixer.cpp
    Source/TrackEffects.cpp
    Source/Recorder.cpp
    Source/SessionRecording.cpp
    Source/PluginScanner.cpp
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
target_link_libraries(SpectraBladeTests PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
