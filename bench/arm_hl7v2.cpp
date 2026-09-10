#include "harness.hpp"
#include "hl7v2_message.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#define ARM_HL7V2
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#include "bench_test_5.hpp"
#undef ARM_HL7V2

namespace bench {
namespace {

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
}

}  // namespace

ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

  std::string payload;
  payload.reserve(fixture.bundle.size() * 512);

  std::int64_t test1_entries = 0;
  for (const auto& item : fixture.bundle) {
    hl7v2::OruR01Message message;
    // This item's interned URLs, for the extension converters (which every
    // arm reaches -- v2's ZFX passthrough serializes JSON too).
    const assign::detail::ScopedUrlTable urls(item.url_table);
    assign::assign_patient(item.patient, message);
    ++test1_entries;

    for (const auto& observation : item.observations) {
      assign::assign_observation(observation, message);
      ++test1_entries;
    }

    payload += message.dump();
  }

  const std::int64_t test1_ns = test1_timer.stop_ns();
  const std::int64_t test1_cpu_ns = test1_timer.cpu_ns();
  // Wire size is read AFTER the clock stops (notes.md section 6).
  const std::int64_t test1_bytes = static_cast<std::int64_t>(payload.size());
  out.metrics.push_back({"hl7v2", Stage::Test1Serialize, test1_ns, 0, test1_bytes,
                         /*ops=*/0, /*entries=*/test1_entries,
                         /*cpu_ns=*/test1_cpu_ns});
  out.test1_payload = payload;  // --dump-artifacts input
  // Leaves actually present in what this arm just wrote -- measured from the
  // OUTPUT, not from the fixture, so an arm that dropped fields reports fewer.
  if (bench::g_count_elements)
    out.test1_elements = bench::test_5::BENCH_ARM_NS::count_output_elements(payload);

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"hl7v2", Stage::Test3Query, test3_timer.stop_ns(),
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/test_3::query_entries(query_summary)});
  // SELECTIVE query -- see Stage::Test3Selective. Timed on its own so the
  // early-out cost is not hidden inside the census above.
  Timer test3s_timer;
  test3s_timer.start();
  const auto selective = test_3::query_selective(payload);
  const std::int64_t test3s_ns = test3s_timer.stop_ns();
  out.metrics.push_back({"hl7v2", Stage::Test3Selective, test3s_ns,
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/static_cast<std::int64_t>(selective.scanned)});
  out.selective_matches = static_cast<std::int64_t>(selective.matches);
  out.queried_value = test_3::format_query_summary(query_summary);
  out.query_loinc_matches =
      static_cast<std::int64_t>(query_summary.loinc_2085_9_matches);
  out.reconstructed_bundle_json = payload;

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
    out.metrics.push_back(test_2::random_access_metric("hl7v2", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("hl7v2", enrich_result.summary));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

  return out;
}

}  // namespace bench

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_hl7v2::structural_positions(wire).size();
}
}  // namespace
const ArmOps &arm_ops_hl7v2() {
  static const ArmOps ops{"hl7v2", &arm_hl7v2::calc_stream_hash,
                          &arm_hl7v2::corrupt_stream, &arm_hl7v2::recover_stream,
                          &count_positions};
  return ops;
}
}  // namespace bench::test_5
