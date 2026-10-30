// Copyright 2026 bluish — GPL-2.0-or-later
//
// "Auto" connection mode plus remote control over raw HID.
//
// With the mode switch on Bluetooth the keyboard is in auto mode (on battery):
// the connection and the Mac/Windows layout are chosen in software, the
// layout is kept per connection, and both survive power cycles.
//   Fn+1/2/3  Bluetooth host 1/2/3     Fn+4  2.4G dongle
//   Fn+5      USB cable (only while one is plugged in)
//   Fn+6      toggle Mac/Windows for the current connection
// The Mac/Win switch becomes on/off in auto mode: Mac drops the radio, turns
// the lights off and lets the keyboard sleep (keys are ignored); Win wakes it
// and reconnects.
// Unplugging while on USB falls back to the last wireless connection.
// Cable (off when unplugged; the switch cuts the battery) and 2.4G behave
// as stock.
//
// Raw HID commands (USB, 2.4G dongle or Bluetooth):
//   C0 01                -> C0 01 <battery %> <usb power> <transport> <source> <auto> <os> <off>
//   C0 02 <target> <os>  -> C0 02 <target> <os> <accepted>, then switches connection
//                           target 1..3 = Bluetooth host, 24 = 2.4G,
//                           16 = USB (auto mode with a cable plugged in)
//   C0 03 <os>           -> C0 03 <os> <accepted>; layout for the current connection
//   os: 0 = keep, 1 = Mac, 2 = Windows
// The reply leaves over the link the command came in on, so a switch waits a
// moment before dropping that link.
//
// Host LED commands, for hosts that can't send raw HID (Bluetooth). The
// wireless module only forwards Num, Caps and Scroll Lock (Compose and Kana
// never arrive), so the command is two steps on Num + Scroll, leaving Caps
// alone: Num and Scroll lit together arm it, and the next state within
// 1.5 s picks the target: Num = BT1, Scroll = BT2, neither = 2.4G. Macs have
// neither lock, and a PC rarely lights both.

#include QMK_KEYBOARD_H
#include <string.h>
#include "keychron_common.h"
#include "transport.h"
#include "wireless.h"
#include "battery.h"
#include "lpm.h"
#include "lkbt51.h"
#include "indicator.h"

#define REMOTE_CMD 0xC0  // 0xB0-0xBF are eaten by the Keychron Link dongle
#define REMOTE_GET_STATUS 0x01
#define REMOTE_SWITCH_HOST 0x02
#define REMOTE_SET_OS 0x03
#define REMOTE_LED_DEBUG 0x04  // C0 04 -> C0 04 <host leds now> <last seen by led_update_user> <calls>
#define REMOTE_LED_HISTORY 0x05  // C0 05 -> C0 05 <last target> then 8 x (<leds> <ms since previous, /10>)
#define REMOTE_SWITCH_DELAY_MS 50

#define TARGET_USB 16
#define OS_KEEP 0
#define OS_IS_MAC 1
#define OS_IS_WIN 2

// Base layers, as set by the Mac/Win switch in k3_max.c
#define LAYER_MAC 0
#define LAYER_WIN 2

// eeconfig user word: [magic][target][windows mask][last wireless]; mask bit
// per connection (BT1, BT2, BT3, 2.4G, USB). Defaults: the PC's dongle, Macs on
// Bluetooth with the Mac layout, the PC (dongle and cable) on Windows.
#define AUTO_MAGIC 0xA5
#define DEFAULT_WIN_MASK 0x18

static bool     auto_mode      = false;
static bool     kb_off         = false;
static uint32_t led_armed_at   = 0;  // Num + Scroll seen: a target follows
static uint8_t  led_seen       = 0;  // diagnostics: last LED byte led_update_user saw
static uint8_t  led_hist[8][2];      // diagnostics: last LED bytes and gaps, newest last
static uint32_t led_hist_at    = 0;
static uint8_t  led_last_target = 0;
static uint8_t  led_calls      = 0;
static bool     layout_pending = false;  // apply the layout once the Fn key combo is released
static uint8_t  current_target = P24G_INDEX;
static uint8_t  last_wireless  = P24G_INDEX;
static uint8_t  win_mask       = DEFAULT_WIN_MASK;
static uint8_t  pending_target = 0;
static void     set_off(bool off);
static uint32_t pending_since  = 0;

static bool valid_connection(uint8_t target) {
    return (target >= 1 && target <= 3) || target == P24G_INDEX || target == TARGET_USB;
}

static uint8_t target_bit(uint8_t target) {
    if (target == P24G_INDEX) return 1 << 3;
    if (target == TARGET_USB) return 1 << 4;
    return 1 << (target - 1);
}

static void auto_save(void) {
    eeconfig_update_user(AUTO_MAGIC | ((uint32_t)current_target << 8) | ((uint32_t)win_mask << 16) |
                         ((uint32_t)last_wireless << 24));
}

