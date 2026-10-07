#pragma once

// "Rebirth in the Nightmare" at the Altar of Despair.
//
// With the Yharnam Stone, the altar returns the hunter to their origin's level
// and attributes, gives back the blood echoes those levels cost, and opens the
// doll's level-up menu to spend them again; leaving it asks to accept or undo
// (closing that choice goes back to the menu). world.rebirth (BBHOST_REBIRTH=0|1,
// on by default): the altar's talk script is rewritten from the dump into
// <data>/bbhost/rebirth-assets at start (engine/rebirth_script.h) and served
// behind the player's own overlay; the host's half of its handshake runs once a
// frame. Off, the altar is the game's.

struct ElfImage;

void rebirth_install(ElfImage* image);
// Once a frame on the game's main thread (engine/frame_rate.cpp).
void rebirth_tick();
