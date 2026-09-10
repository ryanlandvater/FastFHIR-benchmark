#include "harness.hpp"

#include "proto/google/fhir/proto/r4/core/codes.pb.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"

#include <cstdint>
#include <string>

#define ARM_GOOGLE_FHIR
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#include "bench_test_5.hpp"
#undef ARM_GOOGLE_FHIR

namespace bench {

namespace {

inline void append_u32_le(std::string& out, uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

inline void append_record(std::string& payload, char record_type, const std::string& record_bytes) {
  payload.push_back(record_type);
  append_u32_le(payload, static_cast<uint32_t>(record_bytes.size()));
  payload.append(record_bytes);
}

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
}

}  // namespace

ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

  std::string payload;
  payload.reserve(fixture.bundle.size() * 320);

  // Counted at the point of EMISSION, not of iteration: SerializeToString can
  // fail, and the two branches below already drop the resource when it does.
  // A count of loop trips would report resources this arm never wrote.
  std::int64_t test1_entries = 0;
  for (const auto& item : fixture.bundle) {
    google::fhir::r4::core::Patient patient;
    assign::GooglePatientTarget patient_target{patient};
    // This item's interned URLs, for the extension converters (which every
    // arm reaches -- v2's ZFX passthrough serializes JSON too).
    const assign::detail::ScopedUrlTable urls(item.url_table);
    assign::assign_patient(item.patient, patient_target);

    std::string patient_bytes;
    if (patient.SerializeToString(&patient_bytes)) {
      append_record(payload, 'P', patient_bytes);
      ++test1_entries;
    }

    for (const auto& observation : item.observations) {
      google::fhir::r4::core::Observation obs;
      assign::GoogleObservationTarget observation_target{obs, item.patient.id};
      assign::assign_observation(observation, observation_target);

      std::string obs_bytes;
      if (obs.SerializeToString(&obs_bytes)) {
        append_record(payload, 'O', obs_bytes);
        ++test1_entries;
      }
    }
  }

  const std::int64_t test1_ns = test1_timer.stop_ns();
  const std::int64_t test1_cpu_ns = test1_timer.cpu_ns();
  // Wire size is read AFTER the clock stops (notes.md section 6).
  const std::int64_t test1_bytes = static_cast<std::int64_t>(payload.size());
  out.metrics.push_back({"google_fhir", Stage::Test1Serialize, test1_ns, 0, test1_bytes,
                         /*ops=*/0, /*entries=*/test1_entries,
                         /*cpu_ns=*/test1_cpu_ns});
  out.test1_payload = payload;  // --dump-artifacts input
  // Leaves actually present in what this arm just wrote -- measured from the
  // OUTPUT, not from the fixture, so an arm that dropped fields reports fewer.
  if (bench::g_count_elements)
    out.test1_elements = bench::test_5::BENCH_ARM_NS::count_output_elements(payload);

  Timer test3_timer;
  test3_timer.start();
  const auto summary = test_3::query(payload);
  out.metrics.push_back({"google_fhir", Stage::Test3Query, test3_timer.stop_ns(),
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/test_3::query_entries(summary)});
  // SELECTIVE query -- see Stage::Test3Selective. Timed on its own so the
  // early-out cost is not hidden inside the census above.
  Timer test3s_timer;
  test3s_timer.start();
  const auto selective = test_3::query_selective(payload);
  const std::int64_t test3s_ns = test3s_timer.stop_ns();
  out.metrics.push_back({"google_fhir", Stage::Test3Selective, test3s_ns,
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/static_cast<std::int64_t>(selective.scanned)});
  out.selective_matches = static_cast<std::int64_t>(selective.matches);

  out.queried_value = test_3::format_query_summary(summary);
  out.query_loinc_matches =
      static_cast<std::int64_t>(summary.loinc_2085_9_matches);
  if (std::getenv("BENCH_TOUCHED")) {
    std::cerr << "[query] google_fhir " << out.queried_value << "\n";
  }
  out.reconstructed_bundle_json = "protobuf_payload_bytes=" + std::to_string(payload.size());

  // Test 2 -- random access (IN-B / WF-1.1). Out-of-order reads, navigating
  // from the root each time; the retired materialize walk read in layout
  // order, and the two disagree by three orders of magnitude.
  //
  // MUST run BEFORE Test 4. FastFHIR::Memory is a shared_ptr handle, so the
  // enrich appends into this very arena (PA-9) -- running Test 2 afterwards
  // had the FastFHIR arm reading 1,474 entries while the other arms read
  // 1,473, and the cross-arm byte gate caught it.
  {
    const auto ra = test_2::random_access(payload);
    out.metrics.push_back(test_2::random_access_metric("google_fhir", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("google_fhir", enrich_result.summary));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_google_fhir::structural_positions(wire).size();
}
}  // namespace
const ArmOps &arm_ops_google_fhir() {
  static const ArmOps ops{"google_fhir", &arm_google_fhir::calc_stream_hash,
                          &arm_google_fhir::corrupt_stream,
                          &arm_google_fhir::recover_stream, &count_positions};
  return ops;
}
}  // namespace bench::test_5
