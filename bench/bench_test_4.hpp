#pragma once

#include "harness.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#include <FF_Ingestor.hpp>
// APPEND-1 (FastFHIR FF_BundleAppend.hpp) is newer than some revisions the A/B
// driver pins as a baseline. Detect it rather than require it, so both sides
// of an A/B still build -- and the pair then measures exactly this change.
#if __has_include(<FF_BundleAppend.hpp>)
#include <FF_BundleAppend.hpp>
#define BENCH_FF_HAS_APPEND1 1
#else
#define BENCH_FF_HAS_APPEND1 0
#endif
#define BENCH_TEST_4_ENRICH_FN enrich_fastfhir
#elif defined(ARM_JSON)
#include <nlohmann/json.hpp>
#define BENCH_TEST_4_ENRICH_FN enrich_json
#elif defined(ARM_HL7V2)
#include "hl7v2_message.hpp"
#define BENCH_TEST_4_ENRICH_FN enrich_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#define BENCH_TEST_4_ENRICH_FN enrich_google_fhir
#endif

#include "bench_test_1.hpp"


// ---------------------------------------------------------------------------
// Per-arm namespace -- REQUIRED FOR CORRECTNESS, not style.
// ---------------------------------------------------------------------------
// Each arm compiles these headers with a different ARM_* macro, so the SAME
// type and function names get four DIFFERENT definitions across four
// translation units: bench::test_2::MaterializedTree holds a
// unique_ptr<FastFHIR::Parser> in one TU, a simdjson element in another, and
// two protobuf vectors in a third.
//
// That is a One Definition Rule violation. The linker keeps one definition of
// each inline function and destructor and discards the rest, so an object built
// with one layout gets destroyed with another. It manifests as heap corruption
// far from the cause -- ASan caught it as a SEGV inside
// ~vector<google::fhir::r4::core::Observation> from
// bench::test_2::MaterializedTree::~MaterializedTree, and it also moved the
// apparent crash site around between -c opt and -c dbg builds, which is the
// classic signature.
//
// An inline namespace gives each arm its own mangled symbols while leaving
// every existing call site (bench::test_2::query, bench::assign::assign_patient)
// spelled exactly as before.
#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::test_4 {
inline namespace BENCH_ARM_NS {

struct EnrichMetricsSummary {
  std::size_t source_bytes = 0;
  std::size_t enriched_bytes = 0;
  std::size_t appended_observations = 0;
  std::int64_t duration_ns = 0;
  // See MetricEvent::bytes_written / bytes_overwritten. Set per arm from what
  // its update model does to a STORED stream, not from buffer sizes alone.
  std::int64_t bytes_written = -1;
  std::int64_t bytes_overwritten = -1;
  // First source byte the update rewrote in place, beyond fixed metadata
  // (headers, footers, one re-pointed slot); -1 when it rewrote none.
  std::int64_t rewrite_from = -1;
};

template <typename StreamT>
struct EnrichResult {
  StreamT enriched_stream;
  EnrichMetricsSummary summary;
};

inline std::string format_enrich_summary(const EnrichMetricsSummary& summary) {
  return "source_bytes=" + std::to_string(summary.source_bytes) +
         " enriched_bytes=" + std::to_string(summary.enriched_bytes) +
         " appended_observations=" + std::to_string(summary.appended_observations) +
         " bytes_written=" + std::to_string(summary.bytes_written) +
         " bytes_overwritten=" + std::to_string(summary.bytes_overwritten) +
         " duration_ns=" + std::to_string(summary.duration_ns);
}

// EnrichMetricsSummary has carried source_bytes/enriched_bytes since the port
// and nothing ever read them -- they were formatted into a debug string and
// dropped. This overload is what puts them in the results (TASKS.md IN-0).
inline MetricEvent enrich_metric(std::string_view arm, const EnrichMetricsSummary& summary,
                                 Stage stage = Stage::Test4Enrich) {
  MetricEvent m{std::string(arm), stage, summary.duration_ns,
                static_cast<std::int64_t>(summary.source_bytes),
                static_cast<std::int64_t>(summary.enriched_bytes),
                /*ops=*/0,
                /*entries=*/static_cast<std::int64_t>(summary.appended_observations)};
  m.bytes_written = summary.bytes_written;
  m.bytes_overwritten = summary.bytes_overwritten;
  return m;
}

inline MetricEvent enrich_metric(std::string_view arm, std::int64_t duration_ns) {
  return MetricEvent{std::string(arm), Stage::Test4Enrich, duration_ns};
}

#if defined(ARM_FASTFHIR)

using StreamType = FastFHIR::Memory;

inline EnrichResult<StreamType> enrich_fastfhir(const StreamType& payload,
                                                const ObservationData& enrichment_observation) {
  // FastFHIR::Memory holds a shared_ptr<FF_Memory_t>, so the copy below shares
  // the arena: the append grows `payload` too. Read the source size FIRST or it
  // reports the post-append size and the appended-bytes delta is always 0.
  // (The in-place append is the feature -- WF-4.1 -- but the other three arms
  // build a separate buffer, so the two columns are not the same measurement.
  // Tracked as PA-9.)
  const std::size_t source_bytes_before = payload.view().size();
  StreamType enriched_stream = payload;
  // Opening a Builder on the sealed stream hydrates the root and rewinds the
  // write head onto the old checksum block (FastFHIR "Re-open for append").
  const FastFHIR::FF_Builder builder_handle = make_builder(enriched_stream, FHIR_VERSION_R5);
  FastFHIR::Builder& builder = *builder_handle;

  Timer timer;
  timer.start();

  auto root_handle = builder.root_handle();
  auto root_node = root_handle.as_node();
  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())) {
    throw std::runtime_error("test_4::enrich expected Bundle root in FastFHIR stream");
  }

