# FastFHIR-benchmark — Task Backlog

**This file is the only backlog.** Everything else is a companion of a different
kind, and none of them carry open work items:

| Document | Kind | Role |
|---|---|---|
| [`notes.md`](notes.md) | **field report** | what was silently broken during the port, and how it was found |
| [`TODO.md`](TODO.md) | **design spec for Instrument G** | the four resilience tests, in detail — not a second task list |
| `../FastFHIR/TASKS.md` | upstream backlog | our API asks live there as **CAPI-1…CAPI-6** |

If a task exists, it is below. If a design exists, it is in one of those.

---

## Decisions — standing, do not re-litigate

Recorded because each of these has already cost a wrong turn, and because a
benchmark that changes its own rules mid-flight cannot be audited.

### D1 — The macro-guarded assignment layer is the parity architecture

**Decided by Ryan, 2026-08-26. Paramount requirement.**

Every field is written by one `assign_<resource>_<field>()` per arm, selected by
`#if defined(ARM_FASTFHIR) / ARM_JSON / ARM_GOOGLE_FHIR / ARM_HL7V2`
([`bench/bench_test_1.hpp:94-3461`](bench/bench_test_1.hpp:94)). A reviewer can
read all four implementations of the same field side by side in one file and
check that they do equivalent work. **That auditability is the point of the
design and it outranks every other consideration**, including line count.

Consequences that follow, and are therefore also decided:

- **The per-arm inline namespace is mandatory**, not hygiene. One type name with
  four definitions across four TUs is an ODR violation, and it was the root
  cause of every "impossible" crash during the port (notes.md §1).
- ~~notes.md §1's "serious alternative" — arms as templates or classes over a
  shared interface, compiled once~~ — **rejected.** It trades the side-by-side
  audit for a smaller ODR surface; the ODR surface is already closed by the
  namespaces. Do not propose it again.
- ASan in CI is the compensating control for keeping the pattern (HY-1).

### D2 — `value[x]` is tiered, not stripped

**Decided 2026-08-26**, after checking what the other formats can actually do
rather than assuming.

The open question was whether protobuf and HL7v2 can represent FHIR choice types
at all, and whether `value[x]` should be dropped from the comparison and claimed
as a FastFHIR-only capability. **Both can represent it. Verified:**

| Arm | Native choice representation | Evidence |
|---|---|---|
| Google protobuf | `ValueX` message with a `oneof choice` over 11 variants | `third_party/google_fhir/proto/google/fhir/proto/r4/core/resources/observation.proto:159-176` |
| HL7v2 | OBX-2 value type (`NM`/`ST`/`CWE`/`CE`/`SN`) selecting the type of OBX-5 | [`bench/hl7v2_message.hpp:71`](bench/hl7v2_message.hpp:71); the arm already switches on the tag at [`bench/bench_test_1.hpp:2785`](bench/bench_test_1.hpp:2785) |
| simdjson/nlohmann | `valueQuantity` / `valueString` / … natively | `write_choice`, [`bench/bench_test_1.hpp:2725`](bench/bench_test_1.hpp:2725) |
| FastFHIR | `ChoiceEntry` + `RECOVERY_TAG` | blocked across arenas — CAPI-3 |

**So the blocker is ours, not theirs.** No arm is short of a representation; the
FastFHIR `ChoiceEntry` carries a source-arena offset that cannot cross arenas
(notes.md §4), so the harness blanks the field in *all four* arms to keep the
inputs byte-identical. Stripping `value[x]` and claiming it as a differentiator
would assert a capability gap that **does not exist**, which is the exact
opposite of auditable.

The measured surface is therefore tiered, and **no `value[x]` number is ever
reported without its tier**:

| Tier | Variants | Arms that carry it losslessly | Status |
|---|---|---|---|
| **S** — scalar | `valueString`, `valueBoolean`, `valueInteger`, `valueDecimal` (and `valueCode` when it resolves to a dictionary index) | all four | unblocked; only the HL7v2 arm still mocks it (PA-2) |
| **B** — block | `valueQuantity`, `valueCodeableConcept`, `valueReference`, `valueCoding`, `valueAddress`, `valueRange`, `valueRatio`, `valuePeriod`, `valueSampledData` | FastFHIR, JSON, protobuf in full; HL7v2 for `Quantity` (NM+OBX-6) and `CodeableConcept`/`Coding` (CWE) only — `Range`/`Ratio`/`SampledData` degrade, **and that lossiness is a result to report** | blocked on CAPI-3 |
| **X** — choice-bearing extensions | `_valueString` and other `_`-prefixed primitive extensions; unknown extensions inside a choice | FastFHIR only — protobuf JSON does not implement the model at all | belongs to **Instrument F**, never to a timing row |

**Corpus census (342 Synthea fixtures, 246,878 choice occurrences, 2026-08-26):**

| Variant | Count | Share | Tier |
|---|---|---|---|
| `valueQuantity` | 142,517 | 57.7 % | B |
| `valueCodeableConcept` | 86,421 | 35.0 % | B |
| `valueReference` | 4,845 | 2.0 % | B |
| `valueInteger` | 4,396 | 1.8 % | S |
| `valueString` | 4,331 | 1.8 % | S |
| `valueBoolean` | 1,680 | 0.7 % | S |
| `valueDecimal` | 1,344 | 0.5 % | S |
| `valueCoding` | 672 | 0.3 % | B |
| `valueCode` | 336 | 0.1 % | S *or* B — portable only when the payload is a dictionary index (MSB clear) |
| `valueAddress` | 336 | 0.1 % | B |

**Tier S is 4.8 % of the choice surface; Tier B is 95.1 %.**

**So Tier S is a correctness win, not a coverage win.** Fixing PA-2 removes a
false blanket statement ("no arm serializes `value[x]`") and exercises the
machinery end to end, but it leaves **95 % of the choice surface unmeasured**.
Do not let it read as "`value[x]` is now covered". CAPI-3 is the real unblock,
and this census is the argument for its priority.

Tier X is where the genuine differentiator lives (README § 2.4, § 5.4). It is a
**conformance** result, not a speed result, and it is provable without any
timing methodology to defend — which makes it the strongest artifact available.

### D3 — Serialization model: field-by-field for all four arms

**Decided 2026-08-26**, resolving the open choice in notes.md §3.

notes.md §3 offered whole-object serialization for every arm as its
recommendation. **Rejected on two grounds:**

1. **HL7v2 has no whole-object serializer.** There is no generic "serialize this
   FHIR resource" path in v2.x — an ORU^R01 is built segment by segment by
   definition. Whole-object parity is therefore impossible for one of the four
   arms, and an option that only three arms can take is not a parity model.
2. It conflicts with **D1**.

So: **one canonical parity field set, written field by field, by every arm,
through the macro layer.** The FastFHIR arm's current `append_obj(item.patient)`
(notes.md §3) is a temporary deviation — it writes every POCO field while the
others write ~25 — and it must be replaced by field-by-field assembly, which is
blocked on **CAPI-1** upstream (no public API for inline-block arrays).

A separate, clearly labelled **native whole-object row** may be reported
alongside, per arm that offers one (`append_obj`, `SerializeToString`,
`json::dump`). It is a different measurement and never shares a table cell with
a parity row.

### D4 — Test 2 is random access; the materialize walk is retired

**Decided 2026-08-26 (Ryan).** The former Test 2 (materialize) measured a full
traversal in LAYOUT order — a contiguous tape's best case and an offset-indexed
layout's worst — and **no consumer reads a bundle in write order**: you jump to
the resources you care about. It was replaced by the random-access probe
(IN-B, formerly Test 5) as **the** Test 2: pick N random `Bundle.entry`
ordinals, navigate to each from the root, read the resource's id. Every lookup
pays its own path cost — that asymmetry *is* the WF-1.1 claim (O(1) jump vs
O(N) scan), and it is what real medical-data retrieval does.

Consequences:

- The stage string is **`test_2_random_access`**, deliberately NOT reusing
  `test_2_materialize` — legacy CSVs remain distinguishable, and the plot
  script drops their rows with a "pre-D4 data" skip rather than mislabelling
  them.
- PA-5 (node-count normalization) is **resolved by retirement**; PA-11's
  walk analysis is retained as the record of why the walk was wrong evidence.
- Test 2 runs **before Test 4** in every arm (the in-place enrich, PA-9, would
  otherwise move the ordinal space under it).

---

## Progress log

Newest first. One line per change, with what it did and did not settle.

- **2026-09-17** — **PA-10c: APPEND-1 implemented upstream (uncommitted); Test 4 uses it.**
  - Stream growth per enrich is now a constant 4,619 B, where it was up to
    2.13 MB at 256 MB. FastFHIR enrich time at 256 MB fell from 0.90 to
    0.67 ms.
  - FastFHIR gained `FF_BundleAppendEntries` and `serialize_bundle_array`, plus
    a unit test checked against its own `StreamMap`. README Example 5 now
    really attaches its Observation.
  - Filed upstream: APPEND-2 (map-guided move instead of relocation), E15
    (pre-existing `ctest -j8` flake in `cpp_test_7/8`), E16 (FastFHIR's own
    Bazel build fails under Xcode 27).
  - **Not settled:**
    - CI can't measure this until FastFHIR's changes are pushed.
    - The FastFHIR arm's appended Observation is 4.5 KB against JSON's 2.4 KB:
      a larger block, not overhead from the append.

- **2026-09-16 (evening)** — **PA-10a/b: Bundle written last; enrich storage cost measured.**
  - What changed: the FastFHIR arm now writes resources first and the
    Bundle + entry array last (`BENCH_FF_BUNDLE=backfill` keeps the old
    layout). Test 4 reports `bytes_written` / `bytes_overwritten`, and fig4
    shows both.
  - At 16 MB, FastFHIR writes 243 KB (5% of the stream) and overwrites 98 B.
    JSON and Google FHIR rewrite 100% of the stream. HL7v2 appends 2.7 KB and
    overwrites nothing.
  - **Found:** FastFHIR already rewinds its write head when reopening a sealed
    stream (onto the checksum block). APPEND-1's rollback generalizes a
    primitive that exists, rather than adding a new one.
  - **Not settled:**
    - FastFHIR's appended bytes still grow with N until APPEND-1 (PA-10c).
    - The HL7v2 timer includes a full-stream copy (PA-10d).
    - The A/B needs a new baseline.

- **2026-09-16 (later)** — **PB-2a–c: workflow, same-box A/B, publish gate.**
  - What changed: `bench-release.yml` runs interleaved A/B timing
    (`scripts/bench_ab.py`), then the candidate's artifacts, sweep and figures,
    then `scripts/publish_check.py`. It publishes only on a clean, full,
    parity-passing run.
  - New harness flags: `--replicate-first` and `--fastfhir-root`.
  - Verified: both sides measure identical bundles, and each provenance names
    its own commit. The smoke test turned out to be an A/A (a tests-only
    commit gives a byte-identical binary) with paired ratios of 0.83–1.60 at a
    1 run × 2 replicate ladder. That is the noise floor tiny ladders must never
    be quoted against.
  - **Not settled:**
    - The workflow has not run on a real runner yet (PB-2d).
    - The full-ladder noise floor on this Mac is unmeasured: run one A/A with
      `--baseline` equal to the candidate.

- **2026-09-16** — **PB-1: the library under test can be pinned.**
  - What changed: `scripts/pin_fastfhir.sh` and `run_benchmark.sh
    --fastfhir-ref` build against a clean checkout at one commit and profile.
    `provenance.json` now names the tree Bazel actually compiled and records
    host details.
  - Verified: a quick run pinned to `ee578e4` exits 0, all four arms handle
    412/412 resources at every stage, and the pinned `generated_src` is
    byte-identical to the live one.
  - Build fix: Xcode 27 broke the build ("absolute path inclusion" of
    `SDKSettings.json`). `MODULE.bazel` now declares `apple_support` first so
    its Apple toolchain wins toolchain resolution.
  - **Not settled:**
    - The corpus still links into `../FastFHIR/build/` (PB-3b).
    - Same-box A/B (PB-2b) does not exist yet.
    - Cloud choice is Google Cloud; nothing under `infra/` exists yet.

- **2026-08-26 (review)** — Reviewing the D4/lens/compact commit surfaced
  **PA-14 / CAPI-13**: `as<ObservationData>()` drops singular block fields, so
  `Observation.code` and `subject` have never reached any arm's output and the
  LOINC query has never matched. The lens retool in that commit is a genuine
  fix (Test 3 `as<ObservationData>()` → node lenses, 5.1x, byte-identical
  output) but it was measured against a workload with 0 matches, so the number
  does not stand yet. Also restored the full-traversal walk as
  [`bench/walk_diagnostic.hpp`](bench/walk_diagnostic.hpp) — **diagnostic only,
  never a stage, off unless `BENCH_WALK=1`** — because upstream CAPI-7/CAPI-8
  cited measurements the retirement had made unreproducible. Re-measuring
  corrected those figures: −22.4% (strlen) and −19.2% (`entries()` allocation)
  reproduce; the previously claimed combined −35.1% does **not**, and is
  withdrawn upstream.

- **2026-08-26 (later)** — **JSON arm serializes effectiveDateTime correctly —
      the Test 3 FF↔JSON gate is now clean on every census field.** The hydrated
      `ChoiceEntry` for a datetime choice carries the RAW 63-bit packed slot as
      `uint64_t`; `write_choice` emitted it as a JSON number
      (`"effectiveDateTime":1619552459707908099`), which the census's
      `is_string()` check counted as 0 (FF 692 vs JSON 0). The JSON arm now
      decodes through `FF_UNPACK_DATETIME` + `FF_FORMAT_DATETIME` (the
      `FF_DATETIME` type path) and emits canonical ISO-8601 — matching
      print_json on the FF side. Filed upstream as **CAPI-11** (ChoiceEntry
      exposes the raw slot, undocumented). The remaining exit-2 causes are now
      only the HL7v2 arm's coverage gaps (PA-6).
