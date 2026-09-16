#include "harness.hpp"


#include <FF_Bundle.hpp>
#include <FF_Concurrency.hpp>
#include <FF_Compactor.hpp>

#include <algorithm>
#include "FF_Queue.hpp"
#include <cstdlib>
#include <atomic>
#include <thread>
#include <string_view>
#include <nlohmann/json.hpp>
#include <sstream>
#include <execution>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

#define ARM_FASTFHIR
#include "bench_test_1.hpp"
#include "bench_test_2.hpp"
#include "bench_test_3.hpp"
#include "bench_test_4.hpp"
#include "bench_test_5.hpp"
#include "walk_diagnostic.hpp"
#undef ARM_FASTFHIR

namespace bench
{
  namespace
  {

    const ObservationData &enrichment_observation_fixture()
    {
      static const EnrichmentObservationFixture fixture =
          load_enrichment_observation_from_json("bench/enrich.json");
      return fixture.observation;
    }

    // Parallel Bundle construction, in the shape the engine actually uses
    // (src/FF_Ingestor.cpp step 5): pre-allocate the Bundle's inline entry
    // array, hand every worker a FIFO::Queue consumer, and have each worker
    // append its resource and then amend the parent slot with the finished
    // child's offset. No worker allocates the array and no two workers touch
    // the same slot, so the only shared mutable state is the arena write head.
    //
    // The previous dispatch_apply form is gone. It measured the same thing but
    // could not vary the worker count, which is the axis this arm exists to
    // show.
    enum class BuildPool : std::uint8_t
    {
      RUNNING,
      COMPLETE,
      FAULTED
    };

    // Park/notify, copied from the ingestor's pool rather than spin-yielding:
    // a spinning worker burns CPU that process_cpu_ns() then reports as work.
    inline void park_for_task(std::atomic<BuildPool> &status, std::atomic<std::uint32_t> &waiters)
    {
      waiters.fetch_add(1, std::memory_order_release);
      if (status.load(std::memory_order_acquire) == BuildPool::RUNNING)
        status.wait(BuildPool::RUNNING);
      waiters.fetch_sub(1, std::memory_order_release);
    }

    inline void notify_parked(std::atomic<BuildPool> &status, std::atomic<std::uint32_t> &waiters)
    {
      if (waiters.load(std::memory_order_acquire))
        status.notify_all();
    }

    struct FfBuildItem
    {
      const PatientData *patient = nullptr;
      const ObservationData *observation = nullptr;
    };

    // A task is a RANGE, not one resource.
    //
    // append_obj on one POCO costs about 1 us -- the same order as pushing a
    // task and as one park/wake round trip. At one resource per task the pool
    // therefore spends its time going to sleep and waking up: measured on the
    // 64 MB corpus at 18 workers, 3.2 ms of USER work carried 180 ms of SYS
    // time, and wall time was 4.4x WORSE than a single thread. notify_one in
    // place of notify_all did not change it, so the cost is the park/wake rate
    // itself rather than a thundering herd.
    //
    // The ingestor gets away with one entry per task because its task is a full
    // simdjson parse plus store -- tens of microseconds -- which amortizes the
    // same protocol. This arm's task is the store alone, so it batches.
    struct FfBuildTask
    {
      std::uint32_t begin = 0;
      std::uint32_t end = 0;
    };

    // Resources per queued task. BENCH_FF_BATCH sweeps it.
    inline std::uint32_t ff_batch_size()
    {
      static const std::uint32_t n = []() -> std::uint32_t {
        if (const char *env = std::getenv("BENCH_FF_BATCH"))
        {
          const long v = std::strtol(env, nullptr, 10);
          if (v > 0) return static_cast<std::uint32_t>(v);
        }
        return 64;
      }();
      return n;
    }

    using FfBuildQueue = FIFO::Queue<FfBuildTask, 256>;

    // BENCH_FF_THREADS sweeps the worker count without a rebuild -- the whole
    // point of the exercise is the curve across it. Unset means the engine's
    // own default, FastFHIR::performance_core_count(), so the arm measures the
    // configuration a consumer actually gets rather than a benchmark-only one.
    inline unsigned int ff_worker_threads()
    {
      static const unsigned int n = []() -> unsigned int {
        if (const char *env = std::getenv("BENCH_FF_THREADS"))
        {
          const long v = std::strtol(env, nullptr, 10);
          if (v > 0) return static_cast<unsigned int>(v);
        }
        return FastFHIR::performance_core_count();
      }();
      return n;
    }

