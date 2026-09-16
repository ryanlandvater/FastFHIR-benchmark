#pragma once

#include <sys/resource.h>
#include <sys/time.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Condition.hpp>
#include <FF_Encounter.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Observation.hpp>
#include <FF_Patient.hpp>
#include <FF_Procedure.hpp>

namespace bench {

// Timed sections are declared here first for manual reviewability.
// Test 1 start: immediately before first field write to destination representation.
// Test 1 end: immediately after payload sealed.
// Test 1-compact start: immediately before Compactor::archive; end: compact stream sealed.
//   FastFHIR-native serialization variant (IN-E), NOT part of the cross-arm
//   parity grid -- the arm emits it only when the losslessness gate passes.
// Test 2 start: first random-access read, navigating from the root.
// Test 2 end: all target reads complete, values in hand.
// Test 3 start: first parser/read call that consumes bytes or materialized nodes for query.
// Test 3 end: target value extracted into result variable.
// The *_Compact stages run the same probes over the compact archive (FF arm
// only, same losslessness gate). The claim they test: the reader is
// layout-agnostic, so compact ≈ standard speed.

enum class Stage {
  Test1Serialize,
  Test1Compact,
  Test2RandomAccess,
  Test2RandomAccessCompact,
  Test3Query,
  Test3QueryCompact,
  // A SELECTIVE query, as opposed to Test3Query's census.
  //
  // Test3Query's QuerySummary has 17 output fields -- value kind, effective
  // kind, issued presence and component value kinds for EVERY observation --
  // so it cannot short-circuit: every field it reads feeds a required output.
  // That measures full traversal, which is a real workload but is not what a
  // clinical system usually asks. The common ask is "find the cholesterol
  // results": is this an Observation, does it carry LOINC 2085-9, and if not,
  // move to the next record without touching anything else.
  //
  // The distinction matters because it is exactly where a zero-copy lens
  // architecture should win and a parse-first format cannot: FastFHIR can
  // reject a non-match on a tag compare and a code compare, while JSON must
  // materialise the document before it can ask the first question.
  Test3Selective,
  Test4Enrich,
  Test4EnrichCompact,
};

struct MetricEvent {
  std::string arm;
  Stage stage;
  std::int64_t duration_ns;

  // Wire bytes crossing this stage's boundary (TASKS.md IN-0). Until these
  // existed the harness emitted only nanoseconds, so no size claim -- the
  // compact-archive figure, or any throughput expressed per byte -- had an
  // instrument at all. `target_mb` is the *requested* corpus size and is not a
  // measurement; these are.
  //
  //   Test 1 serialize   bytes_out = sealed payload; bytes_in 0 (input is POCOs)
  //   Test 1 compact     bytes_out = compact archive size (FastFHIR arm only,
  //                       IN-E losslessness gate)
  //   Test 2 random acc. bytes_out = id bytes read (the cross-arm parity
  //                       accumulator); bytes_in 0
  //   Test 3 query       both 0
  //   Test 4 enrich      bytes_in  = source stream; bytes_out = enriched stream
  //
  // 0 means "not applicable to this stage", never "measured zero" -- a stage
  // that produces bytes and reports 0 is a bug, and emit_metric says so.
  std::int64_t bytes_in = 0;
  std::int64_t bytes_out = 0;

  // Units of work this stage performed, so a duration can be normalized rather
  // than compared raw across formats that do different amounts of work for the
  // same clinical content (PA-5). Field reads for Test 2. 0 where the notion
  // does not apply.
  std::int64_t ops = 0;

  // RESOURCES this stage handled -- the cross-arm parity accumulator for
  // CONTENT, as bytes_out is for Test 2's reads.
  //
  // Every arm is handed the same fixture, so every arm must serialize the same
  // number of resources, query the same number, and enrich the same number. An
  // arm that quietly drops some looks FASTER, and nothing in a duration or a
  // byte count says otherwise: a smaller payload reads as a more compact
  // format, not as a lossy one. Both defects found in this suite had that
  // shape -- the v2 arm writing a literal for every observation value, and the
  // protobuf arm never writing Meta.profile at all -- and neither was visible
  // in any number the harness printed.
  //
  // 0 means "not applicable to this stage", never "handled none": a stage that
  // processes resources and reports 0 fails the gate in main().
  std::int64_t entries = 0;

