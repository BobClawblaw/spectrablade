#include <JuceHeader.h>
#include "../Source/Dock.h"
#include "TestUtils.h"
#include "DockTests.h"

namespace
{
    using TestUtils::check;
    using Dock::Layout;
    using Dock::Side;

    /** A layout as text: a leaf as its tabs joined by '|' (the one in front starred), a split as
        row(...) or col(...) with each child's share in percent. */
    juce::String describe (const Dock::Node& n)
    {
        if (n.isLeaf())
        {
            juce::StringArray t;

            for (int i = 0; i < n.tabs.size(); ++i)
                t.add (n.tabs[i] + (i == n.current && n.tabs.size() > 1 ? "*" : ""));

            return t.joinIntoString ("|");
        }

        double sum = 0.0;

        for (auto w : n.weights)
            sum += w;

        juce::StringArray parts;

        for (size_t i = 0; i < n.children.size(); ++i)
            parts.add (describe (n.children[i]) + ":" + juce::String (juce::roundToInt (100.0 * n.weights[i] / sum)));

        return juce::String (n.column ? "col(" : "row(") + parts.joinIntoString (" ") + ")";
    }

    juce::String describe (const Layout& l)
    {
        auto s = describe (l.root);

        for (auto& f : l.floating)
            s << " float:" << f.panel;

        return s;
    }

    void expect (const Layout& l, const juce::String& wanted, const juce::String& what)
    {
        const auto got = describe (l);
        check (got == wanted, what + (got == wanted ? juce::String() : ": got [" + got + "], wanted [" + wanted + "]"));
    }

    void testLayout()
    {
        auto l = Layout::single ("editor");
        check (l.dock ("rack", "editor", Side::right, 0.3), "Dock: a panel docks beside the editor");
        expect (l, "row(editor:70 rack:30)", "Dock: docking right makes a row, the new panel taking its share");

        l.dock ("selection", "rack", Side::bottom, 0.25);
        expect (l, "row(editor:70 col(rack:75 selection:25):30)", "Dock: docking below a panel in a row stacks the two");

        l.dock ("files", "editor", Side::left, 0.2);
        expect (l, "row(files:14 editor:56 col(rack:75 selection:25):30)",
                "Dock: docking beside a panel in a row of the same way joins the row, sharing that panel's room");

        l.dock ("markers", "rack", Side::centre);
        expect (l, "row(files:14 editor:56 col(rack|markers*:75 selection:25):30)", "Dock: docking in the middle adds a tab, in front");
        check (l.isFront ("markers") && ! l.isFront ("rack") && l.isDocked ("rack"), "Dock: the tab behind is still docked but not in front");

        l.bringToFront ("rack");
        check (l.isFront ("rack"), "Dock: a tab is brought to the front");

        l.remove ("selection");
        expect (l, "row(files:14 editor:56 rack*|markers:30)", "Dock: closing a panel collapses the stack it leaves with one group");

        l.remove ("rack");
        expect (l, "row(files:14 editor:56 markers:30)", "Dock: closing the front tab brings the next one forward");

        check (! l.dock ("editor", "editor", Side::left) && ! l.dock ("x", "nothing", Side::left),
               "Dock: a panel can't dock beside itself or beside a panel that isn't docked");

        l.dock ("markers", "files", Side::bottom, 0.5);
        expect (l, "row(col(files:50 markers:50):20 editor:80)", "Dock: moving a docked panel takes it out of its old place first");

        l.floatPanel ("files", { 100, 100, 300, 200 });
        expect (l, "row(markers:20 editor:80) float:files", "Dock: a floated panel leaves the dock");
        check (l.isFloating ("files") && l.isShown ("files") && ! l.isDocked ("files"), "Dock: a floating panel is shown but not docked");

        l.dock ("files", "editor", Side::top, 0.25);
        expect (l, "row(markers:20 col(files:25 editor:75):80)", "Dock: docking a floating panel takes it out of its window");
        check (l.floating.empty(), "Dock: no window is left for it");

        l.dockAtEdge ("mixer", Side::bottom, 0.3);
        expect (l, "col(row(markers:20 col(files:25 editor:75):80):70 mixer:30)", "Dock: docking at an edge spans the whole width");
        l.dockAtEdge ("levels", Side::bottom, 0.1);
        expect (l, "col(row(markers:20 col(files:25 editor:75):80):63 mixer:27 levels:10)", "Dock: a second panel at that edge joins the column");

        check (l.panels().joinIntoString (",") == "markers,files,editor,mixer,levels", "Dock: the panels are listed in the order the tree has them");

        // A split left with a split of the same way inside merges with it.
        auto m = Layout::single ("a");
        m.dock ("b", "a", Side::right, 0.5);
        m.dock ("c", "b", Side::bottom, 0.5);
        m.dock ("d", "a", Side::bottom, 0.5);
        expect (m, "row(col(a:50 d:50):50 col(b:50 c:50):50)", "Dock: four panels in two columns");
        m.remove ("b");
        m.remove ("c");
        expect (m, "col(a:50 d:50)", "Dock: closing a whole column leaves the other as the root");
    }

