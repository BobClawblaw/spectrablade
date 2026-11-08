# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

Mastering and repair: every job a full mastering suite and a full audio-repair suite do, built our own way (clean-room: the same jobs, with our own methods, names, presets and look; nothing taken from another product's code, presets or help). One milestone each, roughly in this order. Already here: parametric (with a linear-phase mode), dynamic and graphic EQ (the parametric and dynamic bands on the mid, the side, the left or the right), compressor, multiband compressor, true-peak limiter, de-esser, multiband stereo width, noise reduction from a noise print, reduce reverb, click repair, hum removal, de-clip, repair, spectral attenuate/remove/heal, centre channel, phase correction, stretch and pitch, pitch correction, level speech, match loudness and delivery specs.

1. **Exciter:** multiband saturation (tube, tape and transistor curves) with a mix.
2. **Maximizer:** a loudness limiter with selectable character, transient emphasis and a ceiling, beyond the plain limiter.
3. **Transient shaper:** attack and sustain up or down, per band.
4. **Match EQ:** the spectrum of a reference (a file or a selection) taken onto the sound, smoothed and with an amount.
5. **Tonal balance view:** the sound's spectrum against target ranges for a genre.
6. **Dither and word length:** noise-shaped dither to 16 or 24 bits, as a last step.
7. **Dialogue isolation:** speech kept and everything else lowered, with an amount for each.
8. **Voice de-noise (adaptive):** noise tracked as it changes, with no noise print needed.
9. **Mouth de-click, de-crackle and de-plosive.**
10. **Breath control:** breaths found between words and lowered.
11. **Wind, rustle and bleed reduction.**
12. **Wow and flutter:** pitch drift from tape or vinyl found and taken out.
13. **Spectral recovery:** the highs missing from band-limited audio (phone, low-rate MP3) rebuilt.
14. **Music rebalance:** voice, bass, drums and the rest each turned up or down (source separation).
15. **Ambience match:** the room tone of one part laid under another.
16. **Dialogue contour:** the pitch of a phrase's ending reshaped.
17. **Azimuth:** the time and phase offset between the channels of a tape transfer found and corrected (beyond phase correction).
18. **Codec preview:** the sound heard as it will come back from MP3 or AAC at a bit rate.

Sound Library: a browser for a large sound-effects library (100k to a million files and more), with drag and drop into the app and out of it.

1. **Category suggestions:** most libraries don't name their files the UCS way, so a category could be suggested for the rest (never taken as the sound's category until accepted). Not built, because what has been tried isn't good enough to offer. Measured on one large library, each vendor's UCS-named sounds guessed from the others': scoring UCS's own words and synonyms against a sound's name and folders got the category right for 0 to 63% of most vendors' sounds; learning from the library's own UCS-named sounds as well (naive Bayes over name and folder words, UCS's words as the prior) got 40% of categories and 17% of exact subcategories, and its confidence didn't single out the right ones (44% right among the half it was surest of). The sounds whose names follow UCS come from only six vendors, 85% of them from one (mostly footsteps), so there is little to learn from. Suggesting from a folder's other sounds was measured too and reaches almost nothing that is really UCS: of the 646,200 sounds there without a category, 31,154 share a folder with sounds that have one, and nearly all of those folders' categorised sounds are ordinary all-caps names that happen to start with a general CatID (WIND_LTL2-0988, RAIN_LIGHT_TO_HEAVY_CS); the folders named the UCS way are named so throughout. Such all-caps names can't be told from UCS names by their shape (requiring UCS's four fields would drop 108 real UCS names, ROBTVox_Mechanimal 070_F0X among them, and keep about as many ordinary ones), so about 150 of that library's sounds still take WIND, RAIN or BEEPS from their names. Next to try: words from the audio itself.

Not code: Reduce Reverb and Level Speech are yet to be tried on real recordings and listened to, and a Developer ID (for notarizing) is the remaining step before installing opens without Privacy & Security.
