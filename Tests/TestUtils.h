#pragma once

#include <JuceHeader.h>

namespace TestUtils
{
    inline int failures = 0;

    inline void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "PASS  " : "FAIL  ") << what << std::endl;
        failures += ok ? 0 : 1;
    }

    /** Pumps the message loop until the predicate holds or the timeout passes. */
    inline bool runUntil (std::function<bool()> done, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! done() && juce::Time::getMillisecondCounter() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        return done();
    }
}
