// Test-5 HL7v2 arm for the CMake/Xcode DEBUG build.
//
// The Bazel arm TU (bench/arm_hl7v2.cpp) also carries tests 1-4 and the whole
// harness -- the enrichment fixture, the JSON corpus loader, the timers. None
// of that is test 5, and dragging it in is what would otherwise force the
// debug build to link nlohmann and the Synthea fixture just to step through a
// segment scan. This TU compiles the SAME bench_test_5.hpp under the SAME
// ARM_HL7V2 macro; only the harness is absent.
#include <algorithm>
#include <string>
#include <vector>

// The v2 arm decodes ZFX payloads, which are JSON fragments, so it needs
// nlohmann now -- it is no longer the dependency-free arm its Bazel comment
// describes. The Bazel target already got this transitively via
// bench_core_common; only this harness-free shim had to be told.
#include <nlohmann/json.hpp>

#define ARM_HL7V2
#include "bench_test_5.hpp"
#undef ARM_HL7V2

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_hl7v2::structural_positions(wire).size();
}
}  // namespace

const ArmOps& arm_ops_hl7v2() {
  static const ArmOps ops{"hl7v2", &arm_hl7v2::calc_stream_hash,
                          &arm_hl7v2::corrupt_stream, &arm_hl7v2::recover_stream,
                          &count_positions};
  return ops;
}
}  // namespace bench::test_5
