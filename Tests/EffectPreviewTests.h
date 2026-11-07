#pragma once

class AudioEngine;

/** Previewing effects: rendered previews in the background (the newest request only), the player
    playing one in place of the document, bypassing the live preview, and the effect dialog's
    wheel, keys, Space and Return. */
void runEffectPreviewTests (AudioEngine&);
