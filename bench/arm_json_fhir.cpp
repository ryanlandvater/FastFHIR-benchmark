#include "harness.hpp"

#include <fstream>

#include <algorithm>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_JSON
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#include "bench_test_5.hpp"
#undef ARM_JSON

namespace bench {
namespace {

const ObservationData& enrichment_observation_fixture() {
  static const EnrichmentObservationFixture fixture =
      load_enrichment_observation_from_json("bench/enrich.json");
  return fixture.observation;
}

#if defined(__APPLE__)
struct JsonPatientBuildContext {
  const std::vector<BundlePatient>* bundle;
  std::vector<nlohmann::json>* entries;
};

// An observation is flattened out of its BundlePatient, which is where its
// URL table lives -- so the table has to travel with it or the index resolves
// against the wrong patient's trie.
struct ObservationWithUrls {
  const ObservationData* observation;
  const std::vector<std::string>* urls;
};

struct JsonObservationBuildContext {
  const std::vector<ObservationWithUrls>* observations;
  std::vector<nlohmann::json>* entries;
};

static inline void build_patient_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<JsonPatientBuildContext*>(raw_context);
  nlohmann::json patient_json;
  const assign::detail::ScopedUrlTable urls((*context->bundle)[idx].url_table);
  assign::assign_patient((*context->bundle)[idx].patient, patient_json);
  (*context->entries)[idx] = nlohmann::json{{"resource", std::move(patient_json)}};
}

static inline void build_observation_entry(void* raw_context, std::size_t idx) {
  auto* context = static_cast<JsonObservationBuildContext*>(raw_context);
  nlohmann::json observation_json;
  const auto& entry = (*context->observations)[idx];
  const assign::detail::ScopedUrlTable urls(*entry.urls);
  assign::assign_observation(*entry.observation, observation_json);
  (*context->entries)[idx] = nlohmann::json{{"resource", std::move(observation_json)}};
}
#endif

}  // namespace