- **2026-08-26 (later)** — **Compact stream runs tests 1–4 (test_*_compact).**
      The FF arm compacts once per sample, then runs random access and the
      census over the compact archive (`test_2_compact` / `test_3_compact`)
      with the same lens reads — the claim under test: the reader is
      layout-agnostic, so compact ≈ standard speed. The compact census must
      answer identically to the standard one (mismatch warns). Measured at
      4 MB: random access 1.26× and query 2.3× slower than standard — a fixed
      parser/root cost in the dense compact root that may amortize at scale.
      **Test 4 compact cannot exist**: the API refuses to open a Builder on a
      compact archive — compact is write-once. Filed upstream as **CAPI-10**;
      the row is skipped with a once-per-run note, by design.
- **2026-08-26 (later)** — **Test 3's FF query is lens-based now (PA-7 scope
      shrinks).** The FF arm read every observation via
      `as<ObservationData>()`, deserializing the whole POCO — including fields
      the census never classifies — which cancelled the O(1) per-field access
      Test 2 demonstrates. Rewritten to read only `code.coding[*]` (index-walked,
      no `entries()` allocation), the `value`/`effective` choice tags, `issued`
      presence, and `component[*].value` tags, all through node lenses. Counts
      are byte-identical to the POCO path (verified against the parity gate).
      Measured: FF Test 3 **20.9 µs vs JSON 161 µs at 4 MB (7.7×)** and
      **116 µs vs 242 µs at 16 MB (2.1×)** — was ~parity (0.90×). One trap:
      `issued` is a packed date/time slot, and `as<std::string_view>()` on it
      throws ("Node is not a string or code") — presence-only reads are the
      census's need anyway. `birthDate` still pays `print_json` (CAPI-4).
- **2026-08-26 (later)** — **Compact archive size lands in Test 1 (IN-E,
      partial).** The FastFHIR arm emits `test_1_compact` —
      `Compactor::archive()` size — gated on losslessness: the compact stream
      must re-parse to JSON semantically identical to the standard stream's
      (nlohmann comparison, not string equality — layouts may legitimately
      reorder fields), or the row is withheld with a loud warning. `fig2`
      plots it (dashed) against the JSON baseline. Verified at 4 MB: compact
      257,646 B vs standard FFHR 505,670 B vs JSON 231,274 B; gate passed.
      Still IN-E: the full size table (gzip(JSON), protobuf, sparse/dense
      separation).
- **2026-08-26 (later)** — **D4: Test 2 is random access; the materialize walk is
      retired.** The former Test 2 was a layout-order full traversal — a tape's
      best case, a query's worst. The random-access probe (IN-B, ex-Test 5) took
      the slot: [`bench/bench_test_2.hpp`](bench/bench_test_2.hpp), stage
      `test_2_random_access`, all four arms, cross-arm byte gate (exit 2 on
      mismatch). Legacy `test_2_materialize` rows are dropped with a "pre-D4
      data" skip; the plot script's 2×2 grids now fit the four stages exactly
      (which also fixed a silent test-5 truncation in fig1/fig5). Verified:
      build green, conformance passes, 4 MB smoke run — all arms emitted
      identical 72,000-byte accumulators (2000 reads × 36-char ids).
- **2026-08-26** — Filed six public-API asks upstream as
  `../FastFHIR/TASKS.md` **CAPI-1…CAPI-6** (inline-block array writer;
  validator vs deserializer disagreement; cross-arena `ChoiceEntry`; packed
  date/time reader; `TypeTraits<std::string>`; stale doc comment). Filed two
  claims-alignment items as **I3.6** (the orjson citation cites a result that
  does not exist) and **I3.7** (the −66 % compact figure predates the
  compaction data-loss fix by four months and no test pins it). **Not
  committed** — `../FastFHIR` is a symlinked live tree.
- **2026-08-26** — **Scaled the sweep to real bundles and attributed the Test 2
  gap.** Two findings. (1) `--bundle-max-mb` never meant bundle size — it counts
  ingested source, so the old "256 MB" runs were **8 MiB bundles** (PA-13). At
  `--bundle-targets-mb 4096` the FastFHIR stream is **316 MiB** (929 patients,
  6.2 GB RSS), which is the first time this repo has measured a bundle at the
  scale its docs claim. (2) The Test 2 gap is ~35% API overhead — 18.5% from
  `entries()` allocating per array, 16.6% from the reflective `strlen` — with a
  ~1.94x residual that is **not yet attributed**. Filed CAPI-8 upstream. The
  ratio is flat at 2.4-3.1x across a 300x range of bundle size and narrows
  slightly at the top (3.07 -> 2.42), so it is a constant factor, not a scaling
  divergence. Serialize at 316 MiB: FastFHIR **1.95 GB/s** vs the JSON arm's
  0.375 GB/s.
- **2026-08-26** — **Violins and the speedup panel restored.** The pre-IN-0
  notebook had grouped violin plots and a speedup chart; dropping them in the
  rewrite lost the run-to-run spread, which is the thing that says whether a gap
  between two arms means anything. Both are back in the script
  (`fig5_distribution`, `fig6_speedup`). The speedup panel is a lollipop
  anchored at 1.0 rather than bars — on a log axis a bar length is arbitrary,
  and distance from parity is the quantity that matters. **Sub-parity results
  are drawn and labelled like any other**; a speedup chart that only shows wins
  is advertising. Current medians: FastFHIR wins 9 of 12 arm×stage comparisons
  (4.91x vs simdjson on serialize *while writing 2.2x more bytes*; 53x on
  enrich), loses Test 2 to simdjson (0.37x, cause measured in PA-11), and is
  within noise on Test 3 vs JSON (0.90x) and Test 4 vs HL7v2 (0.95x).
- **2026-08-26** — **Figures.** [`scripts/plot_benchmarks.py`](scripts/plot_benchmarks.py)
  renders five figures + a table view from the CSV (or Postgres) and stamps every
  one with provenance, the applicable caveats, and **PROVISIONAL — NOT AN
  ARTIFACT** when the gate fails; [`notebooks/benchmark_results.ipynb`](notebooks/benchmark_results.ipynb)
  is now a thin wrapper that shells out to it rather than a second copy of the
  plotting logic. Palette validated (worst adjacent CVD ΔE 9.1; two slots below
  3:1 contrast, so direct labels + table view are mandatory, not decoration).
  Building them surfaced **PA-11** (Test 2 is a full-traversal measurement, not a
  zero-copy one — 2,396 heap allocations in the FastFHIR walk, and simdjson
  isn't materializing anything) and **PA-12** (`target_mb` controls nothing
  below ~4 MB). Filed **CAPI-7** upstream. Also caught and fixed a walk I broke
  mid-session in the JSON arm — the `[warn] materialize touched 0 nodes` guard
  from HY-2 is what would have caught it, and it is not wired to fail the run.
- **2026-08-26** — **IN-0 landed.** Wire bytes (`bytes_in`/`bytes_out`) in the
  CSV and PG schema, `bench/provenance.hpp` + `provenance.json` with a refusal
  gate, `provenance_test` green (11 checks). Building it surfaced three things
  the harness could not previously see: the FastFHIR arm emits **2.2x** the
  JSON arm's wire bytes at Test 1 (PA-1, now quantified); its Test-4 enrich
  **shares the source arena** and mutates in place, so `source_bytes` was being
  read post-append (fixed; asymmetry filed as PA-9); and one appended
  Observation costs it **28 KB** against 198–502 bytes elsewhere (PA-10).
  Did **not** settle: no size number is publishable yet — PA-1 is open, and the
  profile on this machine is ambiguous until pinned with `--profile`.
- **2026-08-26** — Censused `value[x]` across the full 342-fixture corpus:
  Tier B is **95.1 %** of 246,878 occurrences, Tier S only 4.8 %. This
  demoted PA-2 from "the quick win that unblocks `value[x]`" to "a correctness
  fix covering 5 % of it", and promoted CAPI-3 to the dominant upstream
  dependency. Sequencing in handoff.md amended to match.
- **2026-08-26** — Recorded D1/D2/D3. D2 required checking the protobuf and
  HL7v2 choice representations rather than assuming they were absent; both are
  present, which changed the design from "strip and claim" to "tier and
  report".
- **2026-08-26** — `TODO.md` re-scoped as the design spec for **Instrument G**
  and unblocked (its blocker was PORT, closed 2026-08-25).
- **2026-08-25** — PORT closed. Harness builds and runs; 161 metric rows;
  numbers deliberately unpublished (notes.md §8).

---

## ✅ PORT — restore the build against the FastFHIR public API redesign

**Opened** 2026-08-24 · **Closed** 2026-08-25 · **Status** DONE

`bazel build -c opt //bench:all` is green, `//bench:timing_conformance_test`
passes, and a 1–16 MB sweep produces 161 metric rows. Exit code 2 signals a
cross-arm parity mismatch, not a crash.

| ID | What it was | Resolution |
|---|---|---|
| PORT-1 | `Builder::set_root` / `finalize` private | `make_builder()` + `seal_stream()` in `harness.hpp` wrap `FF_BuilderSetRoot` / `FF_BuilderFinalize`. |
| PORT-2 | `Ingest::SourceType` gone | `FF_SOURCE_FHIR_JSON`; `extension_filter` pinned to `FILTER_ALL_KNOWN`; `payload_capacity` passed so simdjson parses in place. |
| PORT-3 | Code enums unprefixed | `FF_`-prefixed throughout; `FF_UNSET` still falls through to each arm's `default:` — see PA-4. |
| PORT-4 | `*ToString` removed | `serialize_<Enum>()`. |
| PORT-5 | `ExtensionData::ext_ref` | → `::url`, null is `FF_NULL_UINT32`. |
| PORT-6 | Attachment `data`/`hash` | now `unique_ptr`, null-checked. |
| PORT-7 | Hand-rolled CODE encoding | routed through `ENCODE_FF_CODE`. Never executes on this corpus — every code is a dictionary hit. |
| PORT-8 | `TypeTraits<std::string>` undefined | date/time POCO fields assigned as `string_view`. Filed upstream as CAPI-5. |
| PORT-9 | Re-validate | conformance green; numbers not published — notes.md §8. |

---

## ▶ PA — parity repair

**Status** OPEN · **Blocks** publishing any number

- [ ] **PA-14. 🔴 The query has never matched anything, in any arm.**
      Found 2026-08-26. Test 3 searches Observations for LOINC 2085-9 and
      reports **0 matches over 24,583 observations**, while the corpus carries
      ~3.75 instances of that code per Synthea file. Cause is upstream and is
      now **CAPI-13 (P0)**: `Node::as<ObservationData>()` silently drops
      **singular** block-typed fields. Same nodes, two readers:

      | field | cardinality | lens | `as<ObservationData>()` |
      |---|---|---:|---:|
      | `code` | 0..1 | **93/93** | **0/93** |
      | `subject` | 0..1 | **93/93** | **0/93** |
      | `category` | 0..* | 93/93 | 93/93 ✔ |
      | `component` | 0..* | 13/13 | 13/13 ✔ |

      Every arm serializes from the hydrated POCO, so **no arm has ever written
      `Observation.code`**. Consequences, all of which invalidate current
      numbers:

      1. **Test 3 is not a query.** The match branch is dead code in all four
         arms; the stage measures a scan that cannot succeed. No Test 3 number
         is publishable, including the 5.1x FF-over-JSON result from the lens
         retool — that fix is real, but it was measured on a workload with no
         matches.
      2. **Test 1 under-serializes.** Every arm writes Observations without a
         code, so every wire size in this repo is smaller than real FHIR.
      3. **Bundles are not partitioned by patient.** The fixture filters on
         `resource.subject` matching the patient id
         ([`bench/harness.hpp:501`](bench/harness.hpp:501)); `subject` is always
         null, so the filter never runs and every observation is attached to
         every patient.

      Reproduce: `BENCH_CODE_CENSUS=1 ./bazel-bin/bench/bench_harness --runs 1
      --bundle-targets-mb 1`. **Blocked on CAPI-13** for the real fix; until it
      lands, Test 3 must be reported as "scan-only, 0 matches by construction"
      or held back entirely.

- [ ] **PA-1. Test 1 is not at parity.** The FastFHIR arm serializes every POCO
      field via `append_obj`; the other three write the ~25 fields the macro
      layer covers. Per **D3**, converge on field-by-field for all four.
      *Blocked on upstream CAPI-1* — there is no public API for writing an
      inline-block array such as `Observation.category`. Until it lands, state
      the asymmetry wherever a Test 1 number appears (the bias runs *against*
      FastFHIR, which is the safe direction, but it is still not parity).
      **Now quantified (IN-0, 2026-08-26):** on a 1 MB bundle the FastFHIR arm
      emits **232,534 wire bytes vs the JSON arm's 105,637** — 2.2x — for the
      same clinical content. Before the byte columns existed this gap was
      invisible. No size comparison is publishable until PA-1 closes.
- [ ] **PA-2. Tier S of `value[x]` is mocked in the HL7v2 arm.** Per **D2**,
      Tier S needs no upstream change: OBX-2/OBX-5 can carry the real scalar.
      [`bench/bench_test_1.hpp:2785-2810`](bench/bench_test_1.hpp:2785) writes a
      hardcoded `"1"` for every variant. Write the actual value, then Tier S is
      measured at four-arm parity. Do this early — it is the only part of
      `value[x]` not blocked on anything — but **report it as 4.8 % of the
      choice surface** (D2 census), never as "`value[x]` is covered".
- [ ] **PA-3. Tier B of `value[x]` is blanked in every arm.** `sanitize_choice`
      at [`bench/harness.hpp:538`](bench/harness.hpp:538) zeroes non-portable
      choices at hydration so no arm corrupts its stream. *Blocked on upstream
      CAPI-3.* Note the fix belongs at **hydration** (deep-copy the block from
      the source `Parser` into the destination builder), **not** in the
      assignment sink — handoff.md's sequencing said "assignment sink" and that
      is the wrong place; it would tie the fix to D3's outcome for no reason.
      **This is the high-value item, not PA-2:** it is 95.1 % of the choice
      surface on the shipped corpus (D2 census), including every `valueQuantity`
      and `valueCodeableConcept`.
