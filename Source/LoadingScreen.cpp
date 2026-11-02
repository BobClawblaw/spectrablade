#include "LoadingScreen.h"
#include "BinaryData.h"

LoadingScreen::LoadingScreen()
    : logo (juce::ImageCache::getFromMemory (BinaryData::spectrabladebanner_png, BinaryData::spectrabladebanner_pngSize)),
      shownAt (juce::Time::getMillisecondCounter())
{
    setOpaque (true);
    continueButton.setTooltip ("Use the app now; what is still loading carries on behind");
    continueButton.onClick = [this] { finish(); };
    addChildComponent (continueButton);
    startTimer (250);
}

void LoadingScreen::begin (const juce::String& key, const juce::String& text, const juce::String& slowNote)
{
    if (finished)
        return;

    for (auto& s : steps)
        if (s.key == key)
        {
            s.text = text;
            s.note = slowNote;
            repaint();
            return;
        }

    steps.push_back ({ key, text, slowNote });
    repaint();
}

void LoadingScreen::end (const juce::String& key)
{
    std::erase_if (steps, [&key] (const Step& s) { return s.key == key; });
    repaint();
    finishIfDone();
}

bool LoadingScreen::isRunning (const juce::String& key) const
{
    return std::any_of (steps.begin(), steps.end(), [&key] (const Step& s) { return s.key == key; });
}

juce::String LoadingScreen::getStatus() const
{
    return steps.empty() ? juce::String() : steps.back().text;
}

juce::String LoadingScreen::getNote() const
{
    if (isOfferingContinue())
        for (auto& s : steps)
            if (s.note.isNotEmpty())
                return s.note;

    return {};
}

void LoadingScreen::timerCallback()
{
    dots = (dots + 1) % 4;

    if (! continueButton.isVisible() && ! steps.empty() && juce::Time::getMillisecondCounter() - shownAt >= (juce::uint32) continueAfterMs)
        continueButton.setVisible (true);

    repaint();
    finishIfDone();
}

void LoadingScreen::finishIfDone()
{
    if (steps.empty() && juce::Time::getMillisecondCounter() - shownAt >= (juce::uint32) shownAtLeastMs)
        finish();
}

void LoadingScreen::finish()
{
    if (finished)
        return;

    finished = true;
    steps.clear();
    stopTimer();

    if (onFinished != nullptr)
        onFinished();
}

void LoadingScreen::paint (juce::Graphics& g)
{
    // The banner's corner colour around it, and its edges faded into that, so no edge shows.
    const auto back = logo.isValid() ? logo.getPixelAt (2, 2).withAlpha (1.0f) : juce::Colour (0xff0b0a14);
    g.fillAll (back);
    auto area = getLocalBounds().reduced (16);
    auto text = area.removeFromBottom (juce::jmin (112, area.getHeight() / 3));

    if (logo.isValid())
    {
        const auto placed = juce::RectanglePlacement (juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize)
                                .appliedTo (logo.getBounds().toFloat(), area.toFloat());
        g.drawImage (logo, placed);

        const auto fade = juce::jmin (placed.getWidth(), placed.getHeight()) * 0.12f;
        const auto edge = [&] (juce::Rectangle<float> band, juce::Point<float> solid, juce::Point<float> clear)
        {
            g.setGradientFill (juce::ColourGradient (back, solid, back.withAlpha (0.0f), clear, false));
            g.fillRect (band);
        };
        edge (placed.withWidth (fade), placed.getTopLeft(), placed.getTopLeft().translated (fade, 0));
        edge (placed.withLeft (placed.getRight() - fade), placed.getTopRight(), placed.getTopRight().translated (-fade, 0));
        edge (placed.withHeight (fade), placed.getTopLeft(), placed.getTopLeft().translated (0, fade));
        edge (placed.withTop (placed.getBottom() - fade), placed.getBottomLeft(), placed.getBottomLeft().translated (0, -fade));
    }

    g.setColour (juce::Colour (0xff2a2f36));
    g.drawRect (getLocalBounds());

    if (const auto status = getStatus(); status.isNotEmpty())
    {
        g.setColour (juce::Colour (0xffc9cfdb));
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (status + juce::String ("...").substring (0, dots), text.removeFromTop (22), juce::Justification::centred);
    }

    if (const auto note = getNote(); note.isNotEmpty())
    {
        g.setColour (juce::Colour (0xff8a93a6));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText (note, text.removeFromTop (48), juce::Justification::centredTop, 3);
    }
}

void LoadingScreen::resized()
{
    auto area = getLocalBounds().reduced (16);
    continueButton.setBounds (area.removeFromBottom (28).withSizeKeepingCentre (110, 26));
}
