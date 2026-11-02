// Before JUCE's headers, whose juce::Point would clash with the Mac's own Point.
#include <CoreFoundation/CoreFoundation.h>

#include "PluginScanner.h"

#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace
{
    constexpr double checkRate = 48000.0;
    constexpr int checkBlock = 512, checkBlocks = 16;

    /** A plug-in that crashes the worker ends it quietly: the coordinator sees the connection
        drop and marks the plug-in as not working. */
    void exitOnCrash (int)
    {
        _exit (70);
    }

    /** Releases a Core Foundation object when it goes. */
    template <typename Ref>
    struct Owned
    {
        explicit Owned (Ref r) : ref (r) {}
        ~Owned()                          { if (ref != nullptr) CFRelease (ref); }
        Owned (const Owned&) = delete;
        Owned& operator= (const Owned&) = delete;
        Ref ref;
    };

    bool isAppleSilicon()
    {
       #if JUCE_ARM
        return true;
       #else
        return false;
       #endif
    }

    /** True when the bundle's executable has code for the architecture this process runs as. */
    bool hasCodeForThisMac (const juce::File& bundleFile)
    {
        const auto path = bundleFile.getFullPathName();
        const Owned<CFURLRef> url (CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(),
                                                                           (CFIndex) path.getNumBytesAsUTF8(), true));
        const Owned<CFBundleRef> bundle (url.ref != nullptr ? CFBundleCreate (nullptr, url.ref) : nullptr);

        if (bundle.ref == nullptr)
            return true;

        const Owned<CFArrayRef> archs (CFBundleCopyExecutableArchitectures (bundle.ref));

        if (archs.ref == nullptr)
            return true;

        const SInt32 wanted = isAppleSilicon() ? kCFBundleExecutableArchitectureARM64 : kCFBundleExecutableArchitectureX86_64;

        for (CFIndex i = 0; i < CFArrayGetCount (archs.ref); ++i)
        {
            SInt32 arch = 0;

            if (CFNumberGetValue ((CFNumberRef) CFArrayGetValueAtIndex (archs.ref, i), kCFNumberSInt32Type, &arch) && arch == wanted)
                return true;
        }

        return false;
    }

    /** A Core Foundation string as a JUCE one (empty when it isn't a string). */
    juce::String toString (CFTypeRef value)
    {
        if (value == nullptr || CFGetTypeID (value) != CFStringGetTypeID())
            return {};

        char text[256] = {};
        CFStringGetCString ((CFStringRef) value, text, (CFIndex) sizeof (text), kCFStringEncodingUTF8);
        return juce::String::fromUTF8 (text);
    }

    /** The bundle a plug-in comes from: the file itself, or for an Audio Unit
        ("AudioUnit:Effects/aufx,dely,appl") the installed component that registers it. */
    juce::File bundleFor (const juce::String& formatName, const juce::String& identifier)
    {
        if (formatName != "AudioUnit")
            return juce::File::isAbsolutePath (identifier) ? juce::File (identifier) : juce::File();

        const auto codes = juce::StringArray::fromTokens (identifier.fromLastOccurrenceOf ("/", false, false), ",", {});

        if (codes.size() != 3)
            return {};

        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        for (auto& dir : { juce::File ("/Library/Audio/Plug-Ins/Components"), home.getChildFile ("Library/Audio/Plug-Ins/Components") })
        {
            for (auto& component : dir.findChildFiles (juce::File::findDirectories, false, "*.component"))
            {
                const auto path = component.getFullPathName();
                const Owned<CFURLRef> url (CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(),
                                                                                   (CFIndex) path.getNumBytesAsUTF8(), true));
                const Owned<CFBundleRef> bundle (url.ref != nullptr ? CFBundleCreate (nullptr, url.ref) : nullptr);

                if (bundle.ref == nullptr)
                    continue;

                // Info.plist: AudioComponents, an array of { type, subtype, manufacturer, ... }.
                const auto components = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle.ref, CFSTR ("AudioComponents"));

                if (components == nullptr || CFGetTypeID (components) != CFArrayGetTypeID())
                    continue;

                for (CFIndex i = 0; i < CFArrayGetCount (components); ++i)
                {
                    const auto entry = (CFDictionaryRef) CFArrayGetValueAtIndex (components, i);

                    if (CFGetTypeID (entry) != CFDictionaryGetTypeID())
                        continue;

                    // Codes are four characters, padded with spaces ("UVI ").
                    const auto matches = [&] (CFStringRef key, const juce::String& code)
                    {
                        return toString (CFDictionaryGetValue (entry, key)).paddedRight (' ', 4) == code.paddedRight (' ', 4);
                    };

                    if (matches (CFSTR ("type"), codes[0]) && matches (CFSTR ("subtype"), codes[1]) && matches (CFSTR ("manufacturer"), codes[2]))
                        return component;
                }
            }
        }

        return {};
    }
}

