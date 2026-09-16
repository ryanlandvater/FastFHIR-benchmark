// bench/provenance.hpp — build provenance for every published number (TASKS.md IN-0)
//
// A number without provenance is not evidence. Five things silently change what
// this harness measures and none of them are visible in the result rows:
//
//   1. `.external/FastFHIR` is a SYMLINK to a live working tree, not a pinned
//      checkout, so the library under test is whatever that tree is right now.
//   2. Bazel does not run FastFHIR's generator -- `../FastFHIR/BUILD.bazel`
//      globs `generated_src/*.cpp`, which CMake produces at configure time. The
//      benchmarked profile is therefore whatever CMake last generated, and
//      there is no Bazel-visible signal when it changes.
//   3. The same code runs ~10x faster at -O2 than at -O0. FastFHIR's own CMake
//      presets are all Debug; that is how upstream's tables came to be labelled
//      -O2 while being Debug measurements.
//   4. Bundle composition is seeded; two seeds are two different workloads.
//   5. The corpus is a symlink into a build tree and can be regenerated.
//
// So: collect() gathers all of it, missing_fields() says what could not be
// established, and write_json() REFUSES to emit unless the record is complete
// and the build is optimized. See handoff.md "provenance.json -- non-negotiable
// fields".

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace bench::provenance {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
// Self-contained on purpose. BoringSSL is in the graph via FastFHIR's own
// targets but is not visible to this module, and a benchmark whose point is
// hermetic, identically-built comparison arms should not grow a crypto
// dependency to hash a directory. FIPS 180-4; self_test() below checks it
// against the standard vectors before any digest is trusted.
namespace sha256 {

struct Ctx {
  std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::uint64_t total_bits = 0;
  std::array<unsigned char, 64> buf{};
  std::size_t buf_len = 0;
};

inline constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void compress(Ctx& c, const unsigned char* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
  std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], hh = c.h[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    const std::uint32_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
  c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += hh;
}

inline void update(Ctx& c, const void* data, std::size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  c.total_bits += static_cast<std::uint64_t>(len) * 8;
  while (len > 0) {
    const std::size_t take = std::min(len, c.buf.size() - c.buf_len);
    std::copy(p, p + take, c.buf.begin() + static_cast<std::ptrdiff_t>(c.buf_len));
    c.buf_len += take;
    p += take;
    len -= take;
    if (c.buf_len == c.buf.size()) {
      compress(c, c.buf.data());
      c.buf_len = 0;
    }
  }
}

inline void finish(Ctx& c, unsigned char out[32]) {
  const std::uint64_t bits = c.total_bits;
  const unsigned char pad = 0x80;
  update(c, &pad, 1);
  c.total_bits = bits;  // padding is not message content
  const unsigned char zero = 0;
  while (c.buf_len != 56) {
    update(c, &zero, 1);
    c.total_bits = bits;
  }
  unsigned char len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = static_cast<unsigned char>((bits >> (56 - 8 * i)) & 0xFF);
  std::copy(std::begin(len_be), std::end(len_be), c.buf.begin() + 56);
  compress(c, c.buf.data());
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<unsigned char>((c.h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<unsigned char>((c.h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<unsigned char>((c.h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<unsigned char>(c.h[i] & 0xFF);
  }
}

}  // namespace sha256

inline constexpr std::size_t kSha256Len = 32;

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

struct Provenance {
  // --- upstream identity (what library was actually measured)
  std::string fastfhir_path;
  std::string fastfhir_path_source;  // "bazel external repo" | "workspace symlink"
  std::string fastfhir_sha;
  std::string fastfhir_tag;
  bool fastfhir_dirty = false;

  // --- the compiled profile: decides which resources are typed vs opaque
  std::string production_profile;
  std::string production_profile_source;  // "operator" | "cmake-cache"
  bool production_profile_ambiguous = false;
  std::vector<std::string> production_profile_candidates;
  // Corroboration. The profile string is a claim; these are evidence, and they
  // come from the generated tree that is actually compiled in.
  int codesystem_enums = -1;
  int generated_cpp = -1;
  std::vector<std::string> generated_resources;

  // --- build
  std::string compilation_mode;  // "opt" | "optimized-no-ndebug" | "unoptimized"
  std::string compiler;
  std::string compiler_version;
  std::string os;
  std::string arch;
  std::string cpu_model;

  // --- host disclosure (TASKS.md PB-1d). Recorded, never gated: macOS has no
  // /sys, and a laptop run is still a valid development record. Empty or -1
  // means "not observable here".
  std::string kernel;
  int logical_cpus = -1;
  std::int64_t memory_bytes = -1;
  std::string cpu_microcode;
  std::string cpu_governor;
  std::string smt_control;
  std::string turbo;  // "on" | "off"
  // Set by the workflow, which knows what the harness cannot see from inside
  // the guest: BENCH_HOST_INSTANCE_TYPE, BENCH_HOST_IMAGE, BENCH_HOST_RUNNER.
  std::string host_instance_type;
  std::string host_image;
  std::string host_runner;

  // --- corpus
  std::string corpus_id;
  std::string corpus_sha256;
  int corpus_doc_count = -1;
  std::int64_t corpus_bytes = -1;

  // --- this repo and this run
  std::string benchmark_path;
  std::string benchmark_sha;
  bool benchmark_dirty = false;
  std::uint64_t seed = 0;
  std::string generated_at;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// Run a command and return its trimmed stdout. Empty on any failure -- every
// caller treats "" as "could not establish", which is what the gate checks.
inline std::string capture(const std::string& cmd) {
  std::string out;
  FILE* pipe = ::popen((cmd + " 2>/dev/null").c_str(), "r");
  if (!pipe) return {};
  std::array<char, 512> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  const int rc = ::pclose(pipe);
  if (rc != 0) return {};
  return trim(std::move(out));
}

inline std::string hex(const unsigned char* d, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(kHex[d[i] >> 4]);
    s.push_back(kHex[d[i] & 0x0F]);
  }
  return s;
}

inline std::string json_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::ostringstream esc;
          esc << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c));
          o += esc.str();
        } else {
          o.push_back(c);
        }
    }
  }
  return o;
}

