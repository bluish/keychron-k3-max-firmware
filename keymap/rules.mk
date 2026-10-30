VIA_ENABLE = yes
SRC += remote.c
# Keychron's stock sources trip newer GCC warnings (unused variable in
# lkbt51.c, an 85-entry initializer for 84 LEDs in rgb.c).
ALLOW_WARNINGS = yes
