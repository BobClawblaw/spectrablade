#include "Dock.h"
#include "Theme.h"
#include "WindowPlaces.h"

namespace Dock
{
namespace
{
    bool wantsColumn (Side side)        { return side == Side::top || side == Side::bottom; }
    bool goesBefore (Side side)         { return side == Side::left || side == Side::top; }

    Node leafWith (const juce::String& panel)
    {
        Node n;
        n.tabs.add (panel);
        return n;
    }

    /** Drops empty groups, merges a split into a parent running the same way, replaces a split
        holding one child with it, and keeps each group's current tab in range. */
    void normalise (Node& node)
    {
        if (node.isLeaf())
        {
            node.current = node.tabs.isEmpty() ? 0 : juce::jlimit (0, node.tabs.size() - 1, node.current);
            return;
        }

        std::vector<Node> children;
        std::vector<double> weights;

        for (size_t i = 0; i < node.children.size(); ++i)
        {
            auto child = std::move (node.children[i]);
            normalise (child);
            auto w = i < node.weights.size() && node.weights[i] > 0.0 && std::isfinite (node.weights[i]) ? node.weights[i] : 1.0;

            if (child.isLeaf() && child.tabs.isEmpty())
                continue;

            if (! child.isLeaf() && child.column == node.column)
            {
                double sum = 0.0;

                for (auto cw : child.weights)
                    sum += cw;

                for (size_t j = 0; j < child.children.size(); ++j)
                {
                    children.push_back (std::move (child.children[j]));
                    weights.push_back (w * (sum > 0.0 ? child.weights[j] / sum : 1.0 / (double) child.children.size()));
                }

                continue;
            }

            children.push_back (std::move (child));
            weights.push_back (w);
        }

        if (children.empty())
        {
            node = Node {};
            return;
        }

        if (children.size() == 1)
        {
            auto only = std::move (children.front());
            node = std::move (only);
            return;
        }

        node.children = std::move (children);
        node.weights = std::move (weights);
    }

    void collect (const Node& node, juce::StringArray& out)
    {
        out.addArray (node.tabs);

        for (auto& c : node.children)
            collect (c, out);
    }

    bool findPath (const Node& node, const juce::String& panel, std::vector<int>& path)
    {
        if (node.isLeaf())
            return node.tabs.contains (panel);

        for (size_t i = 0; i < node.children.size(); ++i)
        {
            path.push_back ((int) i);

            if (findPath (node.children[i], panel, path))
                return true;

            path.pop_back();
        }

        return false;
    }

    void removeFrom (Node& node, const juce::String& panel)
    {
        if (node.isLeaf())
        {
            const auto index = node.tabs.indexOf (panel);

            if (index >= 0)
            {
                node.tabs.remove (index);

                if (node.current > index || node.current >= node.tabs.size())
                    node.current = juce::jmax (0, node.current - 1);
            }

            return;
        }

        for (auto& c : node.children)
            removeFrom (c, panel);
    }

    std::unique_ptr<juce::XmlElement> nodeToXml (const Node& node)
    {
        if (node.isLeaf())
        {
            auto xml = std::make_unique<juce::XmlElement> ("TABS");
            xml->setAttribute ("current", node.current);

            for (auto& t : node.tabs)
                xml->createNewChildElement ("PANEL")->setAttribute ("id", t);

            return xml;
        }

        auto xml = std::make_unique<juce::XmlElement> ("SPLIT");
        xml->setAttribute ("column", node.column);

        for (size_t i = 0; i < node.children.size(); ++i)
        {
            auto child = nodeToXml (node.children[i]);
            child->setAttribute ("weight", node.weights[i]);
            xml->addChildElement (child.release());
        }

        return xml;
    }

