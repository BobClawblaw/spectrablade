# SpectraBlade

A clean-room audio editor for macOS that hosts VST3 and Audio Unit (v2 and v3) plug-ins. Built on JUCE 9.0.4.

SpectraBlade was called Waveloom, then briefly WaveDrill, until 31 October 2026. On first launch it copies the settings of whichever earlier version you ran (audio device, inputs and outputs, plug-in list), and it still opens `.wdrack` and `.wlrack` rack files.

![SpectraBlade](Art/spectrablade-banner.png)

**Milestone 138: Paste to Fill Selection**
- Edit > Paste to Fill Selection replaces the selection with the clipboard played over and over, the last copy cut short, so the file keeps its length and the markers stay where they are. A clipboard at another sample rate is converted first, as for Paste. It refuses a selection that would take more than 10,000 copies of the clipboard. It is one undo step, and the selection is kept.
- Measured by the tests: filling frames 500 to 600 of a ramp with a 30-frame clip gives three whole copies and the first 10 frames of a fourth, bit for bit, with the rest of the file unchanged and the length kept; an empty range is refused, and a one-frame clip fills 10,000 frames but not 10,001. The menu item and the rate conversion were not tried by hand. The whole suite passed, the default-device test included.

**Milestone 137: Repeat Selection**
- Edit > Repeat Selection asks how many copies (1 to 100) and puts them right after the selection, moving the rest of the file and its markers later. The copies share the selection's audio rather than duplicating samples. It is one undo step, and the copies are selected afterwards.
- Measured by the tests: three copies of frames 100 to 300 of a ramp give the first 300 frames, the copies bit for bit, then the rest of the ramp; an empty range or zero copies is refused. The dialog and the marker move were not tried by hand. The whole suite passed, the default-device test included.

