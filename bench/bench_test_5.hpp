// ===========================================================================
// Test 5 -- corruption & recovery, macro-parity architecture (IN-G2)
// ===========================================================================
// Shared header compiled once per arm (D1): each arm TU defines ARM_* and
// includes this file, so bench::test_5::<arm_ns> gets that format's
// implementations. The driver (bench_test_5.cpp) links the arm TUs -- each
// of which exports its operations through a non-inline arm_ops_*() accessor
// at the bottom of this header -- and dispatches INDEPENDENT process modes:
//
//   --hash    <format> --in WIRE            calc_stream_hash -- structural
//                                           fingerprint (anchored units +
//                                           sha256); the BASELINE producer
//   --corrupt <format> --bits K --seed S    corrupt_stream -- flip k random
//               --in WIRE --out DAMAGED     STRUCTURAL bits (per-format)
//   --recover <format> --in DAMAGED         recover_stream -- resync from the
//                                           corrupted bytes ONLY, report the
//                                           recovered units + digest
//   --check   --baseline FILE --recovered FILE   verify recovered ⊆ baseline
//                                           (offset+tag AND parent anchor),
//                                           report integrity, print the
//                                           content-verified %
//   --positions <format> --in WIRE          structural-position count of a
//                                           clean wire (the damage-density
//                                           denominator -- flaw B)
//
// The check is a THIRD process holding the baseline: the recoverer never sees
// the clean artifact. "Recovered" means a unit whose two halves corroborate
// the clean structure -- content verification, not boundary survival, and the
// parent anchor (F3) makes misattachment fail the subset check instead of
// passing it (fixes recovery-test flaws C/F; handoff.md § Test 5).
// ===========================================================================

#pragma once

#include "harness.hpp"
#include "provenance.hpp"  // sha256

