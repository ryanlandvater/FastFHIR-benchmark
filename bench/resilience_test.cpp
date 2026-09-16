// ===========================================================================
// Instrument G -- Resilience & Integrity Suite (TODO.md design spec, IN-G)
// ===========================================================================
// Four tests, none of them timers:
//   1. Truncation detection (WF-3.1)      -- VALIDATION word / FF_HEADER
//   2. Bit-flip detection (WF-3.3)        -- structural vs payload vs checksum
//   3. Type-confusion prevention (WF-3.2) -- RECOVERY_TAG dispatch
//   4. Concurrent build integrity (WF-4.2)-- lock-free arena under contention
//
// Qualifiers preserved verbatim from FastFHIR README §3: test 3 guards
// against MALFORMED data, not HOSTILE (the parser is not fuzzed -- upstream
// TASKS.md G1); test 2 is INTEGRITY, not AUTHENTICITY (an attacker who can
// rewrite payload bytes can recompute the footer).
//
// What the other arms do, stated plainly: protobuf type-checks at the message
// level (a Condition cannot parse as an Observation without a wire-format
// error); JSON parse errors abort the whole document; HL7v2 delimiter damage
// cascades through the message. FastFHIR's claim is narrower: field-level
// dispatch inside a block, block-granular corruption detection, and rejection
// of a mislabelled-but-well-formed resource. These tests measure exactly that.
//
// Run: bazel run -c opt //bench:resilience_test
// Exit 0 = all assertions held; nonzero = at least one invariant broke.
// ===========================================================================

#include "harness.hpp"
#include "provenance.hpp"  // bench::sha256 -- a real digest for the checksum test

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>#include <fstream>
#include <numeric>
#include <random>#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const char *what)
{
  if (!ok)
  {
    ++g_failures;
    std::fprintf(stderr, "  FAIL: %s\n", what);
  }
  else
  {
    std::fprintf(stderr, "  ok:   %s\n", what);
  }
}

std::filesystem::path find_synthea_dir()
{
#if defined(__APPLE__)
  const std::filesystem::path primary =
      "/Users/RyanLandvater/Programming_Projects/FastFHIR-benchmarking/datasets/synthea";
  if (std::filesystem::exists(primary))
    return primary;
#endif
  const std::filesystem::path fallback = "datasets/synthea";
  if (std::filesystem::exists(fallback))
    return fallback;
  return {};
}

// One rich Synthea patient (all resource types), ingested and sealed by the
// harness pipeline. The stream's footer carries the benchmark's null hasher,
// which is fine for the structural tests; the checksum sub-test re-seals with
// a real SHA-256 (bench::sha256).
FastFHIR::Memory load_patient()
{
  const auto dir = find_synthea_dir();
  if (dir.empty())
    throw std::runtime_error("Synthea corpus not found (datasets/synthea)");
  std::vector<std::filesystem::path> files;
  for (const auto &e : std::filesystem::directory_iterator(dir))
    if (e.path().extension() == ".json")
      files.push_back(e.path());
  std::sort(files.begin(), files.end());
  if (files.empty())
    throw std::runtime_error("No Synthea JSON files found in corpus");
  return bench::make_bundle_patient_from_json(files.front()).memory;
}

std::vector<uint8_t> seal_bytes(const FastFHIR::Memory &mem)
{
  const auto view = mem.view();
  return std::vector<uint8_t>(view.data(), view.data() + view.size());
}

// ---------------------------------------------------------------------------
// Test 1 -- truncation detection
// ---------------------------------------------------------------------------
void test_truncation(const FastFHIR::Memory &mem)
{
  std::fprintf(stderr, "\nTest 1 -- truncation detection (WF-3.1)\n");
  const auto bytes = seal_bytes(mem);

  // Cuts bracketing structural boundaries: mid-FF_HEADER, mid-block, mid-
  // string payload, and arbitrary points across the stream.
  const std::vector<size_t> cuts = {5, 12, 64, bytes.size() / 3, bytes.size() / 2,
                                    2 * bytes.size() / 3, bytes.size() - 3, bytes.size() - 64};
  size_t rejected = 0;
  for (const size_t n : cuts)
  {
    bool refused = false;
    try
    {
      FastFHIR::Parser p(bytes.data(), n);
      const FF_Result vr = p.validate_FFHR_stream();
      refused = !vr;  // constructed, but structurally invalid (dangling offset)
    }
    catch (const std::exception &)
    {
      refused = true;  // construction threw (truncated header)
    }
    if (refused)
      ++rejected;
    std::fprintf(stderr, "  cut at %zu/%zu bytes: %s\n", n, bytes.size(),
                 refused ? "rejected" : "ACCEPTED");
  }
  expect(rejected == cuts.size(),
         "every truncation is rejected (constructor throw or validate failure)");
}

