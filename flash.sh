#!/bin/sh
# Flash the patched K3 Max firmware: waits for the keyboard's DFU bootloader,
# then writes it. Usage: sudo sh flash.sh [firmware.bin]
set -e
fw="${1:-$(dirname "$0")/keychron_k3_max_ansi_rgb_desk.bin}"
[ -f "$fw" ] || { echo "firmware not found: $fw"; exit 1; }

echo "Firmware: $fw"
echo "Now: mode switch on Cable, unplug the cable, hold Esc and plug it back in."
echo "Waiting for DFU mode (up to 60s)..."

i=0
until lsusb -d 0483:df11 >/dev/null 2>&1; do
  i=$((i + 1))
  [ "$i" -le 120 ] || { echo "No DFU device showed up; nothing was written."; exit 1; }
  sleep 0.5
done

echo "DFU device found, flashing..."
sleep 1
dfu-util -a 0 -d 0483:df11 -s 0x08000000:leave -D "$fw"
echo "Done. Move the mode switch to Bluetooth for auto mode."
