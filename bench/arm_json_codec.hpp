/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// FHIR-JSON codec for Instrument G test 5: canonical scan, the damage model's
// structural-position map, and recovery.
//
// Split out of bench_test_5.hpp on 2026-09-08, same rationale as
// arm_hl7v2_codec.hpp: the harness owns the census and the corruption driver,
// the codec owns format knowledge.
//
// WHAT LIVES HERE
//   json_walk_leaves() / json_collect()   the reader
//   structural_positions()                which bytes the damage model may flip
//   recover_stream()                      whole-document parse, else resync on
//                                         "resource" markers with brace-matched
//                                         extents -- the arm HAS had record
//                                         resynchronisation all along
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {` under `#if defined(ARM_JSON)`. Not self-contained by design;
// do not include it directly.
#ifndef ARM_JSON_CODEC_HPP
#define ARM_JSON_CODEC_HPP

#include "json_leaf_walk.hpp"
#include "json_syntax_repair.hpp"

// ---------------------------------------------------------------------------
// READER -- canonical scan
// ---------------------------------------------------------------------------
// The byte span of the RESOURCE OBJECT a `"resource"` marker introduces: the
// '{' after the marker's colon, brace-matched forward. Used by the resync path
// when the document as a whole will not parse.
//
// The span must be the resource object itself, not the enclosing entry object.
// Scanning BACKWARD from the marker to a brace lands on the entry's '{', and a
// parsed entry root has no resourceType -- so the salvage parsed fine and then
// skipped every resource as unidentifiable, scoring 0 for any damage at all
// (measured at every k >= 1 across seeds). Matching forward from the value
// brace keeps resourceType at the parsed root, exactly where json_collect
// reads it. Strings and escapes are honoured: braces inside string values must
// not count, and nested objects (contained resources, extensions) must.
inline std::pair<std::size_t, std::size_t> json_resource_extent(const std::string& text,
                                                                std::size_t marker) {
    const auto colon = text.find(':', marker);
    std::size_t open = (colon == std::string::npos) ? marker : colon + 1;
    while (open < text.size() && text[open] != '{')
        ++open;
    if (open >= text.size())
        return {marker, marker};  // no resource object to salvage
    int depth = 0;
    bool in_str = false, esc = false;
    std::size_t close = open;
    for (; close < text.size(); ++close) {
        const char c = text[close];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { ++close; break; }
    }
    if (depth != 0)
        return {marker, marker};  // unbalanced: nothing salvageable here
    return {open, close};
}

// The JSON arm's document IS FHIR, so it defines the canonical scheme the other
// arms are measured against: walk to every scalar and name it by its element
// path under <ResourceType>/<id>.

inline void json_collect(const nlohmann::json& doc, StreamFingerprint& fp) {
    const auto entries = doc.find("entry");
    if (entries == doc.end() || !entries->is_array()) return;
    for (const auto& entry : *entries) {
        const auto res = entry.find("resource");
        if (res == entry.end() || !res->is_object()) continue;
        const auto type = res->value("resourceType", std::string{});
        const auto id = res->value("id", std::string{});
        const auto key = resource_key(type, id);
        if (key.empty()) continue;  // unidentifiable -- not comparable
        // Walk the resource's members with the key already in the path: the
        // prefix is part of what gets hashed, not something bolted on after.
        for (auto it = res->begin(); it != res->end(); ++it) {
            if (it.key() == "resourceType") continue;  // it IS the key
            json_walk_leaves(it.value(), key + "." + it.key(), fp);
        }
    }
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    try {
        const auto doc = nlohmann::json::parse(std::string(wire.begin(), wire.end()));
        json_collect(doc, fp);
    } catch (const std::exception&) {
        // Unparseable: no units. The check reports the loss rather than this
        // throwing and taking the whole measurement with it.
    }
    fp.finalize();
    return fp;
}

// ---------------------------------------------------------------------------
// DAMAGE MODEL -- structural positions eligible for a bit flip
// ---------------------------------------------------------------------------
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  // Comparable blast rule (Ryan 2026-09-02): EVERY syntax character is a
  // corruption target unless it is EXPLICITLY ESCAPED -- in a string or out.
  // An escape pair (\X) skips both bytes; nothing else is exempt. A brace or
  // comma inside a string value is unescaped content that still LOOKS like
  // syntax to a blind scanner, so it gets blasted exactly like a structural
  // brace -- the recovery (reparse) is what decides what survives. This is
  // the same rule v2 applies to its delimiters (v2 has no escapes for them),
  // which is what makes the four arms comparable.
  auto is_syntax = [](uint8_t c) {
    return c == '{' || c == '}' || c == '[' || c == ']' || c == '"' || c == ':' || c == ',';
  };
  std::vector<std::size_t> positions;
  bool escaped = false;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    if (escaped) {
      escaped = false;
      continue;  // \X: the escaped byte is explicitly exempt
    }
    if (wire[i] == '\\') {
      escaped = true;
      continue;  // the escape introducer itself is not a syntax char
    }
    if (is_syntax(wire[i]))
      positions.push_back(i);
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
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    const std::string text(wire.begin(), wire.end());

    // Whole document still parses: every leaf is reachable.
    try {
        json_collect(nlohmann::json::parse(text), fp);
        fp.finalize();
        return fp;
    } catch (const std::exception&) {
    }

    // NOT attempted: whole-document repair. It was tried and removed on
    // 2026-09-08 -- it is redundant with the per-resource repair below (the
    // damaged entry is repaired there either way) and it cost 4.6s per
    // replicate, because every candidate re-parses the entire ~1.6 MB Bundle.
    // The repair budget belongs where the damage is, which is inside one entry.

    // RESYNC. One broken brace makes the whole document unparseable, and
    // scoring that as total loss would flatter the failure -- JSON's real
    // behaviour is that the damage is local to the object it lands in. So
    // recover each resource independently: find each `"resource"` marker, take
    // its brace-matched span, and keep the ones that still parse.
    std::size_t pos = 0;
    while (true) {
        const auto marker = text.find("\"resource\"", pos);
        if (marker == std::string::npos) break;
        const auto [open, close] = json_resource_extent(text, marker);
        if (close > open) {
            try {
                nlohmann::json res;
                // REC-3 again, now scoped to one resource. A resync extent that
                // will not parse is exactly the entry the damage landed in, so
                // this is the only path that can return it.
                if (!bench::json_repair::repair(text.substr(open, close - open), res))
                    throw std::runtime_error("unrepairable resource extent");
                const auto type = res.value("resourceType", std::string{});
                const auto id = res.value("id", std::string{});
                const auto key = resource_key(type, id);
                if (!key.empty()) {
                    for (auto it = res.begin(); it != res.end(); ++it) {
                        if (it.key() == "resourceType") continue;
                        json_walk_leaves(it.value(), key + "." + it.key(), fp);
                    }
                }
            } catch (const std::exception&) {
                // this resource is unreadable; the rest of the document is not
            }
        }
        const auto next = text.find("\"resource\"", marker + 10);
        pos = (next == std::string::npos) ? text.size() : next;
    }
    fp.finalize();
    return fp;
}

#endif  // ARM_JSON_CODEC_HPP