- [ ] **PA-4. `FF_UNSET` is handled by a per-arm `default:`.** Decide what an
      unset gender/status means in each format and apply it identically.
- [x] **PA-5. Normalize Test 2.** ✅ **RESOLVED 2026-08-26 by D4** — the walk
      is retired; Test 2 is now random access and carries `ops` = field reads,
      with the cross-arm byte gate enforcing identical work (exit 2 on
      mismatch). The old node-count divergence (4,443 / 8,327 / 8,008 / 9,539
      for identical content) is history.
- [ ] **PA-6. HL7v2 arm does not report `obs_issued_present`,
      `obs_component_value_*`, or `obs_effective_datetime`.** This is the
      remaining exit-2 mismatch: a real coverage gap in the arm, not a harness
      fault. (`obs_effective_datetime` was previously masked — the JSON
      baseline itself counted 0; fixed 2026-08-26.)
- [ ] **PA-7. Test 3 penalises the FastFHIR arm.** ✎ **Scope shrank 2026-08-26:**
      the query no longer materializes whole observations (lens reads), so the
      `print_json` penalty now applies only to `Patient.birthDate` — a per-patient
      cost, not per-observation. `read_text_field()` goes through `print_json`
      for packed date/time because no zero-copy public reader exists. *Filed
      upstream as CAPI-4.* Until it lands, subtract or disclose the cost.
- [x] **PA-11. Test 2 does not measure what the charts imply, and the arms are
      not doing comparable work.** ✅ **RESOLVED 2026-08-26 by D4** — Test 2 is
      now random access (IN-B), which is the claim-appropriate measurement; the
      analysis below is retained as the record of why the walk was wrong
      evidence. Normalized per node on an 8 MB target bundle (former Test 2):

      | Arm | nodes | duration | ns/node | what it actually does |
      |---|---:|---:|---:|---|
      | json_fhir | 29,956 | 140 µs | **4.7** | parse to a contiguous tape, scan it — no objects, no per-element allocation |
      | fastfhir | 15,920 | 442 µs | **27.8** | mmap + reflective walk — **2,396 heap allocations** (`BENCH_ARRAYS=1`) |
      | google_fhir | 34,387 | 1,679 µs | **48.8** | real materialization into C++ message objects |
      | hl7v2 | 29,005 | 1,799 µs | **62.0** | segment scan into parsed structs |

      Three separate problems, all of which have to be stated wherever Test 2
      appears: (a) `entries()` returns an owning `vector<Node>` — one allocation
      per array node, the documented exception to the zero-allocation read path,
      and a full walk is nothing *but* array materialization; (b) the reflective
      key API charges a `strlen` per field, ~13% of the walk (upstream CAPI-7);
      (c) **simdjson is not materializing anything** — its tape is an index, so
      "materialize" means categorically different work in each arm. Test 2 is a
      *full-traversal throughput* measurement, and § Why FastFHIR? does not make
      a full-traversal claim. **Do not present it as evidence for or against
      zero-copy** — that is IN-B and IN-D.

      **Cause attributed 2026-08-26** by two controlled variants of the same walk
      through the same public API, at a 128 MB target:

      | walk | median | vs current |
      |---|---:|---:|
      | `entries()` — current | 5.32 ms | — |
      | `node[i]` index walk, no vector | 4.33 ms | **-18.5%** |
      | index walk + no `strlen` | 3.45 ms | **-35.1%** |
      | simdjson baseline | 1.78 ms | |

      So ~35% of the gap is the two API artifacts (`BENCH_INDEX_WALK=1`,
      `BENCH_NO_STRLEN=1` reproduce it) and a **~1.94x residual remains**. The
      residual is *not yet measured*: the working hypothesis is pointer-chasing
      versus a linear tape scan (FastFHIR reaches children by offset into the
      arena; simdjson scans contiguous memory with perfect prefetch) plus the
      indirect call through `ParserOps` on every lookup. A `sample` profile of
      the whole harness shows **no FastFHIR frames in the top-of-stack at all**,
      consistent with it winning the stages that dominate wall time but not an
      isolation of the walk. Settling the residual needs a walk-only binary — do
      that before quoting a cause.

      **Split measurement, 2026-08-26 (`BENCH_SPLIT=1`), target 2048 — this is
      the finding that reframes Test 2.** Separating "make the bytes addressable"
      from "walk every node":

      | | FastFHIR | simdjson |
      |---|---:|---:|
      | open / parse | **7.4 µs** | **27.4 ms** |
      | walk | 84.78 ms | 7.14 ms |
      | nodes | 3,095,160 | 5,852,922 |
      | ns per node | 27.39 | 1.22 |

      **FastFHIR makes a 141 MiB stream addressable 3,696x faster** — 7 µs
      against 27 ms — and that is the zero-copy claim, measured, holding exactly
      as advertised. It then walks 22.5x slower per node. Test 2 is ~100% walk
      for FastFHIR (the open is 0.009% of its time) and 79% parse for simdjson,
      so the stage as defined hands FastFHIR a 3,700x win and then buries it
      under the one operation it is worst at. Confirmed at target 4096: JSON
      parse 56.5 ms, FastFHIR open still ~7 µs.

      **Crossover: FastFHIR is ahead until you touch ~35% of the document**
      (1.09M of 3.10M nodes). No realistic query touches 35% of a bundle, which
      is precisely why § Why FastFHIR? claims random access and not traversal —
      and why IN-B/IN-D are the instruments that would show it.

      **The ratio is flat across a 300x range of bundle size** (2.1-3.0x from
      0.5 MiB to 140 MiB of JSON wire), so this is a constant factor, not an
      asymptotic divergence, and no crossover appears at scale. `entries()`
      returning a non-allocating view would retire the 18.5%; filed as CAPI-8.
- [ ] **PA-13. `--bundle-max-mb` does not name the bundle size — it is off by
      ~20-30x.** The accumulator counts `p.patient.memory.size()`, the **ingested
      source** arena ([`bench/main.cpp:477`](bench/main.cpp:477)) — a whole
      Synthea bundle, every resource type — while the arms serialize only the
      Patient + Observation subset. Measured 2026-08-26:

      | `--bundle-targets-mb` | patients | JSON wire | FastFHIR wire | peak RSS |
      |---:|---:|---:|---:|---:|
      | 256 | 65 | 8.2 MiB | 17.7 MiB | — |
      | 1024 | 272 | 33.4 MiB | 72.0 MiB | 4.2 GB |
      | 2048 | 437 | 71.7 MiB | 153.8 MiB | 4.4 GB |
      | 4096 | 929 | 140.0 MiB | **300.1 MiB** | 6.2 GB |

      So **"256 MB" was an 8 MiB bundle**, and every sweep range this repo has
      ever quoted meant something ~25x smaller than it said. A true 256 MiB
      *JSON* bundle needs `--bundle-targets-mb 7500` and ~11 GB RSS; a 256 MiB
      *FastFHIR* stream is reached at 4096. Fix the flag to mean produced wire
      bytes (calibrate bytes-per-patient once at startup, then accumulate against
      that) and until then **quote wire bytes, never the target**. The figures
      already use measured bytes on the x-axis for exactly this reason.
- [ ] **PA-12. `target_mb` does not control the workload below ~4 MB.** The
      fixture accumulates patients until the total exceeds the target, and one
      Synthea patient is ~3 MB ingested — so every target under ~4 MB yields a
      bundle of exactly one randomly chosen patient. In one run the 1 MB target
      produced 273 KB of FastFHIR wire and the 2 MB target produced 130 KB: the
      smaller request produced the larger bundle. Every duration-vs-size curve
      below 4 MB was plotting against noise, which is why the plots now use
      measured wire bytes as x. Fix the ladder (start at ~4 MB, or accumulate to
      a byte target rather than a patient count).
- [ ] **PA-9. Test 4 is not the same operation in the FastFHIR arm.**
      `FastFHIR::Memory` holds a `shared_ptr<FF_Memory_t>`, so
      `StreamType enriched_stream = payload` shares the arena and the enrich
      **mutates the source in place**, while the other three arms build a
      separate buffer. Found 2026-08-26 by IN-0: `source_bytes` was being read
      after the append, so FastFHIR's appended-bytes delta was always 0 and its
      Test-4 `bytes_in` disagreed with its own Test-1 `bytes_out`. The read
      order is fixed ([`bench/bench_test_4.hpp:105`](bench/bench_test_4.hpp:105));
      **the asymmetry is not.** In-place append is the feature (WF-4.1), so the
      fix is not to force a deep copy — it is to report the two shapes as
      different measurements, and to note that FastFHIR's timer also starts
      after its copy while the JSON arm's starts before its parse.
- [ ] **PA-10. Enrich cost is wildly uneven and now visible.** ✎ **Investigated
      2026-08-26 — the API cannot do the in-place append.** All three public
      paths for "append one element to an existing sealed array" fail:
      `MutableEntry[n]` past the end throws `out_of_range`; `insert_at_field`
      refuses an already-assigned slot; README Example 3's array append is
      unvalidated (works only on absent slots). Filed upstream as **CAPI-12**.
      Until it lands, the FF arm MUST re-serialize the bundle root — the delta
      is O(entry-array), and the claim waits on CAPI-12 + IN-D.
      - **Measured 2026-09-16** (full run 35126898251, median): FastFHIR appends
        18 KB at 1 MB and **2.34 MB at 256 MB**. The other arms add a constant
        1.6–2.7 KB.
        - Cause: `Bundle.entry` is an inline array of **84-byte**
          `FF_BUNDLE_ENTRY` blocks, and Test 1 preallocates it at the **front**
          (`[Bundle | entry[N] | resources…]`). The enrich therefore writes a new
          Bundle block and a new N+1 array: 412 entries × 84 B + Observation +
          Bundle = the 39.8 KB measured at 4 MB.
        - `fig4`'s "bytes added" is misleading in both directions. JSON and
          Google re-serialize the whole stream and HL7v2 copies it, while
          FastFHIR overwrites nothing.
      - **Direction (Ryan, 2026-09-16): tail-rewrite append**, filed upstream
        as **APPEND-1**.
        - Resources are written first. `Bundle.entry` is written last by
          `serialize_bundle_array(std::vector<BundleentryData>)`.
        - The enrich reads the light entry vector, rolls the write head back to
          the array, appends the resource there, rewrites the N+1 array after
          it, and reseals.
        - Growth = resource + 84 B, with nothing orphaned.
      - Benchmark follow-ups:
        - [x] **PA-10a. Build the Bundle last** (Test 1, FastFHIR arm). ✅ 2026-09-16
              - Workers record each child's `(offset, type)` into their own slot
                of the in-memory `bundle.entry`. One `append_obj(bundle)` runs
                after the join.
              - The old layout stays available as `BENCH_FF_BUNDLE=backfill`, and
                the run banner prints which layout ran.
              - Verified with `BENCH_VALIDATE` for both layouts: the stream
                validates, entry counts match, and the sealed size is identical.
                The root sits at 720,795 of 750,519 B (tail) vs 54 (backfill).
              - Timing is unchanged within noise (FF arm, 6 replicates × 3 runs):
                the fastest Test 1 run is 0.399 vs 0.398 ms at 16 MB and 1.335 vs
                1.370 ms at 64 MB. Test 2 and Test 4 are also unchanged.
              - The binary changes, so the A/B baseline still needs a re-run.
        - [x] **PA-10b. Report bytes written and bytes overwritten** (Test 4). ✅ 2026-09-16
              - New CSV columns `bytes_written` and `bytes_overwritten`, appended
                at the end (-1 = not applicable). They are filled per arm from a
                stored-stream update model:
                - JSON and Google FHIR: written = the whole enriched stream,
                  overwritten = the whole source.
                - HL7v2: written = the message, overwritten = 0.
                - FastFHIR: written = growth + 98, overwritten = **98**.
              - **FastFHIR's 98 B, found by measuring rather than assuming:**
                opening a Builder on a sealed stream already rewinds the write
                head onto the old checksum block (FastFHIR
                `src/FF_Builder.cpp`, "Re-open for append",
                `m_memory.reset(...)`). The Observation overwrites that 44 B
                block, and the reseal restamps the 54 B header.
              - `BENCH_VALIDATE` snapshots the source and asserts that every
                changed byte lies in those two regions. Verified at 1, 4 and
                16 MB, both layouts.
              - `fig4` is now two panels on a shared arm axis (bytes written /
                existing bytes overwritten, with each value's share of the
                stream). Zero is labelled "append-only", since a log axis cannot
                draw it.
              - Pre-PA-10b CSVs fall back to the old growth chart, with a
                caveat. `summary.md` gains a storage-cost table for Test 4.
              - Not done: the PG schema (`benchmark_results`) has no columns for
                these yet.
              - Also moved each arm's `timer.stop_ns()` ahead of its size reads
                (nothing between the last real operation and the stop).
        - [ ] **PA-10d. The HL7v2 enrich copies the whole stream inside its
              timer** (`enriched_stream = payload`). Its storage model is an
              append, so its duration includes a memcpy of the source (1.7 ms at
              256 MB) that an append-to-file would not pay. Either append to a
              buffer the arm owns, or report the copy separately.
        - [x] **PA-10c. Test 4 (FF arm) uses APPEND-1.** ✅ 2026-09-17
              - Built against FastFHIR's uncommitted APPEND-1
                (`FF_BundleAppendEntries`).
              - `bench_test_4.hpp` detects the API with
                `__has_include(<FF_BundleAppend.hpp>)` and otherwise falls back to
                the old re-serialize path. An A/B against a pre-APPEND-1 baseline
                therefore still builds, and measures exactly this change.
              - Measured locally (FF arm, 3 replicates × 3 runs, identical
                bundles):

                | Bundle | Stream growth (before → after) | Enrich time (before → after) |
                |---:|---|---|
                | 1 MB | 17,835 → **4,619 B** | 0.013 → 0.012 ms |
                | 16 MB | 136,107 → **4,619 B** | 0.087 → 0.050 ms |
                | 64 MB | 606,507 → **4,619 B** | 0.262 → 0.212 ms |
                | 256 MB | 2,132,115 → **4,619 B** | 0.898 → 0.670 ms |

              - Bytes written are about unchanged: the N × 84 B array is now
                rewritten in place instead of copied.
              - `bytes_overwritten` = header 54 + `Bundle.entry` slot 8 +
                `[rewrite_from, old end)`; a relocation overwrites 54 + 8 + 44.
              - New `rewrite_from` field in `EnrichMetricsSummary`.
              - `BENCH_VALIDATE` checks, for both layouts:
                - the changed bytes lie inside the modelled regions;
                - the enriched stream validates, with N+1 entries and an
                  Observation last;
                - tail layout takes the tail-rewrite path; backfill relocates.
              - `fig4` now has three panels: **stream growth** (linear axis),
                bytes written and bytes overwritten (log axes).
              - **CI needs APPEND-1 pushed upstream first**: the runner pins
                FastFHIR from GitHub.
              - Framing (Ryan, 2026-09-16): **growth per append is minimal, not
                zero.** Each append adds one 84 B entry plus the resource,
                unavoidably, because an entry is being added. That is far less
                than a format that must rewrite the whole stream.
              - Report it as "grows by the minimum", never as a defect.
              - What APPEND-1 removes is only today's **excess**: the orphaned
                N × 84 B copy of the previous array.
        - Both array-building paths are now advertised upstream as first-class
          (FastFHIR README Example 6a/6b, both executed by its README gate).
          This arm defaults to 6a (tail); `BENCH_FF_BUNDLE=backfill` runs 6b.