  // CPU nanoseconds (user+sys, summed over every thread in the process) burned
  // inside the same window duration_ns measures. cpu_ns / duration_ns is the
  // average number of cores busy: 1.0 is a serial stage, 18.0 saturates this
  // host. Without it a wall-clock table cannot distinguish "this arm is
  // efficient" from "this arm used more cores", and those are different claims.
  //
  // -1 means the stage did not sample it.
  std::int64_t cpu_ns = -1;
};

struct ArmRunResult {
  std::vector<MetricEvent> metrics;
  std::string queried_value;
  std::string reconstructed_bundle_json;
  // Test-1 wire payload (sealed FFHR bytes / JSON text / protobuf TLV / HL7v2
  // batch). Used only by --dump-artifacts to produce corruption-probe inputs;
  // a copy per sample, so populated lazily by that mode's arms.
  std::string test1_payload;
  std::variant<std::monostate, FastFHIR::Memory, std::string> enriched_stream;
  std::string enrich_metrics_summary;
  std::string random_access_summary;
  // LOINC 2085-9 (HDL cholesterol) hits from Test 3. Carried as a NUMBER rather
  // than left inside queried_value's formatted string: the cross-arm gate has to
  // compare it, and google_fhir was never compared at all because validate_parity
  // only ever looked at fastfhir/json/hl7.
  std::int64_t query_loinc_matches = 0;
  // Cross-arm gate for Stage::Test3Selective: every arm must find the same
  // cholesterol results. A selective query that is fast because it matched
  // nothing is exactly the failure this catches.
  std::int64_t selective_matches = -1;
  // DATA ELEMENTS (leaves) in this arm's Test 1 output -- patient.name,
  // patient.gender, each lab value. `entries` counts RESOURCES: 1 Patient plus
  // 316 Observations is 317, which is true and much smaller than the data. The
  // parity question is about elements, so the table reports both.
  //
  // -1 means "not measured on this run": computing it re-walks the whole
  // serialized stream, so the harness asks for it once per bundle size rather
  // than on every timed run. It is always read AFTER the clock stops.
  std::int64_t test1_elements = -1;
};

// Set by the harness for the first run of each size only; see test1_elements.
inline bool g_count_elements = false;

// --serial-build: force every arm's Test 1 construction loop onto one thread.
//
// Two arms build their per-resource representation with dispatch_apply on
// macOS -- FastFHIR through Builder::append_obj (lock-free claim_space) and
// json_fhir through independent nlohmann DOMs merged afterwards. Default is
// parallel for both: each arm runs at its best, which is the comparison the
// timing table claims to make.
//
// The flag exists because the two runs answer different questions. Parallel vs
// parallel is the format comparison; serial vs serial isolates per-resource
// encoding cost from thread count. Running the same corpus both ways is what
// separates "FastFHIR is a faster encoder" from "FastFHIR scales with cores",
// and one number cannot say both.
//
// hl7v2 and google_fhir are serial in every configuration, so this flag does
// not reach them.
inline bool g_serial_build = false;

// Process-wide CPU time (user+sys over ALL threads). getrusage(RUSAGE_SELF)
// aggregates every thread, which is exactly the quantity that reveals whether a
// parallel stage actually occupied the cores it dispatched to.
inline std::int64_t process_cpu_ns() {
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
  const auto to_ns = [](const struct timeval& tv) -> std::int64_t {
    return static_cast<std::int64_t>(tv.tv_sec) * 1000000000LL +
           static_cast<std::int64_t>(tv.tv_usec) * 1000LL;
  };
  return to_ns(ru.ru_utime) + to_ns(ru.ru_stime);
}

// user/sys split of the same counters. A parallel stage that inflates USER
// time is spinning (a CAS retry loop); one that inflates SYS time is churning
// on park/wake syscalls. The totals cannot tell those apart, and the fix is
// different for each.
inline void process_cpu_split_ns(std::int64_t& user_ns, std::int64_t& sys_ns) {
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) != 0) { user_ns = sys_ns = -1; return; }
  const auto to_ns = [](const struct timeval& tv) -> std::int64_t {
    return static_cast<std::int64_t>(tv.tv_sec) * 1000000000LL +
           static_cast<std::int64_t>(tv.tv_usec) * 1000LL;
  };
  user_ns = to_ns(ru.ru_utime);
  sys_ns = to_ns(ru.ru_stime);
}

