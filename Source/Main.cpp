#include <JuceHeader.h>
#include "MainComponent.h"
#include "PluginScanner.h"
#include "AudioFileIO.h"

class SpectraBladeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    // The plug-in scanner runs as a second copy of this executable.
    bool moreThanOneInstanceAllowed() override              { return true; }

    void initialise (const juce::String& commandLine) override
    {
        auto worker = std::make_unique<ScannerWorker>();

        if (worker->initialiseFromCommandLine (commandLine, scannerProcessUID))
        {
            scannerWorker = std::move (worker);
            return;
        }

        // ~/Library/Logs/SpectraBlade/SpectraBlade.log
        logger.reset (juce::FileLogger::createDefaultAppLogger ("SpectraBlade", "SpectraBlade.log",
                                                               getApplicationName() + " " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        anotherInstanceStarted (commandLine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::Logger::setCurrentLogger (nullptr);
        scannerWorker = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->getMainComponent().confirmQuit ([] { quit(); });
        else
            quit();
    }
    // Files opened from Finder or the Dock arrive here on macOS, as do any named on the command line.
    // Only the last one stays open, so they open one after another, each asking about unsaved changes.
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;

        for (auto& file : AudioFileIO::filesIn (commandLine))
            mainWindow->getMainComponent().openDocument (file);
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colour (0xff111317), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent();
            content->onTitleChanged = [this] (const juce::String& title) { setName (title); };
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (800, 480, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        MainComponent& getMainComponent()   { return *static_cast<MainComponent*> (getContentComponent()); }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<ScannerWorker> scannerWorker;
};

START_JUCE_APPLICATION (SpectraBladeApplication)
