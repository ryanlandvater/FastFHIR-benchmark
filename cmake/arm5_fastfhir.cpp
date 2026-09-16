// Test-5 FastFHIR arm for the CMake/Xcode DEBUG build. See arm5_hl7v2.cpp.
#include <algorithm>
#include <string>
#include <vector>

#define ARM_FASTFHIR
#include "bench_test_5.hpp"
#undef ARM_FASTFHIR

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_fastfhir::structural_positions(wire).size();
}
}  // namespace

const ArmOps& arm_ops_fastfhir() {
  static const ArmOps ops{"fastfhir", &arm_fastfhir::calc_stream_hash,
                          &arm_fastfhir::corrupt_stream, &arm_fastfhir::recover_stream,
                          &count_positions};
  return ops;
}
}  // namespace bench::test_5
