# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

Sound Library: a browser for a large sound-effects library (100k to a million files and more), with drag and drop into the app and out of it.

1. **Sound Library window:** a window of its own (opened from the Window menu, reopening where it was left, on any screen) with Add Folder... and Remove Folder for the folders it scans (and nothing scanned until one is added), listing the index with search as you type and sorting, dragging one or more sounds into the main window's multitrack view or editor, Finder and other apps.
2. **Preview:** the selected sound's entire waveform drawn in the Sound Library window, with a playhead moving across it while it plays and a scrubber (click or drag in the waveform to hear from there), played on selection or the space bar, looping if wanted, at a preview volume, all before it is dragged or inserted. Optionally a part of it selected by dragging across the waveform: dragging from inside that selection carries only the selected part (written as a file of its own), from anywhere else the whole sound.
3. **UCS categories:** each sound's category from its UCS name (CatID_FXName_Creator_Source) or metadata, browsing by category and subcategory, and setting it by hand.
4. **Tags, ratings and favourites:** your own, kept in the index, with filters for each.
5. **Writing metadata into files:** tags, category and description written into WAV files' BWF and iXML chunks so other apps see them, safely (a temporary copy replacing the file only when whole, the audio and other chunks kept bit for bit).
6. **Insert by keyboard:** the selected sound, or the part selected in its waveform, placed at the cursor on the selected track.

Not code: Reduce Reverb and Level Speech are yet to be tried on real recordings and listened to, and a Developer ID (for notarizing) is the remaining step before installing opens without Privacy & Security.