static void auto_load(void) {
    uint32_t v = eeconfig_read_user();
    if ((v & 0xFF) != AUTO_MAGIC) return;
    uint8_t target = (v >> 8) & 0xFF;
    if (valid_connection(target)) current_target = target;
    win_mask = (v >> 16) & 0x1F;
    uint8_t wireless = (v >> 24) & 0xFF;
    if (valid_connection(wireless) && wireless != TARGET_USB) last_wireless = wireless;
}

static transport_t target_transport(uint8_t target) {
    if (target == TARGET_USB) return TRANSPORT_USB;
    if (target == P24G_INDEX) return TRANSPORT_P2P4;
    return TRANSPORT_BLUETOOTH;
}

static uint8_t current_os(void) {
    return get_highest_layer(default_layer_state) == LAYER_WIN ? OS_IS_WIN : OS_IS_MAC;
}

static void apply_os(uint8_t target) {
    default_layer_set(1UL << ((win_mask & target_bit(target)) ? LAYER_WIN : LAYER_MAC));
}

static void remember_os(uint8_t target, uint8_t os) {
    if (os == OS_IS_WIN) win_mask |= target_bit(target);
    if (os == OS_IS_MAC) win_mask &= ~target_bit(target);
}

/* Auto mode picked a connection: remember it and load its layout. */
static void select_target(uint8_t target) {
    current_target = target;
    if (target != TARGET_USB) last_wireless = target;
    apply_os(target);
    auto_save();
}

static void schedule(uint8_t target) {
    pending_target = target;
    pending_since  = timer_read32();
}

/* Mode switch moved (or boot): Bluetooth means auto, restoring the last choice. */
transport_t dip_switch_transport_user(transport_t stock) {
    auto_mode = stock == TRANSPORT_BLUETOOTH;
    if (!auto_mode) {
        if (kb_off) {
            kb_off = false;  // leaving auto mode always turns the keyboard back on
            rgb_matrix_reload_from_eeprom();
        }
        return stock;
    }

    auto_load();
    if (current_target == TARGET_USB && !usb_power_connected()) current_target = last_wireless;
    // The Mac/Win switch may have been read before auto mode existed; the
    // pin reads low on Mac (off).
    set_off(!gpio_read_pin(B14));
    apply_os(current_target);
    // set_transport() reconnects the dongle by itself; a Bluetooth host
    // still needs picking once the module is up.
    if (current_target >= 1 && current_target <= 3) schedule(current_target);
    return target_transport(current_target);
}

static void set_off(bool off) {
    if (off == kb_off) return;
    kb_off = off;
    if (off) {
        pending_target = 0;
        clear_keyboard();
        wireless_disconnect();
        rgb_matrix_disable_noeeprom();  // the low-power task only sleeps with the lights off
    } else {
        rgb_matrix_reload_from_eeprom();
        schedule(current_target);  // reconnect where we were
    }
}

/* Mac/Win switch, after k3_max.c has set the base layer from it. In auto mode
 * it is on/off (Mac = off), so restore the connection's own layout over that. */
void dip_switch_os_user(bool mac) {
    if (!auto_mode) return;
    apply_os(current_target);
    set_off(mac);
}

/* Off: let the Mac/Win switch wake the MCU from STOP. The stock code parks the
 * switch pin with a pull-down, which can't see it move. The Mac position
 * closes the contact, so the internal pull-up draws ~80 uA while off. */
void lpm_enter_low_power_kb(void) {
    if (!kb_off) return;
    palSetLineMode(B14, PAL_MODE_INPUT_PULLUP);
    palEnableLineEvent(B14, PAL_EVENT_MODE_BOTH_EDGES);
}

void lpm_post_wakeup(void) {
    palDisableLineEvent(B14);  // dip_switch_init() already restored the pin
}

/* Pick a connection from an Fn key: remember it now, but change the base
 * layer only on release. Changing it while the key is down makes QMK look the
 * release up on the new layer (the Windows base sits above the Mac Fn layer),
 * so the stock handler never sees BT_HSTn/P2P4G released and, 2 s later,
 * starts pairing. */
static void select_target_from_key(uint8_t target) {
    current_target = target;
    if (target != TARGET_USB) last_wireless = target;
    auto_save();
    layout_pending = true;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (!auto_mode) return keycode != AUTO_USB && keycode != AUTO_OS;
    if (kb_off) return false;

    if (!record->event.pressed) {
        switch (keycode) {
            case BT_HST1 ... BT_HST3:
            case P2P4G:
            case AUTO_USB:
            case AUTO_OS:
                if (layout_pending) {
                    layout_pending = false;
                    apply_os(current_target);
                }
                break;
        }
        return keycode != AUTO_USB && keycode != AUTO_OS;
    }

    switch (keycode) {
        case BT_HST1 ... BT_HST3:
            // Switch transport, then let the stock handler connect (and keep
            // its hold-to-pair behaviour).
            if (get_transport() != TRANSPORT_BLUETOOTH) set_transport(TRANSPORT_BLUETOOTH);
            select_target_from_key(keycode - BT_HST1 + 1);
            return true;
        case P2P4G:
            if (get_transport() != TRANSPORT_P2P4) set_transport(TRANSPORT_P2P4);
            select_target_from_key(P24G_INDEX);
            return true;
        case AUTO_USB:
            if (!usb_power_connected()) return false;  // no cable, nothing to switch to
            if (get_transport() != TRANSPORT_USB) set_transport(TRANSPORT_USB);
            select_target_from_key(TARGET_USB);
            return false;
        case AUTO_OS:
            remember_os(current_target, current_os() == OS_IS_MAC ? OS_IS_WIN : OS_IS_MAC);
            auto_save();
            layout_pending = true;
            return false;
    }
    return true;
}

