// GenMC model of the SPSC ring (core/include/adsb/common/spsc_ring.hpp):
// try_push / try_pop, and with -DBATCH try_push_n / try_pop_n, transcribed
// line for line, including the producer's and consumer's cached copies of
// the other side's index. GenMC explores every execution the C11 (RC11)
// memory model allows.
//
// Checked:
//   * every message arrives exactly once, in order, with intact contents;
//   * no data race on a slot: slots are plain (non-atomic) memory, as in the
//     C++ ring, so a producer overwriting a slot the consumer is still
//     reading, or a consumer reading a slot before it is written, is a race
//     GenMC reports.
//
// Capacity 2 with 4 messages forces the ring full and the indices to wrap.
// Each memory order is a macro so run.sh can weaken them one at a time and
// confirm GenMC catches every weakening (the model has teeth).

#include <assert.h>
#include <genmc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#ifndef PUSH_PUBLISH   // try_push: prod_.write.store(w + 1, release)
#define PUSH_PUBLISH memory_order_release
#endif
#ifndef PUSH_SEE_READ  // try_push: cons_.read.load(acquire) when the ring looks full
#define PUSH_SEE_READ memory_order_acquire
#endif
#ifndef POP_SEE_WRITE  // try_pop: prod_.write.load(acquire) when the ring looks empty
#define POP_SEE_WRITE memory_order_acquire
#endif
#ifndef POP_FREE       // try_pop: cons_.read.store(r + 1, release)
#define POP_FREE memory_order_release
#endif

#define CAPACITY 2u
#define MASK (CAPACITY - 1u)
#define MESSAGES 4u
#define TRIES 3

typedef struct {
  uint32_t a, b;  // both = the message number: a torn copy is detectable
} slot_t;

static atomic_uint_fast64_t prod_write;  // ProducerState::write
static uint64_t prod_cached_read;        // ProducerState::cached_read (producer only)
static atomic_uint_fast64_t cons_read;   // ConsumerState::read
static uint64_t cons_cached_write;       // ConsumerState::cached_write (consumer only)
static slot_t slots[CAPACITY];

static int try_push(uint32_t v) {
  const uint64_t w = atomic_load_explicit(&prod_write, memory_order_relaxed);
  if (w - prod_cached_read == CAPACITY) {
    prod_cached_read = atomic_load_explicit(&cons_read, PUSH_SEE_READ);
    if (w - prod_cached_read == CAPACITY) return 0;
  }
  slots[w & MASK].a = v;
  slots[w & MASK].b = v;
  atomic_store_explicit(&prod_write, w + 1, PUSH_PUBLISH);
  return 1;
}

static int try_pop(uint32_t* out) {
  const uint64_t r = atomic_load_explicit(&cons_read, memory_order_relaxed);
  if (r == cons_cached_write) {
    cons_cached_write = atomic_load_explicit(&prod_write, POP_SEE_WRITE);
    if (r == cons_cached_write) return 0;
  }
  const uint32_t a = slots[r & MASK].a;
  const uint32_t b = slots[r & MASK].b;
  assert(a == b);  // intact
  *out = a;
  atomic_store_explicit(&cons_read, r + 1, POP_FREE);
  return 1;
}

// Batch forms: one acquire / one release covers several slots.
static uint32_t try_push_n(const uint32_t* v, uint32_t n_in) {
  const uint64_t w = atomic_load_explicit(&prod_write, memory_order_relaxed);
  uint64_t free_slots = CAPACITY - (w - prod_cached_read);
  if (free_slots < n_in) {
    prod_cached_read = atomic_load_explicit(&cons_read, PUSH_SEE_READ);
    free_slots = CAPACITY - (w - prod_cached_read);
  }
  const uint32_t n = n_in < free_slots ? n_in : (uint32_t)free_slots;
  for (uint32_t i = 0; i < n; ++i) {
    slots[(w + i) & MASK].a = v[i];
    slots[(w + i) & MASK].b = v[i];
  }
  if (n != 0) atomic_store_explicit(&prod_write, w + n, PUSH_PUBLISH);
  return n;
}

static uint32_t try_pop_n(uint32_t* out, uint32_t n_in) {
  const uint64_t r = atomic_load_explicit(&cons_read, memory_order_relaxed);
  uint64_t avail = cons_cached_write - r;
  if (avail < n_in) {
    cons_cached_write = atomic_load_explicit(&prod_write, POP_SEE_WRITE);
    avail = cons_cached_write - r;
  }
  const uint32_t n = n_in < avail ? n_in : (uint32_t)avail;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t a = slots[(r + i) & MASK].a;
    const uint32_t b = slots[(r + i) & MASK].b;
    assert(a == b);
    out[i] = a;
  }
  if (n != 0) atomic_store_explicit(&cons_read, r + n, POP_FREE);
  return n;
}

// A failed attempt changes no shared state, so a few attempts per message
// cover every distinct behaviour; executions needing more are pruned.
static void* producer(void* arg) {
  (void)arg;
#ifdef BATCH
  uint32_t next = 1;
  while (next <= MESSAGES) {
    const uint32_t v[2] = {next, next + 1};
    uint32_t pushed = 0;
    for (int t = 0; t < TRIES && pushed == 0; ++t) pushed = try_push_n(v, next < MESSAGES ? 2u : 1u);
    __VERIFIER_assume(pushed != 0);
    next += pushed;
  }
#else
  for (uint32_t v = 1; v <= MESSAGES; ++v) {
    int ok = 0;
    for (int t = 0; t < TRIES && !ok; ++t) ok = try_push(v);
    __VERIFIER_assume(ok);
  }
#endif
  return NULL;
}

static void* consumer(void* arg) {
  (void)arg;
  uint32_t expect = 1;
  while (expect <= MESSAGES) {
#ifdef BATCH
    uint32_t got[2];
    uint32_t n = 0;
    for (int t = 0; t < TRIES && n == 0; ++t) n = try_pop_n(got, 2);
    __VERIFIER_assume(n != 0);
    for (uint32_t i = 0; i < n; ++i) assert(got[i] == expect++);  // exactly once, in order
#else
    uint32_t v = 0;
    int ok = 0;
    for (int t = 0; t < TRIES && !ok; ++t) ok = try_pop(&v);
    __VERIFIER_assume(ok);
    assert(v == expect);
    ++expect;
#endif
  }
  return NULL;
}

int main(void) {
  pthread_t p, c;
  pthread_create(&p, NULL, producer, NULL);
  pthread_create(&c, NULL, consumer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  return 0;
}
