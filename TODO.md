# Instrument G — Resilience & Integrity Suite (design spec)

> ✅ **SHIPPED 2026-08-26** — implemented as
> [`bench/resilience_test.cpp`](bench/resilience_test.cpp), all four tests
> passing (truncation, bit-flip, type-confusion, concurrent build) plus a
> recovery probe (test 5): structural-bit-flip curve vs % entries recovered
> by VALIDATION-word resync (`fig8_recovery`). Two upstream findings:
> CAPI-13 (Parser ctor SEGVs on a corrupted header) and CAPI-14 (POCO
> `string_view` fields dangle on temporary assignment).
> This document remains the spec/record; the tracker is TASKS.md **IN-G**.

> **This is a design specification, not a task list.** The only backlog is
> [`TASKS.md`](TASKS.md); this document is what **IN-G** there points at. Four
> tests that exercise FastFHIR's structural integrity guarantees under
> real-world corruption scenarios. Each maps to a failure mode observed in
> production FHIR/HL7v2 pipelines, and to a claim in
> [`TASKS.md`](TASKS.md)'s Decisions.

**Claims this instrument validates:**

| Test | Claim | § Why FastFHIR? |
|---|---|---|
| 1 — Truncation detection | **WF-3.1** OS-protected memory, stable pointers, deterministic layout | § 3 |
| 2 — Bit-flip detection | **WF-3.3** checksum footers detect corruption — *integrity, not authenticity* | § 3 |
| 3 — Type-confusion prevention | **WF-3.2** `RECOVERY_TAG` catches mis-typed reads — *malformed, not hostile* | § 2, § 3 |
| 4 — Concurrent build integrity | **WF-4.2** lock-free concurrent generation (correctness half) | § 4 |

⚠️ **Preserve the qualifiers.** README § 3 is already careful: 3.2 is scoped to
*malformed* data, not *hostile* (the parser has not been fuzzed — upstream
TASKS.md G1), and 3.3 is *integrity*, not *authenticity* (an attacker who can
rewrite payload bytes can recompute the footer). Any artifact this suite emits
carries those qualifiers forward verbatim. Do not let a headline round them off.

> **Unblocked 2026-08-25** — the blocker was the API port, closed that day
> (TASKS.md § PORT). ⚠️ The API references below were written against the
> pre-`a9fd4e9` surface and have **not** been re-verified since; run a pass
> against the current headers before implementing. Known moves: `Builder::set_root`
> / `finalize` are private (use `make_builder()` / `seal_stream()` in
> `bench/harness.hpp`), and `SourceType::FHIR_JSON` is now `FF_SOURCE_FHIR_JSON`.

> **Test 4's second blocker is cleared (2026-09-05).** The parallel path in
> `bench/arm_fastfhir.cpp` was commented out, so the FastFHIR arm built its
> Bundle on one thread while the json_fhir arm ran `dispatch_apply_f` across
> every core. Test 1 therefore compared 1 thread against 18 and reported a flat
> ~5.4x ratio at every corpus size — the constant ratio was the artifact.
>
> The arm now builds through a `FIFO::Queue` worker pool in the
> `FF_Ingestor.cpp` step-5 shape: pre-allocate the Bundle's inline entry array,
> latch one consumer per worker before the first push, and have each worker
> append its resource and amend its own parent slot. `--serial-build` forces
> every arm to one thread for the encoder-only comparison; `hl7v2` and
> `google_fhir` are serial in both configurations, which the run banner states.
> Every Test 1 row now carries `cpu_ns`, so `cpu_ns / duration_ns` is the
> average number of cores the stage occupied.

**Data source:** All tests use Synthea FHIR JSON files from `datasets/synthea/`
(119 real patient bundles with Observations, Conditions, Encounters, Procedures).
This ensures realistic payload sizes, nested structures, and resource diversity.
The directory is populated by `generate_repo.sh` and is **not** checked in — it
does not exist in a fresh clone until that script runs.

Ingestion path, in current API terms:
`make_bundle_patient_from_json()` → `FF_CreateIngestor` / `FF_Ingest`
→ `BundlePatient` struct → `FF_BuilderAppendObject()` → `FF_BuilderSetRoot()`
→ `FF_BuilderFinalize()` → sealed `Memory::View`; same pipeline as the
benchmark harness.

**A resilience-specific note on the upstream change:** out-of-profile resources
are now retained as opaque JSON blocks rather than dropped. Opaque blocks carry
the same 10-byte universal header (`VALIDATION` + `RECOVERY_TAG`) as every
other block, so the truncation and bit-flip mechanisms below apply to them —
but their *interiors* are unparsed bytes, so corruption inside an opaque
payload is detectable only at block granularity, not field granularity. Any
test that asserts field-level damage detection must run on an in-profile
resource. Worth an explicit test either way: the Synthea corpus routes 1,444
`ImagingStudy` records through this path on every run.

---

## Test 1: Truncation Detection via VALIDATION Word