class Timer {
 public:
  void start() { begin_ = std::chrono::steady_clock::now(); cpu_begin_ = process_cpu_ns(); }
  // CPU nanoseconds since start(). Read it in the same breath as stop_ns().
  std::int64_t cpu_ns() const {
    const std::int64_t now = process_cpu_ns();
    return (now < 0 || cpu_begin_ < 0) ? -1 : now - cpu_begin_;
  }
  std::int64_t stop_ns() const {
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_).count();
    if (elapsed_ns <= 0) {
      return 1;
    }
    return elapsed_ns;
  }

 private:
  std::chrono::steady_clock::time_point begin_{};
  std::int64_t cpu_begin_ = -1;
};

inline std::string to_string(Stage s) {
  switch (s) {
    case Stage::Test1Serialize:
      return "test_1_serialize";
    case Stage::Test1Compact:
      return "test_1_compact";
    case Stage::Test2RandomAccess:
      return "test_2_random_access";
    case Stage::Test2RandomAccessCompact:
      return "test_2_compact";
    case Stage::Test3Query:
      return "test_3_query";
    case Stage::Test3QueryCompact:
      return "test_3_compact";
    case Stage::Test3Selective:
      return "test_3_selective";
    case Stage::Test4Enrich:
      return "test_4_enrich";
    case Stage::Test4EnrichCompact:
      return "test_4_compact";
  }
  return "unknown";
}

inline void print_metric(const MetricEvent& e) {
  std::cout << e.arm << "," << to_string(e.stage) << "," << e.duration_ns << "\n" << std::flush;
}

inline constexpr std::string_view kPatientQueryField = "birthDate";
inline constexpr std::string_view kCholesterolLoincCode = "2085-9";
inline constexpr std::string_view kLoincSystem = "http://loinc.org";

// ---------------------------------------------------------------------------
// FastFHIR stream helpers (ported to the FF_* facade 2026-08-25)
// ---------------------------------------------------------------------------
// Builder::set_root() and Builder::finalize() became private in FastFHIR
// a9fd4e9. The only way in is the friend free functions FF_BuilderSetRoot /
// FF_BuilderFinalize, which take an FF_Builder -- and FF_Builder is exactly
// std::shared_ptr<Builder>. So we build the shared_ptr ourselves rather than
// going through FF_CreateBuilder: that keeps the existing "the fixture owns the
// arena, the builder borrows it" model, which BundlePatient::memory and
// clone_bundle_patient() both depend on.

inline FastFHIR::FF_Builder make_builder(const FastFHIR::Memory& memory,
                                        FHIR_VERSION version = FHIR_VERSION_R5) {
  return std::make_shared<FastFHIR::Builder>(memory, version);
}

// Benchmark-mode checksum. FF_BuilderFinalize requires a hasher whenever the
// algorithm is not NONE; hashing for real would measure OpenSSL rather than
// FastFHIR, so this returns a zeroed digest of the right width. The stream
// still carries a SHA256 header, exactly as it did before the port.
inline std::vector<BYTE> bench_null_sha256(const unsigned char*, Size) {
  return std::vector<BYTE>(32, 0);
}

