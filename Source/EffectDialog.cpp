#include "EffectDialog.h"

namespace
{
    constexpr int width = 560, margin = 16, rowHeight = 30, labelWidth = 200, gap = 8;

    double stepOf (const EffectDialog::Field& f)
    {
        return f.step > 0.0 ? f.step : std::pow (10.0, -f.decimals);
    }

    double rounded (double value, int decimals)
    {
        const auto scale = std::pow (10.0, decimals);
        return std::round (value * scale) / scale;
    }

    /** Wheel notches from a wheel move: a mouse wheel's click is one, a trackpad's fractions add up. */
    double notchesOf (const juce::MouseWheelDetails& wheel, double& leftOver)
    {
        auto delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;

        if (wheel.isReversed)
            delta = -delta;

        if (! wheel.isSmooth)
            return delta > 0.0f ? 1.0 : (delta < 0.0f ? -1.0 : 0.0);

        leftOver += delta / 0.08;   // a trackpad gives about 0.01 to 0.1 a move
        const auto whole = std::trunc (leftOver);
        leftOver -= whole;
        return whole;
    }
}

//==============================================================================
/** A number: a slider with its value beside it. */
class EffectDialog::Number final : public juce::Slider
{
public:
    explicit Number (const Field& f) : field (f)
    {
        setSliderStyle (juce::Slider::LinearHorizontal);
        setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, rowHeight - 6);
        setRange (f.min, f.max, 0.0);

        // Wide ranges of positive values (frequencies, times) spread evenly in ratio rather than in steps.
        if (f.min > 0.0 && f.max / f.min >= 100.0)
            setSkewFactorFromMidPoint (std::sqrt (f.min * f.max));

        setNumDecimalPlacesToDisplay (f.decimals);
        setValue (f.value, juce::dontSendNotification);
        setDoubleClickReturnValue (true, f.value);
        setWantsKeyboardFocus (true);
    }

    double getShownValue() const      { return rounded (getValue(), field.decimals); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! isEnabled())
            return;

        if (const auto notches = notchesOf (wheel, leftOver); notches != 0.0)
            setValue (nudged (field, getValue(), notches, e.mods.isShiftDown()), juce::sendNotificationSync);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        const auto up = code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey;
        const auto down = code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey;

        if (! up && ! down)
            return false;   // Space, Return and Escape go to the dialog

        setValue (nudged (field, getValue(), up ? 1.0 : -1.0, key.getModifiers().isShiftDown()), juce::sendNotificationSync);
        return true;
    }

private:
    Field field;
    double leftOver = 0.0;
};

namespace
{
    /** A choice: a menu the wheel steps through, which leaves Space to the dialog. */
    class Choice final : public juce::ComboBox
    {
    public:
        explicit Choice (const EffectDialog::Field& f)
        {
            addItemList (f.choices, 1);
            setSelectedItemIndex (juce::jlimit (0, f.choices.size() - 1, (int) std::lround (f.value)), juce::dontSendNotification);
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
        {
            if (! isEnabled())
                return;

            // Wheel down moves down the list, as the menu shows it.
            if (const auto notches = notchesOf (wheel, leftOver); notches != 0.0)
                setSelectedItemIndex (juce::jlimit (0, getNumItems() - 1, getSelectedItemIndex() - (int) notches), juce::sendNotificationSync);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            return key != juce::KeyPress (juce::KeyPress::spaceKey) && juce::ComboBox::keyPressed (key);
        }

    private:
        double leftOver = 0.0;
    };
}

namespace
{
    /** Text: leaves Space, Return and Escape to the dialog. */
    class Text final : public juce::TextEditor
    {
    public:
        explicit Text (const EffectDialog::Field& f)
        {
            setInputRestrictions (f.maxLength, f.allowed.removeCharacters (" "));
            setText (f.text, false);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress (juce::KeyPress::spaceKey) || key == juce::KeyPress (juce::KeyPress::returnKey)
                || key == juce::KeyPress (juce::KeyPress::escapeKey))
                return false;

            return juce::TextEditor::keyPressed (key);
        }
    };
}

struct EffectDialog::Row
{
    Field field;
    juce::Label label;
    std::unique_ptr<Number> number;
    std::unique_ptr<juce::ComboBox> menu;
    std::unique_ptr<juce::TextEditor> text;

    juce::Component& control() const
    {
        if (number != nullptr)
            return *number;

        return menu != nullptr ? static_cast<juce::Component&> (*menu) : *text;
    }
};

