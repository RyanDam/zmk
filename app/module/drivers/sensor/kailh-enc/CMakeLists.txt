# Copyright (c) 2020 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_include_directories(.)

zephyr_library()

zephyr_library_sources(kailh_enc.c)
zephyr_library_sources_ifdef(CONFIG_KAILH_ENC_TRIGGER kailh_enc_trigger.c)