#!/usr/bin/env python3
"""The publish gate for an A/B run, and its release notes (TASKS.md PB-2c).

    scripts/publish_check.py RESULTS_DIR [--notes FILE] [--github-output FILE]

RESULTS_DIR is a scripts/bench_ab.py output directory. The run is publishable
only if EVERY check below holds; each failure is printed with its reason.

  - ab.json is complete, the run was not --quick, and cross-arm parity held
    on every invocation (the harness's exit 2 is a lossy arm, not noise)
  - both sides wrote provenance.json -- the harness refuses to write it when
    any required field is missing or the build is not -c opt, so its presence
    is the completeness gate; opt is re-checked here anyway
  - neither FastFHIR tree was dirty, and the benchmark checkout was clean
  - each provenance names the same commit ab.json says that side built

Exit 0 when publishable, 1 otherwise. --github-output appends
`publishable=true|false` plus the release naming fields for the workflow.
The notes are written either way, so a refused run still says why.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path


def load_json(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def check(results: Path) -> tuple[list[str], dict, dict[str, dict]]:
    failures: list[str] = []
    ab = load_json(results / "ab.json") or {}
    prov: dict[str, dict] = {}

    if not ab:
        return [f"{results / 'ab.json'} missing or unreadable"], ab, prov
    if "finished_at" not in ab:
        failures.append("ab.json has no finished_at -- the A/B run did not complete")
    if ab.get("quick"):
        failures.append("quick run -- reduced ladder, not an artifact")
    if not ab.get("parity_ok", False):
        n = len(ab.get("parity_failures", []))
        failures.append(f"cross-arm parity failed on {n} invocation(s) (harness exit 2)")

    for side in ("candidate", "baseline"):
        p = load_json(results / side / "provenance.json")
        if p is None:
            failures.append(f"{side}: provenance.json missing -- the harness refused it (exit 3)")
            continue
        prov[side] = p
        if p.get("compilation_mode") != "opt":
            failures.append(f"{side}: compilation_mode is {p.get('compilation_mode')!r}, not 'opt'")
        if p.get("fastfhir_dirty") is not False:
            failures.append(f"{side}: FastFHIR tree was dirty")
        if p.get("benchmark_dirty") is not False:
            failures.append(f"{side}: benchmark checkout was dirty ({p.get('benchmark_sha', '?')[:12]})")
        want = ab.get("sides", {}).get(side, {}).get("sha")
        if want and p.get("fastfhir_sha") != want:
            failures.append(f"{side}: provenance names {p.get('fastfhir_sha')}, ab.json built {want}")
    return failures, ab, prov


def fmt_ns(ns: float | None) -> str:
    if ns is None:
        return "–"
    for unit, scale in (("s", 1e9), ("ms", 1e6), ("µs", 1e3)):
        if ns >= scale:
            return f"{ns / scale:.2f} {unit}"
    return f"{ns:.0f} ns"


def notes(results: Path, failures: list[str], ab: dict, prov: dict[str, dict]) -> str:
    cand, base = ab.get("sides", {}).get("candidate", {}), ab.get("sides", {}).get("baseline", {})
    p = prov.get("candidate", {})
    out = [
        f"## FastFHIR {cand.get('describe', '?')} vs {base.get('describe', '?')}",
        "",
        "**Publishable.**" if not failures else "**Not publishable:**",
        *[f"- {f}" for f in failures],
        "",
        "| | |",
        "|---|---|",
        f"| candidate | `{cand.get('sha', '?')}` |",
        f"| baseline | `{base.get('sha', '?')}` |",
        f"| profile | `{cand.get('profile', '?')}` |",
        f"| host | {p.get('host_instance_type') or 'local'} · {p.get('cpu_model', '?')} · "
        f"{p.get('logical_cpus', '?')} cpus · {p.get('os', '?')}/{p.get('arch', '?')} "
        f"{p.get('kernel', '')} |",
        f"| tuning | governor `{p.get('cpu_governor') or 'n/a'}` · SMT `{p.get('smt_control') or 'n/a'}` · "
        f"turbo `{p.get('turbo') or 'n/a'}` |",
        f"| build | {p.get('compiler', '?')} {p.get('compiler_version', '')} `-c "
        f"{p.get('compilation_mode', '?')}` |",
        f"| corpus | {p.get('corpus_doc_count', '?')} docs, sha256 `{p.get('corpus_sha256', '?')[:16]}…` |",
        "| ladder | {} MB · {} replicates × {} runs · {} warmup · seed {}{} |".format(
            ",".join(str(t) for t in ab.get("ladder", {}).get("targets_mb", [])),
            *(ab.get("ladder", {}).get(k, "?") for k in ("replicates", "runs", "warmup_iterations", "seed")),
            "".join(f" · `{h}`" for h in ab.get("ladder", {}).get("harness_args", [])),
        ),
        f"| order | {ab.get('order', '?')} |",
        f"| benchmark | `{p.get('benchmark_sha', '?')}` |",
        "",
    ]
    if ab.get("a_a"):
        out += ["> **A/A run** — both sides compiled to the same binary. The ratios below "
                "measure this machine's noise, not a change in FastFHIR.", ""]

    summary_path = results / "ab_summary.csv"
    if summary_path.exists():
        rows = [r for r in csv.DictReader(summary_path.open()) if r["arm"] == "fastfhir"]
        if rows:
            largest = max(int(r["target_mb"]) for r in rows)
            out += [
                f"### FastFHIR arm, {largest} MB bundle",
                "",
                "Paired ratio = candidate ÷ baseline per replicate (same bundle), median "
                "[min, max]. Below 1 is faster.",
                "",
                "| stage | candidate | baseline | paired ratio |",
                "|---|---:|---:|---:|",
            ]
            for r in rows:
                if int(r["target_mb"]) != largest or not r["paired_median_ratio"]:
                    continue
                out.append(
                    f"| {r['test']} | {fmt_ns(float(r['median_ns_candidate']))} | "
                    f"{fmt_ns(float(r['median_ns_baseline']))} | "
                    f"{float(r['paired_median_ratio']):.3f} "
                    f"[{float(r['paired_min_ratio']):.3f}, {float(r['paired_max_ratio']):.3f}] |"
                )
            out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", type=Path)
    ap.add_argument("--notes", type=Path)
    ap.add_argument("--github-output", type=Path)
    args = ap.parse_args()

    failures, ab, prov = check(args.results)
    if args.notes:
        args.notes.write_text(notes(args.results, failures, ab, prov))

    for f in failures:
        print(f"publish_check: FAIL {f}", file=sys.stderr)
    if not failures:
        print("publish_check: publishable", file=sys.stderr)

    if args.github_output:
        p = prov.get("candidate", {})
        cand = ab.get("sides", {}).get("candidate", {})
        with args.github_output.open("a") as f:
            f.write(f"publishable={'false' if failures else 'true'}\n")
            f.write(f"fastfhir_describe={cand.get('describe', '')}\n")
            f.write(f"fastfhir_sha={cand.get('sha', '')}\n")
            f.write(f"platform={p.get('os', 'unknown')}-{p.get('arch', 'unknown')}\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
