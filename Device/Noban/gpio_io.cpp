#include "gpio_io.hpp"

#include <iostream>

#if __has_include(<gpiod.h>)
#define HAVE_GPIOD 1
#include <gpiod.h>
#else
#define HAVE_GPIOD 0
#endif

namespace gpioio {

namespace {
constexpr unsigned BTN_LINE = 17;          // BCM17, physical pin 11
constexpr unsigned LED_R = 7, LED_G = 1, LED_B = 12;
constexpr const char* CHIP_PATH = "/dev/gpiochip0";   // Pi 4 pinctrl-bcm2711

#if HAVE_GPIOD
gpiod_chip*         g_chip   = nullptr;
gpiod_line_request* g_btn_req = nullptr;   // input (pull-up)
gpiod_line_request* g_led_req = nullptr;   // 3 outputs
bool g_ok = false;

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
#endif  // HAVE_GPIOD
}  // namespace

bool init() {
#if !HAVE_GPIOD
    return false;
#else
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
    std::cerr << "[gpio] libgpiod ready (button BCM17, LED 7/1/12)\n";
    return true;
#endif
}

bool button_pressed() {
#if !HAVE_GPIOD
    return false;
#else
    if (!g_ok) return false;
    // Active-low: pull-up holds the line high (INACTIVE→value 1) when idle;
    // pressing connects to GND (value 0). gpiod returns ACTIVE for high.
    enum gpiod_line_value v = gpiod_line_request_get_value(g_btn_req, BTN_LINE);
    return v == GPIOD_LINE_VALUE_INACTIVE;   // 0 = pressed
#endif
}

void led(bool r, bool g, bool b) {
#if HAVE_GPIOD
    if (!g_ok) return;
    unsigned offs[3] = {LED_R, LED_G, LED_B};
    enum gpiod_line_value vals[3] = {
        r ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        g ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        b ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
    };
    gpiod_line_request_set_values_subset(g_led_req, 3, offs, vals);
#else
    (void)r; (void)g; (void)b;
#endif
}

}  // namespace gpioio
