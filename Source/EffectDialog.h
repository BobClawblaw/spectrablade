#pragma once

#include <JuceHeader.h>
#include "EffectPresets.h"

/** The dialog every built-in effect is set up in: a slider with its value for each number (the
    mouse wheel or the arrow keys step it, Shift by a tenth of the step; double-click sets it back
    to where it started), a menu for each choice (the wheel steps it too), the effect's presets,
    Preview and Bypass switches, and Apply and Cancel. Space starts and stops the preview, except
    while a value is being typed.

    It doesn't apply or preview anything itself: it says when the settings change, when they have
    stayed put for settleMs, and when the switches or buttons are used. */
class EffectDialog final : public juce::Component,
                           private juce::Timer
{
public:
    static constexpr int settleMs = 250;

    struct Field
    {
        juce::String id, label;
        double value = 0.0;
        int decimals = 0;
        juce::StringArray choices {};        // a menu, its value the choice's index
        double min = 0.0, max = 0.0;         // a number's range
        double step = 0.0;                   // a wheel notch or arrow key; 0: one in the last decimal shown

        // A text field instead (digits to dial, a time as 1:30): not among the values or presets.
        bool isText = false;
        juce::String text {}, allowed {};    // allowed: the characters it takes (Space never: it starts the preview)
        int maxLength = 0;

        static Field textField (juce::String id, juce::String label, juce::String text, juce::String allowed, int maxLength);
    };

    using Values = EffectPresets::Values;

    struct Options
    {
        juce::String message;                // above the settings; empty for none
        std::vector<Field> fields;
        juce::String presetKey;              // empty: no presets
        bool canPreview = true;
        juce::String applyText = "Apply";
        juce::String extraText {};           // a button at the left of Apply and Cancel ("Instrument Window..."); empty for none
    };

    explicit EffectDialog (Options);
    ~EffectDialog() override;

    /** Each field's value: a number as shown (rounded to its decimals), a choice's index (text fields aren't among them). */
    Values getValues() const;

    /** A text field's text, trimmed. */
    juce::String getText (const juce::String& id) const;

    /** Every text field's text, trimmed, by its ID. */
    std::map<juce::String, juce::String> getTexts() const;

    /** Sets the fields named (others are left), as a preset does, then calls onChange. */
    void setValues (const Values&);

    bool isPreviewOn() const;
    void setPreviewOn (bool on, juce::NotificationType = juce::sendNotification);
    bool isBypassed() const;
    void setBypassed (bool, juce::NotificationType = juce::sendNotification);

    /** A line beside the switches: what the preview is doing ("Processing..."), or why it can't. */
    void setStatus (const juce::String&);
    juce::String getStatus() const;

    std::function<void (const Values&)> onChange;    // a field changed (each step of a drag too)
    std::function<void (const Values&)> onSettled;   // no change for settleMs since the last one
    std::function<void (bool)> onPreview, onBypass;
    std::function<void (const Values&)> onApply;     // Apply or Return; then the dialog closes
    std::function<void()> onClose;                   // closed any way, after onApply when applied
    std::function<void (const Values&)> onExtra;     // the extra button (Options::extraText) clicked

    /** As if a setting had changed: onChange now, and onSettled once nothing changes for settleMs.
        For a change made elsewhere that the preview should follow (an instrument's own window). */
    void nudge();

    /** Opens it in a modal window of its own over `parent`; the window owns it. */
    static void show (std::unique_ptr<EffectDialog>, const juce::String& title, juce::Component* parent);

    /** Closes its window (as Cancel does, unless after Apply). */
    void close();

    /** The slider, menu or text field of a field, for tests. */
    juce::Slider* getSlider (const juce::String& id) const;
    juce::ComboBox* getMenu (const juce::String& id) const;
    juce::TextEditor* getTextField (const juce::String& id) const;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** How far a wheel move or arrow key moves a number: notches (a mouse wheel's click is one;
        a trackpad gives fractions) times its step, a tenth of it with Shift; kept in range. */
    static double nudged (const Field&, double value, double notches, bool fine);

    class Number;

private:
    struct Row;

    void changed();
    void timerCallback() override;
    void apply();

    Options options;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label message;
    std::unique_ptr<EffectPresets::Bar> presets;
    juce::ToggleButton preview { "Preview" }, bypass { "Bypass" };
    juce::Label status;
    juce::TextButton applyButton, cancelButton { "Cancel" }, extraButton;
    bool closing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectDialog)
};