    void testXml()
    {
        auto l = Layout::single ("editor");
        l.dock ("rack", "editor", Side::right, 0.3);
        l.dock ("markers", "rack", Side::centre);
        l.dock ("selection", "rack", Side::bottom, 0.2);
        l.floatPanel ("mixer", { 40, 50, 600, 300 });

        const juce::StringArray known { "editor", "rack", "markers", "selection", "mixer" };
        const auto xml = l.toXml();
        const auto back = Layout::fromXml (*xml, known, "editor");
        check (back.has_value() && *back == l, "Dock: a layout reads back as it was written");

        const auto parsed = juce::parseXML (xml->toString());
        const auto again = parsed != nullptr ? Layout::fromXml (*parsed, known, "editor") : std::nullopt;
        check (again.has_value() && describe (*again) == describe (l) && again->floating[0].bounds == juce::Rectangle<int> (40, 50, 600, 300),
               "Dock: and through its text, the floating window's place included");

        const auto fewer = Layout::fromXml (*xml, { "editor", "rack", "selection" }, "editor");
        check (fewer.has_value() && describe (*fewer) == "row(editor:70 col(rack:80 selection:20):30)",
               "Dock: panels this version doesn't have are left out, and the groups they leave tidied");

        check (! Layout::fromXml (*xml, { "rack", "markers" }, "editor").has_value(), "Dock: a layout without the editor is refused");

        auto doubled = juce::parseXML ("<DOCK><SPLIT column=\"0\"><TABS current=\"5\"><PANEL id=\"editor\"/><PANEL id=\"rack\"/></TABS>"
                                       "<TABS weight=\"-3\"><PANEL id=\"rack\"/><PANEL id=\"mixer\"/></TABS></SPLIT></DOCK>");
        const auto cleaned = Layout::fromXml (*doubled, known, "editor");
        check (cleaned.has_value() && describe (*cleaned) == "row(editor|rack*:50 mixer:50)",
               "Dock: a panel named twice is kept once, a tab out of range and a weight below zero are put right");

        auto broken = juce::parseXML ("<DOCK><SPLIT><SOMETHING/></SPLIT></DOCK>");
        check (! Layout::fromXml (*broken, known, "editor").has_value(), "Dock: an unreadable layout is refused");
    }

    void testWorkspaces()
    {
        const juce::StringArray known { "editor", "rack", "files", "mixer" };
        auto classic = Layout::single ("editor");
        classic.dock ("rack", "editor", Side::right);
        auto mixing = Layout::single ("editor");
        mixing.dock ("mixer", "editor", Side::bottom);

        Dock::Workspaces w ({ { "Classic", classic }, { "Mixing", mixing } });
        check (w.current().name == "Classic" && w.names().joinIntoString (",") == "Classic,Mixing", "Workspaces: the built-in ones, the first current");

        auto changed = classic;
        changed.dock ("files", "editor", Side::left);
        w.update (changed);
        check (w.current().now == changed && w.current().saved == classic, "Workspaces: changes are kept as the workspace's layout now, apart from how it was saved");

        check (w.select ("Mixing") && w.current().now == mixing, "Workspaces: switching shows the other workspace as it was left");
        check (w.select ("Classic") && w.current().now == changed, "Workspaces: and switching back shows the changes still there");

        w.reset();
        check (w.current().now == classic, "Workspaces: Reset puts the saved layout back");

        check (w.saveAs ("  Podcast ", changed) && w.current().name == "Podcast" && w.current().saved == changed && ! w.current().builtIn,
               "Workspaces: Save As makes a new workspace of the layout, named without spaces at the ends, and switches to it");
        check (! w.saveAs ("Mixing", changed) && ! w.saveAs ("  ", changed), "Workspaces: a built-in one's name or no name is refused");
        check (w.saveAs ("Podcast", classic) && w.all().size() == 3 && w.current().saved == classic, "Workspaces: saving under one of the person's own names replaces it");

        auto onlyMixer = Layout::single ("editor");
        onlyMixer.dock ("mixer", "editor", Side::top);
        w.select ("Mixing");
        w.update (onlyMixer);
        w.select ("Podcast");
        const auto xml = w.toXml();

        Dock::Workspaces back ({ { "Classic", classic }, { "Mixing", mixing } });
        back.restore (*xml, known, "editor");
        check (back.names().joinIntoString (",") == "Classic,Mixing,Podcast" && back.current().name == "Podcast"
                   && back.all()[1].now == onlyMixer && back.all()[1].saved == mixing && back.all()[2].saved == classic,
               "Workspaces: restored as saved, the current one and each layout now included, built-ins saved as this version has them");

        check (! back.remove ("Classic") && back.remove ("Podcast") && back.current().name == "Classic" && back.all().size() == 2,
               "Workspaces: a built-in one can't be deleted; deleting the current one goes back to the first");
    }

