/*
 * The implementation of stb_image, and nothing else.
 *
 * stb_image is a single-header library: every file that includes it gets the
 * declarations, and exactly one has to define STB_IMAGE_IMPLEMENTATION to get
 * the code. That one used to be main.c, which worked and put the decoder for
 * every picture in the system in the same translation unit as the entry point.
 *
 * Its own file, because a test target that needs a decoder should not have to
 * link the compositor to get one. `recon_ico_decode` hands PNG-shaped icons to
 * stb_image, so the suite that feeds it malformed input needs this and needs
 * nothing else from main.c -- and the whole point of the test targets in this
 * project is that none of them needs a display.
 *
 * Warnings are turned off for this file alone, in CMakeLists.txt. It is
 * borrowed code: the rule stated in docs/ROADMAP.md is that borrowed code is
 * owned once it is here, and owning it means not rewriting it to satisfy a
 * warning switch this project chose and its author did not.
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
