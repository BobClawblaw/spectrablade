#include <JuceHeader.h>
#include "TestUtils.h"
#include "CommandAuditTests.h"

namespace
{
    using TestUtils::check;

    /** The main window is not in the test build, so the table is read from its source by the
        script, which prints what it found and exits 1 on a finding. */
    void testCommandTable()
    {
        const auto script = juce::File (SPECTRABLADE_SOURCE_DIR).getChildFile ("Scripts").getChildFile ("check_commands.py");
        check (script.existsAsFile(), "command table: the audit script is at Scripts/check_commands.py");

        juce::ChildProcess python;
        const auto started = python.start (juce::StringArray { "python3", "-I", script.getFullPathName() });
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
