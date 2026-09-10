/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// HL7v2 codec for Instrument G test 5: canonical scan, the damage model's
// structural-position map, and recovery.
//
// Split out of bench_test_5.hpp on 2026-09-08. That header is the test-5
// harness -- fingerprints, the four-outcome census, the corruption driver --
// and it had grown 442 lines of v2-specific decoding across three separate
// macro chains. Format knowledge belongs with the format.
//
// WHAT LIVES HERE
//   scan_v2_canonical()     the reader: segments -> canonical FHIR leaves
//   hl7_expected_arity()    REC-1, the frame-shift detector
//   structural_positions()  which bytes the damage model may flip
//   recover_stream()        recovery-ON read of a damaged wire
//
// This wire is 70.7% ZFX-JSON by structural position (82.4% of flips at
// k=2048 land there), so most of what follows is about the JSON payloads v2
// carries rather than about pipes.
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {` under `#if defined(ARM_HL7V2)`. It is not self-contained by
// design: it uses StreamFingerprint, resource_key, safe_dump and
// content_hash from the harness. Do not include it directly.
#ifndef ARM_HL7V2_CODEC_HPP
#define ARM_HL7V2_CODEC_HPP

#include "json_syntax_repair.hpp"

// ---------------------------------------------------------------------------
// READER -- canonical scan
// ---------------------------------------------------------------------------
// ── THE INVERSE OF THE v2 ENCODER ───────────────────────────────────────────
//
// Derived from the forward mapping (hl7v2_message.hpp + the ARM_HL7V2 assign
// path), not from reading the bytes and guessing. Each rule below names the
// encoder line it inverts:
//
//   MSH ... |<message_control_id>|P|2.5      MshSegment::serialize -- field 9
//                                            carries the PATIENT id.
//   PID|1||<id>||<name>||<dob>|<sex>|||...   PidSegment::serialize -- fixed
//                                            positions; birthDate and gender
//                                            reach the wire ONLY here.
//   OBX|<n>|<type>|<code>||<value>|<units>   ObxSegment::serialize -- the
//                                            observation VALUE is only here.
//   ZFX|<fhir.path>|<json>                   CustomFieldSegment::serialize --
//                                            the path is literal, the payload
//                                            is JSON. `.details` means "the
//                                            whole object at this path"
//                                            (hl7_append_json_field(...,
//                                            "patient.name[0].details", ...)),
//                                            so the inverse drops the marker
//                                            and walks the payload beneath it.
//
// Grouping is by ORDER, and the encoder makes that safe: assign_observation
// writes `observation.id` FIRST, so every observation.* segment after one
// belongs to it until the next.
inline std::string hl7_unescape(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\\' && i + 2 < src.size() && src[i + 2] == '\\') {
            switch (src[i + 1]) {
                case 'F': out += '|'; i += 2; continue;
                case 'S': out += '^'; i += 2; continue;
                case 'T': out += '&'; i += 2; continue;
                case 'R': out += '~'; i += 2; continue;
                case 'E': out += '\\'; i += 2; continue;
                default: break;
            }
        }
        out += src[i];
    }
    return out;
}

inline std::vector<std::string> hl7_split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

// Walk a ZFX payload, which is a JSON fragment rooted at `path`.
inline void hl7_walk_json(const nlohmann::json& node, const std::string& path,
                          StreamFingerprint& fp) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            hl7_walk_json(it.value(), path + "." + it.key(), fp);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            hl7_walk_json(node[i], path + "[" + std::to_string(i) + "]", fp);
    } else {
        fp.add_leaf(path, safe_dump(node));
    }
}