#if BENCH_FF_HAS_APPEND1
  // APPEND-1 (PA-10c): read the light entry vector, roll the head back onto
  // the Bundle.entry array when it is the payload's tail (Test 1 builds it
  // there, PA-10a), write the Observation where it was, then the N+1 array
  // after it. The root Bundle stays where it is; only Bundle.entry is
  // re-pointed.
  FastFHIR::FF_BundleAppendResult append_result;
  const FF_Result appended = FastFHIR::FF_BundleAppendEntries(
      FastFHIR::FF_BundleAppendInfo{
          .builder = builder_handle,
          .append =
              [&](FastFHIR::Builder& b, std::vector<BundleentryData>& new_entries) {
                auto observation_handle = b.append_obj(ObservationData{});
                assign::assign_observation(enrichment_observation, observation_handle);
                new_entries.push_back(
                    BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)});
              },
      },
      append_result);
  if (!appended) {
    throw std::runtime_error("fastfhir arm enrich: " + appended.message);
  }
  (void)seal_stream(builder_handle, root_handle, "fastfhir arm enrich");
#else
  // Pre-APPEND-1 FastFHIR: the only public path re-serializes the bundle
  // root -- a new Bundle and a new (N+1) x 84 B entry array, the old array
  // left unreferenced (PA-10, CAPI-12).
  BundleData bundle = root_node.as<BundleData>();
  auto observation_handle = builder.append_obj(ObservationData{});
  assign::assign_observation(enrichment_observation, observation_handle);
  bundle.entry.push_back(BundleentryData{.resource = static_cast<ResourceReference>(observation_handle)});

  auto new_root = builder.append_obj(bundle);
  (void)seal_stream(builder_handle, new_root, "fastfhir arm enrich");
#endif
  const std::int64_t duration_ns = timer.stop_ns();

  EnrichMetricsSummary summary;
  summary.source_bytes = source_bytes_before;
  summary.enriched_bytes = enriched_stream.view().size();
  summary.appended_observations = 1;
  summary.duration_ns = duration_ns;
  // Every append rewrites two fixed regions: the stream header (the reseal
  // stamps it whole) and the old 44 B checksum block (the reopen rewinds onto
  // it). Everything else is either new -- past the old end -- or, under
  // APPEND-1, one of:
  //   tail path   the old entry array and its children, [rewrite_from, end),
  //               plus the 8 B Bundle.entry slot re-pointed at the new array;
  //   relocation  only that 8 B slot (the old array is left in place).
  // BENCH_VALIDATE checks this byte for byte.
  const auto header = static_cast<std::int64_t>(FF_HEADER::HEADER_SIZE);
  const auto footer = static_cast<std::int64_t>(FF_CHECKSUM::HEADER_SIZE);
  const auto source = static_cast<std::int64_t>(summary.source_bytes);
  std::int64_t in_place = footer;