    /** A dock with plain panels, each a component of its own. */
    struct TestHost final : Dock::Host
    {
        std::map<juce::String, std::unique_ptr<juce::Component>> panels;
        juce::StringArray hidden;
        int changes = 0;

        juce::Component* getPanel (const juce::String& id) override
        {
            auto& p = panels[id];

            if (p == nullptr)
                p = std::make_unique<juce::Component> (id);

            return p.get();
        }

        void panelHidden (const juce::String& id) override             { hidden.add (id); panels.erase (id); }
        juce::String getTitle (const juce::String& id) override         { return id.toUpperCase(); }
        bool isMovable (const juce::String& id) override                { return id != "editor"; }
        void layoutChanged() override                                   { ++changes; }
        std::pair<juce::String, Side> defaultPlace (const juce::String& id) override
        {
            return id == "mixer" ? std::pair { juce::String ("editor"), Side::bottom } : std::pair { juce::String ("rack"), Side::centre };
        }
    };

    void testArea()
    {
        TestHost host;
        Dock::Area area (host);
        auto l = Layout::single ("editor");
        l.dock ("rack", "editor", Side::right, 0.2);
        area.setLayout (l);
        area.setSize (1006, 600);

        auto* editor = host.panels["editor"].get();
        auto* rack = host.panels["rack"].get();
        const auto editorBounds = editor->getBoundsInParent();
        check (editor->getParentComponent() != nullptr && rack->getParentComponent() != nullptr,
               "Dock area: each docked panel is put in the dock");
        check (editorBounds.getY() == Dock::Area::tabHeight && editor->getHeight() == 600 - Dock::Area::tabHeight
                   && editor->getParentComponent()->getWidth() == 800 && rack->getParentComponent()->getWidth() == 200
                   && rack->getParentComponent()->getX() == 800 + Dock::Area::barSize,
               "Dock area: the groups share the width by their weights with a bar between, each panel under its tabs (editor group "
                   + juce::String (editor->getParentComponent()->getWidth()) + " wide)");

        area.show ("markers");
        check (area.getLayout().isFront ("markers") && host.panels["markers"]->getParentComponent() != nullptr
                   && rack->getParentComponent() == nullptr && host.panels.count ("rack") == 1,
               "Dock area: showing a panel puts it where it goes, here in front of the rack's tabs; the rack waits behind it");

        area.show ("rack");
        check (area.getLayout().isFront ("rack") && rack->getParentComponent() != nullptr, "Dock area: showing a docked panel brings its tab to the front");

        area.show ("mixer");
        check (describe (area.getLayout()) == "row(col(editor:70 mixer:30):80 rack*|markers:20)", "Dock area: the mixer goes under the editor, by default");

        area.toggle ("mixer");
        check (! area.isShown ("mixer") && host.hidden.contains ("mixer") && host.panels.count ("mixer") == 0,
               "Dock area: toggling hides a shown panel, and the host is told it can let it go");

        area.hide ("editor");
        check (area.isShown ("editor"), "Dock area: the editor can't be closed");

        area.dropPanel ("markers", "editor", Side::left);
        check (describe (area.getLayout()) == "row(markers:24 editor:56 rack:20)", "Dock area: a dropped tab docks where it was dropped");
        check (host.panels["markers"]->getParentComponent() != nullptr && host.panels["markers"]->getParentComponent()->getX() == 0,
               "Dock area: and shows there");

        area.setWeights ({}, { 1.0, 1.0, 3.0 });
        check (rack->getParentComponent()->getWidth() == 596 && host.panels["markers"]->getParentComponent()->getWidth() == 199,
               "Dock area: weights share the room, " + juce::String (rack->getParentComponent()->getWidth()) + " wide for the larger");

        const auto before = host.changes;
        area.floatPanel ("markers", { 200, 200, 300, 240 });
        auto* window = area.getWindow ("markers");
        check (window != nullptr && window->isVisible() && host.panels["markers"]->isShowing() && host.changes > before,
               "Dock area: a floated panel shows in a window of its own, and the host hears of the change");
        check (describe (area.getLayout()) == "row(editor:25 rack:75) float:markers", "Dock area: the dock closes up after it");

        area.dropPanel ("markers", "rack", Side::bottom);
        check (area.getWindow ("markers") == nullptr && area.getLayout().floating.empty() && host.panels["markers"]->getParentComponent() != nullptr,
               "Dock area: dropping a floating panel's tab on the dock docks it and closes its window");

        area.floatPanel ("editor", { 0, 0, 100, 100 });
        check (area.getLayout().isDocked ("editor"), "Dock area: the editor can't be floated");
    }