//==============================================================================
ScannerWorker::ScannerWorker()
{
    juce::addDefaultFormatsToManager (formatManager);
}

bool ScannerWorker::initialiseFromCommandLine (const juce::String& commandLine, const juce::String& uid)
{
    if (! ChildProcessWorker::initialiseFromCommandLine (commandLine, uid))
        return false;

    juce::Process::setDockIconVisible (false);

    for (auto sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP })
        std::signal (sig, exitOnCrash);

    return true;
}

juce::String ScannerWorker::check (juce::AudioPluginFormat& format, const juce::PluginDescription& description)
{
    juce::String error;
    auto instance = format.createInstanceFromDescription (description, checkRate, checkBlock, error);

    if (instance == nullptr)
        return error.isNotEmpty() ? error : juce::String ("it could not be created");

    return check (*instance);
}

juce::String ScannerWorker::check (juce::AudioPluginInstance& instance)
{
    instance.enableAllBuses();
    const auto inputs = instance.getTotalNumInputChannels();
    const auto outputs = instance.getTotalNumOutputChannels();
    instance.prepareToPlay (checkRate, checkBlock);

    juce::AudioBuffer<float> buffer (juce::jmax (1, inputs, outputs), checkBlock);
    juce::MidiBuffer midi;
    juce::Random random (1);

    for (int block = 0; block < checkBlocks; ++block)
    {
        buffer.clear();

        for (int ch = 0; ch < inputs; ++ch)
            for (int i = 0; i < checkBlock; ++i)
                buffer.setSample (ch, i, (random.nextFloat() - 0.5f) * 0.2f);

        midi.clear();

        if (block == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        else if (block == checkBlocks / 2)
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

        instance.processBlock (buffer, midi);

        for (int ch = 0; ch < outputs; ++ch)
            for (int i = 0; i < checkBlock; ++i)
                if (! std::isfinite (buffer.getSample (ch, i)))
                    return "it played invalid samples (not a number, or infinite)";
    }

    juce::MemoryBlock state;
    instance.getStateInformation (state);

    if (! state.isEmpty())
        instance.setStateInformation (state.getData(), (int) state.getSize());

    instance.releaseResources();
    return {};
}

void ScannerWorker::handleMessageFromCoordinator (const juce::MemoryBlock& message)
{
    if (message.isEmpty())
        return;

    // Requests arrive on the IPC thread and are handled from the message thread.
    {
        const std::lock_guard<std::mutex> lock (mutex);
        pending.push (message);
    }

    triggerAsyncUpdate();
}

void ScannerWorker::handleConnectionMade()
{
    // Sent before any request is answered: the coordinator ends this process by its id.
    juce::XmlElement hello ("WORKER");
    hello.setAttribute ("pid", (int) getpid());
    reply (hello);
}

void ScannerWorker::handleConnectionLost()
{
    // The coordinator closed the pipe: it is done, or it gave up on a plug-in that hung. This
    // runs on the connection's thread, so it ends the worker even when a plug-in holds the
    // message thread, rather than leaving it running on its own.
    std::_Exit (0);
}

void ScannerWorker::handleAsyncUpdate()
{
    for (;;)
    {
        juce::MemoryBlock request;

        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (pending.empty() || waitingForAudioUnit)
                return;

            request = std::move (pending.front());
            pending.pop();
        }

        juce::MemoryInputStream in (request, false);
        const auto formatName = in.readString();
        const auto identifier = in.readString();

        for (auto* format : formatManager.getFormats())
        {
            if (format->getName() != formatName)
                continue;

            if (formatName == "AudioUnit")
                scanAudioUnit (*format, identifier);
            else
                reply (scan (*format, identifier));
        }
    }
}

