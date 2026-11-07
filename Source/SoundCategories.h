#pragma once

#include <JuceHeader.h>

/** The Universal Category System's categories (v8.2.1, universalcategorysystem.com, a public
    domain initiative), bundled with the app: 82 categories of 753 subcategories, each with its
    CatID (e.g. DOORWood for DOORS / WOOD), CatShort, explanation and synonyms.

    A sound's category comes from its name when that follows UCS (CatID_FXName_CreatorID_SourceID:
    the part before the first underscore is a CatID), or from its iXML metadata (USER CATID, the
    ASWG catId, or USER CATEGORY and SUBCATEGORY), or is set by hand. */
namespace Ucs
{
    struct Entry
    {
        juce::String category, subCategory, catId, catShort, explanation;
        juce::StringArray synonyms;

        juce::String describe() const      { return category + " / " + subCategory; }
    };

    /** Every subcategory, in the list's order (by category, then subcategory). */
    const std::vector<Entry>& list();

    /** The 82 categories, in order. */
    juce::StringArray categories();

    /** The subcategories of a category, in order. */
    std::vector<const Entry*> subCategoriesOf (const juce::String& category);

    /** The entry with this CatID, matched exactly or else ignoring case; null for none. */
    const Entry* byCatId (const juce::String&);

    /** The entry with this category and subcategory (ignoring case); null for none. */
    const Entry* byNames (const juce::String& category, const juce::String& subCategory);

    /** The CatID that starts a UCS file name (before its first underscore); null for none. */
    const Entry* fromName (const juce::String& nameWithoutExtension);

    /** The category an iXML chunk gives: USER CATID, else the ASWG catId, else USER CATEGORY and
        SUBCATEGORY; null for none (or for XML that doesn't parse). */
    const Entry* fromIXml (const juce::String& xml);
}