    juce::MouseEvent click (juce::Component& c, juce::Point<int> at, int clicks)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                 now, at.toFloat(), now, clicks, false);
    }

    /** Runs the message loop until a condition holds, or a second has gone. */
    void settle (std::function<bool()> done)
    {
        for (int i = 0; i < 50 && ! done(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }

    void testMaximise()
    {
        TestHost host;
        Dock::Area area (host);
        auto l = Layout::single ("editor");
        l.dock ("rack", "editor", Side::right, 0.25);
        l.dock ("markers", "rack", Side::centre);
        l.dock ("mixer", "editor", Side::bottom, 0.3);
        area.setLayout (l);
        area.setSize (1000, 600);
        const auto saved = area.getLayout();

        area.maximise ("rack");
        auto* group = host.panels["markers"]->getParentComponent();
        check (area.getMaximised() == "rack" && area.getLayout() == saved && group != nullptr && group->getBounds() == area.getLocalBounds()
                   && host.panels["editor"]->getParentComponent() == nullptr && host.panels["mixer"]->getParentComponent() == nullptr
                   && host.hidden.isEmpty(),
               "Dock maximise: a panel's group fills the dock alone, with its other tabs, and the layout itself is unchanged");

        area.restore();
        check (area.getMaximised().isEmpty() && host.panels["editor"]->getParentComponent() != nullptr
                   && host.panels["mixer"]->getParentComponent() != nullptr && host.panels["editor"]->getParentComponent()->getWidth() < 1000,
               "Dock maximise: restoring shows every group again where it was");

        area.maximise ("editor");
        area.hide ("mixer");
        check (area.getMaximised().isEmpty() && host.panels["markers"]->getParentComponent() != nullptr,
               "Dock maximise: a change to the layout shows it all again");

        area.maximise ("notes");
        check (area.getMaximised().isEmpty(), "Dock maximise: a panel that isn't docked can't be maximised");

        // Double-clicking a tab maximises its group, and again puts it back.
        auto* editorGroup = host.panels["editor"]->getParentComponent();
        editorGroup->mouseDoubleClick (click (*editorGroup, { 20, Dock::Area::tabHeight / 2 }, 2));
        settle ([&] { return area.getMaximised().isNotEmpty(); });
        const auto maximised = area.getMaximised() == "editor" && host.panels["editor"]->getParentComponent()->getWidth() == 1000;
        editorGroup = host.panels["editor"]->getParentComponent();
        editorGroup->mouseDoubleClick (click (*editorGroup, { 20, Dock::Area::tabHeight / 2 }, 2));
        settle ([&] { return area.getMaximised().isEmpty(); });
        check (maximised && area.getMaximised().isEmpty() && host.panels["markers"]->getParentComponent() != nullptr,
               "Dock maximise: double-clicking a tab maximises its group, and double-clicking again restores them all");

        // A double-click under the tabs, in the panel, does nothing.
        editorGroup = host.panels["editor"]->getParentComponent();
        editorGroup->mouseDoubleClick (click (*editorGroup, { 20, Dock::Area::tabHeight + 40 }, 2));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (area.getMaximised().isEmpty(), "Dock maximise: a double-click below the tabs leaves the layout alone");

        area.maximise ("rack");
        area.setLayout (saved);
        check (area.getMaximised().isEmpty(), "Dock maximise: switching layouts (a workspace) shows it all");
    }
}

void runDockTests()
{
    testMaximise();
    testLayout();
    testXml();
    testWorkspaces();
    testArea();
}