    std::optional<Node> nodeFromXml (const juce::XmlElement& xml, const juce::StringArray& known, juce::StringArray& seen, int depth)
    {
        if (depth > 32)
            return std::nullopt;

        Node node;

        if (xml.hasTagName ("TABS"))
        {
            for (auto* p : xml.getChildWithTagNameIterator ("PANEL"))
            {
                const auto id = p->getStringAttribute ("id");

                if (known.contains (id) && ! seen.contains (id))
                {
                    node.tabs.add (id);
                    seen.add (id);
                }
            }

            node.current = xml.getIntAttribute ("current");
            return node;
        }

        if (! xml.hasTagName ("SPLIT"))
            return std::nullopt;

        node.column = xml.getBoolAttribute ("column");

        for (auto* c : xml.getChildIterator())
        {
            auto child = nodeFromXml (*c, known, seen, depth + 1);

            if (! child.has_value())
                return std::nullopt;

            node.children.push_back (std::move (*child));
            node.weights.push_back (c->getDoubleAttribute ("weight", 1.0));
        }

        return node;
    }
}

//==============================================================================
Layout Layout::single (const juce::String& panel)
{
    Layout l;
    l.root = leafWith (panel);
    return l;
}

bool Layout::isDocked (const juce::String& panel) const
{
    return pathTo (panel).has_value();
}

bool Layout::isFloating (const juce::String& panel) const
{
    return std::any_of (floating.begin(), floating.end(), [&] (const Floating& f) { return f.panel == panel; });
}

bool Layout::isFront (const juce::String& panel) const
{
    const auto path = pathTo (panel);

    if (! path.has_value())
        return false;

    const auto* leaf = nodeAt (*path);
    return leaf->tabs[leaf->current] == panel;
}

juce::StringArray Layout::panels() const
{
    juce::StringArray out;
    collect (root, out);

    for (auto& f : floating)
        out.add (f.panel);

    return out;
}

void Layout::remove (const juce::String& panel)
{
    removeFrom (root, panel);
    floating.erase (std::remove_if (floating.begin(), floating.end(), [&] (const Floating& f) { return f.panel == panel; }), floating.end());
    normalise (root);
}

bool Layout::dock (const juce::String& panel, const juce::String& target, Side side, double share)
{
    if (panel == target || ! isDocked (target))
        return false;

    remove (panel);
    share = juce::jlimit (0.05, 0.95, share);
    const auto path = *pathTo (target);
    auto* leaf = nodeAt (path);

    if (side == Side::centre)
    {
        leaf->tabs.add (panel);
        leaf->current = leaf->tabs.size() - 1;
        return true;
    }

    const auto column = wantsColumn (side);
    const auto before = goesBefore (side);

    if (! path.empty())
    {
        auto* parent = nodeAt (std::vector<int> (path.begin(), path.end() - 1));
        const auto index = (size_t) path.back();

        if (parent->column == column)
        {
            const auto w = parent->weights[index];
            parent->weights[index] = w * (1.0 - share);
            const auto at = before ? index : index + 1;
            parent->children.insert (parent->children.begin() + (std::ptrdiff_t) at, leafWith (panel));
            parent->weights.insert (parent->weights.begin() + (std::ptrdiff_t) at, w * share);
            return true;
        }
    }

    Node split;
    split.column = column;
    auto old = std::move (*leaf);
    split.children.push_back (before ? leafWith (panel) : std::move (old));
    split.children.push_back (before ? std::move (old) : leafWith (panel));
    split.weights = before ? std::vector<double> { share, 1.0 - share } : std::vector<double> { 1.0 - share, share };
    *nodeAt (path) = std::move (split);
    return true;
}

void Layout::dockAtEdge (const juce::String& panel, Side side, double share)
{
    remove (panel);

    if (side == Side::centre || (root.isLeaf() && root.tabs.isEmpty()))
    {
        if (root.isLeaf())
        {
            root.tabs.add (panel);
            root.current = root.tabs.size() - 1;
            return;
        }

        side = Side::right;
    }

    share = juce::jlimit (0.05, 0.95, share);
    const auto column = wantsColumn (side);
    const auto before = goesBefore (side);

    if (! root.isLeaf() && root.column == column)
    {
        double total = 0.0;

        for (auto w : root.weights)
            total += w;

        const auto w = total * share / (1.0 - share);
        root.children.insert (before ? root.children.begin() : root.children.end(), leafWith (panel));
        root.weights.insert (before ? root.weights.begin() : root.weights.end(), w);
        return;
    }

    Node split;
    split.column = column;
    auto old = std::move (root);
    split.children.push_back (before ? leafWith (panel) : std::move (old));
    split.children.push_back (before ? std::move (old) : leafWith (panel));
    split.weights = before ? std::vector<double> { share, 1.0 - share } : std::vector<double> { 1.0 - share, share };
    root = std::move (split);
}

void Layout::floatPanel (const juce::String& panel, juce::Rectangle<int> screenBounds)
{
    remove (panel);
    floating.push_back ({ panel, screenBounds });
}

void Layout::bringToFront (const juce::String& panel)
{
    if (const auto path = pathTo (panel))
    {
        auto* leaf = nodeAt (*path);
        leaf->current = leaf->tabs.indexOf (panel);
    }
}

Node* Layout::nodeAt (const std::vector<int>& path)
{
    auto* node = &root;

    for (auto i : path)
    {
        if (! juce::isPositiveAndBelow (i, (int) node->children.size()))
            return nullptr;

        node = &node->children[(size_t) i];
    }

    return node;
}

const Node* Layout::nodeAt (const std::vector<int>& path) const
{
    return const_cast<Layout*> (this)->nodeAt (path);
}

std::optional<std::vector<int>> Layout::pathTo (const juce::String& panel) const
{
    std::vector<int> path;

    if (findPath (root, panel, path))
        return path;

    return std::nullopt;
}

std::unique_ptr<juce::XmlElement> Layout::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("DOCK");
    xml->addChildElement (nodeToXml (root).release());

