# Keychron K3 Max: auto mode and remote control

A small patch on **Keychron's own QMK tree** (not a fork: `build.sh` fetches
[Keychron/qmk_firmware](https://github.com/Keychron/qmk_firmware) at a known
commit, applies [`keychron-hooks.patch`](keychron-hooks.patch) and adds the
[`keymap/`](keymap) directory). Built and used on a K3 Max **ANSI RGB**
(USB PID `0x0A30`).

## What it adds

**Auto mode, on the Bluetooth position of the mode switch** (on battery):

| Keys | Connection |
|---|---|
| Fn+1 / Fn+2 / Fn+3 | Bluetooth host 1 / 2 / 3 |
| Fn+4 | 2.4G dongle |
| Fn+5 | USB cable (only while one is plugged in) |
| Fn+6 | toggle Mac / Windows layout for the current connection |

- The connection and a Mac/Windows layout per connection are stored in
  EEPROM and survive power cycles.
- The **Mac/Win switch becomes off/on**: Mac turns the radio and lights off,
  ignores keys and lets the keyboard sleep; Win wakes it and reconnects. The
  switch pin is set up to wake the MCU from STOP while off.
- Unplugging while on USB falls back to the last wireless connection.
- **Cable** and **2.4G** positions behave as stock. Cable with no cable
  plugged in is off: that position cuts the battery in hardware.

**Remote control** over raw HID, from the USB cable or through the 2.4G
dongle (32-byte VIA reports, usage page `0xFF60`):

| Command | Reply |
|---|---|
| `C0 01` | `C0 01 <battery %> <usb power> <transport> <source> <auto> <layout> <off>` |
| `C0 02 <target> <layout>` | `C0 02 <target> <layout> <accepted>`, then switches |
| `C0 03 <layout>` | `C0 03 <layout> <accepted>` |
| `C0 04` | `C0 04 <host LEDs now> <last LEDs seen by led_update_user> <call count>` (diagnostics) |
| `C0 05` | `C0 05 <last LED-command target>` + the last 8 LED states with the gap before each, in 10 ms (diagnostics) |

`target`: 1–3 Bluetooth host, 24 = 2.4G, 16 = USB (auto mode, cable present).
`layout`: 0 = keep, 1 = Mac, 2 = Windows. A switch waits 50 ms so the reply
leaves over the link the command came in on.

**Host LED commands**, for hosts on Bluetooth (which can't send raw HID).
The wireless module only forwards Num, Caps and Scroll Lock to the keyboard
(Compose and Kana never arrive; measured with `C0 04`), so the command is two
steps on Num + Scroll, leaving Caps alone: light Num and Scroll together to
arm it, then within 1.5 s the next state picks the target (Num = BT1,
Scroll = BT2, neither = 2.4G). Macs have neither lock and a PC rarely lights
both. A host app sets the lock LEDs through IOKit (macOS) to send the keyboard away.

Stock VIA and Keychron commands (lighting, layers, `0xA0`–`0xAB`) also work
through the dongle now, so RGB can be set without a cable.

## What the patch changes in Keychron's code

- `WILRESS_RAW_ENABLE` (sic): routes raw HID arriving through the LKBT51
  wireless module to the same handler as USB. Keychron's code path existed
  but had never been built: it called `via_raw_hid_receive`, which no longer
  exists (fixed to `raw_hid_receive`).
- `KEEP_USB_CONNECTION_IN_WIRELESS_MODE` (as on the K0/K10 Max): USB stays
  enumerated while typing over Bluetooth, so the computer the cable is plugged
  into can always reach the keyboard.
- Four weak hooks (plus QMK's own `led_update_user`): unknown raw HID commands (`kc_raw_hid_rx_user`), a
  periodic task (`keychron_task_user`), the mode switch's transport choice
  (`dip_switch_transport_user`) and the Mac/Win switch (`dip_switch_os_user`).
- `ALLOW_WARNINGS` in the keymap: Keychron's stock sources trip GCC 16
  warnings (an unused variable, an 85-entry initialiser for 84 LEDs).

## Things found on the way

- **The Keychron Link dongle swallows raw HID commands `0xB0`–`0xBF`** (it
  uses that range itself); `0xC0` and up pass through.
- **The wireless module forwards only Num, Caps and Scroll Lock** from the
  host, plus a bit of its own (`0x40`); Compose and Kana are dropped.
- **A Bluetooth-to-Bluetooth host switch needs the link dropped first.** A
  bare `wireless_connect_ex()` while still connected to the old host did
  nothing (the `C0 05` history showed the right target being picked); the
  patch disconnects and runs the wireless task for 100 ms first, as
  `bt_transport_enable()` does.
- **The Bluetooth module exposes no raw HID collection to macOS**, only
  keyboard, consumer and mouse, hence the LED channel above. Status (battery)
  still needs the cable or the dongle, so a Bluetooth host asks the computer that has one.

- **Keychron's VIA only saves lighting when one value changed.** Its
  `via_qmk_rgb_matrix_save()` compares the changed-values mask with `==`, so
  setting brightness, effect and colour and then saving once writes nothing
  (the values stay in RAM until the next transport change reloads EEPROM).
  A VIA client must save after every value.

## Build and flash

```sh
sudo pacman -S --needed arm-none-eabi-gcc arm-none-eabi-newlib dfu-util
sh build.sh            # -> keychron_k3_max_ansi_rgb_desk.bin
sudo sh flash.sh       # waits for DFU, then flashes
```

DFU mode: mode switch on **Cable**, unplug, hold **Esc**, plug the cable back
in. EEPROM settings (lighting, the auto-mode choices) reset after flashing;
Bluetooth pairings live in the wireless module and survive.

To go back to stock, use the same DFU mode with the
[Keychron Launcher](https://launcher.keychron.com/) firmware update.

GPL-2.0-or-later, as QMK.
