# `synthea_fixture.cpp` — Synthea JSON Ingestion Pipeline

> ✅ **Ported 2026-08-25 — builds and runs.** The ingest path now uses
> `FF_SOURCE_FHIR_JSON`, pins `extension_filter` to `FILTER_ALL_KNOWN`, and
> passes `payload_capacity` so simdjson parses the document in place instead of
> making a padded copy. Sealing goes through `make_builder()` / `seal_stream()`
> in `harness.hpp` (`FF_BuilderSetRoot` + `FF_BuilderFinalize`).
>
> Hydration also **sanitises cross-arena `choice[x]` values** here — see
> [notes.md](../notes.md) §4. Without it the FastFHIR arm wrote a foreign arena
> offset into a block-tagged slot and corrupted its own stream.

## Purpose

Loads real-world Synthea-generated FHIR JSON patient files from disk and converts them into the benchmark's in-memory `BundlePatient` representation. This is the **data ingestion pipeline** that feeds all four benchmark arms.

## How It Works

### `make_bundle_patient_from_json(fs::path)`

This function performs a complete FHIR JSON → POCO transformation in one shot:

1. **File Loading**: Uses `simdjson::padded_string::load()` to memory-map the JSON file with simdjson's required padding. This is the fastest way to load JSON for parsing.

2. **Arena Sizing**: Computes `arena_size = max(4096, file_size * 2)`. The `2x` multiplier ensures the FastFHIR arena has room for the ingested resource tree plus metadata overhead. This matches the FastFHIR `ff_ingest` toolchain convention.

3. **FastFHIR Ingestion**: Creates a `FastFHIR::Memory` arena, a `FastFHIR::Builder`, and invokes `FastFHIR::Ingest::Ingestor`:
   ```cpp
   IngestRequest request{
       .builder = builder,
       .source_type = SourceType::FHIR_JSON,
       .json_string = ingest_payload,
   };
   result = ingestor.ingest(request, root, parsed_count);
   ```
   The ingestor parses the Synthea JSON, creates FFHR binary objects for every resource, and links them into a tree under `root`.

4. **POCO Hydration**: Calls `detail::hydrate_bundle_resources(root_node, item)` to walk the FFHR tree and populate the `BundlePatient` struct fields (`patient`, `encounters`, `conditions`, `procedures`, `observations`).

5. **Validation**: Checks that `item.patient.id` is non-empty (guarantees at least one Patient resource was found).

6. **Finalization**: Calls `builder.set_root(root)` and `builder.finalize(FF_CHECKSUM_SHA256, ...)` to seal the arena. The checksum callback returns a dummy 32-byte vector (benchmark mode — no real hashing).

7. **Returns** the populated `BundlePatient`.

### `load_enrichment_observation_from_json(fs::path)`

Similar pattern but loads a **single Observation resource** (from `bench/enrich.json`) for use in Test 4 enrichment:

1. Loads and ingests the JSON (same simdjson + FastFHIR Ingestor pipeline)
2. Validates the root is an `Observation` resource
3. Returns an `EnrichmentObservationFixture{memory, observation}` containing both the FFHR arena and the POCO struct

### Error Handling

Every failure mode produces a descriptive `std::runtime_error`:
- `"Failed to load JSON payload"` — simdjson load failure
- `"Empty file"` — zero-byte JSON
- `"Ingestor exception"` — C++ exception during ingestion
- `"Ingestor failed"` — FF_Result failure with code and message
- `"Ingestor returned null root"` — no root object after ingestion
- `"No Patient resource"` — missing patient (for bundle ingestion)
- `"Expected Observation resource"` — wrong resource type (for enrichment fixture)

## Key Design Decisions

| Decision | Rationale |
|---|---|
| `simdjson::padded_string::load` | Required by simdjson's parser for SIMD-safe overreading |
| `max(4096, file_size * 2)` arena sizing | Proven by `ff_ingest` toolchain; avoids FFHR reallocation |
| Ingest via `FastFHIR::Ingest::Ingestor` | Reuses FastFHIR's production JSON→FFHR conversion rather than manual field mapping |
| POCO re-hydration after ingest | Provides the canonical C++ structs that all four arms serialize from — ensures identical starting data |
| No-op checksum | The checksum is a required finalize parameter; computing SHA256 would add non-benchmark overhead |

## Dependencies

- `harness.hpp` — `BundlePatient`, `EnrichmentObservationFixture`, `detail::hydrate_bundle_resources()`
- `simdjson.h` — Padded string loading and parsing
- FastFHIR: `FF_Ingestor.hpp` — `Ingestor`, `IngestRequest`, `SourceType`
- Standard: `<filesystem>`, `<simdjson.h>`, `<stdexcept>`

## Performance Characteristics

- **CPU-bound**: The ingestion is dominated by simdjson parsing (Stage 1 of FastFHIR's pipeline) and FFHR binary construction.
- **Not timed**: Ingestion happens before the benchmark loop. The measured stages start from the already-hydrated `BundlePatient` structs.