// REC-1: SEGMENT ARITY -- the frame-shift detector.
//
// v2 field identity is ORDINAL: OBX-5 IS the fifth thing between pipes. A
// flipped '|' does not merge two fields and stop there -- hl7_split returns one
// fewer element and EVERY later index shifts down, so every field after the
// damage is read from the wrong position. The shift runs to the segment
// terminator, because v2 restarts field numbering at each segment.
//
// It moves both ways. '~' (0x7E) and '\' (0x5C) are each one bit from '|'
// (0x7C), so flipping a repetition or escape separator can CREATE a field
// boundary; flipping a '\r' merges two segments into one over-long line.
//
// This encoder emits exactly one arity per segment type -- measured on the
// clean wire, not assumed: MSH 12, PID 14, OBX 7, ZFX 3 over 14,773 segments.
// A count that does not match means the field mapping is untrustworthy, so the
// segment is dropped rather than read at shifted indices. That converts silent
// corruption into honest loss: `wrong` and `spurious` become `missing`.
//
// It deliberately does NOT try to locate WHICH delimiter moved. One flip plus a
// known arity tells you that a shift happened, not where, and guessing would
// re-invent the global attribution bug that OBX-4 keying fixed.
//
// Lives in the shared scanner rather than in recover_stream() because refusing
// to misread is reader robustness, not repair -- any competent v2 reader
// sanity-checks field counts, so the no-recovery number should get it too.
// REC-2's resynchronisation is what will separate the ON and OFF curves.
// On the clean wire every arity matches, so the baseline fingerprint is
// unchanged by construction.
inline int hl7_expected_arity(const std::string& name) {
    if (name == "MSH") return 12;
    if (name == "PID") return 14;
    if (name == "OBX") return 7;
    if (name == "ZFX") return 3;
    return -1;  // unknown segment type: no expectation to check against
}

// REC-2: SEGMENT RESYNCHRONISATION on the 3-char header.
//
// v2's records are self-identifying -- every segment opens with its own type
// name -- which is the property JSON lacks and the reason a v2 reader can find
// where the next record starts. Two rules use it, and BOTH are gated on the
// REC-1 arity so neither can fire on a healthy segment.
//
// The gate is sound because a raw '|' cannot occur inside a payload: the
// encoder escapes it to \F\. Every unescaped '|' on the wire is therefore a
// real field separator, so the field count is trustworthy evidence even when
// the bytes around it are not.
//
// R2a NAME REPAIR (7.6% of flips land on a 3-char name). A flipped name makes
//     the segment unrecognised and it is dropped whole. But the SHAPE still
//     identifies it: the four arities are distinct (12/14/7/3), so a field
//     count that matches exactly one known type names the type. ZFX
//     additionally requires field 1 to be a FHIR path, because arity 3 is the
//     common case and a coincidence there would be costly.
//
// R2b TERMINATOR RE-SPLIT (2.5% of flips land on a '\r'). A flipped terminator
//     merges two segments onto one line, and REC-1 then drops BOTH. The second
//     segment's name is still in the bytes -- it is sitting at the tail of some
//     field -- so the line can be cut back apart there. This is the rule Mirth
//     does not have: its preprocessor only ever JOINS a spuriously broken line,
//     because its fault model is an inserted line break, not a lost one.
//     Note the CR byte itself is consumed: it became one byte of content
//     immediately before the embedded name, and dropping it restores the
//     original field exactly.
struct Hl7Resplit { bool ok = false; std::string first, second; };

/// Find a known segment name sitting at the tail of an interior field, which is
/// where a flipped '\r' leaves it.
inline Hl7Resplit hl7_try_resplit(const std::vector<std::string>& f) {
    Hl7Resplit out;
    for (std::size_t i = 1; i + 1 < f.size(); ++i) {
        if (f[i].size() < 4) continue;                     // need a byte + a name
        const std::string tail = f[i].substr(f[i].size() - 3);
        if (hl7_expected_arity(tail) < 0) continue;
        std::string first;
        for (std::size_t j = 0; j < i; ++j) { if (j) first += '|'; first += f[j]; }
        // -3 drops the embedded name, -1 more drops the byte that WAS the '\r'.
        first += '|' + f[i].substr(0, f[i].size() - 4);
        std::string second = tail;
        for (std::size_t j = i + 1; j < f.size(); ++j) second += '|' + f[j];
        out.ok = true; out.first = first; out.second = second;
        return out;
    }
    return out;
}

