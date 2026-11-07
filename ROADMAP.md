# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

Sound Library: a browser for a large sound-effects library (100k to a million files and more), with drag and drop into the app and out of it.

1. **Category suggestions:** most libraries don't name their files the UCS way, so a category could be suggested for the rest (never taken as the sound's category until accepted). Not built, because what has been tried isn't good enough to offer. Measured on one large library, each vendor's UCS-named sounds guessed from the others': scoring UCS's own words and synonyms against a sound's name and folders got the category right for 0 to 63% of most vendors' sounds; learning from the library's own UCS-named sounds as well (naive Bayes over name and folder words, UCS's words as the prior) got 40% of categories and 17% of exact subcategories, and its confidence didn't single out the right ones (44% right among the half it was surest of). The sounds whose names follow UCS come from only six vendors, 85% of them from one (mostly footsteps), so there is little to learn from. Next to try: words from the audio itself, or suggesting only within a folder whose other sounds have categories.

Not code: Reduce Reverb and Level Speech are yet to be tried on real recordings and listened to, and a Developer ID (for notarizing) is the remaining step before installing opens without Privacy & Security.
