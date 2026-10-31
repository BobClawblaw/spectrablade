# Waveloom

A clean-room audio editor for macOS that hosts VST3 and Audio Unit (v2 and v3) plug-ins. Built on JUCE 9.0.4.

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
- Save and load the rack, plug-in state included, as `.wlrack`.
- Plug-ins are scanned **out of process**: each one is loaded in a worker copy of the app. One that crashes, or hangs longer than 60 s, is marked as failed; the editor keeps running.
- Works with multi-output interfaces such as the RME Fireface UFX (30 outputs). Pick any output pair in the bottom bar. The device opens at its current sample rate, and files at other rates are resampled on playback.

## Build

You need Xcode (command-line tools) and CMake 3.22 or later.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 12
open build/Waveloom_artefacts/Release/Waveloom.app
```

CMake fetches JUCE 9.0.4 by itself. To use a local checkout instead, add `-DWAVELOOM_JUCE_DIR=/path/to/JUCE`.

## Integration tests

```bash
./build/WaveloomTests_artefacts/Release/WaveloomTests
```

The tests run against the real machine:
- Editing and undo, bit for bit: cut and paste, crops across many segments, and peak tables against a brute-force scan.
- The effects, file round trips (float exact; integer formats within the dither and rounding bound), the playback source, and offline rendering through Apple's AUDelay.
- Out-of-process scanning, using the built app as the worker: Apple's AUDelay plus the first 8 VST3 bundles installed.
- The default audio device: output-pair routing on a multi-output interface. With `--live-input`, also arming inputs 1/2, a one-second live take during (silent) playback, and turning the inputs off again.
- Markers: the edit rules (delete, insert, replace, crop), undo and redo, and WAV, AIFF and FLAC round trips.
- The recorder against a simulated device: channel mapping among active inputs, a 30 s take checked sample by sample, the playback anchor, and a punch-in through a simulated loop cable that must line up exactly.
- Rack save and restore with real plug-ins (Apple's own effects, which never stop for licence dialogs): order, bypass and a changed parameter.

The player only ever plays silence, so the tests make no sound. The live input test is opt-in because macOS asks for microphone access for each new build of the test binary, and the test waits until someone answers:

```bash
./build/WaveloomTests_artefacts/Release/WaveloomTests --live-input
```

## Plug-in locations

- AU: `/Library/Audio/Plug-Ins/Components`, `~/Library/Audio/Plug-Ins/Components`, and AUv3 app extensions
- VST3: `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3`

VST2 is deliberately not supported: Steinberg no longer licenses the VST2 SDK.

## Licensing

JUCE is dual-licensed under AGPLv3 or a commercial JUCE licence. Shipping Waveloom as closed source requires a commercial JUCE licence; otherwise the app must be released under the AGPLv3.

The release build enables the hardened runtime with `disable-library-validation`, which is needed to load third-party plug-ins. These entitlements take effect only when the app is signed. Building with the Xcode generator (`-G Xcode`) is the simplest way to sign and notarize.

## Clean-room rules

This codebase is written from functional specs and public documentation only:
- Never decompile, disassemble or inspect another editor's binaries or bundles.
- Never copy its names, icons, UI layout, help text or factory presets.

## Next milestones

1. Sample-rate conversion (also for pasting and recording across rates)
2. Multitrack session: clips, buses, sends, automation, delay compensation, recording (Fireface inputs) and punch-in
3. Spectral view and editing, noise reduction, click and pop repair, loudness (EBU R128)
