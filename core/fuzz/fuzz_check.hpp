#pragma once

#include <cstdio>
#include <cstdlib>

// Invariant check that works in every build type (unlike assert) and gives
// the fuzzer a crash to minimize.
#define FUZZ_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FUZZ_CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (0)
