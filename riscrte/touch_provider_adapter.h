#pragma once

// Owner-task side of the RiscRTE input.touch.raw adapter. The original
// touch_gt911.h API remains the console-task facade used by Paperboy.
void paperboy_touch_owner_begin();
void paperboy_touch_owner_poll();
void paperboy_touch_owner_end();
