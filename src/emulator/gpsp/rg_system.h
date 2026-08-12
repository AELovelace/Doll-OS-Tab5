#pragma once

// Minimal Retro-Go allocation/logging compatibility used by the P4 gpSP fork.
// Doll-OS owns the actual display, input, audio and storage frontends.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

enum {
  MEM_ANY = 0,
  MEM_FAST = 1 << 0,
  MEM_SLOW = 1 << 1,
  MEM_8BIT = 1 << 2,
  MEM_NOPANIC = 1 << 3,
};

static inline void* rg_alloc(size_t size, unsigned flags) {
  uint32_t caps = MALLOC_CAP_8BIT;
  if (flags & MEM_FAST) {
    caps |= MALLOC_CAP_INTERNAL;
  } else if (flags & MEM_SLOW) {
    caps |= MALLOC_CAP_SPIRAM;
  }

  void* result = heap_caps_malloc(size, caps);
  if (!result && !(flags & (MEM_FAST | MEM_SLOW))) {
    result = malloc(size);
  }
  return result;
}

#define RG_LOGI(...) do { printf("[gba core] " __VA_ARGS__); printf("\n"); } while (0)