// Assign the root and seal the arena. Throws with the FF_Result message on
// failure -- the facade is noexcept and returns status codes, so an unchecked
// call would silently produce an unsealed stream and a fast, meaningless
// number.
inline FastFHIR::Memory::View seal_stream(const FastFHIR::FF_Builder& builder,
                                          const FastFHIR::Reflective::ObjectHandle& root,
                                          std::string_view context,
                                          FF_Checksum_Algorithm algorithm = FF_CHECKSUM_SHA256) {
  FF_Result result = FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
      .builder = builder,
      .root = root,
  });
  if (!result) {
    throw std::runtime_error("FF_BuilderSetRoot failed for " + std::string(context) + ": " +
                             result.message);
  }

  FastFHIR::Memory::View view;
  result = FastFHIR::FF_BuilderFinalize(
      FastFHIR::FF_BuilderFinalizeInfo{
          .builder = builder,
          .algorithm = algorithm,
          // A hasher is required only when the algorithm is not NONE.
          .hasher = algorithm == FF_CHECKSUM_NONE ? FastFHIR::FF_HashCallback{}
                                                  : FastFHIR::FF_HashCallback{bench_null_sha256},
      },
      view);
  if (!result) {
    throw std::runtime_error("FF_BuilderFinalize failed for " + std::string(context) + ": " +
                             result.message);
  }
  return view;
}

// Reads a FHIR text-valued field off a FastFHIR node.
//
// Upstream DT-2 packs date / dateTime / time / instant into an inline 8-byte
// slot, so Node::as<std::string_view>() -- which demands a string-layout
// recovery tag -- throws "Node is not a string or code" on Patient.birthDate.
// Only values that do not fit the packed form spill to an FF_STRING, and the
// parser hands those back already tagged as strings, so the fast path below
// still covers them.
//
// There is no public zero-copy accessor that renders a packed date/time as
// text; print_json is the only public path, and it costs an ostringstream plus
// JSON escaping that the string case does not pay. That asymmetry is a real
// measurement distortion in the FastFHIR arm's query stage -- see notes.md,
// "Packed date/time has no zero-copy public reader".
inline std::string read_text_field(const FastFHIR::Reflective::Node& node) {
  if (!node) {
    return {};
  }
  if (node.kind() == FF_FIELD_DATETIME) {
    std::ostringstream oss;
    node.print_json(oss);
    std::string text = oss.str();
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
      return text.substr(1, text.size() - 2);
    }
    return text;
  }
  return std::string(node.as<std::string_view>());
}


// One ingested Synthea patient item kept in RAM.
// The FastFHIR::Memory arena contains the serialized FFHR patient binary.
struct BundlePatient {
  FastFHIR::Memory memory;

  // EXTENSION URLs, RESOLVED FROM THE TRIE.
  //
  // ExtensionData::url is a uint32 index into FastFHIR's FF_URL_DIRECTORY --
  // a per-'/' radix trie that stores each path segment once, so repeated
  // endpoints cost one entry rather than one string per use. That is a
  // deliberate size win, not lost information: get_url() walks the prior
  // chain and rebuilds the string.
  //
  // Nothing did that walk, so every arm serialized the raw index. The JSON
  // arm emitted {"urlIndex": 16} where FHIR requires
  // {"url": "http://hl7.org/fhir/StructureDefinition/geolocation"} -- which
  // is not FHIR, and made the JSON competitor unparseable by anything else.
  //
  // Resolved ONCE here, at ingest, because the trie lives in this arena and
  // the encoders only ever see the structs.
  std::vector<std::string> url_table;

  PatientData patient;
  std::vector<EncounterData> encounters;
  std::vector<ConditionData> conditions;
  std::vector<ProcedureData> procedures;
  std::vector<ObservationData> observations;
};

struct BundleBenchFixture {
  std::vector<BundlePatient> bundle;
  int64_t target_size_bytes = 0;
  int64_t actual_ingested_bytes = 0;
  int64_t fastfhir_vma_bytes = 0;
};

struct EnrichmentObservationFixture {
  FastFHIR::Memory memory;
  ObservationData observation;
};

