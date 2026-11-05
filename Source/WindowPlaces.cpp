#include "WindowPlaces.h"
#include "Theme.h"

WindowPlaces::WindowPlaces (juce::PropertiesFile& s) : settings (s) {}

WindowPlaces::~WindowPlaces()
{
    for (auto& [window, name] : watched)
        window->removeComponentListener (this);
}

bool WindowPlaces::watch (juce::ResizableWindow& window, const juce::String& name, bool keepSize)
{
    auto used = false;
    const auto saved = settings.getValue (keyFor (name));
    const auto ownSize = window.getBounds();

    if (saved.isNotEmpty() && window.restoreWindowStateFromString (saved))
    {
        used = true;

        if (keepSize && ! window.isFullScreen())
            window.setSize (ownSize.getWidth(), ownSize.getHeight());

        if (! window.isFullScreen())
        {
            juce::Array<juce::Rectangle<int>> screens;

            for (auto& display : juce::Desktop::getInstance().getDisplays().displays)
                screens.add (display.userBounds.getSmallestIntegerContainer());

            if (const auto kept = reachable (window.getBounds(), screens); kept != window.getBounds())
                window.setBounds (kept);
        }
    }

    window.addComponentListener (this);
    watched.emplace_back (&window, name);
    return used;
}

juce::Rectangle<int> WindowPlaces::reachable (juce::Rectangle<int> bounds, const juce::Array<juce::Rectangle<int>>& screens)
{
    if (screens.isEmpty())
        return bounds;

    const auto top = bounds.withHeight (20);

    for (auto& screen : screens)
    {
        const auto shown = screen.getIntersection (top);

        if (shown.getWidth() >= 100 && shown.getHeight() >= 20)
            return bounds;
    }

    const auto& main = screens.getReference (0);
    return main.withSizeKeepingCentre (juce::jmin (bounds.getWidth(), main.getWidth()), juce::jmin (bounds.getHeight(), main.getHeight()));
}

void WindowPlaces::componentMovedOrResized (juce::Component& c, bool, bool)
{
    auto* window = dynamic_cast<juce::ResizableWindow*> (&c);

    if (window == nullptr || window->isMinimised())
        return;

    // JUCE keeps a window's last normal place only while it shows; its bounds are what it has now.
    // Full screen, JUCE's own string keeps the size to come back to.
    const auto place = window->isFullScreen() ? window->getWindowStateAsString() : window->getBounds().toString();

    for (auto& [w, name] : watched)
        if (w == &c)
            settings.setValue (keyFor (name), place);
}

void WindowPlaces::componentBeingDeleted (juce::Component& c)
{
    c.removeComponentListener (this);
    watched.erase (std::remove_if (watched.begin(), watched.end(), [&c] (auto& w) { return w.first == &c; }), watched.end());
}

//==============================================================================
DividerBar::DividerBar()
{
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    setRepaintsOnMouseActivity (true);
}

void DividerBar::paint (juce::Graphics& g)
{
    g.setColour (Theme::lineStrong.withAlpha (isMouseOverOrDragging() ? 1.0f : 0.35f));
    g.fillRect (getLocalBounds().withSizeKeepingCentre (2, getHeight()));
}

void DividerBar::mouseDrag (const juce::MouseEvent& e)
{
    if (onDrag != nullptr && getParentComponent() != nullptr)
        onDrag (e.getEventRelativeTo (getParentComponent()).getPosition().x);
}

void DividerBar::mouseUp (const juce::MouseEvent&)
{
    if (onDragEnd != nullptr)
        onDragEnd();
}