    for (auto& f : floating)
    {
        auto* e = xml->createNewChildElement ("FLOATING");
        e->setAttribute ("panel", f.panel);
        e->setAttribute ("x", f.bounds.getX());
        e->setAttribute ("y", f.bounds.getY());
        e->setAttribute ("w", f.bounds.getWidth());
        e->setAttribute ("h", f.bounds.getHeight());
    }

    return xml;
}

std::optional<Layout> Layout::fromXml (const juce::XmlElement& xml, const juce::StringArray& known, const juce::String& required)
{
    if (! xml.hasTagName ("DOCK"))
        return std::nullopt;

    juce::StringArray seen;
    Layout layout;
    bool haveRoot = false;

    for (auto* c : xml.getChildIterator())
    {
        if (c->hasTagName ("FLOATING"))
            continue;

        if (haveRoot)
            return std::nullopt;

        auto node = nodeFromXml (*c, known, seen, 0);

        if (! node.has_value())
            return std::nullopt;

        layout.root = std::move (*node);
        haveRoot = true;
    }

    for (auto* f : xml.getChildWithTagNameIterator ("FLOATING"))
    {
        const auto id = f->getStringAttribute ("panel");
        const juce::Rectangle<int> bounds (f->getIntAttribute ("x"), f->getIntAttribute ("y"), f->getIntAttribute ("w"), f->getIntAttribute ("h"));

        if (known.contains (id) && ! seen.contains (id) && id != required && ! bounds.isEmpty())
        {
            layout.floating.push_back ({ id, bounds });
            seen.add (id);
        }
    }

    normalise (layout.root);

    if (! layout.isDocked (required))
        return std::nullopt;

    return layout;
}

//==============================================================================
Workspaces::Workspaces (std::vector<std::pair<juce::String, Layout>> builtIns)
{
    for (auto& [name, layout] : builtIns)
        list.push_back ({ name, layout, layout, true });

    jassert (! list.empty());
}

juce::StringArray Workspaces::names() const
{
    juce::StringArray out;

    for (auto& w : list)
        out.add (w.name);

    return out;
}

bool Workspaces::select (const juce::String& name)
{
    const auto index = names().indexOf (name);

    if (index < 0)
        return false;

    currentIndex = index;
    return true;
}

void Workspaces::update (const Layout& layout)
{
    list[(size_t) currentIndex].now = layout;
}

void Workspaces::reset()
{
    auto& w = list[(size_t) currentIndex];
    w.now = w.saved;
}

bool Workspaces::saveAs (const juce::String& rawName, const Layout& layout)
{
    const auto name = rawName.trim();

    if (name.isEmpty())
        return false;

    if (const auto index = names().indexOf (name); index >= 0)
    {
        if (list[(size_t) index].builtIn)
            return false;

        list[(size_t) index].saved = list[(size_t) index].now = layout;
        currentIndex = index;
        return true;
    }

    list.push_back ({ name, layout, layout, false });
    currentIndex = (int) list.size() - 1;
    return true;
}

bool Workspaces::remove (const juce::String& name)
{
    const auto index = names().indexOf (name);

    if (index < 0 || list[(size_t) index].builtIn)
        return false;

    list.erase (list.begin() + index);

    if (currentIndex == index)
        currentIndex = 0;
    else if (currentIndex > index)
        --currentIndex;

    return true;
}