/// Name a segment from its shape when its own name is unreadable.
inline std::string hl7_type_from_shape(const std::vector<std::string>& f) {
    switch (f.size()) {
        case 12: return "MSH";
        case 14: return "PID";
        case 7:  return "OBX";
        case 3:
            if (f[1].rfind("patient.", 0) == 0 || f[1].rfind("observation.", 0) == 0)
                return "ZFX";
            return {};
        default: return {};
    }
}

/// Rebuild the segment list, resynchronising on segment headers when `repair`.
/// Without `repair` this is a plain split, so the baseline is untouched.
inline std::vector<std::string> hl7_resync_segments(const std::string& text, bool repair) {
    std::vector<std::string> raw = hl7_split(text, '\r');
    if (!repair) return raw;

    std::vector<std::string> out;
    out.reserve(raw.size());
    // Bounded: a line is re-split at most a few times, and each half is
    // re-examined once. No recursion on the halves' halves beyond this depth.
    std::vector<std::string> work(raw.rbegin(), raw.rend());
    int budget = static_cast<int>(raw.size()) * 4 + 64;
    while (!work.empty() && budget-- > 0) {
        std::string line = work.back();
        work.pop_back();
        if (line.size() < 4) { out.push_back(line); continue; }

        const auto f = hl7_split(line, '|');
        const int arity = hl7_expected_arity(f[0]);

        if (arity > 0 && static_cast<int>(f.size()) == arity) { out.push_back(line); continue; }

        // R2b: too many fields for a known type is the merged-line signature.
        if (arity > 0 && static_cast<int>(f.size()) > arity) {
            const auto sp = hl7_try_resplit(f);
            if (sp.ok) { work.push_back(sp.second); work.push_back(sp.first); continue; }
        }

        // R2a: unknown name -- let the shape name it.
        if (arity < 0) {
            const std::string guess = hl7_type_from_shape(f);
            if (!guess.empty()) { out.push_back(guess + line.substr(3)); continue; }
            const auto sp = hl7_try_resplit(f);
            if (sp.ok) { work.push_back(sp.second); work.push_back(sp.first); continue; }
        }
        out.push_back(line);   // unrepairable; REC-1's arity check drops it below
    }
    for (auto it = work.rbegin(); it != work.rend(); ++it) out.push_back(*it);
    return out;
}