void ScannerWorker::reply (const juce::XmlElement& xml)
{
    const auto text = xml.toString();
    sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
}

juce::String ScannerWorker::missingCodeReason (const juce::String& formatName, const juce::String& identifier)
{
    if (const auto bundle = bundleFor (formatName, identifier); bundle.exists() && ! hasCodeForThisMac (bundle))
        return juce::String ("it has no code for ")
               + (isAppleSilicon() ? "Apple silicon (it is built only for Intel Macs; it may work with SpectraBlade opened using Rosetta)"
                                   : "Intel Macs (it is built only for Apple silicon)");

    return {};
}

juce::XmlElement ScannerWorker::scan (juce::AudioPluginFormat& format, const juce::String& identifier)
{
    juce::XmlElement xml ("LIST");
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, identifier);

    if (found.isEmpty())
        if (const auto reason = missingCodeReason (format.getName(), identifier); reason.isNotEmpty())
            xml.setAttribute ("reason", reason);

    for (auto* description : found)
    {
        if (const auto problem = check (format, *description); problem.isNotEmpty())
        {
            auto* failed = xml.createNewChildElement ("NOTWORKING");
            failed->setAttribute ("name", description->name);
            failed->setAttribute ("reason", problem);
        }
        else
        {
            xml.addChildElement (description->createXml().release());
        }
    }

    return xml;
}

void ScannerWorker::scanAudioUnit (juce::AudioPluginFormat& format, const juce::String& identifier)
{
    // Many Audio Units can only be created while the message thread is free, so each one is
    // created the way a host does it, asynchronously, and checked when it arrives. One
    // identifier is one component, so one type. Requests wait until it is done.
    {
        const std::lock_guard<std::mutex> lock (mutex);
        waitingForAudioUnit = true;
    }

    juce::PluginDescription wanted;
    wanted.fileOrIdentifier = identifier;
    wanted.pluginFormatName = format.getName();

    format.createPluginInstanceAsync (wanted, checkRate, checkBlock,
                                      [this, identifier, formatName = format.getName()] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                                                        const juce::String& error)
    {
        juce::XmlElement xml ("LIST");

        if (instance == nullptr)
        {
            const auto missing = missingCodeReason (formatName, identifier);
            xml.setAttribute ("reason", missing.isNotEmpty() ? missing : error.isNotEmpty() ? error : juce::String ("it could not be created"));
        }
        else
        {
            const auto description = instance->getPluginDescription();

            if (const auto problem = check (*instance); problem.isNotEmpty())
            {
                auto* failed = xml.createNewChildElement ("NOTWORKING");
                failed->setAttribute ("name", description.name);
                failed->setAttribute ("reason", problem);
            }
            else
            {
                xml.addChildElement (description.createXml().release());
            }
        }

        instance = nullptr;
        reply (xml);

        {
            const std::lock_guard<std::mutex> lock (mutex);
            waitingForAudioUnit = false;
        }

        triggerAsyncUpdate();
    });
}

//==============================================================================
class OutOfProcessScanner::Coordinator final : private juce::ChildProcessCoordinator
{
public:
    explicit Coordinator (const juce::File& executable)
    {
        launched = launchWorkerProcess (executable, scannerProcessUID, 0, 0);
    }

