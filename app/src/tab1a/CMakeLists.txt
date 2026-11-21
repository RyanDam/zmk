# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(CONFIG_ZMK_TAB1A_HARNESS app PRIVATE tab1a_harness.c)
target_sources_ifdef(CONFIG_ZMK_TAB1A_DEBUG app PRIVATE tab1a_debug.c)
