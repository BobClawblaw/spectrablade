cmake_minimum_required(VERSION 3.22)

project(WaveDrill VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

# Use a local JUCE checkout with -DWAVEDRILL_JUCE_DIR=/path/to/JUCE, otherwise fetch the pinned release.
set(WAVEDRILL_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(WAVEDRILL_JUCE_DIR)
    add_subdirectory("${WAVEDRILL_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(WaveDrill
    PRODUCT_NAME                 "WaveDrill"
    COMPANY_NAME                 "WaveDrill"
    BUNDLE_ID                    org.wavedrill.editor
    PLUGINHOST_AU                TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT   "WaveDrill records from your audio interface."
    HARDENED_RUNTIME_ENABLED     TRUE
    # Third-party plug-ins are signed by other teams, so library validation must be off to load them.
    HARDENED_RUNTIME_OPTIONS     com.apple.security.cs.disable-library-validation
                                 com.apple.security.device.audio-input
    NEEDS_WEB_BROWSER            FALSE
    NEEDS_CURL                   FALSE)

juce_generate_juce_header(WaveDrill)

target_sources(WaveDrill PRIVATE
    Source/Main.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/AudioEdits.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/Recorder.cpp
    Source/PluginScanner.cpp
    Source/MainComponent.cpp
    Source/WaveformView.cpp
    Source/RackView.cpp)

target_compile_definitions(WaveDrill PRIVATE
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
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WaveDrill,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WaveDrill,JUCE_VERSION>")

target_link_libraries(WaveDrill PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Integration checks: plug-in scanning, device routing and rack restore on this machine.
juce_add_console_app(WaveDrillTests PRODUCT_NAME "WaveDrillTests" PLUGINHOST_AU TRUE)
juce_generate_juce_header(WaveDrillTests)
target_sources(WaveDrillTests PRIVATE
    Tests/EngineTests.cpp
    Tests/DocumentTests.cpp
    Tests/RecorderTests.cpp
    Tests/MarkerTests.cpp
    Tests/ConverterTests.cpp
    Source/AudioDocument.cpp
    Source/Markers.cpp
    Source/AudioEdits.cpp
    Source/AudioFileIO.cpp
    Source/OfflineRender.cpp
    Source/AudioEngine.cpp
    Source/SampleRateConverter.cpp
    Source/Recorder.cpp
    Source/PluginScanner.cpp)
get_target_property(wavedrill_defs WaveDrill COMPILE_DEFINITIONS)
target_compile_definitions(WaveDrillTests PRIVATE ${wavedrill_defs} JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(WaveDrillTests WaveDrill)
target_compile_definitions(WaveDrillTests PRIVATE WAVEDRILL_APP_EXECUTABLE="$<TARGET_FILE:WaveDrill>")
target_link_libraries(WaveDrillTests PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
