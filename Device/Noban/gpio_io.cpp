#include "gpio_io.hpp"

#include <iostream>

#if __has_include(<gpiod.h>)
#define HAVE_GPIOD 1
#include <gpiod.h>
// API-version split: libgpiod 1.x (Raspberry Pi OS Bookworm) defines the
// bulk-API macro GPIOD_LINE_BULK_MAX_LINES; libgpiod 2.x (Trixie) removed the
// bulk API entirely, so the macro's absence means the newer v2 API. (Note the
// request-flag *constants* are enum values in v1, not #defines, so they can't
// be tested with #ifdef — the bulk macro is the reliable discriminator.) We
// support both because the device standardises on Bookworm (BlueZ 5.66 for BLE
// onboarding) while older builds shipped on Trixie.
#ifdef GPIOD_LINE_BULK_MAX_LINES
#define GPIOD_V1 1
#else
#define GPIOD_V1 0
#endif
#else
#define HAVE_GPIOD 0
#endif

namespace gpioio {

namespace {
constexpr unsigned BTN_LINE = 17;          // BCM17, physical pin 11
constexpr unsigned LED_R = 7, LED_G = 1, LED_B = 12;
constexpr const char* CHIP_PATH = "/dev/gpiochip0";   // Pi 4 pinctrl-bcm2711

#if HAVE_GPIOD
bool g_ok = false;

#if GPIOD_V1
// ---- libgpiod 1.x (Bookworm) ----
gpiod_chip* g_chip   = nullptr;
gpiod_line* g_btn    = nullptr;            // input (pull-up)
gpiod_line* g_led[3] = {nullptr, nullptr, nullptr};  // 3 outputs (R,G,B)
#else
// ---- libgpiod 2.x (Trixie) ----
gpiod_chip*         g_chip   = nullptr;
gpiod_line_request* g_btn_req = nullptr;   // input (pull-up)
gpiod_line_request* g_led_req = nullptr;   // 3 outputs

// Request a single input line with internal pull-up.
gpiod_line_request* request_button(gpiod_chip* chip) {
    gpiod_line_settings* s = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(s, GPIOD_LINE_BIAS_PULL_UP);

    gpiod_line_config* lc = gpiod_line_config_new();
    unsigned off = BTN_LINE;
    gpiod_line_config_add_line_settings(lc, &off, 1, s);

    gpiod_request_config* rc = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rc, "elderly-button");

    gpiod_line_request* req = gpiod_chip_request_lines(chip, rc, lc);
    gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(s);
    return req;
}

// Request the three LED lines as outputs, initially off.
gpiod_line_request* request_leds(gpiod_chip* chip) {
    gpiod_line_settings* s = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(s, GPIOD_LINE_VALUE_INACTIVE);

    gpiod_line_config* lc = gpiod_line_config_new();
    unsigned offs[3] = {LED_R, LED_G, LED_B};
    gpiod_line_config_add_line_settings(lc, offs, 3, s);

    gpiod_request_config* rc = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rc, "elderly-led");

    gpiod_line_request* req = gpiod_chip_request_lines(chip, rc, lc);
    gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(s);
    return req;
}
#endif  // GPIOD_V1
#endif  // HAVE_GPIOD
}  // namespace

bool init() {
#if !HAVE_GPIOD
    return false;
#elif GPIOD_V1
    // ---- libgpiod 1.x (Bookworm) ----
    if (g_ok) return true;
    g_chip = gpiod_chip_open(CHIP_PATH);
    if (!g_chip) {
        std::cerr << "[gpio] " << CHIP_PATH << " unavailable — button/LED disabled\n";
        return false;
    }
    g_btn = gpiod_chip_get_line(g_chip, BTN_LINE);
    bool ok = g_btn &&
              gpiod_line_request_input_flags(
                  g_btn, "elderly-button",
                  GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) == 0;
    const unsigned led_off[3] = {LED_R, LED_G, LED_B};
    for (int i = 0; i < 3 && ok; ++i) {
        g_led[i] = gpiod_chip_get_line(g_chip, led_off[i]);
        ok = g_led[i] &&
             gpiod_line_request_output(g_led[i], "elderly-led", 0) == 0;
    }
    if (!ok) {
        std::cerr << "[gpio] line request failed (in use?) — button/LED disabled\n";
        if (g_btn) gpiod_line_release(g_btn);
        for (int i = 0; i < 3; ++i)
            if (g_led[i]) gpiod_line_release(g_led[i]);
        gpiod_chip_close(g_chip);
        g_chip = nullptr; g_btn = nullptr;
        g_led[0] = g_led[1] = g_led[2] = nullptr;
        return false;
    }
    g_ok = true;
    std::cerr << "[gpio] libgpiod v1 ready (button BCM17, LED 7/1/12)\n";
    return true;
#else
    // ---- libgpiod 2.x (Trixie) ----
    if (g_ok) return true;
    g_chip = gpiod_chip_open(CHIP_PATH);
    if (!g_chip) {
        std::cerr << "[gpio] " << CHIP_PATH << " unavailable — button/LED disabled\n";
        return false;
    }
    g_btn_req = request_button(g_chip);
    g_led_req = request_leds(g_chip);
    if (!g_btn_req || !g_led_req) {
        std::cerr << "[gpio] line request failed (in use?) — button/LED disabled\n";
        if (g_btn_req) gpiod_line_request_release(g_btn_req);
        if (g_led_req) gpiod_line_request_release(g_led_req);
        gpiod_chip_close(g_chip);
        g_chip = nullptr; g_btn_req = nullptr; g_led_req = nullptr;
        return false;
    }
    g_ok = true;
    std::cerr << "[gpio] libgpiod v2 ready (button BCM17, LED 7/1/12)\n";
    return true;
#endif
}

bool button_pressed() {
#if !HAVE_GPIOD
    return false;
#elif GPIOD_V1
    if (!g_ok) return false;
    // Active-low: pull-up holds the line high (1) when idle; pressing connects
    // to GND (0).
    return gpiod_line_get_value(g_btn) == 0;   // 0 = pressed
#else
    if (!g_ok) return false;
    // Active-low: pull-up holds the line high (INACTIVE→value 1) when idle;
    // pressing connects to GND (value 0). gpiod returns ACTIVE for high.
    enum gpiod_line_value v = gpiod_line_request_get_value(g_btn_req, BTN_LINE);
    return v == GPIOD_LINE_VALUE_INACTIVE;   // 0 = pressed
#endif
}

void led(bool r, bool g, bool b) {
#if !HAVE_GPIOD
    (void)r; (void)g; (void)b;
#elif GPIOD_V1
    if (!g_ok) return;
    gpiod_line_set_value(g_led[0], r ? 1 : 0);
    gpiod_line_set_value(g_led[1], g ? 1 : 0);
    gpiod_line_set_value(g_led[2], b ? 1 : 0);
#else
    if (!g_ok) return;
    unsigned offs[3] = {LED_R, LED_G, LED_B};
    enum gpiod_line_value vals[3] = {
        r ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        g ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        b ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
    };
    gpiod_line_request_set_values_subset(g_led_req, 3, offs, vals);
#endif
}

}  // namespace gpioio
