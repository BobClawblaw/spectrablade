# Roadmap

What comes next, in order. Finished work moves to the [changelog](CHANGELOG.md).

1. The commands Audition users will reach for that SpectraBlade lacks, each then added to the Audition-style keys: go to the previous or next clip or selection edge, vertical zoom, and repeat the last command
2. Opening a file from a slow or absent network volume without holding up the app's launch: the last file is reopened on launch, and a volume that doesn't answer keeps the window from appearing until it does
3. The crash seen once in 0.9.0 while playing a file (the file player's read wrote through a bad buffer pointer on the audio thread), not yet reproduced