namespace detail {
void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                              BundlePatient& bundle_patient);
}

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path);
// Same ingest, from TEXT: the shared back half of every arm's decoder.
BundlePatient make_bundle_patient_from_json_text(std::string_view json,
                                                 std::string_view label);
EnrichmentObservationFixture load_enrichment_observation_from_json(const std::filesystem::path& json_path);

inline BundlePatient clone_bundle_patient(const BundlePatient& src) {
  BundlePatient dst{};
  dst.memory = src.memory;
  // The resolved URL strings travel with the arena they were rebuilt from.
  // Re-hydration below restores the POCOs but not this: it reads the structs,
  // and ExtensionData carries the intern INDEX, not the string. Dropping it
  // here left every cloned item with an empty table, so the encoders resolved
  // nothing and the JSON arm emitted extensions with no url at all.
  dst.url_table = src.url_table;

  // Re-hydrate the POCO bundle from copied FFHR memory so cloned fixtures keep
  // string-backed fields valid after arena duplication.
  if (dst.memory) {
    FastFHIR::Parser parser(dst.memory);
    detail::hydrate_bundle_resources(parser.root(), dst);
  }

  // Fallback for defensive compatibility if re-hydration cannot locate a patient.
  if (dst.patient.id.empty() && !src.patient.id.empty()) {
    dst.patient.id = src.patient.id;
  }
  if (dst.patient.birthdate.empty() && !src.patient.birthdate.empty()) {
    dst.patient.birthdate = src.patient.birthdate;
  }
  dst.patient.gender = src.patient.gender;
  dst.patient.active = src.patient.active;
  return dst;
}

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture);

// ---------------------------------------------------------------------------
// Cross-arm result validation
// ---------------------------------------------------------------------------

