#include "paperboy_ui.h"
#include <cassert>
#include <Arduino.h>

uint32_t test_now = 1000;
bool paperboy_is_landscape() { return false; }
uint32_t paperboy_landscape_actions(const touch_state_t *) { return 0; }
void night_light_init() {}
uint8_t night_light_brightness() { return 0; }
bool night_light_set_brightness(uint8_t) { return true; }
void paperboy_ui_controller_page_changed() {}

int main() {
  paperboy_ui_init();
  touch_state_t touch{};
  touch.touched = true;
  touch.points = 1;
  touch.x[0] = 270;
  touch.y[0] = 630; // Center of the visible Gamepad Test row.
  assert(paperboy_ui_map_actions(&touch, PaperboyPage::Settings) ==
         PAPERBOY_ACTION_GAMEPAD_TEST);
  assert(paperboy_ui_map_actions(&touch, PaperboyPage::Settings) == 0);
  touch.touched = false;
  touch.points = 0;
  paperboy_ui_map_actions(&touch, PaperboyPage::Settings);
  test_now += 150;
  touch.touched = true;
  touch.points = 1;
  assert(paperboy_ui_map_actions(&touch, PaperboyPage::Settings) ==
         PAPERBOY_ACTION_GAMEPAD_TEST);
  paperboy_ui_on_page_changed();
  assert(paperboy_ui_map_actions(&touch, PaperboyPage::GamepadTest) == 0);
  const auto tap = [&](PaperboyPage page, unsigned x, unsigned y, uint32_t action) {
    touch.touched = false; touch.points = 0;
    paperboy_ui_map_actions(&touch, page);
    test_now += 150;
    touch.touched = true; touch.points = 1; touch.x[0] = x; touch.y[0] = y;
    assert(paperboy_ui_map_actions(&touch, page) == action);
    assert(paperboy_ui_map_actions(&touch, page) == 0);
  };
  tap(PaperboyPage::Settings, 270, 770, PAPERBOY_ACTION_DISPLAY);
  paperboy_ui_on_page_changed();
  assert(paperboy_ui_map_actions(&touch, PaperboyPage::Display) == 0);
  tap(PaperboyPage::Display, 140, 416, PAPERBOY_ACTION_FPS_DOWN);
  tap(PaperboyPage::Display, 400, 416, PAPERBOY_ACTION_FPS_UP);
  tap(PaperboyPage::Display, 270, 532, PAPERBOY_ACTION_FPS_DEFAULT);
}
