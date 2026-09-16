#!/usr/bin/env bash
#
# The whole benchmark, in the one order that works.
#
# Five stages, and the order between them is not a preference:
#
#   1. build      bazel, -c opt (.bazelrc pins --compilation_mode=opt; the flag
#                 is repeated here so the Debug trap is visible in the command)
#   2. timing     bench_harness --results-dir -> metrics.csv + provenance.json.
#                 The provenance gate runs BEFORE the first measurement and
#                 exits 3, so a run that cannot become an artifact fails in
#                 seconds instead of at the end of the ladder.
#   3. artifacts  bench_harness --dump-artifacts -> artifacts/*.bin. This is an
#                 EXCLUSIVE mode of the same binary (main.cpp returns before the
#                 timing study), which is why it is a second invocation and not
#                 a flag on the first.
#   4. sweep      recovery_sweep.py reads artifacts/ -> results/recovery_curve.csv
#   5. figures    plot_benchmarks.py reads metrics.csv + provenance.json + that
#                 curve. It must run from the repo root: fig8 opens
#                 results/recovery_curve.csv by a hard-coded relative path.
#
# Stage 4 depends on stage 3 and stage 5 on all of them, so nothing here is
# reorderable. Each stage is individually skippable for re-running one piece.
#
#   scripts/run_benchmark.sh                 # full
#   scripts/run_benchmark.sh --quick         # reduced ladder, no sweep
#   scripts/run_benchmark.sh --skip-build --skip-timing --skip-artifacts
#
# Numbers become an artifact only if provenance.json is complete; see README
# "Result provenance and the artifact gate".

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# --- knobs (env overrides; the CLI flags below win over these) --------------
RESULTS_DIR="${RESULTS_DIR:-results/dev}"
FIGURES_DIR="${FIGURES_DIR:-}"           # default: $RESULTS_DIR/figures
ARTIFACTS_DIR="${ARTIFACTS_DIR:-artifacts}"
RUNS="${RUNS:-3}"            # timed repetitions per replicate
REPLICATES="${REPLICATES:-20}"  # distinct bundle draws per size (matches
                                # recovery_sweep.py REPLICATES)
WARMUP_ITERATIONS="${WARMUP_ITERATIONS:-1}"
BUNDLE_TARGETS_MB="${BUNDLE_TARGETS_MB:-1,2,4,8,16,32,64}"
BUNDLE_MAX_MB="${BUNDLE_MAX_MB:-64}"
SEED="${SEED:-20260825}"
PROFILE="${PROFILE:-}"                   # pin FASTFHIR_PRODUCTION_PROFILE
FASTFHIR_REF="${FASTFHIR_REF:-}"         # benchmark a pinned commit, not the live tree
FASTFHIR_REPO="${FASTFHIR_REPO:-}"       # where FASTFHIR_REF is fetched from
FIGURE_FORMAT="${FIGURE_FORMAT:-png}"
# Passed through verbatim to recovery_sweep.py. Empty today -- the sweep takes
# no arguments (see --quick below); this is the seam for when it does.
RECOVERY_SWEEP_ARGS="${RECOVERY_SWEEP_ARGS:-}"

QUICK=0
WITH_TESTS=0
DO_BUILD=1
DO_TIMING=1
DO_ARTIFACTS=1
DO_SWEEP=1
DO_FIGURES=1
SWEEP_FORCED=0
# An explicit --runs/--targets-mb/--max-mb outranks the --quick defaults
# whatever order they arrive in; without this, `--quick --runs 3` silently
# ran 2.
RUNS_SET=0
TARGETS_SET=0
MAX_MB_SET=0

