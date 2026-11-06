#pragma once

#include <JuceHeader.h>

/** Panels docked around the editor: side by side, stacked, or as tabs in one group, each group's
    share of the room dragged with the bars between them; or floating in windows of their own.
    Layouts are kept as named workspaces. */
namespace Dock
{
    enum class Side { left, right, top, bottom, centre };

    /** A group of tabs (a leaf), or a split holding its children in a row or a column, each with a
        weight: its share of the room. */
    struct Node
    {
        juce::StringArray tabs;              // leaves: the panels, in tab order
        int current = 0;                     // leaves: the tab showing
        std::vector<Node> children;          // splits
        std::vector<double> weights;         // splits: one per child
        bool column = false;                 // splits: stacked top to bottom rather than left to right

        bool isLeaf() const                  { return children.empty(); }
        bool operator== (const Node&) const = default;
    };

    struct Floating
    {
        juce::String panel;
        juce::Rectangle<int> bounds;         // on the screen
        bool operator== (const Floating&) const = default;
    };

    /** Where each panel is. A panel is docked (in one of the root's leaves), floating, or neither
        (hidden). */
    class Layout
    {
    public:
        Node root;
        std::vector<Floating> floating;

        /** One group holding one panel. */
        static Layout single (const juce::String& panel);

        bool isDocked (const juce::String& panel) const;
        bool isFloating (const juce::String& panel) const;
        bool isShown (const juce::String& panel) const      { return isDocked (panel) || isFloating (panel); }

        /** Whether the panel is docked and its tab is the one showing in its group. */
        bool isFront (const juce::String& panel) const;

        /** Every panel docked or floating: docked ones in the order a walk of the tree meets them. */
        juce::StringArray panels() const;

        /** Takes a panel out, wherever it is. Empty groups go, and a split left with one child is
            replaced by it. */
        void remove (const juce::String& panel);

        /** Docks the panel beside the group holding target (or as a tab in it, for centre), taking
            `share` of that group's room. Fails (returning false) when target isn't docked or is
            the panel itself. */
        bool dock (const juce::String& panel, const juce::String& target, Side, double share = 0.3);

        /** Docks the panel along an edge of the whole dock, taking `share` of it. */
        void dockAtEdge (const juce::String& panel, Side, double share = 0.25);

        /** Floats the panel in a window of its own. */
        void floatPanel (const juce::String& panel, juce::Rectangle<int> screenBounds);

        /** Brings a docked panel's tab to the front of its group. */
        void bringToFront (const juce::String& panel);

        /** The node at a path of child indexes from the root, or null if there is none. */
        Node* nodeAt (const std::vector<int>& path);
        const Node* nodeAt (const std::vector<int>& path) const;

        /** The path to the leaf holding the panel, or none. */
        std::optional<std::vector<int>> pathTo (const juce::String& panel) const;

        std::unique_ptr<juce::XmlElement> toXml() const;

        /** Reads a layout, keeping only the panels in `known` (each once); returns none when it
            can't be read or `required` isn't docked in it. */
        static std::optional<Layout> fromXml (const juce::XmlElement&, const juce::StringArray& known, const juce::String& required);

        bool operator== (const Layout&) const = default;
    };

    //==============================================================================
    /** Named layouts. Each has the layout it was saved with and the one it has now (as the person
        last left it); Reset puts back the saved one. The built-in ones can be reset but not
        deleted, and a name is used once. */
    class Workspaces
    {
    public:
        struct Workspace
        {
            juce::String name;
            Layout saved, now;
            bool builtIn = false;
        };

        /** Starts with the built-in workspaces; the first is the current one. */
        explicit Workspaces (std::vector<std::pair<juce::String, Layout>> builtIns);

        const std::vector<Workspace>& all() const      { return list; }
        const Workspace& current() const               { return list[(size_t) currentIndex]; }
        int getCurrentIndex() const                    { return currentIndex; }
        juce::StringArray names() const;

        /** Switches to a workspace by name; false if there is none. */
        bool select (const juce::String& name);

        /** Records the current workspace's layout as it is now. */
        void update (const Layout&);

        /** Puts the current workspace's saved layout back. */
        void reset();

        /** Saves the current layout as a new workspace (or over one of the person's own with that
            name) and switches to it. False for an empty name or a built-in one's. */
        bool saveAs (const juce::String& name, const Layout&);

