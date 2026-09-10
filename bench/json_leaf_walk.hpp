/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The FHIR-JSON leaf walk, shared by the json and ndjson codecs.
//
// Extracted 2026-09-08 for REC-6. The ndjson arm exists to vary ONE thing --
// record framing -- so it must emit byte-identical leaf paths to the json arm.
// Two copies of this walk could drift, and a drift here would look like a
// corruption-resilience difference in the results when it was really a
// difference in what the two readers chose to call a leaf.
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {`. Not self-contained; do not include it directly.
#ifndef BENCH_JSON_LEAF_WALK_HPP
#define BENCH_JSON_LEAF_WALK_HPP

inline void json_walk_leaves(const nlohmann::json& node, const std::string& path,
                             StreamFingerprint& fp) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            json_walk_leaves(it.value(), path + "." + it.key(), fp);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            json_walk_leaves(node[i], path + "[" + std::to_string(i) + "]", fp);
    } else {
        // dump() rather than a type-specific formatter: every arm has to render
        // the value the SAME way or equal data compares unequal. JSON's own
        // canonical form is the one all four can reach.
        fp.add_leaf(path, safe_dump(node));
    }
}

#endif  // BENCH_JSON_LEAF_WALK_HPP