std::unique_ptr<juce::XmlElement> Workspaces::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("WORKSPACES");
    xml->setAttribute ("current", current().name);

    for (auto& w : list)
    {
        auto* e = xml->createNewChildElement ("WORKSPACE");
        e->setAttribute ("name", w.name);
        e->setAttribute ("builtIn", w.builtIn);

        if (! w.builtIn)
            e->createNewChildElement ("SAVED")->addChildElement (w.saved.toXml().release());

        e->createNewChildElement ("NOW")->addChildElement (w.now.toXml().release());
    }

    return xml;
}

void Workspaces::restore (const juce::XmlElement& xml, const juce::StringArray& known, const juce::String& required)
{
    if (! xml.hasTagName ("WORKSPACES"))
        return;

    const auto read = [&] (const juce::XmlElement* e, const char* tag) -> std::optional<Layout>
    {
        if (auto* holder = e->getChildByName (tag))
            if (auto* dock = holder->getChildByName ("DOCK"))
                return Layout::fromXml (*dock, known, required);

        return std::nullopt;
    };

    for (auto* e : xml.getChildWithTagNameIterator ("WORKSPACE"))
    {
        const auto name = e->getStringAttribute ("name").trim();
        const auto index = names().indexOf (name);

        if (name.isEmpty())
            continue;

        if (index >= 0 && list[(size_t) index].builtIn)
        {
            if (auto now = read (e, "NOW"))
                list[(size_t) index].now = *now;

            continue;
        }

        if (e->getBoolAttribute ("builtIn"))
            continue;   // a built-in one this version doesn't have

        auto saved = read (e, "SAVED");

        if (! saved.has_value())
            continue;

        auto now = read (e, "NOW");
        Workspace w { name, *saved, now.value_or (*saved), false };

        if (index >= 0)
            list[(size_t) index] = std::move (w);
        else
            list.push_back (std::move (w));
    }

    select (xml.getStringAttribute ("current"));
}

//==============================================================================
namespace Palette
{
    const juce::Colour& background   = Theme::base;
    const juce::Colour& header       = Theme::surface;
    const juce::Colour& tab          = Theme::raised;
    const juce::Colour& tabFront     = Theme::hover;
    const juce::Colour& text         = Theme::text;
    const juce::Colour& dimText      = Theme::dimText;
    const juce::Colour& accent       = Theme::accent;
    const juce::Colour& bar          = Theme::inset;
}

/** One group: a strip of tabs over the panel in front. A target for dragged tabs, except in a
    floating window. */
