// FastFHIR Benchmark Harness
// Supports macOS (all 4 arms) and Windows (FFHR + JSON arms).
// Platform differences gated by __APPLE__ and HAVE_GOOGLE_FHIR macros.

#include <thread>
#include <FF_Concurrency.hpp>
#include "harness.hpp"
#include "poco_leaves.hpp"
#include "poco_set.hpp"
#include "provenance.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Synthea data path: macOS uses a hardcoded primary path; other platforms
// use a relative datasets/ directory.
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
static fs::path find_synthea_dir()
{
  const fs::path primary = "/Users/RyanLandvater/Programming_Projects/FastFHIR-benchmarking/datasets/synthea";
  const fs::path fallback = "datasets/synthea";
  if (fs::exists(primary))
    return primary;
  if (fs::exists(fallback))
    return fallback;
  return {};
}
#else
static fs::path find_synthea_dir()
{
  const fs::path dir = "datasets/synthea";
  if (fs::exists(dir))
    return dir;
  return {};
}
#endif

// ---------------------------------------------------------------------------
// Warmup wrapper — runs only the arms available on this platform.
// ---------------------------------------------------------------------------
static void warmup_arms(bench::BundleBenchFixture &fixture, int warmup_iterations)
{
  for (int w = 0; w < warmup_iterations; ++w)
  {
    (void)bench::run_fastfhir_bundle(fixture);
    (void)bench::run_json_bundle(fixture);
#if defined(HAVE_HL7V2)
    (void)bench::run_hl7v2_bundle(fixture);
#endif
#if defined(HAVE_GOOGLE_FHIR)
    (void)bench::run_google_fhir_bundle(fixture);
#endif
  }
}

// ---------------------------------------------------------------------------
// Per-arm run helpers — each returns its metrics vector.
// ---------------------------------------------------------------------------
static std::vector<bench::MetricEvent> run_fastfhir(bench::BundleBenchFixture &bundle)
{
  return bench::run_fastfhir_bundle(bundle).metrics;
}
static std::vector<bench::MetricEvent> run_json(bench::BundleBenchFixture &bundle)
{
  return bench::run_json_bundle(bundle).metrics;
}
#if defined(HAVE_HL7V2)
static std::vector<bench::MetricEvent> run_hl7v2(bench::BundleBenchFixture &bundle)
{
  return bench::run_hl7v2_bundle(bundle).metrics;
}
#endif
#if defined(HAVE_GOOGLE_FHIR)
static std::vector<bench::MetricEvent> run_google_fhir(bench::BundleBenchFixture &bundle)
{
  return bench::run_google_fhir_bundle(bundle).metrics;
}
#endif

// ---------------------------------------------------------------------------
// Cross-arm validation (only when all arms are available)
// ---------------------------------------------------------------------------
#if defined(HAVE_GOOGLE_FHIR)
static bool validate_parity(const bench::ArmRunResult &ff,
                            const bench::ArmRunResult &jf,
                            const bench::ArmRunResult &h2,
                            bool &failed)
{
  if (!bench::validate_results(ff, jf))
  {
    failed = true;
    std::cerr << "  [validate] values: fastfhir=[" << ff.queried_value << "]"
              << " json=[" << jf.queried_value << "]\n";
  }
  if (!bench::validate_hl7_results(ff, jf, h2))
  {
    failed = true;
    std::cerr << "  [validate-hl7] values: baseline_json=[" << jf.queried_value << "]"
              << " hl7=[" << h2.queried_value << "]\n";
  }
  return failed;
}
#endif

// ===========================================================================