// Every arm's scanner renders leaf values as JSON, so this is a direct
// dependency of this header rather than one inherited from whichever arm
// happens to include it first.
#include <nlohmann/json.hpp>
#include <set>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_NDJSON)
#define BENCH_ARM_NS arm_ndjson
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::test_5 {
inline namespace BENCH_ARM_NS {

// A recoverable unit's location + identity -- the atom the recovery is
// verified against. "Recovered" requires BOTH halves to match the baseline.
// `parent` is the referencing slot's absolute offset (parent block + field),
// the F3 anchor: without it a resync onto another block's child passes the
// subset check, and misattachment is invisible (the flaw the edge-level
// fingerprint exists to close). Non-FFHR arms leave it 0.
struct UnitRef {
  std::size_t parent = 0;
  std::size_t offset = 0;
  // 32-bit: the FFHR arms zero-extend 16-bit RECOVERY_TAGs, but HL7v2's tag is
  // the full 3-char segment name (OBX vs OBR must not collide, and a flip on
  // the name's third byte must not be invisible to the anchored check).
  std::uint32_t tag = 0;

  // THE DATA THE UNIT CARRIES, not merely the fact that it is findable.
  //
  // Every arm used to report a unit as recovered on the strength of its
  // HEADER alone -- v2 scanned for `XXX|`, the protobuf arm asked only whether
  // ParseFromArray succeeded, JSON looked for a `"resource"` marker. None of
  // them read the payload, so a record whose every data byte was destroyed
  // still counted. Measured on the v2 artifact: obliterating 95.4% of the
  // file -- every byte of clinical content replaced with 'Z' -- reported
  // pct=100.0 with digest_ok=1. Destroying all 14,701 segment terminators, or
  // 20,673 interior field separators, likewise reported 100.0.
  //
  // `content` is a hash of the unit's own data bytes, so an entry that is
  // still FINDABLE but no longer CORRECT is reported as `wrong` instead of
  // recovered: 01223 != 01223Nsomething. Identity (parent, offset, tag) says
  // the unit is there; content says it is intact. They are different
  // questions and the check now asks both.
  std::uint64_t content = 0;

  // Identity only -- deliberately NOT content. The check compares the two
  // separately so it can distinguish a LOST unit from a WRONG one.
  bool operator==(const UnitRef& o) const {
    return parent == o.parent && offset == o.offset && tag == o.tag;
  }
};

// FNV-1a: the content digest is an equality check over bytes, never a
// security boundary, and it has to stay identical across the four arms.
inline std::uint64_t content_hash(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// The bytes of one unit, clamped to the wire. An empty or out-of-range extent
// hashes as 0, which is distinguishable from any real payload.
inline std::uint64_t content_of(const std::vector<uint8_t>& wire, std::size_t from,
                                std::size_t to) {
  if (from >= wire.size() || to <= from)
    return 0;
  return content_hash(wire.data() + from, std::min(to, wire.size()) - from);
}

#if defined(ARM_FASTFHIR)
// Recovery requires a Memory arena — it is not a read-only file view like
// Parser — but the probe pipeline operates on raw byte vectors. Wrap them in
// a scratch arena whose written extent equals the byte length (claim_space
// advances the head that Memory::size() reports).
inline FastFHIR::Memory wrap_wire_bytes(const std::vector<uint8_t>& wire) {
  FastFHIR::Memory mem =
      FastFHIR::Memory::create(std::max<std::size_t>(wire.size(), 1));
  if (!wire.empty()) {
    mem.claim_space(wire.size());
    std::memcpy(mem.base(), wire.data(), wire.size());
  }
  return mem;
}
#endif

struct StreamFingerprint {
  std::vector<UnitRef> units;  // sorted by offset
  std::string digest;          // sha256 of the unit list -- report integrity stamp

  // The same leaves, UNHASHED. --check only ever needs the hashes, but a
  // DECODER needs the strings back: rebuilding a POCO means addressing a field
  // by name. Carrying both here means all four arms get it from the single
  // funnel they already pass through, instead of four parallel extractions
  // that can drift apart.
  std::vector<std::pair<std::string, std::string>> raw;

  void add_leaf(const std::string& path, const std::string& value);

  void finalize() {
    // Canonical, offset-sorted serialization of the unit list.
    std::string canon;
    for (const auto& u : units) {
      canon.append(reinterpret_cast<const char*>(&u.parent), sizeof(u.parent));
      canon.append(reinterpret_cast<const char*>(&u.offset), sizeof(u.offset));
      canon.append(reinterpret_cast<const char*>(&u.tag), sizeof(u.tag));
      canon.append(reinterpret_cast<const char*>(&u.content), sizeof(u.content));
    }
    bench::provenance::sha256::Ctx c;
    bench::provenance::sha256::update(c, canon.data(), canon.size());
    digest.assign(32, '\0');
    bench::provenance::sha256::finish(c, reinterpret_cast<unsigned char*>(digest.data()));
  }
};

// ── CANONICAL FHIR LEAF PATHS ───────────────────────────────────────────────
//
// Every arm emits the SAME key for the same datum:
//
//     <ResourceType>/<id>.<element path>     e.g. Patient/0fb515d6.address[0].city
//
// Keyed by RESOURCE ID, never by entry index. A positional key would require all
// four encodings to preserve Bundle ordering, and nothing guarantees that --
// protobuf is a record stream, v2 is a segment stream. Every encoding carries
// resourceType and id, so this key survives re-encoding.
//
// `resourceType` is NOT emitted as a leaf: it is already in the key, and the
// encodings disagree about whether it is a stored field at all (FastFHIR
// synthesizes it from the block tag). A field that only some arms can produce
// is a difference in the ENCODERS, not in the data, and counting it would
// charge that difference to the format.
//
// Identity is the path hash, content is the value hash -- which fits the
// existing UnitRef and fingerprint layout unchanged: `offset` carries the path
// hash, `parent`/`tag` go unused, and --check's linear merge already compares
// identity first and content second. That is precisely the four-outcome
// comparison, now over a population all four arms can agree on.
inline UnitRef canonical_leaf(const std::string& path, const std::string& value_raw);

// NUMBERS COMPARE AS NUMBERS.
//
// FastFHIR preserves a decimal's source SCALE (that is a feature -- 1.50 and 1.5
// are different in FHIR), while nlohmann emits shortest-round-trip. So the same
// datum rendered by two arms can differ in text while being the same value, and
// 576 of 1,496 float leaves did. Hashing the text would charge a RENDERING
// difference to the format.
//
// Only bare numbers are normalized: a JSON string arrives quoted, so a string
// field holding "6.07" is untouched and still compares as text. %.17g is
// round-trip exact for a double, so genuinely different values stay different.
// Serialising a leaf value from a CORRUPTED wire.
//
// nlohmann's dump() throws type_error.316 when a string holds invalid UTF-8,
// and a corruption sweep produces exactly that: at k=512 a flipped byte inside
// an HL7v2 ZFX payload aborted the whole run with
// "invalid UTF-8 byte at index 36: 0xFC". A recovery benchmark cannot abort on
// damaged input -- damaged input is the experiment.
//
// error_handler_t::replace substitutes U+FFFD for the bad bytes. The unit stays
// in the census and its content hash no longer matches the baseline, so it is
// scored `wrong` -- the datum changed. That is the accurate verdict; throwing
// reports nothing and dropping the leaf would score it `missing`, which
// understates the damage by calling altered data absent.
inline std::string safe_dump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline std::string canonical_number(const std::string& value) {
    if (value.empty() || value.front() == '"') return value;
    const char* begin = value.c_str();
    char* end = nullptr;
    const double d = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return value;  // not a bare number
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

inline void StreamFingerprint::add_leaf(const std::string& path, const std::string& value) {
    units.push_back(canonical_leaf(path, value));
    raw.emplace_back(path, value);
}

inline UnitRef canonical_leaf(const std::string& path, const std::string& value_raw) {
    const std::string value = canonical_number(value_raw);
    return UnitRef{
        0,
        static_cast<std::size_t>(
            content_hash(reinterpret_cast<const std::uint8_t*>(path.data()), path.size())),
        0,
        content_hash(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()),
    };
}

// The key a resource's leaves hang from. Empty when either half is missing --
// an unidentifiable resource cannot be compared across encodings, and guessing
// a key would manufacture agreement.
inline std::string resource_key(const std::string& type, const std::string& id) {
    if (type.empty() || id.empty()) return {};
    return type + "/" + id;
}

// ---------------------------------------------------------------------------
// 1. calc_stream_hash -- structural fingerprint of a CLEAN stream
// ---------------------------------------------------------------------------
// Forward declaration: the codec headers included below (arm_json_codec.hpp,
// arm_hl7v2_codec.hpp) define each arm's corrupt_stream(), which calls this
// shared harness helper. The definition sits with the rest of the damage
// driver further down; only the declaration needs to precede the includes.
inline std::vector<uint8_t> flip_positions(const std::vector<uint8_t>& wire,
                                           std::vector<std::size_t> positions,
                                           std::size_t k, unsigned seed);

#if defined(ARM_FASTFHIR)
#include "arm_fastfhir_codec.hpp"
#elif defined(ARM_JSON)
#include "arm_json_codec.hpp"
#elif defined(ARM_NDJSON)
#include "arm_ndjson_codec.hpp"
#elif defined(ARM_GOOGLE_FHIR)
#include "arm_google_fhir_codec.hpp"
#elif defined(ARM_HL7V2)
#include "arm_hl7v2_codec.hpp"
#endif

// ---------------------------------------------------------------------------
// 2. corrupt_stream -- polymorphic structural corruption
// ---------------------------------------------------------------------------
// Shared flip engine (used by every arm): XOR one random bit in each of the
// first min(k, N) shuffled structural positions. Positions come from the
// per-arm structural_positions() enumeration below; keeping the shuffle here
// means corruption semantics (deterministic per seed, never payload bytes)
// cannot drift between arms. `positions` is taken by value so callers can
// move a freshly enumerated vector in.
inline std::vector<uint8_t> flip_positions(const std::vector<uint8_t>& wire,
                                           std::vector<std::size_t> positions,
                                           std::size_t k, unsigned seed) {
  auto damaged = wire;
  std::vector<std::size_t> idx(positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
    idx[i] = i;
  std::mt19937 rng(seed);
  std::shuffle(idx.begin(), idx.end(), rng);
  for (std::size_t i = 0; i < k && i < idx.size(); ++i)
    damaged[positions[idx[i]]] ^= static_cast<uint8_t>(1u << (rng() % 8));
  return damaged;
}
#if defined(ARM_FASTFHIR)
// fastfhir: see arm_fastfhir_codec.hpp (included at the reader chain above).
#elif defined(ARM_JSON)
// json: see arm_json_codec.hpp (included at the reader chain above).
#elif defined(ARM_GOOGLE_FHIR)
// google_fhir: see arm_google_fhir_codec.hpp (included at the reader chain above).
#elif defined(ARM_HL7V2)
// hl7v2: see arm_hl7v2_codec.hpp (included at the reader chain above).
#endif

// ---------------------------------------------------------------------------
// 3. recover_stream -- polymorphic recovery (corrupted bytes ONLY)
// ---------------------------------------------------------------------------
#if defined(ARM_FASTFHIR)
// fastfhir: see arm_fastfhir_codec.hpp (included at the reader chain above).
#elif defined(ARM_JSON)
// json: see arm_json_codec.hpp (included at the reader chain above).
#elif defined(ARM_GOOGLE_FHIR)
// google_fhir: see arm_google_fhir_codec.hpp (included at the reader chain above).
#elif defined(ARM_HL7V2)
// hl7v2: see arm_hl7v2_codec.hpp (included at the reader chain above).
#endif


#if defined(ARM_FASTFHIR) || defined(ARM_JSON) || defined(ARM_HL7V2) || \
    defined(ARM_GOOGLE_FHIR) || defined(ARM_NDJSON)
// Leaves present in what an arm actually WROTE, measured from the output
// rather than from the fixture, so an arm that dropped fields reports fewer.
// Always called after the stage's clock has stopped.
//
// All four arms had this same four-line block inline (build a byte vector,
// call calc_stream_hash, take units.size()). It lives here because
// calc_stream_hash is the per-arm overload selected by BENCH_ARM_NS, so a
// helper in harness.hpp could not see it.
inline std::int64_t count_output_elements(const char* data, std::size_t size) {
    const std::vector<uint8_t> bytes(data, data + size);
    return static_cast<std::int64_t>(calc_stream_hash(bytes).units.size());
}

inline std::int64_t count_output_elements(std::string_view wire) {
    return count_output_elements(wire.data(), wire.size());
}
#endif  // an arm is selected -- calc_stream_hash only exists under one of the
        // four ARM_* macros, and bench_test_5.cpp compiles this header with
        // none of them set (BENCH_ARM_NS == arm_none) to get the shared types.

}  // inline namespace BENCH_ARM_NS

// ---------------------------------------------------------------------------
// Per-arm dispatch (driver side)
// ---------------------------------------------------------------------------
// The implementations above are `inline`: each arm TU compiles exactly its own
// macro-guarded copy, and a driver TU that CALLS them cross-TU would link
// against definitions it never saw (the ODR failure mode notes.md § 1 was
// written about). Each arm TU therefore exports one non-inline accessor below;
// the driver builds its format table from the four accessors and never touches
// the inline functions itself.
struct ArmOps {
  const char* name;  // format string accepted on the command line
  StreamFingerprint (*calc_hash)(const std::vector<uint8_t>& wire);
  std::vector<uint8_t> (*corrupt)(const std::vector<uint8_t>& wire,
                                  std::size_t bits, unsigned seed);
  StreamFingerprint (*recover)(const std::vector<uint8_t>& wire);
  // Structural-position count of a CLEAN wire -- the damage-density
  // denominator (handoff.md test-5 flaw B): k flips means something different
  // per format until the axis is "fraction of positions corrupted".
  std::size_t (*count_positions)(const std::vector<uint8_t>& wire);
};

const ArmOps& arm_ops_fastfhir();
const ArmOps& arm_ops_json();
const ArmOps& arm_ops_ndjson();
const ArmOps& arm_ops_google_fhir();
const ArmOps& arm_ops_hl7v2();

}  // namespace bench::test_5
