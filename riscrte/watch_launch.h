#pragma once
/* Returns false when the display is not the T-Watch 240x240 RGB565 panel.
 * The caller then continues with the paper GUI. Returns true after the watch
 * path has finished; the paper GUI must not run. */
bool paperboy_watch_launch();
