#include "KeyShortcuts.h"

namespace KeyShortcuts
{
void store (juce::KeyPressMappingSet& mappings, juce::PropertiesFile& settings)
{
    const auto xml = mappings.createXml (true);

    if (xml == nullptr || xml->getNumChildElements() == 0)
        settings.removeValue (settingsKey);
    else
        settings.setValue (settingsKey, xml.get());

    settings.saveIfNeeded();
}

bool restore (juce::KeyPressMappingSet& mappings, juce::PropertiesFile& settings)
{
    const auto xml = settings.getXmlValue (settingsKey);
    return xml != nullptr && xml->getNumChildElements() > 0 && mappings.restoreFromXml (*xml);
}
}