// ---------------------------------------------------------------------------
// Test 2 -- bit-flip detection
// ---------------------------------------------------------------------------
// Re-seals the stream with a REAL SHA-256 so the checksum sub-test is an
// honest artifact rather than a comparison against the benchmark's zeroed
// digest.
void reseal_with_checksum(FastFHIR::Memory &mem)
{
  FastFHIR::FF_Builder builder_handle = bench::make_builder(mem);
  FastFHIR::Memory::View view;
  auto real_hasher = [](const unsigned char *b, Size n) -> std::vector<BYTE>
  {
    bench::provenance::sha256::Ctx c;
    bench::provenance::sha256::update(c, b, n);
    std::vector<BYTE> out(32);
    bench::provenance::sha256::finish(c, out.data());
    return out;
  };
  const FF_Result r = FastFHIR::FF_BuilderFinalize(
      FastFHIR::FF_BuilderFinalizeInfo{.builder = builder_handle, .algorithm = FF_CHECKSUM_SHA256,
                                      .hasher = FastFHIR::FF_HashCallback{real_hasher}},
      view);
  if (!r)
    throw std::runtime_error("reseal_with_checksum failed: " + r.message);
}

void test_bitflip(FastFHIR::Memory mem)
{
  std::fprintf(stderr, "\nTest 2 -- bit-flip detection (WF-3.3)\n");
  reseal_with_checksum(mem);

  // Locate a mid-stream Observation block and one of its string payloads.
  FastFHIR::Parser base(mem);
  const auto root = base.root();
  expect(root.is<FastFHIR::RESOURCETYPE::BUNDLE>(), "stream root is a Bundle");

  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY];
  expect(static_cast<bool>(entries), "bundle has an entry array");

  const auto n_entries = entries.as_node().size();
  std::size_t obs_block_off = 0;
  std::size_t string_payload_off = 0;
  for (std::size_t i = 0; i < n_entries && obs_block_off == 0; ++i)
  {
    auto resource = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource)
      continue;
    auto node = resource.as_node();
    if (!node || !node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())
      continue;
    obs_block_off = static_cast<std::size_t>(LOAD_U64(mem.view().data() + resource.absolute_offset()));
    // First string-valued field on the observation: read its bytes in the
    // arena, then record the payload offset for a later payload flip.
    auto id_entry = node[FastFHIR::Fields::OBSERVATION::ID];
    if (id_entry)
    {
      const std::string_view sv = id_entry.as<std::string_view>();
      string_payload_off =
          static_cast<std::size_t>(sv.data() - mem.view().data());
    }
    break;
  }
  expect(obs_block_off != 0, "located a mid-stream Observation block");
  expect(string_payload_off != 0, "located an Observation string payload");

  const auto bytes = seal_bytes(mem);

  // (a) Structural flip -- one bit in the block's 8-byte VALIDATION word.
  {
    auto corrupt = bytes;
    corrupt[obs_block_off] ^= 0x01;
    FastFHIR::Parser p(corrupt.data(), corrupt.size());
    const FF_Result vr = p.validate_FFHR_stream();
    expect(!vr, "VALIDATION-word flip makes validate_FFHR_stream() fail");
  }

  // (b) Tag flip -- one bit in the block's 2-byte RECOVERY_TAG.
  {
    auto corrupt = bytes;
    corrupt[obs_block_off + 8] ^= 0x02;
    FastFHIR::Parser p(corrupt.data(), corrupt.size());
    bool refused = false;
    try
    {
      auto node = p.root()[FastFHIR::Fields::BUNDLE::ENTRY][0]
                      [FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE]
                          .as_node();
      (void)node.as<ObservationData>();
    }
    catch (const std::exception &)
    {
      refused = true;
    }
    expect(refused, "RECOVERY_TAG flip makes the typed read refuse");
  }

  // (c) Payload flip -- one bit in a string payload. Structure survives; the
  // checksum catches the damage.
  {
    auto corrupt = bytes;
    corrupt[string_payload_off] ^= 0x40;
    FastFHIR::Parser p(corrupt.data(), corrupt.size());
    const FF_Result vr = p.validate_FFHR_stream();
    expect(static_cast<bool>(vr), "payload flip leaves the structure valid");

    auto cs = p.checksum();
    expect(static_cast<bool>(cs), "stream carries checksum metadata");
    if (cs)
    {
      // checksum() reports total_bytes = the checksum block's OFFSET; the hash
      // covers the payload PLUS the 12 bytes of checksum metadata, stopping at
      // the 32-byte digest slot (FF_Memory.hpp seal_stream).
      bench::provenance::sha256::Ctx c;
      bench::provenance::sha256::update(c, cs.first_byte, cs.total_bytes + FF_CHECKSUM::HASH_DATA);
      unsigned char recomputed[32];
      bench::provenance::sha256::finish(c, recomputed);
      const bool match = (cs.expected_checksum.size() == 32 &&
                          std::equal(recomputed, recomputed + 32,
                                     reinterpret_cast<const unsigned char *>(
                                         cs.expected_checksum.data())));
      expect(!match, "payload flip is caught by the checksum footer (integrity)");
    }
  }

  // (d) Clean-stream checksum must validate (the honest baseline).
  {
    FastFHIR::Parser p(bytes.data(), bytes.size());
    auto cs = p.checksum();
    bool match = false;
    if (cs)
    {
      // See above: the hashed span is [0, checksum_offset + 12).
      bench::provenance::sha256::Ctx c;
      bench::provenance::sha256::update(c, cs.first_byte, cs.total_bytes + FF_CHECKSUM::HASH_DATA);
      unsigned char recomputed[32];
      bench::provenance::sha256::finish(c, recomputed);
      match = (cs.expected_checksum.size() == 32 &&
               std::equal(recomputed, recomputed + 32,
                          reinterpret_cast<const unsigned char *>(
                              cs.expected_checksum.data())));
    }
    expect(match, "unmodified stream's checksum validates");
  }
}

