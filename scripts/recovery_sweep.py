#!/usr/bin/env python3
"""Instrument G test 5 driver: corruption and recovery as INDEPENDENT processes.

For each format x k x replicate, `bench_test_5` is invoked as separate subprocesses --
neither the recoverer nor the checker can leak state to the other:

    --hash     clean wire  -> baseline fingerprint file  (the baseline producer)
    --corrupt  clean wire  -> corrupted artifact         (k structural flips)
    --recover  corrupted   -> recovered fingerprint      (corrupted bytes ONLY)
    --check    baseline + recovered fingerprints
               content verification per unit: identity (parent, offset, tag)
               AND a content hash of the unit's data bytes must match; the
               digest each recoverer stamped must match its own units.

For every replicate the sweep ALSO runs --hash on the DAMAGED wire (no recovery
step) and checks that against the baseline: `no_recovery_pct` is what a
consumer gets who simply reads the damaged bytes. recovered_pct - no_recovery_pct
is the arm's actual repair yield on identical damaged bytes -- the one
comparison with a shared denominator on both sides (handoff.md: only FFHR
should show a positive delta; HL7v2's "recovery" is a scan and shows ~0).

Emits results/recovery_curve.csv with columns:
    format,bits_corrupted,positions_total,replicate,recovered_pct,no_recovery_pct,
    baseline_count,recovered_count,correct,wrong,missing,spurious

`recovered_pct` is CORRECT/baseline -- the KNOWN DENOMINATOR: the baseline is
the clean stream's own census (captured before any corruption). The check has
FOUR outcomes because "found" and "intact" are different claims:
  correct   unit is there AND carries the data it carried before
  wrong     unit is there and the data CHANGED (the dangerous one -- it still
            parses and reads as valid data that is not what was written)
  missing   in the baseline, not in the recovery -- honest loss
  spurious  in the recovery, not in the baseline -- invention
Each arm hashes its unit's own data bytes (FFHR the referenced block, JSON the
entry span, protobuf the record payload, HL7v2 the segment body AFTER the
header), so a blasted interior pipe makes that segment WRONG, not recovered.

Artifacts come from the harness's --dump-artifacts mode (one representative
bundle's Test-1 wire payload per arm). Structural bits only (per-format syntax
regions). Qualifiers preserved: malformed-not-hostile; integrity-not-authenticity.

Run from the repo root after:
    bazel build -c opt //bench:bench_harness //bench:bench_test_5
    ./bazel-bin/bench/bench_harness --dump-artifacts artifacts
Then: .venv/bin/python scripts/recovery_sweep.py
"""

import pathlib
import re
import subprocess
import sys

DRIVER = pathlib.Path("bazel-bin/bench/bench_test_5")
ARTIFACTS = {
    "fastfhir": "artifacts/fastfhir.bin",
    "json": "artifacts/json.bin",
    "google_fhir": "artifacts/google_fhir.bin",
    "hl7v2": "artifacts/hl7v2.bin",
    # REC-6 CONTROL, not a competitor. Same resources and byte-identical leaves
    # as `json` (both fingerprint to 3dfb8b1f…), framed one record per line.
    # It varies framing and nothing else, which is what makes it a control.
    "ndjson": "artifacts/ndjson.bin",
}
KS = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]

# REPLICATE: one independent draw of the randomized input -- here a damage
# pattern at a given k, in bench 1-4 (bench/main.cpp) a bundle composition.
# Same word, same default, same statistic (median + IQR) in both, so a reader
# moving between the two benchmarks is reading the same axis.
#
# There is no `run` axis here: given a damage pattern the recovered fraction is
# deterministic, so repeating the measurement adds nothing. Bench 1-4 has both
# because timing has measurement noise on top of workload variance.
REPLICATES = 20


def run(args: list[str]) -> str:
    p = subprocess.run([str(DRIVER)] + args, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"bench_test_5 failed: {' '.join(args)}\n{p.stderr}{p.stdout}")
    return p.stdout.strip()


def replicate_step(fmt: str, k: int, t: int, step: str, crashes, args: list[str]) -> str | None:
    """Run one driver step; a crashed replicate (signal/nonzero) is recorded and
    scored, not dropped. Recovery of a PATHOLOGICAL repaired stream can still
    segfault the driver (pre-existing upstream parser crash, reproduced on
    fastfhir k=512 t=12): one bad sample must not abort the whole sweep -- the
    curve is a median over REPLICATES, and the crash is logged loudly instead
    of hidden."""
    p = subprocess.run([str(DRIVER)] + args, capture_output=True, text=True)
    if p.returncode == 0:
        return p.stdout.strip()
    crashes.write(f"{fmt},{k},{t},{step},{p.returncode}\n")
    crashes.flush()
    print(f"  CRASH {fmt} k={k} t={t} step={step} rc={p.returncode} -- "
          f"replicate scored 0, sweep continues", file=sys.stderr)
    return None


def field(line: str, key: str) -> str:
    """Pull key=value out of a driver summary line (e.g. 'pct=99.8')."""
    m = re.search(rf"\b{key}=([0-9.]+)", line)
    if not m:
        sys.exit(f"cannot parse '{key}' from driver output: {line!r}")
    return m.group(1)


