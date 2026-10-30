#pragma once

#include <JuceHeader.h>

/** Command-line token that makes the app start as a plug-in scanning worker instead of the editor. */
inline constexpr const char* scannerProcessUID = "waveloom-plugin-scanner";

//==============================================================================
/** Runs inside the worker process: receives (format, file) requests, loads the plug-in
    and sends back its descriptions as XML. A plug-in that crashes takes down only this process. */
class ScannerWorker final : private juce::ChildProcessWorker,
                            private juce::AsyncUpdater
{
public:
    ScannerWorker();

    using ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock&) override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;

    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;
    juce::AudioPluginFormatManager formatManager;
};

//==============================================================================
/** Plugs into KnownPluginList so every scan request goes to a worker process.
    A crash or a hang (past the timeout) makes the scan report failure, and the
    list then marks that file as failed instead of the editor going down with it. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    /** By default the worker is this app itself, started with the scanner token. */
    explicit OutOfProcessScanner (const juce::File& workerExecutable = {});
    ~OutOfProcessScanner() override;

    bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&,
                             const juce::String& fileOrIdentifier) override;
    void scanFinished() override;

    /** How long one plug-in may take to load before it counts as hung. */
    static constexpr int perPluginTimeoutMs = 60000;

private:
    const juce::File workerExecutable;
    class Coordinator;
    std::unique_ptr<Coordinator> coordinator;
};
