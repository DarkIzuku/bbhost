#pragma once

// The game's own frame pace: 30 or 60.
//
// FD4's frame-time manager (0x2434770, called once a frame from the main
// loop) picks an interval by mode - the int at +8, or +0xc while the one-shot
// flag +0x276 is set - through a switch at 0x24347f5. Modes 0, 1, 3 and 4 wait
// out 1/30 s; mode 2 waits 1/60 with a vblank interval of 1. The engine has a
// 60 fps mode; the game never selects it. The wait is a usleep to within 5 ms
// of the deadline and then a spin on gettimeofday - which is why the main
// thread reads ~90% busy at 30 fps while half of that is waiting.
//
// With 60 chosen (the Frame cap option at 60 or more, or Off, when the game
// starts; BBHOST_GAME_FPS=30/60 overrides) this hooks the manager's entry and
// puts it in mode 2 every frame, with the one-shot overrides (+0x2bc, +0x2c0,
// +0x271, +0x272, +0x2c4) cleared so nothing drops it back to 30.
//
// The choice is made when the game starts: the game's fixed 1/30 s steps are
// constants in its code, and 60 needs them all changed together.

struct ElfImage;

void frame_rate_install(ElfImage* image);

// The pace the game was started at: 30 or 60.
int frame_rate_game_fps();

// Game time against real time (plugins' set_time_scale): 0.5 is half speed,
// 2 double (as far as the machine keeps up), clamped to 0.1-4. The game steps
// a fixed amount a frame, so this is the frame pacing's interval.
void frame_rate_set_time_scale(float scale);