def main() -> int:
    for artifact in ARTIFACTS.values():
        if not pathlib.Path(artifact).is_file():
            sys.exit(f"missing artifact {artifact} -- run the harness with "
                     "--dump-artifacts artifacts first")
    out = pathlib.Path("results")
    out.mkdir(exist_ok=True)
    csv = out / "recovery_curve.csv"
    crashes_file = out / "_crashes.csv"
    with open(csv, "w") as f, open(crashes_file, "w") as crashes:
        crashes.write("format,bits_corrupted,replicate,step,returncode\n")
        f.write("format,bits_corrupted,positions_total,replicate,recovered_pct,"
                "no_recovery_pct,"
                "baseline_count,recovered_count,correct,wrong,missing,spurious,"
                "recover_ns,norec_ns\n")
        censuses: dict[str, int] = {}
        crashed_replicates = 0
        for fmt, artifact in ARTIFACTS.items():
            baseline = out / f"_baseline_{fmt}.fp"
            units = int(field(
                run(["--hash", fmt, "--in", artifact, "--out", str(baseline)]), "units"))
            censuses[fmt] = units
            positions = int(field(run(["--positions", fmt, "--in", artifact]), "positions"))
            print(f"{fmt}: units={units} positions={positions}", file=sys.stderr)
            for k in KS:
                for t in range(REPLICATES):
                    seed = 20260826 + t * 7 + k
                    damaged = out / f"_corrupt_{fmt}_{k}_{t}.bin"
                    recovered = out / f"_recovered_{fmt}_{k}_{t}.fp"
                    no_recovery = out / f"_norec_{fmt}_{k}_{t}.fp"
                    # A CRASHED STEP SCORES 0%, IT IS NOT DROPPED.
                    #
                    # Skipping the replicate removed exactly the hardest samples
                    # from the mean, so the curve reported the average of the
                    # runs that survived -- higher than the truth by
                    # construction, and with the failure itself invisible. A
                    # reader that cannot read is a reader that recovered
                    # nothing, which is what 0% means.
                    #
                    # The two readings fail independently: a recover that
                    # crashes does not stop the no-recovery reading of the same
                    # damaged bytes, so one column going to 0 never silently
                    # takes the other with it.
                    base_n = str(censuses[fmt])
                    pct, norec_pct = "0.0", "0.0"
                    rec_n = correct = wrong = spurious = "0"
                    missing = base_n
                    # REC-7: what recovery COSTS. A crashed step leaves 0, which
                    # reads as "no time spent" -- correct, since nothing was
                    # recovered either.
                    recover_ns = norec_ns = "0"

                    if replicate_step(fmt, k, t, "corrupt", crashes,
                                  ["--corrupt", fmt, "--bits", str(k), "--seed", str(seed),
                                   "--in", artifact, "--out", str(damaged)]) is None:
                        crashed_replicates += 1
                    else:
                        # Recovery ON.
                        rec_line = replicate_step(fmt, k, t, "recover", crashes,
                                      ["--recover", fmt, "--in", str(damaged),
                                       "--out", str(recovered)])
                        if rec_line is not None:
                            recover_ns = field(rec_line, "recover_ns")
                            line = replicate_step(fmt, k, t, "check", crashes,
                                              ["--check", "--baseline", str(baseline),
                                               "--recovered", str(recovered)])
                            if line is not None:
                                pct = field(line, "pct")
                                base_n = field(line, "baseline")
                                rec_n = field(line, "recovered")
                                correct = field(line, "correct")
                                wrong = field(line, "wrong")
                                missing = field(line, "missing")
                                spurious = field(line, "spurious")
                            else:
                                crashed_replicates += 1
                        else:
                            crashed_replicates += 1

                        # Recovery OFF, on the SAME damaged bytes -- the sound
                        # comparison, shared denominator both sides.
                        nr_line = replicate_step(fmt, k, t, "hash_norec", crashes,
                                      ["--hash", fmt, "--in", str(damaged),
                                       "--out", str(no_recovery)])
                        if nr_line is not None:
                            norec_ns = field(nr_line, "read_ns")
                            line_nr = replicate_step(fmt, k, t, "check_norec", crashes,
                                                 ["--check", "--baseline", str(baseline),
                                                  "--recovered", str(no_recovery)])
                            if line_nr is not None:
                                norec_pct = field(line_nr, "pct")
                            else:
                                crashed_replicates += 1
                        else:
                            crashed_replicates += 1

                    f.write(f"{fmt},{k},{positions},{t},{pct},{norec_pct},{base_n},{rec_n},"
                            f"{correct},{wrong},{missing},{spurious},"
                            f"{recover_ns},{norec_ns}\n")
                    f.flush()  # live rows: a crash must not swallow a day of replicates
                    damaged.unlink(missing_ok=True)
                    recovered.unlink(missing_ok=True)
                    no_recovery.unlink(missing_ok=True)
            baseline.unlink(missing_ok=True)
            print(f"  {fmt}: done", file=sys.stderr)

    # UNIT-CENSUS PARITY.
    #
    # recovered_pct is correct/baseline, so the baseline is the denominator of
    # every number this sweep produces. Comparing arms whose baselines differ
    # compares percentages of different populations: an arm that enumerates
    # fewer units gets an easier denominator and reports a higher score for
    # recovering less. The harness gates the same quantity at Test 1
    # (test_1 ELEMENTS); this is that gate for the recovery curve.
    print("\n[parity] baseline unit census", file=sys.stderr)
    for fmt, n in censuses.items():
        print(f"    {fmt:<12} {n:>10}", file=sys.stderr)
    distinct = set(censuses.values())
    if len(distinct) > 1:
        ref = censuses.get("fastfhir")
        detail = "  ".join(f"{f}={n}" for f, n in censuses.items())
        sys.exit(
            "[validate] baseline unit census differs across arms: " + detail +
            "\nrecovered_pct is correct/baseline, so these percentages are not "
            "comparable. Fix the enumeration before trusting the curve."
            + ("" if ref is None else f" (fastfhir={ref})"))

    if crashed_replicates:
        print(f"[warn] {crashed_replicates} replicate(s) crashed the driver (segfault on a "
              f"pathological repaired stream) and were SCORED 0% -- see "
              f"{crashes_file}", file=sys.stderr)
    print(f"wrote {csv}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
