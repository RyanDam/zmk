# SPDX-License-Identifier: MIT

zephyr_include_directories(.)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library()

zephyr_library_sources(mpr121.c mpr121_ab_filter.c)
