/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// REC-6 control arm: NDJSON. Test 5 only -- it is not a fifth arm on the 4x4
// timing grid, because it answers a framing question about corruption, not a
// performance question. See arm_ndjson_codec.hpp for what it isolates.
#include "harness.hpp"

#include <nlohmann/json.hpp>

#define ARM_NDJSON
#include "bench_test_5.hpp"
#undef ARM_NDJSON

namespace bench::test_5 {

std::size_t count_positions_ndjson(const std::vector<uint8_t> &wire) {
  return arm_ndjson::structural_positions(wire).size();
}

const ArmOps &arm_ops_ndjson() {
  static const ArmOps ops{"ndjson", &arm_ndjson::calc_stream_hash,
                          &arm_ndjson::corrupt_stream, &arm_ndjson::recover_stream,
                          &count_positions_ndjson};
  return ops;
}
}  // namespace bench::test_5