class Area::TabGroup final : public juce::Component,
                             public juce::DragAndDropTarget
{
public:
    TabGroup (Area& a, std::vector<int> p, juce::StringArray t, int front, bool floats)
        : area (a), path (std::move (p)), tabs (std::move (t)), current (front), floating (floats)
    {
        showFront();
    }

    ~TabGroup() override
    {
        if (shown != nullptr && shown->getParentComponent() == this)
            removeChildComponent (shown);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);
        auto header = getLocalBounds().removeFromTop (tabHeight);
        g.setColour (Palette::header);
        g.fillRect (header);

        for (int i = 0; i < tabs.size(); ++i)
        {
            const auto r = tabBounds (i);
            const auto front = i == current;
            g.setColour (front ? Palette::tabFront : Palette::tab);
            g.fillRect (r.reduced (0, 0).withTrimmedRight (1));

            if (front)
            {
                g.setColour (Palette::accent);
                g.fillRect (r.withHeight (2).withTrimmedRight (1));
            }

            g.setColour (front ? Palette::text : Palette::dimText);
            g.setFont (juce::FontOptions (13.0f));
            auto textArea = r.reduced (10, 0);

            if (area.host.isMovable (tabs[i]))
            {
                const auto close = closeBounds (i);
                g.setColour ((hoveredClose == i ? Palette::text : Palette::dimText).withAlpha (front || hoveredClose == i ? 1.0f : 0.6f));
                const auto c = close.toFloat().reduced (4.0f);
                g.drawLine ({ c.getTopLeft(), c.getBottomRight() }, 1.4f);
                g.drawLine ({ c.getTopRight(), c.getBottomLeft() }, 1.4f);
                textArea.removeFromRight (close.getWidth());
                g.setColour (front ? Palette::text : Palette::dimText);
            }

            g.drawText (area.host.getTitle (tabs[i]), textArea, juce::Justification::centredLeft, true);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (! dropZone.has_value())
            return;

        g.setColour (Palette::accent.withAlpha (0.25f));
        g.fillRect (zoneBounds (*dropZone));
        g.setColour (Palette::accent);
        g.drawRect (zoneBounds (*dropZone), 2);
    }

    void resized() override
    {
        if (shown != nullptr)
            shown->setBounds (getLocalBounds().withTrimmedTop (tabHeight));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto i = tabAt (e.getPosition());
        const auto over = i >= 0 && area.host.isMovable (tabs[i]) && closeBounds (i).contains (e.getPosition()) ? i : -1;

        if (over != hoveredClose)
        {
            hoveredClose = over;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredClose >= 0)
        {
            hoveredClose = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = tabAt (e.getPosition());
        dragging = false;

        if (pressed < 0)
            return;

        const auto id = tabs[pressed];

        if (e.mods.isPopupMenu())
        {
            showMenu (id);
            return;
        }

        if (area.host.isMovable (id) && closeBounds (pressed).contains (e.getPosition()))
        {
            later ([id] (Area& a) { a.hide (id); });
            pressed = -1;
            return;
        }

        select (pressed);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressed < 0 || dragging || e.getDistanceFromDragStart() < 6 || ! area.host.isMovable (tabs[pressed]))
            return;

        dragging = true;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            const auto r = tabBounds (pressed);
            auto image = createComponentSnapshot (r, true);
            area.droppedOnTarget = false;
            container->startDragging (dragPrefix() + tabs[pressed], this, juce::ScaledImage (image), true,
                                      nullptr, &e.source);
        }
    }

    //==============================================================================
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        if (floating)
            return false;

        const auto id = draggedPanel (d);
        return id.isNotEmpty() && ! (tabs.size() == 1 && tabs[0] == id);
    }

    void itemDragMove (const SourceDetails& d) override
    {
        auto zone = zoneAt (d.localPosition);

        if (zone == Side::centre && tabs.contains (draggedPanel (d)))
            zone.reset();

        if (zone != dropZone)
        {
            dropZone = zone;
            repaint();
        }
    }

    void itemDragExit (const SourceDetails&) override
    {
        dropZone.reset();
        repaint();
    }

    void itemDropped (const SourceDetails& d) override
    {
        const auto zone = dropZone;
        dropZone.reset();
        repaint();
        area.droppedOnTarget = true;

        if (! zone.has_value() || tabs.isEmpty())
            return;

        const auto id = draggedPanel (d);
        const auto target = tabs[current] == id ? tabs[current == 0 ? 1 : 0] : tabs[current];
        const auto side = *zone;
        later ([id, target, side] (Area& a) { a.dropPanel (id, target, side); });
    }

    const juce::StringArray& getTabs() const      { return tabs; }