- [ ] **PA-8. Cross-arm validation must cover every arm.**
      [`bench/main.cpp:95`](bench/main.cpp:95) compares FastFHIR↔JSON and
      JSON↔HL7v2 only. Nothing has ever checked the Google arm, which is how a
      `birthdate` microsecond-epoch mismatch survived undetected (notes.md §5b).

---

## ▶ IN — instruments for the claim register

Designs lived in `handoff.md`, removed 2026-09-08; this is the tracker. Each instrument
is a separate narrow binary, **not** another stage on the 4×4 grid. The existing
four-arm × four-stage harness stays as a smoke and regression rig.

Ordered by what unblocks the most.

- [x] **IN-0. Emit size and provenance.** ✅ **DONE 2026-08-26.**
      - CSV is now `arm,test,duration_ns,bytes_in,bytes_out,target_mb,patients_in_bundle`.
        `MetricEvent` carries wire bytes per stage
        ([`bench/harness.hpp:51`](bench/harness.hpp:51)); all four arms populate
        them **after** `stop_ns()`, never inside the window. 0 means "not
        applicable to this stage" and a serialize stage reporting 0 warns.
      - `EnrichMetricsSummary`'s `source_bytes`/`enriched_bytes` — computed
        since the port and never read — now reach the results via a new
        `enrich_metric(arm, summary)` overload.
      - PG: `bytes_in`/`bytes_out` on `benchmark_results`; `benchmark_runs`
        gains `fastfhir_sha`, `fastfhir_dirty`, `production_profile`,
        `compilation_mode`, `corpus_sha256`, `benchmark_sha`, `seed`.
      - [`bench/provenance.hpp`](bench/provenance.hpp): collects every field
        handoff.md requires, prints a summary on **every** run, and
        `--results-dir DIR` writes `provenance.json` — refusing (exit 3) when
        the record is incomplete. Self-contained SHA-256 (no crypto dep) for the
        corpus digest, memoized on (count, bytes, newest mtime).
      - [`bench/provenance_test.cpp`](bench/provenance_test.cpp): 11 checks —
        FIPS 180-4 vectors plus every refusal path. `bazel test
        //bench:provenance_test`.
      - **Two gates bite in practice, both deliberately:** a non-`opt` build
        cannot produce an artifact (the Debug trap, enforced), and an ambiguous
        profile is rejected — this machine has three CMake caches carrying two
        different profile values, so `--profile` must pin it.
- [ ] **IN-F. Preservation matrix** (WF-2.2, 2.3, 2.4, 5.4). No new arms, no
      timing methodology to defend, validates four claims at once, and carries
      Tier X of **D2**. Round-trip each corpus document JSON → library → JSON,
      diff, classify every difference as preserved / reordered / coerced /
      **lost**. Include `MessageToJsonString` as a column — README § 5.4 names
      it. *Do this before any new timing work.*
- [ ] **IN-A. Receiver-side throughput + orjson arm** (WF-1.3). Retires the
      uncited citation (upstream I3.6). Two rows per arm, not one: **time to
      addressable** and **time to all fields materialized** — `orjson.loads()`
      is eager and mmap+header-validate is lazy, and collapsing that asymmetry
      into a single ratio flatters FastFHIR. Warm the interpreter; keep process
      start out of the window and say so.
- [ ] **IN-D. Bytes touched** (WF-4.1). `mincore` resident pages + `getrusage`
      minor faults for one `Patient.id` read from a large bundle; pages dirtied
      by an enrich. A flat line against a linear one is a stronger artifact than
      any nanosecond ratio, and much harder to argue with. **Strong candidate to
      cite upstream in place of a throughput number.**
- [x] **IN-B. Random-access curve** (WF-1.1) — **PROBE LANDED 2026-08-26**,
      `BENCH_RANDOM_ACCESS=N`. Still needs wiring into the CSV/figures to be an
      instrument, but the measurement exists and it is the most important number
      this repo has produced.

      **Method.** Pick N random `Bundle.entry` indices, navigate to each **from
      the root**, read the resource's `id`. Every lookup pays its own path cost.
      The JSON arm gets the fairest implementation available (array lookup
      hoisted out of the loop); `at(i)` remains O(i) because a simdjson DOM has
      no O(1) index — which is precisely the "O(N) linear scanning" WF-1.1 claims
      to bypass. **Both arms return an identical accumulator at every size**, so
      they demonstrably read the same fields.

      | bundle entries | FastFHIR ns/read | simdjson ns/read | ratio |
      |---:|---:|---:|---:|
      | 5,844 | 92.6 | 5,913 | 64x |
      | 24,661 | 342.0 | 38,225 | 112x |
      | 105,202 | 929.2 | 366,602 | 395x |
      | 226,925 | 791.5 | 2,030,842 | **2,566x** |

      **simdjson grows linearly with entry count; FastFHIR does not.** That is
      WF-1.1, measured, and it is the mirror image of Test 2: read in layout
      order and the tape wins 22x, read out of order and FastFHIR wins 2,566x.
      FastFHIR is not perfectly flat (92.6 -> 791.5 ns as entries grow 39x)
      because random access into a larger arena costs more cache and TLB misses
      — honest, and worth saying rather than claiming a flat line.

      **The counterpoint that must be reported with it:** a simdjson DOM is not
      built for indexed access, and a real consumer wanting many random lookups
      would build an index once and amortize. Estimated crossover — *not yet
      measured, do not quote* — JSON pays ~34 ms (parse + index build) then
      ~100 ns/lookup; FastFHIR pays 7.4 us then ~782 ns/lookup; they meet near
      **~50,000 lookups** into a 141 MiB bundle. Measure it before publishing;
      an unmeasured crossover is exactly the kind of number this repo exists to
      stop.

- [x] **IN-B2. Random access is a real stage — and it is Test 2 (D4)** —
      ✅ **2026-08-26.**
      [`bench/bench_test_2.hpp`](bench/bench_test_2.hpp), macro-guarded per arm
      with the mandatory inline namespace (D1), emitting to the CSV and PG like
      any other stage. `MetricEvent` gained `ops` (units of work), so the stage
      reports **ns/read** — the normalization PA-5 asked for and never had
      anywhere to live. `fig3_random_access` and a notebook section ship with
      it. **The former Test 2 (materialize) was retired the same day — D4.**

      | bundle | FastFHIR | simdjson | protobuf | HL7v2 | FF vs simdjson |
      |---|---:|---:|---:|---:|---:|
      | ~0.5 MiB | 49.5 ns | 877 ns | 1,271 ns | 126,300 ns | **18x** |
      | ~2 MiB | 62.1 ns | 6,125 ns | 7,360 ns | 450,647 ns | **99x** |
      | ~10 MiB | 227.5 ns | 43,387 ns | 50,178 ns | 1,914,388 ns | **191x** |
      | ~36 MiB | 344.9 ns | 295,397 ns | 322,183 ns | 10,519,591 ns | **856x** |

      **The cross-arm byte gate is the reason to trust this**, and it earned its
      keep immediately — it caught three real bugs before any number was
      reported: the HL7v2 probe splitting on `\n` when v2 terminates segments
      with `\r`; a wrong PID field index (`parse_segment_line` keeps the segment
      name in `Segment::name`, so PID-3 is `fields[2]`); and **the probe running
      after Test 4**, whose in-place enrich (PA-9) had the FastFHIR arm reading
      1,474 entries against the others' 1,473. It now runs before Test 4 in
      every arm.

      **Still open:** HL7v2 addresses *messages* (5) where the others address
      *resources* (1,473) — a v2 batch has no resource-level index. Its number
      is a different operation and is captioned as such rather than dropped. The
      indexed-JSON counterpoint is still unmeasured; the ~50k-read crossover
      estimate must not be quoted until it is.

- [ ] **IN-B3. Measure the indexed-JSON counterpoint.** Build the index once,
      amortize it over N reads, and find the real crossover. Until this exists,
      Test 2 shows a scan-based consumer, not the best a JSON consumer can do.

- [ ] **IN-C. Allocation gate** (WF-1.2, WF-3.4). Count, do not time. Assert
      exactly 0 for navigation and reflection, exactly 1 per `entries()` call.
      A `bazel test` target that fails the build when violated, not a metric row.
- [ ] **IN-E. Size table** (WF-1.4). **Partial: 2026-08-26** — the harness now
      emits `test_1_compact` (FastFHIR compact archive size, gated on
      losslessness; the gate itself is the "quote no compact number until the
      compact export re-parses identically" rule from handoff.md). Still to
      do: gzip(JSON), gzip(protobuf), and the sparse vs dense corpus split —
      the −66 % claim is scoped to sparse resources and must never be blended
      into one headline. See also upstream I3.7:
      the published −66 % may itself be a lossy measurement, so **do not
      reproduce that number here as a baseline to compare against**.
- [x] **IN-G. Resilience & integrity suite** (WF-3.1, 3.2, 3.3, and WF-4.2's
      correctness half) — ✅ **SHIPPED 2026-08-26** as
      [`bench/resilience_test.cpp`](bench/resilience_test.cpp), all four tests
      (truncation, bit-flip, type confusion, concurrent build). Run from the
      repo root: `./bazel-bin/bench/resilience_test` (bazel run changes cwd and
      loses the corpus). Verified on a real Synthea patient: truncation
      rejected at every structural cut; VALIDATION flips fail
      `validate_FFHR_stream`; RECOVERY_TAG flips refuse typed reads; payload
      flips leave structure intact and are caught by the real SHA-256 footer
      (the ingest's null hasher is re-sealed with `bench::provenance::sha256`);
      concurrent `append_obj` (8×25) validates with all blocks reachable.
      Qualifiers preserved: malformed-not-hostile (G1), integrity-not-
      authenticity. Test 4's "blocked on IN-H" note was stale — the suite
      exercises the library's `claim_space` directly, not the arm's parallel
      path. Spec: [`TODO.md`](TODO.md) (marked shipped).
      **Test 5 (recovery) restructured 2026-09-02** — see IN-G2. The curve
      is produced by the macro-parity driver `//bench:bench_test_5` (content-
      verified anchored subset; the embedded hand-rolled probe that used to
      live in this binary was removed — it clobbered `recovery_curve.csv` on
      every run). Upstream findings from the probe era: **CAPI-13** (Parser
      ctor SEGV — fixed by `FF_HEADER::validate_full` + the `Recovery` API;
      the manual header pre-validation workaround is retired) and **CAPI-14**
      (POCO `string_view` dangles). Harness gained `--dump-artifacts` (one
      bundle's Test-1 wire payload per arm).
- [ ] **IN-G2. Recovery-test methodology — content-verified metric SHIPPED 2026-09-02;**
      remaining items are format findings, not methodology. Done: the curve is now
      produced by the macro-parity driver `//bench:bench_test_5` +
      `scripts/recovery_sweep.py` — corruption and recovery remain INDEPENDENT
      processes, and `--check` is a THIRD process that (a) re-derives each
      recoverer's digest from its own units and (b) verifies recovered ⊆ baseline
      on the anchored (parent, offset, tag) triple, so a unit counts only when its
      two halves corroborate the clean structure (fixes flaws C/F: content-verified,
      and misattachment/invented references surface as spurious). The CSV carries
      `positions_total` per format (flaw B's density denominator) and fig8 now
      plots damage DENSITY (bits / structural positions) so one format does not get
      a free pass from a smaller syntactic surface. Clean-stream k=0 scores 100.0
      for every format (the clean-baseline control). Syntax-only corruption
      (2026-09-02 round 2, matching FastFHIR tests/cpp/test_recovery.cpp): FFHR
      flips the live-edge census witnesses (header + child headers + pointer
      slots via Recovery::reachable_blocks); JSON blasts EVERY unescaped
      brace/bracket/quote/colon/comma (`\X` exempts the pair) in or out of
      strings -- an unescaped brace inside a value looks like syntax to a blind
      scanner, so it gets hit like a structural one; HL7v2 flips CR + type
      names + the full `| ^ & ~ \\` delimiter set; protobuf flips TLV
      headers. v2 recovery resyncs on segment-header patterns (known 3-char
      type + `|`); unit tags carry the full 3-char name. Medians (20 trials):
      FastFHIR 98.2% at 512 bits (0.14% density), JSON 72.0, HL7v2 96.6,
      protobuf TLV 7.7 at 6.9% density -- CONTENT-VERIFIED per unit (each unit
      carries an FNV-1a hash of its own data bytes; correct = identity AND
      data match, wrong = present but data changed, pct = correct/baseline).
      REMAINING, per handoff.md § "What remains": v2's pct floor is unit
      granularity (512 flips can damage at most 512 of 14,701 whole-segment
      units; interior damage shows in the wrong column, not the pct) -- the
      per-value axis (corruption_probe extract/verify, v2 ~15% at k=16) is the
      finer complement. Flaw D (delimiters in the v2 structural set) and flaw B
      (density axis) are DONE. The 2026-08-26 curve remains do-not-cite.
      Context — the recovery semantic that can legitimately differentiate FFHR:
      FastFHIR's arena encodes every parent→child edge TWICE, and the two
      halves corroborate each other:

      ```
      Node A (Observation)                      Node B (valueQuantity block)
        ┌──────────────────────────┐              ┌─────────────────────────┐
        │  field slot for "value": │  offset ──▶  │ VALIDATION == own offset│
        │  { expected RECOVERY_TAG,│              │ RECOVERY_TAG (Quantity) │
        │    stored offset → B }   │              │ payload bytes           │
        └──────────────────────────┘              └─────────────────────────┘
      ```

      **Failure mode 1 — A's pointer is corrupt (the offset value in A's
      slot is garbage).** A loses its result; B is ORPHANED (reachable by
      nobody, but still physically present and self-consistent). Recovery:
      sweep for orphan blocks (VALIDATION == position + known tag) → B is
      found → **B's RECOVERY_TAG matches A's slot's expected tag** (the
      corrupted offset sits in the "value" slot of an Observation, whose
      declared child type is Quantity — and B IS a Quantity). The two halves
      corroborate: the relationship is reconstructible, and the edge can be
      counted as recovered.

      **Failure mode 2 — B's header is corrupt (B's VALIDATION or
      RECOVERY_TAG damaged).** A's pointer is intact — it still names B's
      location, and A's slot still declares B's expected type. Recovery:
      the pointer identifies where B must be; whether the content survives
      depends on how much of B's header is damaged, but the EDGE's existence
      is provable from A's side alone.

      **The principle: two sets of correct information for each one set of
      mistakes.** Every pointer edge is encoded independently in the parent's
      slot (offset + expected type) and the child's header (VALIDATION +
      actual type). Corruption of either side leaves the other as evidence;
      cross-validating both detects the edge from either half and can repair
      (or at least count) it from the intact half.

      **What this changes about the metric:** recovery should count
      RESTORED EDGES — parent→child pairs whose two halves corroborate —
      not surviving blocks. That is the content-verified, relationship-level
      semantic the comparison needs, and it is FFHR-specific: JSON values are
      inline in the object (no separate addressable block, no orphan to
      sweep, no slot-type to match); protobuf fields are inline in the
      message; HL7v2 has no structure of this kind. Only the arena's
      offset-addressed, self-describing blocks have the redundancy, so only
      FFHR can demonstrate cross-validated edge recovery. Build a
      `Recovery::recover_edges()` (or extend the library `Recovery`) that:
      (1) walks intact parents, (2) for each pointer slot with a corrupt
      target, sweeps for an orphan whose tag matches the slot's expected
      type, (3) counts the edge recovered when a unique corroborating match
      exists, (4) reports edges recovered / total edges — then re-run the
      comparison with this as the FFHR curve against content-verified curves
      for the other formats.
