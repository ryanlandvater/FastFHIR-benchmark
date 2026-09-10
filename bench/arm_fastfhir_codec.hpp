/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// FastFHIR codec for Instrument G test 5: canonical scan, the damage model's
// structural-position map, and recovery.
//
// Split out of bench_test_5.hpp on 2026-09-08 with the other three arms.
//
// WHAT LIVES HERE
//   ffhr_walk_leaves() / ffhr_collect()   Node-lens walk to canonical leaves
//   structural_positions()                FF_HEADER, VALIDATION/RECOVERY_TAG
//                                         headers, pointer-slot witnesses
//   recover_stream()                      the only arm that REPAIRS rather than
//                                         resynchronises: FF_Recovery::recover()
//                                         diagnoses, apply() rewrites into a
//                                         copy, and the read happens after
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {`. Not self-contained by design -- it uses StreamFingerprint,
// resource_key, safe_dump, content_hash and flip_positions from the harness.
// Do not include it directly.
#ifndef ARM_FASTFHIR_CODEC_HPP
#define ARM_FASTFHIR_CODEC_HPP

// ---------------------------------------------------------------------------
// READER -- canonical scan
// ---------------------------------------------------------------------------
// THE FORWARD MAPPING, USED BACKWARDS.
//
// A choice ([x]) element is written as base + variant type name --
// `value` + `Quantity` -> `valueQuantity` -- and the EXPORTER already knows how
// to build that (FF_Parser.cpp's choice_suffix, which defers complex variants to
// reflected_choice_suffix). Mirroring it here is what lets the lens name a leaf
// the same way the JSON document does; deriving the rule independently is how
// the two arms end up 10,753 paths apart while holding identical data.
//
// reflected_resource_type is deliberately NOT the lookup: it enumerates
// resources only and returns "" for Quantity, CodeableConcept, Period and every
// other datatype -- the exporter records that it printed a bare `value` for
// 1,416 fields that way.
inline std::string bench_choice_suffix(RECOVERY_TAG tag) {
    switch (tag) {
        case RECOVER_FF_BOOL:     return "Boolean";
        case RECOVER_FF_INT32:    return "Integer";
        case RECOVER_FF_FLOAT64:  return "Decimal";
        case RECOVER_FF_STRING:   return "String";
        case RECOVER_FF_CODE:     return "Code";
        case RECOVER_FF_DATE:     return "Date";
        case RECOVER_FF_DATETIME: return "DateTime";
        case RECOVER_FF_TIME:     return "Time";
        case RECOVER_FF_INSTANT:  return "Instant";
        default:                  return std::string(FastFHIR::reflected_choice_suffix(tag));
    }
}

// The lens walk, emitting the SAME canonical paths the JSON arm does.
//
// This replaces a block-REFERENCE fingerprint (Recovery::reachable_blocks): the
// two counted different populations -- 54,504 edges against JSON's 1,473
// resources -- so a percentage over either meant nothing next to the other.
// Both now count FHIR leaves, which is the thing the formats are actually being
// asked to preserve.
//
// PER LEAF, not per document: print_json() over the whole tree would make one
// broken subtree unparseable and score every surviving value as lost, which
// flatters the failure. Walking leaf by leaf keeps the damage local, which is
// the honest accounting.
inline void ffhr_walk_leaves(const Reflective::Node& node, const std::string& path,
                             StreamFingerprint& fp, uint32_t version, int depth) {
    if (!node || depth > 64) return;

    if (node.is_array()) {
        std::size_t n = 0;
        try { n = node.size(); } catch (const std::exception&) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            try {
                ffhr_walk_leaves(node[i], path + "[" + std::to_string(i) + "]", fp, version,
                                 depth + 1);
            } catch (const std::exception&) {
                // this element is unreadable; the rest of the array is not
            }
        }
        return;
    }

    if (!node.is_object()) {
        std::ostringstream v;
        node.print_json(v);
        fp.add_leaf(path, v.str());
        return;
    }

    std::span<const FF_FieldInfo> fields;
    try { fields = node.fields(); } catch (const std::exception&) { return; }
    for (const auto& f : fields) {
        const FF_FieldKey key = FF_FieldKey::from_cstr(node.recovery(), f.kind, f.field_offset,
                                                       f.child_recovery,
                                                       f.array_entries_are_offsets, f.name);
        const Reflective::Entry e = node[key];
        if (!e) continue;
        std::string child = path + "." + std::string(f.name);
        if (f.kind == FF_FIELD_CHOICE) {
            try { child += bench_choice_suffix(e.as_node().recovery()); }
            catch (const std::exception&) {}
        }
        if (ff_kind_is_inline_scalar(f.kind)) {
            std::ostringstream v;
            try { e.print_scalar_json(v, version); } catch (const std::exception&) { continue; }
            fp.add_leaf(child, v.str());
            continue;
        }
        try { ffhr_walk_leaves(e.as_node(), child, fp, version, depth + 1); }
        catch (const std::exception&) {}
    }
}

inline void ffhr_collect(const Reflective::Node& root, StreamFingerprint& fp,
                         uint32_t version) {
    if (!root) return;
    std::vector<Reflective::Node> entries;
    try { entries = root[FastFHIR::Fields::BUNDLE::ENTRY].as_node().entries(); }
    catch (const std::exception&) { return; }

    for (const auto& entry : entries) {
        Reflective::Node resource;
        try { resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE].as_node(); }
        catch (const std::exception&) { continue; }
        if (!resource) continue;

        // The type comes from the block's own tag (FastFHIR does not store
        // resourceType as a field), the id from the wire.
        const std::string type(reflected_resource_type(resource.recovery()));
        std::span<const FF_FieldInfo> fields;
        try { fields = resource.fields(); } catch (const std::exception&) { continue; }

        // `id` is found through the reflection table, not a type-specific key:
        // Fields::PATIENT::ID names Patient's slot, so using it for every
        // resource left every Observation keyless and silently skipped -- 437
        // leaves against the JSON arm's 34,831.
        std::string id;
        for (const auto& f : fields) {
            if (std::string_view(f.name) != "id") continue;
            const FF_FieldKey idk = FF_FieldKey::from_cstr(resource.recovery(), f.kind,
                                                           f.field_offset, f.child_recovery,
                                                           f.array_entries_are_offsets, f.name);
            try {
                const Reflective::Entry e = resource[idk];
                if (e) id = std::string(e.as<std::string_view>());
            } catch (const std::exception&) {}
            break;
        }
        const std::string key = resource_key(type, id);
        if (key.empty()) continue;
        for (const auto& f : fields) {
            const FF_FieldKey fk = FF_FieldKey::from_cstr(resource.recovery(), f.kind,
                                                          f.field_offset, f.child_recovery,
                                                          f.array_entries_are_offsets, f.name);
            const Reflective::Entry e = resource[fk];
            if (!e) continue;
            std::string child = key + "." + std::string(f.name);
            if (f.kind == FF_FIELD_CHOICE) {
                try { child += bench_choice_suffix(e.as_node().recovery()); }
                catch (const std::exception&) {}
            }
            if (ff_kind_is_inline_scalar(f.kind)) {
                std::ostringstream v;
                try { e.print_scalar_json(v, version); } catch (const std::exception&) { continue; }
                fp.add_leaf(child, v.str());
                continue;
            }
            try { ffhr_walk_leaves(e.as_node(), child, fp, version, 1); }
            catch (const std::exception&) {}
        }
    }
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    FastFHIR::Memory mem = wrap_wire_bytes(wire);
    FastFHIR::Parser parser(mem);
    ffhr_collect(parser.root(), fp, FHIR_VERSION_R5);
    fp.finalize();
    return fp;
}

// ---------------------------------------------------------------------------
// DAMAGE MODEL -- structural positions eligible for a bit flip
// ---------------------------------------------------------------------------
// Corruption targets = the structural witnesses of the LIVE-EDGE census, the
// same model FastFHIR's own recovery tests use (test_recovery.cpp picks a ref
// from a clean recover() and flips base[child] / base[slot]): the stream
// FF_HEADER region, then for every parent→child block reference from
// Recovery::reachable_blocks() -- the child's 10-byte header (VALIDATION +
// RECOVERY_TAG) and the parent's pointer slot (8 bytes, or the 10-byte
// {offset, tag} tuple a choice/resource slot stores). Scalar VALUES (string
// payloads, numbers, codes) are NEVER corrupted. Leaf-data slots (string/code
// references) stay excluded: recovery does not cross-validate them and a
// broken leaf reference has no witness to heal it (70feda9 design note).
inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  std::vector<std::size_t> positions;
  // 1. FF_HEADER region (stream-level syntax).
  for (std::size_t i = 0; i < 54 && i < wire.size(); ++i)
    positions.push_back(i);
  FastFHIR::Memory mem = wrap_wire_bytes(wire);
  FastFHIR::Recovery rec(mem);
  const auto refs = rec.reachable_blocks();
  for (const auto& r : refs) {
    const bool tuple = r.kind == FF_FIELD_CHOICE || r.kind == FF_FIELD_RESOURCE;
    if (!(r.kind == FF_FIELD_BLOCK || r.kind == FF_FIELD_ARRAY || tuple))
      continue;
    // 2. The parent's slot naming the child (slot + tuple tag half).
    const std::size_t slot = static_cast<std::size_t>(r.parent + r.field);
    const std::size_t slot_len = tuple ? 10 : 8;
    for (std::size_t j = 0; j < slot_len && slot + j < wire.size(); ++j)
      positions.push_back(slot + j);
    // 3. The child's 10-byte block header (VALIDATION + RECOVERY_TAG).
    const std::size_t child = static_cast<std::size_t>(r.child);
    for (std::size_t j = 0; j < 10 && child + j < wire.size(); ++j)
      positions.push_back(child + j);
  }
  // Overlapping witnesses (an array element that is itself a header target)
  // must flip once: a double-XOR would silently cancel.
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
inline StreamFingerprint recover_stream(const std::vector<uint8_t>& wire) {
    StreamFingerprint fp;
    FastFHIR::Memory mem = wrap_wire_bytes(wire);
    FastFHIR::Recovery rec(mem);
    const auto rep = rec.recover();

    // APPLY BEFORE READING. recover() only DIAGNOSES; apply() is the only
    // mutating entry point and it writes into a copy. Reading the damaged bytes
    // here would score every correctly-repaired value as wrong -- reporting the
    // damage the engine had just fixed. A failed apply leaves the copy as-is,
    // so this never reads better than the engine actually achieved.
    std::vector<BYTE> repaired;
    rec.apply(rep, repaired);
    const std::vector<uint8_t>& src =
        repaired.empty() ? wire : reinterpret_cast<const std::vector<uint8_t>&>(repaired);

    FastFHIR::Memory fixed = wrap_wire_bytes(src);
    FastFHIR::Parser parser(fixed);
    ffhr_collect(parser.root(), fp, FHIR_VERSION_R5);
    fp.finalize();
    return fp;
}

#endif  // ARM_FASTFHIR_CODEC_HPP