private:
    static juce::String draggedPanel (const SourceDetails& d)
    {
        const auto s = d.description.toString();
        return s.startsWith (dragPrefix()) ? s.fromFirstOccurrenceOf (dragPrefix(), false, false) : juce::String();
    }

    /** Runs something on the area after this callback has returned (it may delete this group). */
    void later (std::function<void (Area&)> action)
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Area> (&area), f = std::move (action)]
        {
            if (safe != nullptr)
                f (*safe);
        });
    }

    void showMenu (const juce::String& id)
    {
        // The menu's choices run once it has gone, by when this group may have been rebuilt.
        const auto run = [safe = juce::Component::SafePointer<Area> (&area)] (std::function<void (Area&)> action)
        {
            return [safe, f = std::move (action)]
            {
                juce::MessageManager::callAsync ([safe, f]
                {
                    if (safe != nullptr)
                        f (*safe);
                });
            };
        };

        juce::PopupMenu menu;
        const auto movable = area.host.isMovable (id);
        menu.addItem ("Close " + area.host.getTitle (id), movable, false, run ([id] (Area& a) { a.hide (id); }));

        if (! floating)
        {
            const auto bounds = juce::Rectangle<int> (juce::jmax (240, getWidth()), juce::jmax (180, getHeight()))
                                    .withPosition (getScreenPosition() + juce::Point<int> (30, 30));
            menu.addItem ("Float in a Window", movable, false, run ([id, bounds] (Area& a) { a.floatPanel (id, bounds); }));
        }

        juce::PopupMenu edges;

        for (auto [name, side] : { std::pair { "Left", Side::left }, { "Right", Side::right }, { "Top", Side::top }, { "Bottom", Side::bottom } })
            edges.addItem (name, movable, false, run ([id, s = side] (Area& a) { a.dockAtEdge (id, s); }));

        menu.addSubMenu ("Dock at the Window's Edge", edges, movable);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showFront()
    {
        if (shown != nullptr && shown->getParentComponent() == this)
            removeChildComponent (shown);

        shown = juce::isPositiveAndBelow (current, tabs.size()) ? area.host.getPanel (tabs[current]) : nullptr;

        if (shown != nullptr)
        {
            addAndMakeVisible (shown);
            resized();
        }
    }

    void select (int index)
    {
        if (index == current)
            return;

        current = index;

        if (! floating)
            if (auto* node = area.layout.nodeAt (path))
                node->current = index;

        showFront();
        repaint();
        area.host.layoutChanged();
    }

    juce::Rectangle<int> tabBounds (int index) const
    {
        int x = 0;

        for (int i = 0; i < tabs.size(); ++i)
        {
            const auto title = area.host.getTitle (tabs[i]);
            const auto w = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (13.0f)), title) + 20
                           + (area.host.isMovable (tabs[i]) ? 18 : 0);

            if (i == index)
                return { x, 0, w, tabHeight };

            x += w;
        }

        return {};
    }

    juce::Rectangle<int> closeBounds (int index) const
    {
        const auto r = tabBounds (index);
        return { r.getRight() - 24, (tabHeight - 16) / 2, 16, 16 };
    }

    int tabAt (juce::Point<int> p) const
    {
        if (p.y >= tabHeight)
            return -1;

        for (int i = 0; i < tabs.size(); ++i)
            if (tabBounds (i).contains (p))
                return i;

        return -1;
    }

    std::optional<Side> zoneAt (juce::Point<int> p) const
    {
        if (! getLocalBounds().contains (p))
            return std::nullopt;

        const auto r = getLocalBounds().toFloat();
        const auto fx = ((float) p.x - r.getX()) / juce::jmax (1.0f, r.getWidth());
        const auto fy = ((float) p.y - r.getY()) / juce::jmax (1.0f, r.getHeight());

        if (p.y < tabHeight || (fx > 0.3f && fx < 0.7f && fy > 0.3f && fy < 0.7f))
            return Side::centre;

        // The nearest edge.
        const auto horizontal = juce::jmin (fx, 1.0f - fx), vertical = juce::jmin (fy, 1.0f - fy);

        if (horizontal <= vertical)
            return fx < 0.5f ? Side::left : Side::right;

        return fy < 0.5f ? Side::top : Side::bottom;
    }

    juce::Rectangle<int> zoneBounds (Side side) const
    {
        auto r = getLocalBounds();

        switch (side)
        {
            case Side::left:   return r.removeFromLeft (r.getWidth() / 3);
            case Side::right:  return r.removeFromRight (r.getWidth() / 3);
            case Side::top:    return r.removeFromTop (r.getHeight() / 3);
            case Side::bottom: return r.removeFromBottom (r.getHeight() / 3);
            case Side::centre: return r.reduced (2);
        }

        return r;
    }

    Area& area;
    std::vector<int> path;
    juce::StringArray tabs;
    int current = 0;
    bool floating = false;
    juce::Component* shown = nullptr;
    int pressed = -1, hoveredClose = -1;
    bool dragging = false;
    std::optional<Side> dropZone;
};

//==============================================================================
/** Groups side by side or stacked, with bars between them to share the room. */
class Area::Split final : public juce::Component
{
public:
    Split (Area& a, std::vector<int> p, bool isColumn) : area (a), path (std::move (p)), column (isColumn) {}

    void add (std::unique_ptr<juce::Component> child)
    {
        addAndMakeVisible (*child);
        children.push_back (std::move (child));

        if (children.size() > 1)
        {
            auto bar = std::make_unique<Bar> (*this, (int) children.size() - 2);
            addAndMakeVisible (*bar);
            bars.push_back (std::move (bar));
        }
    }