namespace detail {

// A resource belongs to the fixture's patient when its subject reference names
// that patient. The Synthea corpus writes intra-bundle references two ways:
// the canonical "Patient/<id>" and the raw "urn:uuid:<id>" form (the file's
// fullUrl style). Both must match, or the whole corpus hydrates to patient-only
// fixtures -- measured 2026-09-02: 93-171 Observation nodes per Synthea file,
// zero surviving the filter, and the test-5 artifacts shrank ~50x as a result.
inline bool reference_matches_patient_id(const ReferenceData& reference,
                                         std::string_view patient_id) {
  if (patient_id.empty() || reference.reference.empty()) {
    return false;
  }

  if (reference.reference == patient_id) {
    return true;
  }

  constexpr std::string_view kPatientPrefix = "Patient/";
  constexpr std::string_view kUuidPrefix = "urn:uuid:";
  if (reference.reference.size() == kPatientPrefix.size() + patient_id.size() &&
      reference.reference.substr(0, kPatientPrefix.size()) == kPatientPrefix &&
      reference.reference.substr(kPatientPrefix.size()) == patient_id) {
    return true;
  }
  return reference.reference.size() == kUuidPrefix.size() + patient_id.size() &&
         reference.reference.substr(0, kUuidPrefix.size()) == kUuidPrefix &&
         reference.reference.substr(kUuidPrefix.size()) == patient_id;
}

template <typename Resource>
inline void copy_resources_for_patient(const std::vector<FastFHIR::Reflective::Node>& resource_nodes,
                                       std::string_view patient_id,
                                       std::vector<Resource>& out) {
  out.clear();
  out.reserve(resource_nodes.size());

  for (const auto& resource_node : resource_nodes) {
    auto resource = resource_node.as<Resource>();
    if (!patient_id.empty() && resource.subject) {
      if (!reference_matches_patient_id(*resource.subject, patient_id)) {
        continue;
      }
    }
    out.push_back(std::move(resource));
  }
}

inline void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                                     BundlePatient& bundle_patient) {
  bundle_patient.patient = PatientData{};
  bundle_patient.encounters.clear();
  bundle_patient.conditions.clear();
  bundle_patient.procedures.clear();
  bundle_patient.observations.clear();

  FastFHIR::Reflective::Node patient_node;
  std::vector<FastFHIR::Reflective::Node> encounter_nodes;
  std::vector<FastFHIR::Reflective::Node> condition_nodes;
  std::vector<FastFHIR::Reflective::Node> procedure_nodes;
  std::vector<FastFHIR::Reflective::Node> observation_nodes;

  if (root && root.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    patient_node = root;
  } else if (root && root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    if (auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY]) {
      for (auto& entry : entries.entries()) {
        auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource) {
          continue;
        }

        auto resource_node = resource.as_node();
        if (!resource_node) {
          continue;
        }

        if (!patient_node && resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
          patient_node = resource_node;
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::ENCOUNTER>()) {
          encounter_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::CONDITION>()) {
          condition_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::PROCEDURE>()) {
          procedure_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
          observation_nodes.push_back(resource_node);
        }
      }
    }
  }

  if (patient_node && patient_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    bundle_patient.patient = patient_node.as<PatientData>();
  }

  copy_resources_for_patient(encounter_nodes, bundle_patient.patient.id, bundle_patient.encounters);
  copy_resources_for_patient(condition_nodes, bundle_patient.patient.id, bundle_patient.conditions);
  copy_resources_for_patient(procedure_nodes, bundle_patient.patient.id, bundle_patient.procedures);
  copy_resources_for_patient(observation_nodes, bundle_patient.patient.id, bundle_patient.observations);

  // choice[x] sanitisation used to run here, clearing every block-typed value
  // because the POCO carried a raw source-arena OFFSET that could not be
  // re-serialized. FastFHIR now hands back the DECODED value instead
  // (ChoiceEntry::block for blocks; resolved text for codes and date/time
  // fallbacks), so there is nothing left to cut -- all three arms of the old
  // cross-arena test are unreachable, and Test 1 measures value[x] again.
  if (std::getenv("BENCH_CODE_CENSUS")) {
    // Lens vs POCO on the SAME node: does the reflective reader find
    // Observation.code where as<ObservationData>() does not?
    std::size_t lens_code = 0, lens_coding = 0, poco_code = 0;
    for (const auto& n : observation_nodes) {
      if (auto ce = n[FastFHIR::Fields::OBSERVATION::CODE]) {
        ++lens_code;
        auto cc = ce.as_node();
        if (cc) {
          if (auto cod = cc[FastFHIR::Fields::CODEABLECONCEPT::CODING]) {
            if (cod.size() > 0) ++lens_coding;
          }
        }
      }
      if (n.as<ObservationData>().code) ++poco_code;
    }
    // Which other fields does the POCO deserializer drop on the same nodes?
    std::size_t p_cat = 0, p_subject = 0, p_id = 0, p_dar = 0, p_comp = 0;
    std::size_t l_cat = 0, l_comp = 0, l_subj = 0;
    for (const auto& n : observation_nodes) {
      const auto o = n.as<ObservationData>();
      if (!o.category.empty()) ++p_cat;
      if (o.subject) ++p_subject;
      if (!o.id.empty()) ++p_id;
      if (o.dataabsentreason) ++p_dar;
      if (!o.component.empty()) ++p_comp;
      if (auto e = n[FastFHIR::Fields::OBSERVATION::CATEGORY]) { if (e.size() > 0) ++l_cat; }
      if (n[FastFHIR::Fields::OBSERVATION::SUBJECT]) ++l_subj;
      if (auto e = n[FastFHIR::Fields::OBSERVATION::COMPONENT]) { if (e.size() > 0) ++l_comp; }
    }
    std::fprintf(stderr,
                 "[code-census] POCO: id=%zu subject=%zu category=%zu component=%zu "
                 "dataAbsentReason=%zu | lens: category=%zu component=%zu subject=%zu\n",
                 p_id, p_subject, p_cat, p_comp, p_dar, l_cat, l_comp, l_subj);
    std::fprintf(stderr, "[code-census] source nodes=%zu | lens sees code=%zu coding[]=%zu | "
                 "as<ObservationData>().code=%zu\n",
                 observation_nodes.size(), lens_code, lens_coding, poco_code);
  }
  if (std::getenv("BENCH_CODE_CENSUS")) {
    std::size_t with_code = 0, with_coding = 0, with_code_str = 0;
    for (const auto& o : bundle_patient.observations) {
      if (!o.code) continue;
      ++with_code;
      if (!o.code->coding.empty()) ++with_coding;
      for (const auto& c : o.code->coding)
        if (!c.code.empty()) { ++with_code_str; break; }
    }
    std::fprintf(stderr, "[code-census] pre-sanitize: %zu observations, %zu with code, "
                 "%zu with coding[], %zu with a non-empty coding.code\n",
                 bundle_patient.observations.size(), with_code, with_coding, with_code_str);
  }
  for (auto& observation : bundle_patient.observations) {
    if (std::getenv("BENCH_DROP_OBS_CODE")) observation.code.reset();
    if (std::getenv("BENCH_DROP_OBS_CAT")) observation.category.clear();
    if (std::getenv("BENCH_DROP_OBS_COMP")) observation.component.clear();
    if (std::getenv("BENCH_DROP_OBS_EXT")) { observation.extension.clear(); observation.modifierextension.clear(); }
    if (std::getenv("BENCH_DROP_OBS_RR")) observation.referencerange.clear();
    if (std::getenv("BENCH_DROP_OBS_INTERP")) observation.interpretation.clear();
  }
}

