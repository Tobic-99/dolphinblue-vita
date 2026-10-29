#pragma once
// Vita build: route printf to stderr (the TTY / Vita3K log); stdout is not connected.
#include <cstdio>
#define printf(...) fprintf(stderr, __VA_ARGS__)
