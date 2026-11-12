# Testing

SpectraBlade's checks are one console program, built with the app, that runs against the real machine: its audio device, its installed plug-ins and the built app itself.

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests
```

On Windows it is `build\SpectraBladeTests_artefacts\Release\SpectraBladeTests.exe`, and the same checks run, with these differences:
- What only the Mac has is left out: Apple's AUDelay (an Audio Unit) in the render, mixdown and rack checks, and the Intel-only bundle; a bundle holding only 32-bit Windows code stands in for the last, reported as having no 64-bit code.
- VST3 bundles are looked for in `C:\Program Files\Common Files\VST3` (and `%LOCALAPPDATA%\Programs\Common\VST3`). With none installed, the scan of installed bundles is skipped, and the plug-in menu and the rack are checked with SpectraBlade's built-in effects.
- AAC is written by Windows' own encoder, held to its own levels against the sweeps (47.5 dB at 320 kbps, 48 at 256, 45 for mono at 128); MP3 and M4A are read by Windows' decoders (MP3 at 320 kbps to 76.5 dB).
- The video panel shows AVI itself and MP4, M4V, MOV, MKV and WebM through previews, and is put on the screen for the check (Windows' player draws into a window of its own).
- The command table audit runs `python`, as `python3` on Windows is often the Store's stand-in.

The master rack checks on separate outputs need an interface with eight outputs, as the surround device checks need six; with fewer they are skipped, on either system.

What they check:
- The command table, read from the main window's source by `Scripts/check_commands.py` (the window is not in the test build; the check needs `python3`): every command described, performed and in a menu, and no default key claimed by two commands.
- Editing and undo, bit for bit: cut and paste, crops across many segments, and peak tables against a brute-force scan.
- The effects, file round trips (float exact; integer formats within the dither and rounding bound), the playback source, and offline rendering through Apple's AUDelay.
- Out-of-process scanning, using the built app (or the one named with `--app`) as the worker: Apple's AUDelay plus the first 8 VST3 bundles installed.
- Third-party plug-ins: the scan check, kHs Gain as a VST3 and an Audio Unit in the rack (bit for bit at unity, saved and restored), and a broken bundle marked not working with its reason logged.
- The default audio device: output-pair routing on a multi-output interface. With `--live-input`, also arming inputs 1/2, a one-second live take during (silent) playback, and turning the inputs off again.
- Spectrogram level calibration and image, and spectral edits: identity, removal depth, neighbouring tones, untouched audio, attenuation, and keeping only the band (what plays with a box selected).
- Multitrack: the mixer (pan laws, the pan after the effects and an instrument, gains, mute, solo, trims, fades, block-size independence), clip edits (split, trim, move, delete), undo with merged steps, session files (round trip after moving the folder, shared audio read once, rate conversion, missing files), and session playback through the player.
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
- SpectraBlade Stretch: a tone's length, pitch, level and edges from 50% to 200%, its channels exactly opposite as they went in; a click train's onsets found within 1 ms, each click coming out in place at its own peak with silence before it; a busy kit (ghost hits 15 dB down under a dying kick, 50 and 60 ms apart) with every hit found within 1 ms and each coming out as it went in; hats about 20 dB under a bass line, each found within 1 ms and coming out as it went in; no attacks in steady white noise; a steady chord with no attacks found and its energy away from the notes under -40 dB; progress and stopping; Shift Pitch with it; and the method as a sequence step.
- Extreme stretch: eight times longer at the same level with the two tones' shares of the spectrum kept, channels alike and a repeat bit for bit, noise holding its level within 0.4 dB across the hops, the first and last 50 ms carrying sound, the settings' limits and the window sizes, a factor of 1 keeping the length, a stop, a stretched range spliced with the audio either side bit for bit, and the step in a sequence bit for bit.
- Time stretch: lengths, pitch and level kept on a tone, channels in step (within 0.001), progress and cancelling, and the clip edit; a click train's 16 clicks kept once each, in place, from 50% to 200%; the level kept at the edges; Stretch and Shift Pitch on part of a file (the audio either side bit for bit, the pitch, the joins), markers, and as sequence steps.
- Pitch shift: four shifts of a tone landing on their pitches at the same length and level, cancelling, and the clip edit.
- The undo history: listing steps, going back and forward by clicking, and new steps.
- Track templates: what they keep, sends by bus name, and a file round trip; their envelopes exactly through a file, the effect's on its slot, and moved to another sample rate.
- Saving a copy with audio: copies, numbering, missing files, and the moved copy opening.
- The session overview: its span, clicking and dragging to scroll, and drawing.
- Time edits: clearing, deleting and inserting time across every track, with envelopes, markers and locked clips.
- Copying and pasting time: the mix, envelopes, markers, rates and tracks.
- The tempo map: beats and bars across changes, the grid, snapping, nudging, the ruler (and the time under the bars with Time with Bars), the click and the session file.
- The marker list and markers as CSV.
- Export Regions: which regions, and their files.
- Quantizing clips to seconds and to beats, and snapping dropped files.
- Recovery copies: writing, finding, clearing, and a recovered session needing saving.
- Fade shapes: their levels, the mix, the menu step and the session file.
- Loop playback in the file player, sample for sample.
- The heard frame: the player's position less the latency of the effects after it (the start while they fill, the pass before round a loop), the silence it plays on past the end so they empty before the transport stops, and the graph telling the player a preview effect's latency as it joins and leaves.
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
- The loading screen's steps, least time, Continue, notes and version, and the audio device probe off the message thread.
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
- Time formats: 83.304 s written in each of the eight formats (29.97 non-drop lagging, drop frame two frames on at the minute and back on the clock at the hour); parsing samples, seconds, m:ss, h:mm:ss, timecode with or without hours, drop frame refusing the dropped numbers, out-of-range parts refused; 3,200 random times written and read back in every format; the rulers' marks (1-2-5 seconds, 1-2-5 samples, frames of timecode then seconds) and labels; the setting's keys, listeners told once of a change, and typed times read as samples or timecode when that format is on.
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
- Nudge selection: either end by the step, stopping at the file's ends and a frame short of the other end; the first zero crossing from a frame on the way asked, and the nearest in reach.
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
- Parametric EQ: bands read from the panel's controls, the curve the sum of the active bands, its scales, no band at Nyquist; picking a band by its handle, double-clicking a handle off, its ring on again and empty space to place a band switched off; the spectrum falling back; a colour for each band; a band on the mid, the side or the left alone (the other left bit for bit; a mono sound all mid), the live EQ bit for bit with offline on such bands, the row's channel box and presets keeping the channel Linear phase: sines at the bands' response (a narrow bell drawn more broadly, measured), an impulse symmetric, the channels composed, an untouched channel bit for bit, the engine and the live EQ bit for bit with offline late by the latency, the latency reported only while on, the switch and presets.
- Match EQ: the correction against a known shelf and bell, the matched profile within 1 dB of the reference's, the correction's zero mean and the level kept, the amount, the smoothing, the most, refused references and ranges.
- Amplitude statistics: a half-scale sine's peak, where it is, true peak, RMS, steady windows and histogram; a 16-bit square wave at full scale then at a tenth then silence: every full-scale sample counted, 16 bits, the loudest and quietest windows 20 dB apart and the histogram by bins; DC offset of a constant; 8-, 16-, 24-bit and float detection; an empty range; the loudness the same as Loudness measures; channel names and formats; the table's rows and text; the panel scanning in the background, saying what it measured and when the selection has changed since, and refusing with nothing to measure.
- Tonal balance: pink noise within the Full mix target, white noise outside at the top, the targets' zero mean, the Speech target narrower, the style switch.
- Dither: samples multiples of the step at 16 and 24 bits within full scale and the same twice over, a quiet sine's distortion turned to noise, the level kept, Light and Strong shaping measured at 2.5 and 15 kHz, refused settings.
- Dialogue isolation: the pitch finder on a buzz and on noise with a tone, and on a low buzz with a strong formant (its own pitch, not a third of its period, at 48 and 96 kHz), the background between words lowered by the reduction, the harmonics kept, a tone between them lowered, a clean voice passed, cancelling, refused settings; the report (the share of frames with speech, the whole's fall) on the scene and on its background alone.
- Adaptive noise reduction: noise stepping up 12 dB followed without a print (the gaps after each word lowered by the reduction in both halves), the voice's harmonics kept, 0 dB leaving the audio as it was, cancelling, refused settings; the report (the share of frames with sound clear of the noise, the whole's fall) on the words and on noise alone.
- De-plosive: two thumps lowered by near the reduction, the voice's harmonics and a steady hum untouched, a voice without thumps passed, refused settings.
- Breath control: two breaths found where they are and nothing else, lowered by the reduction, a word and the room noise untouched, a range too short refused.
- Wind, rustle and bleed: a gust lowered with the bleed between words left, a burst of rustle lowered with the gust left, the bleed between words lowered, the words' harmonics kept under each job, cancelling, refused settings.
- Wow and flutter: 1 % of wow read as a speed curve and taken out of three tones, steady tones left, one tone's vibrato not taken for wow, refused settings, cancelling.
- Spectral recovery: the cutoff of a brick-walled buzz found (and none for full audio), the missing band rebuilt within 6 dB of the original's profile, the band below untouched, refused settings, cancelling.
- Rebalance: every stem at 0 dB giving the mix back, each stem turned down taking its own part of a synthetic mix down and leaving the others, refused settings, cancelling.
- Ambience match: room tone laid under silence matching the print's shape and level, eased at the edge, the same twice, filling only the quiet against adding regardless, refused prints and settings.
- Dialogue contour: a fall and a rise over a phrase's ending measured before, in the middle of and at the end of it, zero shifts before the ending, refused settings, cancelling.
- Azimuth: a drifting offset read at both ends and taken out to within 0.05 frames at the start, the middle and the end, mono and short ranges refused, cancelling.
- Codec preview: MP3 and AAC round trips lined up with the original, their error and level measured, the audio outside the range untouched, a bad bit rate refused, cancelling.
- EQ points: dragging a band's point sets its frequency and puts the curve under the mouse; the wheel sets the Q.
- Compressor and limiter: the compressor's curve, settling and timing on steps; no sample over the limiter's ceiling, with the audio before it kept.
- True-peak limiting and Match Loudness through it: a sine's crests between samples held at the ceiling; peaky audio brought to its target under the ceiling.
- Built-in parametric EQ as a plug-in: created by description, bit for bit with the offline EQ in any blocks, its state exact, saved and restored in the rack.
- Its window: the EQ panel setting the parameters as gestures, and showing changes made from outside.
- Live compressor and limiter: bit for bit with the offline effects (the limiter a look-ahead late), the limiter's latency reported and counted on a track.
- Preview: the live effects take the dialogs' settings; the engine's preview slot swaps and clears, and stays out of the rack.
- Rendered previews: a preview processes the range's first 30 s (or all of it for effects that need it all) and gives, bit for bit, what applying the step gives; a stretch says where its range ends now; a newer request stops one under way at its next progress check and only the newest is answered; a cancelled one isn't; the player plays the result in place of the document (past the document's end too) until it is taken away; a generator's preview is what it puts in at the cursor, bit for bit (the noise with the same seed), going in at the end when the cursor is past it, and of a long one only the first 30 s; the live preview effect bypassed and put back.
- The effect dialog: a wheel notch or arrow key moves a number a step onto the step's grid (Shift a tenth), within its range, and the wheel steps a menu; the settings settle once they stop changing; Space passes by the fields and starts and stops the preview, stopping ending Bypass; Return applies the settings shown, a preset's too.
- Factory presets: every one a setting its effect takes (run as a step where the effect is one, and for a live effect every parameter but the side-chain switch set within its range), no name repeated within an effect; the presets menu lists them first, then the user's, applies the one chosen, and deletes only the user's. A text field in the effect dialog takes only its characters and stays out of the values.
- Chorus, flanger, phaser: delays and notches where the arithmetic puts them, the spread, bounded feedback, refused settings; live bit for bit with offline.
- Reverb: the decay time measured, the pre-delay, damping, width, the tail and its end; live bit for bit with offline.
- Convolution reverb: against direct convolution, a delayed click, the impulse's scaling and rate, the tail; live bit for bit a block late, the impulse kept in its state.
- Multiband compressor and de-esser: the bands adding back up flat, each band acting only on its own frequencies, the de-esser's reduction on the band and on the whole sound and what Listen plays, refused settings; live bit for bit with offline, state and presets, the multiband window following its parameters, the windows' reduction readings.
- Multiband stereo width: each band's side scaled on its own (the low at 0 % gone, the high at 200 % doubled), the mid untouched, every band at 100 % flat, a mono sound bit for bit, refused settings and spacing, the live effect bit for bit with offline in uneven blocks, state and presets.
- The linear-phase band split: the width's bands the same in linear phase (the low side under -30 dB, the mid side kept, the high side doubled, the mid untouched), every band at 100 % the sound back within 1e-5 and a mono sound bit for bit; full-scale squares through the minimum-phase split peak over 1.1 and come back within 1e-5 through the linear-phase one (compressor at 1:1, width at 100 %, de-esser taking nothing, transient shaper at zero); each live effect bit for bit with offline late by the 8,703 frames it reports (the exciter's 40 added), 0 again once the switch is off; the state and the sequence steps keep the switch.
- Exciter: the oversampler's latency, DC, passband and stopband; the curves' slope and bounds and each one's harmonics (Tube even, Tape odd, Transistor harder); aliasing of a driven 15 kHz tone against shaping at the rate; mix 0 flat; the live effect bit for bit with offline late by its latency, state, presets, refused settings.
- Maximizer: the ceiling held in samples and true peak for every character (a faded sine; a sound starting loud at once measured), a quiet sound bit for bit in place, Loud louder than Balanced than Transparent, the emphasis lowering the body after a hit, the live effect bit for bit with offline late by a latency that follows the character, state, presets, refused settings.
- Transient shaper: attack lifting the first millisecond of hits and leaving their tails, sustain lifting the tails (the tone in the band below moved only by the crossovers' skirts, measured), every gain at 0 flat, a band alone, the live effect bit for bit with offline, no latency, state, presets, refused settings.
- Empty editor: the recent files listed, a click opening one.
- Opening from Finder: the types offered all read, the files picked out of an open request.
- Transport bar: tooltips following the keys; the selection panel's cells.
- Keyboard shortcuts window: search, keys taken and reset, sets exported and imported, key sets applied over the defaults.
- Reverse Each Region: regions turned round in place, the rest untouched, overlapping and one-frame regions skipped.
- Effect presets: exact values through the files, names, separate effects, damaged files; built-in effects' and the EQ's values.
- Graphic EQ: the bands, a band's gain at and off its centre, the cascade against the bands' responses, refused settings; live bit for bit with offline; the panel: pressing sets a band, one stroke across sets every band passed over along its line, double-click resets one, the wheel steps it, setting the gains from outside says nothing.
- Dynamic EQ: every band at 0 dB (or under its threshold) passing the sound bit for bit; a bell cutting a loud 1 kHz by its full gain at a ratio of 20 while 100 Hz and 8 kHz keep within 0.3 dB, letting go once it falls quiet; a high shelf lifting a quiet 10 kHz under its threshold by the static gain at 2:1; the response (a bell's gain at its centre, flat a decade off, a shelf's 6 dB an octave); refused settings and withinRate; live bit for bit with offline in uneven blocks, the readings the core's, state and presets restoring every setting, the window both ways and left alone on closing; the panel's dragging, wheel, double-click, the meter's threshold line and a quiet setSettings A band on the mid, the side or the left, and on a mono sound.
- Moving tracks: the edit, as an undo step, and dragging a header in the view.
- Track height: clicks at another height, its limits, and short headers.
- Pre-roll and post-roll: where playback starts and when a punch-out recording stops.
- Repeating clips to a range's end and reversing them bit for bit.
- Clip properties: typing each of a clip's settings, refused values, and following undo.
- Exporting stems: which tracks, their file names, and the stems adding up to the mix bit for bit.
- Bouncing a track: the mix bit for bit the same after a bounce, where it lands, and zooming to the time range.
- Copying, pasting and duplicating clips through the view, over what was on the track, and several clips selected, moved, pasted, duplicated and deleted together.
- Several note clips selected: Shift-click adds and removes one, an audio clip joins the same selection, a plain click selects one alone; dragged, nudged and quantized together with the audio clip, each keeping its track and stopping at the session's start; muted and unmuted together; two copied note clips pasted onto one new instrument track keeping their spacing, duplicated after themselves and deleted together; Select All takes both kinds.
- Clip gain: normalizing against a brute-force peak, its limits, and Command-dragging a clip in the view.
- Session markers: adding, going to, clicking and dragging them in the view, snapping to and with them, and session files.
- The mixer window: track and bus peaks measured as the session mixes, and the strips editing the session (volume, pan, mute, solo, undo, automated tracks, meters).
- The metronome: the beat grid and click, clicks through the real player (on, during a count-in, through a loop, at another device rate) bit for bit, snapping to beats and bars, and the tempo in session files.
- Click repair on tones with planted clicks: every click found, none in clean audio or silence, only nearby samples changed, repair accuracy, a range, the length limit, a predictable tone burst left alone, cancelling, and speed.
- Noise reduction on a tone in white noise: depth at 12 and 24 dB, the tone's level, noise under the tone, identity at 0 dB, untouched audio outside the range, a mono print on stereo, cancelling, and speed.
- Reverb reduction on bursts of noise in a synthetic room (velvet noise dying away 60 dB over 1 s, as loud as the sound): how far the tail after each burst is lowered, the bursts' own level, the room's time measured (0.5, 1 and 2 s rooms), a time set longer taken as the measured one and shorter leaving more, dry bursts measuring under 0.15 s and left within 0.1 dB at a 1.5 s setting, a steady tone's level, channels worked alone, untouched audio more than a frame outside the range, cancelling and refused settings.
- Scrubbing on a 440 Hz tone: dragged at normal speed, twice it and backwards (pitch, level, how far behind the mouse), silent until moved and again when held still, the fastest it follows a jump, bit for bit the same in blocks of any size, fading out when let go, a 44.1 kHz file on a 48 kHz device, a mono file, and the file's end.
- Scrubbing a session: a track's clip against scrubbing the file of it, bit for bit; its volume, a muted track and its effects (heard, or left out); a frozen track and a folded 5.1 master; the player stopped in each mode; the multitrack view's time ruler and marker lane driven with the mouse; how long 32 tracks take.
- Scrubbing a session through its effects: an EQ against scrubbing the mix as played from where the scrub began, forward and back; a latency-compensated effect bit for bit; an edit made mid-scrub; playing after a scrub; the cost of 16 tracks of EQ and compressor scrubbed forward and back.
- The Pitch Editor on a session clip: the part of its audio it works on (played, under a time selection, a selection missing it), and a note moved in it played from the clip's own copy, its neighbour unmoved, the clip's place, length and fades kept.
- Stretching a clip by Option-dragging its edge: the outline while dragging, the ratio asked for from the end and from the start, the limit of twice as long, a drag back to where it began, a plain drag still trimming, and a clip stretched from its start ending where it did.
- Meeting a delivery spec: the specs' values and the checks at their edges, a quiet programme brought to EBU R128 by gain alone with its report, a peaky one to -14 LUFS through the true-peak limiter, a selection changed alone, and silence refused.
- Stems to a delivery spec: two stems given the one gain that brings their mix to EBU R128, summing to the gained mix; stems whose mix would pass the true-peak ceiling reported as needing limiting; a silent mix refused.
- Meet Delivery Spec as a sequence step and a Batch Process level: the step gives the command's result bit for bit, is named after its spec, refuses a tolerance past 10 LU and notes what the result measures; Batch brings a file to -14 LUFS and its report says it meets it.
- Keeping takes when a clip is processed: three takes stretched alike from what the clip played of each (one padded with silence) and lined up, so choosing another plays it in the same place and length; mismatched takes refused; with notes moved, the other takes play the same frames as before.
- Ducking on surround tracks: a 7.1 and a 5.1 track in a 7.1 master ducked under a voice come down on every channel by the gains a stereo track does, untouched before the voice; a 7.1 track as the key ducks as a stereo key of its fold-down does.
- Loudness of 7.1 audio: the LFE alone does not count, each side surround alone reads +1.5 dB, a rear surround and a front channel alone read as one front channel, and all eight together read as five channels once and two at 1.41.
- Reverse Clip with takes: three takes reversed alike from what the clip played of each, lined up, so choosing another plays it back to front in the same place; takes of other lengths refused.
- Custom delivery specs: the ranges at their edges, a custom -20 LUFS +/- 1 LU spec at -2 dBTP met through the limiter and named in the report, and Batch refusing one out of range.
- Batch delivery report: each file's lines against the spec written as a text file in the batch's folder, numbered when the name is taken, counting the files that meet it and saying why one wasn't written; none for other levels or when nothing was written.
- Sound Library index: names split into words (case changes, acronyms, digits, letters past ASCII); a scan of chosen folders (headers, BWF descriptions, unreadable and non-audio files), refused folders, search by the start of words in names and descriptions, sorting and paging, rescans that read only what changed, stopping, a second connection, removing a folder; and the time of counts, searches and pages on an index of a million sounds.
- Sound Library window: nothing scanned without a folder, a missing folder refused, an added one scanned on its own thread and listed, the bar beside the folders dragged to size the pane and reported, search and every column's text, sorting by length and rate, a drag from selected rows carrying their files (and none from a row not selected), Rescan, and removing a folder. Formats and folders left out: the choices offered (each once, MP3 not on its own), an index queried for WAV alone or leaving the AIFF folder out finding and counting the WAVs alone, the window starting with WAV alone, its menu's toggles (reported, the last format kept, every format), a list set by the app, and a folder left out and brought back.
- Sound Library folders: the folders inside one that hold sounds, read from the index in name order (not a sibling whose name starts the same, nor empty folders), how many sounds are in each and deeper, queries limited to one folder (searched too); the window's By Folder pane first, a folder chosen, By Category showing every sound again, a removed folder's sounds gone.
- Sound Library preview: playing sample for sample at the device's rate and stopping at the end, a selected part looping, the preview volume, mono in both channels, 5.1 folded down, resampling 44.1 kHz on a 48 kHz device, an unreadable file playing silence, a selected part written as a WAV of its own (rate, channels, bits and samples kept; asked again, the same file), and the mouse: selecting by dragging, carrying the selected part out, clicking to play from a point, scrubbing along the strip; selecting a row shows its sound.
- Sound Library categories: the bundled UCS list (753 subcategories in 82 categories, the 8.2.1 fix in), CatIDs from UCS names and from iXML (USER CATID, USER CATEGORY and SUBCATEGORY, the ASWG catId), the name winning, a query by category, subcategory or none, counts by category, searching by a category's names, sorting by category, setting by hand (kept through a rescan, cleared back to the file's own), an index from before categories brought up to date and read again, and the window's category list, column and setting a selection's category. A name gives a category only with its CatID in its own case, and an older index loses the categories that names gave otherwise.
- Sound Library ratings, favourites and tags: setting them (ratings clamped, tags cleaned and the same in any case), filtering by each and together, searching by tags, counting tags, sorting by rating, keeping them through a rescan of a changed file, a removed file taking its tags, an index from before them upgraded, the window's stars, heart, keys, menu hooks and filter bar, and their speed on a million sounds.
- Writing into files: the category and tags in a WAV file's iXML (USER and ASWG), the rest of the iXML and every other chunk kept, a file without iXML given one, refusals (not WAV, RF64, unreadable or non-text iXML, chunks past the end, read-only, nothing to write) leaving the file as it was, no temporary file left, permissions kept, and the window writing the selection and rescanning. `SpectraBladeTests --write-check <folder>` does the same to copies of the first two WAV files in each folder at the top of a real library (its own files are only read).
- Sound Library insert: Return (and the menu) handing the selected sound, the part selected in its waveform (written as a drag writes it), or several sounds whole, to the main window; nothing selected inserting nothing.
- Centre channel: tones in the middle, on one side and below the band, removed and extracted; a voice-like sound in the middle of noise on each side; a range, no reduction, and mono files, empty ranges and settings out of range refused.
- Automatic phase correction: offsets late and early, whole and fractional, found and taken out; two microphones with their own noise; a channel upside down; unrelated channels, silence, offsets past the most looked for, mono, short ranges and settings out of range; a range eased in, and a correction by nothing bit for bit.
- DTMF and MF tones: every key read back from the generated pairs, each tone's level, the other frequencies, silence between, the length and peak; lower case, and characters, timings, levels and rates refused.
- Pitch correction on a voice-like buzz: the pitch found at four pitches, none in noise or silence; a note moved to the chromatic and C major scales and half way at half strength, its level kept; a glide put on notes; the speed of a slow correction; a voice in tune kept in tune, noise passing as it was, a range, refused settings and cancelling.
- Pitch correction by hand: the notes of a melody (one with vibrato) found, one moved a semitone and one snapped, the others kept, the shift eased between notes, refused tracks and ranges, and how long 30 s takes to analyse; the Pitch Editor panel driven with the mouse: dragging to semitones, by cents with Option, double-click to snap, Apply, and a stale analysis refused. Splitting and joining notes: the halves' pitches and shifts, too near an end refused, joined back as found, the longer half's shift, notes apart not joined, one half moved by ear (pitch) and the other kept; Cmd-click and the right-click menu in the panel. Moving notes in time: a note moved later keeping its pitch, the note before stretched to meet it and the pause after squeezed (pitch and level by ear), the last note unchanged, moves past the next note or over four times refused; dragging sideways in the panel, stopping at the limit, and a drag mostly up moving only the pitch. Formants: a voice-like buzz moved an octave and a fifth up and down, each harmonic's level against the envelope kept and the envelope moved with the pitch, and its pitch.
- Speech levelling on syllable-like bursts of noise at different loudnesses with room noise between: each passage brought to the target, the fastest change (at 6 dB a second the gains rise at most 0.3 dB a step and never over the unlimited ones, the quiet passage within 1.5 LU of the target by its end; at 30 the unlimited gains bit for bit; 0.5 and 31 refused), pauses not raised, the largest boost, a click held under the ceiling, the same gain on every channel, a file of room noise alone untouched, a selection with eased edges and untouched audio either side, cancelling and refused settings.
- Loudness against the EBU Tech 3341 and 3342 signals at two rates, 5.1 channel weights, gating, true peak between samples, measuring part of a file, and cancelling.
- Sample-rate conversion: accuracy against ideal sines at four rate pairs, passband flatness, alias rejection, a round-trip null, an inexact ratio, cancelling, undo of a conversion, and speed.
- Markers: the edit rules (delete, insert, replace, crop), undo and redo, and WAV, AIFF and FLAC round trips.
- The recorder against a simulated device: channel mapping among active inputs, a 30 s take checked sample by sample, the playback anchor, and a punch-in through a simulated loop cable that must line up exactly.
- The dock: docking, tabbing, floating and closing panels, layouts in XML, workspaces, and the dock itself laying out stand-in panels and taking drops.
- The settings panel: a page per section with a switch, a choice, a number, a button and a made component, each reading its setting and writing a change through at once, refreshed after a change made elsewhere, shown by name.
- Several open files: keeping each aside with its history and place, switching, closing, the Files panel and its rows dragged into the multitrack view (the drag the view takes, and the clip a file makes: the editor's audio, converted to the session's rate, named after the file), the tabs over the editor, and dropping several at once.
- The panels: the undo history following the file or the session, the levels, the spectrum against sines at known levels, correlation and the goniometer, and the copy of the output they read.
- The fade handles: the curves, the faded samples, the fades kept adjustable as one undo step, and dragging the handles in the view.
- Maximising a group of panels: filling the dock, the layout kept, restoring, and double-clicking a tab.
- Vertical zoom and the zoom strip: a sine's drawn height at each zoom and its clipping at the lane's edge, the limits and edge levels, Option-scrolling, the strip's buttons and readout, and its tool buttons (in order left of the times, the tool in hand lit, each running its command, Heal off until there is a drawing).
- The times under the editor: cursor, selection and view as shown, clicks on them, and which are left out as the strip narrows.
- Waveform colours: each one's contrast against every theme's canvas and lanes, and the choice replacing the theme's own.
- Going to edges: a file's and a session's edges walked both ways, with and without a selection, hidden tracks passed over.
- Remove Hum: the hum and its harmonics taken out while a tone beside them is kept, from the first frame, on a selection only, at 50 and 60 Hz, and the limits.
- Repair Clipping: a clipped sine rebuilt close to the true one with the rest left bit for bit, the peak kept, clipping found at any level, unclipped sines left, long flat stretches counted.
- Generators: each wave's level and fundamental, the sawtooth's aliasing, a sweep's frequency over time, each noise's level, slope, channel independence and peaks, and putting generated audio in.
- Piano roll: the grid at a quarter beat and its rows and columns read back; step entry (at the grid line before the click, a step long, selected, one undo step); a drag moving a note in pitch and time, snapped; its right edge dragged to a new length; Shift-click adding to the selection and the velocity slider setting every selected note; the arrow keys (semitone, grid step, octave); Option-click, Cmd-A and Delete; a Shift-drag box, a click past the clip's end adding nothing, the grid off using the last length; a different clip clearing the selection and nothing shown disabling the panel; drawn in every theme.
- Instrument menus: a few scanned instruments flat with format and maker, many under a submenu a format and a maker, the built-in first.
- Instrument tracks from the menu: the known instruments (the Basic Synth first, titled built in, all instruments); Add Instrument Track adding a track named after one, selected, with the instrument first in its chain; Remove Instrument.
- Stems of instrument tracks: a track with only a note clip is a stem, reaches to its last note clip's end and keeps the clip isolated for a bounce; a track without clips has none.
- Keys: the command audit (Scripts/check_commands.py) on the table and docs/SHORTCUTS.md (every default key on its command in the document, every documented key a default of the command named; keys in `?:` and character literals read); every panel's fixed key the shortcuts window lists in the document's Inside panels table, written alike; the window's rows listing the panel keys after the commands, read-only, searchable, left out of the changed-only list.
- Controller lanes: a rising wheel's values at the clip's start and each 64-frame step where the rounded value changes, a bend at rest once, both before the note at that frame, the same over blocks of 1, 997 and 4,096 frames; a repeating lane starting over each pass; lane points tidied (in order, 0 to 1, a duplicate controller and an empty lane dropped); the file round trip with lanes; captured wheel and bend moves as lanes; the piano roll's lane taking, moving and dropping a point, and the notes' full height without one.
- Live MIDI: queued messages out of the next block in order, then nothing, the instrument kept running for the tail; captured notes at their session frames (a held note ended at the stop) and the clip from the record start; nothing kept outside a capture; a MIDI file's tracks read into clips named by the track at the session rate through the tempo, an empty track left out; what isn't a MIDI file, one with no notes and a missing file refused; the on-screen keyboard's keys reaching the live input; live messages reaching the track's instrument at their frames across a block mixed in two runs, another track's instrument hearing nothing, a block without messages playing nothing new.
- Note clips as clips: a repeating clip trimmed at both ends playing exactly what it did in that time (the note across the new start left out, the one across the new end cut off there), trimmed back out playing as before and not before its origin; split in two with the parts playing what the whole did (a note across the split cut off there), refused at an edge; a copy placed on the track and on a new track; copied time carrying the clips cut to it and pasted time playing them in time; moves onto a bus (in time only) and past the last track; deleted time cutting back a clip across its edge and inserted time splitting one; the file round trip with the offset; the piano roll's whole timeline and loop-length drag.
- Instrument tracks: a note clip's MIDI the same over blocks of 1, 997 and 4,096 frames (note-offs before note-ons at one frame, the last pass's note cut off at the clip's end, a note at the pattern's length left out, once without a loop, pitch and velocity limited); the edits (added to a track but not a bus or folder, values tidied, replaced keeping id and place, moved not before 0, muted, removed, counted in the session's length, deleted time removing the clips inside it and moving later ones back, inserted time moving them on, one across an edit staying, a duplicated track's copies with their own ids); the session file round trip; a chain with a click instrument and a gain (the instrument first however it was added and through moves; each click on its session frame through 37 frames of latency, summed with the clip's audio and run through the gain; the same mix in blocks of 64 and 1,000 frames; all notes off after a jump and the clicks in time from there; a muted clip and a bypassed instrument leaving the audio alone; on a 5.1 track no copies and L and R alone); the Basic Synth on a track playing a note clip and silent after it, its level envelope giving a hundredth of the sound at -40 dB, the note clips kept in what freezing renders, and the chain a bounce or mixdown loads from the XML with the instrument first and mixing the same bit for bit. Track kinds: each kind of clip on its kind of track or a new one, moves onto the wrong kind refused, a kind changed only on a track with no clips of its own kind, pasted time sorted by kind, a file keeping the kinds and one from before kinds given them (an instrument in the chain, or note clips alone), clips on the wrong kind moved to a track of their own.
- The multitrack toolbar and tools: a repeating clip flattened, two clips merged into one that plays what both did, a clip stretched by a ratio keeping its start or end; Draw making a note clip along an instrument lane (a new instrument track below the tracks, nothing on an audio track), Split cutting audio and note clips where clicked, Stretch scaling a note clip's notes, Glue joining a clip with the next; the strip's buttons lit for the tool in hand and running their commands, the tempo readout and the Bars switch.
- Instruments: pitches from names and numbers (and refused), phrases together and in turn and their MIDI (an off before an on at one frame, a note past the length left out); the built-in Basic Synth made from its description (a bare sine at exactly its velocity from its cycle's start, silent after the hold; velocity and level; the release; a chord's partials; notes in turn; sixteen voices with the oldest taken over, and All Notes Off; its state; the sawtooth's and square's fundamentals against their peaks); rendering through any instrument (its latency dropped, also one longer than the sound; a mono output on every channel; the notes at their frames; prepared offline at the rate and released; stopped by progress; a bad phrase refused); and a fresh instance from a description and state playing exactly what the instance plays.
- Spectral drawings: lasso and brush geometry, strokes that take from the drawing and the brush's strength (in the geometry and in the audio removed), removing and lowering inside a drawing with the rest left bit for bit, keeping only what is inside it (what plays with a drawing selected), healing a burst, a click and noise over a tone, healing time, drawing with the tools in the view, how the lasso looks (only its line and a faint closing edge while it is drawn, the wash and the dashed edge once it is closed, its own pointer), Undo taking back the strokes one by one (and leaving the drawing alone once the file has changed), and a selection dragged with the view asked to snap landing on zero crossings.
- Track outputs: a track played into a bus, against sums made by hand, with mute, solo and plug-in latency; the edits, deleting the bus, freezing, session files and templates.
- Bus into bus: a bus played into another against sums made by hand, in either list order, with mute, solo and plug-in latency along the chain; loops of two and three buses refused; deleting, session files and bus templates.
- 5.1 surround files: channel names, the fold-down against the arithmetic (and through Make Stereo, Mix to Mono, a sequence step and a batch), the WAV speaker mask, FLAC, saving channels apart and putting them back, the 5.1 file order, the player folded down or to separate outputs in long blocks, and the outputs the engine connects on the interface.
- A session's 5.1 master: the surround pan's gains (exact by default, constant power, the spread from the middle, the LFE), the master bit for bit against the stereo mix, folded down and to separate outputs, in blocks of any size, buses and interface pairs in it, the six-channel mixdown, the edits, the session file, track templates, freezing and bouncing, and the mixer strips' pad, width and LFE.
- Writing the surround pan: one pass over the pad's x and y as one undo step, and the mixer's pad and width recording while the session plays (silently), live with writing on and set or still when stopped.
- The 5.1 master meter: the mixer's peaks of the six channels as mixed, per reader, folded down or not, none in a stereo master, and the mixer's six-bar master meter.
- Surround pan automation: the pan following envelopes of its x, y, width and LFE on a 32-frame grid, the same in any block size, an envelope at the setting's value, buses, a stereo master and 5.1 tracks unaffected, the value ranges, folders, time edits, the file, and the mixer strip.
- 5.1 tracks: six channels to a 5.1 master bit for bit (mono to C, stereo to L and R), volume, envelopes and polarity on all six, effects on all six and in time, no pan, the fold-down into buses and a stereo master, delay compensation through a bus with latency, import, the edits, the file, templates, freezing and bouncing, and the mixer strip.
- Effects on 5.1 tracks: a parametric EQ and a phaser on a 5.1 track against the same effects on stereo tracks holding each pair, bit for bit; a compressor and a limiter linking the six, against compressing and limiting them as one file, bit for bit, with one gain on every channel each frame; a sidechained compressor left to the pairs; copies following settings, automation and (when refreshed) whole state; latency; a mixdown's own copies; the engine making and dropping them as a track becomes 5.1 and back; an effect that can't be copied.
- Freezing 5.1 tracks: frozen with effects, volume and polarity and played back bit for bit as live, in a 5.1 and a stereo master; two-channel frozen audio on a 5.1 track; the session file; unfreezing.
- 7.1 files: the channel names, the fold-down to stereo and mono bit for bit against the arithmetic, the WAV speaker mask and reading back, eight named files put in 7.1 order, and playing folded down or to separate player channels.
- 7.1 tracks: a 7.1 clip's eight channels and a 5.1 clip's six to a 7.1 master bit for bit, volume, polarity and effects on all eight, the mix into a 5.1 and a stereo master, import, the edits, the session file, templates, bounces, freezing, and the effects in four pairs, linked dynamics on eight, the mixdown's copies and the engine's copy counts.
- A 7.1 master: the stereo mix in L and R as it starts, the pan over seven speakers (power, the middle, a side, behind), placed tracks and a bus folded down, to separate outputs and mixed down bit for bit, a 5.1 track's surrounds in the side pair, freezing a 5.1 track as 5.1, the edits, the session file, the mixer's eight-bar meter, and the live rack's copies and linking on the test interface.
- The effects rack past left and right: a rack slot's copy against the slot's plug-in (settings, bypass, latency); an EQ and a phaser applied to 5.1 and three-channel files, pair by pair; a compressor and a limiter linking a 5.1 file's six, applied against compressing and limiting them as one file and played with their buses six wide, bit for bit; a 5.1 master played to separate outputs of the test interface, its C, LFE, Ls and Rs connected through an EQ's copies and into and out of a linking compressor and limiter (and straight with an empty rack, two wide when folded down).
- Undo history in projects: a file's steps (audio, markers, tags with a cover, a cut undone) saved and opened again after the folder moved: the saved step, redo, back to the start bit for bit, shared blocks; files with no steps keep none; history audio missing.
- Picture following edits: the picture map through edits in place, a cut, inserted silence, a paste, undo, redo and a rate conversion, and read back from text; a video whose frames each have their own brightness, with sound cut and silence inserted, re-cut and decoded frame by frame against the map.
- Session video: where its picture is at a session time (black before its start), the session frames its length takes, kept in the session file with its start and found beside the session when both are moved, undone; the mix written into a MOV with the picture's packets unchanged, the session's own mix of that time within 1 step of 24 bits and starting at the video's start; stopping, a missing video and writing over the video refused.
- MKV and WebM pictures: the Video panel showing them through ffmpeg previews (size, length, silent), and a kept preview used again without being made again.
- Projects: saving what is open (an unchanged, an edited and a never-saved file, a video, a rack effect, an untitled session) with the edited audio written beside the project and the opened files left byte for byte; moving the project's folder and opening it again (files, audio, places, video, the one shown, the rack setting, the session, the view in front); missing files; refused files; saving again unchanged.
- Video: following the playhead against a simulated late-starting player; the video a file remembers; for MP4, M4V, MOV, MKV, WebM and AVI test videos made by ffmpeg, opening their sound and putting new sound into a copy (picture packets compared by MD5, the codec checked with ffprobe, the sound read back); refusing to write over the video; and the Video panel on the real player (which types macOS shows, the frame when stopped, the drift while playing).
- Recorded command sequences: each of the 27 steps against its command's own code, steps with nothing to do and steps refused, a sequence on part of a file through a rate change, stopping and failing changing nothing, saving and loading exactly, the store, the recorder, and a sequence in a batch.
- Batch processing: collecting files from folders, naming the new files (suffix, numbering, never the source), rate conversion with markers and tags against the converter's own result, the dither bound on an unchanged rewrite, peak and loudness levels with and without the limiter, a silent file, FLAC, MP3, AAC and (with ffmpeg) Opus and Apple Lossless written and read back, the effects rack against a direct render, unreadable files, settings that can't run, and the report.
- Sidechains: a key track into a test plug-in that adds its sidechain input to its output, against sums made by hand, stereo and mono inputs, muted and no keys; plug-in latency before and in the effect, on the key and on a bus with a sidechain effect, bit for bit against none, in two block sizes and after a jump; the built-in compressor keyed, frame for frame against the Dynamics core, alone and on a session track; loops refused through ducking too; files, deleting the key, freezing and bouncing a listener, and bouncing a ducked track.
- Interface outputs: a track and a bus played out of interface pairs, bit for bit against the mix they leave, with plug-in latency along the way; mixdowns without them; edits, session files, templates, freezing, bounce and stems; the player's channels for each pair; on the real interface, the pair turned on and connected, one it lacks left out, and disconnected again.
- Ducking: the attack, hold and release measured against their formulas, block-size independence, mute, pre- and post-fader sends, the edits (loops) and session files.
- MP3 and AAC export: each setting read back at the same length and start, its level and signal-to-error ratio, bit rates on noise, the MP3 tag frame, the .m4a overhead, repeatability, 96 kHz and mono, refusals and stopping.
- Typing a volume into the mixer: parsing, setting the track as an undo step, refusals.
- ffmpeg (where installed): Opus, Apple Lossless, WavPack and AC-3 written and opened again, stopping, and an unreadable file.
- File info: WAV (INFO and bext), AIFF (ID3 chunk; NAME, AUTH and ANNO read), FLAC (Vorbis comments, replaced when tagged again), MP3 (ID3v2.3 written, 2.4 read), .m4a and Opus tags read back, checked against ffmpeg where installed; tagging leaves AIFF and FLAC audio bit for bit; covers (JPEG and PNG) byte for byte in WAV, AIFF, FLAC, MP3 and .m4a, checked by extracting them with ffmpeg, covers ffmpeg attaches read back, a 2.5 MB cover in an .m4a, replacing and removing a FLAC cover; files with no info; the undo step.
- The layout: the transport bar's width against its buttons and time, and the heights of the tabs.
- The themes: text, accent, cursor and waveform contrast measured as WCAG ratios in every theme with every accent, switching themes at runtime (colours, controls and the waveform view), and the standard controls drawn in the theme's colours.
- The editor's double-click selecting the whole file; a saved device setup that names no device opening the default.
- Rack save and restore with real plug-ins (Apple's own effects, which never stop for licence dialogs): order, bypass and a changed parameter.

The player only ever plays silence, so the tests make no sound. The live input test is opt-in because macOS asks for microphone access for each new build of the test binary, and the test waits until someone answers:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --live-input
```

