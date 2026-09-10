/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// NDJSON codec for Instrument G test 5 (REC-6).
//
// WHAT THIS ARM IS FOR
// It is a CONTROL, not a competitor. It carries byte-for-byte the same FHIR
// resources as the json arm and emits byte-for-byte the same canonical leaves;
// the only difference is FRAMING -- one resource per line instead of one
// Bundle wrapping an array.
//
// THE QUESTION IT ANSWERS
// The json arm loses ~0.8 of a resource per single flip against hl7v2's ~0.13,
// and the explanation on record is damage GRANULARITY: a JSON Bundle entry
// frames a whole resource in one brace-delimited object, so structural damage
// inside it costs the whole resource, while v2 spreads that resource across
// many independently framed segments. If that is right, NDJSON -- same syntax,
// same escaping rules, same repair, records delimited by '\n' rather than by
// nesting -- should behave like v2 and not like json. If instead NDJSON tracks
// json, the explanation is wrong and the cause is the syntax itself.
//
// It is therefore the difference between publishing "JSON is fragile" and
// "single-document nesting is fragile", which are different claims about a
// different thing to fix.
//
// FRAMING IS THE ONLY VARIABLE
//   - identical resources (the artifact is generated FROM json.bin)
//   - identical leaf paths (the same walk, so the census must match at 34,839)
//   - identical damage rule (every unescaped syntax byte, plus the '\n'
//     record separators, which are this format's terminators exactly as '\r'
//     is v2's)
//   - identical repair (bench/json_syntax_repair.hpp)
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {`. Not self-contained; do not include it directly.
#ifndef ARM_NDJSON_CODEC_HPP
#define ARM_NDJSON_CODEC_HPP

#include "json_leaf_walk.hpp"
#include "json_syntax_repair.hpp"

// ---------------------------------------------------------------------------
// READER -- canonical scan
// ---------------------------------------------------------------------------

/// Walk one resource object into canonical leaves. Identical to the json arm's
/// per-resource body, so the two artifacts fingerprint to the same units.
inline void nd_collect_resource(const nlohmann::json& res, StreamFingerprint& fp) {
    if (!res.is_object()) return;
    const auto type = res.value("resourceType", std::string{});
    const auto id = res.value("id", std::string{});
    const auto key = resource_key(type, id);
    if (key.empty()) return;  // unidentifiable -- not comparable
    for (auto it = res.begin(); it != res.end(); ++it) {
        if (it.key() == "resourceType") continue;  // it IS the key
        json_walk_leaves(it.value(), key + "." + it.key(), fp);
    }
}

inline std::vector<std::string> nd_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto at = text.find('\n', start);
        if (at == std::string::npos) { out.push_back(text.substr(start)); break; }
        out.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    return out;
}

/// `repair` is REC-3, ON only from recover_stream(). A line that will not parse
/// is handed to the shared JSON repair before it is given up on.
///
/// The record boundary is the newline, so a damaged line costs THAT LINE and
/// its neighbours are unaffected -- there is no enclosing structure whose depth
/// the damage can disturb. That is the whole hypothesis under test.
inline StreamFingerprint nd_scan(const std::vector<uint8_t>& wire, bool repair) {
    StreamFingerprint fp;
    const std::string text(wire.begin(), wire.end());
    for (const auto& line : nd_lines(text)) {
        if (line.size() < 2) continue;
        nlohmann::json res;
        bool ok = nlohmann::json::accept(line);
        if (ok) {
            res = nlohmann::json::parse(line, nullptr, false);
            ok = !res.is_discarded();
        } else if (repair) {
            ok = bench::json_repair::repair(line, res);
        }
        if (ok) nd_collect_resource(res, fp);
    }
    fp.finalize();
    return fp;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    return nd_scan(wire, /*repair=*/false);
}

// ---------------------------------------------------------------------------
// DAMAGE MODEL -- structural positions eligible for a bit flip
// ---------------------------------------------------------------------------
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
    // The json arm's rule verbatim -- every unescaped syntax byte, an escape
    // pair (\X) exempting both of its bytes -- PLUS the '\n' record separators.
    // Including them is required for the comparison to be honest: v2's '\r' is
    // a corruption target, so NDJSON's terminator must be one too, or this arm
    // would be handed damage-free framing that neither of the others gets.
    auto is_syntax = [](uint8_t c) {
        return c == '{' || c == '}' || c == '[' || c == ']' || c == '"' ||
               c == ':' || c == ',' || c == '\n';
    };
    std::vector<std::size_t> positions;
    bool escaped = false;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        if (escaped) { escaped = false; continue; }
        if (wire[i] == '\\') { escaped = true; continue; }
        if (is_syntax(wire[i])) positions.push_back(i);
    }
    return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
    return flip_positions(wire, structural_positions(wire), k, seed);
}

// ---------------------------------------------------------------------------
// RECOVERY -- recovery-ON read of a damaged wire
// ---------------------------------------------------------------------------
// No resynchronisation step is needed or possible beyond the newline itself:
// the record boundary IS a byte, so finding the next record is a scan, not a
// search. A flipped '\n' merges two records and both are lost, which is the
// same failure mode a flipped '\r' produces in v2.
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
    return nd_scan(wire, /*repair=*/true);
}

#endif  // ARM_NDJSON_CODEC_HPP
