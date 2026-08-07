cmake_minimum_required(VERSION 3.16)

#
# Slate firmware. One binary per board model (ADR-1); the dashboard arrives as
# JSON at runtime and nothing about it is compiled in.
#
# The project name decides the image name, and §11.1 fixes that: the host-side
# OTA helper is a `curl --data-binary @build/slate.bin`.
#
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(slate)
