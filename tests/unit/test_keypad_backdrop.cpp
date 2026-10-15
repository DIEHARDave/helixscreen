// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"

#include <algorithm>
#include <array>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class KeypadBackdropFixture : public LVGLUITestFixture {
  public:
    KeypadBackdropFixture() {
        std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }

    ~KeypadBackdropFixture() override {
        // The keypad tree hangs off the screen; free it so the next case starts
        // with none.
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
    }
};

std::vector<lv_obj_t*> children_of(lv_obj_t* obj) {
    std::vector<lv_obj_t*> out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        out.push_back(lv_obj_get_child(obj, i));
    return out;
}

} // namespace

// The backdrop hangs off the screen, so anything clearing the screen takes it along
// without the keypad being told. Its address is then free for the next allocation, and
// lv_obj_is_valid() cannot tell a recycled address from the original object.
TEST_CASE_METHOD(KeypadBackdropFixture,
                 "Keypad: closing after the backdrop died leaves its recycled address alone",
                 "[keypad]") {
    lv_obj_t* screen = lv_screen_active();
    const std::vector<lv_obj_t*> before = children_of(screen);

    ui_keypad_config_t cfg{};
    cfg.min_value = 0;
    cfg.max_value = 300;
    cfg.title_label = "Test";
    ui_keypad_show(&cfg);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ui_keypad_is_visible());

    // The show adds two backdrops to the screen besides the keypad itself: the keypad's
    // own, raised before the push, and the one navigation gives a first overlay during
    // it. The keypad's is therefore the lower of the two.
    std::vector<lv_obj_t*> added;
    for (lv_obj_t* c : children_of(screen)) {
        if (std::find(before.begin(), before.end(), c) == before.end() &&
            lv_obj_find_by_name(c, "header_title") == nullptr) {
            added.push_back(c);
        }
    }
    REQUIRE(added.size() == 2);
    lv_obj_t* backdrop = added.front();
    const lv_obj_class_t* backdrop_class = lv_obj_get_class(backdrop);

    lv_obj_delete(backdrop);

    // Recycle the freed address into an unrelated widget. glibc hands a just-freed
    // block of the same size straight back, so this lands within a few tries;
    // an allocator that never recycles (ASan's quarantine) leaves nothing to test.
    std::vector<lv_obj_t*> spares;
    lv_obj_t* victim = nullptr;
    for (int i = 0; i < 256 && victim == nullptr; i++) {
        lv_obj_t* obj = lv_obj_class_create_obj(backdrop_class, screen);
        lv_obj_class_init_obj(obj);
        if (obj == backdrop)
            victim = obj;
        else
            spares.push_back(obj);
    }
    for (lv_obj_t* s : spares)
        lv_obj_delete(s);
    if (victim == nullptr)
        SKIP("the allocator did not recycle the backdrop's address");

    ui_keypad_hide();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(100);

    // A deferred delete would have moved it to the top layer and then freed it.
    REQUIRE(lv_obj_is_valid(victim));
    CHECK(lv_obj_get_parent(victim) == screen);
    lv_obj_delete(victim);
}