// Extract a key=value field from a queried_value string.
inline std::string qv_field(const std::string& qv, const std::string& key) {
  const std::string needle = key + "=";
  const auto pos = qv.find(needle);
  if (pos == std::string::npos) return "";
  const auto start = pos + needle.size();
  const auto end = qv.find(' ', start);
  return end == std::string::npos ? qv.substr(start) : qv.substr(start, end - start);
}

// Reduce a date string to its digit characters for format-agnostic comparison.
inline std::string digits_only(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) if (std::isdigit(c)) out += static_cast<char>(c);
  return out;
}

} // namespace detail

// Compare the query results across FFHR and JSON arms and print a warning to stderr
// for each discrepancy.  Returns true only if every check passes.
//
// Checks performed:
//   1. patients=N   — both arms must agree
//   2. birthdate    — normalized to digits; both arms must agree
//   3. encounters   — when present in both arms, values must agree
//   4. conditions   — when present in both arms, values must agree
//   5. observations — required in both arms and values must agree
//   6. loinc_2085_9_matches — required in both arms and values must agree
//   7. Observation value/effective/component semantic counters — required parity
inline bool validate_results(const ArmRunResult& ff, const ArmRunResult& jf) {
  using detail::qv_field;
  using detail::digits_only;
  bool ok = true;

  auto compare_required_field = [&](const char* key) {
    const auto ff_value = qv_field(ff.queried_value, key);
    const auto jf_value = qv_field(jf.queried_value, key);
    if (ff_value.empty() || jf_value.empty()) {
      std::cerr << "[validate] MISSING required field " << key << ":"
                << " fastfhir=" << (ff_value.empty() ? "<missing>" : ff_value)
                << " json=" << (jf_value.empty() ? "<missing>" : jf_value) << "\n";
      ok = false;
      return;
    }
    if (ff_value != jf_value) {
      std::cerr << "[validate] MISMATCH " << key << ":"
                << " fastfhir=" << ff_value
                << " json=" << jf_value << "\n";
      ok = false;
    }
  };

  // 1. Patient count
  const auto ff_pat = qv_field(ff.queried_value, "patients");
  const auto jf_pat = qv_field(jf.queried_value, "patients");
  if (ff_pat != jf_pat) {
    std::cerr << "[validate] MISMATCH patients:"
              << " fastfhir=" << ff_pat
              << " json="     << jf_pat << "\n";
    ok = false;
  }

  // 2. Birthdate (spot-check first patient; format-normalised)
  const auto ff_bd = digits_only(qv_field(ff.queried_value, "birthdate"));
  const auto jf_bd = digits_only(qv_field(jf.queried_value, "birthdate"));
  if (!ff_bd.empty() && ff_bd != jf_bd) {
    std::cerr << "[validate] MISMATCH birthdate:"
              << " fastfhir=" << ff_bd
              << " json="     << jf_bd << "\n";
    ok = false;
  }

  // 3. Encounter count (optional until all arms emit it)
  const auto ff_enc = qv_field(ff.queried_value, "encounters");
  const auto jf_enc = qv_field(jf.queried_value, "encounters");
  if (!ff_enc.empty() && !jf_enc.empty()) {
    if (ff_enc != jf_enc) {
      std::cerr << "[validate] MISMATCH encounters:"
                << " fastfhir=" << ff_enc
                << " json="     << jf_enc << "\n";
      ok = false;
    }
  }

  // 4. Condition count (optional until all arms emit it)
  const auto ff_cond = qv_field(ff.queried_value, "conditions");
  const auto jf_cond = qv_field(jf.queried_value, "conditions");
  if (!ff_cond.empty() && !jf_cond.empty()) {
    if (ff_cond != jf_cond) {
      std::cerr << "[validate] MISMATCH conditions:"
                << " fastfhir=" << ff_cond
                << " json="     << jf_cond << "\n";
      ok = false;
    }
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // 6. Reconstructed fixture parity (both arms must reconstruct identical bundle JSON)
  if (!ff.reconstructed_bundle_json.empty() && !jf.reconstructed_bundle_json.empty()) {
    if (ff.reconstructed_bundle_json != jf.reconstructed_bundle_json) {
      std::cerr << "[validate] MISMATCH reconstructed_bundle_json:"
                << " fastfhir=" << ff.reconstructed_bundle_json.size()
                << " json=" << jf.reconstructed_bundle_json.size() << "\n";
      ok = false;
    }
  }

  return ok;
}

// Compare HL7v2 query outputs against FFHR/JSON baseline for the subset of
// fields that HL7v2 currently models directly.
inline bool validate_hl7_results(const ArmRunResult& ff,
                                 const ArmRunResult& jf,
                                 const ArmRunResult& h2) {
  using detail::digits_only;
  using detail::qv_field;

  bool ok = true;
  auto compare_required_field = [&](const char* key) {
    const auto baseline_value = qv_field(jf.queried_value, key);
    const auto hl7_value = qv_field(h2.queried_value, key);
    if (baseline_value.empty() || hl7_value.empty()) {
      std::cerr << "[validate] MISSING hl7 required field " << key << ":"
                << " baseline=" << (baseline_value.empty() ? "<missing>" : baseline_value)
                << " hl7=" << (hl7_value.empty() ? "<missing>" : hl7_value) << "\n";
      ok = false;
      return;
    }
    if (baseline_value != hl7_value) {
      std::cerr << "[validate] MISMATCH hl7 " << key << ":"
                << " baseline=" << baseline_value
                << " hl7=" << hl7_value << "\n";
      ok = false;
    }
  };

  compare_required_field("patients");

  const auto baseline_birthdate = digits_only(qv_field(jf.queried_value, "birthdate"));
  const auto hl7_birthdate = digits_only(qv_field(h2.queried_value, "birthdate"));
  if (baseline_birthdate.empty() || hl7_birthdate.empty()) {
    std::cerr << "[validate] MISSING hl7 required field birthdate:"
              << " baseline=" << (baseline_birthdate.empty() ? "<missing>" : baseline_birthdate)
              << " hl7=" << (hl7_birthdate.empty() ? "<missing>" : hl7_birthdate) << "\n";
    ok = false;
  } else if (baseline_birthdate != hl7_birthdate) {
    std::cerr << "[validate] MISMATCH hl7 birthdate:"
              << " baseline=" << baseline_birthdate
              << " hl7=" << hl7_birthdate << "\n";
    ok = false;
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // Keep FFHR input referenced to make baseline dependency explicit and avoid
  // accidental drift where only one arm is checked.
  (void)ff;
  return ok;
}

}  // namespace bench
