#pragma once

#include "AudioEdits.h"
#include "AudioEngine.h"
#include "LevelMeter.h"
#include "RackView.h"
#include "WaveformView.h"

class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    /** Asks about unsaved changes, then calls proceed unless the user cancels. */
    void confirmDiscardChanges (std::function<void()> proceed);

    std::function<void (const juce::String& title)> onTitleChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    ApplicationCommandTarget* getNextCommandTarget() override  { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int, int) override {}

private:
    class PluginEditorWindow;
    class PluginListWindow;
    class RackRenderJob;
    class BackgroundJob;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    // File
    void openFile (const juce::File&);
    void chooseFile();
    void save();
    void saveAs (std::function<void (bool saved)> done = {});
    void writeFile (const juce::File&, int bitDepth, std::function<void (bool saved)> done);

    // Edit
    juce::Range<juce::int64> selectionOrAll() const;
    void applyEdit (AudioDocument::Snapshot next, const juce::String& description, juce::Range<juce::int64> newSelection);
    void applyEdit (AudioDocument::Snapshot next, const juce::String& description, juce::Range<juce::int64> newSelection,
                    Markers);
    void cut();
    void copy();
    void paste();
    void pasteClip (const AudioClip&);
    void chooseSampleRate();
    void convertSampleRate (double newRate);
    void deleteSelection();
    void cropToSelection();

    // Spectrogram
    void setDisplay (WaveformView::Display);
    void applySpectral (float gainDb);

    // Markers
    void addMarker();
    void goToMarker (bool forwards);

    // Effects
    void askForDecibels (const juce::String& title, const juce::String& label, float initial,
                         std::function<void (float)> onOk);
    void applyGain();
    void applyNormalize();
    void applySimpleEffect (const juce::String& name,
                            AudioDocument::Snapshot (*effect) (const AudioSnapshot&, juce::int64, juce::int64),
                            bool needsSelection);
    void applyRackToSelection();

    // Transport and windows
    void togglePlay();
    void toggleRecord();
    void startRecording();
    void stopRecording();
    void showAudioSettings();
    void showPluginList();
    void showEditor (int slotIndex);
    void closeEditorFor (juce::AudioProcessor*);
    void saveRack();
    void loadRack();
    void refreshOutputChoices();
    void refreshInputChoices();
    void refreshTitle();
    void saveSettings();

    juce::ApplicationProperties appProperties;
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    AudioClip clipboard;

    juce::TextButton openButton { "Open..." }, playButton { "Play" }, stopButton { "Stop" }, recordButton { "Record" },
                     pluginsButton { "Plug-ins..." }, audioButton { "Audio Settings..." },
                     saveRackButton { "Save Rack..." }, loadRackButton { "Load Rack..." };
    juce::Label outputLabel { {}, "Output" };
    juce::ComboBox outputPair;
    juce::Label inputLabel { {}, "Input" };
    juce::ComboBox inputChoice;
    LevelMeter inputMeter;
    juce::Label fileLabel, deviceLabel, timeLabel, selectionLabel;

    WaveformView waveform { engine };
    RackView rackView { engine };

    std::unique_ptr<juce::FileChooser> chooser;
    juce::OwnedArray<PluginEditorWindow> editorWindows;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<RackRenderJob> renderJob;
    std::unique_ptr<BackgroundJob> backgroundJob;
    juce::File deadMansPedalFile;

    // The take in progress: where it goes in the document ([punchIn, punchOut), or open-ended when
    // punchOut < 0) and the round-trip latency measured when it started.
    juce::int64 punchIn = 0, punchOut = -1;
    int recordLatency = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