#if BENCH_FF_HAS_APPEND1
  const std::int64_t entry_slot = sizeof(Offset);
  if (append_result.rewrite_from != FF_NULL_OFFSET) {
    summary.rewrite_from = static_cast<std::int64_t>(append_result.rewrite_from);
    in_place = source - summary.rewrite_from;  // covers the old footer too
  }
  summary.bytes_overwritten = header + entry_slot + in_place;
#else
  summary.bytes_overwritten = header + in_place;
#endif
  summary.bytes_written =
      static_cast<std::int64_t>(summary.enriched_bytes) - source + summary.bytes_overwritten;
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_JSON)

using StreamType = std::string;

inline EnrichResult<StreamType> enrich_json(const StreamType& payload,
                                            const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  nlohmann::json bundle = nlohmann::json::parse(payload);
  nlohmann::json observation;
  assign::assign_observation(enrichment_observation, observation);

  if (!bundle.contains("entry") || !bundle["entry"].is_array()) {
    bundle["entry"] = nlohmann::json::array();
  }
  bundle["entry"].push_back(nlohmann::json{{"resource", std::move(observation)}});

  StreamType enriched_stream = bundle.dump();
  const std::int64_t duration_ns = timer.stop_ns();

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = duration_ns;
  // A JSON document has no append point inside it: the stored file is
  // replaced by the new serialization, so every existing byte is rewritten.
  summary.bytes_written = static_cast<std::int64_t>(summary.enriched_bytes);
  summary.bytes_overwritten = static_cast<std::int64_t>(summary.source_bytes);
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_HL7V2)

using StreamType = std::string;

