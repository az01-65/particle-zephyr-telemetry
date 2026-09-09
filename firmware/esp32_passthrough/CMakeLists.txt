# SPDX-License-Identifier: Apache-2.0
#
# esp32_passthrough - turns the Particle Argon's nRF52840 into a transparent
# USB<->UART bridge to its own onboard ESP32 Wi-Fi co-processor, so the
# ESP32 can be flashed and used directly from Arduino IDE / esptool.py.
# See src/main.c for the application-level design notes.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_passthrough)

target_sources(app PRIVATE src/main.c)
