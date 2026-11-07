# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

Sound Library: a browser for a large sound-effects library (100k to a million files and more), with drag and drop into the app and out of it.

1. **UCS categories:** each sound's category from its UCS name (CatID_FXName_Creator_Source) or metadata, browsing by category and subcategory, and setting it by hand.
2. **Tags, ratings and favourites:** your own, kept in the index, with filters for each.
3. **Writing metadata into files:** tags, category and description written into WAV files' BWF and iXML chunks so other apps see them, safely (a temporary copy replacing the file only when whole, the audio and other chunks kept bit for bit).
4. **Insert by keyboard:** the selected sound, or the part selected in its waveform, placed at the cursor on the selected track.

Not code: Reduce Reverb and Level Speech are yet to be tried on real recordings and listened to, and a Developer ID (for notarizing) is the remaining step before installing opens without Privacy & Security.