    // Append one resource and amend the parent entry slot with its offset.
    // Shared by the serial and parallel paths so they write identical bytes.
    inline void ff_build_one(FastFHIR::Builder &builder,
                             FastFHIR::Reflective::ObjectHandle &entry_array,
                             const FfBuildItem &item,
                             std::uint32_t idx)
    {
      FastFHIR::Reflective::ObjectHandle child =
          item.patient ? builder.append_obj(*item.patient)
                       : builder.append_obj(*item.observation);
      FastFHIR::Reflective::MutableEntry slot = entry_array[idx];
      FastFHIR::Reflective::MutableEntry resource_slot =
          slot[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
      resource_slot = child;
    }

  } // namespace

  // ---------------------------------------------------------------------
  // Why this arm calls append_obj(POCO) instead of the shared assignment layer
  // ---------------------------------------------------------------------
  // The shared, macro-guarded layer assembles a resource field-by-field:
  // append an EMPTY resource, then amend each slot. For arrays of FHIR
  // datatypes that is not expressible through FastFHIR's public API.
  //
  // Observation.category (and identifier, interpretation, note, ...) is stored
  // as FF_ARRAY::INLINE_BLOCK: entry i is a fixed-size block header at
  // entries_start + i*HEADER_SIZE, with its variable-length tail written
  // elsewhere in child space. The generator emits that with the four-argument
  // STORE_FF_<TYPE>(base, header_offset, child_offset, data) overload.
  //
  // The bench's stream_assign_array_offsets() instead wrote a vector<Offset>.
  // The reader then walked those 8-byte offsets as if they were inline
  // CodeableConcept headers and dereferenced payload text as a string offset:
  //   ASan: SEGV in FF_STRING::read_view <- FF_CODEABLECONCEPT::deserialize
  //         <- FF_OBSERVATION::deserialize <- Node::as<ObservationData>()
  // validate_FFHR_stream() did NOT catch it -- the stream is self-consistent by
  // its rules; only the generated deserializer walks the array as blocks.
  //
  // TypeTraits<T> exposes only the self-contained three-argument store, so a
  // generic inline-block array writer cannot be built on the public API. Doing
  // it properly means per-type dispatch to the four-argument overloads, i.e.
  // re-implementing part of the generator.
  //
  // So this arm hands the whole POCO to the generated STORE, which lays out
  // every array correctly. PARITY COST: this arm now serializes EVERY field of
  // the POCO, while the other three arms serialize only the ~25 fields the
  // shared assignment layer covers. That is more work, not less, so it biases
  // against FastFHIR -- but it is not parity. See notes.md.
  // ---------------------------------------------------------------------
  ArmRunResult run_fastfhir_bundle(const BundleBenchFixture &fixture)
  {
    ArmRunResult out;

    std::size_t arena_hint = 4096;
    std::size_t total_observation_count = 0;
    for (const auto &p : fixture.bundle)
    {
      arena_hint += p.memory.capacity();
      total_observation_count += p.observations.size();
    }
    FastFHIR::Memory payload_memory = FastFHIR::Memory::create(arena_hint);
    const FastFHIR::FF_Builder builder_handle = make_builder(payload_memory, FHIR_VERSION_R5);
    FastFHIR::Builder &builder = *builder_handle;

    // BENCH_FF_PREFAULT: touch one byte per page of the arena BEFORE the clock
    // starts. Memory::create only reserves; every page is first-touched during
    // the build, and N threads first-touching one fresh anonymous mapping
    // serialize on the kernel's VM lock. Pre-faulting moves that cost outside
    // the measured window, which says how much of the parallel sys time it is.
    // Diagnostic only -- a run with this set is not comparable to one without.
    if (std::getenv("BENCH_FF_PREFAULT"))
    {
      // The write goes through a volatile lvalue so it cannot be elided.
      // (Chaining it into another assignment is deprecated in C++20.)
      std::uint8_t *base = const_cast<std::uint8_t *>(payload_memory.base());
      const std::size_t page = 16384;  // Apple silicon
      for (std::size_t off = 0; off < arena_hint; off += page)
        static_cast<volatile std::uint8_t &>(base[off]) = 0;
    }

    std::int64_t trace_user0 = 0, trace_sys0 = 0;
    const bool trace = std::getenv("BENCH_FF_TRACE") != nullptr;
    if (trace) bench::process_cpu_split_ns(trace_user0, trace_sys0);

    Timer test1_timer;
    test1_timer.start();

    // Task list first: patients then observations, the entry order the other
    // arms use.
    std::vector<FfBuildItem> items;
    items.reserve(fixture.bundle.size() + total_observation_count);
    for (const auto &item : fixture.bundle)
      items.push_back(FfBuildItem{&item.patient, nullptr});
    for (const auto &item : fixture.bundle)
      for (const auto &observation : item.observations)
        items.push_back(FfBuildItem{nullptr, &observation});

    // Pre-allocate the inline entry array (FF_Ingestor.cpp step 2): N
    // default-constructed BundleentryData means append_obj lays down
    // [FF_ARRAY header | entry[0] | entry[1] | ...] as one contiguous region,
    // and every worker below patches inside its own already-allocated slot.
    BundleData bundle{};
    bundle.type = FF_BundleType::Collection;
    bundle.entry = std::vector<BundleentryData>(items.size());

    FastFHIR::Reflective::ObjectHandle root_handle = builder.append_obj(bundle);
    FastFHIR::Reflective::ObjectHandle entry_array =
        root_handle[FastFHIR::Fields::BUNDLE::ENTRY];

    const unsigned int worker_count = bench::g_serial_build ? 1u : ff_worker_threads();

    if (worker_count <= 1)
    {
      for (std::uint32_t idx = 0; idx < items.size(); ++idx)
        ff_build_one(builder, entry_array, items[idx], idx);
    }
    else if (const char *mode = std::getenv("BENCH_FF_MODE");
             mode && std::string_view(mode) == "split")
    {
      // Diagnostic mode: no queue, no parking. Each worker takes a contiguous
      // pre-assigned range, so the ONLY shared mutable state left is the arena
      // write head and the pages it hands out. If sys time collapses here, the
      // kernel time the queue mode burns is park/notify; if it does not, it is
      // page faults on the sparse VMA.
      std::vector<std::thread> workers;
      workers.reserve(worker_count);
      const std::size_t n = items.size();
      for (unsigned int w = 0; w < worker_count; ++w)
      {
        const std::size_t begin = (n * w) / worker_count;
        const std::size_t end = (n * (w + 1)) / worker_count;
        workers.emplace_back([&builder, &entry_array, &items, begin, end]()
                             {
          for (std::size_t idx = begin; idx < end; ++idx)
            ff_build_one(builder, entry_array, items[idx], static_cast<std::uint32_t>(idx)); });
      }
      for (auto &worker : workers)
        worker.join();
    }
    else
    {
      FfBuildQueue task_queue;
      std::atomic<BuildPool> status{BuildPool::RUNNING};
      std::atomic<std::uint32_t> waiters{0};
      std::atomic<bool> faulted{false};

      // Consumers are latched HERE, before the first push, on this thread.
      // get_consumer() pins the queue's current head node; a consumer created
      // inside the worker body can start mid-stream and silently lose every
      // entry retired before it ran (FF_Queue.hpp; TASKS.md AR-3).
      std::vector<FfBuildQueue::Consumer> consumers;
      consumers.reserve(worker_count);
      for (unsigned int i = 0; i < worker_count; ++i)
        consumers.emplace_back(task_queue.get_consumer());

      std::vector<std::thread> workers;
      workers.reserve(worker_count);
      for (unsigned int i = 0; i < worker_count; ++i)
      {
        workers.emplace_back([&builder, &entry_array, &items, &status, &waiters, &faulted,
                              consumer = std::move(consumers[i])]() mutable
                             {
          FfBuildTask task;
          for (;;)
          {
            if (status.load(std::memory_order_acquire) == BuildPool::FAULTED) break;
            if (consumer.pop(task))
            {
              try
              {
                for (std::uint32_t idx = task.begin; idx < task.end; ++idx)
                  ff_build_one(builder, entry_array, items[idx], idx);
              }
              catch (...)
              {
                faulted.store(true, std::memory_order_release);
                status.store(BuildPool::FAULTED, std::memory_order_release);
                status.notify_all();
                break;
              }
              continue;
            }
            if (status.load(std::memory_order_acquire) == BuildPool::COMPLETE) break;
            park_for_task(status, waiters);
          } });
      }

      {
        auto injector = task_queue.get_injector();
        const std::uint32_t batch = ff_batch_size();
        const std::uint32_t n = static_cast<std::uint32_t>(items.size());
        for (std::uint32_t begin = 0; begin < n; begin += batch)
        {
          injector.push(FfBuildTask{begin, std::min(begin + batch, n)});
          notify_parked(status, waiters);
        }
      }
      status.store(BuildPool::COMPLETE, std::memory_order_release);
      status.notify_all();

      for (auto &worker : workers)
        worker.join();

      if (faulted.load(std::memory_order_acquire))
        throw std::runtime_error("fastfhir arm: concurrent bundle build faulted");
    }

    // The sealed Bundle's own entry count, like the JSON arm's. The Bundle was
    // appended before the workers ran, so root_handle IS the root -- appending
    // it a second time here would write a whole duplicate Bundle whose entry
    // slots are the unpatched originals.
    const std::int64_t test1_entries = static_cast<std::int64_t>(bundle.entry.size());
    (void)seal_stream(builder_handle, root_handle, "fastfhir arm bundle");
    const std::int64_t test1_ns = test1_timer.stop_ns();
    const std::int64_t test1_cpu_ns = test1_timer.cpu_ns();
    if (trace)
    {
      std::int64_t u1 = 0, s1 = 0;
      bench::process_cpu_split_ns(u1, s1);
      std::fprintf(stderr,
                   "[cpu] fastfhir test_1 workers=%u wall=%.3fms user=%.3fms sys=%.3fms "
                   "cores=%.2f\n",
                   worker_count, test1_ns / 1e6, (u1 - trace_user0) / 1e6,
                   (s1 - trace_sys0) / 1e6,
                   test1_cpu_ns > 0 ? (double)test1_cpu_ns / (double)test1_ns : 0.0);
    }
    // Wire size is read AFTER the clock stops -- nothing goes between the last
    // real operation and stop_ns() (notes.md section 6).
    const std::int64_t test1_bytes = static_cast<std::int64_t>(payload_memory.view().size());
    out.metrics.push_back({"fastfhir", Stage::Test1Serialize, test1_ns, /*bytes_in=*/0,
                           test1_bytes, /*ops=*/0, /*entries=*/test1_entries,
                           /*cpu_ns=*/test1_cpu_ns});
    // --dump-artifacts input (see main.cpp): the sealed wire bytes.
    {
      const auto v = payload_memory.view();
      out.test1_payload.assign(v.data(), v.size());
      // Leaves actually present in what this arm just wrote -- measured from
      // the OUTPUT, not from the fixture, so an arm that dropped fields reports
      // fewer. After the clock stops, like the byte count above.
      if (bench::g_count_elements)
        out.test1_elements = bench::test_5::BENCH_ARM_NS::count_output_elements(
            reinterpret_cast<const char *>(v.data()), v.size());
    }

    if (std::getenv("BENCH_VALIDATE"))
    {
      FastFHIR::Parser check(payload_memory);
      const FF_Result vr = check.validate_FFHR_stream();
      std::fprintf(stderr, "[validate] fastfhir arm stream: code=%d %s\n",
                   (int)vr.code, vr.message.c_str());
      // Does the REFLECTIVE reader survive this stream, even though the
      // generated deserializer does not?
      std::ostringstream sink;
      try {
        check.print_json(sink);
        std::fprintf(stderr, "[validate] print_json ok, %zu bytes\n", sink.str().size());
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[validate] print_json threw: %s\n", ex.what());
      }
      // And what does the bundle actually contain?
      auto rn = check.root();
      if (auto es = rn[FastFHIR::Fields::BUNDLE::ENTRY]) {
        std::size_t n_pat = 0, n_obs = 0, n_other = 0, n_null = 0;
        for (auto& e : es.entries()) {
          auto r = e[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
          if (!r) { ++n_null; continue; }
          auto node = r.as_node();
          if (!node) { ++n_null; continue; }
          if (node.is<FastFHIR::RESOURCETYPE::PATIENT>()) ++n_pat;
          else if (node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) ++n_obs;
          else ++n_other;
        }
        std::fprintf(stderr, "[validate] entries: patient=%zu observation=%zu other=%zu null=%zu\n",
                     n_pat, n_obs, n_other, n_null);
      }
    }

    // FastFHIR-native compact archive (WF-1.4 / IN-E). Compaction runs once
    // per sample; the arena and its losslessness verdict stay alive so the
    // same probes run over the compact stream (test_2_compact / test_3_compact
    // / test_4_compact) -- the claim under test is that the reader is
    // layout-agnostic, i.e. compact ≈ standard speed.
    //
    // Gated on losslessness: the compact stream must re-parse to JSON
    // semantically identical to the standard stream's, or no compact row is
    // emitted -- the upstream compactor silently dropped scalar arrays until
    // 459e8d8, and no compact number leaves without the gate (handoff.md
    // Instrument E, upstream I3.7).
    FastFHIR::Memory compact_mem;
    FastFHIR::Memory::View compact_view{};
    bool compact_lossless = false;
    {
      const FastFHIR::Memory::View standard_view = payload_memory.view();
      compact_mem =
          FastFHIR::Memory::create(std::max<std::size_t>(standard_view.size() * 2, 1));
      Timer compact_timer;
      compact_timer.start();
      compact_view = FastFHIR::Compactor::archive(FastFHIR::Parser(payload_memory), compact_mem,
                                                  FF_CHECKSUM_NONE);
      const std::int64_t compact_ns = compact_timer.stop_ns();

      // Semantic JSON equality (nlohmann), not string equality: the two
      // layouts may legitimately order fields differently, and a false-negative
      // gate would silently suppress every compact number.
      bool lossless = false;
      try
      {
        nlohmann::json std_json, cmp_json;
        {
          std::ostringstream sink;
          FastFHIR::Parser(payload_memory).print_json(sink);
          std_json = nlohmann::json::parse(sink.str());
        }
        {
          std::ostringstream sink;
          FastFHIR::Parser(compact_view.data(), compact_view.size()).print_json(sink);
          cmp_json = nlohmann::json::parse(sink.str());
        }
        lossless = (std_json == cmp_json);
      }
      catch (const std::exception &ex)
      {
        std::fprintf(stderr, "[warn] compact losslessness probe threw: %s\n", ex.what());
      }
      compact_lossless = lossless;

      if (lossless)
      {
        out.metrics.push_back({"fastfhir", Stage::Test1Compact, compact_ns, 0,
                               static_cast<std::int64_t>(compact_view.size())});
      }
      else
      {
        std::fprintf(
            stderr,
            "[warn] compact losslessness FAILED -- compact size not emitted (IN-E gate)\n");
      }
    }

    Timer test3_timer;
    test3_timer.start();
    const auto query_summary = test_3::query(payload_memory.view());
    out.metrics.push_back({"fastfhir", Stage::Test3Query, test3_timer.stop_ns(),
                         /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                         /*entries=*/test_3::query_entries(query_summary)});
    // SELECTIVE query -- see Stage::Test3Selective. Timed on its own so the
    // early-out cost is not hidden inside the census above.
    Timer test3s_timer;
    test3s_timer.start();
    const auto selective = test_3::query_selective(payload_memory.view());
    const std::int64_t test3s_ns = test3s_timer.stop_ns();
    out.metrics.push_back({"fastfhir", Stage::Test3Selective, test3s_ns,
                           /*bytes_in=*/0, /*bytes_out=*/0, /*ops=*/0,
                           /*entries=*/static_cast<std::int64_t>(selective.scanned)});
    out.selective_matches = static_cast<std::int64_t>(selective.matches);
    out.queried_value = test_3::format_query_summary(query_summary);
  out.query_loinc_matches =
      static_cast<std::int64_t>(query_summary.loinc_2085_9_matches);

    // Test 3 over the compact archive (test_3_compact). Same census, same
    // lens reads -- the reader dispatches on the stream layout, so this must
    // come out ~equal in speed and IDENTICAL in answers. A query-summary
    // mismatch would mean the compact stream lost content the print_json
    // gate could not see.
    if (compact_lossless)
    {
      try
      {
        Timer t3c_timer;
        t3c_timer.start();
        const auto compact_query =
            test_3::query(std::string_view(compact_view.data(), compact_view.size()));
        const std::int64_t t3c_ns = t3c_timer.stop_ns();
        if (test_3::format_query_summary(compact_query) != out.queried_value)
        {
          std::fprintf(stderr, "[warn] compact query summary differs from standard!\n");
        }
        out.metrics.push_back({"fastfhir", Stage::Test3QueryCompact, t3c_ns});
      }
      catch (const std::exception &ex)
      {
        std::fprintf(stderr, "[warn] test_3_compact failed: %s\n", ex.what());
      }
    }

  // Full-traversal walk -- DIAGNOSTIC ONLY, off unless BENCH_WALK=1, never a
  // reported stage. Retained so upstream CAPI-7 / CAPI-8 stay reproducible;
  // D4 retired the walk as a stage, not as evidence.
  (void)walk_diag::run(payload_memory);

  // Test 2 -- random access (IN-B / WF-1.1). Out-of-order reads, navigating
  // from the root each time; the retired materialize walk read in layout
  // order, and the two disagree by three orders of magnitude.
  //
  // MUST run BEFORE Test 4. FastFHIR::Memory is a shared_ptr handle, so the
  // enrich appends into this very arena (PA-9) -- running Test 2 afterwards
  // had the FastFHIR arm reading 1,474 entries while the other arms read
  // 1,473, and the cross-arm byte gate caught it.
  {
    const auto ra = test_2::random_access(payload_memory);
    out.metrics.push_back(test_2::random_access_metric("fastfhir", ra));
    out.random_access_summary = test_2::format_random_access_summary(ra);
  }

  // Random access over the compact archive (test_2_compact). Same lens reads;
  // the byte accumulators are NOT cross-arm-gated here (the gate covers the
  // standard stage) -- the reader is the same, which is what makes the two
  // comparable, and the query cross-check above guards content.
  if (compact_lossless)
  {
    try
    {
      Timer t2c_timer;
      t2c_timer.start();
      const auto compact_ra = test_2::random_access(compact_mem);
      const std::int64_t t2c_ns = t2c_timer.stop_ns();
      out.metrics.push_back({"fastfhir", Stage::Test2RandomAccessCompact, t2c_ns,
                             /*bytes_in=*/0, /*bytes_out=*/compact_ra.bytes_read,
                             /*ops=*/compact_ra.reads});
    }
    catch (const std::exception &ex)
    {
      std::fprintf(stderr, "[warn] test_2_compact failed: %s\n", ex.what());
    }
  }

  auto enrich_result = test_4::BENCH_TEST_4_ENRICH_FN(payload_memory, enrichment_observation_fixture());
    out.metrics.push_back(test_4::enrich_metric("fastfhir", enrich_result.summary));
    out.enriched_stream = std::move(enrich_result.enriched_stream);
    out.enrich_metrics_summary = test_4::format_enrich_summary(enrich_result.summary);

    // Enrich over the compact archive (test_4_compact). The API REFUSES this
    // by design -- "Cannot open Builder on a compact archive. Decompact to a
    // standard stream before append/mutation" -- so there is no row to emit
    // (0 would claim N/A per the MetricEvent contract but break the duration
    // gate). Attempting it anyway is the instrument: it verifies the
    // write-once property still holds. CAPI-10 tracks the undocumented
    // immutability.
    if (compact_lossless)
    {
      try
      {
        auto compact_enrich =
            test_4::BENCH_TEST_4_ENRICH_FN(compact_mem, enrichment_observation_fixture());
        out.metrics.push_back({"fastfhir", Stage::Test4EnrichCompact,
                               compact_enrich.summary.duration_ns,
                               static_cast<std::int64_t>(compact_enrich.summary.source_bytes),
                               static_cast<std::int64_t>(compact_enrich.summary.enriched_bytes)});
      }
      catch (const std::exception &ex)
      {
        static bool logged = false;
        if (!logged)
        {
          std::fprintf(stderr,
                       "[compact] test_4_compact: API refuses to open a Builder on a compact "
                       "archive (write-once format) -- no row emitted, by design: %s\n",
                       ex.what());
          logged = true;
        }
      }
    }
    return out;
  }

} // namespace bench

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_fastfhir::structural_positions(wire).size();
}
}  // namespace
const ArmOps &arm_ops_fastfhir() {
  static const ArmOps ops{"fastfhir", &arm_fastfhir::calc_stream_hash,
                          &arm_fastfhir::corrupt_stream,
                          &arm_fastfhir::recover_stream, &count_positions};
  return ops;
}
}  // namespace bench::test_5
