# `bench_test_4.hpp` — Enrichment (Test 4)

> ✅ **Ported 2026-08-25.** The
> `append_obj()` → `set_root()` → `finalize()` sequence is now
> `append_obj()` → `seal_stream()` (which wraps `FF_BuilderSetRoot` /
> `FF_BuilderFinalize`). Also wrapped in a per-arm inline namespace — see
> [notes.md](../notes.md) §1.

## Purpose

Implements **Test 4 (Enrich)**: appending a new Observation resource to an existing serialized bundle. This simulates a common EHR workflow — a lab result arrives and must be added to a patient's record.

Each arm has its own `enrich_*` function that:

1. Takes the arm's serialized payload (sealed arena / JSON string / TLV string / HL7v2 string)
2. Append a new Observation (loaded from `bench/enrich.json`)
3. Re-serializes the updated bundle
4. Returns byte-count and timing metrics

## Architecture

### Common Types

```cpp
struct EnrichMetricsSummary {
  std::size_t source_bytes;
  std::size_t enriched_bytes;
  std::size_t appended_observations;
  std::int64_t duration_ns;
};

template <typename StreamT>
struct EnrichResult {
  StreamT enriched_stream;
  EnrichMetricsSummary summary;
};

inline MetricEvent enrich_metric(arm, duration_ns);
inline std::string format_enrich_summary(summary);
```

### Macro Selection

```cpp
#if defined(ARM_FASTFHIR)
  #define BENCH_TEST_4_ENRICH_FN enrich_fastfhir
#elif defined(ARM_JSON)
  #define BENCH_TEST_4_ENRICH_FN enrich_json
// ...
```

Each arm file includes this header after `#define`-ing its arm macro, which sets `BENCH_TEST_4_ENRICH_FN` to the correct enrichment function.

### Per-Arm Implementations

#### FastFHIR — `enrich_fastfhir`

**Stream type**: `FastFHIR::Memory`

```cpp
EnrichResult<StreamType> enrich_fastfhir(const StreamType& payload,
                                          const ObservationData& enrichment_obs);
```

1. **Copies the arena** — `StreamType enriched_stream = payload;` (shallow copy of FFHR memory handle)
2. **Creates builder** — `FastFHIR::Builder(enriched_stream, FHIR_VERSION_R5)` on the copied arena
3. **Reads bundle root** — `builder.root_handle().as_node()` → `BundleData`
4. **Appends observation** — `builder.append_obj(ObservationData{})` + `assign::assign_observation()`
5. **Pushes to entry list** — `bundle.entry.push_back(BundleentryData{...})`
6. **Re-writes root** — `builder.append_obj(bundle)` → `builder.set_root(new_root)` → `builder.finalize()`
7. **Records metrics** — Source vs enriched byte count, duration

**Key advantage**: Only the new observation is serialized. The existing FFHR binary is not re-parsed or re-serialized.

#### JSON — `enrich_json`

**Stream type**: `std::string`

```cpp
EnrichResult<StreamType> enrich_json(const StreamType& payload,
                                      const ObservationData& enrichment_obs);
```

1. **Parse** — `nlohmann::json::parse(payload)` — deserializes the entire JSON bundle
2. **Append** — Creates a new JSON observation object via `assign::assign_observation()`, pushes to `bundle["entry"]`
3. **Re-serialize** — `bundle.dump()` — serializes the entire JSON back to string
4. **Records metrics**

**Key cost**: The entire bundle must be parsed and re-serialized, even if only one observation is added.

#### HL7v2 — `enrich_hl7v2`

**Stream type**: `std::string`

```cpp
EnrichResult<StreamType> enrich_hl7v2(const StreamType& payload,
                                       const ObservationData& enrichment_obs);
```

1. **Creates message** — `hl7v2::OruR01Message` with the new observation
2. **Concatenates** — `enriched_stream = payload + message.dump()`
3. **Records metrics**

**Key property**: HL7v2 is append-only. No parsing or re-serialization needed. This is the cheapest enrich operation.

#### Google FHIR — `enrich_google_fhir`

**Stream type**: `std::string`

```cpp
EnrichResult<StreamType> enrich_google_fhir(const StreamType& payload,
                                             const ObservationData& enrichment_obs);
```

1. **Parses TLV records** — Iterates the custom envelope format, deserializes each protobuf
2. **Preserves record order** — Tracks `record_order` vector to maintain original interleaving
3. **Constructs new Observation** — Via `assign::GoogleObservationTarget`
4. **Appends to the list** — Adds to observations vector and record_order
5. **Re-serializes all records** — Every patient and observation must be re-serialized to protobuf + TLV
6. **Records metrics**

**Key cost**: Full deserialization + re-serialization of every record.

## Comparison Table

| Arm | Enrich Cost Model | Re-parses existing data? | Re-serializes existing data? |
|---|---|---|---|
| **FastFHIR** | Append to arena + re-finalize | No | No (only new observation) |
| **JSON** | Full parse + full dump | Yes (nlohmann) | Yes (nlohmann) |
| **HL7v2** | String concatenation | No | No |
| **Google FHIR** | Full deserialize + full re-serialize | Yes (protobuf) | Yes (protobuf) |

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Appending, not modifying | Simplest possible mutation — reveals each format's append-cost baseline |
| Single observation enrich | Represents a common atomic EHR operation (one lab result arrives) |
| Byte-count tracking | Source vs enriched size ratio reveals wire-format overhead of modification |

## Dependencies

- `harness.hpp` — Types, `EnrichMetricsSummary`, `EnrichResult`
- `bench_test_1.hpp` — `assign::assign_observation()` for the enrichment observation
- FastFHIR: `FF_Bundle.hpp` (for FFHR arm)
- `nlohmann/json.hpp` (for JSON arm)
- `hl7v2_message.hpp` (for HL7v2 arm)
- Google FHIR protobuf headers (for Google FHIR arm)
