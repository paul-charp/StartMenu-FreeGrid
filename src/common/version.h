#pragma once

// build.ps1 -Version writes build\version_override.h and defines
// STARTGRID_VERSION_OVERRIDE.
#ifdef STARTGRID_VERSION_OVERRIDE
#include "version_override.h"
#else
#define STARTGRID_VER_MAJOR 0
#define STARTGRID_VER_MINOR 2
#define STARTGRID_VER_PATCH 0
#define STARTGRID_VERSION_TEXT "0.2.0"
#define STARTGRID_VERSION_STR L"0.2.0"
#endif
