#pragma once

/** The heard frame: the player's position less the latency of the effects after it, round the loop,
    and the tail it plays on past the end so they empty. */
void runPlayheadTests();
