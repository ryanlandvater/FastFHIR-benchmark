# `timing_conformance_test.cpp` — Timing Conformance Test

> ✅ **Ported 2026-08-25 — `bazel test -c opt //bench:timing_conformance_test`
> passes.** Uses `bench::make_builder()` / `bench::seal_stream()` and
> `FF_AdministrativeGender::Male`. Seals with `FF_CHECKSUM_NONE` to preserve the
> pre-port stream size.

## Purpose

A lightweight, standalone **validation binary** that verifies the benchmark harness produces correct, consistent timing measurements. It runs both the FastFHIR and JSON arms on a single synthetic patient and checks:

1. All metrics have positive duration values
2. Both arms report exactly `patients=1`
3. Both arms report matching birthdate values

This serves as a build-time sanity check and a regression guard.

## How It Works

### Test Fixture Construction

1. Allocates a 4096-byte `FastFHIR::Memory` arena
2. Creates a `PatientData` with known values: `id="patient-conformance"`, `birthdate="1990-03-21"`, `gender=Male`, `active=1`
3. Serializes it to FFHR binary via `builder.append_obj(patient)`, `builder.set_root()`, `builder.finalize()`
4. Records the FFHR byte count for informational purposes
5. Populates a `BundlePatient` (just the patient, no observations/encounters)
6. Wraps in a `BundleBenchFixture` with one entry

### Arm Execution

```cpp
const auto fastfhir = bench::run_fastfhir_bundle(fixture);
const auto json = bench::run_json_bundle(fixture);
```

Both arms receive the identical fixture.

### Validation Checks

1. **Metric count**: Each arm must produce at least 3 metrics (Serialize, Materialize, Query)
2. **Duration validity** (`metrics_are_valid`): Every metric must have `duration_ns > 0`. The flag `allow_test2_zero` exists for Materialize stubs (where a zero means "not implemented")
3. **Patient count**: Both arms must report `patients=1` in their `queried_value`
4. **Birthdate parity**: The birthdate from both arms must match after digit-only normalization (handles FFHR `1990-03-21` vs JSON `1990-03-21` formats — both normalize to `19900321`)

### Output

- **Pass**: Prints `"timing conformance passed"` and exits with code 0
- **Fail**: Prints details of which check failed and exits with code 1

### Integration

Defined as both a CTest test and a `bazel test` target:

```bash
# CTest
cd build && ctest -R timing_conformance

# Bazel
bazel test //bench:timing_conformance_test
```

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Minimal fixture (1 patient, no observations) | Keeps the test fast (~microseconds) for rapid iteration during development |
| Digit-only birthdate normalization | Accounts for format differences (FFHR stores `1990-03-21`, JSON dumps ISO string) |
| `metrics_are_valid` checks all stages | Catches regressions where a stage silently returns 0 |

## Dependencies

- `harness.hpp` — `ArmRunResult`, `Stage`, `BundlePatient`, `BundleBenchFixture`
- `run_fastfhir_bundle`, `run_json_bundle` — The two primary arms
- FastFHIR: `FF_Patient.hpp`, `FastFHIR.hpp`

## Build Targets

```python
# Bazel
cc_test(name = "timing_conformance_test", ...)

# CMake/CTest
add_test(NAME timing_conformance COMMAND bench_timing_conformance)
```