    void resized() override
    {
        const auto* node = area.layout.nodeAt (path);

        if (node == nullptr || node->weights.size() != children.size())
            return;

        const auto total = (column ? getHeight() : getWidth()) - barSize * (int) bars.size();
        double sum = 0.0;

        for (auto w : node->weights)
            sum += w;

        double edge = 0.0;
        int start = 0;

        for (size_t i = 0; i < children.size(); ++i)
        {
            edge += node->weights[i] / juce::jmax (1.0e-9, sum) * total;
            const auto end = i + 1 == children.size() ? total : juce::roundToInt (edge);
            const auto offset = barSize * (int) i;
            const auto size = juce::jmax (0, end - start);

            if (column)
                children[i]->setBounds (0, start + offset, getWidth(), size);
            else
                children[i]->setBounds (start + offset, 0, size, getHeight());

            if (i < bars.size())
            {
                if (column)
                    bars[i]->setBounds (0, start + offset + size, getWidth(), barSize);
                else
                    bars[i]->setBounds (start + offset + size, 0, barSize, getHeight());
            }

            start = end;
        }
    }

private:
    class Bar final : public juce::Component
    {
    public:
        Bar (Split& s, int i) : split (s), index (i)
        {
            setMouseCursor (s.column ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::bar);

            if (isMouseOverOrDragging())
            {
                g.setColour (Palette::accent.withAlpha (0.6f));
                g.fillRect (split.column ? getLocalBounds().withSizeKeepingCentre (getWidth(), 2) : getLocalBounds().withSizeKeepingCentre (2, getHeight()));
            }
        }

        void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
        void mouseExit (const juce::MouseEvent&) override    { repaint(); }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            auto* node = split.area.layout.nodeAt (split.path);

            if (node == nullptr)
                return;

            auto& a = *split.children[(size_t) index];
            auto& b = *split.children[(size_t) index + 1];
            const auto pos = e.getEventRelativeTo (&split).position;
            const auto start = split.column ? a.getY() : a.getX();
            const auto end = split.column ? b.getBottom() : b.getRight();
            const auto room = end - start - barSize;

            if (room <= 2 * minimumPane)
                return;

            const auto at = juce::jlimit (minimumPane, room - minimumPane, juce::roundToInt (split.column ? pos.y : pos.x) - start - barSize / 2);
            const auto pairWeight = node->weights[(size_t) index] + node->weights[(size_t) index + 1];
            node->weights[(size_t) index] = pairWeight * at / room;
            node->weights[(size_t) index + 1] = pairWeight - node->weights[(size_t) index];
            split.resized();
        }

        void mouseUp (const juce::MouseEvent&) override       { split.area.host.layoutChanged(); repaint(); }

    private:
        Split& split;
        int index;
    };

    Area& area;
    std::vector<int> path;
    bool column;
    std::vector<std::unique_ptr<juce::Component>> children;
    std::vector<std::unique_ptr<Bar>> bars;
};

//==============================================================================
/** A floating panel: a window of its own, its tab dragged back onto the main window to dock it. */
class Area::FloatingWindow final : public juce::DocumentWindow
{
public:
    FloatingWindow (Area& a, const juce::String& id, juce::Rectangle<int> bounds)
        : DocumentWindow (a.host.getTitle (id), Palette::background, DocumentWindow::closeButton), area (a), panel (id)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new Content (a, id), false);
        setResizable (true, false);
        setResizeLimits (160, 100, 4000, 3000);

        juce::Array<juce::Rectangle<int>> screens;

        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
            screens.add (d.userBounds.getSmallestIntegerContainer());

        setBounds (WindowPlaces::reachable (bounds, screens));
        a.host.windowCreated (*this);
        ready = true;
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Area> (&area), id = panel]
        {
            if (safe != nullptr)
                safe->hide (id);
        });
    }

    void moved() override      { DocumentWindow::moved(); remember(); }
    void resized() override    { DocumentWindow::resized(); remember(); }

private:
    class Content final : public juce::Component,
                          public juce::DragAndDropContainer
    {
    public:
        Content (Area& a, const juce::String& id) : area (a), group (a, {}, juce::StringArray (id), 0, true)
        {
            addAndMakeVisible (group);
            setSize (360, 300);
        }

        void resized() override    { group.setBounds (getLocalBounds()); }

        void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override   { area.droppedOnTarget = false; }

    private:
        Area& area;
        TabGroup group;
    };

    void remember()
    {
        if (! ready)
            return;

        for (auto& f : area.layout.floating)
            if (f.panel == panel && f.bounds != getBounds())
            {
                f.bounds = getBounds();
                area.host.layoutChanged();
            }
    }

    Area& area;
    juce::String panel;
    bool ready = false;
};

//==============================================================================
Area::Area (Host& h) : host (h)
{
    setLayout (Layout {});
}

Area::~Area()
{
    windows.clear();
    view = nullptr;
}

void Area::setLayout (const Layout& l)
{
    layout = l;
    rebuild();
}

