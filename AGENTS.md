# AGENTS.md

A patch on Keychron's own QMK tree for the Keychron K3 Max (ANSI RGB, USB
PID 0x0A30), not a fork: `build.sh` clones Keychron/qmk_firmware at a pinned
commit, applies `keychron-hooks.patch` and copies `keymap/` in as the `desk`
keymap. Unlike the other repositories, this one is hardware-specific by
nature.

## Layout

- `keychron-hooks.patch`: the changes to Keychron's code (raw HID over the
  wireless module, USB kept up in wireless mode, weak hooks). Keep it small;
  new behaviour goes in the keymap through the hooks.
- `keymap/`: the keymap, auto connection mode and raw HID commands
  (`remote.c`, command 0xC0).
- `build.sh`, `flash.sh`.

## Rules

- The raw HID command layout (0xC0 01..05) is public: host tools
  depend on it, so keep existing commands and reply bytes stable and add new
  ones after them.
- The Keychron Link dongle swallows raw HID 0xB0–0xBF; stay at 0xC0 and up.
- The wireless module forwards only Num, Caps and Scroll Lock LEDs to the
  keyboard; the LED command channel depends on that.
- GPL-2.0-or-later, as QMK.

## Testing

`sh build.sh` must build without new warnings beyond Keychron's own (it
needs `ALLOW_WARNINGS` for those). QMK's build runs its own Python tooling
in a venv under `build/`; that is upstream's, not this repository's.
Flashing needs the keyboard in DFU mode and a person at the keyboard; never
flash unattended. After flashing, send `C0 01` (status) and a `C0 02` switch
between hosts with any raw HID client.

## Commits

`type(scope): description` in English; body with why, then lowercase
bullets of what changed, then a `Validation:` line with what was actually run.
