// Copyright 2026 bluish — GPL-2.0-or-later
#pragma once

/* Route VIA / Keychron raw HID that arrives over the wireless module
 * (2.4G dongle or Bluetooth) to the same handler as USB. Keychron's
 * spelling. */
#define WILRESS_RAW_ENABLE

/* Auto mode keys (mode switch on Cable), see remote.c */
#define AUTO_USB QK_USER_0 /* Fn+5: USB cable */
#define AUTO_OS QK_USER_1  /* Fn+6: toggle Mac/Windows */

/* Stay enumerated on USB while typing over Bluetooth/2.4G, so the computer
 * the cable is plugged into can always send remote commands (as Keychron
 * does on the K0/K10 Max). */
#define KEEP_USB_CONNECTION_IN_WIRELESS_MODE
