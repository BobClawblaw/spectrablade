// Before JUCE's headers: letting the copy already running come to the front.
#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include <JuceHeader.h>
#include "MainComponent.h"
#include "PluginScanner.h"
#include "AudioFileIO.h"
#include "Theme.h"

class SpectraBladeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    // The plug-in scanner runs as a second copy of this executable. On Windows, where opening a file
    // from Explorer starts the program again, any other second copy hands its command line (the files
    // to open) to the one already running and ends; macOS sends files to the running app itself.
    bool moreThanOneInstanceAllowed() override
    {
       #if JUCE_WINDOWS
        if (getCommandLineParameters().contains (scannerProcessUID))
            return true;

        AllowSetForegroundWindow (ASFW_ANY);   // so the running copy can come to the front
        return false;
       #else
        return true;
       #endif
    }

    void initialise (const juce::String& commandLine) override
    {
        auto worker = std::make_unique<ScannerWorker>();

        if (worker->initialiseFromCommandLine (commandLine, scannerProcessUID))
        {
            scannerWorker = std::move (worker);
            return;
        }

        // ~/Library/Logs/SpectraBlade/SpectraBlade.log on macOS, %APPDATA%/SpectraBlade/SpectraBlade.log on Windows
        logger.reset (juce::FileLogger::createDefaultAppLogger ("SpectraBlade", "SpectraBlade.log",
                                                               getApplicationName() + " " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());

        // A crash writes where it happened to the log, under the lines that led up to it, so a
        // report can carry both.
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            juce::Logger::writeToLog ("Crashed. The stack:\n" + juce::SystemStats::getStackBacktrace());
        });

        juce::LookAndFeel::setDefaultLookAndFeel (&theme);

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        anotherInstanceStarted (commandLine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
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
    // Files opened from Finder or the Dock arrive here on macOS, as do any named on the command line
    // (on Windows, files opened from Explorer).
    // Each opens beside the files already open, the last one showing.
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;

        mainWindow->getMainComponent().openDocuments (AudioFileIO::filesIn (commandLine));

       #if JUCE_WINDOWS
        // Started again from Explorer or the Start menu: this window comes forward, restored if minimised
        // (not at start-up, when it shows once loading is done).
        if (mainWindow->isVisible())
        {
            if (mainWindow->isMinimised())
                mainWindow->setMinimised (false);

            mainWindow->toFront (true);
        }
       #endif
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, Theme::base, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent();
            content->onTitleChanged = [this] (const juce::String& title) { setName (title); };
            setContentOwned (content, true);
           #if ! JUCE_MAC
            setMenuBar (content);   // the Mac's menus are at the top of the screen
           #endif
            setResizable (true, true);
            setResizeLimits (800, 480, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            content->rememberPlace (*this, "Main");

            // The window shows once loading is done; until then the loading screen stands where it will be.
            content->showLoadingScreen (getBounds(), [this]
            {
                setVisible (true);
                toFront (true);
            });
        }

        MainComponent& getMainComponent()   { return *static_cast<MainComponent*> (getContentComponent()); }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<juce::FileLogger> logger;
    Theme::LookAndFeel theme;   // before the window, so it outlives it
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<ScannerWorker> scannerWorker;
};

START_JUCE_APPLICATION (SpectraBladeApplication)
