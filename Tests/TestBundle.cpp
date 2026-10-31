// Plug-in bundles that misbehave on purpose, for the scan tests. Each is built as a .vst3:
//   IntelOnlyBundle  Intel code only: the scan must say it has no code for Apple silicon.
//   HangingBundle    never returns from GetPluginFactory: the scan must give up on it.
//   CrashingBundle   crashes in GetPluginFactory: the scan must mark it not working.
#include <thread>

#define TEST_BUNDLE_EXPORT extern "C" __attribute__ ((visibility ("default")))

// VST3 hosts on macOS call these before and after using the bundle.
TEST_BUNDLE_EXPORT bool bundleEntry (void*);
TEST_BUNDLE_EXPORT bool bundleExit();
TEST_BUNDLE_EXPORT void* GetPluginFactory();

extern "C" bool bundleEntry (void*)  { return true; }
extern "C" bool bundleExit()         { return true; }

extern "C" void* GetPluginFactory()
{
   #if defined (TEST_BUNDLE_HANGS)
    for (;;)
        std::this_thread::sleep_for (std::chrono::seconds (1));
   #elif defined (TEST_BUNDLE_CRASHES)
    *static_cast<volatile int*> (nullptr) = 1;
   #endif
    return nullptr;
}