// `repair` is REC-3: when set, a ZFX payload that will not parse is handed to
// bench::json_repair before it is given up on. OFF for the baseline and for the
// no-recovery read, ON only from recover_stream() -- that separation is what
// makes the two curves mean different things for this arm.
inline StreamFingerprint scan_v2_canonical(const std::vector<uint8_t>& wire,
                                           bool repair = false,
                                           bench::json_repair::Stats* rstats = nullptr) {
    StreamFingerprint fp;
    const std::string text(wire.begin(), wire.end());
    std::string patient_key, obs_key;

    // OBX IS DECODED, which requires deferring it.
    //
    // A message is MSH, PID, every OBX, then the Z-segments -- Z last is the
    // convention -- so an OBX is read before the ZFX that names the observation
    // it belongs to. The OBX rows are therefore buffered and resolved at the end
    // of the message, matching OBX-1 (set id, 1-based) to the Nth observation.
    //
    // Only observations with no ZFX value[x] are decoded from OBX: the encoder
    // emits one carrier per datum, OBX for a Quantity and ZFX for the datatypes
    // OBX cannot hold. Decoding both would double-count, and would let a blasted
    // OBX resurrect from the passthrough.
    struct PendingObx { std::string sub_id, v5, v6; };
    std::vector<PendingObx> pending_obx;
    std::set<std::string> obs_with_zfx_value;      // had a ZFX value[x]

    // REC-1c: the `sub`s already seen in the CURRENT observation scope. The
    // encoder emits each at most once per observation -- measured on the clean
    // wire, 1,468 scopes and 0 repeated (scope, sub) pairs -- so a repeat means
    // an `observation.id` boundary was missed and this field belongs to a
    // resource whose id was never read.
    std::set<std::string> obs_subs_seen;

    const auto flush_obx = [&]() {
        for (const auto& o : pending_obx) {
            // OBX-4 NAMES THE OWNER. Matching by OBX-1's ordinal position
            // instead made attribution global: a single damaged observation id
            // shifted every later row onto the wrong resource, so values did
            // not go missing, they MOVED -- 571 changed leaves at 64 flips,
            // units migrating between results. Keyed by id, a damaged row
            // costs that row.
            if (o.sub_id.empty()) continue;
            const std::string key = resource_key("Observation", o.sub_id);
            if (obs_with_zfx_value.count(key)) continue;
            if (o.v5.empty()) continue;
            // OBX-5 is the number; OBX-6 is a CWE of units, code^unit^system.
            try {
                fp.add_leaf(key + ".valueQuantity.value",
                            nlohmann::json(std::stod(o.v5)).dump());
            } catch (const std::exception&) { continue; }
            const auto comp = hl7_split(o.v6, '^');
            if (comp.size() > 0 && !comp[0].empty())
                fp.add_leaf(key + ".valueQuantity.code", safe_dump(nlohmann::json(comp[0])));
            if (comp.size() > 1 && !comp[1].empty())
                fp.add_leaf(key + ".valueQuantity.unit", safe_dump(nlohmann::json(comp[1])));
            if (comp.size() > 2 && !comp[2].empty())
                fp.add_leaf(key + ".valueQuantity.system", safe_dump(nlohmann::json(comp[2])));
        }
        pending_obx.clear();
        obs_with_zfx_value.clear();
    };

    for (const auto& seg : hl7_resync_segments(text, repair)) {
        if (seg.size() < 4) continue;
        const auto f = hl7_split(seg, '|');
        const std::string& name = f[0];

        // REC-1: a wrong field count means the indices below are shifted.
        const int expected_arity = hl7_expected_arity(name);
        if (expected_arity > 0 && static_cast<int>(f.size()) != expected_arity)
            continue;

        if (name == "MSH" && f.size() > 9) {
            flush_obx();                      // resolve the previous message
            // A new message starts a new observation scope. Without this the
            // last observation of message N stays current into message N+1
            // until its first `observation.id` is read -- and if that id is
            // damaged, the leak crosses a message boundary.
            obs_key.clear();
            obs_subs_seen.clear();
            patient_key = resource_key("Patient", f[9]);
            // MSH-10 carries the patient id, and it is the ONLY place this arm
            // writes it. Using it to key the resource but never emitting it as
            // a leaf lost `Patient.id` for every patient in the corpus.
            if (!f[9].empty())
                fp.add_leaf(patient_key + ".id", safe_dump(nlohmann::json(f[9])));
        } else if (name == "PID" && f.size() > 8) {
            // birthDate and gender exist ONLY here; name/address/telecom are
            // carried structurally by ZFX `.details` and would double-count.
            if (!patient_key.empty()) {
                // PID-7 is a v2 DT: YYYYMMDD, no separators. Emitting it raw
                // compared "19510216" against the document's "1951-02-16" --
                // the same date, spelled the way v2 spells it. Converting on
                // the way back is exactly what a v2 reader does.
                if (!f[7].empty()) {
                    std::string d = f[7];
                    if (d.size() >= 8 && d.find('-') == std::string::npos)
                        d = d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
                    fp.add_leaf(patient_key + ".birthDate", safe_dump(nlohmann::json(d)));
                }
                // PID-8 is a v2 sex code (M/F/O/U); FHIR's ValueSet spells them
                // out. sex_code() wrote the v2 form during Test 1, so the
                // inverse belongs here -- the raw letter matches no FHIR code
                // and was refused by the POCO setter for every patient.
                if (!f[8].empty()) {
                    const std::string &sx = f[8];
                    const char *g = sx == "M" ? "male"
                                  : sx == "F" ? "female"
                                  : sx == "O" ? "other"
                                              : "unknown";
                    fp.add_leaf(patient_key + ".gender", safe_dump(nlohmann::json(g)));
                }
            }
        } else if (name == "OBX") {
            if (f.size() > 6)
                pending_obx.push_back({hl7_unescape(f[4]), hl7_unescape(f[5]),
                                       hl7_unescape(f[6])});
            (void)0;
            // Buffered above, resolved by flush_obx() at the end of the
            // message. An earlier version emitted `<Observation>.value`, which
            // is not a FHIR path and so matched nothing in POCO 1; the fix then
            // was to emit no leaf at all, which left OBX unmeasured and made
            // its 14,516 delimiter bytes free armor -- a flipped '|' inside an
            // OBX left the digest bit-identical. The canonical path is
            // `.valueQuantity.value`, and that is what flush_obx emits.
        } else if (name == "ZFX" && f.size() > 2) {
            const std::string field = hl7_unescape(f[1]);
            const std::string payload = hl7_unescape(f[2]);

            std::string key;
            std::string sub;
            bool is_observation = false;
            if (field.rfind("patient.", 0) == 0) {
                key = patient_key;
                sub = field.substr(8);
            } else if (field.rfind("observation.", 0) == 0) {
                key = obs_key;
                sub = field.substr(12);
                is_observation = true;
            } else {
                continue;
            }

            nlohmann::json payload_json;
            bool parsed = true;
            try { payload_json = nlohmann::json::parse(payload); }
            catch (const std::exception&) { parsed = false; }

            // REC-3: this is the 82.4% surface. A ZFX payload is a small JSON
            // fragment carrying one FHIR element, so a single flip usually
            // costs exactly this field -- and structural repair usually gets it
            // back. Structure only; nothing here invents content.
            if (!parsed && repair)
                parsed = bench::json_repair::repair(payload, payload_json, rstats);

            // REC-1b: AN OBSERVATION SCOPE WE CANNOT NAME MUST BE CLEARED, NOT
            // INHERITED.
            //
            // `observation.id` opens a scope that every following
            // `observation.*` ZFX attaches to. When its payload is damaged, the
            // old code took one of two paths -- `continue` on a parse throw, or
            // fall through with a non-string payload -- and BOTH left `obs_key`
            // holding the PREVIOUS observation's key. Every subsequent field
            // then attached to the wrong resource: the values do not go
            // missing, they MOVE, which is the same failure the OBX-4 keying
            // change was made to eliminate (see ObxSegment in
            // hl7v2_message.hpp). The ZFX path still had it.
            //
            // Clearing the scope makes the loss honest: `key.empty()` below
            // drops the orphaned fields, so a damaged id costs its own
            // observation instead of silently corrupting the previous one.
            if (is_observation && sub == "id" && (!parsed || !payload_json.is_string())) {
                obs_key.clear();
                obs_subs_seen.clear();
                continue;
            }
            if (!parsed) continue;

            if (sub == "id") {
                // Starts a new scope AND is a leaf in its own right.
                if (is_observation) {
                    obs_key = resource_key("Observation", payload_json.get<std::string>());
                    obs_subs_seen.clear();      // a scope legitimately opens here
                }
                key = is_observation ? obs_key : patient_key;
                if (!key.empty())
                    fp.add_leaf(key + ".id", safe_dump(payload_json));
                continue;
            }
            if (key.empty()) continue;

            // REC-1c: A REPEATED `sub` MEANS A MISSED SCOPE BOUNDARY.
            //
            // REC-1b covers a damaged id PAYLOAD. If the field NAME is damaged
            // instead (`observation.id` -> `observatioX.id`) the branch above
            // falls through its trailing `else { continue; }`, the scope never
            // opens, and this field attaches to the PREVIOUS observation -- the
            // same misattribution reached a different way. Nothing in the id
            // path can see that, because the id was never recognised as one.
            //
            // The duplicate is the tell, and it is unambiguous: on the clean
            // wire the encoder emits each `sub` at most once per scope. Checked
            // on the RAW sub, before the .details / [*] / [x] trimming below,
            // because that is the form the 0-repeat measurement used.
            if (is_observation) {
                if (!obs_subs_seen.insert(sub).second) {
                    obs_key.clear();
                    obs_subs_seen.clear();
                    continue;
                }
            }

            // An observation whose value came through ZFX must not ALSO be
            // decoded from its OBX -- one carrier per datum.
            if (sub == "value[x]") obs_with_zfx_value.insert(key);

            // `.details` / `[*]` are container markers, not path elements.
            if (sub.size() > 8 && sub.compare(sub.size() - 8, 8, ".details") == 0)
                sub.erase(sub.size() - 8);
            if (sub.size() > 3 && sub.compare(sub.size() - 3, 3, "[*]") == 0)
                sub.erase(sub.size() - 3);

            // A CHOICE ELEMENT. The ZFX name is the `[x]` BASE --
            // `observation.effective[x]` -- and the concrete element name only
            // exists in the payload, which the encoder writes as a single-key
            // object: {"effectiveDateTime": ...}. Walking the base would emit
            // `Observation/<id>.effective[x].effectiveDateTime`, which matches
            // no canonical leaf, so every choice field on this arm was refused.
            if (sub.size() > 3 && sub.compare(sub.size() - 3, 3, "[x]") == 0) {
                if (payload_json.is_object() && payload_json.size() == 1) {
                    hl7_walk_json(payload_json.begin().value(),
                                  key + "." + payload_json.begin().key(), fp);
                    continue;
                }
                sub.erase(sub.size() - 3);
            }
            hl7_walk_json(payload_json, key + "." + sub, fp);
        }
    }
    flush_obx();          // the last message has no following MSH
    fp.finalize();
    return fp;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    return scan_v2_canonical(wire);
}