        /** Deletes one of the person's own workspaces; the current one becomes the first if it
            was the one deleted. */
        bool remove (const juce::String& name);

        /** The person's workspaces and every workspace's current layout, to keep in the settings. */
        std::unique_ptr<juce::XmlElement> toXml() const;

        /** Restores what toXml wrote: layouts read with Layout::fromXml (those that can't be read
            are left as they were), built-ins kept and new ones added. */
        void restore (const juce::XmlElement&, const juce::StringArray& known, const juce::String& required);

    private:
        std::vector<Workspace> list;
        int currentIndex = 0;
    };

    //==============================================================================
    class Area;

    /** What the dock asks of its owner: each panel's component and title. */
    struct Host
    {
        virtual ~Host() = default;

        /** The panel's component, made if need be. The dock shows it and takes it away, but
            doesn't own it. */
        virtual juce::Component* getPanel (const juce::String& id) = 0;

        /** Called when a panel is no longer shown anywhere; its component may be deleted. */
        virtual void panelHidden (const juce::String&)               {}

        virtual juce::String getTitle (const juce::String& id) = 0;

        /** Whether a panel can be closed or floated (the editor can't). */
        virtual bool isMovable (const juce::String&)                 { return true; }

        /** The layout changed (by the person, or by show and hide). */
        virtual void layoutChanged()                                 {}

        /** A floating panel's window was made (to give it the app's keys, say). */
        virtual void windowCreated (juce::DocumentWindow&)            {}

        /** The share of the room a panel shown again takes where it goes. */
        virtual double defaultShare (const juce::String&)             { return 0.3; }

        /** Where a panel shown again goes: beside which panel, on which side (beside no panel: along
            that edge of the whole dock). */
        virtual std::pair<juce::String, Side> defaultPlace (const juce::String& id) = 0;
    };

    /** The docked panels, laid out by a Layout, with the floating ones in windows of their own.
        Drag a tab onto another group's edge to dock it there, onto its middle to join its tabs,
        or out of the main window to float it; drag a floating panel's tab back in to dock it. */
    class Area final : public juce::Component,
                       public juce::DragAndDropContainer
    {
    public:
        explicit Area (Host&);
        ~Area() override;

        void setLayout (const Layout&);
        const Layout& getLayout() const               { return layout; }

        bool isShown (const juce::String& panel) const { return layout.isShown (panel); }

        /** Shows a panel where it goes by default (or brings it to the front); hides one that is shown. */
        void toggle (const juce::String& panel);
        void show (const juce::String& panel);
        void hide (const juce::String& panel);

        /** Moves a panel to a group's side (or into its tabs), as dropping its tab does. */
        void dropPanel (const juce::String& panel, const juce::String& target, Side);

        /** Docks a panel along an edge of the whole dock, as its tab's menu does. */
        void dockAtEdge (const juce::String& panel, Side);

        /** Floats a panel at a place on the screen, as dragging its tab out does. */
        void floatPanel (const juce::String& panel, juce::Rectangle<int> screenBounds);

        /** The floating window holding a panel, if it floats. */
        juce::DocumentWindow* getWindow (const juce::String& panel) const;

        /** Shows the group holding a docked panel alone, filling the dock, as double-clicking its
            tab does; restore() (or any change to the layout) shows them all again. The layout
            itself is not changed. */
        void maximise (const juce::String& panel);
        void restore();
        juce::String getMaximised() const              { return maximised; }

        /** Sets the weights of the split at a path, as dragging its bars does. */
        void setWeights (const std::vector<int>& path, std::vector<double> weights);

        void resized() override;
        void paint (juce::Graphics&) override;

        static constexpr int tabHeight = 22;
        static constexpr int barSize = 6;
        static constexpr int minimumPane = 60;

        // Used by the parts of the dock.
        Host& host;
        void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;
        bool droppedOnTarget = false;
        static juce::String dragPrefix()             { return "dockPanel:"; }

        class TabGroup;
        class Split;
        class FloatingWindow;

    private:
        void rebuild();
        void syncWindows();
        void changed();

        Layout layout;
        juce::String maximised;
        std::unique_ptr<juce::Component> view;
        std::map<juce::String, std::unique_ptr<FloatingWindow>> windows;
        juce::StringArray shownBefore;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Area)
    };
}
