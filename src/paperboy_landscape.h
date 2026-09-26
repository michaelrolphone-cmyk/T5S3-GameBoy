#pragma once
#include "paperboy_ui.h"
#include "paperboy_orientation.h"
static constexpr uint16_t PAPERBOY_LANDSCAPE_GAME_Y = 54;
static constexpr uint16_t PAPERBOY_LANDSCAPE_FULLSCREEN_X = 180;
static constexpr uint16_t PAPERBOY_LANDSCAPE_FULLSCREEN_WIDTH = 600;
static constexpr uint16_t PAPERBOY_LANDSCAPE_FULLSCREEN_HEIGHT = 540;

bool paperboy_landscape_fullscreen();
void paperboy_landscape_set_fullscreen(bool enabled);
void paperboy_landscape_draw(uint8_t *canvas, uint8_t *panel, const uint8_t *game,
                            uint8_t buttons, bool power_on, bool save_available,
                            const PaperboyBatteryStatus *battery, const char *notice,
                            int64_t monotonic_us);
void paperboy_landscape_game(const uint8_t *game, uint8_t *panel);
uint8_t paperboy_landscape_buttons(const touch_state_t *touch);
uint32_t paperboy_landscape_actions(const touch_state_t *touch);