    ~Coordinator() override
    {
        // A plug-in that hangs holds the worker's message thread, where JUCE's quit message
        // would arrive, so the worker is ended directly. It is still this process's unreaped
        // child here, so the id can't belong to anything else.
        if (const auto pid = workerPid.load(); pid > 0)
            ::kill (pid, SIGKILL);

        killWorkerProcess();
    }

    enum class Result { ok, failed, cancelled };

    /** On failure, reason says why; problems lists types found but not working. */
    Result scan (const juce::String& formatName, const juce::String& identifier, int timeout,
                 juce::OwnedArray<juce::PluginDescription>& results, juce::StringArray& problems,
                 juce::String& reason, const std::function<bool()>& shouldExit)
    {
        if (! launched)
        {
            reason = "the scanner process did not start";
            return Result::failed;
        }

        {
            const std::lock_guard<std::mutex> lock (mutex);
            reply.reset();
            gotReply = false;
        }

        juce::MemoryBlock request;
        {
            juce::MemoryOutputStream out (request, false);
            out.writeString (formatName);
            out.writeString (identifier);
        }

        if (! sendMessageToWorker (request))
        {
            reason = "the scanner process did not respond";
            return Result::failed;
        }

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeout;
        std::unique_lock<std::mutex> lock (mutex);

        while (! gotReply && ! connectionLost)
        {
            if (shouldExit())
                return Result::cancelled;

            if (juce::Time::getMillisecondCounter() > deadline)
            {
                reason = "it took longer than " + (timeout % 1000 == 0 ? juce::String (timeout / 1000) : juce::String (timeout / 1000.0, 1))
                         + " s to load and run";
                return Result::failed;
            }

            condition.wait_for (lock, std::chrono::milliseconds (50));
        }

        if (! gotReply)
        {
            reason = "it crashed while loading or playing";
            return Result::failed;
        }

        if (reply != nullptr)
        {
            for (auto* item : reply->getChildIterator())
            {
                if (item->hasTagName ("NOTWORKING"))
                {
                    problems.add (item->getStringAttribute ("name") + ": " + item->getStringAttribute ("reason"));
                    continue;
                }

                auto description = std::make_unique<juce::PluginDescription>();

                if (description->loadFromXml (*item))
                    results.add (description.release());
            }
        }

        if (results.isEmpty())
        {
            reason = ! problems.isEmpty() ? problems.joinIntoString ("; ")
                   : reply != nullptr && reply->hasAttribute ("reason") ? reply->getStringAttribute ("reason")
                   : juce::String ("no plug-in was found in it");
            return Result::failed;
        }

        return Result::ok;
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& message) override
    {
        auto xml = juce::parseXML (message.toString());

        if (xml != nullptr && xml->hasTagName ("WORKER"))
        {
            workerPid = xml->getIntAttribute ("pid");
            return;
        }

        const std::lock_guard<std::mutex> lock (mutex);
        reply = std::move (xml);
        gotReply = true;
        condition.notify_all();
    }

    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        connectionLost = true;
        condition.notify_all();
    }

    bool launched = false;
    std::atomic<int> workerPid { 0 };
    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> reply;
    bool gotReply = false;
    bool connectionLost = false;
};

//==============================================================================
OutOfProcessScanner::OutOfProcessScanner (const juce::File& executable, int timeout)
    : workerExecutable (executable == juce::File() ? juce::File::getSpecialLocation (juce::File::currentExecutableFile) : executable),
      timeoutMs (timeout)
{
}

OutOfProcessScanner::~OutOfProcessScanner() = default;

