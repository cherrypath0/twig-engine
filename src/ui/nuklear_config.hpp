#pragma once
// Single source of truth for Nuklear's compile-time configuration. EVERY
// translation unit that touches Nuklear must include THIS header (never
// <nuklear.h> directly) so the flags stay identical everywhere — mismatched
// flags give you silent ABI corruption.
//
// The one .cpp that provides the implementation defines NK_IMPLEMENTATION
// *before* including this header (see src/core/thirdparty.cpp).

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT

#include "nuklear.h"