int main(int argc, char **argv)
{
  const std::vector<std::string_view> args(argv, argv + argc);

  // Defaults
  // TWO AXES, NAMED THE SAME WAY IN BOTH BENCHMARKS (scripts/recovery_sweep.py
  // uses REPLICATES/`replicate` for its damage draws).
  //
  //   replicate = one independent draw of the randomized INPUT. Here that is a
  //               bundle composition; in bench 5 it is a damage pattern.
  //   run       = one timed repetition over a replicate whose input is FIXED.
  //
  // They were previously collapsed into one loop that drew a fresh bundle every
  // iteration, so run-to-run spread was workload variance and measurement noise
  // added together and neither could be reported. Measured on the 2026-09-05
  // corpus: raw duration CV 70-130% per arm, because the fill loop needs
  // anywhere from 9 to 22 Synthea patients to reach the same nominal 64 MB.
  int num_runs = 3;
  int warmup_iterations = 1;
  int num_replicates = 20;
  int64_t bundle_max_mb = 0;
  bool bundle_max_mb_explicit = false;
  int64_t fastfhir_vma_mb = 0;
  std::string db_connstr;
  // IN-0: where the release artifact goes, and the escape hatch for a profile
  // the CMake caches cannot settle. Without --results-dir the harness still
  // prints provenance to stderr; it just does not claim to be an artifact.
  std::string results_dir;
  std::string artifacts_dir;  // --dump-artifacts: corruption-probe inputs
  std::string profile_override;
  // Bundle composition was seeded from std::random_device, so two runs of the
  // same command measured different bundles and a corruption bug reproduced
  // only intermittently. Default to a fixed seed; --seed 0 restores random.
  unsigned int rng_seed = 20260825u;
  std::vector<int64_t> target_sizes_bytes = {
      1LL * 1024 * 1024,
      2LL * 1024 * 1024,
      4LL * 1024 * 1024,
      8LL * 1024 * 1024,
      16LL * 1024 * 1024,
      32LL * 1024 * 1024,
      64LL * 1024 * 1024,
      256LL * 1024 * 1024,
  };

  for (std::size_t i = 1; i < args.size(); ++i)
  {
    if (args[i] == "--iterations" && i + 1 < args.size())
    {
      // Deprecated alias: it nested a second fresh-bundle loop inside --runs,
      // which is the confound the replicate/run split removes. Folded onto
      // --runs so old invocations still mean "measure more times".
      num_runs = std::max(1, std::atoi(args[i + 1].data()));
      std::cerr << "[warn] --iterations is deprecated; treating it as --runs "
                   "(timed repetitions per replicate). Use --replicates for "
                   "distinct bundle draws.\n";
    }
    else if (args[i] == "--warmup-iterations" && i + 1 < args.size())
    {
      warmup_iterations = std::max(0, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--runs" && i + 1 < args.size())
    {
      // NOTE: this changed meaning on 2026-09-05. It used to be the number of
      // bundle draws; it is now timed repetitions over one fixed bundle. Use
      // --replicates for the old axis. The banner prints both so an old script
      // cannot be misread as the old semantics.
      num_runs = std::max(1, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--replicates" && i + 1 < args.size())
    {
      num_replicates = std::max(1, std::atoi(args[i + 1].data()));
    }
    else if (args[i] == "--bundle-max-mb" && i + 1 < args.size())
    {
      bundle_max_mb = std::max<int64_t>(1, std::atoll(args[i + 1].data()));
      bundle_max_mb_explicit = true;
    }
    else if (args[i] == "--ff-vma-mb" && i + 1 < args.size())
    {
      fastfhir_vma_mb = std::max<int64_t>(0, std::atoll(args[i + 1].data()));
    }
    else if (args[i] == "--db" && i + 1 < args.size())
    {
      db_connstr = std::string(args[i + 1]);
    }
    else if (args[i] == "--results-dir" && i + 1 < args.size())
    {
      results_dir = std::string(args[i + 1]);
    }
    else if (args[i] == "--dump-artifacts" && i + 1 < args.size())
    {
      // Instrument G test 5 (recovery comparison): write one representative
      // bundle's Test-1 wire payload per arm for the corruption probe.
      artifacts_dir = std::string(args[i + 1]);
    }
    else if (args[i] == "--profile" && i + 1 < args.size())
    {
      profile_override = std::string(args[i + 1]);
    }
    else if (args[i] == "--seed" && i + 1 < args.size())
    {
      rng_seed = static_cast<unsigned int>(std::strtoul(args[i + 1].data(), nullptr, 10));
    }
    else if (args[i] == "--serial-build")
    {
      // One thread for every arm's Test 1 construction loop. See
      // harness.hpp g_serial_build: the default (parallel) run compares
      // formats at their best; this run compares per-resource encoding cost
      // with thread count held at one.
      bench::g_serial_build = true;
    }
    else if (args[i] == "--bundle-targets-mb" && i + 1 < args.size())
    {
      target_sizes_bytes.clear();
      std::stringstream ss{std::string(args[i + 1])};
      std::string token;
      while (std::getline(ss, token, ','))
      {
        const auto mb = std::atoll(token.c_str());
        if (mb > 0)
          target_sizes_bytes.push_back(mb * 1024 * 1024);
      }
    }
  }

  // Cap, sort, deduplicate targets
  if (bundle_max_mb_explicit)
  {
    target_sizes_bytes.erase(
        std::remove_if(target_sizes_bytes.begin(), target_sizes_bytes.end(),
                       [bundle_max_mb](int64_t b)
                       { return b > bundle_max_mb * 1024 * 1024; }),
        target_sizes_bytes.end());
  }
  std::sort(target_sizes_bytes.begin(), target_sizes_bytes.end());
  target_sizes_bytes.erase(
      std::unique(target_sizes_bytes.begin(), target_sizes_bytes.end()),
      target_sizes_bytes.end());

  if (target_sizes_bytes.empty())
  {
    std::cerr << "No target sizes configured.\n";
    return 1;
  }

  // Locate Synthea data
  const fs::path synthea_dir = find_synthea_dir();
  if (synthea_dir.empty())
  {
    std::cerr << "Synthea data not found. Place FHIR JSON files in datasets/synthea/\n";
    return 1;
  }

  // Pre-load all patient JSON files
  struct IngestedPatient
  {
    bench::BundlePatient patient;
  };
  std::vector<IngestedPatient> all_patients;

  std::cerr << "Ingesting Synthea JSON files from " << synthea_dir << " ...\n";
  for (const auto &entry : fs::directory_iterator(synthea_dir))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    try
    {
      auto patient = bench::make_bundle_patient_from_json(entry.path());
      all_patients.push_back({std::move(patient)});
    }
    catch (const std::exception &ex)
    {
      std::cerr << "  skip " << entry.path().filename() << ": " << ex.what() << "\n";
    }
  }

  if (all_patients.empty())
  {
    std::cerr << "No patient JSON files found in " << synthea_dir << ".\n";
    return 1;
  }
  std::cerr << "Loaded " << all_patients.size() << " patients.\n";
  // Which configuration produced these numbers. Two arms build in parallel by
  // default and two never do, so a timing table read without this line cannot
  // be placed on either curve.
  // Which configuration produced these numbers, and how many cores it was
  // allowed. A timing table read without this line cannot be placed on either
  // curve. The cpu_ns column carries the measured answer per row; this is the
  // requested configuration.
  std::cerr << "Test 1 construction: "
            << (bench::g_serial_build
                    ? "SERIAL (--serial-build) -- every arm one thread"
                    : "PARALLEL -- fastfhir builds through FIFO::Queue workers, json_fhir"
                      " dispatches per resource; hl7v2 + google_fhir are serial either way")
            << "\n  hardware_concurrency=" << std::thread::hardware_concurrency()
            << ", performance cores=" << FastFHIR::performance_core_count()
            << " (the pool default)";
  if (const char *t = std::getenv("BENCH_FF_THREADS"))
    std::cerr << ", BENCH_FF_THREADS=" << t;
  std::cerr << "\n\n";

  std::mt19937 rng(rng_seed != 0 ? rng_seed : std::random_device{}());
  std::cerr << "Bundle composition seed: "
            << (rng_seed != 0 ? std::to_string(rng_seed) : std::string("random")) << "\n";
  std::uniform_int_distribution<std::size_t> patient_dist(0, all_patients.size() - 1);

  if (!artifacts_dir.empty())
  {
    // One representative bundle, one run per arm, wire payloads to files --
    // the corruption probe (Instrument G test 5 comparison) consumes these.
    bench::BundleBenchFixture bundle{};
    bundle.target_size_bytes = 16LL * 1024 * 1024;
    int64_t accumulated = 0;
    while (accumulated < bundle.target_size_bytes)
    {
      const auto &p = all_patients[patient_dist(rng)];
      bundle.bundle.push_back(bench::clone_bundle_patient(p.patient));
      accumulated += p.patient.memory.size();
    }
    bundle.actual_ingested_bytes = accumulated;
    const auto ff = bench::run_fastfhir_bundle(bundle);
    const auto jf = bench::run_json_bundle(bundle);
#if defined(HAVE_HL7V2)
    const auto h2 = bench::run_hl7v2_bundle(bundle);
#endif
#if defined(HAVE_GOOGLE_FHIR)
    const auto gf = bench::run_google_fhir_bundle(bundle);
#endif
    // REPORT WHAT WAS WRITTEN, NOT WHAT WAS INTENDED.
    //
    // This printed "wrote <name> (N bytes)" from bytes.size() without ever
    // testing the stream. An artifacts directory that does not exist makes the
    // ofstream fail to open, the write a no-op, and the message a lie -- the
    // run reports four artifacts and produces none, and every downstream stage
    // then reads whatever stale files were there before.
    //
    // Loud, not silent: a corruption sweep run against artifacts that were
    // never written measures the previous run.
    auto write = [&](const char *name, const std::string &bytes)
    {
      const std::string path = std::string(artifacts_dir) + "/" + name;
      std::ofstream ofs(path, std::ios::binary);
      if (!ofs)
      {
        std::cerr << "[artifacts] FAILED to open " << path
                  << " -- does the directory exist?\n";
        std::exit(4);
      }
      ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      ofs.flush();
      if (!ofs)
      {
        std::cerr << "[artifacts] FAILED writing " << path << "\n";
        std::exit(4);
      }
      std::cerr << "[artifacts] wrote " << name << " (" << bytes.size() << " bytes)\n";
    };
    write("fastfhir.bin", ff.test1_payload);
    write("json.bin", jf.test1_payload);
#if defined(HAVE_HL7V2)
    write("hl7v2.bin", h2.test1_payload);
#endif
#if defined(HAVE_GOOGLE_FHIR)
    write("google_fhir.bin", gf.test1_payload);
#endif
    // POCO 1 -- the model every arm was HANDED, and the neutral reference the
    // arms are measured against. Written beside the wires so the comparison
    // never has to nominate one branch's output as the truth.
    {
        std::vector<bench::poco::Leaf> truth;
        for (const auto& item : bundle.bundle) {
            auto ls = bench::poco::leaves(item);
            truth.insert(truth.end(), ls.begin(), ls.end());
        }
        std::sort(truth.begin(), truth.end());
        std::ostringstream os;
        for (const auto& [path, value] : truth) os << path << '\t' << value << '\n';
        write("poco1.leaves", os.str());
        std::cerr << "[artifacts] POCO 1: " << truth.size() << " leaves (neutral reference)\n";

        // SELF-TEST of the inverse mechanism, before any arm relies on it.
        // Feeding POCO 1's own leaves back through set_path must rebuild POCO 1:
        // if that is not identity, every arm's decoder is measuring the setter's
        // gaps rather than the format's.
        std::size_t rebuilt = 0, refused = 0;
        std::vector<bench::poco::Leaf> back;
        {
            std::map<std::string, PatientData> patients;
            std::map<std::string, ObservationData> observations;
            for (const auto& [path, value] : truth) {
                const auto dot = path.find('.');
                if (dot == std::string::npos) continue;
                const std::string key = path.substr(0, dot);
                const std::string rest = path.substr(dot + 1);
                nlohmann::json v;
                try { v = nlohmann::json::parse(value); } catch (const std::exception&) { continue; }
                const bool ok = key.rfind("Patient/", 0) == 0
                                    ? bench::poco::set_path(patients[key], rest, v)
                                    : bench::poco::set_path(observations[key], rest, v);
                if (ok) { ++rebuilt; }
                else {
                    if (refused < 8) std::cerr << "[selftest] refused: " << path << "\n";
                    ++refused;
                }
            }
            for (auto& [k, d] : patients) bench::poco::walk(d, k, back);
            for (auto& [k, d] : observations) bench::poco::walk(d, k, back);
            std::sort(back.begin(), back.end());
        }
        {
            std::ostringstream bs;
            for (const auto& [path, value] : back) bs << path << '\t' << value << '\n';
            write("poco1_rebuilt.leaves", bs.str());
        }
        std::cerr << "[selftest] set_path rebuilt " << rebuilt << " leaves, refused "
                  << refused << "; re-walk yields " << back.size() << " (want "
                  << truth.size() << ")\n";
    }
    std::cerr << "Artifact dump complete.\n";
    return 0;
  }

  // -----------------------------------------------------------------------
  // Provenance (TASKS.md IN-0)
  // -----------------------------------------------------------------------
  // Collected before the first measurement so the record describes the build
  // that produced the rows below it, and printed on every run -- artifact or
  // not -- because the profile and compilation mode are invisible otherwise.
  bench::provenance::Options prov_opts;
  prov_opts.profile_override = profile_override;
  prov_opts.corpus_dir = synthea_dir;
  prov_opts.seed = rng_seed;
  const bench::provenance::Provenance prov = bench::provenance::collect(prov_opts);
  std::cerr << "\n" << bench::provenance::to_summary(prov);

  if (!results_dir.empty())
  {
    if (!bench::provenance::write_json(prov, fs::path(results_dir), std::cerr))
      return 3;
  }
  else
  {
    const auto missing = bench::provenance::missing_fields(prov);
    if (!missing.empty())
    {
      std::cerr << "[provenance] " << missing.size()
                << " field(s) unestablished -- these numbers cannot become an artifact:\n";
      for (const auto &m : missing)
        std::cerr << "[provenance]   - " << m << "\n";
    }
  }
  std::cerr << "\n";

  std::cout << "arm,test,duration_ns,ops,bytes_in,bytes_out,target_mb,patients_in_bundle,cpu_ns,"
               "replicate,run\n"
            << std::flush;

  // -----------------------------------------------------------------------
  // PostgreSQL connection
  // -----------------------------------------------------------------------
#ifdef HAVE_LIBPQ
  PGconn *db_conn = nullptr;
  int run_id = -1;
  if (!db_connstr.empty())
  {
    db_conn = PQconnectdb(db_connstr.c_str());
    if (PQstatus(db_conn) != CONNECTION_OK)
    {
      std::cerr << "Database connection failed: " << PQerrorMessage(db_conn) << "\n";
      PQfinish(db_conn);
      db_conn = nullptr;
    }
    else
    {
      PGresult *r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_ns BIGINT");
      PQclear(r);
      r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS duration_us BIGINT");
      PQclear(r);
      // IN-0: wire bytes, so a size or throughput claim has somewhere to live.
      r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS bytes_in BIGINT");
      PQclear(r);
      r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS bytes_out BIGINT");
      PQclear(r);
      r = PQexec(db_conn, "ALTER TABLE benchmark_results ADD COLUMN IF NOT EXISTS ops BIGINT");
      PQclear(r);

#if defined(__APPLE__)
      const char *host_label = "macOS";
#else
      const char *host_label = "Windows";
#endif
      // The run record carries provenance because a results row is meaningless
      // without it: same query, different profile or compilation mode, different
      // numbers, no visible difference in the table (TASKS.md IN-0, PR-1).
      for (const char *ddl : {
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS fastfhir_sha TEXT",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS fastfhir_dirty BOOLEAN",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS production_profile TEXT",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS compilation_mode TEXT",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS corpus_sha256 TEXT",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS benchmark_sha TEXT",
               "ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS seed BIGINT",
           })
      {
        PGresult *ddl_r = PQexec(db_conn, ddl);
        PQclear(ddl_r);
      }

      // Named locals: every one of these must outlive the PQexecParams call,
      // so no std::to_string(...).c_str() temporaries here.
      const std::string iterations_s = std::to_string(iterations);
      const std::string seed_s = std::to_string(prov.seed);
      const char *run_params[9] = {
          host_label,
          iterations_s.c_str(),
          prov.fastfhir_sha.c_str(),
          prov.fastfhir_dirty ? "true" : "false",
          prov.production_profile.c_str(),
          prov.compilation_mode.c_str(),
          prov.corpus_sha256.c_str(),
          prov.benchmark_sha.c_str(),
          seed_s.c_str(),
      };
      r = PQexecParams(db_conn,
                       "INSERT INTO benchmark_runs "
                       "(hostname, iterations, notes, fastfhir_sha, fastfhir_dirty, "
                       "production_profile, compilation_mode, corpus_sha256, benchmark_sha, seed) "
                       "VALUES ($1::text, $2::int, 'benchmark harness', $3::text, $4::boolean, "
                       "$5::text, $6::text, $7::text, $8::text, $9::bigint) RETURNING id",
                       9, nullptr, run_params, nullptr, nullptr, 0);
      if (PQresultStatus(r) == PGRES_TUPLES_OK)
      {
        run_id = std::atoi(PQgetvalue(r, 0, 0));
        std::cerr << "Database run_id: " << run_id << "\n";
      }
      else
      {
        std::cerr << "Failed to insert run record: " << PQerrorMessage(db_conn) << "\n";
      }
      PQclear(r);
    }
  }

  // Parameterized DB insert helper
  auto insert_metric = [&](const bench::MetricEvent &m, int64_t target_mb, int64_t n_patients)
  {
    if (!(db_conn && run_id >= 0))
      return;
    const std::string run_id_s = std::to_string(run_id);
    const std::string dur_ns_s = std::to_string(m.duration_ns);
    const std::string dur_us_s = std::to_string((m.duration_ns + 999) / 1000);
    const std::string ops_s = std::to_string(m.ops);
    const std::string bytes_in_s = std::to_string(m.bytes_in);
    const std::string bytes_out_s = std::to_string(m.bytes_out);
    const std::string target_s = std::to_string(target_mb);
    const std::string patients_s = std::to_string(n_patients);
    const std::string stage_s = bench::to_string(m.stage);

    const char *params[10] = {
        run_id_s.c_str(),
        m.arm.c_str(),
        stage_s.c_str(),
        dur_ns_s.c_str(),
        dur_us_s.c_str(),
        ops_s.c_str(),
        bytes_in_s.c_str(),
        bytes_out_s.c_str(),
        target_s.c_str(),
        patients_s.c_str(),
    };
    PGresult *r = PQexecParams(db_conn,
                               "INSERT INTO benchmark_results "
                               "(run_id, arm, stage, duration_ns, duration_us, ops, bytes_in, "
                               "bytes_out, target_mb, patients_in_bundle) "
                               "VALUES ($1::int, $2::text, $3::text, $4::bigint, $5::bigint, "
                               "$6::bigint, $7::bigint, $8::bigint, $9::int, $10::int)",
                               10, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      std::cerr << "DB insert failed: " << PQerrorMessage(db_conn) << "\n";
    PQclear(r);
  };
#else
  // Stub — built without libpq (the default //bench:bench_harness target).
  // PGconn is a libpq type, so it cannot even be *named* here; the previous
  // `PGconn *db_conn = nullptr;` only ever compiled because HAVE_LIBPQ was
  // always defined. Build //bench:bench_harness_pg for --db support.
  if (!db_connstr.empty())
  {
    std::cerr << "Warning: --db was given but this binary was built without libpq; "
                 "metrics will be written to stdout only. "
                 "Use //bench:bench_harness_pg for PostgreSQL persistence.\n";
  }
  auto insert_metric = [](const bench::MetricEvent &, int64_t, int64_t) {};
#endif

  // Emit CSV regardless of DB availability
  auto emit_metric = [&](const bench::MetricEvent &m, int64_t target_mb, int64_t n_patients,
                         int replicate, int run_index)
  {
    // A serialize stage that produced no wire bytes did not serialize anything.
    // 0 is reserved for "not applicable to this stage" (harness.hpp), so this
    // is a real defect rather than a fast number -- the same class as the Test 2
    // walk that reported 83 ns for one node (notes.md section 2).
    if (m.stage == bench::Stage::Test1Serialize && m.bytes_out <= 0)
    {
      std::cerr << "[warn] " << m.arm << " test_1_serialize reported 0 wire bytes\n";
    }
    std::cout << m.arm << "," << bench::to_string(m.stage) << "," << m.duration_ns
              << "," << m.ops << "," << m.bytes_in << "," << m.bytes_out
              << "," << target_mb << "," << n_patients << "," << m.cpu_ns
              << "," << replicate << "," << run_index << "\n"
              << std::flush;
  };

  // -----------------------------------------------------------------------
  // Main benchmark loop
  // -----------------------------------------------------------------------
  bool validation_failed = false;

  for (const int64_t target_bytes : target_sizes_bytes)
  {
    const int64_t target_mb = target_bytes / (1024 * 1024);
    std::cerr << "=== " << target_mb << " MB (" << num_replicates
              << " replicates x " << num_runs << " runs, " << warmup_iterations
              << " warmups) ===\n";

    for (int replicate = 0; replicate < num_replicates; ++replicate)
    {
      // ONE bundle per replicate, drawn from a seed derived from
      // (base seed, target size, replicate index) rather than from a single
      // stream shared by the whole sweep. Two consequences, both load-bearing:
      //
      //  - Replicate r at a given size is the SAME bundle regardless of
      //    --runs, --replicates, or which sizes ran before it. Under the old
      //    single-stream RNG, changing --runs changed every bundle drawn
      //    afterwards, so two invocations were not comparable at all.
      //  - The bundle is fixed for the whole replicate, so the run loop below
      //    measures the engine and nothing else.
      std::seed_seq seq{static_cast<uint32_t>(rng_seed),
                        static_cast<uint32_t>(target_bytes & 0xFFFFFFFF),
                        static_cast<uint32_t>((target_bytes >> 32) & 0xFFFFFFFF),
                        static_cast<uint32_t>(replicate)};
      std::mt19937 rep_rng(seq);

      bench::BundleBenchFixture bundle{};
      bundle.target_size_bytes = target_bytes;
      bundle.fastfhir_vma_bytes = fastfhir_vma_mb > 0 ? fastfhir_vma_mb * 1024 * 1024 : 0;

      int64_t accumulated = 0;
      while (accumulated < target_bytes)
      {
        const auto &p = all_patients[patient_dist(rep_rng)];
        bundle.bundle.push_back(bench::clone_bundle_patient(p.patient));
        accumulated += p.patient.memory.size();
      }
      bundle.actual_ingested_bytes = accumulated;
      const int64_t n_patients = static_cast<int64_t>(bundle.bundle.size());

      if (replicate == 0)
      {
        std::cerr << "  Typical bundle: " << n_patients << " patients, "
                  << (accumulated / (1024 * 1024)) << " MB ingested FFHR\n";
      }

      // Warmup once per replicate, not once per run: the run loop below is
      // repeated measurement of an already-warm fixture.
      warmup_arms(bundle, warmup_iterations);

      for (int run_index = 0; run_index < num_runs; ++run_index)
      {
        // Element counting re-walks each arm's whole output, so ask for it
        // once per bundle size rather than on every timed run. It happens after
        // each stage's clock stops and so cannot affect a measurement.
        bench::g_count_elements = (replicate == 0 && run_index == 0);

        // Timed runs — all available arms
        const auto ff = bench::run_fastfhir_bundle(bundle);
        const auto jf = bench::run_json_bundle(bundle);
#if defined(HAVE_HL7V2)
        const auto h2 = bench::run_hl7v2_bundle(bundle);
#endif
#if defined(HAVE_GOOGLE_FHIR)
        const auto gf = bench::run_google_fhir_bundle(bundle);
#endif

        // Cross-arm validation when all arms present
#if defined(HAVE_GOOGLE_FHIR)
        validate_parity(ff, jf, h2, validation_failed);
#endif

        // Collect and emit all metrics
        //
        // CROSS-ARM PARITY GATES. Every arm is handed the same fixture, so at
        // every stage every arm must have touched the same content. An arm that
        // silently handles less looks FASTER, and no duration or byte count
        // contradicts it -- a smaller payload reads as a more compact format,
        // not as a lossy one. Both defects this suite has actually shipped had
        // that shape: the HL7v2 probe (wrong segment terminator, then wrong
        // field index) and, later, an arm writing a constant for every
        // observation value. Cheap to check, and it converts a fast meaningless
        // number into a hard failure.
        {
          // bench::to_string(Stage), never a private list. A hand-kept parallel
          // table drifts the moment the enum grows, and this one did
          // immediately: Stage has EIGHT values (each test has a _compact
          // twin) against a five-name array, so Test3Query printed under
          // "test_4 enrich" and Test4Enrich fell off the end as "stage". The
          // numbers were right and the labels lied, which is the worse failure
          // -- to_string is exhaustive over the enum and cannot drift.
          const auto stage_label = [](bench::Stage st) { return bench::to_string(st); };

          struct Named { const char *name; const bench::ArmRunResult *run; };
          const std::vector<Named> arms = {
              {"fastfhir", &ff},
              {"json_fhir", &jf},
#if defined(HAVE_HL7V2)
              {"hl7v2", &h2},
#endif
#if defined(HAVE_GOOGLE_FHIR)
              {"google_fhir", &gf},
#endif
          };

          // SHOW THE COUNTS, do not merely gate on them. A gate that passes
          // silently is indistinguishable from a gate that is not running, and
          // this one is asserting the central claim of the whole suite: that
          // the four arms handled the same content. Print it every run.
          const auto entries_of = [](const bench::ArmRunResult &r,
                                     bench::Stage st) -> std::int64_t {
            for (const auto &m : r.metrics)
              if (m.stage == st) return m.entries;
            return -1;
          };
          {
            std::cerr << "  [parity] resources handled per stage\n" << std::setw(24) << "";
            for (const auto &a : arms) std::cerr << std::setw(14) << a.name;
            std::cerr << "\n";
            for (const auto st : {bench::Stage::Test1Serialize,
                                  bench::Stage::Test2RandomAccess,
                                  bench::Stage::Test3Query,
                                  bench::Stage::Test4Enrich}) {
              std::cerr << "  " << std::setw(22) << std::left << stage_label(st)
                        << std::right;
              for (const auto &a : arms) {
                const std::int64_t n = entries_of(*a.run, st);
                if (n < 0) std::cerr << std::setw(14) << "-";
                else       std::cerr << std::setw(14) << n;
              }
              std::cerr << "\n";
            }
            std::cerr << "  " << std::setw(22) << std::left
                      << "test_3 chol 2085-9" << std::right;
            for (const auto &a : arms)
              std::cerr << std::setw(14) << a.run->query_loinc_matches;
            std::cerr << "\n";
            std::cerr << "  " << std::setw(22) << std::left
                      << "test_3 SELECTIVE chol" << std::right;
            for (const auto &a : arms)
              std::cerr << std::setw(14) << a.run->selective_matches;
            std::cerr << "\n";
            if (arms.front().run->test1_elements >= 0) {
              std::cerr << "  " << std::setw(22) << std::left
                        << "test_1 ELEMENTS" << std::right;
              for (const auto &a : arms)
                std::cerr << std::setw(14) << a.run->test1_elements;
              std::cerr << "\n";
            }
          }

          // SELECTIVE-QUERY PARITY. The whole point of Stage::Test3Selective is
          // that an arm may skip work -- so the one thing that must be checked
          // is that skipping did not skip the ANSWER. An arm that returns fast
          // because it matched nothing is the failure mode, and a duration
          // column cannot show it.
          {
            const std::int64_t ref = arms.front().run->selective_matches;
            for (const auto &a : arms) {
              if (a.run->selective_matches != ref) {
                std::cerr << "[validate] test_3 SELECTIVE mismatch: "
                          << arms.front().name << "=" << ref << " " << a.name
                          << "=" << a.run->selective_matches
                          << " -- the arms did not find the same results\n";
                validation_failed = true;
              }
            }
          }

          // ELEMENT PARITY. The rows above count RESOURCES; this one counts the
          // data itself -- every leaf the arm actually wrote. An arm can emit
          // all 317 resources and still drop half their fields, and every
          // resource-level check would pass. This is the row that would have
          // caught the HL7v2 arm writing OBX-5 = "1" for every observation.
          {
            const std::int64_t ref = ff.test1_elements;
            if (ref >= 0) {
              for (const auto &a : arms) {
                if (a.run->test1_elements != ref) {
                  std::cerr << "[validate] test_1 ELEMENT mismatch: fastfhir=" << ref
                            << " " << a.name << "=" << a.run->test1_elements
                            << " -- the arms did not serialize the same data\n";
                  validation_failed = true;
                }
              }
            }
          }

          const auto field_at = [](const bench::ArmRunResult &r, bench::Stage st,
                                   bool want_entries) -> std::int64_t {
            for (const auto &m : r.metrics)
              if (m.stage == st) return want_entries ? m.entries : m.bytes_out;
            return -1;   // stage absent for this arm
          };

          // ENTRY PARITY, every stage. The reference is fastfhir's count; a
          // stage no arm reports a count for is skipped, so adding a stage does
          // not require touching this.
          for (const auto st : {bench::Stage::Test1Serialize, bench::Stage::Test2RandomAccess,
                                bench::Stage::Test3Query, bench::Stage::Test4Enrich})
          {
            const std::int64_t ref = field_at(ff, st, /*want_entries=*/true);
            if (ref <= 0)
            {
              // Nobody counted here. Only a problem if somebody else did --
              // that means the stage HAS a notion of entries and this arm lost it.
              for (const auto &a : arms)
              {
                const std::int64_t n = field_at(*a.run, st, true);
                if (n > 0)
                {
                  std::cerr << "[validate] " << stage_label(st)
                            << " entry count missing: fastfhir reported none but "
                            << a.name << " reported " << n << "\n";
                  validation_failed = true;
                }
              }
              continue;
            }
            for (const auto &a : arms)
            {
              const std::int64_t n = field_at(*a.run, st, true);
              if (n < 0) continue;            // arm does not run this stage
              if (n != ref)
              {
                std::cerr << "[validate] " << stage_label(st)
                          << " entry mismatch: fastfhir=" << ref << " " << a.name << "=" << n
                          << " -- the arms did not handle the same resources\n";
                validation_failed = true;
              }
            }
          }

          // CROSS-STAGE: what Test 2 reaches must be what Test 1 wrote.
          //
          // Comparing arms to each other is not enough on its own -- all four
          // could agree on a number that is smaller than what they serialized,
          // and every per-stage check would still pass. The serialize count is
          // the only one anchored to the fixture, so every later stage is
          // measured against it.
          {
            for (const auto &a : arms) {
              const std::int64_t wrote = field_at(*a.run, bench::Stage::Test1Serialize, true);
              const std::int64_t read  = field_at(*a.run, bench::Stage::Test2RandomAccess, true);
              if (wrote > 0 && read >= 0 && read != wrote) {
                std::cerr << "[validate] " << a.name << " serialized " << wrote
                          << " resources but random-access reached " << read
                          << " -- a stage lost resources the arm had written\n";
                validation_failed = true;
              }
            }
          }

          // CHOLESTEROL PARITY (LOINC 2085-9). The query exists to find a
          // specific clinical result; an arm that finds a different number of
          // them has not answered the same question, however fast it was.
          // google_fhir reached this check for the first time here --
          // validate_parity only ever compared fastfhir, json and hl7.
          {
            const std::int64_t ref = ff.query_loinc_matches;
            for (const auto &a : arms) {
              if (a.run->query_loinc_matches != ref) {
                std::cerr << "[validate] test_3 LOINC 2085-9 (cholesterol) mismatch: fastfhir="
                          << ref << " " << a.name << "=" << a.run->query_loinc_matches
                          << " -- the arms did not answer the same query\n";
                validation_failed = true;
              }
            }
          }

          // BYTE PARITY for Test 2 specifically: the arms must have read the
          // same fields, not merely the same number of resources.
          {
            const std::int64_t ref = field_at(ff, bench::Stage::Test2RandomAccess, false);
            for (const auto &a : arms)
            {
              const std::int64_t b = field_at(*a.run, bench::Stage::Test2RandomAccess, false);
              if (b >= 0 && b != ref)
              {
                std::cerr << "[validate] test_2 random-access byte mismatch: fastfhir=" << ref
                          << " " << a.name << "=" << b
                          << " -- the arms did not read the same fields\n";
                validation_failed = true;
              }
            }
          }
        }

        auto emit = [&](const std::vector<bench::MetricEvent> &metrics)
        {
          for (const auto &m : metrics)
          {
            emit_metric(m, target_mb, n_patients, replicate, run_index);
            insert_metric(m, target_mb, n_patients);
          }
        };
        emit(ff.metrics);
        emit(jf.metrics);
#if defined(HAVE_HL7V2)
        emit(h2.metrics);
#endif
#if defined(HAVE_GOOGLE_FHIR)
        emit(gf.metrics);
#endif
      }
    }
  }

#ifdef HAVE_LIBPQ
  if (db_conn)
    PQfinish(db_conn);
#endif

  if (validation_failed)
  {
    std::cerr << "Validation failed: one or more cross-arm parity checks did not match.\n";
    return 2;
  }
  return 0;
}
