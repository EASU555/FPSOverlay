#pragma once

// Version policy:
// - Increment VER_PATCH for each delivered update in the current feature line.
// - Refresh VER_BUILD_DATE / VER_BUILD_TIME for every delivered Release build.
#define VER_MAJOR 1
#define VER_MINOR 10
#define VER_PATCH 44
#define VER_BUILD 0

#define VER_BUILD_DATE "2026-07-16"
#define VER_BUILD_TIME "19:58"
#define VER_BUILD_STAMP "20260716-1958"

#define VER_STRINGIFY2(x) #x
#define VER_STRINGIFY(x) VER_STRINGIFY2(x)
#define VER_NUMBER_STRING \
    VER_STRINGIFY(VER_MAJOR) "." VER_STRINGIFY(VER_MINOR) "." \
    VER_STRINGIFY(VER_PATCH) "." VER_STRINGIFY(VER_BUILD)
#define VER_SHORT_STRING \
    "v" VER_STRINGIFY(VER_MAJOR) "." VER_STRINGIFY(VER_MINOR) "." \
    VER_STRINGIFY(VER_PATCH)
#define VER_DISPLAY_STRING \
    VER_SHORT_STRING " (" VER_BUILD_DATE " " VER_BUILD_TIME ")"
#define VER_FILE_STEM \
    "overlay_v" VER_STRINGIFY(VER_MAJOR) "." VER_STRINGIFY(VER_MINOR) "." \
    VER_STRINGIFY(VER_PATCH) "_" VER_BUILD_STAMP