void Area::toggle (const juce::String& panel)
{
    if (layout.isShown (panel))
        hide (panel);
    else
        show (panel);
}

void Area::show (const juce::String& panel)
{
    if (layout.isDocked (panel))
    {
        if (! layout.isFront (panel))
        {
            layout.bringToFront (panel);
            changed();
        }

        return;
    }

    if (auto* w = getWindow (panel))
    {
        w->toFront (true);
        return;
    }

    const auto [target, side] = host.defaultPlace (panel);
    const auto share = host.defaultShare (panel);

    if (! layout.dock (panel, target, side, share))
        layout.dockAtEdge (panel, side == Side::centre ? Side::right : side, share);

    changed();
}

void Area::hide (const juce::String& panel)
{
    if (! layout.isShown (panel) || ! host.isMovable (panel))
        return;

    layout.remove (panel);
    changed();
}

void Area::dropPanel (const juce::String& panel, const juce::String& target, Side side)
{
    if (! host.isMovable (panel))
        return;

    if (layout.dock (panel, target, side))
        changed();
}

void Area::floatPanel (const juce::String& panel, juce::Rectangle<int> screenBounds)
{
    if (! host.isMovable (panel))
        return;

    layout.floatPanel (panel, screenBounds);
    changed();
}

juce::DocumentWindow* Area::getWindow (const juce::String& panel) const
{
    const auto it = windows.find (panel);
    return it != windows.end() ? it->second.get() : nullptr;
}

void Area::dockAtEdge (const juce::String& panel, Side side)
{
    if (! host.isMovable (panel))
        return;

    layout.dockAtEdge (panel, side);
    changed();
}

void Area::setWeights (const std::vector<int>& path, std::vector<double> weights)
{
    if (auto* node = layout.nodeAt (path); node != nullptr && ! node->isLeaf() && weights.size() == node->children.size())
    {
        node->weights = std::move (weights);
        rebuild();
    }
}

void Area::changed()
{
    rebuild();
    host.layoutChanged();
}

void Area::rebuild()
{
    std::function<std::unique_ptr<juce::Component> (const Node&, std::vector<int>)> build = [&] (const Node& node, std::vector<int> path)
        -> std::unique_ptr<juce::Component>
    {
        if (node.isLeaf())
            return std::make_unique<TabGroup> (*this, path, node.tabs, node.current, false);

        auto split = std::make_unique<Split> (*this, path, node.column);

        for (size_t i = 0; i < node.children.size(); ++i)
        {
            auto childPath = path;
            childPath.push_back ((int) i);
            split->add (build (node.children[i], childPath));
        }

        return split;
    };

    // The old groups let go of their panels before the new ones take them.
    view = nullptr;
    syncWindows();
    view = build (layout.root, {});
    addAndMakeVisible (*view);
    resized();

    const auto now = layout.panels();

    for (auto& id : shownBefore)
        if (! now.contains (id))
            host.panelHidden (id);

    shownBefore = now;
}

void Area::syncWindows()
{
    for (auto it = windows.begin(); it != windows.end();)
    {
        if (! layout.isFloating (it->first))
            it = windows.erase (it);
        else
            ++it;
    }

    for (auto& f : layout.floating)
        if (windows.find (f.panel) == windows.end())
            windows[f.panel] = std::make_unique<FloatingWindow> (*this, f.panel, f.bounds);
}

void Area::resized()
{
    if (view != nullptr)
        view->setBounds (getLocalBounds());
}

void Area::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
}

void Area::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto dropped = std::exchange (droppedOnTarget, false);
    const auto description = details.description.toString();

    if (dropped || ! description.startsWith (dragPrefix()))
        return;

    // Let go outside the window: the panel floats there.
    const auto mouse = juce::Desktop::getMousePosition();
    auto* top = getTopLevelComponent();

    if (top == nullptr || top->getScreenBounds().contains (mouse))
        return;

    const auto id = description.fromFirstOccurrenceOf (dragPrefix(), false, false);
    auto* source = details.sourceComponent.get();
    const auto width = source != nullptr ? juce::jlimit (240, 900, source->getWidth()) : 320;
    const auto height = source != nullptr ? juce::jlimit (180, 800, source->getHeight()) : 260;
    const juce::Rectangle<int> bounds (mouse.x - 40, mouse.y - 12, width, height);

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Area> (this), id, bounds]
    {
        if (safe != nullptr)
            safe->floatPanel (id, bounds);
    });
}
}