// FIPS 180-4 vectors. A silently wrong digest would make every corpus look
// identical across regenerations, which is the one thing corpus_sha256 exists
// to prevent -- so the digest is checked before it is ever trusted.
inline bool sha256_self_test() {
  const auto digest = [](std::string_view in) {
    sha256::Ctx c{};
    sha256::update(c, in.data(), in.size());
    unsigned char md[kSha256Len];
    sha256::finish(c, md);
    return hex(md, sizeof(md));
  };
  if (digest("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return false;
  if (digest("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return false;
  // 448 bits: exercises the two-block padding path, where an off-by-one in
  // finish() would otherwise hide.
  if (digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    return false;
  // Multi-buffer: 1,000,000 'a' in chunks, so the streaming path is covered too.
  sha256::Ctx c{};
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) sha256::update(c, chunk.data(), chunk.size());
  unsigned char md[kSha256Len];
  sha256::finish(c, md);
  return hex(md, sizeof(md)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
}

// ---------------------------------------------------------------------------
// Repository roots
// ---------------------------------------------------------------------------

// Walk up from cwd for the workspace marker. The harness is normally run as
// ./bazel-bin/bench/bench_harness from the workspace root, but `bazel run`
// starts in the runfiles tree, so do not assume cwd is the root.
inline fs::path find_benchmark_root() {
  std::error_code ec;
  fs::path p = fs::current_path(ec);
  if (ec) return {};
  for (int up = 0; up < 8; ++up) {
    if (fs::exists(p / "MODULE.bazel", ec) && fs::exists(p / "bench", ec)) return p;
    if (!p.has_parent_path() || p.parent_path() == p) break;
    p = p.parent_path();
  }
  return {};
}

// The record must name the tree that was COMPILED, so canonical() is the point,
// not a convenience.
//
// Bazel's own link is asked first: bazel-<workspace>/external/fastfhir~ is what
// the last build resolved the module to, whether through MODULE.bazel's
// local_path_override (the .external/FastFHIR symlink) or through
// --override_module=fastfhir=<pinned checkout> (TASKS.md PB-1). Reading the
// symlink alone would name the live tree for a pinned build. `~` is Bazel 7's
// canonical-name separator; Bazel 8 uses `+`.
struct FastfhirRoot {
  fs::path path;
  std::string source;
};

inline FastfhirRoot find_fastfhir_root(const fs::path& bench_root) {
  std::error_code ec;
  if (bench_root.empty()) return {};
  const fs::path bazel_external =
      bench_root / ("bazel-" + bench_root.filename().string()) / "external";
  for (const char* name : {"fastfhir~", "fastfhir+"}) {
    const fs::path candidate = bazel_external / name;
    if (fs::exists(candidate / "BUILD.bazel", ec)) {
      const fs::path real = fs::canonical(candidate, ec);
      if (!ec) return {real, "bazel external repo"};
    }
  }
  for (const fs::path& candidate : {bench_root / ".external" / "FastFHIR",
                                    bench_root.parent_path() / "FastFHIR"}) {
    if (fs::exists(candidate, ec)) {
      const fs::path real = fs::canonical(candidate, ec);
      if (!ec) return {real, "workspace symlink"};
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Corpus digest
// ---------------------------------------------------------------------------

struct CorpusFacts {
  std::string sha256;
  int doc_count = -1;
  std::int64_t bytes = -1;
  std::int64_t newest_mtime = 0;
};

// SHA-256 over a manifest of "<relative name> <size> <content hash>\n" lines,
// sorted by name. Hashing content (not just sizes) is the point: a corpus can
// be regenerated with the same file names and sizes but different data.
inline CorpusFacts hash_corpus(const fs::path& corpus_dir) {
  CorpusFacts facts;
  std::error_code ec;
  if (corpus_dir.empty() || !fs::exists(corpus_dir, ec)) return facts;

  static const bool sha_ok = sha256_self_test();
  if (!sha_ok) {
    std::fprintf(stderr, "[provenance] SHA-256 self-test FAILED -- refusing to emit a corpus digest\n");
    return facts;
  }

  std::vector<fs::path> files;
  // follow_directory_symlink: datasets/synthea is itself a symlink into a build
  // tree, and so may be its contents.
  for (fs::directory_iterator it(corpus_dir, fs::directory_options::follow_directory_symlink, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (it->path().extension() == ".json") files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) return facts;

  sha256::Ctx manifest{};
  std::int64_t total = 0;
  std::int64_t newest = 0;
  std::vector<char> buf(1 << 20);

  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return {};  // an unreadable corpus file means no digest, not a partial one

    sha256::Ctx file_ctx{};
    std::int64_t size = 0;
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
      const auto got = static_cast<std::size_t>(in.gcount());
      sha256::update(file_ctx, buf.data(), got);
      size += static_cast<std::int64_t>(got);
    }
    unsigned char file_md[kSha256Len];
    sha256::finish(file_ctx, file_md);

    const std::string line =
        f.filename().string() + " " + std::to_string(size) + " " + hex(file_md, sizeof(file_md)) + "\n";
    sha256::update(manifest, line.data(), line.size());
    total += size;

    const auto wt = fs::last_write_time(f, ec);
    if (!ec) {
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(wt.time_since_epoch()).count();
      newest = std::max<std::int64_t>(newest, static_cast<std::int64_t>(secs));
    }
  }

  unsigned char md[kSha256Len];
  sha256::finish(manifest, md);
  facts.sha256 = hex(md, sizeof(md));
  facts.doc_count = static_cast<int>(files.size());
  facts.bytes = total;
  facts.newest_mtime = newest;
  return facts;
}

// The corpus is 1.3 GB, so hashing it on every run is a few seconds of pure
// waste once it is known to be unchanged. Memoize on (count, bytes, newest
// mtime) -- any of the three moving forces a rehash.
inline CorpusFacts corpus_facts_cached(const fs::path& corpus_dir, const fs::path& cache_file) {
  std::error_code ec;
  std::vector<fs::path> files;
  std::int64_t total = 0;
  std::int64_t newest = 0;
  for (fs::directory_iterator it(corpus_dir, fs::directory_options::follow_directory_symlink, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    files.push_back(it->path());
    const auto sz = fs::file_size(it->path(), ec);
    if (!ec) total += static_cast<std::int64_t>(sz);
    const auto wt = fs::last_write_time(it->path(), ec);
    if (!ec) {
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(wt.time_since_epoch()).count();
      newest = std::max<std::int64_t>(newest, static_cast<std::int64_t>(secs));
    }
  }
  if (files.empty()) return {};

  const int count = static_cast<int>(files.size());

  if (std::ifstream in{cache_file}) {
    int c_count = 0;
    std::int64_t c_bytes = 0, c_mtime = 0;
    std::string c_sha;
    if (in >> c_count >> c_bytes >> c_mtime >> c_sha && c_count == count && c_bytes == total &&
        c_mtime == newest && c_sha.size() == 64) {
      return CorpusFacts{c_sha, c_count, c_bytes, c_mtime};
    }
  }

  std::fprintf(stderr, "[provenance] hashing corpus (%d files, %.1f MB) -- cached after this run\n",
               count, static_cast<double>(total) / (1024.0 * 1024.0));
  CorpusFacts facts = hash_corpus(corpus_dir);
  if (!facts.sha256.empty()) {
    if (std::ofstream out{cache_file}) {
      out << facts.doc_count << " " << facts.bytes << " " << facts.newest_mtime << " " << facts.sha256
          << "\n";
    }
  }
  return facts;
}

// ---------------------------------------------------------------------------
// The compiled profile
// ---------------------------------------------------------------------------

// The profile is read from generated_src/.profile, the stamp CMake writes with
// the profile that produced the tree.
//
// That stamp is the right source because it describes the tree that was
// actually COMPILED. Bazel globs generated_src/ straight out of the source tree
// and never runs the generator, so the benchmark links whatever CMake last
// wrote there -- which is what the stamp records. CMake also refuses to
// regenerate the tree under a different profile, so the stamp cannot drift from
// its contents.
//
// CMakeCache.txt files are the fallback and are scanned regardless, for
// corroboration. They record what a build DIRECTORY asked for, not what the
// shared tree holds, and several may disagree: a stale build dir left at an
// older profile is common and says nothing about the code under test. Cache
// disagreement is therefore recorded as evidence, and only makes the profile
// ambiguous when there is no stamp to settle it.
struct ProfileScan {
  std::string chosen;
  std::string source;
  bool ambiguous = false;
  std::vector<std::string> candidates;  // "<build dir>=<value>"
};

inline ProfileScan scan_profile(const fs::path& fastfhir_root) {
  ProfileScan scan;
  std::error_code ec;
  if (fastfhir_root.empty()) return scan;

  struct Hit {
    std::int64_t mtime;
    std::string dir;
    std::string value;
  };
  std::vector<Hit> hits;

  for (fs::directory_iterator it(fastfhir_root, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const fs::path cache = it->path() / "CMakeCache.txt";
    if (!fs::exists(cache, ec)) continue;
    std::ifstream in(cache);
    std::string line;
    while (std::getline(in, line)) {
      constexpr std::string_view kKey = "FASTFHIR_PRODUCTION_PROFILE:STRING=";
      if (line.rfind(kKey, 0) == 0) {
        const auto wt = fs::last_write_time(cache, ec);
        const auto secs =
            ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(wt.time_since_epoch()).count();
        hits.push_back({static_cast<std::int64_t>(secs), it->path().filename().string(),
                        trim(line.substr(kKey.size()))});
        break;
      }
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.mtime > b.mtime; });
  std::set<std::string> distinct;
  for (const auto& h : hits) {
    distinct.insert(h.value);
    scan.candidates.push_back(h.dir + "=" + h.value);
  }

  // The stamp decides when it exists.
  const fs::path stamp = fastfhir_root / "generated_src" / ".profile";
  if (fs::exists(stamp, ec)) {
    std::ifstream in(stamp);
    std::string value;
    std::getline(in, value);
    value = trim(value);
    if (!value.empty()) {
      scan.chosen = value;
      scan.source = "generated-tree stamp";
      scan.ambiguous = false;
      return scan;
    }
  }

  if (hits.empty()) return scan;
  scan.chosen = hits.front().value;  // newest configure wins
  scan.source = "cmake-cache";
  scan.ambiguous = distinct.size() > 1;
  return scan;
}

// Evidence for the profile claim: what the generated tree actually contains.
inline void collect_generated_evidence(const fs::path& fastfhir_root, Provenance& p) {
  std::error_code ec;
  const fs::path gen = fastfhir_root / "generated_src";
  if (fastfhir_root.empty() || !fs::exists(gen, ec)) return;

  int cpp = 0;
  for (fs::directory_iterator it(gen, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto& path = it->path();
    if (path.extension() == ".cpp") {
      ++cpp;
      // FF_Patient.cpp -> Patient; skip the shared non-resource units.
      std::string stem = path.stem().string();
      if (stem.rfind("FF_", 0) == 0) stem = stem.substr(3);
      // The generated tree also holds shared machinery and the interned
      // dictionaries (R4_Dictionary, UCUM_Dictionary, Dictionary_Strings, ...);
      // counting those as resource types overstates the profile's coverage,
      // which is the exact number this field exists to corroborate.
      static const std::set<std::string> kNotResources = {
          "DataTypes", "Reflection", "FieldKeys", "IngestMappings", "AllTypes", "CodeSystems", "Codes",
          "ChoiceBlock", "Conformance_Layer"};
      const bool is_dictionary = stem.find("Dictionary") != std::string::npos;
      if (!is_dictionary && !kNotResources.count(stem)) p.generated_resources.push_back(stem);
    }
  }
  p.generated_cpp = cpp;
  std::sort(p.generated_resources.begin(), p.generated_resources.end());

  const fs::path codesystems = gen / "FF_CodeSystems.hpp";
  if (std::ifstream in{codesystems}) {
    int enums = 0;
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("enum class", 0) == 0 || line.rfind("enum ", 0) == 0) ++enums;
    }
    p.codesystem_enums = enums;
  }
}

// ---------------------------------------------------------------------------
// Host and build
// ---------------------------------------------------------------------------

inline std::string detect_compilation_mode() {
  // Bazel exposes no macro for -c opt, but the effects are observable: opt is
  // -O2 + NDEBUG, fastbuild and dbg are -O0.
#if defined(__OPTIMIZE__) && defined(NDEBUG)
  return "opt";
#elif defined(__OPTIMIZE__)
  return "optimized-no-ndebug";
#else
  return "unoptimized";
#endif
}

inline std::string detect_cpu_model() {
#if defined(__APPLE__)
  char buf[256];
  std::size_t len = sizeof(buf);
  if (::sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return trim(buf);
  return {};
#elif defined(_WIN32)
  if (const char* id = std::getenv("PROCESSOR_IDENTIFIER")) return trim(id);
  return {};
#else
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(line.substr(0, colon));
    if (key == "model name" || key == "Model") return trim(line.substr(colon + 1));
  }
  return {};
#endif
}

inline std::string now_iso8601() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  ::gmtime_s(&tm, &t);
#else
  ::gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// First line of a small /proc or /sys file, trimmed; empty when absent.
inline std::string read_first_line(const fs::path& path) {
  std::ifstream in(path);
  std::string line;
  if (!in || !std::getline(in, line)) return {};
  return trim(line);
}

inline std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? trim(v) : std::string();
}

// What a reviewer needs to rent the same box and get the same number. None of
// it is gated: most of it does not exist on macOS.
inline void collect_host(Provenance& p) {
  p.kernel = capture("uname -r");
  const unsigned hc = std::thread::hardware_concurrency();
  if (hc > 0) p.logical_cpus = static_cast<int>(hc);

#if defined(__APPLE__)
  std::uint64_t mem = 0;
  std::size_t len = sizeof(mem);
  if (::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
    p.memory_bytes = static_cast<std::int64_t>(mem);
  }
#elif defined(__linux__)
  {
    std::ifstream in("/proc/meminfo");
    std::string key;
    std::int64_t kib = 0;
    while (in >> key >> kib) {
      if (key == "MemTotal:") {
        p.memory_bytes = kib * 1024;
        break;
      }
      in.ignore(64, '\n');
    }
  }
  {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
      const auto colon = line.find(':');
      if (colon != std::string::npos && trim(line.substr(0, colon)) == "microcode") {
        p.cpu_microcode = trim(line.substr(colon + 1));
        break;
      }
    }
  }
  p.cpu_governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  p.smt_control = read_first_line("/sys/devices/system/cpu/smt/control");
  // intel_pstate inverts the sense; the generic cpufreq knob does not.
  if (const std::string no_turbo = read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
      !no_turbo.empty()) {
    p.turbo = no_turbo == "1" ? "off" : "on";
  } else if (const std::string boost = read_first_line("/sys/devices/system/cpu/cpufreq/boost");
             !boost.empty()) {
    p.turbo = boost == "1" ? "on" : "off";
  }
#endif

  p.host_instance_type = env_or_empty("BENCH_HOST_INSTANCE_TYPE");
  p.host_image = env_or_empty("BENCH_HOST_IMAGE");
  p.host_runner = env_or_empty("BENCH_HOST_RUNNER");
}

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

struct Options {
  std::string profile_override;  // --profile
  fs::path corpus_dir;
  std::uint64_t seed = 0;
};

inline Provenance collect(const Options& opts) {
  Provenance p;
  p.generated_at = now_iso8601();
  p.seed = opts.seed;

  const fs::path bench_root = find_benchmark_root();
  const FastfhirRoot ff = find_fastfhir_root(bench_root);
  const fs::path& ff_root = ff.path;
  p.benchmark_path = bench_root.string();
  p.fastfhir_path = ff_root.string();
  p.fastfhir_path_source = ff.source;

  if (!bench_root.empty()) {
    const std::string q = "git -C '" + bench_root.string() + "' ";
    p.benchmark_sha = capture(q + "rev-parse HEAD");
    p.benchmark_dirty = !capture(q + "status --porcelain").empty();
  }
  if (!ff_root.empty()) {
    const std::string q = "git -C '" + ff_root.string() + "' ";
    p.fastfhir_sha = capture(q + "rev-parse HEAD");
    p.fastfhir_tag = capture(q + "describe --tags --always");
    p.fastfhir_dirty = !capture(q + "status --porcelain").empty();
  }

  if (!opts.profile_override.empty()) {
    p.production_profile = opts.profile_override;
    p.production_profile_source = "operator";
  } else {
    const ProfileScan scan = scan_profile(ff_root);
    p.production_profile = scan.chosen;
    p.production_profile_source = scan.source;
    p.production_profile_ambiguous = scan.ambiguous;
    p.production_profile_candidates = scan.candidates;
  }
  collect_generated_evidence(ff_root, p);

  p.compilation_mode = detect_compilation_mode();
#if defined(__clang__)
  p.compiler = "clang";
  p.compiler_version = std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
                       std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  p.compiler = "gcc";
  p.compiler_version = std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
                       std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  p.compiler = "msvc";
  p.compiler_version = std::to_string(_MSC_VER);
#endif

#if defined(__APPLE__)
  p.os = "macos";
#elif defined(_WIN32)
  p.os = "windows";
#elif defined(__linux__)
  p.os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  p.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  p.arch = "x86_64";
#endif
  p.cpu_model = detect_cpu_model();
  collect_host(p);

  if (!opts.corpus_dir.empty()) {
    const fs::path cache = bench_root.empty() ? fs::path(".corpus_sha256.cache")
                                              : bench_root / "datasets" / ".corpus_sha256.cache";
    const CorpusFacts facts = corpus_facts_cached(opts.corpus_dir, cache);
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(opts.corpus_dir, ec);
    p.corpus_id = (ec ? opts.corpus_dir : real).string();
    p.corpus_sha256 = facts.sha256;
    p.corpus_doc_count = facts.doc_count;
    p.corpus_bytes = facts.bytes;
  }

  return p;
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

// Every field here silently changes the result. An artifact missing any one of
// them is not reproducible, so it is not an artifact.
inline std::vector<std::string> missing_fields(const Provenance& p) {
  std::vector<std::string> missing;
  const auto need = [&](const char* name, bool ok) {
    if (!ok) missing.emplace_back(name);
  };
  need("fastfhir_sha", !p.fastfhir_sha.empty());
  need("fastfhir_tag", !p.fastfhir_tag.empty());
  need("production_profile", !p.production_profile.empty());
  need("codesystem_enums", p.codesystem_enums > 0);
  need("generated_cpp", p.generated_cpp > 0);
  need("compilation_mode", !p.compilation_mode.empty());
  need("compiler", !p.compiler.empty());
  need("compiler_version", !p.compiler_version.empty());
  need("os", !p.os.empty());
  need("arch", !p.arch.empty());
  need("cpu_model", !p.cpu_model.empty());
  need("corpus_id", !p.corpus_id.empty());
  need("corpus_sha256", p.corpus_sha256.size() == 64);
  need("corpus_doc_count", p.corpus_doc_count > 0);
  need("benchmark_sha", !p.benchmark_sha.empty());
  need("seed", p.seed != 0);  // seed 0 means "random", which is not reproducible

  // An ambiguous profile is worse than a missing one: it looks established.
  if (p.production_profile_ambiguous) {
    missing.emplace_back(
        "production_profile (ambiguous -- several CMake caches disagree and "
        "generated_src/.profile is absent; configure FastFHIR once to write the "
        "stamp, or pin it with --profile)");
  }
  // The Debug trap. Same code, ~10x slower, and nothing in the numbers says so.
  if (p.compilation_mode != "opt") {
    missing.emplace_back("compilation_mode (is '" + p.compilation_mode +
                         "', must be 'opt' -- rebuild with -c opt)");
  }
  return missing;
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

inline std::string to_json(const Provenance& p) {
  std::ostringstream o;
  const auto str = [&](const char* k, const std::string& v, bool comma = true) {
    o << "  \"" << k << "\": \"" << json_escape(v) << "\"" << (comma ? ",\n" : "\n");
  };
  const auto num = [&](const char* k, std::int64_t v, bool comma = true) {
    o << "  \"" << k << "\": " << v << (comma ? ",\n" : "\n");
  };
  const auto boolean = [&](const char* k, bool v, bool comma = true) {
    o << "  \"" << k << "\": " << (v ? "true" : "false") << (comma ? ",\n" : "\n");
  };
  const auto arr = [&](const char* k, const std::vector<std::string>& v, bool comma = true) {
    o << "  \"" << k << "\": [";
    for (std::size_t i = 0; i < v.size(); ++i) {
      o << (i ? ", " : "") << "\"" << json_escape(v[i]) << "\"";
    }
    o << "]" << (comma ? ",\n" : "\n");
  };

  o << "{\n";
  str("generated_at", p.generated_at);
  str("fastfhir_path", p.fastfhir_path);
  str("fastfhir_path_source", p.fastfhir_path_source);
  str("fastfhir_sha", p.fastfhir_sha);
  str("fastfhir_tag", p.fastfhir_tag);
  boolean("fastfhir_dirty", p.fastfhir_dirty);
  str("production_profile", p.production_profile);
  str("production_profile_source", p.production_profile_source);
  boolean("production_profile_ambiguous", p.production_profile_ambiguous);
  arr("production_profile_candidates", p.production_profile_candidates);
  num("codesystem_enums", p.codesystem_enums);
  num("generated_cpp", p.generated_cpp);
  num("generated_resource_count", static_cast<std::int64_t>(p.generated_resources.size()));
  arr("generated_resources", p.generated_resources);
  str("compilation_mode", p.compilation_mode);
  str("compiler", p.compiler);
  str("compiler_version", p.compiler_version);
  str("os", p.os);
  str("arch", p.arch);
  str("cpu_model", p.cpu_model);
  str("kernel", p.kernel);
  num("logical_cpus", p.logical_cpus);
  num("memory_bytes", p.memory_bytes);
  str("cpu_microcode", p.cpu_microcode);
  str("cpu_governor", p.cpu_governor);
  str("smt_control", p.smt_control);
  str("turbo", p.turbo);
  str("host_instance_type", p.host_instance_type);
  str("host_image", p.host_image);
  str("host_runner", p.host_runner);
  str("corpus_id", p.corpus_id);
  str("corpus_sha256", p.corpus_sha256);
  num("corpus_doc_count", p.corpus_doc_count);
  num("corpus_bytes", p.corpus_bytes);
  str("benchmark_path", p.benchmark_path);
  str("benchmark_sha", p.benchmark_sha);
  boolean("benchmark_dirty", p.benchmark_dirty);
  num("seed", static_cast<std::int64_t>(p.seed), false);
  o << "}\n";
  return o.str();
}

// Human-readable one-screen summary for stderr on every run, artifact or not.
inline std::string to_summary(const Provenance& p) {
  std::ostringstream o;
  o << "[provenance] fastfhir " << (p.fastfhir_tag.empty() ? "?" : p.fastfhir_tag) << " @ "
    << (p.fastfhir_sha.empty() ? "?" : p.fastfhir_sha.substr(0, 12)) << (p.fastfhir_dirty ? " DIRTY" : "")
    << "\n"
    << "[provenance] profile " << (p.production_profile.empty() ? "?" : p.production_profile) << " ("
    << p.production_profile_source << (p.production_profile_ambiguous ? ", AMBIGUOUS" : "") << ") -- "
    << p.codesystem_enums << " code-system enums, " << p.generated_cpp << " generated .cpp, "
    << p.generated_resources.size() << " resource types\n"
    << "[provenance] build " << p.compilation_mode << " " << p.compiler << " " << p.compiler_version
    << " on " << p.os << "/" << p.arch << " (" << p.cpu_model << ")\n"
    << "[provenance] host " << (p.host_instance_type.empty() ? "local" : p.host_instance_type)
    << ", kernel " << (p.kernel.empty() ? "?" : p.kernel) << ", " << p.logical_cpus << " cpus"
    << (p.cpu_governor.empty() ? "" : ", governor " + p.cpu_governor)
    << (p.smt_control.empty() ? "" : ", smt " + p.smt_control)
    << (p.turbo.empty() ? "" : ", turbo " + p.turbo) << "\n"
    << "[provenance] fastfhir tree " << (p.fastfhir_path.empty() ? "?" : p.fastfhir_path) << " ("
    << (p.fastfhir_path_source.empty() ? "?" : p.fastfhir_path_source) << ")\n"
    << "[provenance] corpus " << p.corpus_doc_count << " docs, "
    << (p.corpus_bytes < 0 ? 0 : p.corpus_bytes / (1024 * 1024)) << " MB, sha256 "
    << (p.corpus_sha256.empty() ? std::string("?") : p.corpus_sha256.substr(0, 12)) << "\n"
    << "[provenance] benchmark @ "
    << (p.benchmark_sha.empty() ? "?" : p.benchmark_sha.substr(0, 12))
    << (p.benchmark_dirty ? " DIRTY" : "") << ", seed " << p.seed << "\n";
  return o.str();
}

// Returns true if written. Refuses -- loudly, and without writing anything --
// when the record is incomplete or the build is not optimized.
inline bool write_json(const Provenance& p, const fs::path& results_dir, std::ostream& err) {
  const std::vector<std::string> missing = missing_fields(p);
  if (!missing.empty()) {
    err << "\n[provenance] REFUSING to write an artifact: " << missing.size()
        << " required field(s) not established.\n";
    for (const auto& m : missing) err << "[provenance]   - " << m << "\n";
    err << "[provenance] A number without provenance is not evidence "
           "(handoff.md, TASKS.md IN-0). No artifact written.\n";
    return false;
  }
  std::error_code ec;
  fs::create_directories(results_dir, ec);
  const fs::path out_path = results_dir / "provenance.json";
  std::ofstream out(out_path);
  if (!out) {
    err << "[provenance] could not open " << out_path.string() << " for writing\n";
    return false;
  }
  out << to_json(p);
  err << "[provenance] wrote " << out_path.string() << "\n";
  return true;
}

}  // namespace bench::provenance
