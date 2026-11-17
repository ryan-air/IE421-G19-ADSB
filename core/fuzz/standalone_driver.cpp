// Portable replacement for libFuzzer's main(), for toolchains without
// -fsanitize=fuzzer (e.g. Apple clang). It is not coverage-guided; it
//   1. runs every file in the given corpus files/directories, then
//   2. runs N seeded random inputs: fresh random bytes and mutations
//      (bit flips, byte sets, splices, truncations) of corpus entries.
// Combined with ASan/UBSan and the targets' invariant checks this is a useful
// smoke test; use the libFuzzer build (`fuzz` preset) for real fuzzing.
//
// Usage: fuzz_xxx [-runs=N] [-seed=S] [-max_len=L] [corpus_dir_or_file ...]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

using Input = std::vector<std::uint8_t>;

void run_one(const Input& in) {
  // Copy into an exactly-sized heap buffer so ASan catches any overread.
  auto* buf = static_cast<std::uint8_t*>(std::malloc(in.empty() ? 1 : in.size()));
  if (!in.empty()) std::memcpy(buf, in.data(), in.size());
  LLVMFuzzerTestOneInput(buf, in.size());
  std::free(buf);
}

void load(const std::filesystem::path& p, std::vector<Input>& corpus) {
  namespace fs = std::filesystem;
  if (fs::is_directory(p)) {
    for (const auto& e : fs::directory_iterator(p)) {
      if (e.is_regular_file()) load(e.path(), corpus);
    }
    return;
  }
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", p.c_str());
    std::exit(2);
  }
  corpus.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

Input mutate(const Input& base, std::mt19937_64& rng, std::size_t max_len,
             const std::vector<Input>& corpus) {
  Input out = base;
  const int ops = 1 + static_cast<int>(rng() % 4);
  for (int i = 0; i < ops; ++i) {
    switch (rng() % 6) {
      case 0:  // flip a bit
        if (!out.empty()) out[rng() % out.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        break;
      case 1:  // set a byte (biased toward interesting values)
        if (!out.empty()) {
          static constexpr std::uint8_t kInteresting[] = {0x00, 0x1A, 0xFF, 0x7F, 0x80, 0x31, 0x32, 0x33, 0x34, 0x8D};
          out[rng() % out.size()] = (rng() % 2) ? kInteresting[rng() % sizeof(kInteresting)]
                                                : static_cast<std::uint8_t>(rng());
        }
        break;
      case 2:  // insert a byte
        if (out.size() < max_len) {
          out.insert(out.begin() + static_cast<std::ptrdiff_t>(rng() % (out.size() + 1)),
                     static_cast<std::uint8_t>(rng()));
        }
        break;
      case 3:  // erase a byte
        if (!out.empty()) out.erase(out.begin() + static_cast<std::ptrdiff_t>(rng() % out.size()));
        break;
      case 4:  // truncate
        if (!out.empty()) out.resize(rng() % out.size());
        break;
      case 5:  // splice in part of another corpus entry
        if (!corpus.empty()) {
          const Input& other = corpus[rng() % corpus.size()];
          if (!other.empty()) {
            const std::size_t from = rng() % other.size();
            const std::size_t n = 1 + rng() % (other.size() - from);
            out.insert(out.end(), other.begin() + static_cast<std::ptrdiff_t>(from),
                       other.begin() + static_cast<std::ptrdiff_t>(from + n));
          }
        }
        break;
    }
  }
  if (out.size() > max_len) out.resize(max_len);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t runs = 100'000;
  std::uint64_t seed = 1;
  std::size_t max_len = 4096;
  std::vector<Input> corpus;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("-runs=", 0) == 0) {
      runs = std::strtoull(a.c_str() + 6, nullptr, 10);
    } else if (a.rfind("-seed=", 0) == 0) {
      seed = std::strtoull(a.c_str() + 6, nullptr, 10);
    } else if (a.rfind("-max_len=", 0) == 0) {
      max_len = std::strtoull(a.c_str() + 9, nullptr, 10);
    } else if (!a.empty() && a[0] == '-') {
      // Ignore other libFuzzer-style flags so scripts work with both engines.
    } else {
      load(a, corpus);
    }
  }

  for (const Input& in : corpus) run_one(in);
  std::printf("standalone fuzz: replayed %zu corpus inputs\n", corpus.size());

  std::mt19937_64 rng(seed);
  for (std::uint64_t r = 0; r < runs; ++r) {
    Input in;
    if (corpus.empty() || rng() % 4 == 0) {
      in.resize(rng() % (max_len < 64 ? max_len + 1 : 65));
      for (auto& b : in) b = static_cast<std::uint8_t>(rng());
    } else {
      in = mutate(corpus[rng() % corpus.size()], rng, max_len, corpus);
    }
    run_one(in);
  }
  std::printf("standalone fuzz: %llu random/mutated runs OK (seed=%llu)\n",
              static_cast<unsigned long long>(runs), static_cast<unsigned long long>(seed));
  return 0;
}