bool OutOfProcessScanner::findPluginTypesFor (juce::AudioPluginFormat& format,
                                              juce::OwnedArray<juce::PluginDescription>& result,
                                              const juce::String& fileOrIdentifier)
{
    Coordinator* coordinator = nullptr;

    {
        const std::lock_guard<std::mutex> lock (coordinatorsLock);
        auto& mine = coordinators[std::this_thread::get_id()];

        if (mine == nullptr)
            mine = std::make_unique<Coordinator> (workerExecutable);

        coordinator = mine.get();
    }

    juce::StringArray problems;
    juce::String reason;
    const auto outcome = coordinator->scan (format.getName(), fileOrIdentifier, timeoutMs, result, problems, reason,
                                            [this] { return shouldExit(); });

    if (outcome != Coordinator::Result::ok)
    {
        // The worker may have crashed, hung, or still be busy with this plug-in; start a fresh
        // one for the next file, so a late reply can't be taken for the next one's.
        {
            const std::lock_guard<std::mutex> lock (coordinatorsLock);
            coordinators.erase (std::this_thread::get_id());
        }

        if (outcome == Coordinator::Result::cancelled)
            return true;

        juce::Logger::writeToLog ("Plug-in not working (" + format.getName() + "): " + fileOrIdentifier + ": " + reason);
        ++notWorking;
        return false;
    }

    ++working;

    for (auto& problem : problems)
        juce::Logger::writeToLog ("Plug-in type not working (" + format.getName() + "): " + fileOrIdentifier + ": " + problem);

    return true;
}

void OutOfProcessScanner::scanFinished()
{
    const std::lock_guard<std::mutex> lock (coordinatorsLock);
    coordinators.clear();
}

//==============================================================================
class PluginScan::Job final : public juce::ThreadPoolJob
{
public:
    explicit Job (PluginScan& s) : ThreadPoolJob ("Plug-in scan"), scan (s) {}

    JobStatus runJob() override
    {
        // Every job works through each format's list in turn; the scanner hands each file to
        // only one of them.
        for (size_t f = 0; f < scan.formatScanners.size(); ++f)
        {
            auto& format = *scan.scannedFormats[f];
            auto& formatScanner = scan.formatScanners[f];
            juce::String name;

            while (! shouldExit())
            {
                {
                    const std::lock_guard<std::mutex> lock (scan.nameLock);
                    scan.currentName = format.getName() + ": "
                                       + format.getNameOfPluginFromIdentifier (formatScanner->getNextPluginFileThatWillBeScanned());
                }

                if (! formatScanner->scanNextFile (true, name))
                    break;
            }
        }

        --scan.jobsLeft;
        return jobHasFinished;
    }

private:
    PluginScan& scan;
};

PluginScan::PluginScan (juce::AudioPluginFormatManager& formats, juce::KnownPluginList& l, bool retryNotWorking,
                        const juce::File& workerExecutable)
    : list (l)
{
    auto custom = std::make_unique<OutOfProcessScanner> (workerExecutable);
    scanner = custom.get();
    list.setCustomScanner (std::move (custom));

    if (retryNotWorking)
        list.clearBlacklistedFiles();

    // No dead man's pedal: plug-ins are only ever loaded in the workers during a scan.
    for (auto* format : formats.getFormats())
    {
        // The built-in effects have nothing to scan; they are added to the list directly.
        if (! format->canScanForPlugins())
            continue;

        scannedFormats.push_back (format);
        formatScanners.push_back (std::make_unique<juce::PluginDirectoryScanner> (list, *format, format->getDefaultLocationsToSearch(),
                                                                                 true, juce::File(), true));
    }

    for (int i = 0; i < numThreads; ++i)
        pool.addJob (new Job (*this), true);
}

PluginScan::~PluginScan()
{
    // Each worker gives up on the plug-in it is waiting for as soon as its job is told to stop.
    pool.removeAllJobs (true, OutOfProcessScanner::perPluginTimeoutMs + 5000);
    scanner->scanFinished();
}

double PluginScan::getProgress() const
{
    if (formatScanners.empty() || isFinished())
        return 1.0;

    double total = 0.0;

    for (auto& s : formatScanners)
        total += juce::jlimit (0.0, 1.0, (double) s->getProgress());

    return total / (double) formatScanners.size();
}

juce::String PluginScan::getCurrentName() const
{
    const std::lock_guard<std::mutex> lock (nameLock);
    return currentName;
}