**Real-world analog:** Network streams drop mid-transmission — TCP RST, proxy
timeout, partial socket read. In HL7v2 this produces segment-boundary
misalignment (parser reads into garbage or interprets trailing bytes as a
phantom segment). In FHIR JSON it produces an unparseable trailing fragment
(best case) or a silently truncated array (worst case).

**FFHR-unique mechanism:** Every `DATA_BLOCK` stores its own absolute arena
offset at bytes 0–7 (`VALIDATION`). The `FF_HEADER` carries `STREAM_SIZE`.
After truncation the first incomplete block either has a `VALIDATION` word
pointing past EOF or one that doesn't match its arena position — both
detectable by `validate_offset()`. `STREAM_SIZE` also won't match actual byte
count — Parser catches this at construction.

**Implementation:**
1. Ingest a Synthea patient bundle → build & seal with `Builder::finalize()`.
2. Copy sealed bytes into `std::vector<uint8_t>`.
3. Truncate at N offsets bracketing structural boundaries: mid-`FF_HEADER`,
   mid-V-Table of a Patient block, mid-string-payload of a name field,
   mid-`FF_ARRAY` entry region, between two Bundle entries.
4. For each truncation attempt `Parser(buffer, truncated_size)`.
5. Assert: every truncation either (a) throws `std::runtime_error` at Parser
   construction, or (b) if Parser accepts, a recursive tree walk detects at
   least one block whose `VALIDATION` ≠ its offset.

**Key assertion:** No truncated Synthea stream is accepted as valid without
surfacing an error. FastFHIR never silently returns partial data.

---

## Test 2: Bit-Flip Detection — Structural vs Payload Damage

**Real-world analog:** Storage media degradation, faulty RAM, noisy
interconnects. In HL7v2 a single flipped bit in a segment ID (`OBX`→`OBY`)
silently changes semantic meaning of an entire segment group. In FHIR JSON a
flipped bit in `"resourceType"` or a numeric lab value produces no structural
error — the parser gives you wrong data with no indication.

**FFHR-unique mechanism:** Every block carries a 10-byte universal header:
8-byte self-referencing `VALIDATION` offset + 2-byte `RECOVERY_TAG`. A
random bit flip in this 10-byte region is detectable **without computing a
stream-level checksum**:
- Flip in `VALIDATION` → `validate_offset()` fails (stored offset ≠ actual).
- Flip in `RECOVERY_TAG` → tag won't match any known type, or matches the
  *wrong* type (caught by `TypeTraits<T>::read()`).

Crucially, FFHR **separates structural damage from payload damage**: a
flipped bit in a string payload (e.g. a patient name) leaves the block header
intact — the tree remains traversable even though the clinical value is
garbled. This localization is impossible in HL7v2 (where a delimiter flip
cascades) and in JSON (where a bracket flip destroys the entire document).

**Implementation:**
1. Ingest a Synthea patient with at least 5 Observations → build & seal.
2. Walk the sealed stream; record every block's offset, header region, and
   payload region.
3. **Structural flip:** Flip one bit in the `VALIDATION` word of a mid-stream
   Observation block. Assert: `validate_offset()` returns false for that
   block. Assert: blocks *before* the damage are still reachable and their
   data is intact. Assert: the damaged block is flagged.
4. **Tag flip:** Flip one bit in the `RECOVERY_TAG` of a block. Assert:
   `node.as<ExpectedType>()` throws or returns error.
5. **Payload flip:** Flip one bit in an Observation's `valueQuantity.value`
   payload bytes. Assert: the block's `VALIDATION` word and `RECOVERY_TAG`
   are intact. The tree walk completes. The garbled value is accessible
   (structural integrity holds); only the clinical datum is corrupted.

**Key assertion:** Structural corruption is detected at block granularity.
Payload corruption is isolated — the block structure survives. This is the
property that lets a scanner recover from mid-stream damage by resynchronizing
at the next valid `VALIDATION` word.

---

## Test 3: Type Confusion Prevention via RECOVERY_TAG Dispatch

**Real-world analog:** A resource is mislabeled in a pipeline — a Condition
gets tagged as an Observation, or a Procedure's JSON body is pasted under a
wrong `resourceType`. HL7v2: segment types are 3-byte ASCII strings; `OBR`
vs `OBX` differ by one character, trivial to mangle. FHIR JSON: `resourceType`
is a string field with no structural barrier — a consumer that skips validation
reads Condition fields as if they were Observation fields.

**FFHR-unique mechanism:** The `RECOVERY_TAG` is a binary enum embedded in the
block header, not a payload string. `TypeTraits<T>::read()` checks it before
materializing any bytes. `Node::is<RESOURCETYPE>()` checks it at the node
level. There is no code path that interprets block bytes as type T without
first verifying the recovery tag. Cross-type reads are rejected at the binary
layer — this is not a "validate if you remember to" pattern; it's enforced by
the read API itself.

**Implementation:**
1. Ingest a Synthea bundle containing Patient + Observations + Conditions +
   Encounters + Procedures (a typical Synthea file has all 5).
