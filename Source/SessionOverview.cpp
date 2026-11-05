#include "SessionOverview.h"
#include "Theme.h"
#include "TrackColours.h"

SessionOverview::SessionOverview (AudioEngine& e, MultitrackView& v) : engine (e), view (v)
{
    engine.session.addChangeListener (this);
    startTimerHz (15);
}

SessionOverview::~SessionOverview()
{
    engine.session.removeChangeListener (this);
}

double SessionOverview::getTotalFrames() const
{
    const auto s = engine.session.getState();
    return juce::jmax ((double) s->getLength(), s->sampleRate * 60.0, view.getVisibleFrames().getEnd());
}

juce::int64 SessionOverview::frameAt (float x) const
{
    return (juce::int64) std::llround ((double) x / juce::jmax (1, getWidth()) * getTotalFrames());
}

float SessionOverview::xFor (double frame) const
{
    return (float) (frame / getTotalFrames() * getWidth());
}

void SessionOverview::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
    const auto s = engine.session.getState();
    std::vector<int> shown;

    for (int t = 0; t < (int) s->tracks.size(); ++t)
        if (! s->tracks[(size_t) t].hidden)
            shown.push_back (t);

    // One band per shown track, at most four pixels each, filling the strip when there are many.
    const auto band = shown.empty() ? 0.0f : juce::jmin (4.0f, (float) (getHeight() - 4) / (float) shown.size());

    for (size_t row = 0; row < shown.size(); ++row)
    {
        const auto t = shown[row];
        const auto& track = s->tracks[(size_t) t];

        for (auto& c : track.clips)
        {
            g.setColour (c.muted ? Theme::faintText : TrackColours::of (c, track, t));
            const auto x0 = xFor ((double) c.position), x1 = xFor ((double) c.getEnd());
            g.fillRect (juce::Rectangle<float> (x0, 2.0f + band * (float) row, juce::jmax (1.0f, x1 - x0), juce::jmax (1.0f, band - 1.0f)));
        }
    }

    const auto visible = view.getVisibleFrames();
    const auto frame = juce::Rectangle<float> (xFor (visible.getStart()), 0.5f, juce::jmax (3.0f, xFor (visible.getEnd()) - xFor (visible.getStart())),
                                               (float) getHeight() - 1.0f);
    g.setColour (Theme::overlay.withAlpha (0.08f));
    g.fillRect (frame);
    g.setColour (Theme::overlay.withAlpha (0.6f));
    g.drawRect (frame, 1.0f);

    g.setColour (juce::Colour (0xfff2cc60));
    g.drawVerticalLine ((int) xFor ((double) view.getCursor()), 0.0f, (float) getHeight());
}

void SessionOverview::centreOn (float x)
{
    const auto visible = view.getVisibleFrames();
    view.scrollTo ((double) frameAt (x) - visible.getLength() / 2.0);
    repaint();
}

void SessionOverview::mouseDown (const juce::MouseEvent& e)    { centreOn (e.position.x); }
void SessionOverview::mouseDrag (const juce::MouseEvent& e)    { centreOn (e.position.x); }

void SessionOverview::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

void SessionOverview::timerCallback()
{
    // Follows the lanes' scrolling and zoom, and the cursor while playing.
    if (const auto shown = view.getVisibleFrames(); shown != lastShown || engine.getPlayer().isPlaying())
    {
        lastShown = shown;
        repaint();
    }
}