EffectDialog::Field EffectDialog::Field::textField (juce::String id, juce::String label, juce::String text, juce::String allowed, int maxLength)
{
    Field f;
    f.id = std::move (id);
    f.label = std::move (label);
    f.isText = true;
    f.text = std::move (text);
    f.allowed = std::move (allowed);
    f.maxLength = maxLength;
    return f;
}

//==============================================================================
double EffectDialog::nudged (const Field& f, double value, double notches, bool fine)
{
    const auto step = stepOf (f) * (fine ? 0.1 : 1.0);

    // From a value off the step's grid, the first step lands on it.
    auto next = value + notches * step;
    next = std::round (next / step) * step;
    return juce::jlimit (f.min, f.max, rounded (next, f.decimals + (fine ? 1 : 0)));
}

EffectDialog::EffectDialog (Options o) : options (std::move (o))
{
    message.setText (options.message, juce::dontSendNotification);
    message.setJustificationType (juce::Justification::topLeft);
    message.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (message);

    for (auto& f : options.fields)
    {
        auto row = std::make_unique<Row>();
        row->field = f;
        row->label.setText (f.label, juce::dontSendNotification);
        row->label.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (row->label);

        if (f.isText)
        {
            row->text = std::make_unique<Text> (f);
            row->text->onTextChange = [this] { changed(); };
        }
        else if (f.choices.isEmpty())
        {
            row->number = std::make_unique<Number> (f);
            row->number->onValueChange = [this] { changed(); };
        }
        else
        {
            row->menu = std::make_unique<Choice> (f);
            row->menu->onChange = [this] { changed(); };
        }

        addAndMakeVisible (row->control());
        rows.push_back (std::move (row));
    }

    if (options.presetKey.isNotEmpty())
    {
        presets = std::make_unique<EffectPresets::Bar> (EffectPresets::userStore(), options.presetKey,
                                                        [this] { return getValues(); },
                                                        [this] (const Values& v) { setValues (v); });
        addAndMakeVisible (*presets);
    }

    preview.setTooltip ("Play the selection through the effect (Space)");
    bypass.setTooltip ("Play the selection as it is, to compare");
    bypass.setEnabled (false);

    for (auto* b : { &preview, &bypass })
    {
        b->setWantsKeyboardFocus (false);
        b->setVisible (options.canPreview);
        addChildComponent (*b);
    }

    preview.onClick = [this]
    {
        bypass.setEnabled (preview.getToggleState());

        if (! preview.getToggleState())
            bypass.setToggleState (false, juce::dontSendNotification);

        if (onPreview != nullptr)
            onPreview (preview.getToggleState());
    };
    bypass.onClick = [this]
    {
        if (onBypass != nullptr)
            onBypass (bypass.getToggleState());
    };

    status.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.7f));
    status.setFont (juce::FontOptions (12.0f));   // small enough for two lines in its row (a finding can be long)
    status.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (status);

    applyButton.setButtonText (options.applyText);
    applyButton.onClick = [this] { apply(); };
    cancelButton.onClick = [this] { close(); };

    for (auto* b : { &applyButton, &cancelButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    // The message's height at the dialog's width.
    juce::AttributedString text;
    text.append (options.message, message.getFont());
    juce::TextLayout layout;
    layout.createLayout (text, (float) (width - 2 * margin));
    const auto messageHeight = options.message.isEmpty() ? 0 : (int) std::ceil (layout.getHeight()) + 4;

    setWantsKeyboardFocus (true);
    setSize (width, margin + messageHeight + (messageHeight > 0 ? gap : 0) + (int) rows.size() * (rowHeight + 4)
                        + (presets != nullptr ? gap + 28 : 0) + gap + 28 + gap + 28 + margin);
}

EffectDialog::~EffectDialog()
{
    stopTimer();

    if (onClose != nullptr)
        onClose();
}

EffectDialog::Values EffectDialog::getValues() const
{
    Values v;

    for (auto& r : rows)
    {
        if (r->number != nullptr)
            v[r->field.id] = r->number->getShownValue();
        else if (r->menu != nullptr)
            v[r->field.id] = (double) r->menu->getSelectedItemIndex();
    }

    return v;
}

juce::String EffectDialog::getText (const juce::String& id) const
{
    auto* t = getTextField (id);
    return t != nullptr ? t->getText().trim() : juce::String();
}

std::map<juce::String, juce::String> EffectDialog::getTexts() const
{
    std::map<juce::String, juce::String> texts;

    for (auto& r : rows)
        if (r->text != nullptr)
            texts[r->field.id] = r->text->getText().trim();

    return texts;
}

juce::TextEditor* EffectDialog::getTextField (const juce::String& id) const
{
    for (auto& r : rows)
        if (r->field.id == id)
            return r->text.get();

    return nullptr;
}

void EffectDialog::setValues (const Values& values)
{
    for (auto& r : rows)
    {
        const auto found = values.find (r->field.id);

        if (found == values.end() || ! std::isfinite (found->second) || r->text != nullptr)
            continue;

        if (r->number != nullptr)
            r->number->setValue (found->second, juce::dontSendNotification);
        else
            r->menu->setSelectedItemIndex (juce::jlimit (0, r->field.choices.size() - 1, (int) std::lround (found->second)), juce::dontSendNotification);
    }

    changed();
}

bool EffectDialog::isPreviewOn() const       { return preview.getToggleState(); }
bool EffectDialog::isBypassed() const        { return bypass.getToggleState(); }

void EffectDialog::setPreviewOn (bool on, juce::NotificationType n)
{
    if (on == preview.getToggleState() || ! options.canPreview)
        return;

    preview.setToggleState (on, juce::dontSendNotification);
    bypass.setEnabled (on);

    if (! on)
        bypass.setToggleState (false, juce::dontSendNotification);

    if (n != juce::dontSendNotification && onPreview != nullptr)
        onPreview (on);
}

void EffectDialog::setBypassed (bool on, juce::NotificationType n)
{
    if (on == bypass.getToggleState() || ! bypass.isEnabled())
        return;

    bypass.setToggleState (on, juce::dontSendNotification);

    if (n != juce::dontSendNotification && onBypass != nullptr)
        onBypass (on);
}

void EffectDialog::setStatus (const juce::String& s)   { status.setText (s, juce::dontSendNotification); }
juce::String EffectDialog::getStatus() const           { return status.getText(); }

juce::Slider* EffectDialog::getSlider (const juce::String& id) const
{
    for (auto& r : rows)
        if (r->field.id == id)
            return r->number.get();

    return nullptr;
}

juce::ComboBox* EffectDialog::getMenu (const juce::String& id) const
{
    for (auto& r : rows)
        if (r->field.id == id)
            return r->menu.get();

    return nullptr;
}

void EffectDialog::changed()
{
    if (onChange != nullptr)
        onChange (getValues());

    startTimer (settleMs);
}

void EffectDialog::timerCallback()
{
    stopTimer();

    if (onSettled != nullptr)
        onSettled (getValues());
}

void EffectDialog::apply()
{
    if (closing)
        return;

    closing = true;
    stopTimer();

    if (onApply != nullptr)
        onApply (getValues());

    close();
}

void EffectDialog::close()
{
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0);
}

