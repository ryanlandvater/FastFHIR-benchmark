/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// REC-3: best-effort structural repair of damaged JSON.
//
// WHY THIS EXISTS, AND WHY IT IS SHARED
// Both the json arm and the hl7v2 arm need it. 82.4% of the flips the v2
// damage model applies at k=2048 land on JSON punctuation inside ZFX payloads
// -- the v2 wire is 70.7% ZFX-JSON by structural position -- so "repair the
// JSON" is the dominant recovery opportunity for BOTH arms, and neither should
// own the implementation.
//
// GROUNDED IN WHAT PRODUCTION DOES
// The rule set is `jsonrepair`'s (josdejong), restricted to the rules that bear
// on this fault model: missing closing brackets, truncated documents, missing
// commas and colons, missing quotes. Its other rules -- Python constants,
// MongoDB types, comments, JSONP -- address LLM and JS-paste input, not bit
// flips, and are deliberately not implemented.
//
// THE FAULT MODEL IS SUBSTITUTION, NOT OMISSION
// A flip turns '}' into some other byte; it does not delete it. The jsonrepair
// rules still apply because the SYMPTOMS coincide -- unbalanced depth,
// juxtaposed values, an unterminated string -- but the repair that fits is
// putting a structural byte BACK, not inserting a new one. That is why
// substitution is tried first and insertion only as a fallback.
//
// STRUCTURE ONLY, NEVER CONTENT
// Every candidate here edits punctuation. Nothing invents a key, a value, or a
// digit. A repair that guessed content would manufacture units that score as
// `correct` against a baseline they were reverse-engineered from, which would
// make the whole census meaningless. The four-outcome scoring is the check:
// a wrong structural guess reparents real data and scores `wrong`/`spurious`,
// it does not quietly pass.
#ifndef BENCH_JSON_SYNTAX_REPAIR_HPP
#define BENCH_JSON_SYNTAX_REPAIR_HPP

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace bench::json_repair {

// The bytes a flip can have destroyed. '"' is first because it is the most
// destructive loss (a lost quote swallows content until the next one) and so
// the most valuable to restore.
inline constexpr char kStructural[] = {'"', '{', '}', '[', ']', ':', ',' };

// How far either side of the reported error to try. A flip is at or before the
// byte the parser choked on -- an unterminated string reports the error at the
// END of the run it swallowed -- so the window leans backwards.
inline constexpr int kLookBehind = 64;
inline constexpr int kLookAhead = 4;

struct Stats {
    std::size_t attempted = 0;   // fragments that failed to parse
    std::size_t repaired = 0;    // ... and were made to parse
};

/// Close whatever `text` left open, using a quote-aware depth walk.
/// Handles the "truncated document" rule: the damage removed a closer, so the
/// structure never returns to depth 0.
inline std::string close_open_structures(const std::string& text) {
    std::vector<char> stack;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (escaped) { escaped = false; continue; }
        if (in_string) {
            if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') stack.push_back(c);
        else if (c == '}' || c == ']') { if (!stack.empty()) stack.pop_back(); }
    }
    std::string out = text;
    if (in_string) out.push_back('"');
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        out.push_back(*it == '{' ? '}' : ']');
    return out;
}

/// Best-effort repair. Returns true and fills `out` when a candidate PARSES;
/// a candidate is never accepted on looks alone.
///
/// Search is bounded by the parser's own error position rather than sweeping
/// the fragment: nlohmann reports the byte it choked on, and trying a handful
/// of substitutions around it is the difference between microseconds and a
/// full-fragment scan on every damaged payload.
inline bool repair(const std::string& text, nlohmann::json& out, Stats* stats = nullptr) {
    // 1. Already valid: no repair, and no attempt recorded.
    if (nlohmann::json::accept(text)) {
        out = nlohmann::json::parse(text, nullptr, false);
        return true;
    }
    if (stats) ++stats->attempted;

    const auto try_text = [&out](const std::string& candidate) -> bool {
        if (!nlohmann::json::accept(candidate)) return false;
        out = nlohmann::json::parse(candidate, nullptr, false);
        return !out.is_discarded();
    };

    // 2. Unclosed structures -- the cheapest rule and the one that fixes a lost
    //    closer or a lost final quote outright.
    if (try_text(close_open_structures(text))) {
        if (stats) ++stats->repaired;
        return true;
    }

    // 3. Locate the fault. The parser names a byte; a flip is at or before it.
    std::size_t err = text.size();
    try {
        (void)nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        err = e.byte ? e.byte - 1 : 0;   // parse_error::byte is 1-based
    }
    const std::size_t lo =
        err > static_cast<std::size_t>(kLookBehind) ? err - kLookBehind : 0;
    const std::size_t hi = std::min(text.size(), err + kLookAhead);

    // 4. SUBSTITUTION first -- the fault model is a flipped byte, so the repair
    //    that fits is putting a structural byte back where a non-structural one
    //    now sits. Skip positions that already hold the character being tried.
    for (std::size_t i = lo; i < hi; ++i) {
        for (const char c : kStructural) {
            if (text[i] == c) continue;
            std::string candidate = text;
            candidate[i] = c;
            if (try_text(candidate) || try_text(close_open_structures(candidate))) {
                if (stats) ++stats->repaired;
                return true;
            }
        }
    }

    // 5. INSERTION as a fallback -- covers a separator that was flipped into a
    //    byte which is itself legal content, so the character is not missing
    //    from the stream, only its meaning is.
    for (std::size_t i = lo; i <= hi && i <= text.size(); ++i) {
        for (const char c : {',', ':', '"'}) {
            std::string candidate = text;
            candidate.insert(i, 1, c);
            if (try_text(candidate) || try_text(close_open_structures(candidate))) {
                if (stats) ++stats->repaired;
                return true;
            }
        }
    }
    return false;
}

}  // namespace bench::json_repair

#endif  // BENCH_JSON_SYNTAX_REPAIR_HPP