// ---------------------------------------------------------------------------
// DAMAGE MODEL -- structural positions eligible for a bit flip
// ---------------------------------------------------------------------------
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  // v2's syntactic elements -- every byte whose corruption changes the SHAPE or
  // IDENTITY of data this arm reads, and no byte that merely holds a value.
  //
  //  1. segment terminators (\r) and the 3-char segment name. A name cannot be
  //     repaired: nothing in-band cross-validates it.
  //  2. the encoding characters | ^ & ~ \ -- field, component, subcomponent,
  //     repetition and escape separators. v2 has no in-string quoting, so a
  //     delimiter is syntax wherever it sits.
  //  3. inside a ZFX segment: the FHIR path in field 1, and the JSON
  //     punctuation in field 2.
  //
  // (3) is what this model was missing, and it is where the data is. 91.5% of
  // this wire is ZFX payload, and a ZFX payload is JSON -- its braces, brackets,
  // quotes, colons and commas are exactly as structural as a pipe. Treating
  // them as content left the arm's own carrier untouched by every run. The
  // field-1 path is identity for the same reason a segment name is: a flipped
  // path does not fail, it addresses something else.
  //
  // Bytes inside a JSON string literal stay content, which is why the scan
  // below tracks quoting rather than matching punctuation blindly.
  //
  // OBX interiors ARE eligible. They were excluded while this arm emitted no
  // leaf from an OBX -- a position that cannot change the fingerprint is free
  // armor, and 14,516 of the 108,270 positions were exactly that. The fix was
  // to decode OBX rather than to stop damaging it: OBX-5/-6 now carry the whole
  // Quantity and flush_obx() reads them back, so a flipped '|' there costs real
  // units. That is the `OBX|some|data|` -> `OBX|someLdata|` case: the fields
  // merge, OBX-5 becomes something else and OBX-6 disappears.
  std::vector<std::size_t> positions;
  std::size_t seg_start = 0;
  while (seg_start < wire.size()) {
    std::size_t seg_end = seg_start;
    while (seg_end < wire.size() && wire[seg_end] != '\r') ++seg_end;

    for (std::size_t j = 0; j < 3 && seg_start + j < seg_end; ++j)
      positions.push_back(seg_start + j);
    if (seg_end < wire.size()) positions.push_back(seg_end);

    const bool is_zfx = seg_end - seg_start >= 3 && wire[seg_start] == 'Z' &&
                        wire[seg_start + 1] == 'F' && wire[seg_start + 2] == 'X';

    {
      // Field boundaries within this segment.
      std::size_t bar1 = seg_end, bar2 = seg_end;
      for (std::size_t i = seg_start; i < seg_end; ++i) {
        if (wire[i] == '|' || wire[i] == '^' || wire[i] == '&' || wire[i] == '~' ||
            wire[i] == '\\')
          positions.push_back(i);
        if (wire[i] == '|') {
          if (bar1 == seg_end) bar1 = i;
          else if (bar2 == seg_end) bar2 = i;
        }
      }
      if (is_zfx && bar1 < seg_end) {
        // Field 1: the FHIR path. Identity -- every byte counts.
        const std::size_t path_end = (bar2 < seg_end) ? bar2 : seg_end;
        for (std::size_t i = bar1 + 1; i < path_end; ++i) positions.push_back(i);

        // Field 2: JSON. Punctuation outside string literals is structure.
        if (bar2 < seg_end) {
          bool in_str = false, esc = false;
          for (std::size_t i = bar2 + 1; i < seg_end; ++i) {
            const char c = static_cast<char>(wire[i]);
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { in_str = !in_str; positions.push_back(i); continue; }
            if (in_str) continue;
            if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
              positions.push_back(i);
          }
        }
      }
    }
    seg_start = seg_end + 1;
  }

  // One position per byte. The two passes this replaced pushed segment names
  // and delimiters independently, so a byte could appear twice and be selected
  // twice -- and two flips of one bit is no flip at all, which silently made
  // some runs less damaged than the k they reported.
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  return positions;
}