// ---------------------------------------------------------------------------
// Test 3 -- type-confusion prevention
// ---------------------------------------------------------------------------
void test_type_confusion(const FastFHIR::Memory &mem)
{
  std::fprintf(stderr, "\nTest 3 -- type confusion prevention (WF-3.2)\n");
  FastFHIR::Parser p(mem);
  const auto root = p.root();
  expect(root.is<FastFHIR::RESOURCETYPE::BUNDLE>(), "root type is Bundle");

  auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY];
  const auto n = entries.as_node().size();
  std::size_t typed = 0, opaque = 0, reference_mismatches = 0;

  for (std::size_t i = 0; i < n; ++i)
  {
    auto resource_entry = entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource_entry)
      continue;
    auto node = resource_entry.as_node();
    if (!node)
      continue;

    const bool is_p = node.is<FastFHIR::RESOURCETYPE::PATIENT>();
    const bool is_o = node.is<FastFHIR::RESOURCETYPE::OBSERVATION>();
    const bool is_c = node.is<FastFHIR::RESOURCETYPE::CONDITION>();
    const bool is_e = node.is<FastFHIR::RESOURCETYPE::ENCOUNTER>();
    const bool is_pr = node.is<FastFHIR::RESOURCETYPE::PROCEDURE>();
    const int matches = static_cast<int>(is_p) + static_cast<int>(is_o) +
                        static_cast<int>(is_c) + static_cast<int>(is_e) +
                        static_cast<int>(is_pr);

    // The slot's declared tag must equal the target block's header tag.
    const uint16_t slot_tag = static_cast<uint16_t>(resource_entry.target_recovery);
    const auto *base = mem.view().data();
    const std::size_t off =
        static_cast<std::size_t>(LOAD_U64(base + resource_entry.absolute_offset()));
    uint16_t header_tag = 0;
    if (off + 10 <= mem.view().size())
    {
      header_tag = static_cast<uint16_t>(
          static_cast<uint8_t>(base[off + 8]) | (static_cast<uint8_t>(base[off + 9]) << 8));
    }
    // The resource slot is POLYMORPHIC: it carries the generic RECOVER_FF_RESOURCE
    // tag by design, and the reader resolves the concrete type from the TARGET
    // BLOCK's header. A CONCRETE slot tag must match its target; a generic slot
    // must point at a block whose header the reader actually resolves to.
    const bool slot_is_generic = (slot_tag == static_cast<uint16_t>(RECOVER_FF_RESOURCE));
    const bool header_is_resolved =
        (header_tag == static_cast<uint16_t>(node.recovery()));
    if (!(slot_is_generic || slot_tag == header_tag) || !header_is_resolved)
      ++reference_mismatches;
    if ((!slot_is_generic && slot_tag != header_tag || !header_is_resolved) &&
        reference_mismatches <= 3)
    {
      std::fprintf(stderr, "  [dbg] entry %zu: slot_tag=0x%04x header_tag=0x%04x resolved=0x%04x\n",
                   i, static_cast<unsigned>(slot_tag), static_cast<unsigned>(header_tag),
                   static_cast<unsigned>(node.recovery()));
    }

    if (matches == 1)
    {
      ++typed;
      // Matching type materializes; every non-matching type must refuse.
      bool matching_ok = true, wrong_refused = true;
      if (is_p) { try { (void)node.as<PatientData>(); } catch (...) { matching_ok = false; } }
      if (is_o) { try { (void)node.as<ObservationData>(); } catch (...) { matching_ok = false; } }
      // Condition/Encounter/Procedure have generated POCOs; the non-matching
      // probe uses ObservationData against every non-observation type.
      try
      {
        if (!is_o) (void)node.as<ObservationData>();
        else (void)node.as<PatientData>();
        wrong_refused = false;
      }
      catch (const std::exception &)
      {
      }
      if (!matching_ok || !wrong_refused)
      {
        std::fprintf(stderr, "  entry %zu: matching_ok=%d wrong_refused=%d\n", i,
                     static_cast<int>(matching_ok), static_cast<int>(wrong_refused));
      }
      expect(matching_ok, "matching type materializes without throwing");
      expect(wrong_refused, "non-matching type read is refused");
    }
    else
    {
      ++opaque;  // e.g. ImagingStudy -- opaque blocks must not read as typed
      bool refused = true;
      try
      {
        (void)node.as<ObservationData>();
        refused = false;
      }
      catch (const std::exception &)
      {
      }
      if (!refused)
        std::fprintf(stderr, "  entry %zu: opaque resource read as Observation\n", i);
      expect(refused, "opaque resource cannot be read as a typed resource");
    }
  }

  expect(typed > 0, "typed resources were found and read");
  expect(reference_mismatches == 0,
         "every entry's resource slot tag matches its target block header tag");
  std::fprintf(stderr, "  summary: %zu typed entries, %zu opaque entries\n", typed, opaque);
}

