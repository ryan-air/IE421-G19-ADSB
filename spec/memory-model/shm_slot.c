// GenMC model of the shared-memory ring's slot protocol (a seqlock per slot,
// core/include/adsb/feed/shm_ring.hpp: RingView::write and RingView::read),
// transcribed line for line. Default: the fence form every normal build
// compiles. -DTSAN_FORM: the fence-free form ThreadSanitizer builds compile
// (payload stores release, payload loads acquire).
//
// One writer publishes messages 1..MESSAGES into CAPACITY slots, so it laps
// the readers. Each reader follows the ring like ShmSubscriber/adsb-shm-bench:
// on kOk the next message, on kLapped jump to next_seq(). Checked in every
// execution the C11 (RC11) memory model allows:
//   * a message returned as kOk is intact: every payload word and the length
//     come from that message, never a mix with a later lap (torn read);
//   * messages are returned in increasing sequence order;
//   * after a lap the reader resumes with RingView::resume_after_lap, never
//     backwards. next_seq() alone can lag the lap just seen: this model found
//     that (the lap is seen through the writer's busy marker, which does not
//     synchronize with its next_seq store). -DCHECK_HEAD asserts the old
//     assumption, "next_seq() is past the lapped message", to show it.
// The payload is atomic in the C++ code, so there are no data races to find;
// a broken ordering shows up as a torn read, which the assertions catch.
//
// Each ordering (and the re-check) is a macro so run.sh can break them one
// at a time and confirm GenMC catches every one.

#include <assert.h>
#include <genmc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define relaxed memory_order_relaxed
#define acquire memory_order_acquire
#define release memory_order_release

#ifndef CAPACITY
#define CAPACITY 1u
#endif
#ifndef MESSAGES
#define MESSAGES 3u
#endif
#ifndef READERS
#define READERS 1
#endif
#define MASK (CAPACITY - 1u)
#define WORDS 2u
#define ATTEMPTS 4
#define BUSY (1ull << 63)

#ifndef SEQ_PUBLISH  // write: seq.store(s, release)
#define SEQ_PUBLISH release
#endif
#ifndef SEQ_SEE      // read: v1 = seq.load(acquire)
#define SEQ_SEE acquire
#endif
#ifndef WRITE_BUSY   // write: seq.store(kBusy | s, relaxed) before the payload
#define WRITE_BUSY 1
#endif
#ifndef RECHECK      // read: seq.load(relaxed) != s after the copy -> kLapped
#define RECHECK 1
#endif
#ifdef TSAN_FORM
#ifndef PAYLOAD_STORE
#define PAYLOAD_STORE release
#endif
#ifndef PAYLOAD_LOAD
#define PAYLOAD_LOAD acquire
#endif
#else
#define PAYLOAD_STORE relaxed
#define PAYLOAD_LOAD relaxed
#ifndef WRITER_FENCE  // write: atomic_thread_fence(release) after the busy marker
#define WRITER_FENCE 1
#endif
#ifndef READER_FENCE  // read: atomic_thread_fence(acquire) before the re-check
#define READER_FENCE 1
#endif
#endif

typedef struct {
  atomic_uint_fast64_t seq;
  atomic_uint_fast64_t len;
  atomic_uint_fast64_t words[WORDS];
} slot_t;

static slot_t slots[CAPACITY];
static atomic_uint_fast64_t next_seq;  // header_->next_seq

enum { kOk, kEmpty, kLapped };

// Message s carries s in every word and 8 * s as its length.
static void ring_write(uint64_t s) {
  slot_t* sl = &slots[s & MASK];
  if (WRITE_BUSY) atomic_store_explicit(&sl->seq, BUSY | s, relaxed);
#ifndef TSAN_FORM
  if (WRITER_FENCE) atomic_thread_fence(release);
#endif
  atomic_store_explicit(&sl->len, 8 * s, PAYLOAD_STORE);
  for (unsigned i = 0; i < WORDS; ++i) atomic_store_explicit(&sl->words[i], s, PAYLOAD_STORE);
  atomic_store_explicit(&sl->seq, s, SEQ_PUBLISH);
  atomic_store_explicit(&next_seq, s + 1, release);
}

static int ring_read(uint64_t s, uint64_t* len, uint64_t* w) {
  slot_t* sl = &slots[s & MASK];
  const uint64_t v1 = atomic_load_explicit(&sl->seq, SEQ_SEE);
  if (v1 != s) {
    // C++: v1 == 0 || (v1 & ~kBusy) <= s ? kEmpty : kLapped. Written as
    // branches: the select clang makes of the original carries an LLVM
    // `freeze` that GenMC does not interpret.
    if (v1 == 0) return kEmpty;
    if ((v1 & ~BUSY) <= s) return kEmpty;
    return kLapped;
  }
  const uint64_t n = atomic_load_explicit(&sl->len, PAYLOAD_LOAD);
  for (unsigned i = 0; i < WORDS; ++i) w[i] = atomic_load_explicit(&sl->words[i], PAYLOAD_LOAD);
#ifndef TSAN_FORM
  if (READER_FENCE) atomic_thread_fence(acquire);
#endif
  if (RECHECK && atomic_load_explicit(&sl->seq, relaxed) != s) return kLapped;
  *len = n;
  return kOk;
}

static void* writer(void* arg) {
  (void)arg;
  for (uint64_t s = 1; s <= MESSAGES; ++s) ring_write(s);
  return NULL;
}

static void* reader(void* arg) {
  (void)arg;
  uint64_t expect = 1, last = 0;
  for (int t = 0; t < ATTEMPTS && expect <= MESSAGES; ++t) {
    uint64_t len = 0, w[WORDS];
    const int r = ring_read(expect, &len, w);
    if (r == kOk) {
      assert(len == 8 * expect);                                // intact
      for (unsigned i = 0; i < WORDS; ++i) assert(w[i] == expect);
      assert(expect > last);                                    // in order
      last = expect++;
    } else if (r == kLapped) {
      // RingView::resume_after_lap
      const uint64_t head = atomic_load_explicit(&next_seq, acquire);
#ifdef CHECK_HEAD
      assert(head > expect);
#endif
      expect = head > expect ? head : expect + 1;
    }
  }
  return NULL;
}

int main(void) {
  pthread_t w, r[READERS];
  pthread_create(&w, NULL, writer, NULL);
  for (int i = 0; i < READERS; ++i) pthread_create(&r[i], NULL, reader, NULL);
  pthread_join(w, NULL);
  for (int i = 0; i < READERS; ++i) pthread_join(r[i], NULL);
  return 0;
}