2. Seal and parse. Assert `parser.root_type()` reports Bundle.
3. Iterate `Bundle.entry`. For each entry's resource node:
   - Call `node.is<RESOURCETYPE::PATIENT>()`, `::OBSERVATION()`, `::CONDITION()`,
     `::ENCOUNTER()`, `::PROCEDURE()`. Assert exactly one returns true.
   - Materialize with `node.as<T>()` for the matching type. Assert no throw.
   - Attempt `node.as<T>()` for at least one *non-matching* type. Assert it
     throws or returns an error sentinel.
4. Walk typed arrays (e.g. `Bundle.entry`). Verify every resource reference's
   inline recovery tag matches the target block's own header recovery tag.

**Key assertion:** RECOVERY_TAG-based dispatch prevents cross-type confusion
at the API level. It is impossible to read a Condition's bytes as an
Observation without the API rejecting it. This is a structural invariant, not
a best-practice convention.

> **Comparative honesty (TASKS.md D2).** State what the other arms actually do,
> not what they lack. Protobuf *does* type-check at the message level — a
> `Condition` cannot be parsed as an `Observation` without a wire-format error
> in most cases — so the FastFHIR claim is about **field-level** dispatch inside
> a block and about detecting a *mislabelled but well-formed* resource. Measure
> that, and say which arms fail it and which do not. An unearned differentiator
> is worth less than a narrow one that survives review.

---

## Test 4: Concurrent Build Integrity — Lock-Free Arena Under Contention

**Real-world analog:** Multiple ingestion threads writing to a shared FHIR
store simultaneously. HL7v2 has no concurrency model — parallel writes to a
file produce interleaved/corrupted output. FHIR JSON bundling has no atomic
multi-writer guarantee — two threads appending to the same NDJSON stream
produce broken records.

**FFHR-unique mechanism:** `Memory::claim_space()` takes no mutex, no
condition variable, and no producer/consumer queue. Each thread gets an
exclusive byte range by construction. `Builder::finalize()` sets
`m_finalizing`, spins on `m_active_mutators` until zero, then seals. The
protocol guarantees that concurrent appends cannot interleave and a finalizer
cannot seal over in-progress writes.

⚠ **This said "a single `fetch_add`" until 2026-09-05 and the code has never
done that** — `src/FF_Memory.cpp:386` is a `compare_exchange_strong` retry
loop, because bit 63 of the same word is `STREAM_LOCK_BIT`. FastFHIR
architecture.md carried the same wrong claim in four places. The correction
matters here because this test's assertion is about **correctness under
contention**, which holds either way; the throughput claim is a separate thing
and it does not currently hold (see the scaling note below).

**Implementation:**
1. Ingest N Synthea patients (N=16, spread across hardware concurrency)
   into `BundlePatient` structs.
2. Create a single `Memory` arena + `Builder`. Spawn one thread per patient.
3. Each thread calls `builder.append_obj(patient)` on its own Patient.
4. All threads must complete before `finalize()` is called (join all, then
   finalize).
5. Assert: `Parser` accepts the sealed stream (no structural errors).
6. Assert: the root Bundle contains exactly N entries.
7. Assert: every entry's Patient `id` field matches one of the input ids
   (no lost writes, no torn writes, no duplicated entries).
8. Seal with SHA-256. Reopen with a fresh Parser. Assert: checksum
   validates (the concurrent build produced a deterministic, verifiable
   stream).

**Key assertion:** Lock-free concurrent arena building is correct under
contention. The final stream is structurally identical to a serial build —
every entry is present, no data is torn, and all integrity guarantees
(VALIDATION word, RECOVERY_TAG, checksum) hold.

**Assert correctness here, not speed.** Measured 2026-09-05 on the Test 1 path
(64 MB corpus, 4,203 resources, `-c opt`, M5 Pro / 18 logical cores): the
concurrent build is correct — `validate_FFHR_stream` code 0, zero null
entries, element parity exact against all three other arms — and it peaks at
the **performance-core count** (1.93× on 6 P-cores of an M5 Pro), regressing
past it as work spills onto E-cores. Both the engine and the arm now default to
`FastFHIR::performance_core_count()`. Attribution and the reproducing env vars
are in README.md ("Concurrent build scaling"); the engine-side work is
FastFHIR
TASKS.md CONC-0 (done; CONC-1/CONC-2 were rejected on measurement) and
architecture.md §7.5. A Test 4 that reports a speedup number must say which
worker count it used, or it is reporting a scheduling artifact as a concurrency
result.

---

## Reporting contract

- Every test emits **pass/fail per scenario**, not a duration. This instrument
  is a conformance gate; nothing here belongs in a timing table.
- Results land in `results/<fastfhir-tag>/resilience.csv` with the
  `provenance.json` required of every artifact (TASKS.md **IN-0**). No
  provenance, no artifact.
- Report the **opaque fraction** with every run (TASKS.md **CO-2**): opaque
  blocks carry the same 10-byte universal header, so truncation and bit-flip
  detection apply to them, but their interiors are unparsed — corruption inside
  an opaque payload is detectable only at *block* granularity, not field
  granularity. Any test asserting field-level damage detection must run on an
  in-profile resource. The Synthea corpus routes 1,444 `ImagingStudy` records
  through this path on every run, so both cases are always available.
