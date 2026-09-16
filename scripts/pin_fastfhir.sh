#!/usr/bin/env bash
#
# Pin the library under test (TASKS.md PB-1a/PB-1b).
#
# `.external/FastFHIR` is a symlink to the live working tree, which is right
# for development and wrong for a published number: whatever state that tree
# is in is what gets measured. This script produces a clean, detached checkout
# of one commit, with generated_src/ built at one profile, and prints its path.
# run_benchmark.sh hands that path to Bazel with --override_module, so the
# symlink and MODULE.bazel stay the developer default.
#
#   scripts/pin_fastfhir.sh REF [--profile P] [--repo URL|PATH] [--packages DIR]
#
#   REF         tag, branch or SHA in --repo
#   --profile   FASTFHIR_PRODUCTION_PROFILE. Default: the pinned checkout's own
#               CMakePresets.json "base" preset, so the release defines what is
#               benchmarked rather than whatever this machine last configured.
#   --repo      Where to fetch from (default: the GitHub repo). A local path
#               works for commits that are not pushed yet.
#   --packages  An existing fhir_packages/ to reuse instead of downloading it
#               again (default: ../FastFHIR/fhir_packages when present).
#
# The last line of stdout is the pinned directory; everything else goes to
# stderr.
#
# Why not archive_override: a tarball has no .git, so provenance.hpp could not
# establish the SHA from the tree it is handed -- and the SHA is the point.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PINS_DIR="$ROOT_DIR/.external/pins"
MIRROR="$PINS_DIR/FastFHIR.git"

REPO="${FASTFHIR_REPO:-https://github.com/ryanlandvater/FastFHIR.git}"
PROFILE="${PROFILE:-}"
PACKAGES="${FHIR_PACKAGES_DIR:-}"
REF=""

die() { echo "pin_fastfhir: $*" >&2; exit 1; }
say() { echo "pin_fastfhir: $*" >&2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)  PROFILE="${2:?--profile needs a value}"; shift ;;
    --repo)     REPO="${2:?--repo needs a URL or path}"; shift ;;
    --packages) PACKAGES="${2:?--packages needs a directory}"; shift ;;
    -h|--help)  sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)         die "unknown argument: $1" ;;
    *)          [[ -z "$REF" ]] || die "one REF only (got '$REF' and '$1')"; REF="$1" ;;
  esac
  shift
done
[[ -n "$REF" ]] || die "usage: pin_fastfhir.sh REF [--profile P] [--repo URL|PATH] [--packages DIR]"

# A relative local path would be resolved against the mirror by git.
if [[ -d "$REPO" ]]; then REPO="$(cd "$REPO" && pwd)"; fi

# The generator targets 3.11+ (../FastFHIR/CMakeLists.txt, find_package(Python3
# 3.11)); macOS's Command Line Tools python3 is 3.9 and fails at import.
PYTHON="${PYTHON:-python3}"
"$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null \
  || die "$PYTHON is missing or older than 3.11 -- set PYTHON"

# --- resolve REF against a local mirror -----------------------------------
mkdir -p "$PINS_DIR"
if [[ ! -d "$MIRROR" ]]; then
  say "mirroring $REPO"
  git clone --quiet --bare "$REPO" "$MIRROR" >&2
fi
git -C "$MIRROR" remote set-url origin "$REPO"
git -C "$MIRROR" fetch --quiet --prune --tags origin '+refs/heads/*:refs/heads/*' >&2 \
  || die "fetch from $REPO failed"

SHA="$(git -C "$MIRROR" rev-parse --verify --quiet "$REF^{commit}")" \
  || die "'$REF' is not a commit in $REPO"

# --- checkout ---------------------------------------------------------------
# The profile is part of the directory name: one tree per (commit, profile),
# so two pins never regenerate each other's generated_src/.
checkout_profile() {
  git -C "$MIRROR" show "$SHA:CMakePresets.json" 2>/dev/null | "$PYTHON" -c '
import json, sys
for p in json.load(sys.stdin).get("configurePresets", []):
    if p.get("name") == "base":
        print(p.get("cacheVariables", {}).get("FASTFHIR_PRODUCTION_PROFILE", ""))
'
}
PROFILE_SOURCE="operator"
if [[ -z "$PROFILE" ]]; then
  PROFILE="$(checkout_profile)"
  PROFILE_SOURCE="CMakePresets.json base preset"
  [[ -n "$PROFILE" ]] || die "$SHA has no base-preset profile -- pass --profile"
fi
SLUG="$(printf '%s' "$PROFILE" | tr ',' '+' | tr -c 'A-Za-z0-9+._-' '_')"
PIN="$PINS_DIR/fastfhir-${SHA:0:12}-$SLUG"

if [[ ! -d "$PIN/.git" ]]; then
  say "checking out ${SHA:0:12} -> $PIN"
  rm -rf "$PIN"
  git clone --quiet --shared --no-checkout "$MIRROR" "$PIN" >&2
  git -C "$PIN" checkout --quiet --detach "$SHA" >&2
fi

# A pin is only a pin if nothing has touched it since.
[[ "$(git -C "$PIN" rev-parse HEAD)" == "$SHA" ]] \
  || die "$PIN is not at ${SHA:0:12} -- delete it and re-run"
[[ -z "$(git -C "$PIN" status --porcelain)" ]] \
  || die "$PIN has local changes -- delete it and re-run"

# --- FHIR packages ----------------------------------------------------------
# specs.py downloads version-pinned packages into ./fhir_packages when absent.
# Reuse an existing copy by symlink. Upstream's ignore rule is `fhir_packages/`,
# which matches a directory but not a symlink, so exclude it locally or the
# tree reads as dirty.
if [[ -z "$PACKAGES" && -d "$ROOT_DIR/../FastFHIR/fhir_packages" ]]; then
  PACKAGES="$ROOT_DIR/../FastFHIR/fhir_packages"
fi
if [[ -n "$PACKAGES" && ! -e "$PIN/fhir_packages" ]]; then
  [[ -d "$PACKAGES" ]] || die "--packages $PACKAGES is not a directory"
  ln -s "$(cd "$PACKAGES" && pwd)" "$PIN/fhir_packages"
fi
grep -qxF '/fhir_packages' "$PIN/.git/info/exclude" 2>/dev/null \
  || echo '/fhir_packages' >> "$PIN/.git/info/exclude"

# --- generate ---------------------------------------------------------------
# Mirrors ../FastFHIR/CMakeLists.txt (generator section): run the generator
# with the profile in the environment, then stamp generated_src/.profile only
# on success. Bazel globs generated_src/ and never runs the generator itself.
STAMP="$PIN/generated_src/.profile"
if [[ -f "$STAMP" && "$(tr -d '[:space:]' < "$STAMP")" == "$(printf '%s' "$PROFILE" | tr -d '[:space:]')" ]]; then
  say "generated_src already at profile $PROFILE"
else
  # The generator never deletes output it no longer emits; start clean.
  rm -rf "$PIN/generated_src"
  say "generating at profile $PROFILE ($PROFILE_SOURCE)"
  (cd "$PIN" && FASTFHIR_PRODUCTION_PROFILE="$PROFILE" "$PYTHON" -m generator) >&2 \
    || die "generator failed in $PIN"
  printf '%s\n' "$PROFILE" > "$STAMP"
fi

[[ -z "$(git -C "$PIN" status --porcelain)" ]] \
  || die "generation left $PIN dirty: $(git -C "$PIN" status --porcelain | head -3 | tr '\n' ' ')"

say "pinned ${SHA:0:12} ($(git -C "$PIN" describe --tags --always)) at profile $PROFILE"
echo "$PIN"
