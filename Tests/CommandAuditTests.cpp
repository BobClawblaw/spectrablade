#include <JuceHeader.h>
#include "TestUtils.h"
#include "CommandAuditTests.h"
#include "../Source/KeyShortcuts.h"

namespace
{
    using TestUtils::check;

    /** The main window is not in the test build, so the table is read from its source by the
        script, which prints what it found and exits 1 on a finding. */
    void testCommandTable()
    {
        const auto script = juce::File (SPECTRABLADE_SOURCE_DIR).getChildFile ("Scripts").getChildFile ("check_commands.py");
        check (script.existsAsFile(), "command table: the audit script is at Scripts/check_commands.py");

        // The panels' fixed keys, as the shortcuts window lists them, are each a row of the
        // document's "Inside panels" table: "Panel: what" and the keys, written alike.
        const auto doc = juce::File (SPECTRABLADE_SOURCE_DIR).getChildFile ("docs").getChildFile ("SHORTCUTS.md").loadFileAsString();
        const auto panels = doc.fromFirstOccurrenceOf ("## Inside panels", false, false);
        juce::StringArray missing;

        for (auto& k : KeyShortcuts::panelKeys())
        {
            auto found = false;

            for (auto& line : juce::StringArray::fromLines (panels))
            {
                if (! line.startsWith ("|"))
                    continue;

                const auto cells = juce::StringArray::fromTokens (line, "|", "");
                const auto what = cells.size() > 1 ? cells[1].trim() : juce::String();
                const auto keys = cells.size() > 2 ? cells[2].removeCharacters ("`").trim() : juce::String();
                found = found || (what.equalsIgnoreCase (k.panel + ": " + k.action) && keys == k.keys);
            }

            if (! found)
                missing.add (k.panel + ": " + k.action + " (" + k.keys + ")");
        }

        check (missing.isEmpty() && KeyShortcuts::panelKeys().size() >= 18,
               "command table: every panel's fixed key the shortcuts window lists is in docs/SHORTCUTS.md under Inside panels, written alike"
                   + (missing.isEmpty() ? juce::String() : " (missing: " + missing.joinIntoString ("; ") + ")"));

        juce::ChildProcess python;
       #if JUCE_WINDOWS
        // python.org's installer names it python; python3 on Windows is often the Store's stand-in.
        const auto started = python.start (juce::StringArray { "python", "-I", script.getFullPathName() });
       #else
        const auto started = python.start (juce::StringArray { "python3", "-I", script.getFullPathName() });
       #endif
        const auto output = started ? python.readAllProcessOutput().trim() : juce::String();
        const auto finished = started && python.waitForProcessToFinish (60000);
        const auto exitCode = finished ? (int) python.getExitCode() : -1;
        juce::StringArray lines;
        lines.addLines (output);

        check (started && finished && exitCode == 0 && lines.size() == 1,
               started ? (finished ? lines.joinIntoString ("; ") + (exitCode == 0 ? juce::String() : " (exit " + juce::String (exitCode) + ")")
                                   : juce::String ("command table: the audit script did not finish in a minute"))
                       : juce::String ("command table: python3 could not be started (the audit needs the command line tools' Python)"));
    }
}

void runCommandAuditTests()
{
    testCommandTable();
}
