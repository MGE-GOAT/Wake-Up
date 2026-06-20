// libgpiod v2 GPIO access for the push button + RGB status LED.
//
// Pi OS Trixie removed the legacy /sys/class/gpio sysfs interface, so the
// old export/value file approach silently fails (button stuck, LED dark).
// This wraps libgpiod 2.x (the supported character-device API) for:
//   • button on BCM17, input + internal pull-up, active-low (button→GND)
//   • RGB LED on BCM 7 (R) / 1 (G) / 12 (B), output, common-cathode
//
// All functions are no-ops / return false on a machine without the GPIO
// chip (dev laptop) so the same binary still runs there.

#pragma once

namespace gpioio {

// Open /dev/gpiochip0 and request the button + LED lines. Idempotent.
// Returns false if libgpiod/the chip is unavailable (caller falls back to
// SIGUSR1 for the button and skips LED writes).
bool init();

// True while the button is physically held (active-low debounced read).
bool button_pressed();

// Drive the three LED channels (1 = lit, common-cathode). Safe no-op if
// init() failed.
void led(bool r, bool g, bool b);

}  // namespace gpioio
