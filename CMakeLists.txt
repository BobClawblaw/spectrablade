cmake_minimum_required(VERSION 3.22)

project(Waveloom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

# Use a local JUCE checkout with -DWAVELOOM_JUCE_DIR=/path/to/JUCE, otherwise fetch the pinned release.
set(WAVELOOM_JUCE_DIR "" CACHE PATH "Local JUCE checkout (optional)")

if(WAVELOOM_JUCE_DIR)
    add_subdirectory("${WAVELOOM_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(Waveloom
    PRODUCT_NAME                 "Waveloom"
    COMPANY_NAME                 "Waveloom"
    BUNDLE_ID                    org.waveloom.editor
    PLUGINHOST_AU                TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT   "Waveloom records from your audio interface."
    HARDENED_RUNTIME_ENABLED     TRUE
    # Third-party plug-ins are signed by other teams, so library validation must be off to load them.
    HARDENED_RUNTIME_OPTIONS     com.apple.security.cs.disable-library-validation
                                 com.apple.security.device.audio-input
    NEEDS_WEB_BROWSER            FALSE
    NEEDS_CURL                   FALSE)

juce_generate_juce_header(Waveloom)

target_sources(Waveloom PRIVATE
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

target_compile_definitions(Waveloom PRIVATE
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
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Waveloom,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Waveloom,JUCE_VERSION>")

target_link_libraries(Waveloom PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Integration checks: plug-in scanning, device routing and rack restore on this machine.
juce_add_console_app(WaveloomTests PRODUCT_NAME "WaveloomTests" PLUGINHOST_AU TRUE)
juce_generate_juce_header(WaveloomTests)
target_sources(WaveloomTests PRIVATE
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
get_target_property(waveloom_defs Waveloom COMPILE_DEFINITIONS)
target_compile_definitions(WaveloomTests PRIVATE ${waveloom_defs} JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(WaveloomTests Waveloom)
target_compile_definitions(WaveloomTests PRIVATE WAVELOOM_APP_EXECUTABLE="$<TARGET_FILE:Waveloom>")
target_link_libraries(WaveloomTests PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
