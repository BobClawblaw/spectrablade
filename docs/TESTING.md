# Testing

SpectraBlade's checks are one console program, built with the app, that runs against the real machine: its audio device, its installed plug-ins and the built app itself.

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests
```

What they check:
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
- Auditioning in the open panel: a file through the player's audition output, sample for sample and past the rack; stopping it; the Preview strip's choices.
- The effects rack and plug-in menus: device announcements passing the engine by, the rack keeping its rows until it changes, and the menus by format and maker.
- The waveform and spectrogram view: dragging the line between them, its limits, and selecting elsewhere.
- Window places: saving and restoring a window's place, a plug-in window's position alone, bringing back a window that would be off screen, and the rack's bar.
- Reading past a file's end: from the snapshot, through the playback source, and playing to the end through the rate-converting transport.
- Slow volumes: file checks and a file's read off the message thread, with a check and a read that hang until let go.
- The loading screen's steps, least time, Continue and notes, and the audio device probe off the message thread.
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
- Invert one channel: only that channel flipped, only in the range.
- Silence one channel: only that channel zeroed, only in the range.
- Copy one channel to the other: the target replaced in the range, the source untouched.
- Noise gate: quiet stretches faded and zeroed, the sound around them bit for bit.
- Stereo correlation: +1 for the same channels, -1 for one inverted, none for mono.
- Clipping starts: the first frame of every clipped run, in order, capped at the count.
- Repair: clicks on a straight line redrawn back onto it, the frames around untouched.
- Fade selection edges: a fade in and a fade out at the ends, each at most half the range.
- Audible range in a range: only the sound inside it, in document frames.
- Crop margins: the range widened either side, kept inside the file.
- Pieces between markers: the file split at its point markers, a Start piece before the first.
- Delete markers in a range: those starting inside it go, the end excluded.
- Move markers in a range: only those starting inside it, refused past either end; signed times read.
- Markers to zero crossings: points and region ends moved to the nearest crossing in reach.
- Gain ramp: even in decibels from the first frame to the last, the rest untouched.
- Pan ramp: Balance's pan law moved evenly from one position to the other.
- Stereo width: the side scaled and the mid kept, only in the range.
- Sounds between silences: each stretch of sound, short gaps not splitting, silent ends left out.
- Next sound: the sound after the next silence, stepping on from each one's end.
- Tremolo: a raised-cosine dip by the depth once a cycle, the rest untouched.
- Echo: repeats every delay, each quieter by the feedback, stopping at the range's end.
- Silence outside: everything but the range zeroed, the range bit for bit.
- Normalize each region: each peak to the level on its own; overlapping and silent ones skipped.
- Fade each region: each region faded as Fade Selection Edges fades it.
- Previous sound: the sound before the previous silence, stepping back from each one's start, and none before the first.
- EQ band: each band's designed response, sines changed by it, a range filtered as in a whole-file pass with the rest bit for bit.
- Parametric EQ: bands read from the panel's controls, the curve the sum of the active bands, its scales, no band at Nyquist.
- EQ points: dragging a band's point sets its frequency and puts the curve under the mouse; the wheel sets the Q.
- Compressor and limiter: the compressor's curve, settling and timing on steps; no sample over the limiter's ceiling, with the audio before it kept.
- True-peak limiting and Match Loudness through it: a sine's crests between samples held at the ceiling; peaky audio brought to its target under the ceiling.
- Built-in parametric EQ as a plug-in: created by description, bit for bit with the offline EQ in any blocks, its state exact, saved and restored in the rack.
- Its window: the EQ panel setting the parameters as gestures, and showing changes made from outside.
- Live compressor and limiter: bit for bit with the offline effects (the limiter a look-ahead late), the limiter's latency reported and counted on a track.
- Preview: the live effects take the dialogs' settings; the engine's preview slot swaps and clears, and stays out of the rack.
- Chorus, flanger, phaser: delays and notches where the arithmetic puts them, the spread, bounded feedback, refused settings; live bit for bit with offline.
- Reverb: the decay time measured, the pre-delay, damping, width, the tail and its end; live bit for bit with offline.
- Convolution reverb: against direct convolution, a delayed click, the impulse's scaling and rate, the tail; live bit for bit a block late, the impulse kept in its state.
- Multiband compressor and de-esser: the bands adding back up flat, each band acting only on its own frequencies, the de-esser's reduction on the band and on the whole sound and what Listen plays, refused settings; live bit for bit with offline, state and presets, the multiband window following its parameters, the windows' reduction readings.
- Empty editor: the recent files listed, a click opening one.
- Opening from Finder: the types offered all read, the files picked out of an open request.
- Transport bar: tooltips following the keys; the selection panel's cells.
- Keyboard shortcuts window: search, keys taken and reset, sets exported and imported, key sets applied over the defaults.
- Reverse Each Region: regions turned round in place, the rest untouched, overlapping and one-frame regions skipped.
- Effect presets: exact values through the files, names, separate effects, damaged files; built-in effects' and the EQ's values.
- Graphic EQ: the bands, a band's gain at and off its centre, the cascade against the bands' responses, refused settings; live bit for bit with offline.
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
- The dock: docking, tabbing, floating and closing panels, layouts in XML, workspaces, and the dock itself laying out stand-in panels and taking drops.
- Several open files: keeping each aside with its history and place, switching, closing, the Files panel, the tabs over the editor, and dropping several at once.
- The panels: the undo history following the file or the session, the levels, the spectrum against sines at known levels, correlation and the goniometer, and the copy of the output they read.
- The fade handles: the curves, the faded samples, the fades kept adjustable as one undo step, and dragging the handles in the view.
- Maximising a group of panels: filling the dock, the layout kept, restoring, and double-clicking a tab.
- Vertical zoom and the zoom strip: a sine's drawn height at each zoom and its clipping at the lane's edge, the limits and edge levels, Option-scrolling, and the strip's buttons and readout.
- The times under the editor: cursor, selection and view as shown, clicks on them, and which are left out as the strip narrows.
- Waveform colours: each one's contrast against every theme's canvas and lanes, and the choice replacing the theme's own.
- Going to edges: a file's and a session's edges walked both ways, with and without a selection, hidden tracks passed over.
- Remove Hum: the hum and its harmonics taken out while a tone beside them is kept, from the first frame, on a selection only, at 50 and 60 Hz, and the limits.
- Repair Clipping: a clipped sine rebuilt close to the true one with the rest left bit for bit, the peak kept, clipping found at any level, unclipped sines left, long flat stretches counted.
- Generators: each wave's level and fundamental, the sawtooth's aliasing, a sweep's frequency over time, each noise's level, slope, channel independence and peaks, and putting generated audio in.
- Spectral drawings: lasso and brush geometry, removing and lowering inside a drawing with the rest left bit for bit, healing a burst, a click and noise over a tone, healing time, and drawing with the tools in the view.
- The layout: the transport bar's width against its buttons and time, and the heights of the tabs.
- The themes: text, accent, cursor and waveform contrast measured as WCAG ratios in every theme with every accent, switching themes at runtime (colours, controls and the waveform view), and the standard controls drawn in the theme's colours.
- The editor's double-click selecting the whole file; a saved device setup that names no device opening the default.
- Rack save and restore with real plug-ins (Apple's own effects, which never stop for licence dialogs): order, bypass and a changed parameter.

The player only ever plays silence, so the tests make no sound. The live input test is opt-in because macOS asks for microphone access for each new build of the test binary, and the test waits until someone answers:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --live-input
```

## The plug-in sweep

The plug-in sweep is opt-in too: it scans every plug-in on the machine the way Scan for Plug-ins does, which takes a while with a large collection, and some commercial plug-ins open licence windows while it runs. It writes a report of what works and why the rest don't:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --plugin-sweep plugin-sweep.txt
```