inline EnrichResult<StreamType> enrich_hl7v2(const StreamType& payload,
                                             const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  hl7v2::OruR01Message message;
  assign::assign_observation(enrichment_observation, message);

  StreamType enriched_stream = payload;
  enriched_stream += message.dump();
  const std::int64_t duration_ns = timer.stop_ns();

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = duration_ns;
  // A v2 batch is a message sequence: the new ORU^R01 appends to the stored
  // file and nothing before it changes. (The in-memory copy above is a
  // benchmark artifact inside the timer -- PA-10d.)
  summary.bytes_written = static_cast<std::int64_t>(summary.enriched_bytes - summary.source_bytes);
  summary.bytes_overwritten = 0;
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#elif defined(ARM_GOOGLE_FHIR)

using StreamType = std::string;

inline uint32_t read_u32_le(const std::string& in, std::size_t offset) {
  const auto b0 = static_cast<uint8_t>(in[offset + 0]);
  const auto b1 = static_cast<uint8_t>(in[offset + 1]);
  const auto b2 = static_cast<uint8_t>(in[offset + 2]);
  const auto b3 = static_cast<uint8_t>(in[offset + 3]);
  return static_cast<uint32_t>(b0) |
         (static_cast<uint32_t>(b1) << 8) |
         (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

inline std::string first_patient_id(const std::string& payload) {
  std::size_t cursor = 0;
  while (cursor + 5 <= payload.size()) {
    const char record_type = payload[cursor];
    const uint32_t record_size = read_u32_le(payload, cursor + 1);
    cursor += 5;

    if (cursor + record_size > payload.size()) {
      break;
    }

    if (record_type == 'P') {
      google::fhir::r4::core::Patient patient;
      if (patient.ParseFromArray(payload.data() + cursor, static_cast<int>(record_size)) &&
          patient.has_id()) {
        return patient.id().value();
      }
    }

    cursor += record_size;
  }
  return {};
}

inline void append_record(std::string& payload, char record_type, const std::string& record_bytes) {
  payload.push_back(record_type);
  const uint32_t len = static_cast<uint32_t>(record_bytes.size());
  payload.push_back(static_cast<char>(len & 0xFFu));
  payload.push_back(static_cast<char>((len >> 8) & 0xFFu));
  payload.push_back(static_cast<char>((len >> 16) & 0xFFu));
  payload.push_back(static_cast<char>((len >> 24) & 0xFFu));
  payload.append(record_bytes);
}

inline EnrichResult<StreamType> enrich_google_fhir(const StreamType& payload,
                                                   const ObservationData& enrichment_observation) {
  Timer timer;
  timer.start();

  // Materialize all protobuf records first, then mutate and re-serialize the full stream.
  std::vector<google::fhir::r4::core::Patient> patients;
  std::vector<google::fhir::r4::core::Observation> observations;
  std::vector<std::pair<char, std::size_t>> record_order;

  patients.reserve(256);
  observations.reserve(1024);
  record_order.reserve(2048);

  std::size_t cursor = 0;
  while (cursor + 5 <= payload.size()) {
    const char record_type = payload[cursor];
    const uint32_t record_size = read_u32_le(payload, cursor + 1);
    cursor += 5;

    if (cursor + record_size > payload.size()) {
      throw std::runtime_error("test_4::enrich_google_fhir encountered truncated protobuf record");
    }

    const char* record_ptr = payload.data() + cursor;
    if (record_type == 'P') {
      google::fhir::r4::core::Patient patient;
      if (!patient.ParseFromArray(record_ptr, static_cast<int>(record_size))) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to parse Patient record");
      }
      patients.push_back(std::move(patient));
      record_order.push_back({'P', patients.size() - 1});
    } else if (record_type == 'O') {
      google::fhir::r4::core::Observation observation;
      if (!observation.ParseFromArray(record_ptr, static_cast<int>(record_size))) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to parse Observation record");
      }
      observations.push_back(std::move(observation));
      record_order.push_back({'O', observations.size() - 1});
    } else {
      throw std::runtime_error("test_4::enrich_google_fhir encountered unknown protobuf record type");
    }

    cursor += record_size;
  }

  google::fhir::r4::core::Observation observation;
  std::string patient_id;
  for (const auto& patient : patients) {
    if (patient.has_id()) {
      patient_id = patient.id().value();
      break;
    }
  }
  const std::string fallback_patient_id =
      patient_id.empty() ? std::string("benchmark-enrich-patient") : patient_id;
  assign::GoogleObservationTarget target{observation, fallback_patient_id};
  assign::assign_observation(enrichment_observation, target);

  observations.push_back(std::move(observation));
  record_order.push_back({'O', observations.size() - 1});

  StreamType enriched_stream;
  enriched_stream.reserve(payload.size() + 512);
  for (const auto& [record_type, idx] : record_order) {
    std::string record_bytes;
    if (record_type == 'P') {
      if (!patients[idx].SerializeToString(&record_bytes)) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to serialize Patient record");
      }
    } else {
      if (!observations[idx].SerializeToString(&record_bytes)) {
        throw std::runtime_error("test_4::enrich_google_fhir failed to serialize Observation record");
      }
    }
    append_record(enriched_stream, record_type, record_bytes);
  }

  const std::int64_t duration_ns = timer.stop_ns();

  EnrichMetricsSummary summary;
  summary.source_bytes = payload.size();
  summary.enriched_bytes = enriched_stream.size();
  summary.appended_observations = 1;
  summary.duration_ns = duration_ns;
  // This arm re-serializes every record into a new stream (above), so the
  // stored stream is replaced whole, exactly as JSON's is.
  summary.bytes_written = static_cast<std::int64_t>(summary.enriched_bytes);
  summary.bytes_overwritten = static_cast<std::int64_t>(summary.source_bytes);
  return EnrichResult<StreamType>{std::move(enriched_stream), summary};
}

#endif

}  // inline namespace BENCH_ARM_NS
}  // namespace bench::test_4
