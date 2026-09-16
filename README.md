# FastFHIR Lightweight Benchmark

Four-arm comparative benchmark measuring serialization, materialization, query, and enrichment performance across **FastFHIR**, **JSON FHIR** (nlohmann::json + simdjson), **Google FHIR** (protobuf), and **HL7v2** (ORU^R01).

> ## ✅ Build status: GREEN (2026-08-25) — results not yet publishable
>
> The harness was ported to FastFHIR's `FF_*` façade API and **builds and runs
> again**. All four arms report all four stages.
>
> ```bash
> bazel build -c opt //bench:all                     # green
> bazel test  -c opt //bench:timing_conformance_test # PASSED
> ./bazel-bin/bench/bench_harness --runs 2 --bundle-max-mb 16
> ```
>
> **Exit code 2 is expected** — it signals a cross-arm parity mismatch, not a
> crash. The HL7v2 arm does not report `obs_issued_present` or
> `obs_component_value_*`.
>
> **The numbers it produces are proof the harness works, not results to
> publish.** Getting it running surfaced five defects that had been live and
> silent, including one that made every earlier crash undebuggable. Read
> **[notes.md](notes.md)** before quoting any figure, and
> **[TASKS.md](TASKS.md)** before designing new tests — the current 4×4 grid
> does not map onto the claims in FastFHIR's § Why FastFHIR?:
>
> | | |
> |---|---|
> | Test 1 | Not at parity — the FastFHIR arm serializes every POCO field, the others ~25. Excludes `value[x]` entirely. |
> | Test 2 | Random access (D4, 2026-08-26): N random entry reads, each navigating from the root. Replaced the materialize walk — a layout-order traversal that was a tape's best case and a query's worst. Cross-arm byte gate fails the run on mismatched reads. |
> | Test 3 | FastFHIR reads via node lenses — no whole-POCO materialization (2026-08-26); still pays a `print_json` penalty on `birthDate` only (PA-7). |
> | Provenance | Compiled profile and upstream SHA still unrecorded — [TASKS.md § PROFILE](TASKS.md). |

## FastFHIR build flags — the Debug trap (2026-08-19)