**Milestone 136: Crossfade Loop**
- Effects > Crossfade Loop asks for a crossfade time (50 ms to start) and blends the last part of the selection, with an equal-power crossfade, into the audio just before the selection's start, so that the selection's last frame is the frame before its start and looping it plays on without a click. The crossfade must be at most half the selection and no longer than the audio before it; otherwise it says so. It is one undo step, and the selection is kept for looping.
- Measured by the tests: on a ramp, a 100-frame crossfade over frames 400 to 800 leaves every frame before 700 and from 800 on exactly as it was, makes frame 799 exactly frame 399, and changes the frames between; a zero crossfade, one longer than half the selection, and one longer than the audio before the start are refused. How it sounds when looped was not listened to, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 135: Selection to Zero Crossings**
- Edit > Selection to Zero Crossings moves each end of the editor's selection to the nearest zero crossing of the channels' sum, up to 5 ms away, using the same search as the session's Cut at Zero Crossings, so a cut or paste of it doesn't click. An end with no crossing that near stays where it is.
- Measured by the tests: on a square wave crossing zero every 50 frames, a selection from 110 to 390 becomes exactly 100 to 400; with an end 25 frames from the nearest crossings and a reach of 20, that end stays; an empty selection doesn't change. The command was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 134: Double and Halve Selection**
- Edit > Double Selection (Alt Shift Up) and Halve Selection (Alt Shift Down) make the editor's selection twice or half as long, keeping its start: doubling stops at the end of the file, halving at one frame. With Step Selection, a loop length can be found by ear and stepped through.
- Measured by the tests: a 200-frame selection at 100 doubles to 100 to 500 and halves to 100 to 200; one at 700 doubles only to the file's end at 1,000; a one-frame selection stays one frame; an empty one doesn't change. The keys and commands were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 133: Step Selection**
- Edit > Step Selection Later (Alt Shift Right) and Step Selection Earlier (Alt Shift Left) move the editor's selection by its own length, so a file can be heard or edited one same-sized stretch after another, and scroll to it. At the start or end of the file the selection stops there with its length kept.
- Measured by the tests: a 200-frame selection at 100 in a 1,000-frame file steps to 300 and back to 0, one at 700 stops at 800 to 1,000 and stays there, and an empty selection doesn't move. The keys and the commands were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 132: Copy Selection Time**
- Edit > Copy Selection Time puts the selection's (or the session's time range's) start, end and length on the clipboard as one line of text, tab-separated, each written as Go to Time reads it ("1:23.500", with hours from an hour on), ready to paste into a spreadsheet or a note.
- Measured by the tests: 1.5 to 3.5 seconds at 48 kHz is written "0:01.500", "0:03.500" and "0:02.000"; an hour and a bit reads "1:02:05.001" and ten minutes "10:00.000"; a written time read back by Go to Time is the same frame. The clipboard itself was not written by the tests (they would overwrite the person's clipboard), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 131: clip list count line**
- Below the clip list a line counts the session's clips, with how many are locked and muted, and adds up the length of the clips shown ("3 clips, 1 locked, 1 muted; 0.850 s in all"), saying how many are shown while the search hides some. It follows edits and undo with the list.
- Measured by the tests, through a real clip list: three clips (one locked and muted) of 0.5, 0.25 and 0.1 seconds read "3 clips, 1 locked, 1 muted; 0.850 s in all", and a search for one of them "1 shown, 0.250 s in all". The layout was not looked at, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 130: marker list count and buttons**
- Below the marker list a line counts the markers and regions ("3 markers, 2 regions", adding "(2 shown)" while the search hides some), beside Rename and Delete buttons that act on the highlighted row as Return and Delete do. The buttons are off when no row is highlighted.
- Measured by the tests, through a real marker list: two markers and two regions are counted, and "(2 shown)" added when a search shows two; Delete with no row highlighted asks for nothing, and with the second row highlighted asks for exactly the marker that row shows. Clicking the buttons with the mouse and the layout were not checked, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 129: Next and Previous Region**
- Markers > Next Region (Cmd Shift ]) and Previous Region (Cmd Shift [) select the region starting next after the selection (or the cursor), or the last one starting before it, and scroll to it, in the editor or the session. Point markers are passed over; of regions starting together the shortest comes first. At the first or last region nothing changes.
- Measured by the tests: among regions at [50, 450), [150, 250) and [220, 240) with point markers between them, stepping forwards from 0 visits them in that order and stops, and backwards from the last returns to [150, 250); of two regions starting together the shorter is chosen. Through a real session view, Next from the cursor selects the first region, then the second, then stays; Previous returns to the first. The keys and the editor's command were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 128: Loop Region**
- Transport > Loop Region selects the region the cursor is in (the innermost, as Select Region at Cursor picks it), turns Loop Playback on and plays the region over and over from its start, in the editor or the session; if something was playing it starts again from the region. With the cursor in no region it says so. It is off while recording.
- Not measured: the command lives in the app's command table, which is not in the test build, so only the region it picks is covered (by milestone 111's tests); playing, looping and the setting were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 127: Select Clips in Region**
- Edit > Select Clips in Region (in the session) makes the region the cursor is in the time range and selects every clip that plays part of it, as Select Clips in Time Range does, on shown tracks only. With the cursor in no region it changes nothing. It is not an edit.
- Measured by the tests, through a real session view: with the cursor in a region from 20,000 to 50,000 frames, the time range becomes that region and the four clips playing part of it are selected; with the cursor outside every region nothing changes. The command was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 126: Add Regions over Clips**
- Markers > Add Regions over Clips (in the session) adds a region over each stretch where clips run without a gap: clips that overlap or touch, on any track, count as one stretch, and a gap starts the next. New regions are named in turn ("Region 01", ...), a stretch that already has a region exactly over it is skipped, and it is one undo step. Together with Export Regions, each song or take laid out with gaps between becomes a file of its own.
- Measured by the tests: two overlapping clips on two tracks and a third touching them make one region from 0 to 2,000 frames, a clip after a gap makes a second from 5,000 to 6,000, and doing it again adds none. The command in the session view was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 125: Fit Regions to Clips**
- In the session, the same Markers menu command reads Fit Regions to Clips: it shrinks every region to the span of the clips overlapping it, on any track and muted ones included, within the region, so Export Regions leaves out the empty time around them. Regions over no clip, and point markers, are left alone. It is one undo step, and says so when nothing would change.
- Measured by the tests: with clips from 0 to 12,000 frames on two tracks, a region from 500 to 20,000 becomes 500 to 12,000, while a region past every clip, one already from 0 to 12,000 and a point marker are unchanged. The command in the session view was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 124: Fit Regions to Sound**
- Markers > Fit Regions to Sound (in the editor) shrinks every region to the span from its first frame above the level Trim Silence last used to its last, so a region drawn loosely around a take hugs it. The audio is not changed. Regions holding nothing above the level, and point markers, are left alone. It is one undo step, and says so when nothing would change.
- Measured by the tests: a region from inside a long pause into the sound after it now starts exactly where the sound does; a region wholly inside the pause, a point marker and a region already starting and ending on sound are unchanged. Trimming a region's end was not tested separately (it is the same search as Trim Silence's, which was), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 123: Shorten Pauses**
- Effects > Shorten Pauses asks for a level and a time (the same dialog as Find Next Silence) and cuts every pause between sounds that is longer than the time down to it, taking the cut from the pause's middle so its two ends, half the time each, stay as they were. Silence at the very start or end of the file is left alone. Markers after each cut move back with the audio, as they do for Delete. It is one undo step, and says so when there is nothing to shorten.
- Measured by the tests: shortened to 60 frames, a 100-frame pause and a 70,000-frame one are each cut from frame 30 of the pause to 30 frames before its end, and the result is bit-identical to the audio with exactly those frames taken out; a time longer than every pause cuts nothing. The command, the dialog and the marker moves were not exercised by the tests (the marker moves are Delete's, which are), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 122: Add Markers at Silences**
- Markers > Add Markers at Silences (in the editor) asks for the same level and time as Find Next Silence and puts a point marker in the middle of every pause between sounds: silence at the very start or end of the file is not a pause and gets none. Frames already holding a point marker are skipped, at most 1,000 are added, and it is one undo step. With Regions from Markers and Save Each Region As, a recording of several takes can be split into a file per take.
- Measured by the tests: in the Find Next Silence test signal, pauses of at least 100 frames get markers at exactly their middles, 1,050 and 95,000; the same audio followed by 100,000 frames of silence gives the same two; asking for at most one gives the first. Naming, skipping taken frames and the undo step were not exercised by the tests (they work as Add Markers at Intervals' do, which was), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 121: Find Next Silence**
- Edit > Find Next Silence asks for a level (the one Trim Silence last used, -60 dB to start) and a time (half a second to start) and selects the next stretch where every channel stays at or below that level for at least that long, searching from the cursor or the end of the selection, and scrolls to it. The level it is given becomes the one Trim Silence and Select Audible Span use too, and the time is remembered while the app runs. When there is none it says so. Find Next Clipping now shares its search with it, unchanged in what it finds.
- Measured by the tests: in 200,000 frames of a full-level tone, with a 100-frame gap, a 200-frame gap on the left channel only and a 70,000-frame gap crossing a block edge, asking for 150 frames finds exactly the long gap, asking for 100 finds the short one, and nothing is found past the long gap; the clipping test still passed after the search was shared. The dialog was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 120: Find Next Clipping**
- Edit > Find Next Clipping selects the next run of three or more frames where some channel is at or above 0.999 (within 0.009 dB of full scale), searching from the cursor, or from the end of the selection so that choosing it again moves on, and scrolls to it. When there is none it says so. Pairs well with Click and Pop Repair or Gain on the found run.
- Measured by the tests: in 140,000 frames, two full-scale samples are passed over; three on the right channel straddling the 65,536-frame block edge are found as exactly 65,535 to 65,538; searching from there finds four samples of 0.9995 ending the file; past the end finds nothing; and with a run length of two the first pair is found. The command was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 119: channel levels in Loudness Statistics**
- The editor's Loudness Statistics window now also lists, for each channel ("Left" and "Right" for stereo), its sample peak and RMS level in dB and its DC offset as a percentage of full scale, over the same selection or whole file. They are summed in double precision, a block of 65,536 frames at a time; they are worked out on the message thread once the loudness measurement finishes, so a very long file adds a moment before the window shows.
- Measured by the tests: over 70,000 frames, a square wave of 0.5 reads exactly 0.5 peak, 0.5 RMS and no offset, and a constant 0.25 exactly 0.25 for all three. The window's layout was not looked at, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 118: normalizing each channel on its own**
- The editor's Normalize dialog has an "Each channel on its own" switch (for files with more than one channel): with it on, each channel's peak is brought to the level separately, so a quiet side comes up to match; a silent channel is left silent. The switch is remembered while the app runs. With it off, Normalize works as before, by the loudest channel.
- Measured by the tests: a left channel peaking at 0.5 and a right at 0.25 both end at a peak of 0.5 (within 1e-6) when normalized to that level; a silent right channel stays exactly silent while the left is brought up; all silence is refused. The dialog and its switch were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 117: sorting the clip list**
- Clicking a column header of the clip list sorts it by that column, and clicking it again reverses it: tracks in their order in the session, start and length by value, and name, file and state as text ignoring case with numbers in order. Rows that tie keep time order. The search, the highlighted row of the selected clip and clicking a row work the same while sorted, and edits keep the sort.
- Measured by the tests, through a real clip list with three clips on two tracks: sorted by length longest first, by name, and by track (Drums, the first track, before Bass), with the first row still choosing the clip it shows, then back to time order. Clicking the headers with the mouse was not done by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 116: sorting the marker list**
- Clicking a column header of the marker list sorts it by that column, and clicking it again reverses it: names ignoring case with numbers in order ("Take 2" before "Take 10"), or start, end or length, with points counting as ending where they start. Rows that tie keep time order. The search and the row-to-marker mapping work the same while sorted, and edits keep the sort.
- Measured by the tests, through a real marker list: "take 10", "Take 2", "intro" and "Take 1" sort by name as intro, Take 1, Take 2, take 10; by length, longest first, as Take 2, intro, then the two points in time order; the first row then maps to the right marker; and the list goes back to time order. Clicking the headers with the mouse was not done by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 115: Import Markers from CSV**
- File > Import Markers from CSV reads a list written by Export Markers as CSV and adds its markers and regions to the open file or the session, as one undo step, leaving out any already there with the same place, length and name. Markers are placed by their times in seconds, so a list moves between files at different sample rates; in the editor, markers starting past the end of the file are left out and regions are cut to it. Names with commas, quotes or line breaks come back as written. Colours are not in the CSV, so imported markers take the default ones. A file that isn't such a list, or has a line that can't be read, is refused with a message and nothing is added.
- Measured by the tests: four markers and regions, two with a comma, quotes and a line break in their names, written and read back at the same rate are identical; at half the rate they land at half the frames; cut to 480,000 frames the last region ends there, and cut to 100,000 only the two starting before remain; another CSV, a bad time and an unclosed quote are refused, and a header alone is an empty list; merging adds only new markers, and merging again adds none. The menu item and file dialog were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 114: Rename Markers in Sequence**
- Markers > Rename Markers in Sequence asks for a name and gives every marker and region, in time order, that name and a number: "Take 01", "Take 02", and three digits once there are 100 or more, so names sort in order. Colours and positions are kept, and it is one undo step in the editor or the session. An empty name, or names that are already so, change nothing.
- Measured by the tests: five markers and regions named " Take " come out "Take 01" to "Take 05" in time order with a colour kept; 100 markers run "T 001" to "T 100"; a blank name leaves them as they were. Through a real session view the renaming is one undo step, and repeating it adds none. The editor's dialog was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 113: Regions from Markers**
- Markers > Regions from Markers turns each point marker into a region running to the next point marker, or to the end (of the file, or of the session's last clip) after the last one, keeping its name and colour, as one undo step. Existing regions are left alone and don't end the new ones; a point at the very end stays a point. Together with Add Markers at Intervals and Save Each Region As (or Export Regions), a long recording can be cut into equal files.
- Measured by the tests: points at 100, 300, 700 and 1,000 in a 1,000-frame file, with a region at 150, become regions 100 to 300, 300 to 700 (its colour kept) and 700 to 1,000, the point at the end staying a point and the region untouched. Through a real session view, the one point becomes a region to the end of the last clip as one undo step. The editor's command was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 112: Add Markers at Intervals**
- Markers > Add Markers at Intervals asks for an interval in seconds or minutes and seconds and puts a point marker at that spacing through the selection (or the session's time selection), or the whole file or session, from its start. New markers are named in turn ("Marker 02", ...), frames that already hold a point marker are skipped, and it is one undo step. At most 1,000 are added at once; an interval that would give more, or is not a time above 0, adds nothing and says why.
- Measured by the tests: every 100 frames through [50, 400) next to an existing marker at 150 adds three, at 50, 250 and 350, sorted and named in turn; a zero step and a range needing 1,001 are refused, and one needing exactly 1,000 adds all but the frames already taken. Through a real session view with no time selection, a one-second interval in a three-second session adds markers at 0, 1 and 2 seconds as one undo step that undo takes away. Bars and beats as an interval were not built. The editor's dialog was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 111: Select Region at Cursor**
- Markers > Select Region at Cursor selects the region the cursor is in, in the editor and in the session: from its start up to its end (a cursor exactly at the end is outside it). Where regions overlap it picks the shortest, so the innermost wins. It is not an edit. The command is on whenever there is a region.
- Measured by the tests: among overlapping regions [50, 450), [150, 250) and [220, 240), frames 230, 200, 150 and 400 pick the innermost holding them; frames 20 and 450, and a point marker, give nothing. Through a real session view, the cursor inside a region selects it and a cursor outside every region selects nothing, with no undo step added. The editor's command and the menu were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 110: Save Each Region As**
- File > Save Each Region As asks for a folder and saves every region of the open file to a WAV file of its own, named and numbered as the session's Export Regions names them ("Take - 01 Verse.wav"), in the background with a progress bar that can cancel. Each file keeps the markers inside its region, the region itself included, moved to start from 0. The files are written at the open file's bit depth when WAV can hold it, otherwise as 32-bit float, and like Export Regions it replaces files of the same name.
- Measured by the tests: an open file's markers give the same regions and numbering as a session's, with point markers left out; saving each range is the Save Selection As writer measured in milestone 109. The menu item, the folder dialog and the job were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 109: Save Selection As**
- File > Save Selection As saves only the selection to a new WAV, AIFF or FLAC file, in a sample format picked as Save As picks one, with the markers inside the selection moved so it starts at frame 0 (as Crop keeps them). The open file, its name and its saved state are left as they were.
- Save As and Save Selection As now share one file-and-format dialog; Save As's steps are unchanged, but it was not tried again by hand.
- Measured by the tests: frames 1,000 to 2,000 of a stereo test signal saved as 32-bit float read back bit-identical to those frames, with a point marker at 1,500 back at 500, a region from 1,800 to 2,200 cut to 800 to 1,000, and a marker before the selection left out. The menu item and dialog were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 108: Paste Mix**
- Edit > Paste Mix (Cmd Shift V) adds the clipboard into the audio from the cursor, or the start of the selection, instead of inserting it: nothing after it moves, and the file grows only by whatever runs past its end. A clipboard with another channel count is fitted as Paste fits it, and one at another sample rate is converted first, as Paste does. The mixed span is left selected, and it is one undo step. The sum is not limited, so loud material can go past full scale.
- Measured by the tests: 200 frames of 0.125 from a mono clipboard mixed into a stereo file of 1,000 frames at frame 900 leave the first 900 frames bit-identical, add exactly 0.125 to both channels of the last 100, and append 100 frames of 0.125, for 1,100 frames in all. The menu item, the key and the rate conversion path were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 107: Set Selection**
- Edit > Set Selection asks for a start and a length, each in seconds or minutes and seconds, filled in from the current selection (or the cursor), then selects that span and scrolls to its start. A length running past the end of the file is cut to it; a start at or past the end, a zero length or a time that can't be read is refused with a message.
- Measured by the tests: in a 10-second file at 48 kHz, 1.5 and 2 select frames 72,000 to 168,000, 0:09 and 0:05 select 432,000 to the end at 480,000, and 0 and 10 the whole file; a start of 10, a length of 0, and unreadable, empty or negative times give nothing. The dialog was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 106: Balance**
- Effects > Balance asks for a position from -100 (all left) to 100 (all right) and moves the balance of the selection, or the whole file, by lowering the side moved away from, with the same law a session track's pan applies to a stereo clip (half right: the left down 3 dB). It is offered for two-channel files only; a mono file can be made stereo first. Zero changes nothing and adds no undo step.
- Measured by the tests: half right leaves the right channel bit-identical and multiplies the left by exactly the mixer's gain for that position (within float rounding of the square root of a half) inside the range, and nothing outside it. The dialog was not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 105: fade shapes in the editor**
- Effects > Fade Shape picks the curve the editor's Fade In and Fade Out follow: S-Curve (as before), Linear, Equal Power, Slow Start or Fast Start, the same shapes session clips use. The choice is ticked in the menu and kept between runs. The first frame of a fade in is silent and its last at full level, and a fade out the other way round.
- The S-curve now goes through the shared shape function, which orders its float arithmetic differently from the old one; the two were not compared bit for bit, and the existing fade test still passed.
- Measured by the tests: five frames at full level faded in linearly come out exactly 0, 0.25, 0.5, 0.75 and 1, and faded out with a slow start exactly 1, 0.5625, 0.25, 0.0625 and 0. The submenu, its tick and the kept setting were not exercised by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 104: Select Audible Span, and the new effects kept to the editor**
- Edit > Select Audible Span selects from the first frame to the last that is above the level Trim Silence last used (-60 dB until it is first used), and says so when the whole file is below it. Trim Silence now offers that last level too.
- Fixed: Remove DC Offset, Swap Channels, Insert Silence, Trim Silence, Mix to Mono and Make Stereo (milestones 99 to 103) stayed on while the session was showing, where choosing one would have edited the open file behind it. They are now off in the session view, like the editor's other effects.
- Measured by the tests: the span itself is the audible range already measured for Trim Silence. The new command, the remembered level and the commands being off in the session view were not exercised by the tests (the app's command table is not in the test build), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 103: Mix to Mono and Make Stereo**
- Effects > Mix to Mono averages the channels of the whole file into one; Effects > Make Stereo copies a mono file's channel to left and right. Each is offered only when it would change the channel count, is one undo step, and keeps the selection and markers where they were. The file is converted 65,536 frames at a time, so a long file never needs one buffer for all of it.
- Measured by the tests: 70,000 frames of 0.5 on the left and 0.25 on the right mix to exactly 0.375 on every frame, across the block boundary; that mono result made stereo is exactly 0.375 on both channels; and in a document the conversion leaves one channel, with undo bringing the two back bit-identically. The menu items, and how the waveform and playback look and sound after a conversion, were not checked by the tests or by hand. The whole suite passed, the default-device test included.

**Milestone 102: Trim Silence**
- Effects > Trim Silence asks for a threshold (-60 dB to start with) and crops the file to the span from the first frame to the last whose level on any channel is above it, keeping the markers inside that span. A file that is all below the threshold, or has nothing below it at either end, is left as it is with a message saying why. It is one undo step.
- Measured by the tests: in 300,000 frames of quiet hiss at about -66 dB, with one loud sample at frame 100,000 on the left channel and one at frame 200,000 on the right, the span found is exactly 100,000 to 200,001, across the 65,536-frame blocks the search reads; pure silence gives no span. The dialog, the crop itself and the markers were not exercised by the tests (the crop and its marker handling are the ones Crop already uses), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 101: Insert Silence**
- Effects > Insert Silence asks for a length in seconds or minutes and seconds and puts that much silence in at the cursor, or at the start of the selection, moving the audio and markers after it later. The inserted silence is left selected, and it is one undo step. A length that does not parse, is not positive, or is too long for one buffer is refused with a message.
- Measured by the tests: 500 frames inserted at frame 300 of a two-channel file give exactly the first 300 frames, 500 zeros, and the rest moved on unchanged. The dialog, the selection afterwards and the marker shift were not exercised by the tests (the marker shift reuses the edit already tested for paste), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 100: Swap Channels**
- Effects > Swap Channels exchanges left and right in the selection, or the whole file when nothing is selected. It is offered only for two-channel files and is one undo step.
- Measured by the tests: inside the range each channel holds exactly what the other held, outside it nothing changes, and a mono file passed to the edit comes back bit-identical. The menu item, and its being greyed out for mono files, were not checked by the tests or by hand. The whole suite passed, the default-device test included.

**Milestone 99: Remove DC Offset**
- Effects > Remove DC Offset takes each channel's average level out of the selection, or the whole file when nothing is selected, so the audio sits centred on zero. The average is summed in double precision. It is one undo step, like the other effects.
- Measured by the tests: a square wave of plus and minus 0.5 raised by 0.25, and a constant -0.1 on the second channel, come back exactly as plus and minus 0.5 and exactly 0 inside the range, with every sample outside it unchanged. The menu item was not chosen by the tests, and it was not tried by hand. The whole suite passed, the default-device test included.

**Milestone 98: moving between tracks with the keys**
- Multitrack > Select Track Above (Up) and Select Track Below (Down) move the track selection to the next shown track, skipping hidden tracks and the members of collapsed folders, and scroll its lane into view. With no track selected, Down selects the first and Up the last. At the top or bottom nothing changes. The clip selection stays as it was.
- Measured by the tests, through a real view showing three of six tracks: Down from nothing visiting tracks 1, 2, 3, 5 and 6, skipping the hidden fourth, and stopping at the last; the header at the top of the lanes then being the last track's (a click on it keeps that track selected); and Up visiting them back to the first. The keys were not pressed by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 97: unsolo all and unmute all**
- Multitrack > Unsolo All and Unmute All clear the solo or mute of every track, bus and folder at once, each as one undo step. Each is offered only when something is soloed or muted.
- Measured by the tests: a session with a soloed track, a muted track, a soloed bus and a muted, soloed folder; Unsolo All clearing every solo and leaving the mutes; Unmute All clearing the mutes; neither doing anything a second time; and two undos bringing every solo and mute back. The menu items were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 96: a region from clips**
- Markers > Add Region from Clips adds a region running from the start of the earliest selected clip to the end of the latest, whatever tracks they are on, as an undo step. With one clip selected the region takes the clip's name; with several it is named like other new regions ("Region 01", ...).
- Measured by the tests, through the real view: two clips on two tracks giving one region from the first's start to the later end, as an undo step; one clip giving a region of its own length named after it; and nothing with no clip selected. The menu item was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 95: duplicating time**
- Multitrack > Duplicate Time (Cmd Option D) puts a copy of the time range in right after it, on every track: clips, envelopes and markers, moving everything after it on, as Copy Time then Paste Time at the range's end would, in one undo step ("Duplicate Time"). The copy becomes the time range and the cursor goes to its start, so doing it again repeats the section once more. What Copy Time copied is left as it was.
- Measured by the tests, through the real view: a clip and a marker in the range repeated 12,000 frames on, a clip on another track after the range moved on by the same, the copy selected, and the copied time untouched; doing it again making a third copy and moving the later clip on again; and nothing without a time range. The menu item and key were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 94: the clip list as CSV**
- File > Export Clip List as CSV... writes every clip in the session, in the clip list's order (by start, then by track), to a CSV file: its track, name, start, end and length in seconds, the full path of its file, whether it is locked or muted ("yes" or empty), and its start and length in frames. It follows RFC 4180 like the marker export: a header line, CRLF line ends, and fields holding a comma, quote or line break quoted with quotes doubled. The search box doesn't filter it. It is offered in a session that has clips.
- Measured by the tests: three clips on two tracks, added out of time order, written in the list's order with a track name holding a comma, a clip name and file path holding quotes, and a locked, muted clip, compared with the expected text character for character; and an empty session giving just the header. The menu item and the file chooser were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 93: next and previous clip**
- Edit > Select Next Clip (Tab) and Select Previous Clip (Shift Tab) step along the selected clip's track in time order, selecting the next or previous clip alone, putting the cursor at its start and scrolling it into view, as clicking it in the clip list does. With no clip selected they start from the cursor on the selected track: the first clip starting at or after it, or the last starting before it. At either end of the track nothing changes.
- Measured by the tests, through the real view: three clips added out of time order stepped through in time order and stopping at the last, with a clip on another track never chosen; stepping back with the cursor at that clip's start; and with nothing selected, the clips after and before the cursor, and nothing past the last. The keys and menu items were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 92: trimming to the cursor**
- Multitrack > Trim Start to Cursor (Option [) and Trim End to Cursor (Option ]) trim each selected clip the cursor is inside so it starts or ends at the cursor, its audio staying where it is in time. Clips the cursor isn't inside (at an edge counts as outside) and locked clips stay as they are. All the trims are one undo step ("Trim Start to Cursor", or "Trim Start of Clips to Cursor" for several). The cursor is used exactly, without moving to a zero crossing.
- Measured by the tests, through the real view: two selected clips on different tracks trimmed to start at the cursor (their audio offsets moving on by 4,000 and 2,000 frames), a clip past the cursor and a locked clip spanning it left alone, as one undo step; the same for ends; one clip alone named in the singular; and the cursor at a clip's edge or between clips trimming nothing and adding no step. The keys and menu items were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 91: open recent**
- File > Open Recent lists the last ten audio files and sessions opened, newest first, and is kept between launches. Opening one that is already listed moves it to the top. Files that have since gone are left off the menu, and Clear Menu empties it. Choosing a session asks about unsaved session changes first, and choosing an audio file asks about unsaved changes to the open file, as Open Session and Open do. Recovered copies of sessions are not listed. The menu is unavailable while recording.
- Measured by the tests: twelve files noted giving the newest ten, newest first; one noted again moving to the top; a fresh list reading the same ten back from the settings; a deleted file left off the menu while the others keep the ids of their places in the list, with Clear Menu last; and clearing emptying the list and the settings. Choosing items from the real menu bar and opening through it were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 90: go to time**
- Transport > Go to Time... (Cmd J) asks for a time, starting from the cursor's, and moves the cursor there, scrolling it a tenth of the way into view when it is out of view, at the same zoom. It reads seconds ("83.5" or "83.5 s"), minutes and seconds ("1:23.5"), or hours, minutes and seconds ("1:01:23.5"); in a session also bar and beat ("5|3", or "5|3.5" for halfway through the beat), counted from 1 and following tempo and time-signature changes. A time it can't read, a beat past the end of its bar, or bar and beat in the single-file editor gives a message instead. In the editor the cursor stops at the end of the file.
- The multitrack view now scrolls as far as a screen past the cursor, so a cursor put past the end of the session can be seen.
- Measured by the tests: the three time forms at 48 kHz (83.5 s to frame 4,008,000, and so on), spaces around them, and 11 malformed, negative or out-of-range entries refused; bar and beat at 120 beats a minute in 4 with a change to 60 in 3 at bar 3, including a fraction of a beat and spaces around the bar line, and 8 that don't exist refused; and the multitrack view scrolling to 300 s in a shorter session at the same zoom, with the frame a tenth of the way in, then not scrolling for a frame already in view. The dialog, the menu item and the single-file editor's scrolling were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 89: tap tempo**
- Multitrack > Tap Tempo (T) sets the session's tempo from taps: from the second tap on, the tempo is the average spacing of the last eight taps, rounded to hundredths of a beat a minute and kept to 20 to 400. A pause of 3 s or more starts a new run. Each run of taps is one undo step ("Tap Tempo"). It sets the tempo from the start of the session; tempo changes later on stay as they are.
- Measured by the tests: taps half a second apart giving 120 from the second tap; uneven spacings averaged; a 0.7 s spacing giving 85.71; only the last 8 of 12 spacings counting; a 3 s pause or a tap earlier than the last starting again; very fast taps held at 400; and on a session, a run of taps undone in one step, a tap giving the tempo already set changing nothing, and a second run its own step. The key and menu item (which read the system clock) were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 88: polarity invert**
- Each track and bus can have its polarity inverted: its output is turned upside down after its effects and before its volume, so what it sends to buses (before or after its volume) is inverted too. Use it to fix a microphone that is wired backwards, or to line up two microphones on one source.
- In the mixer each strip has a Ø switch between M and S. In the track headers it is Invert Polarity in the right-click menu, and a yellow Ø before the name marks an inverted track. Both are undo steps ("Invert Polarity", "Restore Polarity"), and the session file keeps the setting.
- A frozen track's audio is rendered without the inversion, which is applied as the track plays, so it sounds the same frozen or live and can be switched without freezing again.
- Measured by the tests, through the session mixer: an inverted track (with a send after its volume, and with one before it and one after) playing exactly the negation of the same track uninverted, bit for bit; a track and the bus it feeds both inverted, so its direct sound is upside down and its send through the bus the right way up (worst difference 0 against the sum made by hand); and the freezing source state dropping the inversion. Also the strip's switch, the header command, the undo steps and the session file. The header menu and the Ø mark were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 87: searching the marker list**
- The marker list has a search box like the clip list's: type to show only the markers and regions whose name contains the text, ignoring case and spaces around it; Escape clears it. Clicking, renaming and deleting act on the marker a row shows, and the list keeps following edits and undo while filtered.
- Measured by the tests: a search in capitals finding one region; that row choosing the region (index 1, not row 0) and selecting its range; a marker renamed to match joining the list in time order; a search matching nothing; and clearing it showing every marker again. Typing into the box and Escape were not checked by the tests, nor renaming or deleting from a filtered list (they use the same row-to-marker mapping as choosing), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 86: searching the clip list**
- The clip list has a search box at the top. Type to show only the clips whose name, file name or track name contains the text, ignoring case and spaces around it; Escape clears it. The list keeps following edits and undo while filtered, so a clip renamed away from the search leaves it.
- Measured by the tests: a search by clip name in capitals, by track name, by file extension with a trailing space, one matching nothing, and a clip leaving the filtered list when renamed; then clearing the search showing every clip again. Typing into the box itself (rather than setting its text) and Escape were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 85: a menu on the mixer's strips**
- Right-click a strip in the mixer window (its background or name) for Solo Safe and a Colour submenu, as in the track headers, each an undo step. A solo-safe strip shows the same yellow bar under its S button, and its solo tooltip says so. The master strip has no menu.
- Measured by the tests: the menu offering Solo Safe unticked and the colours, nothing for an unknown strip; choosing Solo Safe making the bus solo safe as an undo step, after which it shows ticked; choosing Violet setting that bus's colour only, ticked, with Automatic unticked; and choosing Solo Safe again clearing it ("Not Solo Safe"). The right-click itself and the bar were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 84: solo safe**
- Right-click a track, bus or folder header and choose Solo Safe to keep it playing when other tracks are soloed: a reverb return stays on while you solo the vocal, say. A solo-safe bus still plays only what reaches it from tracks that are playing. Mute still silences a solo-safe track, and with nothing soloed it changes nothing. A folder's solo safe covers its members. A yellow bar under the S button marks it, and the solo button's tooltip says so.
- Solo Safe and Not Solo Safe are undo steps, and the session file keeps the setting (nothing is written for tracks without it). It does not follow a track's group.
- Measured by the tests, through the session mixer, against the same sums made by hand (worst difference 0.000000119, from adding in another order): with track A soloed, a solo-safe track B playing with its send into bus X, a solo-safe bus Y adding B's send, and a solo-safe Y on its own playing nothing, bit for bit the same as A soloed alone; a muted solo-safe track staying silent; and solo safe without a solo changing nothing, bit for bit. Also: the undo steps, a folder's solo safe covering its member, and the session file. The menu item and the bar were not checked by the tests, the mixer window does not show it yet, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 83: double-clicking to add markers**
- Double-click an empty spot in the marker lane to add a marker there, named like the others ("Marker 01", ...), as an undo step ("Add Marker 01"). With snapping on, it lands on a nearby clip edge, marker, the cursor or the time range's edges, as dragged markers do. Double-clicking a marker or region still renames it.
- Measured by the tests, through the real view: a double-click with snapping off adding a point marker 208 frames from the frame aimed at (99,792 for 100,000: the frame under that pixel at the test's zoom), and one 3 px past a clip's end with snapping on landing exactly on it, each its own undo step. A real double-click (two clicks through the mouse) was not sent by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 82: audio load and dropouts**
- The status line at the bottom of the window shows the audio load (the share of each audio callback's time spent processing, as the audio device manager averages it, in whole percent) and the number of dropouts (under- and overruns) since it was last clicked: "Load 12%  |  0 dropouts". It turns red while the load is over 80% or once there has been a dropout. Click it to count from 0 again. When the device changes, its count starts again from 0. It reads four times a second.
- Measured by the tests: the text at 12%, 85% (red) and over 100% (shown as 100%); three dropouts turning it red; a reset clearing them and one more showing as "1 dropout"; a new device's lower count shown in full; and the readout reading the real device manager. Whether the device reports dropouts itself, the readout under real load, and its place in the window were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 81: selecting the clips in the time range**
- Edit > Select Clips in Time Range selects every clip that plays any part of the time range, on every shown track. A clip that only touches the range at its start or end is left out. It needs a time range, and it replaces the clip selection.
- Measured by the tests, through the real view: a range catching the ends of two clips and the starts of two more, on two tracks, and not the clips on a hidden track; a range between clips, touching both, selecting none; and nothing happening without a range. The menu item was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 80: selecting between markers**
- Markers > Select Between Markers selects from the marker start or region end at or before the cursor to the next one after it. With none before, the selection starts at 0; with none after, it runs to the end of the last clip in a session, or the end of the file in the single-file editor. It does nothing with the cursor past that end, or with no markers. It is not an edit, so it adds no undo step.
- Measured by the tests: the span around a frame among overlapping regions and points (between two stops, on a stop, at a region's end, before the first, after the last), and nothing with no markers, an empty span or the frame past the end; in the multitrack view, the time range set around the cursor, from the start, to the last clip's end and not past it, with no undo step. The single-file editor's use of it and the menu item were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 79: peak hold and clip lights on the header meters**
- Each bar of a header meter now marks its highest recent peak with a short line, which holds for 1.5 s (45 screen refreshes) and then falls back as the bar does. A new peak above it takes over at once. Switching the meter between a track's input and its level into the mix starts the holds afresh.
- A light on top of the meter turns red once either bar reaches 0 dB, and stays on after the bars fall, until the meter is clicked. A held peak at 0 dB or over is drawn red too.
- Measured by the tests: both holds staying at their peaks for 45 refreshes after the bars have fallen to nothing, then falling, and a new peak taking over; a peak of exactly 0 dB lighting the light on that track only, the light still on 200 refreshes later, and a click on the meter putting it out. How it looks was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 78: marker colours**
- A marker's or region's right-click menu has a Colour submenu with the eight track colours and Default. Choosing one is an undo step ("Colour <name>"). The flag or bar and the line through the tracks are drawn in it, and the marker list shows it as a strip before the name. Default keeps markers orange and regions blue, as before.
- The colour is saved with the session (nothing is written for Default; a number past the palette reads as the last colour), and moves with the marker through deleting or inserting time, cropping and pasting.
- Markers in the single-file editor keep their fixed colours: WAV cue and AIFF marker chunks have no place for one.
- Measured by the tests: the undo step; nothing done for the colour a marker already has, a marker that is gone, or a colour past the palette; the flag drawn in its colour on the ruler; the default colours; the colour kept through deleting time and cropping; the session file round trip, and a colour past the palette read as the last; and undo. The menu and the strip in the marker list were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 77: header level meters**
- Each track header has a thin two-bar meter at its right edge. A playing track shows its level into the mix (after its effects, volume and pan); an armed track shows its input instead, tinted red, with a mono input filling both bars. Bars fall back by about a third every screen refresh. Folders have none.
- The headers read the peaks without taking them from the mixer's meters or the recording meter: the recorder and the mixer now keep a separate peak for each reader.
- Measured by the tests: the recorder giving both readers a block's peaks and then nothing; a header showing a track's level, the mixer still getting the same peaks afterwards, the level falling to 0.63 of itself at the next refresh, an armed track switching to its input, and nothing for a folder. How the meters look and move while playing or recording was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 76: session notes**
- Multitrack > Session Notes opens a window with a page of free text kept with the session: lyrics, which take is which, things to do. Typing changes the notes as an undo step ("Session Notes"); further typing joins that step until something else is done. Undo and redo show in the window as they happen, and the notes are saved with the session exactly as written (nothing is written when there are none).
- Measured by the tests: the window showing the session's notes; two rounds of typing making one undo step, and undoing it showing the notes as they were; and notes with blank lines, a tab, trailing spaces, accented letters and the characters XML treats specially, through a session file unchanged. The window itself (opening, closing, the menu tick) was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 75: dropping tracks into folders**
- Dragging a track's or bus's header onto a folder's header puts it in that folder, just below the folder, as one undo step ("Move to Folder"); while dragging over a folder, the folder's lane is outlined instead of the usual line. Dropped anywhere else, a header moves its track as before, and the track stays in whatever folder it was in; to take one out, use Folder > None on its header. A folder's header dropped on another folder just moves (folders don't nest).
- Measured by the tests, through the view: a track dropped on a folder from below and one from above each going in, right below the folder, the moved track staying selected; and a drop on a track in a folder just moving it. The outline and the drag itself were not checked by the tests (they call the drop directly), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 74: folder strips in the mixer**
- Each folder now has a strip in the mixer, in its place among the tracks and buses: its volume, mute and solo, which act on everything in it (Milestones 71 and 72), with no pan and no meter, since a folder has no audio of its own.
- This fixes a bug from Milestone 71: leaving folders out of the mixer put every strip after a folder one place off, so it showed, and changed, the next track's settings, and its meter showed the wrong track. Strips are again one for each track, bus and folder, in order.
- Measured by the tests: with a track, then a folder at -6 dB holding a track at -3 dB panned right, three strips, the folder's showing -6 dB with its pan hidden and the track after it showing its own -3 dB and pan; the folder's fader and mute setting the folder and leaving the track's own alone, the track muted through it. None of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 73: folder stems**
- Multitrack > Export Folder Stems... writes one file per folder, for each folder with a track with clips in it: its tracks and buses mixed together as they are set (volumes, pans, envelopes, clip settings and their own mutes), through their effects (fresh plug-ins for each stem), with the folder's volume. The folder's own mute and any solos are left out, as are tracks outside the folder and sends to buses outside it. Like Export Stems, it covers the time range if there is one, otherwise the session from its start, and the files are 32-bit float, named "Session - 01 Folder.wav" by the folder's place among them. It runs in the background with a progress bar and can be cancelled.
- Measured by the tests: a folder holding a track and the bus it sends to, at -3 dB, muted itself and with a track outside soloed, rendering bit for bit as the session does with only the folder's tracks heard; a send to a bus outside the folder left out, and a muted track in it silent; and only folders with clips in them listed. Export Stems now loads effects the same way for each track alone, and its tests pass unchanged. Writing the files, the menu item and plug-ins in a folder stem were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 72: folder volume**
- A folder's header has a volume control, which acts as if added to the volume of every track and bus in it: it scales what each sends to the mix and its sends after the volume (before-volume sends are left alone), on top of its own volume and volume envelope. Changing it is an undo step, as a track's volume is. It isn't automated, and writing automation leaves it alone.
- Freezing, bouncing and exporting stems render a track on its own, as before, so without its folder's volume (or mute); a frozen track in a folder still has the folder's volume applied as it plays. Bounce Track now puts the new track in the source's folder (in Milestone 71 it went in none), so the folder's mute, solo and volume still act on what is heard. Folder volumes are saved with the session.
- Measured by the tests: a folder at -6 dB, holding a track and the bus it sends to after its volume, mixing within 0.000000001 of the two at -6 dB by their own volumes (the largest difference measured was 0); a folder at 0 dB mixing bit for bit as with no folder; a bounce of the track landing in its folder, the mix staying within float rounding (the largest difference measured was 0); and the volume through a session file. The control in the header was not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 71: track folders**
- Multitrack > Add Folder adds a folder just above the selected track or bus, with it inside (or at the end, empty, when a folder or nothing is selected). Right-click any track's or bus's header to put it in a folder, or take it out, with the Folder submenu. Folders don't nest. Each is an undo step.
- A folder has no audio of its own, only a name, mute and solo: muting it mutes everything in it, and soloing it solos everything in it, exactly as their own buttons would (a soloed track still brings the buses it sends to). Its lane shows where its tracks have clips, one thin bar per track. Click the triangle on its header (or choose Collapse in its menu) to fold its tracks out of the lanes: they keep playing and stay in the mixer, their clips leave the selection, and going to one of their clips (from the clip list, say) opens the folder again. Tracks in a folder are drawn inset, with a rule down their left.
- Folders have no strip in the mixer, take no clips (a clip dropped, pasted or recorded onto one goes onto a new track), and can't send or be saved as track templates. A track's place in a folder doesn't depend on where it is in the list; Add Folder puts the folder right above the track it takes in. Removing a folder leaves its tracks where they are, out of any folder. Folders, whether collapsed, and who is in them are saved with the session.
- Measured by the tests: a folder muted mixing bit for bit as its track and bus muted by their own buttons, and soloed as them soloed; a collapsed folder's tracks mixing as before; the refused cases (a folder into a folder, into a track, a missing folder, where it already is, sends from a folder); a clip for a folder landing on a new track; removing a folder freeing its tracks; folders through a session file; and, through the view, Add Folder taking in the selected track above it, the header's triangle collapsing the folder (its track leaving the lanes and the selection, the track below taking its row), going to its clip expanding it, and moving a track in and out, each as an undo step. How the folder lane and headers look, and the menus, were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 70: clip gain lines**
- Each clip can have a gain line: gain in dB, from -60 to +24, on top of the clip's own gain, changing over the clip in straight lines (in dB) between points. Multitrack > Clip Gain Lines shows every clip's line across its body, 0 dB half way up: click a line to add a point at the line's value, drag a point to move it (it stays between its neighbours and inside the part of the clip heard), Alt-click a point to remove it. Each is an undo step. Hidden, a clip with points still shows its line, fainter. Locked clips and clips on frozen tracks can't be changed. Where a shown track envelope's line is under the mouse too, the track envelope takes the click.
- The points sit on the clip's audio, not the session's time, so the line stays with the sound when the clip is moved, trimmed or split (both parts keep it), and a take change shifts it with the take. Reversing a clip turns its line round and stretching or pitch-shifting scales it, both from the part heard. Playback, mixdowns, bounces, stems, regions and freezing all use it, and it is saved with the session.
- The waveform drawn in the clip doesn't follow the line, and Normalize sets the clip's gain from its audio alone, ignoring the line.
- Measured by the tests: a steady clip, trimmed, mixing with its line every 7th frame within 0.000001 of the mix without it times the line's gain (largest difference 0.000000014), with the points landing on the session frames they should; moved, trimmed and split, the clip mixing bit for bit the same where it still plays; reversing turning the line round (within 0.00000024) and stretching scaling its points; a take change shifting the line; a locked clip refused and values limited; the line through a session file; and, through the view, adding, dragging and removing a point, with nothing happening while the lines are hidden. How the lines look and the menu item were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 69: grouping clips**
- Multitrack > Group Clips (Cmd G), also Group in a clip's right-click menu, groups the selected clips, on any tracks, locked ones too. Clicking any clip in a group then selects the whole group, so dragging it moves them all by the same amount, each on its own track, and deleting, nudging, locking, muting and the other edits on the selection act on them all; Shift-clicking a grouped clip takes the whole group out of the selection. Ungroup Clips (Cmd Shift G, or Ungroup in the menu) breaks up every group the selected clips are in. Grouping a clip again takes it out of its old group. Each is an undo step, and grouped clips show two chain links in their title bar.
- Split parts of a grouped clip stay in its group; copies (Duplicate, Repeat, pasting, pasting time, Duplicate Track) don't join it. As before, a selection with a locked clip in it doesn't move. Select All and the other Select commands select what they match, not whole groups. Groups are saved with the session.
- Measured by the tests: grouping two or more clips (fewer refused); a clip's group found across tracks; copies leaving the group and split parts staying in it; regrouping and ungrouping; groups through a session file, each with a fresh id; and, through the view, grouping as an undo step, a click selecting the whole group, a drag moving three clips on two tracks by the same 47,880 frames, Shift-click taking the group out, and ungrouping. The chain links, the menu items and the keys were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 68: freezing a track**
- Multitrack > Freeze Track renders the selected track's clips through its pan and its effects, from the session's start to 5 seconds past its last clip so tails can ring out, to a 32-bit float file in the recordings folder, and the track then plays that file instead: its effects stop running, which frees the CPU they used. The item is ticked on a frozen track and reads Unfreeze Track, which plays the clips live again. Both are undo steps.
- A frozen track keeps its volume, volume envelope, mute, solo and sends, which still act on the frozen audio. Its pan, pan envelope and effects are in the frozen audio, so changes to them are heard only after unfreezing. Its clips can't be moved, trimmed, split, muted or cleared; time edits (delete and insert time) leave the frozen audio where it is; and clips added, pasted, recorded or dropped onto it go to a new track instead. Its lane shows an icy wash where the frozen audio plays, with the word Frozen. Effect tails longer than 5 seconds are cut off.
- The frozen file is saved with the session (by relative and full path) and copied by Save a Copy with Audio. If it can't be found when the session opens, the track opens unfrozen, playing its clips, and the file is listed as missing.
- Measured by the tests, without effects (mixing as the app does, with no plug-ins): a mono track panned and sent to a bus after its volume, with a volume envelope, at 0 dB and at -6 dB, mixing bit for bit the same frozen as live; unfreezing mixing as before; a bus refused; the frozen track's clip refused moving, splitting, muting and clearing, an added clip landing on a new track and a move onto the frozen track staying put; the frozen audio through a session file, with collecting copying it and the track opening unfrozen without it; and the lane's wash in the view. Freezing through real plug-ins, the CPU saved, the menu item and the background job were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 67: keyboard shortcuts**
- Window > Keyboard Shortcuts... lists every command by menu with its keys: click a key to change or remove it, + to add one, and Reset to Defaults to go back. Changes take effect at once and are saved as they are made, as the differences from the defaults, so a later version's new shortcuts still arrive.
- Measured by the tests, with a real settings file: nothing stored while the keys are the defaults; a changed key stored, then read back by a fresh command set over its defaults (the changed command with only its new key, the other unchanged); and resetting storing nothing again. Launching the app with no changes added nothing to its settings. The window itself was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 66: loop playback**
- Transport > Loop Playback (Cmd L) makes a selection in the editor, or the time range in a session, play over and over instead of stopping at its end, through the effects rack or the tracks' effects as usual; the metronome follows. Changing the selection or range while it plays moves the loop (within a twentieth of a second), and turning the option off lets it play on to the end of the range and stop. With nothing selected, playback runs from the cursor as before. The option is remembered.
- Loop recording keeps its own loop: starting a take drops the playback loop, and an ordinary take never loops.
- Measured by the tests: the file player looping a 2,000-frame selection for 10,240 frames, every sample the one expected, on both channels; then, with the loop cleared, playing on to the selection's end and silence after it. The session player's looping was already measured (Milestone 20 and the metronome tests). The menu item, following a changed range, the key and the session side from the menu were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 65: fade shapes**
- A clip's fade in and fade out each have a shape, chosen from the clip's right-click menu (Fade In Shape, Fade Out Shape) for every selected clip: S-Curve (the raised cosine every fade used before, still the default), Linear, Equal Power (a quarter sine: two crossfading keep the power of unrelated sounds steady), Slow Start (squared) and Fast Start. Each change is an undo step; locked clips keep theirs. The lanes draw each fade in its shape, and playback, mixdowns, bounces, stems and regions all use it. Shapes other than the S-curve are saved with the session.
- Measured by the tests: each shape from 0 to 1, and half way at 0.5, 0.5, 0.7071, 0.25 and 0.75; two equal-power fades summing to a power of 1 (within 0.000001) every 10 frames across a crossfade; the mix of a steady clip following each shape in and out, every 7th frame, within 0.000001; the S-curve computed with the same expression as before, and every earlier fade and crossfade test passing unchanged; locked clips refused; the undo step from the view; and shapes through a session file, with the default left out. How the curves look, and the menu, were not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 64: recovering a session after a crash**
- While the session has unsaved changes, a copy of it (with its track effects) is written every two minutes, when it has changed since the last copy, to a Recovery folder beside the app's settings, with a note of the session's own file and the time. Not while recording, since the takes aren't in the session until they stop. Saving the session, undoing back to where it was saved, or quitting normally removes the copy.
- When SpectraBlade starts and finds a copy, it says which session it was and when the copy was made, and offers to open it. Opened, it takes the session's own file (or none, if it had never been saved) and starts with changes to save; Discard removes it. The audio the copy uses is the session's own files; changes made in the last two minutes before a crash can be lost.
- Measured by the tests: a copy written to a new folder, found with its session's file and a time between before and after writing, and opening with its tracks and markers; an untitled session's copy naming no file; clearing removing it; and a recovered session starting with changes to save. The two-minute timer, the startup question and a real crash were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 63: quantizing clips, and snapping dropped files**
- Multitrack > Quantize Clips to Grid moves each selected clip, on its own track, so it starts on the nearest line of the grid snapping uses: in bars and beats the beats (or bars, zoomed out) of the tempo map, otherwise the ruler's marks. It is one undo step ("Quantize Clip", "Quantize 3 Clips"); locked clips stay, and no clip goes before the session's start.
- Audio files dropped onto the lanes now land on the grid (or another snap target) when snapping is on, as dragged clips do; before, they landed exactly where they were dropped.
- Measured by the tests, through the view: with marks two seconds apart, clips at 0.625 s and 3.125 s moving to 0 s and 4 s; in bars and beats at 120 a minute, to 0.5 s and 3 s; a locked clip staying; a second quantize moving nothing; and a drop 900 frames past a mark landing on it with snapping on, and not with it off. None of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 62: exporting regions**
- Multitrack > Export Regions... asks for a folder and mixes each region of the session to a 32-bit float WAV of its own, as Mix Down would mix that range: every track through fresh copies of its effects for each region, so none carries another's tail. Files are named after the session and the region, numbered by the region's place in time ("Song - 01 Verse.wav"), with characters files can't hold left out; a file of the same name is replaced. Point markers are left out. It runs in the background and can be cancelled. A region's file ends where the region does: effect tails past it are not kept.
- Measured by the tests: the regions chosen and their files (ranges, order, numbering, names with a slash, points left out, none without regions). The export job itself was not run by the tests (it lives in the main window, as Export Stems does), and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 61: the marker list and exporting markers**
- Multitrack > Marker List opens a window listing the session's markers and regions in time order, with their start, and for regions their end and length, in seconds. Clicking one puts the cursor there and scrolls it into view (a region also becomes the time range), as clicking it in the marker lane does; double-clicking or Return renames it, and Delete or Backspace deletes it, each an undo step. The list follows the session, undo included.
- File > Export Markers as CSV... writes the markers and regions of the session (in multitrack mode) or of the open file to a CSV file: a header line, then each one's name, start, end and length in seconds and its start and length in frames. Names holding a comma, a quote or a line break are quoted, with quotes doubled (RFC 4180).
- Measured by the tests: the CSV text, character for character, for a point, a region and names needing quotes; the list's cells; choosing a region selecting it and scrolling to it, and a point moving the cursor; deleting a marker as an undo step with the list following; and nothing done for a marker that isn't there. The window, its keys, renaming from it and the file dialog were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 60: a tempo map**
- Multitrack > Tempo Change at Cursor... changes the tempo, the beats in a bar or both from the start of the bar the cursor is in, up to the next change; on a bar with a change it edits it, and Remove takes it out. Tempo... still sets the first bar's. Each is an undo step ("Tempo Change", "Remove Tempo Change") and is saved with the session.
- The metronome, the count-in, the bars-and-beats ruler, the lane grid, snapping and nudging all follow the changes. Each beat is still rounded to the nearest frame from the start of the session, so with no changes everything is as before. The ruler marks a change with an amber line and its tempo and beats after the bar number; its label spacing is set by the shortest beat and bar in the session, so labels stay apart at every tempo. A count-in lasts as long as the bar the punch-in is in.
- Clips, markers and envelopes stay where they are in time when the tempo changes, and time edits (Milestones 58 and 59) leave the changes at their bars.
- Measured by the tests: the map's beats, bars, tempo and beats in a bar across two changes, frame for frame; with no changes, the plain grid's beats exactly (3,005 beats); with changes at awkward tempos, every 37th frame from -100 to 800,000 inside the beat it is said to be in; grid lines every beat and every two bars; snapping to the changed beats and bars; nudging across a change in the view and the amber mark on the drawn ruler; the click from the player following a change bit for bit; changes kept in order and refused outside the limits; and the changes through a session file. The dialog was not checked by the tests, and none of it was tried by hand or heard. The whole suite passed, the default-device test included.

**Milestone 59: copying and pasting time**
- Multitrack > Copy Time copies the time range from every track and bus: the clips across it, cut to it (losing their fades on the cut sides, unlocked), each envelope's line across it (volume, pan and effect parameters), and the markers in it. Cut Time copies it, then deletes it as Delete Time does, as one undo step.
- Paste Time puts the copied time in at the cursor as one undo step: room is made as Insert Time does, each track's clips go onto the track at the same place in the list (tracks past the last are left out), its envelope lines take the place of the held values, and its markers are added. The pasted time becomes the range and the cursor moves to its end, so pastes follow each other. It is refused for a session at another sample rate.
- A volume or pan line pasted onto a track without an envelope gives it one that stays at the track's setting outside the pasted time. An effect parameter's line is pasted only where the track already has that parameter's envelope. Where the copy had no line and the track has an envelope now, it holds its value across the pasted time.
- Measured by the tests: the mix after pasting compared bit for bit with the mix before: the same up to the cursor, the copied time's mix there, and what was after moved on; a volume envelope matching the old one every 250 frames around the pasted line; a track without an envelope given one at its setting outside it; markers copied; locked clips copied unlocked; refusal at another rate and with nothing copied; a three-track copy onto a one-track session; and Cut Time and Paste Time from the view (undo steps, range, cursor). Pan and effect-parameter lines go through the same code but were not tested separately; none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 58: clearing, deleting and inserting time**
- Three commands in the Multitrack menu work on the time range across the whole session, every track and bus, hidden ones too, each as one undo step:
  - Clear Range cuts every track's clips across the range and leaves the time empty; clips are split where the range starts and ends.
  - Delete Time takes the range out: it is cleared, and the clips, envelope points and markers after it move back by its length. Envelopes keep their shape on both sides; markers inside go and regions across it shrink, as in the single-file editor. The cursor goes to where the range started, and the range goes.
  - Insert Time puts in as much empty time as the range is long, at its start: clips across it are split there, and the clips, envelope points and markers from it on move later. Envelopes hold their value at that point across the new time. The range then covers the new time.
- Locked clips stay as they are, where they are, so Delete Time and Insert Time can leave one overlapping clips that moved, or a gap.
- Measured by the tests: on a session with a send and a bus, the mix after each edit compared with the mix before, bit for bit: the same up to the range, silent across a cleared or inserted range, and the same after it, shifted by its length; envelopes (volume and pan) matching the old ones every 250 frames, shifted; markers inside a deleted range gone and later ones moved; a locked clip staying put; nothing done with no range or an empty one; and the undo steps, cursor and range from the view. Effect-parameter envelopes go through the same code but were not tested separately; none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 57: the session overview**
- In multitrack mode a strip above the lanes shows the whole session in miniature: each shown track's clips as a band in their colours (muted ones grey), the cursor, and a frame around the part the lanes show. It spans the session's end, at least a minute, and as far as the lanes show. Clicking or dragging in it centres the lanes there at the same zoom; the lanes' own limits apply (they don't start before zero). It follows the lanes' scrolling and zoom, and the cursor while playing.
- Measured by the tests, through the real strip: a ten-second session spanning the one-minute floor; a click at the middle centring the lanes on 30 s and a drag near the end on 59 s, with the zoom unchanged; a drag to the left edge stopping at zero; and the clip drawn where it lies. How it looks and feels was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 56: saving a copy with its audio**
- File > Save a Copy with Audio... asks for a new folder's name and saves a copy of the session there, with every audio file it uses (each clip's, and each take's) copied into an Audio folder beside it, so the folder can be moved or sent as a whole. A file used several times is copied once; a second file with the same name (ignoring case) is numbered ("Take 2.wav"). The open session stays as it was, pointing at its own files.
- Files the session uses that aren't there are named afterwards; their clips still point where they were.
- Measured by the tests, with real files: three files (two named take.wav and Take.wav in different folders, one used twice, one only as a take) copied byte for byte as "take.wav", "Take 2.wav" and "pass.wav", a missing file named, the originals left in place, and the copy's folder, moved elsewhere, opening with every clip and take pointing at its own Audio folder. The folder dialog was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 55: track templates**
- Multitrack > Save Track as Template... writes the selected track's or bus's settings to a .sbtrack file: its name, volume, pan, colour, input, note, sends (by the bus's name) and its effects chain, each plug-in's state, bypass and slot included. Its clips, envelopes, group, mute and solo are not kept.
- Multitrack > Add Track from Template... adds a track or bus made from one at the end of the session, as an undo step ("Add Track from Template"), and loads its effects onto it. Its sends go to the session's first bus with the same name; when there is none, the message names the bus and the track doesn't send there. A plug-in that can't be loaded is named, as when a session opens.
- Measured by the tests: a template written to a file and read back, added to a session with the bus and to one without: the settings, note, input, a send matched by name, and the effects element all kept; clips, group and mute left out; the missing bus named; and a track id or XML that isn't a template giving nothing. The file dialogs and the effects loading onto the new track were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 54: the undo history**
- Edit > Undo History opens a window listing the session's undo steps, oldest first, from "Start". The step the session is at is highlighted and the ones that can be redone are grey; clicking a step goes back or forward to it, as that many undos or redos would. A new step drops the grey ones, as it always has. The history is the session's: the single-file editor's own undo isn't listed.
- Measured by the tests, through the real list: three steps listed after Start with the last highlighted; clicking the first going back to it with the rest redoable, the last going forward again, and Start (where the session was saved) leaving it clean; a click past the end doing nothing; and a new step dropping the redoable ones. The window was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 53: shifting a clip's pitch**
- Multitrack > Shift Clip Pitch... asks for a shift from -12 to +12 semitones (fractions allowed) and moves the selected clip's pitch by it without changing its length: the part that plays is stretched by the pitch ratio with the Milestone 51 stretching, then resampled back to its length with the app's sample-rate converter. Like Stretch Clip, it runs in the background with Cancel, writes "Vocal Shifted.wav" to the recordings folder, is an undo step ("Shift Clip Pitch"), keeps the clip's place, length and fades, and is refused on a locked clip.
- Measured by the tests: a 440 Hz tone at -9.03 dBFS shifted by -12, -5, +7 and +12 semitones coming out at 220.00, 329.63, 659.26 and 880.00 Hz (each as worked out) at -9.03 dBFS, the same length, with the channels in step; cancelling part way giving nothing; and on a clip, its place, length and fades kept. Formants move with the pitch (nothing keeps them in place); music and speech were not tested or listened to; the dialog was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 52: stretching in the background**
- Stretch Clip now runs on a thread of its own behind a progress window with Cancel, as mixing down and bouncing do, so a long clip no longer holds the window up. Cancelling leaves the clip as it was and writes no file. Reversing a clip stays immediate: it only copies the audio back to front.
- Measured by the tests: the stretch's progress rising from 0 towards 1, and stopping it part way giving nothing, with every Milestone 51 measurement unchanged (10 s of stereo to 15 s took 869 ms this run). The progress window and Cancel were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 51: stretching clips**
- Multitrack > Stretch Clip... asks for the selected clip's new length, from 50% to 200% of what it is now, and makes it that long at the same pitch. The part of the clip that plays is stretched into a 32-bit float WAV of its own ("Vocal Stretched.wav") in the session's recordings folder, and the clip plays all of it from where it started; its fades scale with it and its takes go. It is an undo step ("Stretch Clip"), and refused on a locked clip.
- The stretching is waveform-similarity overlap-add (WSOLA), written from Verhelst and Roelands' 1993 paper: Hann-windowed frames of about 40 ms laid half a frame apart, each taken from within 10 ms of where it falls in the original, where it best continues the frame before (by the products of every channel, so channels out of phase don't cancel). All channels are cut at the same places.
- It runs while the window waits: 10 s of stereo to 15 s took 891 ms in the tests, so a long clip holds the window up for a while.
- Measured by the tests: lengths at 50%, 75%, 150% and 200%; a 440 Hz tone at -9.03 dBFS coming out at 440.00 Hz and -9.03 dBFS at each (measured over the middle half, by zero crossings and RMS); the right channel, the left inverted, staying exactly the inverse of the left; and on a clip, the part it plays stretched, fades scaled, and a locked clip refused. A first version found matching frames from the channels' sum, which this out-of-phase test caught (the pitch came out as much as 4.07% off). Music, speech and transients were not tested or listened to; the dialog was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 50: cutting at zero crossings**
- Multitrack > Cut at Zero Crossings (off to start with, kept in the settings) moves Split Clip's cut and a dragged clip edge to the nearest point, up to 5 ms away, where the clip's audio crosses zero (its channels summed), so the cut doesn't click. Where there is none that close, the cut stays where it was. It works after snapping: an edge snapped to the cursor can then move up to 5 ms off it.
- Measured by the tests: on a square wave changing sign every 50 frames, the nearest crossing within reach, none out of reach, none when another channel keeps the sum from crossing, and only inside the audio; through the real view, a split landing on the crossing with the option on and at the cursor with it off. Dragging an edge with the option on was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 49: closing gaps**
- Multitrack > Close Gaps moves the selected track's clips back, in time order from the first, so each starts where the ones before it end, as one undo step ("Close Gaps"). A clip that overlapped the one before it moves with it by the same amount, keeping the overlap. Locked clips stay where they are, and the clips after one close up to it.
- Measured by the tests: five clips added out of order, with a gap, a locked clip and an overlap, ending at the frames worked out by hand, a clip on another track untouched, and nothing more to do the second time; through the real view, on the selected track as one undo step. The menu item was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 48: moving clips to the cursor**
- Multitrack > Move Clips to Cursor moves the selected clips together, each staying on its own track, so that the earliest starts at the cursor, as one undo step ("Move Clips to Cursor"). Nothing moves when one of them is locked or they are already there. Nudging now shares the same code.
- Measured by the tests, through the real view: two clips on two tracks moved to the cursor keeping their 20000-frame spacing, as one undo step; nothing when already there or when locked; and the existing nudge tests still passing. The menu item was not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 47: selecting clips by track and by time**
- Edit > Select Clips on Track selects every clip on the selected track; Edit > Select Clips After Cursor selects every clip that starts at or after the cursor, on every track. Select All Clips, and both of these, now leave out clips on hidden tracks.
- Measured by the tests, through the real view, on three tracks of three clips with one track hidden: all clips, a clicked track's clips and the clips after the cursor, none from the hidden track. The menu items were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included.

**Milestone 46: notes on tracks and clips**
- Clip Properties has a Note field: any text, or none to clear it, as an undo step ("Clip Note"). Locked clips take notes too. A clip's note is drawn along its bottom edge when the lane is tall enough.
- Right-click a track's or bus's header and choose Note... to type its note ("Track Note"); hovering over the track's name shows it.
- Tooltips now show: the app had no tooltip window, so none of its controls' tooltips (these notes, and the existing ones on the header buttons and elsewhere) ever appeared.
- Measured by the tests: a clip's note typed into the real Clip Properties, trimmed, and cleared as undo steps, on a locked clip whose start is still refused; a track's note as an undo step, as its name's tooltip, and the same note again adding no step; session files keeping both, with "&" and "<" in them. The Note... dialog, the drawn note and tooltips appearing on screen were not checked by the tests, and none of it was tried by hand. The whole suite passed, the default-device test included: the audio interface opened normally again.

**Milestone 45: hiding tracks**
- Right-click a track's or bus's header and choose Hide, or use Multitrack > Hide Track on the selected one, to leave it out of the lanes; the tracks below close up. It still plays, keeps its place in the list and stays in the mixer window. View > Show All Tracks shows every hidden track again as one undo step; hiding and showing a track are undo steps too ("Hide Track", "Show Track").
- A hidden track is no longer the selected track, and its clips leave the selection, so commands for the selection don't act on what can't be seen. Going to one of its clips from the clip list shows the track again.
- Measured by the tests, through the real view: hiding a track dropping its clip from the selection and its header, and a click in its old row picking the track below; going to its clip from a list showing it again; Show All Tracks as one undo step, and nothing to do when none is hidden; session files keeping which tracks are hidden; and the mix with every track and bus hidden bit for bit the same. The header menu item and the menu commands were not checked by the tests, and none of it was tried by hand. The default-device test still hung opening the audio interface, as for Milestone 40; every other check passed.

**Milestone 44: duplicating a track**
- Multitrack > Duplicate Track puts a copy of the selected track or bus right after it, named "Name Copy", and selects it: its clips (sharing their audio, as unlocked copies), sends, envelopes, volume, pan, mute, solo, group, colour and input, as one undo step ("Duplicate Track"). Its effects are copied too, loaded after it lands from the source chain as a session file holds it (each plug-in's state, bypass and slot, so effect envelopes still find their effects); one that can't be loaded is named in a message.
- Undoing the duplicate removes the track; its copied effects stay loaded but unused, as they do for a deleted track. Duplicating a bus doesn't copy the sends other tracks make to it.
- Measured by the tests: the copy right after its source with every setting, the send, an envelope and its clip (unlocked, with new ids, sharing the audio), rendered alone bit for bit the same as the source; through the real view, the copy as one undo step and selected. Copying the effects was not checked by the tests (it runs in the app's window code), and none of it was tried by hand. The default-device test still hung opening the audio interface, as for Milestone 40; every other check passed.

**Milestone 43: the clip list**
- Multitrack > Clip List opens a window listing every clip in the session in time order (clips that start together by track): its track, name, start and length in seconds, file, and whether it is locked or muted. The selected clip's row is highlighted.
- Clicking a row selects that clip alone, with its track, puts the cursor at its start, and scrolls the timeline to it (a tenth of the way in, at the same zoom) and the lanes to its track when it is out of view. The list follows edits and undo as they happen.
- Measured by the tests, through the real list and view: the rows, their order and every column; clicking a row selecting the clip, setting the cursor and scrolling to a clip 10 minutes in; the list following a rename, its undo and a deletion, the highlight staying on the selected clip. Sorting by other columns is not offered; the window and its resizing were not checked by the tests, and none of it was tried by hand. The default-device test still hung opening the audio interface, as for Milestone 40; every other check passed.

**Milestone 42: clip colours and the clip menu**
- Right-clicking a clip now always opens a menu: its takes (when it has more than one; greyed out while it is locked), Colour, Lock and Mute. Right-clicking a clip that is part of a selection keeps the selection, so Colour, Lock and Mute work on every selected clip.
- Colour gives the clips one of the eight track colours, or Track's to follow their track again, as one undo step ("Clip Colour"). Locked clips can be recoloured. A muted clip is still drawn grey.
- Measured by the tests: a clip in its track's colour until it has its own, locked or not, and a value outside the palette meaning the track's; through the real view, two selected clips recoloured as one undo step; session files, saved and loaded with real audio, keeping a clip's colour and leaving another on its track's. The menu itself and the drawing were not checked by the tests, and none of it was tried by hand. The default-device test still hung opening the audio interface, as for Milestone 40; every other check passed.

**Milestone 41: muting clips**
- Multitrack > Mute Clip mutes the selected clips, or unmutes them when they are all muted, as one undo step ("Mute Clip", "Unmute 2 Clips"). A muted clip stays where it is, drawn grey with "(muted)" after its name, and can be edited as usual, but it isn't heard: not in playback, mixdowns, bounces or stems, nor through its track's sends.
- Copies of a muted clip are muted too. A track whose clips are all muted still gets a stem, a silent one.
- Measured by the tests: a muted clip mixing bit for bit as if it were removed, its send included; copies staying muted, and a bounce of the track leaving it out bit for bit; through the real view, muting and unmuting as undo steps and a muted clip still dragging; session files, saved and loaded with real audio, keeping which clips are muted. The grey drawing and the menu item were not checked by the tests, and none of it was tried by hand. The default-device test still hung opening the audio interface, as for Milestone 40; every other check passed.

**Milestone 40: locking clips**
- Multitrack > Lock Clip (Cmd Shift L) locks the selected clips, or unlocks them when they are all locked, as one undo step ("Lock Clip", "Unlock 2 Clips"). A locked clip shows a padlock in its title bar and no fade handles.
- A locked clip can still be clicked, selected, copied and renamed, but it can't be dragged, trimmed, faded, Command-dragged for gain, nudged, split, reversed, normalized, given another take or deleted. Selected with other clips, it keeps them from being dragged or nudged too; Delete and Cut remove the others and leave it.
- Clips pasted, duplicated, repeated or recorded over a locked clip overlap it rather than cutting it, and a crossfade leaves its fades alone. Copies of a locked clip start unlocked. Deleting its track still deletes it.
- Measured by the tests: moving, trimming, splitting, gain, fades, reversing, crossfading and removing refused on a locked clip (normalizing and choosing a take were not tested); a copy placed over one overlapping it, unlocked, and a crossfade leaving its fades; session files saved and loaded with real audio keeping which clips are locked; through the real view, dragging a locked clip only selecting it, a group with one in it not dragging or nudging, Delete removing only the unlocked clip, and unlocking as an undo step after which it drags again. The padlock and the menu item were not checked by the tests, and none of it was tried by hand. The default-device test didn't finish on this build: opening the audio interface hung in CoreAudio, inside JUCE's device setup, after two test runs had opened it at once. Every other check passed.

**Milestone 39: track colours**
- Right-click a track's or bus's header (its background) and choose Colour: Automatic, or one of eight (Teal, Blue, Violet, Orange, Green, Red, Yellow, Pink). The track's header, its clips in the lanes and a bar along the top of its mixer strip take that colour. Choosing a colour is an undo step ("Track Colour").
- Automatic is what tracks had before: the first six colours in turn by the track's place in the list, so an automatic track's colour changes when it moves, and grey for buses. A chosen colour stays with the track as it moves.
- Measured by the tests: the automatic colours by place, grey buses, and a chosen colour overriding both; choosing one as an undo step, and one outside the palette going back to automatic; session files keeping a chosen colour and leaving the rest automatic. The Colour menu and the colours drawn in the lanes, headers and mixer were not checked by the tests, and none of it was tried by hand.

**Milestone 38: track height**
- View > Taller Tracks and Shorter Tracks (Cmd Option = and Cmd Option -) change the height of every track's lane and header, 24 pixels at a time, from 56 to 312 (104 to start with). Waveforms and envelopes fill the taller lanes; in short ones a header keeps its top row (name, FX, R, M, S) and leaves out the rows that don't fit. The height is kept in the app's settings.
- Measured by the tests, through the real view: at 200 pixels a click 150 pixels down the second lane selects that lane's clip, both headers are 200 pixels tall, the height stays within 56 to 312, and at the shortest the headers' sliders get no room. The keys and how the lanes look were not checked by the tests, and none of it was tried by hand.

**Milestone 37: pre-roll and post-roll**
- Transport > Recording Options... sets the pre-roll (how long playback runs before a punch-in) and the post-roll (how long recording goes on after a punch-out before it stops by itself), each from 0 to 10 seconds. They are kept in the app's settings and apply to punch recording in the editor and in the session. The defaults are what the app did before: 2 s of pre-roll and a quarter of a second of post-roll.
- A count-in still replaces the pre-roll with a bar of clicks, and a loop recording still runs until you stop it.
- Measured by the tests: the frame playback starts from at the defaults and after a change, never before the session's start; when a punch-out recording stops, to the frame, never without a punch-out; and values past 0 to 10 s held within it. The dialog was not run by the tests, and recording with other pre- and post-rolls was not tried by hand.

**Milestone 36: repeating and reversing clips**
- Multitrack > Repeat Clip to Range End lays copies of the selected clip end to end after it, up to the end of the time range, over whatever was on the track there; the last copy is cut short at the range's end. All the copies are one undo step ("Repeat Clip (3 copies)"). Copies share the clip's audio.
- Multitrack > Reverse Clip writes the part of the selected clip that plays, back to front, to a 32-bit float WAV ("Vocal Reversed.wav") in the session's recordings folder, and the clip plays that instead, in the same place; its fade in and fade out change ends. Other takes of the clip are dropped. It is one undo step; undo goes back to the original file, which is untouched. The file is written on the main thread, so a long clip holds the window for the moment it takes.
- Measured by the tests: a 300-frame clip repeated to a range's end gives two copies, the second cut short, and the clip that was in the way cut back; repeating to a point before the clip's end does nothing; through the real view, the selected clip repeated to the time range's end as "Repeat Clip (2 copies)"; a trimmed stereo clip reversed bit for bit, in place, with its fades swapped. The Reverse Clip command itself (writing the file) was not run by the tests, and none of it was tried by hand.

**Milestone 35: clip properties**
- Multitrack > Clip Properties (Cmd Shift I) opens a small window, kept above the others, that follows the selected clip: its file (and take), name, start and length in seconds, gain in dB, and fade in and out in seconds. Type a value and press Return, or click away, to apply it as an undo step ("Move Clip", "Trim Clip", "Clip Gain", "Clip Fade In", "Rename Clip"...); Escape puts the field back. A value that can't apply (not a number, a start before 0, a length of 0) shows the clip's setting again, and lengths and fades are limited as when dragging. Undo and edits elsewhere show in the window at once.
- Measured by the tests, through the real component: nothing shown without a selected clip, then a clip's settings once it is selected; typing a start, length, gain, both fades and a name, each as the right edit and undo step; text that isn't a number and a negative start refused; undo showing at once; a clip that goes away no longer shown. The window and its menu item were not checked by the tests, and none of it was tried by hand.

**Milestone 34: exporting stems**
- Multitrack > Export Stems... asks for a folder and writes every track that has clips to a 32-bit float WAV of its own, as it goes to the mix: its clips through fresh copies of its effects, its volume, pan and automation. They cover the time range, or the whole session from its start, so they line up when imported together. Files are named "Song - 01 Drums.wav" after the session and the track, numbered in order; a file of the same name in the folder is replaced. It runs in the background and can be cancelled.
- A muted track's stem is written as it would play unmuted, and solo is ignored. Buses get no stems, and a track's sends are not in its stem, so with buses the stems don't add up to the mix.
- Measured by the tests: stems for the tracks with clips only (an empty track and a bus left out), their file names (a "/" in a track name left out), and three stems, one from a muted track, adding up to the mix of the same tracks playing, bit for bit. The command itself (the folder chooser, the background job, the files written) was not run by the tests, and none of it was tried by hand.

**Milestone 33: nudging by the grid**
- Multitrack > Nudge Earlier and Nudge Later (comma and period) move the selected clips together so that the one clicked last starts on the grid point before or after where it is: the ruler's marks in seconds, or the beats in bars and beats, at the current zoom. With no clip selected they move the cursor the same way. Nothing goes before the start of the session.
- A run of nudges is one undo step ("Nudge Clip" or "Nudge Clips").
- Measured by the tests, through the real view: the grid points either side of a frame at a 2 s ruler spacing and at 120 beats a minute (never before 0); the cursor nudged on to the next mark; two selected clips on different tracks nudged twice together, undone in one step, and nudged back until the earlier reaches the start. The keys were not pressed by the tests, and none of it was tried by hand.

**Milestone 32: track groups**
- Right-click a track's or bus's header (its background) and choose Group > Group 1 to 8, or None. The header shows its group ("G1"). Joining or leaving a group is an undo step.
- In a group, moving one track's volume, from its header or its mixer strip, moves the others' by as many dB; muting or soloing one mutes or solos them all. Pan, sends and effects stay each track's own. A volume that reaches -60 or +12 dB stops there, so a group pushed past an end no longer keeps its spacing on the way back; a track whose volume is on an envelope is left to its envelope. Groups are saved in the session file.
- Measured by the tests: a volume change moving the rest of the group by as many dB, a track stopping at -60 dB, one on an envelope and one outside the group untouched; mute and solo going to the whole group; through the real mixer window, a fader and a mute button moving the group with its strips showing it; joining and leaving a group as undo steps; groups in session files. The right-click menu and the G tag were not checked by the tests, and none of it was tried by hand.

**Milestone 31: moving tracks**
- Drag a track's or bus's header up or down by its background (not by its controls) to move it in the list; a yellow line shows where it will land. Multitrack > Move Track Up and Move Track Down (Cmd Option Up and Down) move the selected one a place at a time. Each move is an undo step ("Move Track"), and the moved track stays selected.
- A track keeps everything as it moves: clips, effects, sends, envelopes, armed state and input. The mixer window's strips follow the new order.
- Measured by the tests: moving a track down and up the list closes the others up behind it, and moves out of range or onto itself are refused; Move Track as an undo step with the track still selected; and through the real view, a header dragged by its background down two lanes moves its track there. The menu commands and the drop line were not checked by the tests, and none of it was tried by hand.

**Milestone 30: bouncing a track, and zooming to the time range**
- Multitrack > Bounce Track renders the selected track as it goes to the mix (its clips through its effects, volume, pan and automation) over the time range, or from the start to its last clip, into a 32-bit float WAV in the session's recordings folder. The bounce lands as one clip on a new track right after the source, named "Guitar Bounce" after it, and the source is muted, so the session sounds the same with the plug-ins' work done. It runs in the background with a progress bar and can be cancelled; the whole bounce is one undo step.
- The new track takes over the source's sends, now fed with the bounced audio. A post-fader send carries the same signal as before, but a pre-fader send now carries the track after its volume, so it changes unless the volume was 0 dB.
- Bouncing uses fresh copies of the track's effects, as mixing down does; one that can't be loaded is left out and named afterwards.
- View > Zoom to Time Range (in place of Zoom to Selection while the session is showing) fills the timeline with the time range and 2.5% of it either side.
- Measured by the tests: a track bounced (rendered alone, then landed) leaves the session mix bit for bit the same, through a bus it sends to, when its volume is 0 dB; at -6 dB the largest difference is 0.0000000298, from the send now scaling the bounced audio instead of the track's. The bounce lands after its source with its sends, the source muted; a muted track bounces as it would play; and Zoom to Time Range shows 93600 to 194400 for 96000 to 192000. The Bounce Track command itself (its background job, the file it writes, a track with plug-ins on it) was not run by the tests, and none of it was tried by hand.

**Milestone 29: selecting several clips**
- Shift-click clips to add them to the selection (Shift-click a selected one to take it out); Edit > Select All Clips (Cmd A) selects every clip in the session. Clicking where there is no clip selects none.
- Drag any selected clip to move them all in time by the same amount, each staying on its own track; they stop when the earliest reaches the start of the session. With snapping on, the clip you drag snaps, and the other selected clips are not targets. With crossfading on, each one crossfades where it lands.
- Cut, Copy, Paste, Duplicate Clip and Delete work on every selected clip as one undo step ("Paste 2 Clips"). Pasted clips keep their places relative to each other: the earliest lands at the cursor, the topmost on the selected track and the others on the tracks below it (new tracks past the last). Duplicate places the whole selection right after itself.
- Commands for one clip (Split, Clip Gain, Normalize Clip, choosing a take) work on the clip clicked last.
- Measured by the tests, through the real view: Shift-click selecting and deselecting; two clips on different tracks dragged together, by the same amount and stopping at the start; two clips copied and pasted onto a lower track (the second onto a new track, the cursor after them), duplicated and deleted together; Select All; a click on empty space. Dragging a selection across tracks is not supported: a selection of several moves only in time. None of it was tried by hand.

**Milestone 28: copying, pasting and duplicating clips**
- While the session is showing, Edit > Cut, Copy and Paste (Cmd X, C, V) work on clips: copy the selected clip, click a track, place the cursor and paste. The copy goes over what is on the track there, which is cut around it as for a punch-in; it is selected and the cursor moves to its end, so pasting again lays copies end to end. Pasting onto a bus, or with no track selected, makes a new track.
- Multitrack > Duplicate Clip (Cmd D) places a copy right after the selected clip on its track and selects it, so pressing it again makes a row.
- Copies share the clip's audio (nothing is written to disk) and keep its trim, gain, fades and takes. Each paste, cut and duplicate is an undo step. A clip copied from a session can't be pasted into one at another sample rate.
- Measured by the tests, through the real view: duplicating a clip with gain and fades, pasting twice onto a track holding one long clip (cut around the first copy, the rest taken by the second, the cursor following), cutting, the other-rate rule, and a clip placed on a bus going to a new track. The menu commands themselves were not run by the tests, and none of it was tried by hand.

**Milestone 27: clip gain**
- Command-drag a clip up or down to change its gain, 0.25 dB a pixel in steps of 0.1 dB, from -60 to +24 dB; the clip stays where it is. The waveform follows the gain, and a clip's title shows it when it isn't 0 dB ("+3.5 dB"). Each drag is one undo step ("Clip Gain").
- While the session is showing, the Effects menu's first two items work on the selected clip: Clip Gain... types in its gain, and Normalize Clip... sets the gain so that the loudest sample of the part the clip plays reaches a peak level (-0.1 dBFS unless you change it). Neither touches the clip's file.
- Measured by the tests: normalizing a trimmed stereo clip to -1 dBFS gives exactly the gain a brute-force scan of the part it plays calls for, ignoring a louder sample trimmed off; a silent clip is left alone; the gain stays within its limits. Through the real view, a clip Command-dragged 20 pixels up gets +5 dB as "Clip Gain", and 8 pixels down while moving sideways -2 dB, without moving. The two menu items and their dialogs were not run by the tests, and none of it was tried by hand.

**Milestone 26: markers in sessions**
- Sessions have markers and regions, as files do, shown in a lane under the multitrack ruler with a line down through the tracks. The Markers menu works on the session while it is showing: Add Marker (M) marks the play position while playing or recording, otherwise the cursor, or makes a region of the time range; Previous and Next Marker (Option Left and Right) move the cursor to the nearest marker or region edge; Delete All Markers removes them. Adding and deleting are undo steps.
- In the lane, click a marker to move the cursor to it, or a region to select its range; drag one to move it; double-click to rename it; right-click to rename or delete it.
- With snapping on, markers and both ends of regions are snap targets for anything dragged, and a dragged marker or region snaps to clip edges, the other markers and the ruler's marks.
- Markers are saved in the session file, and a mixdown to WAV carries the markers and regions inside the mixed range as cue points (AIFF keeps the points).
- Measured by the tests, through the real view: Add Marker at the cursor and over a time range, as undo steps; Next and Previous Marker stopping at each marker and region edge; clicking a marker and a region; a marker dragged to 3 pixels past a clip's end landing on it as "Move Marker 01"; a clip dragged near a region's start snapping to it, and not once the markers are gone; the marker being dragged not snapping to itself; markers in session files. Not checked by the tests: the markers in a mixdown (the app does it with the tested crop and file code, but the mixdown command itself is not run by the tests), renaming and the right-click menu; none of it was tried by hand.

**Milestone 25: a mixer window**
- Multitrack > Mixer (Cmd Shift M) opens a window with a strip for each track and bus, in the session's order: its name, pan, mute and solo, a volume fader (-60 to +12 dB, double-click for 0 dB) with its level in dB, and a meter. A master strip on the right meters what goes to the outputs, after the effects rack. With many tracks the strips scroll sideways.
- The strips edit the session just as the track headers do: each change is an undo step ("Track Volume", "Track Pan", "Mute", "Solo"), a fader's drag one step, and undo, redo or an edit elsewhere shows on the strips at once. A track or bus with a volume or pan envelope follows it as the session plays, and its fader or pan can't be moved here.
- Track meters show the peak of each side after the track's pan and volume (a muted track shows nothing), bus meters after the bus's volume and pan, measured as the session mixes. The meters fall back over about half a second; a red line marks anything over 0 dB until you click the meter.
- Measured by the tests: through the mixer, the peaks of a mono and a stereo track, a bus fed by a send and a muted track are exactly the levels the mixer computes for them; under a volume envelope the peak follows the envelope. In the mixer window, the strips show each track's volume, pan and mute; a fader, pan, mute and solo edit the session as undo steps, undo shows on the strips, an automated volume can't be moved, a new track gets a strip, and a meter shows its track's peak and falls back. The master meter was not checked by the tests (they play only silence), and the window was not tried by hand.

**Milestone 24: bars and beats**
- Multitrack > Bars and Beats (B) counts the ruler in bars and beats of the session's tempo ("5" is bar 5, "5.3" its third beat), labelled every beat when zoomed in and every bar, or 2, 4, 8... bars, further out. The bar and beat lines over the tracks now come with this option rather than with the metronome.
- With snapping on, anything dragged (a clip, its edges, a fade, an envelope point, the time range) snaps to the beats instead of the ruler's seconds while beats are at least 8 pixels apart, and to the labelled bars when zoomed out further. Beats are the frames the metronome clicks on, so a clip snapped to a beat starts with its click. Clip edges, the cursor and the time range stay targets too, and the nearest wins.
- Measured by the tests: at 133.7 a minute and 44.1 kHz, frames near a beat snap to the beat's frame and ones further than the tolerance stay put; on a grid of three-beat bars only bar starts are targets; a nearer target still wins, and a moved span snaps by its nearer edge. Through the real view at 100 a minute, a clip dragged to about 57000 lands on beat 2 at 57600 in bars and beats, and at 57072 with seconds on the ruler. The ruler's labels and the lines were not checked by the tests or by hand.

**Milestone 23: a metronome and count-in**
- Multitrack > Tempo... sets the session's tempo (20 to 400 beats a minute) and beats in a bar (1 to 16); both are saved with the session, and a change is an undo step. A new session is 120 a minute in four.
- Multitrack > Metronome (C) clicks on every beat while the session plays or records, with a higher, louder click on the first beat of each bar, and draws the bars (and the beats, when zoomed in enough) over the tracks. Beats fall on the tempo from the start of the session, each on the nearest frame, so the grid never drifts. Through a loop, the clicks follow the loop.
- The click goes to the output pair straight from the player, past the effects rack, so the rack's effects don't color it; the processing graph delays it by the rack's latency to keep it in time with the music. It is not recorded or mixed down.
- Multitrack > Count-In records after one bar of clicks instead of the 2 s preroll: playback starts a bar before the punch-in (or the cursor), and the clicks run until the punch-in even with the metronome off.
- The click needs the interface at the session's sample rate (as recording does); at another rate there is none.
- Measured by the tests, through the real player: at 137 a minute in three, every click is bit for bit the click expected at its session frame, the bar's first accented, on its own outputs, with the mix bit for bit the same as without the metronome; a count-in clicks only up to its end; through a loop the clicks follow the session's beats; at another device rate there is no click. Also checked: the beat grid at 133.7 a minute and 44.1 kHz (every frame falls in the last beat starting at or before it, beat 1000 exactly where the tempo puts it), the click's shape, and the tempo in session files. Not tried by hand: hearing the click through a real interface, the count-in, the tempo dialog, and the bar lines.

**Milestone 22: recording several tracks at once**
- Arm as many tracks as you like with their R buttons. Each track's In button chooses what it records from: the recording input chosen at the bottom of the window (the default), any stereo pair of the interface's inputs, or any single input. The choice is saved with the session, and changing it is an undo step.
- Record then puts a take on every armed track, each from its own input, over the same range (or loop, as takes); with none armed, the take still goes onto a new track. Tracks recording from the same input share one file. All the takes land as one undo step ("Record 3 Tracks").
- Each input other than the recording input gets a recorder of its own. Every recorder pairs its input with the frame the player sent in the same audio cycle, so all the takes line up with each other and with the session. The armed tracks' inputs are turned on as you arm them, which may restart the audio device for a moment.
- Input Monitoring now plays each armed track's own input through that track (up to 16 tracks).
- Measured by the tests: a punch-in through the real player, looped back into two recorders at once (inputs 1-2 with the session's output, input 3 with minus half its left side), lands on three tracks, each take bit for bit what its input carried; the two tracks recording from inputs 1-2 share one take and file, and the unarmed track is untouched. Two monitored tracks each play their own input, panned by their own pan, bit for bit. Also checked: a take that can't be saved lands nothing, the undo step names, track inputs in session files, and in the view, arming several tracks (not a bus), choosing a track's input as an undo step, and a deleted track no longer armed. Recording several inputs of a real interface was not tried by hand, and the integration tests don't turn on more than one input.

**Milestone 21: input monitoring through a track's effects**
- Turn on Multitrack > Input Monitoring (I) to hear the recording input through the armed track: it joins the track's clips before its effects, so you hear it through the effects, volume, pan, sends and buses, and through the output rack. It works while stopped, playing and recording. Takes are still recorded dry, straight from the input.
- Monitoring needs a recording input (Input, at the bottom of the window) and the interface at the session's sample rate; with no track armed, or the armed track muted, nothing is monitored. Use headphones with a microphone: through speakers it feeds back.
- What you hear comes later than the input by the track's effects' latency, plus the most latency among the buses it sends to (the mixer lines everything up to the slowest path), plus the output rack's latency, 3 frames the player reads ahead, and the interface's own input and output latency. While playing, the track's clips play under the input as before, including over a range being recorded.
- Measured by the tests, through the real player with a test effect that delays by 64 frames and halves: stopped, the output is the input through the track's pan and the effect, bit for bit, 64 frames late; playing, every frame is the session mix plus that, bit for bit, 67 frames late. Off, on a muted track, or with the device at another rate, nothing of the input plays; a stereo input on a centred track without effects plays unchanged. The tap that hands the device's input to the player was not run with a real interface, and monitoring was not tried by hand.

**Milestone 20: loop recording into takes**
- Turn on Multitrack > Loop Recording (L), select a time range and record: after the preroll the session plays the range over and over, and each pass is kept as a take. Stop when you like; a pass cut short by stopping is dropped, unless it is the only one.
- The passes land as one clip over the range, playing the last pass; what the armed track had there is cut around it, as for a punch-in. All the passes are saved in one 32-bit float WAV, and the whole recording is one undo step ("Loop Record (3 takes)").
- Right-click the clip to choose another take; the one playing is ticked, and the clip's title says "take 2 of 3". Each take stays in time with the range, so you can trim, move or split the clip first: after a split, each half chooses its own take. If a take has no audio for part of the clip, the clip is shortened to where it has. Choosing is an undo step, and takes and the one chosen are saved in the session file.
- Measured by the tests: the session's own output, looped back with 731 frames of latency into the recorder while the real player looped an 11,006-frame range three and a half times, gives three complete passes one loop length apart, each bit for bit equal to the session mix over the range; the half pass is dropped. Also checked: a lone pass cut short is kept, choosing each take plays its pass in place, takes after a split, the clip shortened at either end, session files (and one with a take's file missing, which loads the clip without takes), and choosing through the view as one undo step. Loop recording with a real interface was not tried by hand.

**Milestone 19: writing effect parameters while playing**
- With Multitrack > Write Automation on and the session playing, grab a control in a track effect's own window and turn it: from when you grab it to when you let go, what you do is written into that parameter's envelope, which is shown over the lane. While you hold the control, the envelope leaves the parameter to you; when you let go, it goes back to what it had over 0.1 s (for a parameter with no envelope yet, to the value it had when you grabbed it). Held still, the control keeps writing its value. Each pass is one undo step.
- This needs effects that report when a control is grabbed and let go (begin and end of a gesture), as most Audio Units and VST3 effects do. Values an effect is given from its envelope are not taken for your moves, even from effects that report every change.
- Measured by the tests, with a test plug-in: grabbing, turning and letting go is reported in order with the effect, slot and parameter; following an envelope reports nothing, even when the plug-in echoes every change; a touched parameter is held off its envelope (the mix is bit for bit the hand's value) and follows it again once let go. Through the real view with Apple's AUDelay on a track: a grab while stopped writes nothing, and a grab, turn and release while playing writes exactly the expected points as one undo step. Turning a real effect's control with the mouse in its window during playback was not tried by hand.

**Milestone 18: automating effect parameters**
- A track's or bus's A menu lists each of its effects, with that effect's parameters in its own order (automated ones say so). Pick one to show its envelope over the lane, labelled with the effect, the parameter and its value, and edit it like a volume envelope: click the line to add a point (at the parameter's current value, the first time), drag, Option-click to delete. Clear Shown Effect Envelope removes it. Envelopes are saved in the session file.
- While an envelope has points, it sets its parameter as the session plays, in steps of 64 frames (1.3 ms at 48 kHz) that line up with session time, so the result doesn't depend on the audio interface's block size. An effect after others in its chain works on audio their latency has delayed, and its parameters follow their envelopes from that earlier time, so automation stays lined up with the audio. Without points, the parameter is left where the effect's window has it.
- An envelope belongs to its effect, not its place in the chain: each effect slot has an id, saved with the chain, so reordering the chain keeps every envelope on its effect. If the effect is removed, its envelopes are kept unused, and the lane says the effect is no longer on the track. A bypassed effect ignores its envelopes. Mix Down follows effect envelopes too.
- Measured by the tests, with a test plug-in whose gain is its parameter: on a ramp, the mix is bit for bit the gain the envelope gives at each 64-frame step, in 1-, 37-, 512- and 3000-frame blocks and from part way in; after a 300-frame delay in the chain, and moved ahead of it, it still lines up bit for bit. Also checked: no envelope leaves the parameter untouched, an envelope for a removed effect plays nothing, slot ids are kept and never repeated, session files round trip, fresh copies of Apple's AUDelay in a mixdown keep their slot ids and envelopes on its 4 parameters change the mix, and, through the real view, a click on the line of an AUDelay parameter adds a point at its current value and the envelope is drawn.
- Automation steps are not sample-accurate: a parameter changes at most once every 64 frames.

**Milestone 17: writing automation while playing**
- Multitrack > Write Automation (W) turns writing on. While the session plays, dragging a track's or bus's volume or pan control writes what you do into its envelope, and you hear it as it is written. Held still, the control keeps writing its value as playback moves on. When you let go, the envelope goes back to what it had over 0.1 s, so the rest of it is untouched. On a track with no envelope yet, its setting holds before and after the pass.
- With writing on, automated controls stay live, and while the session plays every automated control shows its envelope's value at the play position. Each pass is one undo step. A pass ends when you let go, when playback stops, or when it jumps back in time.
- Points land at the player's position when the control moves, without allowing for the audio interface's output latency, so a pass sits that much earlier than what was heard when it was made.
- Measured by the tests: over a ramp, the envelope outside a pass and its return is unchanged (worst 0.0000038 dB, float rounding of the new interpolation). Through the real view: a pass with the control moved, held and moved again makes exactly the points expected (a held value moves its last point instead of adding more), a second pass adds to the envelope as its own undo step, and a jump back ends a pass. The controls' wiring to the player was not tested by hand in the running app.

**Milestone 16: crossfades between overlapping clips**
- Move or trim a clip so that it partly overlaps another on its track and the two crossfade: the later clip fades in and the earlier fades out across exactly the overlap, as raised-cosine curves whose sum stays at the clips' level (to within a frame's offset, below). A clip overlapping clips on both sides crossfades with each. Clips wholly inside another are left alone, as are the fades of clips that stop overlapping. The crossfade comes with the move or trim as one undo step, and both curves are drawn across the overlap.
- Multitrack > Crossfade Overlaps (X) turns this off and on; it is on by default and remembered.
- Measured by the tests: two clips at the same steady level stay at that level through the crossfade to within the curves' one-frame offset, sin(π/2N) for an N-frame overlap: worst 0.000327 (under 0.003 dB) for 4800 frames, 0.003272 for 480. Also checked: which fades are set and to what, a clip inside another, a clip between two others, and, through the real view with synthesised mouse events, a clip moved over another's end crossfading by the frames they share, and not with crossfading off.

**Milestone 15: mixing a session down to a file**
- Multitrack > Mix Down (Cmd+Shift+E) writes the session to a WAV, AIFF or FLAC file: the time range if there is one, otherwise the whole session from its start to the end of its last clip. Choose the sample format (32-bit float, or an integer depth with dither) and a tail of up to 10 s after the end, so effects such as reverbs can ring out.
- The mixdown is what playback plays: every track's and bus's effects, sends, envelopes, mute and solo, with delay compensation, primed so a range that starts part way in is already in time. It runs behind a progress window on fresh copies of the effects, set for offline processing, so playback and the live effects are left alone. An effect that can't be loaded is left out, and the file is written with a note naming it.
- Measured by the tests: the whole session mixes down bit for bit like playback's mix. A range starting part way in, with effects delaying a track by 300 frames, a bus by 700 and another track by 55, matches playback from the start bit for bit. Fresh copies of Apple's AUDelay load in order with their bypass and are heard in the mix, an effect that can't load is named and left out, a 32-bit float file reads back bit for bit, and the mixdown can be cancelled.

**Milestone 14: automation, fade handles and snapping**
- Each track and bus has an A button: show its volume or pan envelope over its lane, or clear one. Click the line to add a point, drag a point to move it (it stays between its neighbours), Option-click a point to delete it. Pan envelopes have left at the top. Every change is one undo step, and envelopes are saved in the session file.
- While a track has an envelope, it governs that parameter and the fader or pan control is greyed out, showing the envelope's value at the cursor. Clearing the envelope hands control back to the fader.
- The mixer follows envelopes sample by sample, with straight lines between points (in dB for volume). Two points at the same time make a step. A track's pan envelope applies where its clips are panned, and its volume envelope after its effects, so pre-fader sends ignore it as they ignore the fader. A bus's envelopes apply at its output. Effect latency is compensated, so an envelope acts on the audio it lines up with.
- Clips have a fade handle at each top corner: drag it to set the fade in or out. Fades are drawn as the raised-cosine curves they play.
- Multitrack > Snapping (S) is on by default and remembered. When something dragged comes within 8 pixels of a target, it lines up with it exactly: clip moves (by whichever edge is nearer a target), trims, fade handles, envelope points and time-range selection. The targets are clip edges, the cursor, the time range's edges, the session start and the ruler's marks.
- Measured by the tests: constant pan envelopes (on tracks and a bus) and a constant bus volume envelope play bit for bit like their settings. Constant track volume envelopes match to float rounding (worst under 6e-8); the envelope is a separate multiply, where the fader's gain is fused into the add. A 0 to -40 dB ramp scales every frame by the envelope's gain there (worst under 7.5e-9). With envelopes on tracks and a bus, the mix is bit for bit the same in 1-, 37- and 512-frame blocks, with 300 and 500 frames of effect latency, and after a jump. A pan step moves a mono track from left to right at the step.
- Also checked, through the real view with synthesised mouse events: dragging fade handles, a fade snapping to the cursor, a clip snapping to another's end (and not snapping with snapping off), and adding, moving and deleting envelope points. The envelope maths, fade limits, snap rules and the file round trip are tested on their own.
- Not yet: automating effect parameters (since added in Milestone 18).

**Release 0.1.0: the first build to install**
- `dist/SpectraBlade-0.1.0.dmg` holds a universal app (Apple silicon and Intel), built for macOS 13 or later and tested on macOS 27. See [Installing](#installing), and [Packaging](#packaging) to make it.
- VST3 and Audio Unit plug-ins are checked when they are scanned. Plug-ins > Scan for Plug-ins loads each one in a worker copy of the app, plays 16 blocks of noise (or a note, for instruments) through it at 48 kHz, requires every output sample to be finite, and reads and restores its state. Four plug-ins are checked at once, each in its own worker.
- A plug-in that crashes, hangs for 60 s or fails the check is marked not working and listed at the end of the plug-in list. The reason goes to the log (`~/Library/Logs/SpectraBlade/SpectraBlade.log`), and the scan carries on: nothing stops to ask, and no crash report appears. Retry Not Working checks those again.
- The reasons are plain: "it crashed while loading or playing", "it took longer than 60 s to load and run", "it played invalid samples", "no plug-in was found in it", the plug-in's own error, or, for a VST3 or Audio Unit built only for Intel Macs, "it has no code for Apple silicon" (it may work with SpectraBlade opened using Rosetta). Audio Units are created the way hosts create them, asynchronously, so the ones that need it load too.
- Measured with `--plugin-sweep` on a Mac with a large collection (1,018 files): 874 plug-in types work (Audio Unit: 366 effects and 131 instruments; VST3: 275 effects and 102 instruments) and 149 files don't, scanned in 6.1 minutes with no crash reports and no worker processes left behind. Of the 149, 89 have no code for Apple silicon, 46 crashed, 10 Audio Units returned an OS error during initialisation, 3 VST3 bundles held no plug-in, and 1 played invalid samples. All the crashes but one (Rob Papen's Punch-BD) were Native Instruments plug-ins. The one looked into, Raum, crashed because it could not read `~/Documents`: the sweep ran from a shell without that permission. The app would get macOS's prompt for Documents access instead, but that was not tested.
- Measured by the tests, with the signed app from the disk image as the worker, natively and under Rosetta: Kilohearts' kHs Gain, as both a VST3 and an Audio Unit, passes the check, loads in the rack, hands noise back bit for bit at its default setting, and comes back from a saved rack. A bundle with no code in it is marked not working, with its reason logged, in under 20 s.
- The app no longer reports an error at every launch when the last file it had open can't be read; it logs it and forgets the file.

**Milestone 13: recording onto session tracks**
- Record (Shift+Space) works in the multitrack too. Arm a track with its R button and the take goes onto that track; with no track armed it goes onto a new one. One track is armed at a time, and buses can't be armed.
- Drag across the timeline (or the ruler) to select a time range. Record then punches in at its start and out at its end, after a 2 s preroll, and stops by itself once the end has come back through the input. Without a range it records from the cursor until you stop. Play with a range plays just the range.
- The take replaces what the armed track had under it: clips inside the range go, and clips across its edges are cut back (or split in two) without their fade on the cut side. A red band shows the take as it lands, and the whole recording is one undo step.
- The take is lined up with the round-trip latency, as in the editor, and saved as a 32-bit float WAV in a "<session> Recordings" folder beside the session file (in Music/SpectraBlade/Recordings while the session is unsaved). Past the end of the session the player plays silence, so a take there still lines up.
- A session with no audio yet takes the interface's sample rate; otherwise the interface must run at the session's rate.
- Measured by the tests: the session's own output, looped back with 731 frames of latency into the recorder during a punch-in through the real player, lands bit for bit equal to the session mix over the range. Also checked: clearing a range (clips inside, across each edge, and a split from one clip), placing a take onto an armed track, a bus or no track, and recording from past the end of an empty session.
- Later milestones added loop recording into takes (20), input monitoring (21), recording several tracks at once (22), a metronome with count-in (23), and bars and beats (24).

**Milestone 12: buses and sends**
- Multitrack > Add Bus (Cmd+Shift+B) adds a bus: a row with no clips that plays what tracks send it, through its own effects (the FX button), volume and pan. Its lane lists the tracks feeding it.
- Each track's Sends button opens a panel with a row per bus: turn the send on, set its level, and choose Pre to send before the track's volume instead of after it. New Bus makes a bus and sends the track to it in one step. Every change is undoable, and deleting a bus removes the sends to it.
- Muting a track silences its sends too. Soloing a bus plays it with the tracks that feed it; soloing a track plays it with the buses it feeds.
- Delay compensation covers every path. A track is read ahead by its own effects' latency plus the most latency among the buses it sends to, and each of its outputs is held back by what that path lacks, so the direct sound and every bus line up. After a jump, the mixer runs the frames before the start point that the longest path needs, so the first block plays in time.
- Buses and sends are saved in the session file.
- Measured by the tests: pre- and post-fader sends, bus volume and pan against the same sums made by hand. With latencies of 300 frames on a track, 500 on one bus and 64 on another, the mix matches the mix without effects bit for bit in 333- and 2900-frame blocks and after a jump. Also checked: mute and solo rules, the edit rules (sends only from tracks to buses, clips never on buses, deleting a bus) and the file round trip.
- Not yet: buses feeding buses, sends to anywhere but a bus, or a choice of output for a track.

**Milestone 11: effects on each track**
- The FX button on each track header opens its effect chain: add any scanned VST3 or Audio Unit effect, and edit, bypass, reorder or remove each one. The button shows how many effects the track has and lights while any is on.
- Each track plays its clips, panned, through its effects, then its volume, then the master rack. Plug-ins run at the session's rate in blocks of up to 1024 frames. A mono plug-in gets the average of the two sides and plays on both.
- Plug-in delay compensation adds no delay of its own: a track whose effects delay it by L frames is read L frames ahead, so every track lines up and the session plays when you press play. After each jump, the effects are run over the frames they read ahead before the first block plays. Bypassed effects are skipped and their latency is not counted. The menu shows the latency it compensates.
- Effects are saved in the session file with each plug-in's state and bypass, and come back when it is opened. Deleting a track keeps its effects, so undoing the delete brings them back. Changing effects marks the session as needing a save; they are not undoable steps.
- Measured by the tests, with a test plug-in that delays by a known number of frames: chains of 377 and 1029 frames of latency mix bit for bit like the session without them, in blocks of 333, 512 and 3000 frames. Playing from a jump matches the dry mix bit for bit, both through the mixer and through the player. Bypass, reordering, removal, muting and a new session rate are checked, and a real Apple Audio Unit loads onto a track and is restored with its bypass.
- Changing a chain while playing jumps that track by the change in latency.

**Milestone 10: multitrack sessions (first part)**
- View > Multitrack Session (Cmd+4), or the Multitrack button, switches between the open file and a multitrack session. Drop audio files onto the session, or use Multitrack > Import Audio (Cmd+I), to add them as clips at the cursor (dropped files land on the track under the mouse). Each track has a name (double-click to rename), mute, solo, volume and pan. Drag a clip to move it, onto another track or below the last one for a new track. Drag its edges to trim it, split it at the cursor with Multitrack > Split Clip (Cmd+K), and delete it with Delete. Double-click a clip to open its file in the editor. Every change is undoable.
- The session plays through the same effects rack and outputs as the editor, resampled to the interface's rate. Mono clips pan with a constant-power law (-3 dB each side at the centre); stereo clips are balanced, so the centre leaves them untouched. Clips fade with raised-cosine curves. An empty session takes the first file's sample rate, and later files are converted to it.
- File > New Session, Open Session and Save Session (Cmd+S while the session shows) use `.sbsession` files. These name each clip's audio file relative to the session, so a session folder can be moved. Each file is read once however many clips use it, and missing files are reported.
- Measured by the tests: the mixer's pan laws, gains, mute, solo, trims and fades, sample by sample. Rendering in 7-frame blocks is bit-identical to one block. Split clips play bit for bit like the whole clip, and trimmed clips stay in place in time. Undo merges a dragged control into one step. Sessions saved and reloaded after moving their folder mix bit for bit like the original. The player in session mode plays exactly the mix and stops where told.
- The editing effects are off while the session shows.

**Milestone 9: click and pop repair**
- Effects > Click and Pop Repair finds clicks and pops in the selection, or the whole file, and rebuilds the damaged samples from the audio around them. No other sample changes, and only the stretches with repairs are rewritten. Two settings: a threshold (18 dB above the background by default; lower catches fainter clicks) and the longest click to repair (3 ms by default; longer events are left alone and counted). It is undoable and runs behind a progress window.
- Detection fits an order-32 autoregressive model to each block and predicts every sample from the samples before it and, separately, from the samples after it. A click is where both predictions miss by more than the threshold over the block's typical (median) error. It then reaches back to the first forward miss and on to the last backward miss, which find where the click begins and ends. Repair is least-squares autoregressive interpolation from 2048 samples either side, with a margin that grows with the click's length.
- Measured by the tests on three tones over a -60 dBFS noise floor with 30 planted clicks (1, 5 and 20 samples, in one or both channels): all 30 found and none in clean audio or silence. Only samples near the clicks change, the worst repaired click is left at -52.2 dBFS RMS (about the noise floor, which no model can predict), and the whole file matches the clean signal to 68.2 dB. Five minutes of stereo with 990 clicks take 1.53 s.

**Milestone 8: noise reduction**
- Select a stretch of noise alone (room tone, hiss, hum between phrases) and choose Effects > Capture Noise Print (Shift+P). Then Effects > Noise Reduction (Cmd+Shift+P) lowers that noise in the selection, or the whole file, by up to the reduction you set (12 dB by default). A threshold (3 dB above the print by default) decides how far above the noise a sound must be to be kept. It is undoable and runs behind a progress window.
- Each 4096-point frame (75% overlap, square-root Hann windows) gets a Wiener gain per frequency bin, with its signal-to-noise estimate smoothed over time (Ephraim and Malah's decision-directed method), so the noise that is left stays steady instead of warbling. The gain never drops below the reduction, so 0 dB changes nothing.
- Measured by the tests on a -12 dBFS tone in -40 dBFS white noise: 12 dB asked lowers the noise by 11.84 dB, 24 dB asked by 23.20 dB, the tone's level changes by 0.001 dB, and the noise under the tone drops 11.39 dB too. At 0 dB the audio comes back within -138.5 dB, and audio more than a frame outside the range is bit-identical. Five minutes of stereo take 0.43 s on an M5 Ultra.

**Milestone 7: loudness (EBU R128)**
- Effects > Loudness Statistics measures the selection, or the whole file: integrated loudness (LUFS), loudness range (LU), maximum momentary and short-term loudness, true peak (dBTP) and sample peak, following ITU-R BS.1770-4 and EBU R128. K-weighting is derived for the file's own rate; 5.1 files leave out the LFE and weight the surrounds by 1.41.
- Effects > Match Loudness sets the integrated loudness to a target (EBU R128 -23, ATSC A/85 -24, podcasts -16, streaming -14 LUFS) with one undoable gain change. It never pushes the true peak past a ceiling you set (-1 dBTP by default). When the ceiling allows less gain, it says how close it got. There is no limiter yet.
- True peak is read after 4x oversampling with the sample-rate converter. Measured by the tests: every EBU Tech 3341 loudness case (1-5) reads its expected value to 0.1 LU (EBU's tolerance) at 48 and 44.1 kHz, every Tech 3342 loudness-range case (1-4) reads 5.00, 10.00, 15.00 or 20.00 LU as expected, and a quarter-rate sine sampled off its crests reads its true peak within 0.05 dB.

**Milestone 6: spectrogram and spectral editing**
- View > Waveform (Cmd 1), Spectrogram (Cmd 2), or Waveform and Spectrogram (Cmd 3), per channel. The spectrogram runs from 20 Hz to Nyquist on a log scale with a frequency scale on the left, and shows -130 dBFS (black) to 0 dBFS (white). Each pixel column is a 4096-point Blackman-Harris FFT, calibrated so a sine reads its true level (within 0.01 dB when centred on a bin).
- Option-drag in a spectrogram to select a box of time and frequency. Effects > Attenuate Frequencies lowers what is inside by any amount; Effects > Remove Frequencies (Option+Delete) takes it out. Both are undoable and apply to every channel.
- Edits use short-time Fourier resynthesis (4096-point frames, 75% overlap, square-root Hann windows) with gains tapered over three bins and about a frame of time at the box edges, so they do not click or ring. Measured by the tests: an edit with no change gives the input back within -130 dB; removing 3-8 kHz takes a 5 kHz tone below -130 dB while a 1 kHz tone keeps its level to 0.01 dB; a -12 dB attenuation lowers a tone by 12.00 dB; audio outside the edited stretch is bit-identical.

**Milestone 5: sample-rate conversion**
- Edit > Convert Sample Rate converts the whole file (8 kHz to 192 kHz), with a progress bar and a cancel button. It is one undoable step: undo brings back the original audio and rate. Markers and the selection keep their times.
- Pasting audio copied from a file at another rate converts it to the file's rate first.
- The converter is a Kaiser-windowed sinc filter: flat to 45.5% of the lower rate (20.06 kHz at 44.1 kHz) and at least 140 dB down from half that rate up, so nothing aliases. Common rate pairs use exact polyphase coefficients (44.1 to 48 kHz is 160 phases); other ratios interpolate between 4096 phases. It is linear phase, so nothing moves in time.
- Measured by the tests on 32-bit float audio: a converted sine is 148 to 150 dB above its error for 44.1 to 48, 48 to 44.1, 44.1 to 96 and 96 to 44.1 kHz; the passband is flat within 0.000001 dB to 20 kHz; tones above 22.05 kHz leave at most -144.5 dB when going to 44.1 kHz; 44.1 to 96 and back nulls against the original to 148 dB. A minute of stereo converts in about 16 ms on an M5 Ultra.
- Playback of a file whose rate differs from the interface's still goes through JUCE's real-time resampler, which is lower quality. Convert the file (or set the interface's rate) for critical listening. Recording into a file needs matching rates.

**Milestone 4: markers and regions**
- Press M to drop a marker at the cursor, or at the playing position during playback (you can mark while listening or recording). With a selection, M makes a region of it.
- Markers sit in a lane under the ruler. Click one to move the cursor there (a region selects its range), drag it to move it, double-click to rename, right-click to rename or delete. Option+Left and Option+Right jump between markers and region edges.
- Markers are part of the undo history and follow the audio through edits: cutting before a marker moves it back, pasting moves it on, a region spanning an edit grows or shrinks, and markers inside deleted audio go with it. Crop keeps what overlaps the kept range.
- Saved in WAV files as cue points with labels, and regions as cue points with a length (`ltxt`), the layout other editors read. AIFF keeps point markers with names (a region keeps its start). FLAC and the other formats cannot hold markers; saving one says so.

**Milestone 3: recording**
- Pick the input in the bottom bar: any stereo pair or mono channel of the interface (all 30 inputs of a Fireface UFX). Only the chosen inputs are opened; macOS asks for microphone access the first time. The meter next to it shows the input level with a peak hold and clip lights (click to clear).
- Record with Shift+Space or the Record button; Space, Stop or Shift+Space ends the take. Each take is one undoable edit.
- **Into an empty window** a new file is recorded at the interface's sample rate.
- **Into an existing file**, playback starts 2 s before the punch-in so you can play along. With a selection, the take replaces exactly the selection (punch in and out, and recording stops by itself shortly after the punch-out); without one, it overwrites from the cursor and can run past the end.
- **Latency compensation:** the take is placed using the interface's reported input and output latency (buffers included) plus the effects rack's latency, so a punch-in lines up with what you heard. The recorder pairs each input block with the document frames the player sent in the same audio cycle, which keeps the alignment sample-exact regardless of when you pressed Record. On a Fireface UFX at 44.1 kHz with 512-frame buffers the round trip is 1135 frames (25.7 ms).
- The audio thread only copies into a lock-free buffer; a writer thread stores the take. Lost frames (the computer could not keep up) are reported after the take.
- Recording into a file needs the interface to run at the file's sample rate; the app says so when it does not.

**Milestone 2: waveform editing**
- Select by dragging; shift-click extends the selection. Zoom with the wheel, a pinch or Cmd +/-, Cmd 0 or Cmd E; scroll sideways to pan. Zoomed in far enough, individual samples are drawn.
- Cut, copy, paste (inserts at the cursor or replaces the selection), delete, crop and select all.
- Unlimited undo and redo. Audio is stored in blocks that are never changed in place, and every edit is a cheap new arrangement of them, so cut, copy and undo cost no audio copying. Edits are bit-exact.
- Effects menu: gain, normalize, S-curve fades, silence, invert, reverse. Effects work on the selection, or the whole file when nothing is selected; fades and silence need a selection.
- **Apply Effects Rack (Cmd R):** renders the selection through fresh copies of the rack's plug-ins on a background thread, with a progress bar and a cancel button. Bypassed slots are skipped and each plug-in's latency is compensated. Tails past the end are cut off.
- Save and Save As: WAV, AIFF or FLAC, at 16 or 24 bit (TPDF-dithered) or 32-bit float. Files are written through a temporary file, so a failed save never damages the original. MP3 and Ogg open but save through Save As.
- Space plays the selection (and stops at its end), or plays from the cursor. Playback picks up edits immediately.
- The title bar shows unsaved changes, and Open, drop and Quit ask before discarding them.
- Whole files are loaded into memory (about 1.4 GB for an hour of 48 kHz stereo).

**Milestone 1:**
- Open an audio file (WAV, AIFF, FLAC, Ogg, MP3, CAF and others). Drag and drop also works.
- Waveform overview with click-to-seek; space bar plays and pauses.
- An effects rack: add, bypass, reorder and remove effects, and open each plug-in's editor (or a generic one).
- Save and load the rack, plug-in state included, as `.sbrack`.
- Plug-ins are scanned **out of process**: each one is loaded in a worker copy of the app. One that crashes, or hangs longer than 60 s, is marked as not working; the editor keeps running.
- Works with multi-output interfaces such as the RME Fireface UFX (30 outputs). Pick any output pair in the bottom bar. The device opens at its current sample rate, and files at other rates are resampled on playback.

## Build

You need Xcode (command-line tools) and CMake 3.22 or later.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 12
open build/SpectraBlade_artefacts/Release/SpectraBlade.app
```

CMake fetches JUCE 9.0.4 by itself. To use a local checkout instead, add `-DSPECTRABLADE_JUCE_DIR=/path/to/JUCE`.

## Installing

Open `SpectraBlade-<version>.dmg` and drag SpectraBlade to Applications. The app is signed ad hoc, not with a Developer ID, so macOS blocks it the first time: open it once, then choose Open Anyway under System Settings > Privacy & Security. After that it opens normally. macOS asks for microphone access the first time you choose a recording input.

Then choose Plug-ins and Scan for Plug-ins. The first scan of a large collection takes a while; it runs in the background, and the window shows how far it has got.

## Packaging

```bash
Scripts/package.sh
```

This builds a universal Release app in `build-release/`, signs it ad hoc, runs the integration tests with that signed app as the plug-in scan worker, and writes `dist/SpectraBlade-<version>.dmg` with its SHA-256. Options: `--juce <dir>` builds with a local JUCE checkout, `--jobs <n>` sets the build's parallel jobs (4 by default), and `--skip-tests` skips the tests. The version comes from `CMakeLists.txt`.

To check a disk image's app on its own, mount it and point the tests at it:

```bash
./build-release/SpectraBladeTests_artefacts/Release/SpectraBladeTests --app "/Volumes/SpectraBlade 0.1.0/SpectraBlade.app/Contents/MacOS/SpectraBlade"
```

## Integration tests

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests
```

The tests run against the real machine:
- Editing and undo, bit for bit: cut and paste, crops across many segments, and peak tables against a brute-force scan.
- The effects, file round trips (float exact; integer formats within the dither and rounding bound), the playback source, and offline rendering through Apple's AUDelay.
- Out-of-process scanning, using the built app (or the one named with `--app`) as the worker: Apple's AUDelay plus the first 8 VST3 bundles installed.
- Third-party plug-ins: the scan check, kHs Gain as a VST3 and an Audio Unit in the rack (bit for bit at unity, saved and restored), and a broken bundle marked not working with its reason logged.
- The default audio device: output-pair routing on a multi-output interface. With `--live-input`, also arming inputs 1/2, a one-second live take during (silent) playback, and turning the inputs off again.
- Spectrogram level calibration and image, and spectral edits: identity, removal depth, neighbouring tones, untouched audio, attenuation.
- Multitrack: the mixer (pan laws, gains, mute, solo, trims, fades, block-size independence), clip edits (split, trim, move, delete), undo with merged steps, session files (round trip after moving the folder, shared audio read once, rate conversion, missing files), and session playback through the player.
- Automation and the multitrack view: volume and pan envelopes in the mixer (against the settings they replace, ramps, steps, block sizes, sends, latency) and in session files, fade limits, snapping rules, crossfades (the fades set and the level through them), writing automation passes, and the view driven by the mouse (fade handles, snapping, envelope points, crossfading) and by automation passes.
- Track effects: delay compensation bit for bit across tracks with different latencies and block sizes, playing from a jump, bypass, reordering, removal, saving each chain in the session file, and a real Apple Audio Unit on a track.
- Effect automation: a test plug-in's parameter following its envelope bit for bit across block sizes, after a jump, behind chain latency and after reordering; bypass, removed effects, slot ids and session files; touches in an effect's window reported in order, envelope values not taken for touches, and touched parameters held off their envelopes.
- Mixing down: the mixdown against playback's mix bit for bit, from part way in with effect latency on tracks and a bus, fresh copies of a session's effects (bypass, offline mode, one that can't load), cancelling, and the written file.
- Buses and sends: the routing sums, delay compensation through buses bit for bit, mute and solo, the edit rules, and session files.
- Recording onto tracks: a punch-in and a three-pass loop recording through the real session player looped back into the recorder, bit for bit; clearing a range on a track; placing takes; recording past the end of an empty session; choosing takes, after a split and at the edges of their audio; takes in session files; input monitoring through the real player, stopped and playing, bit for bit; two recorders at once landing on several tracks; two tracks monitored at once; track inputs in session files.
- Nudging: grid points in seconds and beats, and the cursor and selected clips nudged as one undo step.
- Track groups: volume, mute and solo following through a group, from the mixer too, and session files.
- Track colours: automatic and chosen colours, the undo step, and session files.
- Locking clips: every refused edit, clips placed over locked ones, session files, and the view.
- Muting clips: the mix, bounces and copies bit for bit, the view, and session files.
- Clip colours: their own or the track's, the selection as one undo step, and session files.
- The clip list: rows and columns, clicking a row, and following edits.
- Duplicating a track: its settings and clips, playing the same bit for bit, and the view.
- Hiding tracks: the lanes closing up, the selection, showing them again, the mix, and session files.
- Notes: on clips through Clip Properties, on tracks, and in session files.
- Selecting clips on a track and after the cursor, leaving out hidden tracks.
- Moving the selected clips to the cursor, together, and not when locked.
- Closing gaps on a track, around locked clips and keeping overlaps.
- Zero crossings: finding them, and splits landing on them.
- Time stretch: lengths, pitch and level kept on a tone, channels in step, progress and cancelling, and the clip edit.
- Pitch shift: four shifts of a tone landing on their pitches at the same length and level, cancelling, and the clip edit.
- The undo history: listing steps, going back and forward by clicking, and new steps.
- Track templates: what they keep, sends by bus name, and a file round trip.
- Saving a copy with audio: copies, numbering, missing files, and the moved copy opening.
- The session overview: its span, clicking and dragging to scroll, and drawing.
- Time edits: clearing, deleting and inserting time across every track, with envelopes, markers and locked clips.
- Copying and pasting time: the mix, envelopes, markers, rates and tracks.
- The tempo map: beats and bars across changes, the grid, snapping, nudging, the ruler, the click and the session file.
- The marker list and markers as CSV.
- Export Regions: which regions, and their files.
- Quantizing clips to seconds and to beats, and snapping dropped files.
- Recovery copies: writing, finding, clearing, and a recovered session needing saving.
- Fade shapes: their levels, the mix, the menu step and the session file.
- Loop playback in the file player, sample for sample.
- Keyboard shortcuts: storing changes and restoring them over the defaults.
- Freezing a track: the mix bit for bit, refused edits, the session file, collecting, and the lane.
- Clip groups: the edits, the session file, and selecting and dragging a group in the view.
- Clip gain lines: the mix, the line following edits, the session file, and editing points in the view.
- Folders: mute, solo and volume through them, the edits, the session file, and adding and collapsing them in the view.
- Folder stems: what each one renders, and which folders get one.
- Folder strips in the mixer: in place, showing and setting the folder, the strips after it showing their own tracks.
- Dropping a header on a folder: going in from below and above, and plain moves.
- Session notes: typing and undo in the window, and the session file.
- Header meters: separate peak readers in the recorder and mixer, a track's level and fall, and an armed track's input.
- Marker colours: choosing one, drawing it, edits and crops, and the session file.
- Header meters' peak hold and clip light: holding, falling, lighting and clicking out.
- Select Between Markers: spans around a frame, and the time range in the view.
- Selecting the clips in the time range: overlapping, touching and hidden.
- Audio load readout: percent, warnings, dropout counts, resets and new devices.
- Double-clicking the marker lane: a marker where clicked, and snapped.
- Solo safe: mixing beside a solo, mute, folders, undo and the session file.
- The mixer strip menu: solo safe and colour, ticks and undo steps.
- Clip list search: names, files, tracks, case and following edits.
- Marker list search: names, case, rows mapped to markers and following edits.
- Polarity: inverted tracks and buses through the mixer, freezing, the strip switch and the session file.
- Tap tempo: averaging, rounding, the window of taps, restarts, limits and undo steps.
- Go to time: the time forms, bars and beats across tempo changes, refusals, and scrolling there.
- Open recent: order, the limit of ten, settings, missing files and clearing.
- Trim to cursor: starts and ends, several tracks, locked clips, edges and undo steps.
- Next and previous clip: time order along a track, the ends, and starting from the cursor.
- The clip list as CSV: order, times, flags and quoting.
- Duplicate time: clips and markers repeated, later clips moved on, and repeating it.
- Region from clips: spans, names and no selection.
- Unsolo and unmute all: tracks, buses and folders, and undo.
- Moving between tracks: down and up, hidden tracks, and scrolling the lane and header into view.
- Remove DC Offset: each channel centred exactly, nothing outside the range changed.
- Swap Channels: exchanged exactly inside the range, mono unchanged.
- Insert Silence: zeros at the frame, the rest moved on exactly.
- Trim Silence: the audible span found exactly across blocks and channels, none in silence.
- Mix to Mono and Make Stereo: exact averages and copies across blocks, and undo.
- Editor fade shapes: linear in and slow-start out, exact at every frame.
- Balance: the mixer's stereo pan law, inside the range only.
- Set Selection: typed starts and lengths, cut to the file, bad ones refused.
- Paste Mix: added in place, the file lengthened only past its end.
- Save Selection As: the selection's frames and markers, read back from the file.
- Save Each Region As: the open file's regions named and numbered as Export Regions does.
- Select Region at Cursor: the innermost region, none at an end or outside, in the session view too.
- Markers at intervals: placed, named and sorted, taken frames skipped, limits refused, one undo step in the session.
- Regions from markers: each point to the next or the end, names and colours kept, in the session view too.
- Renaming markers in sequence: time order, two or three digits, one undo step in the session.
- Markers from CSV: round trip, other rates, cut to the file, bad files refused, merged without repeats.
- Sorting the marker list: by name, naturally and ignoring case; by length backwards; back to time order.
- Sorting the clip list: by length backwards, name and track order, rows still mapping to their clips.
- Normalizing each channel apart: both peaks to the level, a silent channel left silent.
- Channel levels: peak, RMS and DC offset per channel, exact across blocks.
- Find clipping: runs on any channel, across a block edge and at the end; short ones skipped.
- Find silence: quiet on every channel for the time, across a block edge; short or one-sided gaps skipped.
- Markers at silences: the middles of pauses between sounds, none for silence at the ends, capped.
- Shorten pauses: cut from the middle down to the time, both ends kept, bit for bit.
- Fit regions to sound: a loose region trimmed to its sound; silent, point and tight ones left.
- Fit regions to clips: a region cut to the clips in it; empty, fitting and point ones left.
- Regions over clips: overlapping and touching clips joined, a gap starting another, none twice.
- Select clips in region: the region as the range and its clips; nothing outside a region.
- Next and previous region: by start, points passed over, in the session view too.
- Marker list count and buttons: counts with the search, Delete for the highlighted marker only.
- Clip list count line: clips, locked and muted, and the shown clips' total length.
- Copy selection time: start, end and length as text Go to Time reads back.
- Step selection: by its own length, stopping at the ends.
- Double and halve selection: from its start, to the end or down to one frame.
- Selection to zero crossings: each end to the nearest crossing in reach, others left.
- Crossfade loop: the end blended into the audio before the start, bad lengths refused.
- Repeat selection: copies right after the range, bit for bit, empty requests refused.
- Paste to fill: the clipboard repeated over the range, the last copy cut short.
- Moving tracks: the edit, as an undo step, and dragging a header in the view.
- Track height: clicks at another height, its limits, and short headers.
- Pre-roll and post-roll: where playback starts and when a punch-out recording stops.
- Repeating clips to a range's end and reversing them bit for bit.
- Clip properties: typing each of a clip's settings, refused values, and following undo.
- Exporting stems: which tracks, their file names, and the stems adding up to the mix bit for bit.
- Bouncing a track: the mix bit for bit the same after a bounce, where it lands, and zooming to the time range.
- Copying, pasting and duplicating clips through the view, over what was on the track, and several clips selected, moved, pasted, duplicated and deleted together.
- Clip gain: normalizing against a brute-force peak, its limits, and Command-dragging a clip in the view.
- Session markers: adding, going to, clicking and dragging them in the view, snapping to and with them, and session files.
- The mixer window: track and bus peaks measured as the session mixes, and the strips editing the session (volume, pan, mute, solo, undo, automated tracks, meters).
- The metronome: the beat grid and click, clicks through the real player (on, during a count-in, through a loop, at another device rate) bit for bit, snapping to beats and bars, and the tempo in session files.
- Click repair on tones with planted clicks: every click found, none in clean audio or silence, only nearby samples changed, repair accuracy, a range, the length limit, a predictable tone burst left alone, cancelling, and speed.
- Noise reduction on a tone in white noise: depth at 12 and 24 dB, the tone's level, noise under the tone, identity at 0 dB, untouched audio outside the range, a mono print on stereo, cancelling, and speed.
- Loudness against the EBU Tech 3341 and 3342 signals at two rates, 5.1 channel weights, gating, true peak between samples, measuring part of a file, and cancelling.
- Sample-rate conversion: accuracy against ideal sines at four rate pairs, passband flatness, alias rejection, a round-trip null, an inexact ratio, cancelling, undo of a conversion, and speed.
- Markers: the edit rules (delete, insert, replace, crop), undo and redo, and WAV, AIFF and FLAC round trips.
- The recorder against a simulated device: channel mapping among active inputs, a 30 s take checked sample by sample, the playback anchor, and a punch-in through a simulated loop cable that must line up exactly.
- Rack save and restore with real plug-ins (Apple's own effects, which never stop for licence dialogs): order, bypass and a changed parameter.

The player only ever plays silence, so the tests make no sound. The live input test is opt-in because macOS asks for microphone access for each new build of the test binary, and the test waits until someone answers:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --live-input
```

The plug-in sweep is opt-in too: it scans every plug-in on the machine the way Scan for Plug-ins does, which takes a while with a large collection, and some commercial plug-ins open licence windows while it runs. It writes a report of what works and why the rest don't:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --plugin-sweep plugin-sweep.txt
```

## Plug-in locations

- AU: `/Library/Audio/Plug-Ins/Components`, `~/Library/Audio/Plug-Ins/Components`, and AUv3 app extensions
- VST3: `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3`

VST2 is deliberately not supported: Steinberg no longer licenses the VST2 SDK.

## Licensing

JUCE is dual-licensed under AGPLv3 or a commercial JUCE licence. Shipping SpectraBlade as closed source requires a commercial JUCE licence; otherwise the app must be released under the AGPLv3.

`Scripts/package.sh` signs the app ad hoc, without the hardened runtime, so it loads any plug-in's code. The CMake target asks for the hardened runtime with `disable-library-validation` (which third-party plug-ins need), but that applies only when the app is signed with a Developer ID, as notarization requires. Releases are not notarized yet; building with the Xcode generator (`-G Xcode`) is the simplest way to sign and notarize.

## Clean-room rules

This codebase is written from functional specs and public documentation only:
- Never decompile, disassemble or inspect another editor's binaries or bundles.
- Never copy its names, icons, UI layout, help text or factory presets.

## Next milestones

1. Editor: Reverse Channels' Polarity separately, inverting only the left or the right channel