usage() {
  cat <<'EOF'
Usage: scripts/run_benchmark.sh [options]

  --quick             Reduced run: 2 runs, 1,2,4 MB ladder, no warmup, and the
                      recovery sweep is skipped (it has no reduced mode -- see
                      --sweep). Everything else runs, so the pipeline is
                      exercised end to end in minutes rather than hours.
  --full              Explicit opposite of --quick (the default).
  --sweep             Run the recovery sweep even under --quick. It is the full
                      sweep: there is no short one.
  --with-tests        Also run the bazel gates (timing_conformance_test,
                      provenance_test) after the build.

  --results-dir DIR   metrics.csv + provenance.json + run.log  (default results/dev)
  --figures-dir DIR   rendered figures                         (default <results-dir>/figures)
  --artifacts-dir DIR per-arm wire artifacts                   (default artifacts)
  --profile STR       Pin FASTFHIR_PRODUCTION_PROFILE. Needed only when the
                      generated-tree stamp cannot resolve it and the harness
                      calls the profile ambiguous (it then exits 3).
  --fastfhir-ref REF  Benchmark FastFHIR at REF (tag/branch/SHA) from a clean
                      pinned checkout (scripts/pin_fastfhir.sh) instead of the
                      live .external/FastFHIR tree. Required for anything
                      published. --profile then selects the generated profile;
                      without it the pinned commit's base preset decides.
  --fastfhir-repo R   Fetch --fastfhir-ref from R (URL or local path; default
                      the GitHub repo). A local path reaches unpushed commits.
  --replicates N      Distinct bundle draws per size           (default 20)
  --runs N            Timed repetitions per replicate          (default 3)
  --targets-mb LIST   Sweep ladder, comma separated            (default 1,2,4,8,16,32,64)
  --max-mb N          Cap on the largest target                (default 64)
  --seed N            Bundle composition seed                  (default 20260825)
  --serial-build      Force every arm's Test 1 loop onto one thread. Default is
                      parallel: fastfhir and json_fhir both dispatch across
                      cores (hl7v2 and google_fhir are serial either way). Run
                      the ladder both ways to separate encoder cost from core
                      scaling; RESULTS_DIR must differ between the two.

  --skip-build        Reuse ./bazel-bin/bench/* as they are
  --skip-timing       No timing study (keeps any existing metrics.csv)
  --skip-artifacts    No artifact dump (keeps artifacts/*.bin)
  --skip-sweep        No recovery sweep (keeps results/recovery_curve.csv)
  --skip-figures      No figures
  -h, --help          This text

Environment: RESULTS_DIR FIGURES_DIR ARTIFACTS_DIR RUNS REPLICATES
WARMUP_ITERATIONS BUNDLE_TARGETS_MB BUNDLE_MAX_MB SEED PROFILE FIGURE_FORMAT
FASTFHIR_REF FASTFHIR_REPO RECOVERY_SWEEP_ARGS BAZEL PYTHON
BENCH_HOST_INSTANCE_TYPE BENCH_HOST_IMAGE BENCH_HOST_RUNNER (recorded in provenance)
EOF
}

die() {
  echo "run_benchmark: $*" >&2
  exit 1
}

stage() {
  echo
  echo "=== $* ==="
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick)          QUICK=1 ;;
    --full)           QUICK=0 ;;
    --sweep)          DO_SWEEP=1; SWEEP_FORCED=1 ;;
    --with-tests)     WITH_TESTS=1 ;;
    --results-dir)    RESULTS_DIR="${2:?--results-dir needs a directory}"; shift ;;
    --figures-dir)    FIGURES_DIR="${2:?--figures-dir needs a directory}"; shift ;;
    --artifacts-dir)  ARTIFACTS_DIR="${2:?--artifacts-dir needs a directory}"; shift ;;
    --profile)        PROFILE="${2:?--profile needs a value}"; shift ;;
    --fastfhir-ref)   FASTFHIR_REF="${2:?--fastfhir-ref needs a ref}"; shift ;;
    --fastfhir-repo)  FASTFHIR_REPO="${2:?--fastfhir-repo needs a URL or path}"; shift ;;
    --runs)           RUNS="${2:?--runs needs a number}"; RUNS_SET=1; shift ;;
    --replicates)     REPLICATES="${2:?--replicates needs a number}"; shift ;;
    --targets-mb)     BUNDLE_TARGETS_MB="${2:?--targets-mb needs a list}"; TARGETS_SET=1; shift ;;
    --max-mb)         BUNDLE_MAX_MB="${2:?--max-mb needs a number}"; MAX_MB_SET=1; shift ;;
    --seed)           SEED="${2:?--seed needs a number}"; shift ;;
    --serial-build)   SERIAL_BUILD=1 ;;
    --skip-build)     DO_BUILD=0 ;;
    --skip-timing)    DO_TIMING=0 ;;
    --skip-artifacts) DO_ARTIFACTS=0 ;;
    --skip-sweep)     DO_SWEEP=0 ;;
    --skip-figures)   DO_FIGURES=0 ;;
    -h|--help)        usage; exit 0 ;;
    *)                usage >&2; die "unknown argument: $1" ;;
  esac
  shift
done

if [[ "$QUICK" -eq 1 ]]; then
  [[ "$RUNS_SET" -eq 1 ]]    || RUNS=2
  REPLICATES=3
  [[ "$TARGETS_SET" -eq 1 ]] || BUNDLE_TARGETS_MB=1,2,4
  [[ "$MAX_MB_SET" -eq 1 ]]  || BUNDLE_MAX_MB=4
  WARMUP_ITERATIONS=0
  # recovery_sweep.py has no reduced mode: its ladder (13 k values) and
  # replicate count (20) are module constants, so "quick" cannot mean "fewer
  # replicates"
  # without editing it. A shortened sweep would also be a DIFFERENT curve, not
  # a cheaper one -- the published number is a median over those trials. So
  # quick skips it outright and says so, rather than quietly producing a curve
  # that looks like the real one.
  if [[ "$SWEEP_FORCED" -eq 0 ]]; then
    DO_SWEEP=0
  fi
fi

FIGURES_DIR="${FIGURES_DIR:-$RESULTS_DIR/figures}"

BAZEL="${BAZEL:-}"
if [[ -z "$BAZEL" ]]; then
  # .bazelversion pins 7.7.1, so bazelisk is preferred where it exists --
  # generate_repo.sh makes the same choice.
  if command -v bazelisk >/dev/null 2>&1; then BAZEL=bazelisk
  elif command -v bazel >/dev/null 2>&1; then BAZEL=bazel
  else die "no bazelisk or bazel on PATH"; fi
fi

PYTHON="${PYTHON:-}"
if [[ -z "$PYTHON" ]]; then
  if [[ -x ".venv/bin/python" ]]; then PYTHON=".venv/bin/python"
  elif command -v python3 >/dev/null 2>&1; then PYTHON=python3
  else die "no .venv/bin/python and no python3 on PATH"; fi
fi

BENCH_BIN="./bazel-bin/bench/bench_harness"
SWEEP_DRIVER="./bazel-bin/bench/bench_test_5"
METRICS_CSV="$RESULTS_DIR/metrics.csv"
RUN_LOG="$RESULTS_DIR/run.log"

# --- preflight -------------------------------------------------------------
# The corpus is a symlink on a working machine and its absence is the single
# most common failure; the harness's own message does not say how to fix it.
if [[ ! -d "datasets/synthea" ]] || [[ -z "$(ls -A datasets/synthea 2>/dev/null)" ]]; then
  die "datasets/synthea is missing or empty -- run ./generate_repo.sh first (README: Corpus location)"
fi

# Up front, not inside stage 2: stages 4 and 5 also write here (sweep.log,
# figures), so creating it only in the stage that happens to run first means
# `--skip-timing` makes a later `tee` fail on a missing directory -- and a tee
# that dies takes its producer down with a SIGPIPE mid-sweep.
mkdir -p "$RESULTS_DIR"

# --- 0. pin ------------------------------------------------------------------
# With a ref, the library under test is a clean checkout at that commit and
# Bazel is pointed at it with --override_module. The profile then goes to the
# generator, not to the harness: the harness reads the pinned tree's
# generated_src/.profile stamp, which is evidence rather than an assertion.
BAZEL_FLAGS=()
FASTFHIR_PIN=""
if [[ -n "$FASTFHIR_REF" ]]; then
  stage "0/5 pin FastFHIR @ $FASTFHIR_REF"
  pin_args=("$FASTFHIR_REF")
  [[ -n "$PROFILE" ]] && pin_args+=(--profile "$PROFILE")
  [[ -n "$FASTFHIR_REPO" ]] && pin_args+=(--repo "$FASTFHIR_REPO")
  FASTFHIR_PIN="$(PYTHON="$PYTHON" scripts/pin_fastfhir.sh "${pin_args[@]}" | tail -n 1)" \
    || die "could not pin FastFHIR at $FASTFHIR_REF"
  BAZEL_FLAGS+=("--override_module=fastfhir=$FASTFHIR_PIN")
  PROFILE=""
  if [[ "$DO_BUILD" -eq 0 ]]; then
    echo "  WARNING --skip-build: the binaries were built from whatever tree the last build used;"
    echo "          provenance reports that tree, which may not be this pin."
  fi
fi

stage "plan"
echo "  mode          $([[ "$QUICK" -eq 1 ]] && echo quick || echo full)"
echo "  bazel         $BAZEL"
echo "  python        $PYTHON"
echo "  fastfhir      ${FASTFHIR_PIN:-live tree (.external/FastFHIR) -- not publishable}"
echo "  results       $RESULTS_DIR"
echo "  figures       $FIGURES_DIR"
echo "  artifacts     $ARTIFACTS_DIR"
echo "  ladder        ${BUNDLE_TARGETS_MB} MB (max ${BUNDLE_MAX_MB}), replicates=${REPLICATES}, runs=${RUNS}, warmup=${WARMUP_ITERATIONS}, seed=${SEED}"
echo "  stages        build=${DO_BUILD} timing=${DO_TIMING} artifacts=${DO_ARTIFACTS} sweep=${DO_SWEEP} figures=${DO_FIGURES} tests=${WITH_TESTS}"
if [[ "$QUICK" -eq 1 && "$DO_SWEEP" -eq 0 ]]; then
  echo "  NOTE          quick mode skips the recovery sweep (no reduced mode; pass --sweep to run it in full)"
fi

# --- 1. build --------------------------------------------------------------
if [[ "$DO_BUILD" -eq 1 ]]; then
  stage "1/5 build (bazel -c opt)"
  "$BAZEL" build -c opt ${BAZEL_FLAGS[@]+"${BAZEL_FLAGS[@]}"} //bench:bench_harness //bench:bench_test_5 \
    || die "build failed"

  if [[ "$WITH_TESTS" -eq 1 ]]; then
    stage "1b/5 gates (timing conformance + provenance)"
    "$BAZEL" test -c opt ${BAZEL_FLAGS[@]+"${BAZEL_FLAGS[@]}"} //bench:timing_conformance_test //bench:provenance_test \
      || die "gate tests failed -- do not publish numbers from this tree"
  fi
else
  stage "1/5 build SKIPPED"
fi

[[ -x "$BENCH_BIN" ]] || die "$BENCH_BIN missing -- build first (drop --skip-build)"

# --- 2. timing study -------------------------------------------------------
if [[ "$DO_TIMING" -eq 1 ]]; then
  stage "2/5 timing study -> $METRICS_CSV"

  bench_args=(
    --runs "$RUNS"
    --replicates "$REPLICATES"
    --warmup-iterations "$WARMUP_ITERATIONS"
    --bundle-targets-mb "$BUNDLE_TARGETS_MB"
    --bundle-max-mb "$BUNDLE_MAX_MB"
    --seed "$SEED"
    --results-dir "$RESULTS_DIR"
  )
  [[ -n "$PROFILE" ]] && bench_args+=(--profile "$PROFILE")
  [[ "${SERIAL_BUILD:-0}" -eq 1 ]] && bench_args+=(--serial-build)

  # stdout is the CSV, stderr is the narration (provenance block, parity
  # tables). Keep them apart -- one row of narration in the CSV and every
  # downstream pandas read breaks -- while still streaming the narration live.
  # fd 3 carries stderr out to the tee; stdout goes straight to the file.
  rc=0
  { "$BENCH_BIN" "${bench_args[@]}" 3>&1 1>"$METRICS_CSV" 2>&3 3>&-; } \
    | tee "$RUN_LOG" >&2 || rc=$?

  if [[ "$rc" -eq 3 ]]; then
    die "provenance gate refused this run (exit 3) -- see $RUN_LOG. These numbers cannot become an artifact; pin the profile with --profile if it is reported ambiguous."
  elif [[ "$rc" -ne 0 ]]; then
    die "bench_harness failed (exit $rc) -- see $RUN_LOG"
  fi

  [[ -s "$METRICS_CSV" ]] || die "$METRICS_CSV is empty -- the harness produced no rows"
  echo "  $(( $(wc -l < "$METRICS_CSV") - 1 )) rows, provenance at $RESULTS_DIR/provenance.json"
else
  stage "2/5 timing study SKIPPED"
fi

# --- 3. artifact dump ------------------------------------------------------
if [[ "$DO_ARTIFACTS" -eq 1 ]]; then
  stage "3/5 artifact dump -> $ARTIFACTS_DIR"
  # main.cpp opens these paths with ofstream and never creates the directory,
  # and it does not check the stream -- a missing directory reports "wrote N
  # bytes" and writes nothing. Create it here.
  mkdir -p "$ARTIFACTS_DIR"
  "$BENCH_BIN" --dump-artifacts "$ARTIFACTS_DIR" >/dev/null \
    || die "artifact dump failed"

  for arm in fastfhir json hl7v2 google_fhir; do
    [[ -s "$ARTIFACTS_DIR/$arm.bin" ]] \
      || die "$ARTIFACTS_DIR/$arm.bin missing or empty after the dump"
  done
  echo "  4 arms written"
else
  stage "3/5 artifact dump SKIPPED"
fi

# --- 4. recovery sweep -----------------------------------------------------
if [[ "$DO_SWEEP" -eq 1 ]]; then
# REC-6 control artifact: NDJSON, derived FROM json.bin so the two carry
# byte-identical content and fingerprint to the same 34,839 units. Generated
# here rather than by an arm in the harness, because the point of the control
# is that nothing but the framing differs.
if [[ "$DO_ARTIFACTS" -eq 1 && -f "$ARTIFACTS_DIR/json.bin" ]]; then
  "$PYTHON" - "$ARTIFACTS_DIR" <<'PYEOF'
import json, sys, pathlib
d = pathlib.Path(sys.argv[1])
doc = json.loads((d / "json.bin").read_text())
lines = [json.dumps(e["resource"], separators=(",", ":"))
         for e in doc.get("entry", []) if isinstance(e.get("resource"), dict)]
(d / "ndjson.bin").write_text("\n".join(lines))
print(f"  ndjson.bin  {len(lines):,} records", file=sys.stderr)
PYEOF
fi

  stage "4/5 recovery sweep -> results/recovery_curve.csv"
  [[ -x "$SWEEP_DRIVER" ]] || die "$SWEEP_DRIVER missing -- build first (drop --skip-build)"
  for arm in fastfhir json hl7v2 google_fhir; do
    [[ -s "artifacts/$arm.bin" ]] \
      || die "artifacts/$arm.bin missing -- the sweep reads artifacts/ by a fixed path, so run stage 3 without --artifacts-dir"
  done
  # A SECOND SWEEP IS NOT A SECOND RUN, IT IS A RUINED ONE. recovery_sweep.py
  # opens results/recovery_curve.csv with "w" and holds the handle for hours,
  # so starting one while another is live truncates the running sweep's output
  # from under it and both processes then interleave rows into the same file.
  # Nothing downstream can tell that curve from a good one. Refuse instead.
  if pgrep -f "bazel-bin/bench/bench_test_5" >/dev/null 2>&1 \
     || pgrep -f "scripts/recovery_sweep.py" >/dev/null 2>&1; then
    die "a recovery sweep is already running -- wait for it, or pass --skip-sweep. Starting a second one truncates the first one's results/recovery_curve.csv."
  fi
  echo "  full sweep: 4 formats x 13 damage levels x 20 trials, several driver"
  echo "  processes per trial. Hours, not minutes."

  # Both the driver path and the output path are relative to the repo root
  # inside recovery_sweep.py, so this must run from ROOT_DIR (it does).
  # shellcheck disable=SC2086
  "$PYTHON" scripts/recovery_sweep.py $RECOVERY_SWEEP_ARGS 2>&1 \
    | tee "$RESULTS_DIR/sweep.log" \
    || die "recovery sweep failed -- see $RESULTS_DIR/sweep.log"

  [[ -s "results/recovery_curve.csv" ]] || die "the sweep wrote no curve"
  if [[ -s "results/_crashes.csv" ]] && [[ "$(wc -l < results/_crashes.csv)" -gt 1 ]]; then
    echo "  NOTE $(( $(wc -l < results/_crashes.csv) - 1 )) driver crash(es) recorded in results/_crashes.csv (scored 0%, not dropped)"
  fi
else
  stage "4/5 recovery sweep SKIPPED"
fi

# --- 5. figures ------------------------------------------------------------
if [[ "$DO_FIGURES" -eq 1 ]]; then
  stage "5/5 figures -> $FIGURES_DIR"
  [[ -s "$METRICS_CSV" ]] \
    || die "$METRICS_CSV missing -- figures need a timing study (drop --skip-timing)"
  [[ -f "results/recovery_curve.csv" ]] \
    || echo "  NOTE no results/recovery_curve.csv -- fig8_recovery will be skipped"

  "$PYTHON" scripts/plot_benchmarks.py \
    --csv "$METRICS_CSV" \
    --results-dir "$RESULTS_DIR" \
    --out "$FIGURES_DIR" \
    --format "$FIGURE_FORMAT" \
    || die "figure rendering failed"
else
  stage "5/5 figures SKIPPED"
fi

stage "done"
echo "  metrics     $METRICS_CSV"
echo "  provenance  $RESULTS_DIR/provenance.json"
echo "  curve       results/recovery_curve.csv"
echo "  figures     $FIGURES_DIR"
if [[ "$QUICK" -eq 1 ]]; then
  echo
  echo "  QUICK RUN -- reduced ladder and run count. Not an artifact; do not cite."
fi