static bool valid_target(uint8_t target) {
    if (target == TARGET_USB) return auto_mode && usb_power_connected();
    return valid_connection(target);
}

bool kc_raw_hid_rx_user(uint8_t src, uint8_t *data, uint8_t length) {
    if (data[0] != REMOTE_CMD) return false;

    switch (data[1]) {
        case REMOTE_GET_STATUS:
            data[2] = battery_get_percentage();
            data[3] = usb_power_connected();
            data[4] = get_transport();
            data[5] = src;
            data[6] = auto_mode;
            data[7] = current_os();
            data[8] = kb_off;
            break;

        case REMOTE_SWITCH_HOST:
            data[4] = !kb_off && valid_target(data[2]) && data[3] <= OS_IS_WIN;
            if (data[4]) {
                remember_os(data[2], data[3]);
                schedule(data[2]);
            }
            break;

        case REMOTE_SET_OS:
            data[3] = data[2] == OS_IS_MAC || data[2] == OS_IS_WIN;
            if (data[3]) {
                remember_os(current_target, data[2]);
                if (auto_mode) {
                    apply_os(current_target);
                    auto_save();
                } else {
                    default_layer_set(1UL << (data[2] == OS_IS_WIN ? LAYER_WIN : LAYER_MAC));
                }
            }
            break;

        case REMOTE_LED_HISTORY:
            data[2] = led_last_target;
            memcpy(&data[3], led_hist, sizeof(led_hist));
            break;

        case REMOTE_LED_DEBUG:
            data[2] = host_keyboard_leds();
            data[3] = led_seen;
            data[4] = led_calls;
            break;

        default:
            data[0] = 0xFF;
            break;
    }
    return true;
}

bool led_update_user(led_t led) {
    led_seen = led.raw;
    led_calls++;
    uint32_t gap = led_hist_at ? timer_elapsed32(led_hist_at) / 10 : 255;
    led_hist_at  = timer_read32();
    memmove(led_hist[0], led_hist[1], sizeof(led_hist) - sizeof(led_hist[0]));
    led_hist[7][0] = led.raw;
    led_hist[7][1] = gap > 255 ? 255 : gap;
    if (!auto_mode || kb_off) return true;
    if (led.num_lock && led.scroll_lock) {
        led_armed_at = timer_read32();
    } else if (led_armed_at && timer_elapsed32(led_armed_at) < 1500) {
        led_armed_at = 0;
        led_last_target = led.num_lock ? 1 : led.scroll_lock ? 2 : P24G_INDEX;
        schedule(led_last_target);
    } else {
        led_armed_at = 0;
    }
    return true;
}

void keychron_task_user(void) {
    // Off: a key press wakes the MCU and the stock wake path reconnects;
    // drop the link again so it can go back to sleep.
    // The reconnect also starts the connection indicator, which turns the
    // RGB back on, and the low-power task never sleeps with lights on.
    if (kb_off) {
        static wt_state_t last = WT_RESET;
        wt_state_t state = wireless_get_state();
        if (state != last && (state == WT_CONNECTED || state == WT_RECONNECTING)) wireless_disconnect();
        last = state;
        if (indicator_is_running()) indicator_stop();
        if (rgb_matrix_is_enabled()) rgb_matrix_disable_noeeprom();
        return;
    }

    // Cable pulled while typing over USB: go back to the last wireless link.
    if (auto_mode && !pending_target && current_target == TARGET_USB && !usb_power_connected()) {
        schedule(last_wireless);
    }
    if (!pending_target || timer_elapsed32(pending_since) < REMOTE_SWITCH_DELAY_MS) return;

    uint8_t target = pending_target;
    pending_target = 0;

    // Same calls as the Fn+1..5 paths. Outside auto mode this overrides the
    // switch position until the switch is moved again.
    transport_t want = target_transport(target);
    if (get_transport() != want) {
        set_transport(want);
    } else if (want == TRANSPORT_BLUETOOTH) {
        // Bluetooth host to Bluetooth host: the module ignored a bare
        // connect while still linked to the old host (the LED history showed
        // the right target and nothing happened). Drop the link first, as
        // bt_transport_enable() does.
        extern wt_func_t wireless_transport;
        wireless_disconnect();
        uint32_t t = timer_read32();
        while (timer_elapsed32(t) < 100) wireless_transport.task();
    }
    if (want == TRANSPORT_BLUETOOTH) wireless_connect_ex(target, 0);
    if (auto_mode) {
        select_target(target);
    } else {
        apply_os(target);
    }
}