- [ ] **IN-H. Thread-scaling curve** (WF-4.2). *Blocked:* the parallel path in
      [`bench/arm_fastfhir.cpp:124-172`](bench/arm_fastfhir.cpp:124) is
      commented out, so this repo currently exercises only the serial path and
      proves nothing either way. Re-enable before designing the instrument.
- [ ] **IN-R. `claims.json` + the release artifact.** Per handoff.md. Plus the
      missing enforcement: a checker that extracts the § Why FastFHIR? bullets
      from `../FastFHIR/README.md` and fails if any lacks a `claims.json` entry.
      Without it the rule is an honour system — and the orjson citation is what
      an honour system produces.

---

## ▶ REC — competitor recovery parity (test 5)

**Verified state, 2026-09-08 — read this before proposing work here.** An
earlier draft of this block asserted that "the other three arms have no repair
step at all". That is **false**, and the code disproves it
(`bench/bench_test_5.hpp`):

| arm | `recover_stream()` does | line |
|---|---|---|
| fastfhir | `FF_Recovery::recover()` then `apply()` — diagnoses **and rewrites bytes**, the only arm that repairs rather than resynchronises | 1456 |
| json | whole-document parse; on failure **resync on `"resource"` markers**, brace-match each extent, keep the ones that parse | 1479 |
| google_fhir | **TLV record resync**: scan for a `'P'`/`'O'` type byte with a plausible length prefix, emit, and rescan forward on an implausible one | 1524 |
| hl7v2 | `return scan_v2_canonical(wire);` — **the same scan as the baseline. This is the only arm with no repair.** | 1572 |

Two consequences that change what is worth building:

1. **The gap is hl7v2, not "the competitors".** json and google_fhir already
   have record-level resynchronisation. What none of the three do is repair
   *bytes*; they resynchronise and discard. FastFHIR repairs. That is the real
   axis the curve measures, and it is a fair one — but it must be stated in
   those terms, not as "recovery vs none".
2. **json's measured resilience already includes a repair pass.** Its 0.8
   resources lost per flip is 0.8 *because* the `"resource"` resync bounds
   damage to the entry it lands in. Without that pass one flip would cost the
   document tail. Do not re-derive json's number as though it were raw parser
   behaviour.

⚠ The comment above `scan_v2_canonical` at line 1572 claims a destroyed `\r` is
recoverable because "the merged line still contains the next segment's header".
**It is not**: `hl7_split(text,'\r')` splits only on `\r`, so a flipped
terminator leaves two segments on one line, `f[0]` names the first, and the
second segment's header is buried mid-line and never read. Fix the comment when
REC-2 lands.

**What this block is for:** close the hl7v2 gap, add the byte-level repair
none of the competitors have, then re-measure. The claim worth publishing is not
"FastFHIR survives corruption and the others do not" — it is **how much
FastFHIR's on-wire redundancy buys over the best a format without redundancy can
do**, which is only meaningful once the opponent is real.

### Grounding — what production actually does (researched 2026-09-08)

