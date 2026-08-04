# SPDX-License-Identifier: Apache-2.0
#
# argon_gateway - BLE central "gateway" firmware for the Particle Argon
# (nRF52840). See src/main.c for the application-level design notes.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(argon_gateway)

target_sources(app PRIVATE src/main.c)
