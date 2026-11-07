# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

Sound Library: a browser for a large sound-effects library (100k to a million files and more), with drag and drop into the app and out of it.

1. **Insert by keyboard:** the selected sound, or the part selected in its waveform, placed at the cursor on the selected track.
2. **Category suggestions:** most libraries don't name their files the UCS way, so a category suggested for the rest from the words of a sound's name and description and UCS's synonyms, shown as a suggestion (never taken as the sound's category until accepted), its accuracy measured against the sounds whose UCS names give their category. A first try (scoring UCS's own words, synonyms and explanations against the words of a sound's name, description and folders) got the category right for 69% of the 15,339 sounds in one large library whose UCS names give their categories, but 85% of those were one vendor's footsteps and the other vendors' sounds came out at 0 to 63%, and the exact subcategory at 16%: not good enough to offer. Next to try: learning from the library's own UCS-named sounds.

Not code: Reduce Reverb and Level Speech are yet to be tried on real recordings and listened to, and a Developer ID (for notarizing) is the remaining step before installing opens without Privacy & Security.