| System | Strategy | Source |
|---|---|---|
| HAPI (reference Java v2 parser) | `LenientErrorHandler` is the **default**: log and continue, never abort. Unexpected segments become generic elements rather than failures (`ParserConfiguration.setUnexpectedSegmentBehaviour()`). | [ParserConfiguration](https://hapifhir.github.io/hapi-hl7v2/base/apidocs/ca/uhn/hl7v2/parser/ParserConfiguration.html) |
| Mirth Connect | Channel **preprocessor** runs before the parser: split on `\r`, test each line against `/^[a-zA-Z0-9]{3}\|/`; a match starts a new segment, otherwise append to the previous one. This is **resynchronisation on the 3-char segment header**. | [Mirth line-break repair](https://nelsonwells.com/posts/2011/07/fixing-line-breaks-in-hl7-messages-in-mirth-connect/) |
| `jsonrepair` (canonical JSON repairer) | Applicable rules: missing closing brackets, truncated documents (close all open structures), missing commas between elements, missing quotes, missing escapes. | [josdejong/jsonrepair](https://github.com/josdejong/jsonrepair) |

Two honesty notes that must survive into the write-up:

1. **Production engines mostly reject, they do not repair.** Malformed messages
   go to an error queue; repair is bolted on by the integrator (Mirth
   preprocessor scripts, HAPI lenient config), not shipped as a repair engine.
   Building this makes the competitor arms **stronger than production default**,
   which is the correct direction — it forecloses "you strawmanned the
   alternatives".
2. **Our damage is substitution, not omission.** A bit flip turns `}` into some
   other byte; it does not delete it. The `jsonrepair` rules still apply because
   the *symptoms* coincide (unbalanced depth, juxtaposed values, unterminated
   string), but do not claim we are running jsonrepair's algorithm — we are
   applying its heuristics to a different fault model.

### The frame-shift correction (Ryan, 2026-09-08) — read before REC-1

HL7v2 field identity is **ordinal**: OBX-5 *is* the fifth thing between pipes.
A flipped `|` does not merge two fields and stop; `hl7_split` returns one fewer
field and **every subsequent index in that segment shifts down**. An earlier
note in this repo said the damage was local to the merged pair; that was wrong.

The current scanner already exhibits this, and its outcome mix is the evidence:
`scan_v2_canonical` (`bench/bench_test_5.hpp:973`) splits then indexes `f[4]`,
`f[5]`, `f[6]` positionally, so a shift makes `sub_id` garbage,
`resource_key("Observation", garbage)` match nothing in the baseline, and the
row score **spurious** while the real observation scores **missing**. hl7v2 is
the only arm with a material spurious count (9.7% of non-correct outcomes vs
json's 0.0%) and that is the frame-shift signature.

The shift is bounded by the segment terminator — `hl7_split(text,'\r')` runs
first and real v2 restarts field numbering per segment — **unless the `\r`
itself is flipped**, which the damage model does corrupt, merging two segments
and losing both.

### Measured baseline these tasks must beat

Blast radius at k=1, against an identical 34,839-leaf baseline in all four arms
(23.7 leaves per resource):

| arm | leaves lost per flip | = resources |
|---|---|---|
| fastfhir | 0.0 | 0.00 |
| hl7v2 | 3.0 | 0.13 |
| json | 19.0 | 0.80 |
| google_fhir | 19.0 | 0.80 |

**One flip costs JSON 0.8 of a resource, not the document tail — neither format
cascades.** The gap is *damage granularity*: a JSON Bundle entry frames a whole
resource in one brace-delimited object, so structural damage inside it costs the
whole resource; v2 spreads that resource across many CR-framed segments, so
damage costs one segment. Do not describe this as a cascade or a truncation —
an earlier revision did and the blast-radius numbers refute it.

Density-matched medians (the raw-k table overstates v2's lead, because k=2048 is
0.36% density for v2 and 0.59% for json):

| density | fastfhir | hl7v2 | json | google_fhir |
|---|---|---|---|---|
| 0.10% | 99.6 | 80.2 | 70.4 | 79.2 |
| 0.20% | 98.8 | 72.3 | 56.4 | 63.5 |
| 0.30% | 97.8 | 68.0 | 46.1 | 50.7 |

---

- [x] **REC-1. hl7v2 segment arity check — the frame-shift detector.**
      ✅ **DONE 2026-09-08** (`bench/bench_test_5.hpp`, `hl7_expected_arity()`
      + the guard at the top of `scan_v2_canonical`'s segment loop).
      - Arities measured on the clean wire, not assumed: **MSH 12, PID 14,
        OBX 7, ZFX 3** across 14,773 segments, one fixed arity per type. A
        segment whose `hl7_split(seg,'|')` count differs is dropped rather than
        read at shifted indices.
      - Catches shifts in both directions: a flipped `|` removes a boundary,
        and `~` (0x7E) / `\` (0x5C) are each one bit from `|` (0x7C) so they can
        create one. A flipped `\r` merges two segments into an over-long line,
        also caught.
      - Placed in the shared scanner, not `recover_stream()`: refusing to
        misread is reader robustness, not repair. Clean-wire baseline verified
        unchanged (units=34839).
      - ⚠ **The prediction in this block was WRONG and the measurement says so.**
        It was going to "gut" `wrong` (9,895) and `spurious` (79,858). Measured
        over the same 80 replicates (k=64/256/1024/2048 × 20 seeds):

        | | wrong | spurious | median pct |
        |---|---|---|---|
        | before | 8,056 | 62,438 | unchanged |
        | after | 7,834 | 62,354 | unchanged |
        | delta | **−2.8%** | **−0.1%** | 0 |

      - **Why it barely fires, measured:** only **12.0%** of this arm's
        structural positions are v2 delimiters (`| ^ & ~ \`). **70.7% are JSON
        punctuation inside ZFX payloads**, 12.9% are segment names, 4.3% are
        terminators. A flip in a ZFX payload breaks that payload's JSON but
        leaves the segment at 3 fields, so arity cannot see it. Frame shift is
        real and now handled; it is simply not where this corpus takes damage.
      - The task was still worth doing — it is correct, it is cheap, and it
        removes a silent-corruption path — but it does not move the curve, and
        the write-up must not imply it did.

- [x] **REC-1b. ZFX `observation.id` scope loss — same class as the OBX-4 bug.**
      ✅ **DONE 2026-09-08** (`bench/bench_test_5.hpp`, ZFX branch of
      `scan_v2_canonical`).
      - `observation.id` opens a scope every following `observation.*` ZFX
        attaches to. Two paths left `obs_key` holding the PREVIOUS
        observation's key when that id was damaged: `continue` on a parse throw,
        and falling through with a non-string payload. Both silently
        misattributed every later field — values did not go missing, they MOVED,
        the exact failure the OBX-4 keying change was made to eliminate.
      - Fix: when an `observation.id` cannot be read, **clear `obs_key`**. The
        existing `if (key.empty()) continue;` then drops the orphaned fields, so
        a damaged id costs its own observation instead of corrupting the one
        before it.
      - Clean-wire baseline byte-identical (units=34839, digest cbb2756…).
      - Measured over the same 80 replicates as REC-1:

        | stage | wrong | spurious |
        |---|---|---|
        | before REC-1 | 8,056 | 62,438 |
        | after REC-1 | 7,834 | 62,354 |
        | after REC-1b | **7,200** | **58,490** |
        | REC-1b delta | **−8.1%** | **−6.2%** |

      - Bigger than REC-1, and on the right surface — but still not a
        curve-mover: median `pct` is unchanged at every k. Combined, REC-1 and
        REC-1b remove ~10.6% of `wrong` and ~6.3% of `spurious`. Report it that
        way; neither task changes the headline.

- [x] **REC-1c. Missed scope boundary from a damaged ZFX field NAME.**
      ✅ **DONE 2026-09-08.** The largest of the three by a wide margin.
      - REC-1b covers a damaged id *payload*. A damaged field NAME
        (`observation.id` -> `observatioX.id`) falls through the trailing
        `else { continue; }`, so the scope never opens and the next
        observation's fields attach to the previous one.
      - Signal verified before implementing: on the clean wire the encoder emits
        each `sub` at most once per scope — **1,468 scopes, 0 repeated
        (scope, sub) pairs** — so a repeat is unambiguous. Checked on the RAW
        `sub`, before the `.details` / `[*]` / `[x]` trimming, which is the form
        that measurement used.
      - Also clears the observation scope at each `MSH`: a new message must not
        inherit the previous message's scope.
      - Clean-wire baseline byte-identical (units=34839, digest cbb2756…).

        | stage | wrong | spurious |
        |---|---|---|
        | before REC-1 | 8,056 | 62,438 |
        | after REC-1 | 7,834 | 62,354 |
        | after REC-1b | 7,200 | 58,490 |
        | **after REC-1c** | **645** | **18,756** |
        | REC-1c delta | **−91.0%** | **−67.9%** |
        | cumulative | **−92.0%** | **−70.0%** |

      - Median `pct` rose as well (k=1024: 73.20 → 73.40; k=2048: 65.40 →
        65.65), and `correct` with it — which dropping fields alone cannot do.
      - **Why, verified in the scorer** (`bench_test_5.cpp:287`):
        `Triple::operator<` and `==` order on (parent, offset, tag) and
        **deliberately exclude content**. A misattributed field therefore has
        the SAME identity as the real leaf, so in the merge join it can be
        paired with the baseline unit first — scoring `wrong` — and evict the
        real value out to `spurious`. Misattribution was not only inventing
        data, it was displacing correct data. Removing it recovers both.
      - This closes the misattribution class for hl7v2. The remaining 18,756
        spurious is elsewhere, most likely the 70.7% ZFX-JSON payload surface,
        which is REC-3's territory.

- [x] **REC-0. Split the per-arm codecs out of `bench_test_5.hpp`.**
      ✅ **DONE 2026-09-08.** The harness had grown ~1,400 lines of
      format-specific decoding spread across three separate `#if/#elif` macro
      chains, so each arm's reader, damage model and recovery sat in three
      places and none of them next to each other.
      - `bench_test_5.hpp` **1,700 → 376 lines**: it now owns only test 5 —
        `UnitRef`/`StreamFingerprint`, the canonical leaf and census machinery,
        `flip_positions`, and the arm dispatch.
      - Four codec headers, each with its arm's reader, `structural_positions`
        and `recover_stream` together and labelled in that order:

        | header | lines |
        |---|---|
        | `bench/arm_google_fhir_codec.hpp` | 588 |
        | `bench/arm_hl7v2_codec.hpp` | 483 |
        | `bench/arm_fastfhir_codec.hpp` | 259 |
        | `bench/arm_json_codec.hpp` | 201 |

      - Included from inside `namespace bench::test_5 { inline namespace
        BENCH_ARM_NS {` under the arm guard. Not self-contained by design —
        each says so in its header comment and names what it borrows from the
        harness. `flip_positions` needed a forward declaration in the harness
        because the codecs' `corrupt_stream()` calls it before its definition.
      - **Behaviour-neutral, verified rather than assumed:** all four clean-wire
        digests byte-identical (`hl7v2 cbb2756…`, `json 3dfb8b1f…`,
        `google_fhir f917ea61…`, `fastfhir 72286 63f…`, units 34839 each), the
        80-replicate damaged-wire census unchanged (wrong=645, spurious=18756,
        identical medians), and the four-arm harness still at element parity.
      - `bench/BUILD.bazel` lists all four in `bench_core_common` hdrs.

- [x] **REC-2. hl7v2 segment resync on the 3-char header.**
      ✅ **DONE 2026-09-08.** `hl7_resync_segments()` in
      `bench/arm_hl7v2_codec.hpp`, a pre-pass over the segment list. Both rules
      are gated on the REC-1 arity, so neither can fire on a healthy segment;
      the gate is sound because the encoder escapes `|` to `\F\`, so every
      unescaped `|` on the wire is a real separator and the field count is
      trustworthy even when the surrounding bytes are not.
      - **R2a name repair** (7.6% of flips): a flipped 3-char name makes the
        segment unrecognised and it is dropped whole, but the SHAPE still names
        it — the four arities are distinct (12/14/7/3). ZFX additionally
        requires field 1 to be a FHIR path, since arity 3 is the common case.
      - **R2b terminator re-split** (2.5% of flips): a flipped `\r` merges two
        segments and REC-1 then drops BOTH. The second segment's name is still
        in the bytes, sitting at the tail of a field, so the line is cut back
        apart there — and the byte that WAS the `\r` is consumed, restoring the
        original field exactly. **This is the rule Mirth does not have**: its
        preprocessor only ever JOINS a spuriously broken line, because its fault
        model is an inserted line break, not a lost one.
      - Clean-wire baseline byte-identical (units=34839, digest cbb2756…).

      **Each rule verified in isolation with a targeted single flip**, scored on
      leaf counts because `pct` at one decimal cannot resolve one segment:

      | injected fault | correct, OFF | correct, ON |
      |---|---|---|
      | one `\r` flipped (two ZFX merged) | 34,819 | **34,839** |
      | one ZFX name flipped to `ZFY` | 34,820 | **34,839** |

      Both recover the baseline **exactly** — 20 and 19 leaves respectively, a
      measure of how much one ZFX segment carries.

      Aggregate over the same 80 replicates, on top of REC-3:

      | k | REC-3 only | + REC-2 | delta |
      |---|---|---|---|
      | 64 | 99.40 | **99.60** | +0.20 |
      | 256 | 94.65 | **95.00** | +0.35 |
      | 1024 | 78.40 | **79.85** | +1.45 |
      | 2048 | 70.00 | **72.35** | +2.35 |

      `wrong` and `spurious` are flat (74 and ~620 at k=2048, unchanged), so the
      gain is recovery rather than invention. The delta grows with k because
      name and terminator flips accumulate; at 10.1% of the damage surface it
      was never going to be large, which is what the surface breakdown predicted.

- [x] **REC-3. Byte-level structural repair — the one thing no competitor did.**
      ✅ **DONE 2026-09-08.** `bench/json_syntax_repair.hpp`, shared by the json
      and hl7v2 codecs because both need it: 82.4% of the flips the v2 model
      applies land on JSON punctuation inside ZFX payloads.
      - Rule set is `jsonrepair`'s, restricted to the rules that bear on a
        SUBSTITUTION fault model: close unclosed structures, then bounded
        single-character substitution, then insertion as a fallback. Its
        LLM/JS-paste rules (Python constants, MongoDB types, comments, JSONP)
        are deliberately not implemented.
      - Search is bounded by the parser's own reported error byte (64 back, 4
        forward), not a full-fragment sweep. A candidate is accepted only when
        it PARSES — never on looks.
      - **Structure only, never content.** Nothing invents a key, value or
        digit. json's `spurious` stayed at **0** across every k, which is the
        evidence that it is not manufacturing units.
      - Enabled from `recover_stream()` only. The baselines are byte-identical
        (`hl7v2 cbb2756…`, `json 3dfb8b1f…`), so ON and OFF finally measure
        different things for these arms — acceptance criterion 4.

      **hl7v2**, median pct, same 80 replicates:

      | k | before REC-3 | after | OFF |
      |---|---|---|---|
      | 64 | 96.60 | **99.40** | 96.60 |
      | 256 | 87.60 | **94.65** | 87.60 |
      | 1024 | 73.40 | **78.40** | 73.40 |
      | 2048 | 65.65 | **70.00** | 65.65 |

      **json**, median pct, against the pre-REC full run:

      | k | before (full run) | after | OFF |
      |---|---|---|---|
      | 64 | 91.3 | **93.80** | 0.00 |
      | 256 | 75.2 | **80.10** | 0.00 |
      | 1024 | 46.4 | **54.85** | 0.00 |
      | 2048 | 27.4 | **37.75** | 0.00 |

      - ⚠ **json's recovery-OFF score is 0.00 at every k.** `calc_stream_hash`
        parses the whole Bundle, so a single flip anywhere destroys the entire
        document's readability. Everything json scores comes from the resync +
        repair pass. State it that way: JSON's raw resilience to a structural
        flip is nil, and its curve is a measure of its RECOVERY, exactly as
        FastFHIR's is.
      - hl7v2's `wrong` and `spurious` rose in absolute terms (medians at
        k=2048: 19→74 and 522→614) while `correct` rose far more. That is the
        honest trade — repair that recovers more data also occasionally
        reparents some — and the four-outcome census is what makes it visible
        rather than hidden inside a single percentage.
      - **Whole-document repair was tried and removed.** It is redundant with
        the per-resource path (the damaged entry is repaired there either way)
        and cost 4.6s per replicate re-parsing a 1.6 MB Bundle per candidate.
        Removing it left every median identical (93.80 / 80.10 / 54.85 / 37.75)
        at 284s instead of 368s. Verified, not assumed.

- [x] **REC-4. json record resync on anchors.** ✅ **ALREADY BUILT** —
      `bench/bench_test_5.hpp:1479` resyncs on `"resource"` markers with
      brace-matched extents. Proposed as new work on 2026-09-08 before the code
      was read; it was already there. Left here so it is not proposed a third
      time. Possible refinement, not required: anchor on `"fullUrl"` as well, so
      an entry whose `"resource"` key is itself damaged is still findable.

- [x] **REC-5. google_fhir TLV resync.** ✅ **ALREADY BUILT** —
      `bench/bench_test_5.hpp:1524` scans for a `'P'`/`'O'` type byte with a
      plausible length prefix and rescans forward when the length is
      implausible. Same note as REC-4: proposed before the code was read.

- [x] **REC-6. NDJSON control arm.** ✅ **DONE 2026-09-08.**
      `bench/arm_ndjson_codec.hpp` + `bench/arm_ndjson.cpp`. A CONTROL, not a
      competitor, and test-5 only — it answers a framing question about
      corruption, not a performance one, so it is not on the 4x4 timing grid.
      - Artifact generated **from** `json.bin` in the run script's artifact
        stage. Both fingerprint to the **identical digest `3dfb8b1f…`, 34,839
        units**, which is the proof that framing is the only variable.
      - Shares the leaf walk (`bench/json_leaf_walk.hpp`, extracted for this) so
        the two readers cannot drift into looking like a resilience difference.
      - Same damage rule, **including the `\n` record separators** — v2's `\r`
        is a corruption target, so NDJSON's terminator must be one too, or the
        control would be handed damage-free framing the others do not get.

      | k | json ON | ndjson ON | json OFF | ndjson OFF |
      |---|---|---|---|---|
      | 1 | 99.9 | 100.0 | **0.0** | **99.9** |
      | 256 | 80.1 | 91.1 | 0.0 | 75.0 |
      | 2048 | 37.8 | **55.2** | 0.0 | **27.0** |

      - **The granularity hypothesis holds.** Reframing alone is worth +17.4
        points at k=2048 with recovery, and without recovery it is the entire
        difference between 0.0% and 99.9% at k=1. NDJSON with no repair at all
        beats JSON with full repair up to k=128.
      - **Consequence for the write-up: the supportable claim is "single-document
        nesting is fragile", NOT "JSON is fragile".** Anything published that
        says the latter is now contradicted by this repo's own control.

- [x] **REC-7. Recovery COST.** ✅ **DONE 2026-09-08.** `--recover` and `--hash`
      report `recover_ns` / `read_ns`, timed around the READ only (file I/O and
      fingerprint serialisation outside the window); the sweep carries them as
      CSV columns and `fig9_recovery_cost` plots ms per percentage point
      recovered.
      - One axis, not two: wall time alone rewards an arm that fails fast, and
        percentage alone hides what the percentage cost.

      | k | fastfhir | hl7v2 | ndjson | json |
      |---|---|---|---|---|
      | 1 | 63 ms | 35 ms | 36 ms | 46 ms |
      | 2048 | **84 ms** | 645 ms | 2,509 ms | **5,638 ms** |

      - **FastFHIR is flat across a 2000× damage range** because it checks
        witnesses already on the wire — work proportional to the STREAM. The
        others must SEARCH for a repair that parses, so their cost tracks the
        DAMAGE (json 123×, ndjson 70×, hl7v2 19×).
      - **protobuf is excluded from fig9, and this is not an oversight.**
        Researched 2026-09-08: protobuf has no recovery mechanism and no
        recovery tooling. Its parser validates WIRE FORMAT only, so corruption
        leaving a message well-formed is undetectable by construction, and the
        ecosystem tools (protobuf-inspector, protoscope, blackboxprotobuf,
        protod) reverse-engineer unknown schemas rather than repair bytes. The
        TLV resync in `arm_google_fhir_codec.hpp` is a heuristic THIS REPO
        wrote so the arm would not be measured against nothing. Its recovery
        percentage stays in fig8; costing it would imply protobuf users pay that
        price for that benefit, and they do neither. Its timings also FALL with
        damage (32 → 9 ms) because it gives up earlier, which on a cost axis
        reads as efficiency.
      - ⚠ Process note: the `recover_ns` edits to `scripts/recovery_sweep.py`
        were lost once between runs and the sweep silently emitted the old
        header. Caught by checking the CSV columns before trusting the data.
        **Check the header, not just that the sweep exited 0.**

### Acceptance — this block is done when

1. hl7v2 has a `recover_stream()` that does something its baseline scan does not
   (REC-1 + REC-2). The other arms already clear this bar.
2. The `--check` process still verifies recovered units are a content-verified
   subset of the clean baseline. **Repair must never manufacture a passing
   unit** — a repair that invents plausible content scores `spurious`, and the
   four-outcome census is what proves it did not.
3. Cross-arm census parity still holds at 34,839.
4. Both curves are published: recovery ON and OFF per arm, so the *delta* is
   visible. FastFHIR's headline becomes that delta, not the absolute.
5. The write-up distinguishes **resynchronise-and-discard** (json, google_fhir,
   and hl7v2 after REC-2) from **repair** (FastFHIR, and any arm after REC-3).
   Those are different capabilities and the curve should not blur them.

## ▶ UP — filed upstream

**Status** FILED 2026-08-26 into `../FastFHIR/TASKS.md`. **Not committed** —
that tree is a symlinked live checkout. Track them here; do not fix them here.

| Upstream ID | Ask | Blocks |
|---|---|---|
| **CAPI-1** | Public API for writing an inline-block array | PA-1 (Test 1 parity) |
| **CAPI-2** | `validate_FFHR_stream()` accepts streams the deserializer segfaults on | Test 1 output gating |
| **CAPI-3** | Block-typed `ChoiceEntry` cannot round-trip across arenas | PA-3, D2 Tier B |
| **CAPI-4** | No zero-copy reader for packed date/time | PA-7 (Test 3 distortion) |
| **CAPI-5** | `TypeTraits<std::string>` undefined while POCO fields are `std::string` | papercut only |
| **CAPI-7** | `FF_FieldInfo` has no `name_len`, so reflection pays a `strlen` per field — ~13% of a walk | PA-11 |
| **CAPI-8** | `entries()` allocates per array; no non-allocating iterator — ~18% of a walk | PA-11 |
| **CAPI-6** | Stale doc comment `include/FF_Ingestor.hpp:69` | — |
| **I3.6** | The orjson citation names a result this repo cannot produce | IN-A |
| **I3.7** | The −66 % compact figure predates the compaction fix; nothing pins it | IN-E |

- [ ] Re-check each on the next upstream sync; move to PA/IN when it lands.

---

## ▶ HY — hygiene

**Status** OPEN

- [ ] **HY-1. AddressSanitizer in CI.** The compensating control for **D1**.
      The ODR violation was live for the entire life of the benchmark and no
      ordinary build ever complained; one ASan run names it in seconds.
      ```
      bazel build -c dbg --copt=-fsanitize=address --copt=-fno-omit-frame-pointer \
        --linkopt=-fsanitize=address --strip=never //bench:bench_harness
      ```
- [ ] **HY-2. Assert every timed stage produced observable work.**
      `(void)result` let the optimizer delete the Test 2 walk; part of the
      original 83 ns was dead code (notes.md §2). Every stage must produce a
      value that escapes, checked against an expected magnitude.
- [ ] **HY-3. Nothing between the last real operation and `stop_ns()`.** A
      `getenv` diagnostic landed inside the Test 1 window during the port.
- [ ] **HY-4. Keep `--seed` deterministic by default** (currently `20260825u`,
      [`bench/main.cpp:133`](bench/main.cpp:133)) and record it in results
      metadata alongside profile and SHA.
- [ ] **HY-7. 🔴 A stage that produces zero results must fail the run.** The
      LOINC query returned **0 matches over 24,583 observations** and every
      check passed — including the cross-arm parity gate, because all four arms
      were equally wrong. Agreement is not correctness; four readers agreeing on
      zero is what a shared upstream defect looks like. Every counting stage
      needs an **absolute floor** alongside its cross-arm comparison:
      - Test 3 must assert `loinc_matches > 0` (the corpus contains ~3.75 per
        file; a run finding none is a failure, not a fast number).
      - Test 2 already has its byte-parity gate — add `bytes_read > 0`.
      - Any future census field: assert non-zero before asserting equality.

      This is the third time this class of defect has hidden here: the 1-node
      Test 2 walk (notes.md §2), the HL7v2 random-access probe reading 0 bytes
      twice, and now the query. The pattern is always the same — a gate that
      compares two values without first requiring either to be meaningful.
      Filed upstream as a testing-policy P0 too, since their suites share it.

- [ ] **HY-5. Retire the stale Google-arm "stub" text.** notes.md §5b disproved
      it and correction banners were added to the tops of the affected
      documents, but the body text still asserts it at
      `RESOURCE_COVERAGE_ANALYSIS.md:78`, `:360` and
      `MESSAGE_SURFACE_PARITY_AUDIT.md:109-110`. **A banner at the top of a file
      does not travel with a retrieved chunk** — both repos are navigated
      through a chunk-level `.arbiter/` index, so a stale line is a stale answer.
      Strike them in place.
- [ ] **HY-6. Re-index `../FastFHIR/.arbiter`.** It was generated 2026-08-24
      15:28:33, one second *before* `../FastFHIR/handoff.md` was written, so
      that document — the source of the "treat all pre-2026-08-24 numbers as
      void" rule and of the compaction-loss history — is not in the index.

---

## ▶ CO — corpus

**Status** OPEN

Out-of-profile resources are retained as opaque JSON and re-emitted
byte-for-byte, but they are **not typed-navigable**: no V-Table, so no `Node`
field access, no query, no interior compaction.

The shipped preset deliberately excludes the `imaging` grouping, so the Synthea
corpus's **1,444 `ImagingStudy` resources take the opaque path on every run**.
That is intentional coverage proving the fallback is lossless — **do not enable
`imaging` to "fix" it.**

- [ ] **CO-1.** A "query every resource" benchmark is not querying every
      resource. Either restrict the corpus to in-profile types or build with a
      wider profile, and **say which** — it changes both the binary and the
      workload.
- [ ] **CO-2.** Report the opaque fraction (resource count and bytes) alongside
      every query result, so a reader can see what share was navigable.
- [ ] **CO-3.** Note in any size comparison that FastFHIR-vs-JSON size is
      apples-to-apples only since the opaque-JSON change; earlier size wins were
      partly FastFHIR dropping data the other arms carried.

---

## ▶ PB — publishable benchmark pipeline

**Status** OPEN · PB-1 ✅ · PB-2a–c, 2e ✅ · PB-2d: full run gate-passed, not yet published · PA-10a–c ✅ · direction agreed with Ryan 2026-09-16 ·
cloud: **Google Cloud** (Ryan's preference) · work top to bottom

**Goal.** A FastFHIR release triggers a benchmark of *that* release on
disclosed, rentable hardware. The result is attached to a GitHub Release here,
and the FastFHIR README links it.

**Why not the obvious hosts.**
- **GitHub-hosted runners:** underpowered, and shared with other tenants.
- **Ryan's machine:** fixed hardware, but nobody else can rent it to reproduce a
  number. It stays the fallback runner and the development loop.

**Architecture (agreed):**

```
FastFHIR: push tag vX.Y.Z
  └─ release.yml: build, GitHub Release (source tarball + generated_src)
       └─ repository_dispatch → FastFHIR-benchmark  {tag, sha}

FastFHIR-benchmark: bench-release.yml
  job 1 (ubuntu-latest): create bare-metal GCE VM from a Packer image, register ephemeral runner
  job 2 (that instance):    pin FastFHIR@sha → scripts/run_benchmark.sh → gates
  job 3 (ubuntu-latest): release bench-fastfhir-vX.Y.Z (metrics.csv,
                         provenance.json, figures); ALWAYS delete the VM
```

- **GitHub Actions runs the pipeline; the GCE VM only runs the benchmark job.**
  - Terraform (`google` provider) owns only the long-lived pieces: Workload
    Identity Federation for GitHub OIDC, a least-privilege service account, an
    instance template, an outbound-only firewall, and the Packer image
    (`googlecompute` builder).
  - **Terraform is cloud-agnostic as a tool, not as a config.** The resources
    are provider-specific, so moving clouds means rewriting `infra/`, not
    re-pointing it. Keep everything cloud-specific inside `infra/` and the
    workflow's create/delete steps.
  - **Do not `terraform apply`/`destroy` per run.** A cancelled job leaves a
    running bare-metal VM and a locked state file.
  - Create and delete the VM from the workflow instead:
    - `google-github-actions/auth`, then `gcloud compute instances create`.
    - A startup script registers an ephemeral runner using a just-in-time
      runner config.
    - Put the delete step under `if: always()`.
    - GCP has no maintained equivalent of AWS's `ec2-github-runner` action, so
      this glue is ours; keep it small.
- **A cloud VM is not quieter than a laptop by default.** Credibility comes from:
  - **Bare-metal machine types** (GCE `*-metal`, e.g. the C3 bare-metal
    shapes): no hypervisor and no neighbours, and the governor, turbo and SMT
    settings can be changed. Pin cores with `taskset`.
    - Check current metal availability per region before committing.
    - If arm64 (Axion) has no metal shape, the fallback is a sole-tenant node,
      recorded as such.
    - Metal shapes may need quota and have no spot option.
  - **x86_64 and arm64 both.** One CPU family does not support the claim.
  - **Same-box A/B.** Every run also measures the *previous* release,
    alternating runs with the new one. Two instances of one type differ by a
    few percent; two builds on one box do not.
  - **Disclosure.** The instance type, image, kernel, CPU model, microcode and
    governor/SMT state go into `provenance.json`. A machine family does not
    pin the exact CPU stepping or microcode.
- **Publish gate:** the harness exits 0 (parity clean) **and** provenance is
  complete **and** `fastfhir_dirty == false`. Automation does not fix the open
  **PA** items; until they close, the pipeline runs but does not publish.
- **README reference:**
  `https://github.com/ryanlandvater/FastFHIR-benchmark/releases/latest/download/<fig>.svg`
  for the headline figure, and the versioned release for the data.

### PB-1 — pin the library under test (this repo, local first)

- [x] **PB-1a. A pinned FastFHIR checkout, not the live tree.** ✅ 2026-09-16
      - [`scripts/pin_fastfhir.sh`](scripts/pin_fastfhir.sh) `REF [--profile P]
        [--repo URL|PATH]` keeps a bare mirror at `.external/pins/FastFHIR.git`.
      - It creates one detached checkout per (commit, profile):
        `.external/pins/fastfhir-<sha12>-<profile>`.
      - It refuses a checkout that has moved or become dirty, and prints the
        path on its last line.
      - [`scripts/run_benchmark.sh`](scripts/run_benchmark.sh)
        `--fastfhir-ref REF [--fastfhir-repo R]` runs it as stage 0 and passes
        `--override_module=fastfhir=<pin>` to every bazel call.
      - The plan prints "live tree -- not publishable" when no ref is given.
      - `MODULE.bazel`'s `local_path_override` is unchanged, so it stays the
        developer default.
      - New [`.bazelignore`](.bazelignore) excludes `.external`: pinned trees
        carry their own BUILD files and must never load as packages here.
      - Not `archive_override`: a tarball has no `.git`, so provenance could not
        establish the SHA from the tree.
- [x] **PB-1b. Generate the profile deterministically.** ✅ 2026-09-16
      - The pin script runs `python -m generator` (≥ 3.11) in the pinned tree
        from an empty `generated_src/`, then writes the `.profile` stamp only
        on success, mirroring `../FastFHIR/CMakeLists.txt:182-197`.
      - Profile source: `--profile`, else the pinned commit's own
        `CMakePresets.json` base preset.
      - `fhir_packages/` is reused by symlink and excluded through the clone's
        `.git/info/exclude`. Upstream's ignore rule `fhir_packages/` matches
        only a directory, so the symlink would otherwise read as dirty.
      - **Verified:** the pin of `ee578e4` at the shipped profile generates a
        tree **byte-identical** to the live `../FastFHIR/generated_src`
        (`diff -rq` clean). Pinning takes 3.6 s; a repeat reuses the pin.
      - CI still needs network access or a cached `fhir_packages/` (PB-3b).
- [x] **PB-1c. Provenance names the tree Bazel compiled.** ✅ 2026-09-16
      - `find_fastfhir_root()` resolves `bazel-<ws>/external/fastfhir~`
        (and `+` for Bazel 8) before the workspace symlink.
      - New field `fastfhir_path_source`.
      - **Verified:** a pinned quick run records the pin path with source
        `bazel external repo`, `fastfhir_dirty: false`, and profile source
        `generated-tree stamp`.
      - Caveat: the link reflects the **last build**, so `--skip-build` with a
        ref prints a warning.
- [x] **PB-1d. Host disclosure fields (recorded, not gated).** ✅ 2026-09-16
      - New fields: `kernel`, `logical_cpus`, `memory_bytes`, `cpu_microcode`,
        `cpu_governor`, `smt_control`, `turbo`.
      - From the environment: `BENCH_HOST_INSTANCE_TYPE`, `BENCH_HOST_IMAGE`,
        `BENCH_HOST_RUNNER`.
      - Printed in the stderr summary. The `/sys` fields are empty on macOS by
        design.
      - Also fixed: `generated_resources` counted `ChoiceBlock` and
        `Conformance_Layer` as resource types (38 reported; 36 real at `ee578e4`).

### PB-2 — the workflow, on a local self-hosted runner

- [x] **PB-2a. Workflow.** ✅ 2026-09-16
      [`.github/workflows/bench-release.yml`](.github/workflows/bench-release.yml).
      - Triggers: `workflow_dispatch` (inputs `fastfhir_ref`, `baseline_ref`
        default `auto`, `runner_label` default `bench-local`, `quick` default
        **on**, `publish` default off) and `repository_dispatch` type
        `fastfhir-release`. A dispatched run is always full and publishes if
        the gate passes.
      - **No `pull_request` trigger, ever:** both repos are public.
      - Inputs and payloads reach the shell only through `env:`.
      - One run per runner label (`concurrency`); 12 h timeout.
      - Runner `.env` supplies `BENCH_CORPUS_DIR` (the workflow links
        `datasets/synthea` to it) and, optionally, `FHIR_PACKAGES_DIR` and
        `BENCH_HOST_*`.
      - The workflow creates `datasets/` itself: neither it nor
        `artifacts/.keep` is tracked, despite `.gitignore` expecting them.
      - Python deps are pinned in [`requirements-bench.txt`](requirements-bench.txt).
      - Also fixed: `.gitignore` excluded only `/bazel-fastfhir-benchmark`
        (lowercase). On Linux the link is `bazel-FastFHIR-benchmark`, so every
        CI checkout would have read as dirty. It is now `/bazel-*`.
- [x] **PB-2b. Same-box A/B.** ✅ 2026-09-16
      [`scripts/bench_ab.py`](scripts/bench_ab.py).
      - Builds: pins both refs and builds the baseline first, so `bazel-bin/`
        ends on the candidate. Each copied binary's SHA-256 is recorded with
        its tree and re-checked before every invocation.
      - Timing: one process per (side, size, replicate), in ABBA order.
      - New harness flags: `--replicate-first N` (a replicate's bundle depends
        only on seed, size and index) and `--fastfhir-root DIR` (provenance
        source `operator`). Bazel's link names only the last build, so it
        would mislabel the other side.
      - Outputs: `candidate/` and `baseline/` in the normal `metrics.csv` and
        `provenance.json` format, `ab_summary.csv` (medians plus the **paired**
        per-replicate ratio with min/max), and `ab.json`.
      - Exit 2 on any parity failure; the run still completes.
      - Verified: both sides measure identical bundles (0 mismatches across all
        keys), and each provenance names its own SHA.
      - `--baseline auto` = newest tag before the candidate, else its parent.
        FastFHIR has no tags yet.
      - **Finding: an A/A is detected by binary digest, not by ref.** The smoke
        test compared `ee578e4` against `4703c71`. `ee578e4` changed only
        tests, so both binaries were byte-identical. Paired ratios still ranged
        **0.83–1.60** at 1 run × 2 replicates on this Mac.
        - That spread is the noise floor of a tiny ladder, and it is why
          `--quick` can never publish.
        - `ab.json` sets `a_a`, and the release notes say so.
      - Process startup is ≈2.4 s, which adds ≈11 min over a full two-sided
        ladder. Acceptable.
- [x] **PB-2c. Publish job and gate.** ✅ 2026-09-16
      [`scripts/publish_check.py`](scripts/publish_check.py) checks that:
      - `ab.json` is complete, the run was not quick, and parity held;
      - both provenance files exist (the harness writes them only when
        complete) with `opt`;
      - neither FastFHIR tree nor the benchmark checkout is dirty;
      - each provenance SHA matches the SHA `ab.json` says that side built.

      It writes release notes (a provenance table, plus the FastFHIR-arm
      paired ratios at the largest size) into the job summary, whether or not
      the gate passes.

      The publish job (ubuntu, `contents: write`) creates
      `bench-fastfhir-<describe>` as `--latest`, or adds this platform's
      assets to a release another platform already created.
      - Asset names are stable and platform-suffixed, e.g.
        `fig1_duration_by_stage-linux-x86_64.svg`, `metrics-<platform>.csv`,
        and `bench-results-<platform>.tar.gz`.
      - Rehearsed locally: the gate correctly refused (dirty checkout), and
        every figure rendered as SVG and PNG.
- [ ] **PB-2d. Prove it end to end on Ryan's Mac.**
      - **Quick run: ✅ 2026-09-16**, run
        [35119129906](https://github.com/ryanlandvater/FastFHIR-benchmark/actions/runs/35119129906).
        - Green in 7 min on runner `Ryans-MacBook-Pro`. `baseline auto` resolved
          to `4703c71`, and the run was flagged A/A (identical binaries).
        - The gate refused it with a single reason, *quick run*.
        - `benchmark_dirty: false` confirms the `.gitignore` fix works on a
          real checkout.
        - Publish was skipped; the artifact carries the notes, both sides and
          the figures.
      - Gotcha: the runner needs the **`bench-local` label**. A label added
        while `run.sh` is running is not seen until `run.sh` is restarted.
      - Actions warned that `actions/checkout@v4` and `upload-artifact@v4`
        target the deprecated Node 20. ✅ Bumped to checkout@v7,
        upload-artifact@v7 and download-artifact@v8 (Node 24; v8 fails on a
        digest mismatch).
      - The ladder now reaches **256 MB**. `bench_ab.py` defaults to the
        harness's own ladder `1,2,4,8,16,32,64,256`, and the workflow has a
        `targets_mb` input.
        - Measured on this Mac: one 256 MB replicate (3 runs + 1 warmup) takes
          58 s, with a 4.9 GB peak footprint. The 256 MB rung therefore costs
          ≈40 min across both sides.
      - Full run 1 ([35125924135](https://github.com/ryanlandvater/FastFHIR-benchmark/actions/runs/35125924135))
        failed in setup: the Mac runner uses `/bin/bash` 3.2, where an empty
        array is "unbound" under `set -u`. The expansions are now guarded.
      - Full run 2 ([35126101211](https://github.com/ryanlandvater/FastFHIR-benchmark/actions/runs/35126101211))
        failed at its first measurement, for two reasons:
        1. The unquoted `targets_mb` default `1,2,4,8,16,32,64,256` reached
           the job as `1248163264256`: GitHub's YAML parser dropped the commas
           and read it as a number.
        2. The harness then tried to build a 1.2 TB bundle and was killed
           (SIGKILL) after 56 s.

        The default is now quoted, and `bench_ab.py` rejects any target
        outside 1..4096 MB. Nothing was measured or published.
      - **Full run 3: ✅ gate passed 2026-09-16**, run
        [35126898251](https://github.com/ryanlandvater/FastFHIR-benchmark/actions/runs/35126898251),
        1 h 21 min on `Ryans-MacBook-Pro`.
        - Timing took 68 min; artifacts, recovery sweep and figures took
          12.5 min. The sweep is minutes on this machine, not the "hours" the
          scripts' comments warn about.
        - Parity held on every invocation.
        - **A/A noise floor, full ladder** (identical binaries), FastFHIR arm
          at 256 MB: paired median ratios 1.001–1.019 across the eight stages.
          Single-replicate extremes were 0.861 (`test_3_selective`) and 1.353
          (`test_3_query`). A release delta under ≈2% is not resolvable on
          this Mac.
        - The sweep logged **12 driver crashes** (segfault on a pathological
          repaired stream), scored 0%, not dropped. See `_crashes.csv` in the
          artifact.
        - **Publish job was skipped** although the gate said *Publishable*.
          Either `publish` was not ticked at dispatch, or `publishable` did not
          propagate as a job output. The run API does not expose dispatch
          inputs, so this is unconfirmed.
      - Remaining: publish (re-run with `publish` on, or confirm the output
        propagation).

- [x] **PB-2e. Build warnings: 1,004 → 2.** ✅ 2026-09-16. Measured on a
      fresh `--output_base`, `-c opt`, Xcode 27 / clang 21.
      - `.bazelrc` never applied any `build:<os>` line, because
        `--enable_platform_specific_config` was missing. It is now on, so the
        macOS deployment target (`minos 15.0`, verified with `otool`) and the
        Windows `/std:c++20` take effect.
        - **This changes the macOS binaries.** Results from before this change
          are not bit-comparable with results after it.
      - Other people's code is silenced; codegen is unaffected:
        - `--features=external_include_paths` includes other modules' headers
          with `-isystem`.
        - A `-Wno-*` `--per_file_copt` is scoped to abseil, protobuf and
          apple_support sources.
        - FastFHIR's own `.cpp` files are deliberately **not** silenced.
      - `apple_support` 1.23.1 → **2.8.3**. 1.23.1 builds its wrapper tools with
        a hardcoded `-mmacosx-version-min=10.15`, which Xcode 27's libc++ warns
        is unsupported (below 11.0). 2.8.3 uses 11.0 and needs Bazel ≥ 7.4.
      - `protobuf` is declared as the **resolved** `29.0-rc3` (required by
        rules_python 1.9.0 via fastfhir, and by rules_cc) instead of 27.0. The
        binaries are unchanged; this silences the resolution warning. Moving to
        a stable 29.x changes the google_fhir arm and is a separate decision.
      - Ours: removed four unused `run_*` helpers in `bench/main.cpp`, and a
        C++20-deprecated volatile assignment chain in `bench/arm_fastfhir.cpp`
        (on the diagnostic `BENCH_FF_PREFAULT` path only).
      - The two cc_tests are now `size = "small"`, which removes Bazel's "size
        too big" warning.
      - **Remaining 2 are FastFHIR's**, in `src/FF_Recovery.cpp`: an unused
        function and an unused lambda capture. Filed upstream as **E14**.

      Setup, for reference:
      1. Register a runner with label `bench-local` (Settings → Actions →
         Runners).
      2. Put `BENCH_CORPUS_DIR=/Users/ryanlandvater/GitHub/FastFHIR/build/synthea_fhir_r4`
         in the runner's `.env`.
      3. Start it with `./run.sh`, then dispatch `bench-release` with the
         defaults (quick).

      Expected: green, gate refuses as *quick*. Then do one full run with
      `publish` on.
      - Not yet exercised: the workflow YAML itself (no `actionlint` here),
        `upload-artifact` exclusions, and the release commands.

### PB-3 — cloud runner (Google Cloud)

- [ ] **PB-3a.** `infra/terraform/` (`google` provider):
      - Workload Identity pool and provider restricted to this repo.
      - A service account allowed only to create and delete labelled VMs in one
        project and zone.
      - An instance template and an outbound-only firewall.
      - Remote state in a GCS bucket (GCS locks natively).
- [ ] **PB-3b.** `infra/packer/` (`googlecompute` builder): Ubuntu LTS image
      with the toolchain, Bazel 7.7.1, Python ≥ 3.11, a warm Bazel repository
      cache, `fhir_packages/`, and the Synthea corpus.
      - **The corpus is not self-contained today.** `datasets/synthea` links to
        `../FastFHIR/build/synthea_fhir_r4` (342 docs, 1.29 GB, sha256
        `958b1ceb2642…`).
      - The image must build the corpus from a pinned source, or store that
        exact directory (e.g. in a GCS bucket) and check it against
        `corpus_sha256`.
- [ ] **PB-3c.** Host tuning step before timing: `performance` governor, turbo
      off, SMT off, `taskset` pinning. Record every setting (PB-1d).
- [ ] **PB-3d.** Matrix x86_64 + arm64; each arch gets its own release assets.
- [ ] **PB-3e.** Measure one full run's wall time on the VM and record the cost
      per release here.

### PB-4 — upstream trigger (FastFHIR repo — Ryan's to commit, not ours)

- [ ] **PB-4a.** FastFHIR has **no workflows and no tags** (checked
      2026-09-16). It needs a `release.yml` on tag push: build, test, and a
      GitHub Release whose source asset includes `generated_src/` for the
      shipped profile.
- [ ] **PB-4b.** `repository_dispatch` to this repo with `{tag, sha}`, using a
      fine-grained PAT or a GitHub App token. Fallback without a shared
      secret: a scheduled job here that checks FastFHIR's releases.
- [ ] **PB-4c.** FastFHIR README: embed the `releases/latest/download` figure
      and link the versioned release.

### PB-5 — publication readiness

- [ ] **PB-5a.** The harness exits 0: close the open **PA** items (HL7v2
      `obs_issued_present` / `obs_component_value_*`, Test 1 parity).
- [ ] **PB-5b.** Remove "results not yet publishable" from README only when
      PB-5a holds on the cloud runner.

---

## ▶ PR — build provenance with every result

**Status** PR-1/PR-3 closed; PR-2 is enforced at publish time by **PB**. The
remaining pinning work is **PB-1**.

The compiled profile changes the binary under test with **no Bazel-visible
signal**: `.external/FastFHIR` is a symlink to the live tree; Bazel does not run
the generator (`../FastFHIR/BUILD.bazel` globs `generated_src/*.cpp`, which
**CMake** produces at configure time). So the benchmarked profile is whatever
CMake last generated.

Current upstream state (verified 2026-08-24): profile
`us-core,billing,medication-admin,supply`; 80 code-system enums; 44 generated
`.cpp`; 37 resource types; `ImagingStudy` **not** compiled.

- [x] **PR-1.** Emit profile, upstream git SHA, dirty flag, and
      `--compilation_mode` into the results CSV and the PG schema.
      *Done by IN-0 (`provenance.json` + `benchmark_runs` columns).*
- [x] **PR-2.** Fail the run if `.external/FastFHIR` has uncommitted changes, or
      record the dirty state in the metadata. *Recorded (`fastfhir_dirty`) by
      IN-0; the PB publish gate refuses a dirty tree.*
- [x] **PR-3.** *Upstream CMake now refuses a mismatched profile and prints
      this instruction (`../FastFHIR/CMakeLists.txt:151-170`).* Document that a profile change requires
      `rm -rf ../FastFHIR/generated_src` before regenerating — the generator
      never deletes output it no longer emits, so a stale tree survives.

---

## ▶ IF — infrastructure

**Status** OPEN

- [ ] **IF-1.** [`generate_repo.sh:403-418`](generate_repo.sh:403) falls back to
      `tools/generator/make_lib.py`, which no longer exists. The generator is
      `python -m generator`, or `cmake --preset ninja` from `../FastFHIR`.
- [ ] **IF-2.** [`bench/main.cpp:31`](bench/main.cpp:31) hardcodes
      `/Users/RyanLandvater/Programming_Projects/FastFHIR-benchmarking/datasets/synthea`.
      Drop it; keep the relative fallback and add `--synthea-dir`.
- [ ] **IF-3.** `local/include/` holds one orphaned `FF_Bundle.hpp` from the
      CMake-install era. Delete it.
- [ ] **IF-4.** Google FHIR's Bazel build is macOS-only here, leaving
      `bench_harness_win` unbuildable on Windows. **Fix the dependency, not the
      target** — dropping an arm on one platform makes the platforms
      non-comparable, which defeats the benchmark.
- [ ] **IF-5.** Keep `SYNTHEA_DATA_URL` in sync between README and
      [`generate_repo.sh:22`](generate_repo.sh:22).

---

## Standing rules

- **D1, D2, D3 above are decided.** Reopening one needs Ryan, not a rationale.
- **Keep `--compilation_mode=opt`.** Never publish a number from `-c fastbuild`
  or `-c dbg`. FastFHIR's own CMake presets are all Debug `-O0` and the same
  code runs ~10× faster optimized — this is how upstream's §A/§B tables came to
  be labelled `-O2` while being Debug measurements.
- **Never trust a result from a build you did not confirm succeeded.** A stale
  test binary once printed `8/8 PASS` while its build was failing with 10 errors.
- **Do not commit into `../FastFHIR` from here.** It is a symlinked live tree.
  Filing into its `TASKS.md` is fine; committing is not.
- **Record the profile with every published result** (PR).
- **Do not enable the `imaging` grouping** in `../FastFHIR/CMakePresets.json`.
- **Treat all pre-2026-08-24 FastFHIR numbers as void** — they predate the
  opaque-JSON change (`../FastFHIR/handoff.md` §1).
- **Update this file as you go.** Check the box, add a Progress-log line, and
  note what the change did *not* settle. A backlog that is only accurate at the
  end of a session is not a backlog.
