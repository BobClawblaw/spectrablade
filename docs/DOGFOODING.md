# Dogfooding log

Feature work stopped at 0.14.7. From there to 1.0.0, each release fixes what real use turns up. This is the list: every problem found by using the app, in the order it was reported, with the version it was found in, what was wrong, the milestone that fixed it (see the [changelog](CHANGELOG.md)) and the release that carries the fix. Each has an [issue](https://github.com/BobClawblaw/spectrablade/issues) as well, closed against the commit that fixed it. Problems still open stay at the top until a release closes them. New ones can be reported as issues or straight into this list.

1.0.0 is a round of use that adds nothing here.

## Open

Nothing at the moment.

## Fixed

| # | Issue | Found in | What was wrong | Fix | Released in |
|---|---|---|---|---|---|
| 1 | [#1](https://github.com/BobClawblaw/spectrablade/issues/1) | 0.14.7 | Changing the selection while it played looping went silent at the selection's old end, and the play stopped there. The play's own stop point had stayed where the selection ended when Play was pressed while the loop followed the new selection. | M344: the stop point moves with the loop, and a play left outside the new selection starts the loop again from its start. | 0.14.8 |
| 2 | [#2](https://github.com/BobClawblaw/spectrablade/issues/2) | 0.14.8 | The Multitrack button only tinted when pressed; nothing said which mode was in use or which a click would go to. | M345: a two-part Editor \| Multitrack switch, lit on the side in use, and the status line under the editor starts with "Editing file" or "Multitrack session". | 0.14.9 |
| 3 | [#3](https://github.com/BobClawblaw/spectrablade/issues/3) | 0.14.8 | Moving Stereo Width's slider changed nothing heard. Every effect dialog's Preview was off until ticked (or Space), so the play underneath went on unchanged; and a stereo file whose channels are the same has no difference for Stereo Width to scale, which it did not say. The file in question turned out to be real stereo, but narrow: its side 17 dB under the mid, so 200 % (a 6 dB lift of the side) widens the top end a little and pushes the peak to +1.8 dBFS. | M346, M347: a dialog opened while the file plays starts with Preview on, on every path (the shared dialogs, Multiband Compressor, Dynamic EQ, Graphic EQ and the Parametric EQ window); Stereo Width says when the channels are the same, in its text, its status line and on Apply. | 0.14.9 |
