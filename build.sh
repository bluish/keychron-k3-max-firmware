#!/bin/sh
# Build the patched K3 Max (ANSI RGB) firmware from Keychron's own QMK tree:
# fetches it at a known commit, applies keychron-hooks.patch, adds the
# "desk" keymap and builds. Needs arm-none-eabi-gcc/newlib and python3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work="${WORK:-$here/build}"
commit=2b493ff  # Keychron/qmk_firmware 2025q3, Oct 2026

if [ ! -d "$work/qmk" ]; then
  git clone --filter=blob:none --branch 2025q3 --single-branch https://github.com/Keychron/qmk_firmware.git "$work/qmk"
fi
cd "$work/qmk"
git checkout --quiet "$commit"
git submodule update --init --depth 1 lib/chibios lib/chibios-contrib lib/printf
git apply --check "$here/keychron-hooks.patch" 2>/dev/null && git apply "$here/keychron-hooks.patch"

keymap=keyboards/keychron/k3_max/ansi/rgb/keymaps/desk
rm -rf "$keymap" && cp -r "$here/keymap" "$keymap"

[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install --quiet -r requirements.txt qmk; }
PATH="$PWD/.venv/bin:$PATH" QMK_HOME="$PWD" make -j"$(nproc)" keychron/k3_max/ansi/rgb:desk
cp keychron_k3_max_ansi_rgb_desk.bin "$here/"
echo "Built $here/keychron_k3_max_ansi_rgb_desk.bin; flash with: sudo sh $here/flash.sh"
