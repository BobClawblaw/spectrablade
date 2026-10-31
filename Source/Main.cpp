#include <JuceHeader.h>
#include "MainComponent.h"
#include "PluginScanner.h"

class WaveloomApplication final : public juce::JUCEApplication
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

        // ~/Library/Logs/Waveloom/Waveloom.log
        logger.reset (juce::FileLogger::createDefaultAppLogger ("Waveloom", "Waveloom.log",
                                                               getApplicationName() + " " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
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
            mainWindow->getMainComponent().confirmDiscardChanges ([] { quit(); });
        else
            quit();
    }
    void anotherInstanceStarted (const juce::String&) override {}

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

START_JUCE_APPLICATION (WaveloomApplication)
