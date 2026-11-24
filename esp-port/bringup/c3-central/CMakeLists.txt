# Copyright 2026 Coban Stationery — a13 bench harness
# ESP32-C3 BLE central that connects to the Coban Tab (open link, NO
# pairing/encryption), subscribes to its first HID Report characteristic
# and logs every notification + the disconnect reason. Bench replacement
# for a phone/Mac in the wedge bisection.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(c3_central)

target_sources(app PRIVATE src/main.c)
