#pragma once

#include <JuceHeader.h>

/** Command-line token that makes the app start as a plug-in scanning worker instead of the editor. */
inline constexpr const char* scannerProcessUID = "spectrablade-plugin-scanner";

//==============================================================================
/** Runs inside the worker process: receives (format, file) requests, loads the plug-in, checks
    that each type in it works (created at 48 kHz, 16 blocks of noise or a note played through it,
    every output sample finite, its state read and restored) and sends back the descriptions of
    the ones that do as XML, with the reasons for the ones that don't.

    A plug-in that crashes takes down only this process, which exits at once instead of leaving
    a crash report for macOS to show. */
class ScannerWorker final : private juce::ChildProcessWorker,
                            private juce::AsyncUpdater
{
public:
    ScannerWorker();

    /** Starts the worker if the command line asks for one; hides it from the Dock. */
    bool initialiseFromCommandLine (const juce::String& commandLine, const juce::String& uid);

    /** Empty when the plug-in works, otherwise why it doesn't. */
    static juce::String check (juce::AudioPluginFormat&, const juce::PluginDescription&);
    static juce::String check (juce::AudioPluginInstance&);

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock&) override;
    void handleConnectionMade() override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;

    /** Finds the types in a plug-in file and checks each. */
    juce::XmlElement scan (juce::AudioPluginFormat&, const juce::String& identifier);
    void scanAudioUnit (juce::AudioPluginFormat&, const juce::String& identifier);
    void reply (const juce::XmlElement&);
    static juce::String missingCodeReason (const juce::String& formatName, const juce::String& identifier);

    bool waitingForAudioUnit = false;   // guarded by mutex
    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;
    juce::AudioPluginFormatManager formatManager;
};

//==============================================================================
/** Plugs into KnownPluginList so every scan request goes to a worker process (one per scanning
    thread, so several plug-ins can be checked at once).
    A crash, a hang (past the timeout) or a failed check makes the scan report failure, and the
    list then marks that file as not working instead of the editor going down with it. Each
    failure is written to the log with its reason; nothing is asked. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    /** By default the worker is this app itself, started with the scanner token. A plug-in that
        takes longer than timeoutMs to load and play counts as hung. */
    explicit OutOfProcessScanner (const juce::File& workerExecutable = {}, int timeoutMs = perPluginTimeoutMs);
    ~OutOfProcessScanner() override;

    bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&,
                             const juce::String& fileOrIdentifier) override;
    void scanFinished() override;

    /** Files checked so far that work, and that don't. */
    int getNumWorking() const noexcept             { return working.load(); }
    int getNumNotWorking() const noexcept          { return notWorking.load(); }

    /** How long one plug-in may take to load before it counts as hung, by default. */
    static constexpr int perPluginTimeoutMs = 60000;

private:
    const juce::File workerExecutable;
    const int timeoutMs;
    class Coordinator;
    std::mutex coordinatorsLock;
    std::map<std::thread::id, std::unique_ptr<Coordinator>> coordinators;
    std::atomic<int> working { 0 }, notWorking { 0 };
};

//==============================================================================
/** Scans every plug-in format's default locations (the system's Audio Unit registry, the VST3
    folders) through a fresh out-of-process scanner that it gives the list, checking several
    plug-ins at once, each in its own worker. Plug-ins that fail are marked not working and
    logged, and the scan carries on. Deleting it stops the scan after the plug-ins being checked. */
class PluginScan final
{
public:
    /** With retryNotWorking, plug-ins marked not working are tried again. The worker is this
        app unless another executable is given. */
    PluginScan (juce::AudioPluginFormatManager&, juce::KnownPluginList&, bool retryNotWorking,
                const juce::File& workerExecutable = {});
    ~PluginScan();

    static constexpr int numThreads = 4;

    bool isFinished() const noexcept               { return jobsLeft.load() == 0; }
    double getProgress() const;
    int getNumWorking() const noexcept             { return scanner->getNumWorking(); }
    int getNumNotWorking() const noexcept          { return scanner->getNumNotWorking(); }
    juce::String getCurrentName() const;

private:
    class Job;

    juce::KnownPluginList& list;
    OutOfProcessScanner* scanner = nullptr;   // owned by the list
    std::vector<juce::AudioPluginFormat*> scannedFormats;
    std::vector<std::unique_ptr<juce::PluginDirectoryScanner>> formatScanners;   // one per format, in the same order
    std::atomic<int> jobsLeft { numThreads };
    mutable std::mutex nameLock;
    juce::String currentName;

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (numThreads) };
};
