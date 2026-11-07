#include "SoundCategories.h"
#include "BinaryData.h"

namespace Ucs
{
    namespace
    {
        struct Table
        {
            std::vector<Entry> entries;
            juce::StringArray categoryNames;
            std::unordered_map<juce::String, size_t> exact, folded, named;

            Table()
            {
                const auto text = juce::String::fromUTF8 (BinaryData::ucs_v8_2_1_tsv, BinaryData::ucs_v8_2_1_tsvSize);

                for (const auto& line : juce::StringArray::fromLines (text))
                {
                    if (line.isEmpty() || line.startsWithChar ('#'))
                        continue;

                    juce::StringArray fields;   // every field, empty ones too

                    for (auto rest = line;; )
                    {
                        const auto tab = rest.indexOfChar ('\t');
                        fields.add (tab < 0 ? rest : rest.substring (0, tab));

                        if (tab < 0)
                            break;

                        rest = rest.substring (tab + 1);
                    }

                    if (fields.size() < 4)
                        continue;

                    Entry e { fields[0], fields[1], fields[2], fields[3], fields[4], {} };

                    for (auto synonym : juce::StringArray::fromTokens (fields[5], ",", {}))
                        if ((synonym = synonym.trim()).isNotEmpty())
                            e.synonyms.add (synonym);

                    const auto at = entries.size();
                    exact.emplace (e.catId, at);
                    folded.emplace (e.catId.toLowerCase(), at);
                    named.emplace ((e.category + "\t" + e.subCategory).toLowerCase(), at);
                    categoryNames.addIfNotAlreadyThere (e.category);
                    entries.push_back (std::move (e));
                }
            }
        };

        const Table& table()
        {
            static const Table t;
            return t;
        }

        const Entry* at (const std::unordered_map<juce::String, size_t>& map, const juce::String& key)
        {
            const auto it = map.find (key);
            return it != map.end() ? &table().entries[it->second] : nullptr;
        }
    }

    const std::vector<Entry>& list()                { return table().entries; }
    juce::StringArray categories()                  { return table().categoryNames; }

    std::vector<const Entry*> subCategoriesOf (const juce::String& category)
    {
        std::vector<const Entry*> found;

        for (const auto& e : table().entries)
            if (e.category.equalsIgnoreCase (category))
                found.push_back (&e);

        return found;
    }

    const Entry* byCatId (const juce::String& id)
    {
        const auto trimmed = id.trim();

        if (const auto* e = at (table().exact, trimmed))
            return e;

        return at (table().folded, trimmed.toLowerCase());
    }

    const Entry* byNames (const juce::String& category, const juce::String& subCategory)
    {
        return at (table().named, (category.trim() + "\t" + subCategory.trim()).toLowerCase());
    }

    const Entry* fromName (const juce::String& name)
    {
        const auto underscore = name.indexOfChar ('_');
        return underscore > 0 ? byCatId (name.substring (0, underscore)) : nullptr;
    }

    const Entry* fromIXml (const juce::String& xml)
    {
        const auto doc = juce::parseXML (xml);

        if (doc == nullptr)
            return nullptr;

        const auto textOf = [] (const juce::XmlElement* parent, const char* tag)
        {
            const auto* child = parent != nullptr ? parent->getChildByName (tag) : nullptr;
            return child != nullptr ? child->getAllSubText().trim() : juce::String();
        };

        const auto* user = doc->getChildByName ("USER");
        const auto* aswg = doc->getChildByName ("ASWG");

        if (const auto* e = byCatId (textOf (user, "CATID")))
            return e;

        if (const auto* e = byCatId (textOf (aswg, "catId")))
            return e;

        return byNames (textOf (user, "CATEGORY"), textOf (user, "SUBCATEGORY"));
    }
}
