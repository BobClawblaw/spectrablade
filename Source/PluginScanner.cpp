#include "PluginScanner.h"

//==============================================================================
ScannerWorker::ScannerWorker()
{
    juce::addDefaultFormatsToManager (formatManager);
}

void ScannerWorker::handleMessageFromCoordinator (const juce::MemoryBlock& message)
{
    if (message.isEmpty())
        return;

    // Requests arrive on the IPC thread; plug-ins (AudioUnits especially) must be loaded on the message thread.
    {
        const std::lock_guard<std::mutex> lock (mutex);
        pending.push (message);
    }

    triggerAsyncUpdate();
}

void ScannerWorker::handleConnectionLost()
{
    juce::JUCEApplicationBase::quit();
}

void ScannerWorker::handleAsyncUpdate()
{
    for (;;)
    {
        juce::MemoryBlock request;

        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (pending.empty())
                return;

            request = std::move (pending.front());
            pending.pop();
        }

        juce::MemoryInputStream in (request, false);
        const auto formatName = in.readString();
        const auto identifier = in.readString();

        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* format : formatManager.getFormats())
            if (format->getName() == formatName)
                format->findAllTypesForFile (found, identifier);

        juce::XmlElement reply ("LIST");

        for (auto* description : found)
            reply.addChildElement (description->createXml().release());

        const auto text = reply.toString();
        sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
    }
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
        killWorkerProcess();
    }

    enum class Result { ok, failed, cancelled };

    Result scan (const juce::String& formatName, const juce::String& identifier,
                 juce::OwnedArray<juce::PluginDescription>& results,
                 const std::function<bool()>& shouldExit)
    {
        if (! launched)
            return Result::failed;

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
            return Result::failed;

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) perPluginTimeoutMs;
        std::unique_lock<std::mutex> lock (mutex);

        while (! gotReply && ! connectionLost)
        {
            if (shouldExit())
                return Result::cancelled;

            if (juce::Time::getMillisecondCounter() > deadline)
                return Result::failed;

            condition.wait_for (lock, std::chrono::milliseconds (50));
        }

        if (! gotReply)
            return Result::failed;

        if (reply != nullptr)
        {
            for (auto* item : reply->getChildIterator())
            {
                auto description = std::make_unique<juce::PluginDescription>();

                if (description->loadFromXml (*item))
                    results.add (description.release());
            }
        }

        return Result::ok;
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& message) override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        reply = juce::parseXML (message.toString());
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
    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> reply;
    bool gotReply = false;
    bool connectionLost = false;
};

//==============================================================================
OutOfProcessScanner::OutOfProcessScanner (const juce::File& executable)
    : workerExecutable (executable == juce::File() ? juce::File::getSpecialLocation (juce::File::currentExecutableFile) : executable)
{
}

OutOfProcessScanner::~OutOfProcessScanner() = default;

bool OutOfProcessScanner::findPluginTypesFor (juce::AudioPluginFormat& format,
                                              juce::OwnedArray<juce::PluginDescription>& result,
                                              const juce::String& fileOrIdentifier)
{
    if (coordinator == nullptr)
        coordinator = std::make_unique<Coordinator> (workerExecutable);

    const auto outcome = coordinator->scan (format.getName(), fileOrIdentifier, result,
                                            [this] { return shouldExit(); });

    if (outcome == Coordinator::Result::failed)
    {
        // The worker crashed or hung on this plug-in; start a fresh one for the next file.
        coordinator = nullptr;
        return false;
    }

    return true;
}

void OutOfProcessScanner::scanFinished()
{
    coordinator = nullptr;
}
