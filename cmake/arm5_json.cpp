// Test-5 JSON arm for the CMake/Xcode DEBUG build. See arm5_hl7v2.cpp.
#include <algorithm>
#include <string>
#include <vector>

// The Bazel arm TU picks this up transitively through harness.hpp; this
// harness-free shim has to ask for it directly.
#include <nlohmann/json.hpp>

#define ARM_JSON
#include "bench_test_5.hpp"
#undef ARM_JSON

namespace bench::test_5 {
namespace {
std::size_t count_positions(const std::vector<uint8_t>& wire) {
  return arm_json::structural_positions(wire).size();
}
}  // namespace

const ArmOps& arm_ops_json() {
  static const ArmOps ops{"json", &arm_json::calc_stream_hash,
                          &arm_json::corrupt_stream, &arm_json::recover_stream,
                          &count_positions};
  return ops;
}
}  // namespace bench::test_5