## A page of each theme

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --theme-sweep themes
```

draws the editor (waveform and spectrogram, a selection, markers, the lasso in hand), its strip, a session with two clips, the Amplitude Statistics, the Tonal Balance and the marker list in every theme, one `themes/<theme>.png` each, to look at after a change to a theme or a panel. Anything a component leaves unpainted shows red. The theme test checks the contrast rules on every theme and accent without looking; this is for the eye.

## Under the debugger and the sanitizers

The suite is also run as a Debug build, which turns on JUCE's assertions and leak detector, and as a build with the Address and Undefined Behaviour Sanitizers, which catch reads past a buffer, uses of freed memory and undefined arithmetic that a Release build passes over in silence. Each is its own build folder:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-debug --target SpectraBladeTests
./build-debug/SpectraBladeTests_artefacts/Debug/SpectraBladeTests --app build-debug/SpectraBlade_artefacts/Debug/SpectraBlade.app/Contents/MacOS/SpectraBlade

F="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target SpectraBladeTests
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./build-asan/SpectraBladeTests_artefacts/RelWithDebInfo/SpectraBladeTests --app build-asan/SpectraBlade_artefacts/RelWithDebInfo/SpectraBlade.app/Contents/MacOS/SpectraBlade 2>&1 | tee asan.log
grep "runtime error\|ERROR: AddressSanitizer" asan.log
```

Both pass. Under the sanitizers the only reports are shifts of negative or too-large values inside the LAME encoder and JUCE's bundled JPEG library, which are theirs; none is in SpectraBlade's code. Under Debug the one timing check a Debug build can't meet (scrubbing speed) is given four times the room, and these assertions remain, none ours: JUCE's Opus reader building a tag from UTF-8 bytes as if ASCII (our tags are read from the file itself, so nothing shows), a Label grabbing focus in a mixer that is not on screen in the test, and the VST3 host's checks on kHs Gain.

## The plug-in sweep

The plug-in sweep is opt-in too: it scans every plug-in on the machine the way Scan for Plug-ins does, which takes a while with a large collection, and some commercial plug-ins open licence windows while it runs. It writes a report of what works and why the rest don't:

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --plugin-sweep plugin-sweep.txt
```

## A real sound library

The Sound Library check on a real library is opt-in as well: it indexes a folder of sounds into a new index (in a temporary folder, deleted afterwards), scans it again with nothing changed, and times searches. It only reads the library's files.

```bash
./build/SpectraBladeTests_artefacts/Release/SpectraBladeTests --library "/path/to/your sounds"
```