FastFHIR is a Bazel module dependency of this repo (see `generate_repo.sh`: "Bazel
handles building FastFHIR from source as a module dependency"), so it inherits
this repo's `.bazelrc` `--compilation_mode=opt`. That is load-bearing: FastFHIR's
own CMake presets configure **Debug (`-O0`)** builds, and its published numbers —
including TASKS.md's OPEN TOPIC §A/§B tables, labeled "-O2" — were measured on a
Debug build. The same code is ~10× faster optimized:

| Path (50.8 MiB Synthea bundle, min of 7, same machine) | Debug `-O0` | Release `-O3` |
|---|---|---|
| `validate_FFHR_stream()` full graph walk | 107.5 ms / 0.46 GiB/s | **10.4 ms / 4.9 GiB/s** |
| `Bundle.entry.entries()` (31,042 elements) | 856 µs | **36 µs** |
| `print_json()` export walk | 734 ms | 197 ms |

> ⚠ **These absolute numbers are void.** They were measured before the
> opaque-JSON change (2026-08-24) and describe a stream that did not contain
> out-of-profile resources. The Debug-vs-Release *ratio* is still the point of
> the table and still holds; the milliseconds do not. Re-measure after the
> port — [TASKS.md § PORT-9](TASKS.md).

Rules:

- **Keep `--compilation_mode=opt` in `.bazelrc`.** Never build or run the
  benchmark with `-c fastbuild`/`-c dbg`: the cross-format comparison would stay
  fair (all arms share flags) but every absolute number and every
  FastFHIR-vs-orjson ratio would be Debug-inflated — the exact trap that
  produced FastFHIR's §A table. If you ever re-run the published 2.4–3.4×
  orjson ratios, confirm the build was `opt`.
- When FastFHIR is needed outside Bazel (fixture prep, local profiling),
  configure it with `-DCMAKE_BUILD_TYPE=Release`; its `ninja`/`xcode` CMake
  presets are Debug.
- Do not adopt FastFHIR "read-path optimizations" measured only at Debug: a
  one-shot-fill `entries()` variant won 856→525 µs at `-O0` but **regressed
  36→58 µs at `-O3`** (per-element `push_back` writes each 48-byte `Node` once;
  the fill writes twice once libc++'s container annotations inline away).
  See FastFHIR TASKS.md OPEN TOPIC correction, 2026-08-19.

---

## Result provenance

Two things silently change what this benchmark measures. Neither is visible to
Bazel, and both must be recorded alongside any published number.

### 1. The compiled profile

`.external/FastFHIR` is a **symlink to the live working tree**
(`../FastFHIR`) — not a pinned checkout. Whatever state that tree is in is what
you measure. Worse, Bazel does not run FastFHIR's generator: its `BUILD.bazel`
globs `generated_src/*.cpp`, and that directory is produced by **CMake at
configure time** and is **profile-dependent**.

**So the profile you benchmark is whatever CMake last generated.** A profile
change swaps the binary under test with no Bazel-visible signal.

Current upstream state (verified 2026-08-24, head `a9fd4e9`):

| | |
|---|---|
| profile | `us-core,billing,medication-admin,supply` |
| code-system enums | 80 |
| generated `.cpp` | 44 |
| resource types compiled | 37 |
| `ImagingStudy` compiled | no — takes the opaque-JSON path |

**Record the profile and the upstream git SHA with every result you publish.**

**For anything published, pin it instead** (TASKS.md PB-1):

```bash
scripts/run_benchmark.sh --fastfhir-ref <tag|sha>
```

This builds against a clean checkout at that commit, with `generated_src/`
regenerated at the commit's own shipped profile, via Bazel's
`--override_module`. `provenance.json` then records that tree
(`fastfhir_path_source: "bazel external repo"`) rather than the symlink.

Published results come from `.github/workflows/bench-release.yml`. It runs
[`scripts/bench_ab.py`](scripts/bench_ab.py), which alternates the release and
its predecessor on one machine, then applies the
[`scripts/publish_check.py`](scripts/publish_check.py) gate and attaches the
results to a `bench-fastfhir-<version>` GitHub Release (TASKS.md PB-2).

After any profile change, `rm -rf ../FastFHIR/generated_src` before
regenerating — the generator never deletes output it no longer emits, so a
stale tree survives the change and produces confusing compile errors.

### 2. Out-of-profile resources are opaque, not absent

FastFHIR used to **silently drop** any resource type outside the compiled
profile — one Synthea bundle lost 41 of 250 records and still returned
success. That is fixed: the raw JSON is now retained verbatim in the stream as
an opaque block and re-emitted byte-for-byte on export.

This changes the benchmark in three ways:

- **Size and throughput comparisons are apples-to-apples for the first time.**
  Any earlier FastFHIR-vs-JSON size or throughput win was measured against a
  FastFHIR stream that did not contain data the other arms carried. That
  favoured FastFHIR, invisibly. Treat all pre-2026-08-24 numbers as void.
- **Streams are larger and ingest does more work** for any document containing
  out-of-profile types — which Synthea does, heavily.
- **An opaque resource round-trips perfectly but is not typed-navigable.** No
  V-Table means no `Node` field access, no query, and no interior compaction.

Because the shipped preset deliberately excludes the `imaging` grouping, the
Synthea corpus's **1,444 `ImagingStudy` resources take the opaque path on
every run**. That is intentional coverage — it is what continuously proves the
fallback is lossless. **Do not enable `imaging` to "fix" it.**

The consequence for stage 3: a "query every resource" benchmark is not
querying every resource. Either restrict the corpus to in-profile types or
build with a wider profile — and **say which you did**, because it changes
both the binary and the workload. Report the opaque fraction (resource count
and bytes) next to any query result. Tracked in
[TASKS.md § CORPUS](TASKS.md).

---

## Repository documents

| Document | What it is | Freshness |
|---|---|---|
| [`TASKS.md`](TASKS.md) | **The only backlog**, and the standing **Decisions** (D1 macro parity · D2 `value[x]` tiering · D3 serialization model). **Start here.** | ✅ current (2026-08-26) |
| [`notes.md`](notes.md) | **Field report from the 2026-08-25 port.** What was silently broken and how it was found. Read before designing new tests. | ✅ current (2026-08-26) |
| [`BENCHMARK_IMPLEMENTATION_CHECKLIST.md`](BENCHMARK_IMPLEMENTATION_CHECKLIST.md) | Per-component design vs. build status | ✅ current |
| [`IMPLEMENTATION_SUMMARY.md`](IMPLEMENTATION_SUMMARY.md) | Curated architecture + parity overview | ✅ current |
| [`PARITY_ASSESSMENT.md`](PARITY_ASSESSMENT.md) | Fairness audit of all four arms | ⚠️ 2026-05-07; reasoning stands, verification stale |
| [`RESOURCE_COVERAGE_ANALYSIS.md`](RESOURCE_COVERAGE_ANALYSIS.md) | Which FHIR resources are exercised | ⚠️ denominator corrected in-place |
| [`MESSAGE_SURFACE_PARITY_AUDIT.md`](MESSAGE_SURFACE_PARITY_AUDIT.md) | Google FHIR proto coverage audit | ⚠️ 2026-05-06; Stage 2/3 findings still stand |
| [`MESSAGE_SURFACE_PARITY_QUICK_REFERENCE.md`](MESSAGE_SURFACE_PARITY_QUICK_REFERENCE.md) | TL;DR of the above | ⚠️ same vintage |
| [`FFHRnotes.md`](FFHRnotes.md) | Upstream API friction log | ⚠️ mostly resolved; see its status banner |
| [`TODO.md`](TODO.md) | **Design spec for Instrument G** — the four resilience tests. Not a second backlog. | ⚠️ unblocked 2026-08-25; API references need a pass |
| [`bench/*.md`](bench/) | Per-file architecture docs | ⚠️ describe pre-port code; each carries a banner |

Upstream context lives in `../FastFHIR/handoff.md` (why the API moved and what
it invalidates) and `../FastFHIR/CLAUDE.md` (**not** auto-loaded — read it
before editing that tree).

Our asks against FastFHIR's public API are filed in `../FastFHIR/TASKS.md` as
**CAPI-1…CAPI-6**, plus claims-alignment items **I3.6** (the orjson citation)
and **I3.7** (the compact-size figures). Tracked here in TASKS.md § UP. When
this benchmark hits an API that is unclear, unsafe to misuse, or missing, file it
there — this repo is FastFHIR's first external consumer, and that friction is a
finding, not an inconvenience.

Both repos carry a chunk-level `.arbiter/` index (`repo_map.md` + per-file
JSON). Navigate with it rather than reading trees wholesale — and remember a
correction banner at the top of a document **does not travel with a retrieved
chunk**, so stale body text must be struck in place (TASKS.md HY-5).

---

## Architecture

```
Synthea JSON files (119 patients)
        │
        ▼
  synthea_fixture.cpp ── FastFHIR Ingest ──► BundlePatient (POCO + arena)
        │
        ▼
  BundleBenchFixture ── identical C++ structs fed to all 4 arms
        │
   ┌────┼────┬────┐
   ▼    ▼    ▼    ▼
 arm_   arm_ arm_ arm_
 fastfhir.json hl7v2 google_
 .cpp  .cpp .cpp fhir.cpp
  │     │     │     │
  ▼     ▼     ▼     ▼
  bench_test_1.hpp — shared assign_patient() / assign_observation()
  bench_test_2.hpp — shared random_access() per format
  bench_test_3.hpp — shared query() per format
  bench_test_4.hpp — shared enrich() per format
  │     │     │     │
  ▼     ▼     ▼     ▼
  4× MetricEvent per arm → stdout CSV + optional PostgreSQL
```

Each arm receives the **exact same in-memory `PatientData`/`ObservationData` structs** and serializes them into its respective wire format. See the per-file docs:

| Doc File | What It Describes |
|---|---|
| [`arm_fastfhir.cpp.md`](bench/arm_fastfhir.cpp.md) | FastFHIR binary arena serialization — the primary arm |
| [`arm_json_fhir.cpp.md`](bench/arm_json_fhir.cpp.md) | nlohmann::json → FHIR JSON bundle |
| [`arm_hl7v2.cpp.md`](bench/arm_hl7v2.cpp.md) | HL7v2 ORU^R01 pipe-delimited messages |
| [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Google protobuf FHIR — all 4 stages live |
| [`harness.hpp.md`](bench/harness.hpp.md) | Core types, Timer, Stage enum, ArmRunResult |
| [`main.cpp.md`](bench/main.cpp.md) | CLI args, Synthea discovery, bundle sweep, DB persistence |
| [`synthea_fixture.cpp.md`](bench/synthea_fixture.cpp.md) | JSON → BundlePatient ingestion pipeline |
| [`bench_test_1.hpp.md`](bench/bench_test_1.hpp.md) | Shared assignment layer (the fairness guarantee) |
| [`bench_test_2.hpp.md`](bench/bench_test_2.hpp.md) | Random access: N reads from the root (replaced materialize, D4) |
| [`bench_test_3.hpp.md`](bench/bench_test_3.hpp.md) | Query: LOINC 2085-9 search + value classification |
| [`bench_test_4.hpp.md`](bench/bench_test_4.hpp.md) | Enrichment: append observation to existing bundle |
| [`hl7v2_message.hpp.md`](bench/hl7v2_message.hpp.md) | HL7v2 segment types, builder, batch parser |
| [`timing_conformance_test.cpp.md`](bench/timing_conformance_test.cpp.md) | Build-time parity validation |
| [`read_path_bench.cpp.md`](bench/read_path_bench.cpp.md) | Read-path traversal validation — per-Bundle-entry throughput gate (`<= 50 µs/entry`) |
| [`enrich.json.md`](bench/enrich.json.md) | BMP panel test fixture (LOINC 24321-3) |
| [`BUILD.bazel.md`](bench/BUILD.bazel.md) | Bazel target graph and conditional arms |

---

## Code Parity: The Fundamental Requirement

This benchmark's validity depends on one axiom: **every arm serializes bit-identical source data through structurally identical assignment code.** Any deviation — a missing field, a different code path, a skipped edge case — invalidates the comparison.

### How Parity Is Enforced

The entire assignment layer lives in a **single header per test stage**, guarded by `#define ARM_*` macros:

```cpp
// arm_fastfhir.cpp:
#define ARM_FASTFHIR
#include "bench_test_1.hpp"   // assign_patient() → FastFHIR ObjectHandle
#include "bench_test_2.hpp"   // random_access() → N root-navigated reads
#include "bench_test_3.hpp"   // query()          → FFHR node walk
#include "bench_test_4.hpp"   // enrich()         → FFHR arena append
#undef ARM_FASTFHIR

// arm_json_fhir.cpp:
#define ARM_JSON
#include "bench_test_1.hpp"   // assign_patient() → nlohmann::json
#include "bench_test_2.hpp"   // materialize()    → simdjson DOM
#include "bench_test_3.hpp"   // query()          → simdjson walk
#include "bench_test_4.hpp"   // enrich()         → JSON parse→modify→dump
#undef ARM_JSON
```

Inside each `bench_test_N.hpp`, `#if defined(ARM_FASTFHIR) / #elif defined(ARM_JSON) / #elif defined(ARM_HL7V2) / #elif defined(ARM_GOOGLE_FHIR)` blocks expose the format-specific implementation of the *same logical operation*. The call sites in the arm files are identical — only the backend changes.

This pattern guarantees:
- **Same loop structure** — no arm gets an algorithmic advantage
- **Same field coverage** — every field assigned by one arm is assigned by all arms
- **Same query logic** — LOINC matching, value type classification, component traversal are line-for-line equivalent
- **One audit point** — add a field to the shared assignment, and all four arms get it simultaneously

### Critical Open TODOs for Full Parity

> These are *parity* gaps — they assume a benchmark that builds. The build
> break itself is [TASKS.md § PORT](TASKS.md) and comes first.

| # | TODO | Impact | Effort |
|---|---|---|---|
| 1 | ~~**Google FHIR Stage 2**~~ — **already implemented** (verified 2026-08-25): parses the TLV records, `ParseFromArray` per message, protobuf-reflection walk. 9,539 nodes, ~1.6 ms/MB. | ✅ resolved | — |
| 2 | ~~**Google FHIR Stage 3**~~ — **already implemented**: returns `patients=1 observations=316 obs_issued_present=316`, matching the JSON baseline. **But** it reports `birthdate` as a microsecond epoch where every other arm reports ISO, and the arm is excluded from `validate_parity()`. | 🟡 Normalization + cross-check | ~4h |
| 3 | **Google FHIR Stage 4 (Enrich)** — functional but re-parses *all* records. Verify parity with other arms' enrich semantics (FastFHIR appends without re-parse; JSON re-parses all). | 🟡 Enrich cost model differs fundamentally | ~8h doc |
| 4 | **HL7v2 birthdate normalization mismatch** — `normalize_birthdate()` strips non-digits (`1990-03-21` → `19900321`). All other arms preserve ISO format. The conformance checker accounts for this, but the serialization output differs. | 🟡 Cross-arm query parity | ~2h fix |
| 5 | **Only 2/37 FHIR resources tested** — Patient + Observation are active. Encounter, Condition, Procedure, and 32 others are hydrated in fixture structs but never serialized. The benchmark measures a tiny fraction of real-world EHR workload. | 🟡 Generalizability | ~200h per phase |
| 6 | **No multi-bundle enrichment test** — Test 4 enriches once. Real EHRs enrich millions of times. The one-shot cost profile may not extrapolate. | 🟡 Realism | ~4h config |
| 7 | **Opaque resources are not query-navigable** — 1,444 Synthea `ImagingStudy` records take the opaque-JSON path, so stage 3 silently skips them. See [Result provenance](#result-provenance). | 🔴 Stage 3 measures less than it appears to | ~8h doc + corpus decision |

---

## Recovery under corruption — what redundancy buys

**Every arm now gets a real repair pass**, so this is no longer FastFHIR-with-
recovery measured against competitors-without. The competitor recovery is built
on what production actually does: HAPI's lenient-parse posture, Mirth's
preprocessor resynchronisation on the 3-char HL7v2 segment header, and
`jsonrepair`'s rule set for JSON. That makes the arms *stronger than production
default*, which is the right direction — production engines mostly reject
malformed input to an error queue rather than repair it.

| arm | what its recovery does |
|---|---|
| `fastfhir` | **REPAIRS.** `FF_Recovery::recover()` diagnoses against on-wire witnesses (self-offset `VALIDATION`, the duplicated tuple tag), `apply()` rewrites into a copy |
| `hl7v2` | resynchronises on the segment header, checks segment arity, then repairs the JSON inside ZFX payloads |
| `json` / `ndjson` | resynchronises on a record marker, then repairs structurally |
| `google_fhir` | TLV record resync only — **and see the caveat below** |

### Two curves, because the delta is the finding

Median % of content-verified leaves recovered, 20 replicates per point:

| k | fastfhir | hl7v2 | ndjson | json | google_fhir |
|---|---|---|---|---|---|
| 256 | 99.8 | 95.0 | 91.1 | 80.1 | 75.1 |
| 1024 | 98.2 | 79.8 | 70.6 | 54.8 | 29.9 |
| 2048 | **95.9** | 72.4 | 55.2 | 37.8 | 16.1 |

The same damaged bytes, read with recovery **OFF**:

| k | fastfhir | hl7v2 | ndjson | json | google_fhir |
|---|---|---|---|---|---|
| 256 | 96.5 | 87.6 | 75.0 | **0.0** | 17.9 |
| 1024 | 86.4 | 73.4 | 46.0 | **0.0** | 2.0 |
| 2048 | **74.6** | 65.6 | 27.0 | **0.0** | 0.4 |

**FastFHIR reads 74.6% of a badly damaged stream with recovery switched off.**
That is the redundancy working in the ordinary read path, before any repair
runs: a block whose `VALIDATION` word does not hold its own offset is simply not
followed. JSON reads **0.0% at every damage level** — one flip anywhere breaks
the whole-document parse, so everything it scores comes from the repair pass.
Those are different kinds of resilience and only the two curves separate them.

### The cost axis — the sharper finding

Median wall time to recover, timed around the read only:

| k | fastfhir | hl7v2 | ndjson | json |
|---|---|---|---|---|
| 1 | 63 ms | 35 ms | 36 ms | 46 ms |
| 256 | 63 ms | 404 ms | 904 ms | 2,798 ms |
| 2048 | **84 ms** | 645 ms | 2,509 ms | **5,638 ms** |

**FastFHIR's recovery cost is flat across a 2000× damage range.** It checks
witnesses that are already on the wire, so the work tracks the *stream*. The
others have no redundancy to repair from, so their recovery is a **search** —
resynchronise, then try candidate structural edits until one parses — and search
cost tracks the *damage*: json rises 123×, ndjson 70×, hl7v2 19×.

`fig9_recovery_cost` plots the honest single number, milliseconds per percentage
point recovered, because wall time alone rewards an arm that fails fast and
percentage alone hides its price.

⚠ **protobuf is excluded from the cost figure.** It has no recovery mechanism
and no recovery tooling — its parser validates *wire format only*, so corruption
that leaves a message well-formed is undetectable by construction, and the
ecosystem tools (protobuf-inspector, protoscope, blackboxprotobuf, protod)
reverse-engineer unknown schemas rather than repair damaged bytes. The TLV
resync in `arm_google_fhir_codec.hpp` is a heuristic this repo wrote so the arm
would not be measured against nothing. Its recovery *percentage* stays in fig8;
costing it would imply protobuf users pay that price for that benefit, and they
do neither.

### The NDJSON control — "JSON is fragile" was the wrong claim

`ndjson` is a **control, not a competitor**. Its artifact is generated *from*
`json.bin`, and both fingerprint to the identical digest (`3dfb8b1f…`, 34,839
units). Same resources, same leaf paths (they share `json_leaf_walk.hpp`), same
repair, same damage rule including the `\n` record separators. **Framing is the
only variable.**

| k | json ON | ndjson ON | json OFF | ndjson OFF |
|---|---|---|---|---|
| 1 | 99.9 | 100.0 | **0.0** | **99.9** |
| 256 | 80.1 | 91.1 | 0.0 | 75.0 |
| 2048 | 37.8 | **55.2** | 0.0 | **27.0** |

Reframing alone is worth **+17.4 points at k=2048** with recovery, and without
recovery it is the whole difference between 0.0% and 99.9% at k=1. NDJSON with
*no repair at all* beats JSON *with* full repair up to k=128.

So the supportable claim is **"single-document nesting is fragile", not "JSON is
fragile"**. A JSON Bundle entry frames a whole resource in one brace-delimited
object, so structural damage inside it costs the whole resource; NDJSON and
HL7v2 both delimit records with a byte, so damage costs one record.

### Caveat that must travel with these numbers

hl7v2's figure is flattered by the ZFX encoding: **82.4% of the flips its damage
model applies land on JSON punctuation inside ZFX payloads**, which this arm
chunks into 13,295 independently-parsed fragments — finer damage isolation than
a real ORU^R01, which packs far more into each OBX.

---

## Concurrent build scaling

**Thread count is part of parity, and it was not being controlled (fixed 2026-09-05).**
`bench/arm_fastfhir.cpp` had its parallel path commented out while
`bench/arm_json_fhir.cpp` ran `dispatch_apply_f` across every core. Test 1 was
therefore measuring **1 thread against 18**, which is why FastFHIR's advantage
was a flat ~5.4× at every corpus size — a constant ratio was the signature of
the defect, not a property of the formats.

### What each arm does now

| arm | Test 1 construction |
|---|---|
| `fastfhir` | `FIFO::Queue` worker pool — pre-allocated `Bundle.entry` array, one consumer per worker latched before the first push, each worker appends its resource and amends its own parent slot (the `FF_Ingestor.cpp` step-5 shape) |
| `json_fhir` | `dispatch_apply_f` per resource, independent `nlohmann` DOMs merged serially, then one `dump()` |
| `hl7v2` | serial |
| `google_fhir` | serial |

`--serial-build` forces all four to one thread. The run banner states which
configuration produced a given run, and **every Test 1 row carries `cpu_ns`**
(`getrusage`, all threads), so `cpu_ns / duration_ns` is the average number of
cores the stage occupied. A wall-clock table alone cannot separate "this arm is
efficient" from "this arm used more cores".

### Measured result — 18 cores buy 1.8×

64 MB corpus, 4,203 resources, `-c opt`, Apple M5 Pro (6 P + 12 E). Correctness
is not in question: `validate_FFHR_stream` returns 0, zero null entries, and
element parity is exact against the other three arms at every corpus size.

| configuration, 18 workers | wall | user CPU | sys CPU |
|---|---|---|---|
| one resource per queue task | 17.0 ms | 13.7 ms | 190 ms |
| 64 resources per task | 4.80 ms | 6.3 ms | 56 ms |
| 64 per task, arena pre-faulted | 1.40 ms | 14.7 ms | 2.5 ms |
| *single thread, for reference* | *2.29 ms* | *2.29 ms* | *0.01 ms* |

Three independent costs, each isolated by measurement:

1. **Park/wake rate.** `append_obj` on one Synthea resource is **0.545 µs** —
   the same order as a queue push and as one `__ulock_wait`/`__ulock_wake`
   pair — so one producer cannot feed 18 consumers and they park continuously.
   `notify_one` for `notify_all` changed nothing, so it is the rate, not a
   thundering herd. A queue task is therefore a **range** of resources;
   break-even is ~9 µs of work per task. Batching never splits a resource
   across threads — each is still built start-to-finish by one worker.
2. **First-touch page faults.** `Memory::create` reserves without committing,
   so N threads fault one fresh anonymous mapping and serialize on the kernel
   VM lock.
3. **Efficiency-core spillover — the one that mattered.** The pool defaulted to
   `hardware_concurrency()`. On one fixed bundle, arena pre-faulted:

   | workers | wall | user CPU | speedup |
   |---|---|---|---|
   | 1 | 2.12 ms | 2.07 ms | 1.00× |
   | **6** | **1.10 ms** | 4.41 ms | **1.93×** |
   | 8 | 1.53 ms | 7.91 ms | 1.39× |
   | 18 | 1.79 ms | 8.05 ms | 1.19× |

   The optimum is exactly the 6 P-cores. Both the engine and this arm now
   default to `FastFHIR::performance_core_count()`, and Test 1's paired ratio
   against the JSON arm moved **3.24× → 6.11×** on that change alone.

**The arena write head is not a bottleneck, and an earlier version of this
section said it was.** Instrumented 2026-09-05: a 64 MB build issues **4,498**
`claim_space` calls (`append<T>` claims a whole subtree per call), with 0 CAS
retries at one thread and 318 at eighteen. The residual after the core-count fix
is 1.93× on 6 cores, and it is memory-bound work rather than lock contention.
FastFHIR `TASKS.md` CONC-0 (done) and CONC-1/CONC-2 (rejected on this evidence);
`architecture.md` §7.5.

### Reproducing

Diagnostics are opt-in environment variables on `bench_harness`:

| variable | effect |
|---|---|
| `BENCH_FF_THREADS=N` | worker count (default `FastFHIR::performance_core_count()`) |
| `BENCH_FF_BATCH=N` | resources per queue task (default 64) |
| `BENCH_FF_MODE=split` | bypass the queue entirely — pre-assigned contiguous ranges, no parking. Isolates queue cost from arena cost |
| `BENCH_FF_PREFAULT=1` | touch every arena page before the clock starts. **Diagnostic only** — a run with this set is not comparable to one without |
| `BENCH_FF_TRACE=1` | print per-run `wall / user / sys / cores` for Test 1 |

```bash
BENCH_FF_TRACE=1 BENCH_FF_THREADS=8 BENCH_FF_BATCH=256 \
  ./bazel-bin/bench/bench_harness --runs 5 --bundle-targets-mb 64 --bundle-max-mb 64
```

⚠ The default configuration still puts first-touch page faults inside the
measured window — that is a real `Memory::create` cost, and moving it outside
would measure something the consumer does not get. The worker count is the
engine's own default, not a benchmark-tuned one.

---

## Setup

```bash
export FASTFHIR_REPO=https://github.com/<org>/FastFHIR.git
./generate_repo.sh
```

Google FHIR routine is enabled by default (separate Bazel build in `.external/google-fhir`).
Useful environment overrides:

```bash
# Google FHIR controls
export GOOGLE_FHIR_ENABLE=1
export GOOGLE_FHIR_REPO=https://github.com/google/fhir.git
export GOOGLE_FHIR_SYNC_REMOTE=0
export FORCE_GOOGLE_FHIR_REBUILD=0
export GOOGLE_FHIR_BAZEL_VERSION=7.7.1
export GOOGLE_FHIR_BAZELISK_VERSION=v1.22.1
export TEST_GOOGLE_FHIR_COMPONENTS=1
export TEST_BENCH_COMPONENTS=1
export GOOGLE_FHIR_CLEAN_ARTIFACTS=1

# Data source (this is the script's default; override only if you need a pinned release)
export SYNTHEA_DATA_URL=https://synthetichealth.github.io/synthea-sample-data/downloads/latest/synthea_sample_data_fhir_latest.zip
```

Set `GOOGLE_FHIR_ENABLE=0` to skip the Google FHIR routine entirely.

### Corpus location

`generate_repo.sh` downloads and extracts Synthea FHIR JSON into
**`datasets/synthea/`** at the repo root (`generate_repo.sh:21`). The harness
looks there; if the directory is missing or empty it exits with
`Synthea data not found`.

> ⚠ `bench/main.cpp:31` still hardcodes an absolute developer-machine path as
> its *primary* corpus location and only falls back to `datasets/synthea`. On
> any other machine the fallback is what runs. Removing the hardcoded path and
> adding a `--synthea-dir` flag is tracked in
> [TASKS.md § INFRA](TASKS.md).

FastFHIR itself is **not** vendored: `.external/FastFHIR` is a symlink to
`../FastFHIR`, resolved by `local_path_override` in `MODULE.bazel`. Bazel
builds it from source as a module dependency, which is how it inherits this
repo's `--compilation_mode=opt`.

---

## Run everything

`scripts/run_benchmark.sh` is the whole pipeline in the one order that works.
The stages below can each be run by hand — this script is what puts them in
sequence, checks that each one actually produced its output, and stops at the
first failure.

```bash
scripts/run_benchmark.sh            # full run
scripts/run_benchmark.sh --quick    # reduced ladder, no recovery sweep
scripts/run_benchmark.sh --help     # all flags
```

| # | Stage | Command it runs | Produces |
|---|---|---|---|
| 1 | build | `bazel build -c opt //bench:bench_harness //bench:bench_test_5` | `bazel-bin/bench/*` |
| 2 | timing | `bench_harness --results-dir DIR` | `DIR/metrics.csv`, `DIR/provenance.json`, `DIR/run.log` |
| 3 | artifacts | `bench_harness --dump-artifacts artifacts` | `artifacts/{fastfhir,json,hl7v2,google_fhir}.bin` |
| 4 | sweep | `scripts/recovery_sweep.py` | `results/recovery_curve.csv`, `results/_crashes.csv` |
| 5 | figures | `scripts/plot_benchmarks.py` | `DIR/figures/fig*.png`, `summary.md`, `summary.csv` |

The order is not a preference. Stage 3 is an **exclusive mode of the same
binary** — `main.cpp` returns before the timing study — so it is a second
invocation rather than a flag on the first. Stage 4 reads what stage 3 wrote,
and stage 5 reads all of it. Stage 5 must run from the repo root:
`fig8_recovery` opens `results/recovery_curve.csv` by a hard-coded relative
path.

Every stage is individually skippable (`--skip-build`, `--skip-timing`,
`--skip-artifacts`, `--skip-sweep`, `--skip-figures`) so one piece can be
re-run against existing output. `--with-tests` adds the two Bazel gates from
[Validate](#validate) after the build. Results land in `results/dev` unless
`--results-dir` says otherwise.

**`--quick` skips the recovery sweep rather than shortening it.** The sweep's
damage ladder and trial count are module constants in
`scripts/recovery_sweep.py`, and the published number is a median over those
trials — a shortened sweep is a *different* curve, not a cheaper one, and it
would be indistinguishable from the real thing downstream. Pass `--sweep` to
run the full sweep under `--quick` anyway. For the same reason the script
refuses to start a sweep while another one is running: `recovery_sweep.py`
holds `results/recovery_curve.csv` open for hours, so a second sweep truncates
the first one's output from under it.

A quick run prints `QUICK RUN — do not cite` on the way out, and stage 2's
provenance gate still applies: incomplete provenance exits 3 **before** the
first measurement, so a run that cannot become an artifact fails in seconds
rather than at the end of the ladder.

---

## Build

```bash
bazel build -c opt //bench:bench_harness
```

PostgreSQL persistence is opt-in — the default target links no libpq, so it
builds anywhere. For `--db` support build `//bench:bench_harness_pg`, which
needs libpq on the host (`brew install libpq`).

## Run Benchmark

```bash
./bazel-bin/bench/bench_harness
```

With PostgreSQL persistence:

```bash
./bazel-bin/bench/bench_harness \
  --db "host=localhost port=5432 dbname=fhir_benchmark user=postgres password=postgres"
```

### CLI Arguments

| Argument | Default | Description |
|---|---|---|
| `--iterations N` | 1 | Measurement iterations per bundle run |
| `--warmup-iterations N` | 1 | Warmup rounds (0 to disable) |
| `--runs N` | 10 | Number of bundle build + arm execution repetitions |
| `--bundle-max-mb N` | 256 | Cap on largest target bundle size (MB) |
| `--bundle-max-mb-explicit` | off | If set, only the specified max is used (no sweep) |
| `--bundle-targets-mb a,b,c` | 1,2,4,8,16,32,64,256 | Explicit sweep ladder |
| `--seed N` | 20260825 | Bundle composition seed. `--seed 0` is random — and a random workload cannot become an artifact |
| `--serial-build` | off | Force every arm's Test 1 construction loop onto one thread. Default is parallel for `fastfhir` and `json_fhir` (`hl7v2` and `google_fhir` are serial either way). Run the ladder both ways to separate per-resource encoding cost from core scaling — see [Concurrent build scaling](#concurrent-build-scaling). `RESULTS_DIR` must differ between the two |
| `--results-dir DIR` | none | Write the release artifact (`provenance.json`) here. **Refuses, and exits 3, if the provenance record is incomplete** |
| `--profile STR` | auto | Pin `FASTFHIR_PRODUCTION_PROFILE`. Needed when several CMake caches disagree — see below |
| `--db connstr` | none | PostgreSQL connection string (`//bench:bench_harness_pg` only) |

### Output Columns (stdout CSV)

```text
arm,test,duration_ns,ops,bytes_in,bytes_out,target_mb,patients_in_bundle,cpu_ns
```

Example:
```
fastfhir,test_1_serialize,254875,0,0,612312,1,1,254102
json_fhir,test_1_serialize,1171709,0,0,356672,1,1,4106233
hl7v2,test_1_serialize,3301792,0,0,407628,1,1,3298144
```

`bytes_in` / `bytes_out` are **wire bytes** crossing that stage's boundary, and
they are what makes any size or per-byte throughput claim measurable at all —
before them the harness emitted only nanoseconds:

| Stage | `bytes_in` | `bytes_out` |
|---|---|---|
| `test_1_serialize` | 0 — the input is POCOs, not wire | sealed payload size |
| `test_1_compact` | 0 | compact archive size (FastFHIR arm only — withheld unless the IN-E losslessness gate passes) |
| `test_2_random_access` | 0 | id bytes read — the cross-arm parity accumulator (`ops` = reads) |
| `test_2_compact` | 0 | id bytes read over the compact archive (FF arm only) |
| `test_3_query` | 0 | 0 — the 17-field census (full traversal) |
| `test_3_selective` | 0 | 0 — the early-out 'find the cholesterol results' query |
| `test_3_compact` | 0 | 0 — same census over the compact archive (FF arm only) |
| `test_4_enrich` | source stream | enriched stream |
| `test_4_compact` | — | **no row**: the API refuses to open a Builder on a compact archive (write-once, CAPI-10) |

**0 means "not applicable to this stage", never "measured zero".** A serialize
stage reporting 0 bytes out is a defect and the harness warns about it.

`target_mb` is the *requested* corpus size and is not a measurement — do not use
it as a denominator.

`cpu_ns` is user+sys CPU summed over **every thread in the process** across the
same window `duration_ns` measures, so `cpu_ns / duration_ns` is the average
number of cores the stage occupied: 1.0 is serial, 18.0 saturates an 18-core
host. It exists because a wall-clock number alone cannot distinguish an arm that
is efficient from one that used more cores, and two of the four arms build in
parallel — see [Concurrent build scaling](#concurrent-build-scaling). **-1**
means the stage did not sample it (only `test_1_serialize` does today).

### Result provenance and the artifact gate

Every run prints a provenance block to stderr, because the two things that most
change these numbers are invisible in them: the compiled profile and the
compilation mode.

```text
[provenance] fastfhir a9fd4e9 @ a9fd4e9a7430 DIRTY
[provenance] profile us-core,billing,medication-admin,supply (operator) -- 80 code-system enums, 44 generated .cpp, 36 resource types
[provenance] build opt clang 21.0.0 on macos/arm64 (Apple M5 Pro)
[provenance] corpus 342 docs, 1292 MB, sha256 958b1ceb2642
[provenance] benchmark @ 38705db50262 DIRTY, seed 20260825
```

With `--results-dir` the same record is written as `provenance.json` — and the
harness **refuses to write it** (exit 3) if any required field is unestablished.
Two refusals are deliberate and will bite:

- **`compilation_mode` must be `opt`.** The same code runs ~10x faster at -O2
  than -O0, and nothing in a results row says which you built. This is the Debug
  trap, enforced.
- **An ambiguous profile is rejected.** There is no runtime signal for
  `FASTFHIR_PRODUCTION_PROFILE`, so it is read from the CMake caches under
  `../FastFHIR/*/` — and on a working machine those disagree (three build trees,
  two different values here). The newest wins for the stderr summary, but an
  artifact requires `--profile` to pin it. `codesystem_enums`, `generated_cpp`
  and the resource list are recorded alongside as corroboration: the profile
  string is a claim, those are evidence from the tree actually compiled in.

The corpus digest is a SHA-256 over a manifest of every file's name, size and
content hash — so a regenerated corpus is never mistaken for the same one. It is
memoized in `datasets/.corpus_sha256.cache` keyed on (file count, total bytes,
newest mtime); the first run after a corpus change re-hashes ~1.3 GB.

---

## Validate

```bash
# Bazel test
bazel test //bench:timing_conformance_test

# Direct execution
./bazel-bin/bench/bench_timing_conformance
```

The conformance test verifies:
1. All metrics have positive duration values
2. FastFHIR and JSON arms both report exactly `patients=1`
3. Both arms report matching birthdate values

### Read-path gate

The one validation target that builds today. It reports whole-document
traversal cost as an average per Bundle entry and **fails (exit 1) if any
average exceeds 50 µs/entry**:

```bash
bazel run //bench:read_path_bench -- /path/to/bundle.ffhr
```

The binary self-reports its optimization mode. The 50 µs/entry bound is a
regression gate for *optimized* builds — a Debug FastFHIR measures ~10× slower
on these paths and will fail the gate for the wrong reason. See
[`read_path_bench.cpp.md`](bench/read_path_bench.cpp.md).

---

## Analyze

`scripts/plot_benchmarks.py` renders the figures — stage 5 of
[Run everything](#run-everything) calls it, and it is also usable on its own
against any captured CSV:

```bash
scripts/plot_benchmarks.py --csv results/dev/metrics.csv \
                           --results-dir results/dev \
                           --out results/dev/figures
```

It needs `--results-dir` to find `provenance.json`: a run whose provenance gate
failed is stamped **PROVISIONAL — NOT AN ARTIFACT** on every figure. `fig8_recovery`
additionally reads `results/recovery_curve.csv` (stage 4) and is skipped if that
file is absent, so run it from the repo root.

Use `notebooks/benchmark_results.ipynb` to read from PostgreSQL.

---

## Known Issues & Incident Log

### Open

| Issue | Document | Summary |
|---|---|---|
| **Harness does not compile** | [`TASKS.md`](TASKS.md) | Upstream FastFHIR API redesign; all four arms affected |
| Hand-rolled CODE encoding is wire-invalid | [`TASKS.md`](TASKS.md) § PORT-7 | `bench_test_1.hpp` open-codes a flag scheme that upstream moved from bit 30 to bit 31 |
| Opaque resources not query-navigable | [Result provenance](#result-provenance) | 1,444 Synthea `ImagingStudy` records skip stage 3 |
| No build provenance in results | [`TASKS.md`](TASKS.md) § PROFILE | Profile + upstream SHA are invisible to Bazel and unrecorded |
| Google FHIR static link failure | [`arm_google_fhir.cpp.md`](bench/arm_google_fhir.cpp.md) | Static archives cause undefined symbol explosions from absl/protobuf/utf8_range |
| Google arm `birthdate` unit mismatch | [`notes.md`](notes.md) | Microsecond epoch vs ISO in every other arm; arm also excluded from `validate_parity()` |
| Resource coverage gap | [`RESOURCE_COVERAGE_ANALYSIS.md`](RESOURCE_COVERAGE_ANALYSIS.md) | Only 2/37 compiled FHIR resources tested |
| CODE assignment type expectations | [`FFHRnotes.md`](FFHRnotes.md) § 4 | Now a hard compile error: `TypeTraits<std::string>` is undefined |

### Resolved upstream (kept for history)

| Issue | Resolution |
|---|---|
| Missing installed headers | Moot — consumption is a Bazel module, not an install tree. CMake also now installs an explicit header set. |
| Ingestor symbols not exported | Misdiagnosed. `fastfhir_ingestor` is a *separate target*, which this repo links via `//:fastfhir_runtime`. |
| Relative `../generated_src/` includes | Fixed upstream — no such includes remain in `include/`. |
| `Fields` vs `FieldKeys` namespace | Fixed — both namespaces coexist in `FF_FieldKeys.hpp`. |
| README examples not compile-tested | Fixed — upstream `py_readme_examples` runs them verbatim. |
| FastFHIR ingest CAPACITY errors | CMake-era issue (`SIMDJSON_THREADS_ENABLED` parity with `ff_ingest`). Not reachable under the Bazel module build. |
