# SpectraBlade

A clean-room audio editor for macOS that hosts VST3 and Audio Unit (v2 and v3) plug-ins. Built on JUCE 9.0.4.

SpectraBlade was called Waveloom, then briefly WaveDrill, until 31 October 2026. On first launch it copies the settings of whichever earlier version you ran (audio device, inputs and outputs, plug-in list), and it still opens `.wdrack` and `.wlrack` rack files.

![SpectraBlade](Art/spectrablade-banner.png)

**Milestone 18: automating effect parameters**
- A track's or bus's A menu lists each of its effects, with that effect's parameters in its own order (automated ones say so). Pick one to show its envelope over the lane, labelled with the effect, the parameter and its value, and edit it like a volume envelope: click the line to add a point (at the parameter's current value, the first time), drag, Option-click to delete. Clear Shown Effect Envelope removes it. Envelopes are saved in the session file.
- While an envelope has points, it sets its parameter as the session plays, in steps of 64 frames (1.3 ms at 48 kHz) that line up with session time, so the result doesn't depend on the audio interface's block size. An effect after others in its chain works on audio their latency has delayed, and its parameters follow their envelopes from that earlier time, so automation stays lined up with the audio. Without points, the parameter is left where the effect's window has it.
- An envelope belongs to its effect, not its place in the chain: each effect slot has an id, saved with the chain, so reordering the chain keeps every envelope on its effect. If the effect is removed, its envelopes are kept unused, and the lane says the effect is no longer on the track. A bypassed effect ignores its envelopes. Mix Down follows effect envelopes too.
- Measured by the tests, with a test plug-in whose gain is its parameter: on a ramp, the mix is bit for bit the gain the envelope gives at each 64-frame step, in 1-, 37-, 512- and 3000-frame blocks and from part way in; after a 300-frame delay in the chain, and moved ahead of it, it still lines up bit for bit. Also checked: no envelope leaves the parameter untouched, an envelope for a removed effect plays nothing, slot ids are kept and never repeated, session files round trip, fresh copies of Apple's AUDelay in a mixdown keep their slot ids and envelopes on its 4 parameters change the mix, and, through the real view, a click on the line of an AUDelay parameter adds a point at its current value and the envelope is drawn.
- Not yet: writing effect parameters while playing (from the effect's own window). Automation steps are not sample-accurate: a parameter changes at most once every 64 frames.

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
- Not yet: input monitoring through the track's effects, several armed tracks at once, and loop recording into takes.

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
- Effect automation: a test plug-in's parameter following its envelope bit for bit across block sizes, after a jump, behind chain latency and after reordering; bypass, removed effects, slot ids and session files.
- Mixing down: the mixdown against playback's mix bit for bit, from part way in with effect latency on tracks and a bus, fresh copies of a session's effects (bypass, offline mode, one that can't load), cancelling, and the written file.
- Buses and sends: the routing sums, delay compensation through buses bit for bit, mute and solo, the edit rules, and session files.
- Recording onto tracks: a punch-in through the real session player looped back into the recorder, bit for bit; clearing a range on a track; placing takes; recording past the end of an empty session.
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

1. Multitrack, continued: writing effect parameters while playing, from the effect's own window
