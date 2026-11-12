// The played character's helmet take-off / take-on animations on demand (the helmet key, #34): see anim.cpp.
#pragma once
#include "om.h"

namespace om {

void anim_install();
// Once per UI frame: the played character, the running animation's end.
void anim_tick();
// Starts the take-off (or take-on) animation with this helmet in his hand. False: it can't play now (hooks missing,
// another one running, no character).
bool anim_helmet_start(bool take_off, U16 helmet_key);
// True once when the animation reaches the helmet (the hand lifts it off / puts it on): the moment to hide / show it.
bool anim_cue();
enum class AnimOutcome { Running, Played, Failed };
// The started animation: Running until it ends; Played (it was entered, now left: finished or cut short) or Failed
// (never entered) once, then Running again for the next.
AnimOutcome anim_outcome();
// (sleep.cpp's evaluator hook asks first: the evaluator can only be hooked once)
bool anim_branch_passes(const U64* candidate);

} // namespace om