ArmRunResult run_json_bundle(const BundleBenchFixture& fixture) {
  ArmRunResult out;

  Timer test1_timer;
  test1_timer.start();

  nlohmann::json bundle;
  bundle["resourceType"] = "Bundle";
  bundle["type"] = "collection";
  bundle["entry"] = nlohmann::json::array();

  std::vector<nlohmann::json> patient_entries(fixture.bundle.size());
#if defined(__APPLE__)
  if (!bench::g_serial_build) {
    JsonPatientBuildContext patient_context{&fixture.bundle, &patient_entries};
    dispatch_apply_f(patient_entries.size(),
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                     &patient_context,
                     build_patient_entry);
  } else
#endif
  {
  std::transform(
      fixture.bundle.begin(),
      fixture.bundle.end(),
      patient_entries.begin(),
      [](const BundlePatient& item) -> nlohmann::json {
        nlohmann::json patient_json;
        const assign::detail::ScopedUrlTable urls(item.url_table);
        assign::assign_patient(item.patient, patient_json);
        return nlohmann::json{{"resource", std::move(patient_json)}};
      });
  }

  std::size_t total_observations = 0;
  for (const auto& item : fixture.bundle) {
    total_observations += item.observations.size();
  }

  std::vector<ObservationWithUrls> observation_ptrs;
  observation_ptrs.reserve(total_observations);
  for (const auto& item : fixture.bundle) {
    for (const auto& observation : item.observations) {
      observation_ptrs.push_back({&observation, &item.url_table});
    }
  }

  std::vector<nlohmann::json> observation_entries(observation_ptrs.size());
#if defined(__APPLE__)
  if (!bench::g_serial_build) {
    JsonObservationBuildContext observation_context{&observation_ptrs, &observation_entries};
    dispatch_apply_f(observation_entries.size(),
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                     &observation_context,
                     build_observation_entry);
  } else
#endif
  {
  std::transform(
      observation_ptrs.begin(),
      observation_ptrs.end(),
      observation_entries.begin(),
      [](const ObservationWithUrls& entry) -> nlohmann::json {
        nlohmann::json observation_json;
        const assign::detail::ScopedUrlTable urls(*entry.urls);
        assign::assign_observation(*entry.observation, observation_json);
        return nlohmann::json{{"resource", std::move(observation_json)}};
      });
  }

  nlohmann::json::array_t all_entries;
  all_entries.reserve(patient_entries.size() + observation_entries.size());
  for (auto& entry : patient_entries) {
    all_entries.push_back(std::move(entry));
  }
  for (auto& entry : observation_entries) {
    all_entries.push_back(std::move(entry));
  }
  bundle["entry"] = std::move(all_entries);
  // Read from the BUNDLE, after assignment and immediately before dump() --
  // the entry count a reader of this document will see. Reading the staging
  // vector instead reports what the loop INTENDED to write, which is not the
  // same claim: a deliberate probe that dropped one entry between the count
  // and the serialization passed this gate and was caught only by Test 2 and
  // Test 4, which re-read the wire.
  const std::int64_t test1_entries = static_cast<std::int64_t>(bundle["entry"].size());

  const std::string payload = bundle.dump();
  const std::int64_t test1_ns = test1_timer.stop_ns();
  const std::int64_t test1_cpu_ns = test1_timer.cpu_ns();
  // Wire size is read AFTER the clock stops (notes.md section 6).
  const std::int64_t test1_bytes = static_cast<std::int64_t>(payload.size());
  out.metrics.push_back({"json_fhir", Stage::Test1Serialize, test1_ns, 0, test1_bytes,
                         /*ops=*/0, /*entries=*/test1_entries,
                         /*cpu_ns=*/test1_cpu_ns});
  out.test1_payload = payload;  // --dump-artifacts input
  // Leaves actually present in what this arm just wrote -- measured from the
  // OUTPUT, not from the fixture, so an arm that dropped fields reports fewer.
  if (bench::g_count_elements)
    out.test1_elements = bench::test_5::BENCH_ARM_NS::count_output_elements(payload);

  // A counter nobody reads is the silence it was added to prevent. ChoiceBlock
  // has 29 alternatives and this build has converters for 15; the rest are
  // skipped, and skipped-without-a-number is exactly how a raw arena offset in
  // a value slot survived unnoticed for so long.
  if (const std::size_t skipped = assign::detail::unrendered_choice_count()) {
    std::fprintf(stderr,
                 "WARNING: %zu block-typed choice value(s) had no JSON converter "
                 "and were omitted -- the arms are NOT encoding the same document.\n",
                 skipped);
  }

  if (const char* full = std::getenv("BENCH_DUMP_FULL"))
  {
    std::ofstream f(full);
    f << payload;
  }
  if (std::getenv("BENCH_DUMP_JSON"))
  {
    // Parity debugging: show what the effective/issued fields actually
    // serialized to. The Test 3 gate reads the same keys, so this is the
    // ground truth for the 692-vs-0 effectiveDateTime divergence.
    std::size_t shown = 0;
    for (std::size_t i = 0; i + 2 < payload.size() && shown < 6; ++i)
    {
      if (payload.compare(i, 9, "effective") == 0 || payload.compare(i, 6, "issued") == 0)
      {
        std::cerr << "[json-dump] ..." << payload.substr(i, 60) << "...\n";
        ++shown;
      }
    }
  }

  Timer test3_timer;
  test3_timer.start();
  const auto query_summary = test_3::query(payload);
  out.metrics.push_back({"json_fhir", Stage::Test3Query, test3_timer.stop_ns(),
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/test_3::query_entries(query_summary)});
  // SELECTIVE query -- see Stage::Test3Selective. Timed on its own so the
  // early-out cost is not hidden inside the census above.
  Timer test3s_timer;
  test3s_timer.start();
  const auto selective = test_3::query_selective(payload);
  const std::int64_t test3s_ns = test3s_timer.stop_ns();
  out.metrics.push_back({"json_fhir", Stage::Test3Selective, test3s_ns,
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
    out.metrics.push_back(test_2::random_access_metric("json_fhir", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload, enrichment_observation_fixture());
  out.metrics.push_back(test_4::enrich_metric("json_fhir", enrich_result.summary));
  out.enriched_stream = std::move(enrich_result.enriched_stream);
  out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);
  return out;
}

}  // namespace bench

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_json::structural_positions(wire).size();
}
}  // namespace
const ArmOps &arm_ops_json() {
  static const ArmOps ops{"json", &arm_json::calc_stream_hash,
                          &arm_json::corrupt_stream, &arm_json::recover_stream,
                          &count_positions};
  return ops;
}
}  // namespace bench::test_5
