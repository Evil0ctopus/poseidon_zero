#include "linux_input.h"

#include <iostream>

namespace {

struct TestState {
    bool pressed{false};
    bool consume{true};
    int shortcuts{0};
    int releases{0};
    int clicks{0};
};

void read_key(lv_indev_t* indev, lv_indev_data_t* data) {
    const auto* state = static_cast<TestState*>(lv_indev_get_driver_data(indev));
    data->key = LV_KEY_ENTER;
    data->state = state->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool on_key(uint32_t key, const char*, bool long_pressed, void* user_data) {
    auto* state = static_cast<TestState*>(user_data);
    if (key == LV_KEY_ENTER && !long_pressed) {
        ++state->shortcuts;
        return state->consume;
    }
    return false;
}

void on_release(uint32_t key, const char*, void* user_data) {
    if (key == LV_KEY_ENTER) {
        ++static_cast<TestState*>(user_data)->releases;
    }
}

void on_click(lv_event_t* event) {
    ++static_cast<TestState*>(lv_event_get_user_data(event))->clicks;
}

void press_and_release(lv_indev_t* indev, TestState& state) {
    state.pressed = true;
    lv_indev_read(indev);
    state.pressed = false;
    lv_indev_read(indev);
}

} // namespace

int main() {
    lv_init();
    auto* display = lv_display_create(320, 170);
    auto* group = lv_group_create();
    auto* button = lv_button_create(lv_display_get_screen_active(display));
    TestState state;
    lv_group_add_obj(group, button);
    lv_obj_add_event_cb(button, on_click, LV_EVENT_CLICKED, &state);

    auto* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, display);
    lv_indev_set_group(indev, group);
    lv_indev_set_driver_data(indev, &state);
    lv_indev_set_read_cb(indev, read_key);
    lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
    platform::attach_key_router(indev);
    platform::set_global_key_listener(on_key, &state);
    platform::set_key_release_listener(on_release, &state);

    press_and_release(indev, state);
    if (state.shortcuts != 1 || state.releases != 1 || state.clicks != 0) {
        std::cerr << "Consumed Enter must not click the focused widget\n";
        return 1;
    }
    state.consume = false;
    press_and_release(indev, state);
    if (state.shortcuts != 2 || state.releases != 2 || state.clicks != 1) {
        std::cerr << "Unconsumed Enter must still click the focused widget\n";
        return 1;
    }

    platform::clear_global_key_listener(on_key, &state);
    platform::clear_key_release_listener(on_release, &state);
    lv_indev_delete(indev);
    lv_group_delete(group);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