bool EffectDialog::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress (juce::KeyPress::spaceKey))
    {
        setPreviewOn (! isPreviewOn());
        return true;
    }

    if (key == juce::KeyPress (juce::KeyPress::returnKey))
    {
        apply();
        return true;
    }

    if (key == juce::KeyPress (juce::KeyPress::escapeKey))
    {
        close();
        return true;
    }

    return false;
}

void EffectDialog::paint (juce::Graphics& g)
{
    g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
}

void EffectDialog::resized()
{
    auto r = getLocalBounds().reduced (margin);

    if (options.message.isNotEmpty())
    {
        juce::AttributedString text;
        text.append (options.message, message.getFont());
        juce::TextLayout layout;
        layout.createLayout (text, (float) r.getWidth());
        message.setBounds (r.removeFromTop ((int) std::ceil (layout.getHeight()) + 4));
        r.removeFromTop (gap);
    }

    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rowHeight);
        r.removeFromTop (4);
        row->label.setBounds (line.removeFromLeft (labelWidth - gap));
        line.removeFromLeft (gap);
        row->control().setBounds (row->number == nullptr ? line.withWidth (juce::jmin (line.getWidth(), 300)) : line);
    }

    if (presets != nullptr)
    {
        r.removeFromTop (gap);
        presets->setBounds (r.removeFromTop (28));
    }

    r.removeFromTop (gap);
    auto switches = r.removeFromTop (28);
    preview.setBounds (switches.removeFromLeft (90));
    bypass.setBounds (switches.removeFromLeft (90));
    status.setBounds (switches);

    r.removeFromTop (gap);
    auto buttons = r.removeFromTop (28);
    applyButton.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (gap);
    cancelButton.setBounds (buttons.removeFromRight (100));
}

void EffectDialog::show (std::unique_ptr<EffectDialog> dialog, const juce::String& title, juce::Component* parent)
{
    auto* content = dialog.release();
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = title;
    o.dialogBackgroundColour = content->findColour (juce::ResizableWindow::backgroundColourId);
    o.componentToCentreAround = parent;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;

    o.launchAsync();
    content->grabKeyboardFocus();
}