inline std::vector<uint8_t> corrupt_stream(const std::vector<uint8_t>& wire,
                                           std::size_t k, unsigned seed) {
  return flip_positions(wire, structural_positions(wire), k, seed);
}

// ---------------------------------------------------------------------------
// RECOVERY -- recovery-ON read of a damaged wire
// ---------------------------------------------------------------------------
// The baseline walk plus REC-3 payload repair. Where the damage lands decides
// what is recoverable, and the shares are measured (k=2048):
//
//   82.4%  inside a ZFX payload  -> JSON structural repair, often recovered
//    7.6%  a 3-char segment name -> unrecoverable; nothing cross-validates it
//    7.5%  a delimiter | ^ & ~ \ -> frame shift, detected by arity (REC-1) and
//                                   dropped, because v2 has no second witness
//                                   that could say which boundary moved
//    2.5%  a segment terminator  -> two segments merge; both are lost
//
// ⚠ An earlier version of this comment claimed a destroyed \r was recoverable
// "because the merged line still contains the next segment's header". It is
// not: hl7_split(text,'\r') splits only on '\r', so the second segment's
// header sits mid-line and is never read. Re-splitting on an embedded header
// is REC-2 and is not implemented.
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
    return scan_v2_canonical(wire, /*repair=*/true);
}

#endif  // ARM_HL7V2_CODEC_HPP