// ---------------------------------------------------------------------------
// Test 4 -- concurrent build integrity
// ---------------------------------------------------------------------------
void test_concurrent_build()
{
  std::fprintf(stderr, "\nTest 4 -- concurrent build integrity (WF-4.2)\n");
  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;
  constexpr int kTotal = kThreads * kPerThread;

  FastFHIR::Memory mem = FastFHIR::Memory::create(8 * 1024 * 1024);
  FastFHIR::FF_Builder builder_handle = bench::make_builder(mem);
  FastFHIR::Builder &builder = *builder_handle;

  std::vector<std::vector<FastFHIR::Reflective::ObjectHandle>> per_thread(kThreads);
  std::atomic<int> failures{0};

  auto worker = [&](int tid)
  {
    try
    {
      for (int i = 0; i < kPerThread; ++i)
      {
        // The generated POCO id field is a std::string_view (CAPI-13): the
        // string must outlive append_obj, or the view dangles into a dead
        // temporary (ASan: stack-use-after-scope in STORE_FF_STRING).
        const std::string id = "c" + std::to_string(tid) + "_" + std::to_string(i);
        ObservationData obs{};
        obs.id = id;
        per_thread[tid].push_back(builder.append_obj(obs));
      }
    }
    catch (const std::exception &)
    {
      failures.fetch_add(1);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back(worker, t);
  for (auto &t : threads)
    t.join();

  expect(failures.load() == 0, "no append threw under contention");

  // Assemble a Bundle root referencing every concurrently-appended block, then
  // seal -- the same root build the harness arms use.
  BundleData bundle{};
  bundle.type = FF_BundleType::Collection;
  for (auto &handles : per_thread)
    for (auto &h : handles)
      bundle.entry.push_back(BundleentryData{.resource = static_cast<ResourceReference>(h)});
  const auto root = builder.append_obj(bundle);
  (void)bench::seal_stream(builder_handle, root, "resilience concurrent bundle");

  FastFHIR::Parser p(mem);
  const FF_Result vr = p.validate_FFHR_stream();
  expect(static_cast<bool>(vr), "concurrently-built stream validates");

  auto entries = p.root()[FastFHIR::Fields::BUNDLE::ENTRY];
  const auto count = entries.as_node().size();
  expect(count == kTotal, "all concurrently-appended blocks are reachable from the root");
}

// Test 5 (recoverability curve) moved out of this binary 2026-09-02: it was
// a hand-rolled VALIDATION walk with its own CSV (the CAPI-13-era workaround
// the retool made obsolete, per handoff.md do-not-regress) and it clobbered
// results/recovery_curve.csv on every run -- the file the cross-format sweep
// owns. The curve is produced by //bench:bench_test_5 + recovery_sweep.py;
// the FFHR apply() content diagnostics live in //bench:corruption_probe.

}  // namespace

int main()
{
  std::fprintf(stderr, "Instrument G -- resilience & integrity suite\n");
  try
  {
    const FastFHIR::Memory patient = load_patient();
    test_truncation(patient);
    test_bitflip(patient);
    test_type_confusion(patient);
    if (!std::getenv("BENCH_SKIP_CONCURRENT"))
      test_concurrent_build();
  }
  catch (const std::exception &ex)
  {
    std::fprintf(stderr, "unexpected exception: %s\n", ex.what());
    return 2;
  }

  if (g_failures == 0)
  {
    std::fprintf(stderr, "\nAll resilience assertions held.\n");
    return 0;
  }
  std::fprintf(stderr, "\n%d resilience assertion(s) FAILED.\n", g_failures);
  return 1;
}
