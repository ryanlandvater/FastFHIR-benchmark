#!/usr/bin/env python3
"""Same-box A/B timing: a FastFHIR candidate against a baseline (TASKS.md PB-2b).

Why this exists
---------------
Two cloud instances of the same machine type differ by a few percent. Two
builds measured on ONE box, interleaved, do not -- drift in temperature,
frequency, or background load lands on both sides equally. So every published
release number is paired with the previous release measured in the same job.

How
---
The harness links exactly one FastFHIR, so the two sides are two binaries:

  1. Pin both refs (scripts/pin_fastfhir.sh) and build each with
     --override_module. The baseline is built FIRST, so bazel-bin/ is left
     holding the candidate for the stages that follow (artifacts, sweep).
  2. Copy each binary out and record its SHA-256 next to the tree it was
     compiled from. Bazel's external-repo link names only the LAST build, so
     the pairing is recorded here and handed to the harness as --fastfhir-root;
     the digest is re-checked before every invocation.
  3. For each size and replicate r, run one process per side with
     --replicate-first r --replicates 1. A replicate's bundle depends only on
     (seed, size, r), so both sides measure the identical bundle. Order is
     ABBA: even replicates run candidate first, odd ones baseline first, so
     neither side systematically inherits a warmer or cooler machine.

Outputs (OUT/):
  candidate/metrics.csv, provenance.json, run.log   -- same format as a normal
  baseline/ metrics.csv, provenance.json, run.log      run, so plot_benchmarks.py
                                                       reads either unchanged
  ab_summary.csv   per (target_mb, arm, test): medians and paired ratio
  ab.json          refs, SHAs, trees, binary digests, order, parity outcome

Exit status: 0 clean; 2 if any invocation reported a cross-arm parity
mismatch (the run completes, but cannot be published); 1 on any other failure.

A candidate that compiles to the same binary as the baseline -- the same ref,
or a commit that touched only tests -- is an A/A run. That is allowed and
useful: its ratios are the noise floor for every A/B ratio on that machine.
ab.json flags it (`a_a`), comparing binary digests rather than refs.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIDES = ("candidate", "baseline")


def log(msg: str) -> None:
    print(f"bench_ab: {msg}", file=sys.stderr, flush=True)


def die(msg: str) -> None:
    log(msg)
    sys.exit(1)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(tree: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(tree), *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def pin(ref: str, args: argparse.Namespace) -> Path:
    cmd = [str(ROOT / "scripts" / "pin_fastfhir.sh"), ref]
    if args.profile:
        cmd += ["--profile", args.profile]
    if args.repo:
        cmd += ["--repo", args.repo]
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, cwd=ROOT).stdout
    return Path(out.strip().splitlines()[-1])


def resolve_auto_baseline(candidate_tree: Path) -> str:
    """The previous release, or -- while FastFHIR has no tags -- the parent."""
    mirror = ROOT / ".external" / "pins" / "FastFHIR.git"
    sha = git(candidate_tree, "rev-parse", "HEAD")
    tag = subprocess.run(
        ["git", "-C", str(mirror), "describe", "--tags", "--abbrev=0", f"{sha}^"],
        capture_output=True, text=True,
    )
    if tag.returncode == 0 and tag.stdout.strip():
        return tag.stdout.strip()
    return git(Path(mirror), "rev-parse", f"{sha}^")


def build(tree: Path, bazel: str) -> Path:
    subprocess.run(
        [bazel, "build", "-c", "opt", f"--override_module=fastfhir={tree}",
         "//bench:bench_harness", "//bench:bench_test_5"],
        check=True, cwd=ROOT,
    )
    # Prove the build used the tree we asked for before trusting the pairing.
    external = ROOT / f"bazel-{ROOT.name}" / "external"
    for name in ("fastfhir~", "fastfhir+"):
        if (external / name).exists():
            built = (external / name).resolve()
            if built != tree.resolve():
                die(f"bazel built {built}, expected {tree}")
            break
    else:
        die(f"no fastfhir external repo under {external}")
    return ROOT / "bazel-bin" / "bench" / "bench_harness"


def median_or_none(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


def summarize(out: Path) -> list[dict]:
    """Medians per side, plus the PAIRED ratio: per replicate, candidate median
    over baseline median on the same bundle, then the median of those ratios.
    The paired figure is the one to quote; ratio-of-medians is shown beside it
    because a large gap between the two means the replicates disagree."""
    rows: dict[tuple, dict[str, dict[int, list[float]]]] = {}
    for side in SIDES:
        with (out / side / "metrics.csv").open() as f:
            for r in csv.DictReader(f):
                key = (int(r["target_mb"]), r["arm"], r["test"])
                per_rep = rows.setdefault(key, {s: {} for s in SIDES})[side]
                per_rep.setdefault(int(r["replicate"]), []).append(float(r["duration_ns"]))

    summary = []
    for (target_mb, arm, test), sides in sorted(rows.items()):
        cand = [x for xs in sides["candidate"].values() for x in xs]
        base = [x for xs in sides["baseline"].values() for x in xs]
        paired = [
            statistics.median(sides["candidate"][rep]) / statistics.median(sides["baseline"][rep])
            for rep in sorted(set(sides["candidate"]) & set(sides["baseline"]))
            if statistics.median(sides["baseline"][rep]) > 0
        ]
        mc, mb = median_or_none(cand), median_or_none(base)
        summary.append({
            "target_mb": target_mb,
            "arm": arm,
            "test": test,
            "n_candidate": len(cand),
            "n_baseline": len(base),
            "median_ns_candidate": mc,
            "median_ns_baseline": mb,
            "ratio_of_medians": (mc / mb) if mc is not None and mb else None,
            "paired_replicates": len(paired),
            "paired_median_ratio": median_or_none(paired),
            "paired_min_ratio": min(paired) if paired else None,
            "paired_max_ratio": max(paired) if paired else None,
        })
    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--candidate", required=True, help="FastFHIR ref under test")
    ap.add_argument("--baseline", required=True,
                    help="FastFHIR ref to compare against, or 'auto': the newest tag "
                         "before the candidate, else the candidate's parent")
    ap.add_argument("--repo", help="fetch refs from here (URL or path; default: GitHub)")
    ap.add_argument("--profile", help="FASTFHIR_PRODUCTION_PROFILE for BOTH sides "
                    "(default: each commit's own base preset)")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--targets-mb", default="1,2,4,8,16,32,64")
    ap.add_argument("--replicates", type=int, default=20)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--warmup-iterations", type=int, default=1)
    ap.add_argument("--seed", default="20260825")
    ap.add_argument("--quick", action="store_true",
                    help="1,2,4 MB x 3 replicates x 2 runs, no warmup. Not an artifact.")
    ap.add_argument("--harness-arg", action="append", default=[],
                    help="passed through to every harness invocation (repeatable)")
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse OUT/bin/* from an earlier invocation (digests are still checked)")
    ap.add_argument("--bazel", default=os.environ.get("BAZEL") or shutil.which("bazelisk") or "bazel")
    args = ap.parse_args()

    if args.quick:
        args.targets_mb, args.replicates, args.runs, args.warmup_iterations = "1,2,4", 3, 2, 0
    targets = [int(t) for t in args.targets_mb.split(",") if t.strip()]

    if not (ROOT / "datasets" / "synthea").is_dir():
        die("datasets/synthea is missing -- link the corpus first (README: Corpus location)")

    out: Path = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / "ab.json"

    # --- pin and build: baseline first, so bazel-bin/ ends on the candidate ---
    trees = {"candidate": pin(args.candidate, args)}
    if args.baseline == "auto":
        args.baseline = resolve_auto_baseline(trees["candidate"])
        log(f"baseline auto -> {args.baseline}")
    trees["baseline"] = pin(args.baseline, args)

    sides: dict[str, dict] = {}
    for side in reversed(SIDES):
        ref = getattr(args, side)
        tree = trees[side]
        dest = out / "bin" / side / "bench_harness"
        if args.skip_build:
            if not dest.exists():
                die(f"--skip-build but {dest} does not exist")
            prior = json.loads(manifest_path.read_text())["sides"][side]
            if Path(prior["tree"]) != tree:
                die(f"--skip-build: {dest} was built from {prior['tree']}, not {tree}")
        else:
            log(f"building {side} ({ref}) from {tree}")
            built = build(tree, args.bazel)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(built, dest)
        sides[side] = {
            "ref": ref,
            "sha": git(tree, "rev-parse", "HEAD"),
            "describe": git(tree, "describe", "--tags", "--always"),
            "tree": str(tree),
            "profile": (tree / "generated_src" / ".profile").read_text().strip(),
            "binary": str(dest),
            "binary_sha256": sha256_file(dest),
        }
        (out / side).mkdir(exist_ok=True)

    manifest = {
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "quick": args.quick,
        # Different commits can still compile to the same bytes (a tests-only
        # change, say). Then the run is an A/A whatever the refs say, and its
        # ratios measure noise, not the release -- so say so.
        "identical_binaries": sides["candidate"]["binary_sha256"] == sides["baseline"]["binary_sha256"],
        "a_a": sides["candidate"]["binary_sha256"] == sides["baseline"]["binary_sha256"],
        "order": "ABBA by replicate (even: candidate first; odd: baseline first)",
        "ladder": {"targets_mb": targets, "replicates": args.replicates, "runs": args.runs,
                   "warmup_iterations": args.warmup_iterations, "seed": args.seed,
                   "harness_args": args.harness_arg},
        "sides": sides,
        "parity_failures": [],
    }
    if sides["candidate"]["profile"] != sides["baseline"]["profile"]:
        log(f"NOTE profiles differ: candidate {sides['candidate']['profile']!r}, "
            f"baseline {sides['baseline']['profile']!r} -- the comparison includes that change")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    # --- interleaved timing ---------------------------------------------------
    csv_files = {s: (out / s / "metrics.csv").open("w") for s in SIDES}
    logs = {s: (out / s / "run.log").open("w") for s in SIDES}
    header_written = {s: False for s in SIDES}
    total = len(targets) * args.replicates * 2
    done = 0
    t0 = time.monotonic()
    try:
        for target in targets:
            for rep in range(args.replicates):
                order = SIDES if rep % 2 == 0 else tuple(reversed(SIDES))
                for side in order:
                    info = sides[side]
                    binary = Path(info["binary"])
                    if sha256_file(binary) != info["binary_sha256"]:
                        die(f"{binary} changed since it was built -- refusing to measure it")
                    cmd = [
                        str(binary),
                        "--bundle-targets-mb", str(target),
                        "--replicate-first", str(rep), "--replicates", "1",
                        "--runs", str(args.runs),
                        "--warmup-iterations", str(args.warmup_iterations),
                        "--seed", args.seed,
                        "--fastfhir-root", info["tree"],
                        "--results-dir", str(out / side),
                        *args.harness_arg,
                    ]
                    proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, text=True)
                    logs[side].write(f"### {target} MB replicate {rep} (rc={proc.returncode})\n")
                    logs[side].write(proc.stderr)
                    logs[side].flush()
                    if proc.returncode not in (0, 2):
                        die(f"{side} {target} MB r{rep} exited {proc.returncode} -- "
                            f"see {out / side / 'run.log'}")
                    if proc.returncode == 2:
                        manifest["parity_failures"].append(
                            {"side": side, "target_mb": target, "replicate": rep})
                    lines = proc.stdout.splitlines(keepends=True)
                    if not lines:
                        die(f"{side} {target} MB r{rep} produced no CSV")
                    if not header_written[side]:
                        csv_files[side].write(lines[0])
                        header_written[side] = True
                    csv_files[side].writelines(lines[1:])
                    csv_files[side].flush()
                    done += 1
                    eta = (time.monotonic() - t0) / done * (total - done)
                    log(f"[{done}/{total}] {target} MB r{rep} {side}"
                        f"{' PARITY-FAIL' if proc.returncode == 2 else ''} (eta {eta / 60:.1f} min)")
    finally:
        for f in (*csv_files.values(), *logs.values()):
            f.close()

    # --- summary --------------------------------------------------------------
    summary = summarize(out)
    with (out / "ab_summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
        w.writeheader()
        w.writerows(summary)

    manifest["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    manifest["parity_ok"] = not manifest["parity_failures"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    log(f"candidate {sides['candidate']['describe']} vs baseline {sides['baseline']['describe']}"
        f"{' -- IDENTICAL BINARIES: this is an A/A run; ratios below are noise' if manifest['a_a'] else ''}")
    for row in summary:
        if row["arm"] == "fastfhir" and row["paired_median_ratio"] is not None:
            log(f"  {row['target_mb']:>4} MB {row['test']:<24} paired ratio "
                f"{row['paired_median_ratio']:.3f} [{row['paired_min_ratio']:.3f}, "
                f"{row['paired_max_ratio']:.3f}]")
    if not manifest["parity_ok"]:
        log(f"{len(manifest['parity_failures'])} invocation(s) failed cross-arm parity -- not publishable")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
