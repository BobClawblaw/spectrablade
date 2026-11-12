#pragma once

#include <JuceHeader.h>

/** What stands where the main window will be, from launch until the app is ready: the logo, and
    under it what is being loaded. Each piece of loading is a step, named by a key; the screen goes when the last one ends.
    A step that takes long (past continueAfterMs) brings up a Continue button, and its note if it has
    one, so nothing slow keeps the app from being used: what is still loading carries on behind. */
class LoadingScreen final : public juce::Component, private juce::Timer
{
public:
    static constexpr int continueAfterMs = 3000;
    static constexpr int shownAtLeastMs = 700;   // so a quick launch shows the logo, not a flash

    LoadingScreen();

    /** Starts a step, or changes what it says. slowNote is shown if the step is still going when
        Continue comes up. Ignored once the screen has finished. */
    void begin (const juce::String& key, const juce::String& text, const juce::String& slowNote = {});

    /** Ends a step. When none are left (and the screen has been up long enough), it finishes. */
    void end (const juce::String& key);

    bool isRunning (const juce::String& key) const;
    bool hasFinished() const noexcept                   { return finished; }

    /** The newest step still running ("" for none): the line under the logo. */
    juce::String getStatus() const;

    /** The note of a step still running that has one, once Continue is offered ("" otherwise). */
    juce::String getNote() const;

    bool isOfferingContinue() const                     { return continueButton.isVisible(); }
    bool hasLogo() const                                { return logo.isValid(); }

    /** The version shown in the corner: the app's own to start with ("" when there is no app, as in the tests). */
    void setVersion (const juce::String& v)             { version = v; repaint(); }
    juce::String getVersion() const                     { return version; }

    /** Called once, when the last step ends or Continue is pressed. */
    std::function<void()> onFinished;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::TextButton continueButton { "Continue" };

private:
    struct Step
    {
        juce::String key, text, note;
    };

    void timerCallback() override;
    void finishIfDone();
    void finish();

    std::vector<Step> steps;
    juce::Image logo;
    juce::String version;
    juce::uint32 shownAt;
    int dots = 0;
    bool finished = false;
};
