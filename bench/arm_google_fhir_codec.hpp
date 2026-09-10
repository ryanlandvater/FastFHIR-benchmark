/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Google FHIR protobuf codec for Instrument G test 5: canonical scan, the
// damage model's structural-position map, and recovery.
//
// Split out of bench_test_5.hpp on 2026-09-08 with the other three arms. The
// harness owns the fingerprint, the four-outcome census and the corruption
// driver; each codec owns its format.
//
// WHAT LIVES HERE
//   pb_walk() and friends   descriptor-driven walk to canonical FHIR leaves,
//                           including the FhirProto conventions (choice oneof
//                           folding, typed Reference collapse, timelike
//                           value_us/timezone/precision, Decimal-as-number)
//   structural_positions()  TLV record headers
//   recover_stream()        TLV record resync: a 'P'/'O' type byte with a
//                           plausible length prefix, rescanning forward when
//                           the length is implausible
//
// Included from inside `namespace bench::test_5 { inline namespace
// BENCH_ARM_NS {`. Not self-contained by design -- it uses StreamFingerprint,
// resource_key, safe_dump, content_hash and flip_positions from the harness.
// Do not include it directly.
#ifndef ARM_GOOGLE_FHIR_CODEC_HPP
#define ARM_GOOGLE_FHIR_CODEC_HPP

// ---------------------------------------------------------------------------
// READER -- canonical scan
// ---------------------------------------------------------------------------
// FIELD-LEVEL PROTOBUF, via reflection.
//
// The arm used to emit ONE unit per record and call it recovered whenever
// ParseFromArray() returned true. That answers "is this still valid protobuf",
// never "is this still the same data": protobuf's wire format is permissive,
// so a record with scrambled field bytes reparses happily and scored as fully
// recovered. It also made the arm's units 1,473 whole resources against
// FastFHIR's 44,130 references -- damage per unit differed by orders of
// magnitude, so the percentages were never comparable.
//
// Reflection walks whatever the descriptors say is populated, so this needs no
// per-resource code and cannot drift from the schema. One unit per LEAF value:
//   parent  = the record's byte offset (which record it belongs to)
//   offset  = a hash of the field PATH (stable identity inside the record)
//   tag     = the field number
//   content = a hash of the value itself
inline std::uint64_t pb_path_hash(const std::string& path) {
  return content_hash(reinterpret_cast<const std::uint8_t*>(path.data()), path.size());
}

// JSON-RENDERED, like every other arm. The canonical value is the JSON form
// (a string arrives quoted and escaped), and returning a bare string here made
// 16,213 matched paths compare unequal on every single one -- the arms agreed
// on WHERE the datum was and disagreed only on how to spell it.
// proto generation lowercases FHIR's camelCase into snake_case
// (multipleBirth -> multiple_birth), so the inverse is the mapping that lets
// this arm name an element the way the document does. Without it every
// multi-word field lands as a path JSON does not have -- counted spurious on
// one side and missing on the other, from data that is present and correct.
inline std::string pb_camel(const std::string& snake) {
    std::string out;
    out.reserve(snake.size());
    bool up = false;
    for (const char c : snake) {
        if (c == '_') { up = true; continue; }
        out.push_back(up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        up = false;
    }
    return out;
}

inline std::string pb_json_value(const std::string& raw, bool is_string) {
    return is_string ? safe_dump(nlohmann::json(raw)) : raw;
}

inline std::string pb_scalar_string(const google::protobuf::Message& m,
                                    const google::protobuf::Reflection* refl,
                                    const google::protobuf::FieldDescriptor* f,
                                    int index) {
  using FD = google::protobuf::FieldDescriptor;
  const bool rep = f->is_repeated();
  switch (f->cpp_type()) {
    case FD::CPPTYPE_INT32:  return std::to_string(rep ? refl->GetRepeatedInt32(m, f, index)  : refl->GetInt32(m, f));
    case FD::CPPTYPE_INT64:  return std::to_string(rep ? refl->GetRepeatedInt64(m, f, index)  : refl->GetInt64(m, f));
    case FD::CPPTYPE_UINT32: return std::to_string(rep ? refl->GetRepeatedUInt32(m, f, index) : refl->GetUInt32(m, f));
    case FD::CPPTYPE_UINT64: return std::to_string(rep ? refl->GetRepeatedUInt64(m, f, index) : refl->GetUInt64(m, f));
    case FD::CPPTYPE_DOUBLE: return std::to_string(rep ? refl->GetRepeatedDouble(m, f, index) : refl->GetDouble(m, f));
    case FD::CPPTYPE_FLOAT:  return std::to_string(rep ? refl->GetRepeatedFloat(m, f, index)  : refl->GetFloat(m, f));
    // "true"/"false", not "1"/"0" -- the same spelling problem as the enum
    // case below. JSON has a boolean literal and every other arm emits it, so
    // "0" arrived as the NUMBER 0 and the POCO setter refused it: five
    // multipleBirthBoolean leaves, present and correct on the wire, counted as
    // missing.
    case FD::CPPTYPE_BOOL:   return (rep ? refl->GetRepeatedBool(m, f, index) : refl->GetBool(m, f)) ? "true" : "false";
    case FD::CPPTYPE_ENUM: {
      // proto generation upper-snakes the FHIR code (final -> FINAL,
      // not-done -> NOT_DONE), so the inverse is lowercase with underscores
      // back to hyphens. Emitting the proto spelling made this arm report
      // "FINAL" where every other arm reports "final" -- 4,414 leaves that are
      // the SAME datum, counted as a difference between the formats.
      const auto* e = rep ? refl->GetRepeatedEnum(m, f, index) : refl->GetEnum(m, f);
      if (e == nullptr) return std::string();
      std::string code = e->name();
      for (char& c : code) {
        c = (c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return code;
    }
    case FD::CPPTYPE_STRING: {
      std::string scratch;
      return rep ? refl->GetRepeatedStringReference(m, f, index, &scratch)
                 : refl->GetStringReference(m, f, &scratch);
    }
    default: return std::string();
  }
}

// google-fhir WRAPS PRIMITIVES: `Patient.id` is a message `Id { value }`, and
// the encoder writes it as mutable_id()->set_value(...). So a proto path is one
// level deeper than the FHIR element path everywhere. Collapsing a wrapper --
// a message whose populated content is a single `value` -- is what makes this
// arm name a leaf the way the JSON document does.
// Recognised from the DESCRIPTOR, not from which fields happen to be set.
//
// ListFields omits any proto3 scalar sitting at its default, so a
// Boolean{value:false} lists NOTHING and the old shape test (`exactly one field
// set, named value`) rejected it -- the message was then walked as an ordinary
// submessage, found empty, and emitted no leaf at all. Every `false`, every 0,
// every "" inside a FhirProto primitive wrapper disappeared; in this corpus
// that was multipleBirthBoolean for all five patients.
//
// It is the same mistake as reading FastFHIR's enum ordinal 0 as absence.
// Presence is carried by the WRAPPER MESSAGE existing at all -- the parent's
// ListFields is what decides that -- and the scalar inside is then just a
// value, default or not.
inline bool pb_is_primitive_wrapper(const google::protobuf::Message& m,
                                    const google::protobuf::FieldDescriptor** out) {
    const auto* f = m.GetDescriptor()->FindFieldByName("value");
    if (f == nullptr || f->is_repeated()) return false;
    // A scalar `value` is what makes it a wrapper: Quantity and ContactPoint
    // also have a `value`, but theirs is a message and they are real datatypes.
    if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) return false;
    *out = f;
    return true;
}

// FhirProto's JSON conventions, as its own printer implements them. There is
// no one-to-one mapping between FhirProto fields and FHIR JSON fields, so
// google/fhir ships custom parsers/printers rather than using protobuf's
// generic JSON support; a reflection walk that ignores those conventions names
// elements the FHIR document does not have. Two rules matter here, and both
// are readable off the descriptors, so neither can drift from the schema.

// Which member of a single-oneof message is set, if any.
inline const google::protobuf::FieldDescriptor* pb_oneof_set(
    const google::protobuf::Message& m) {
    const auto* d = m.GetDescriptor();
    if (d->real_oneof_decl_count() != 1) return nullptr;
    return m.GetReflection()->GetOneofFieldDescriptor(m, d->real_oneof_decl(0));
}

inline std::string pb_ucfirst(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

inline std::string pb_upper_camel(const std::string& snake) {
    return pb_ucfirst(pb_camel(snake));
}

// RULE 3 -- a timelike primitive. FhirProto stores dateTime/instant/date/time
// as absolute microseconds plus the timezone the event was recorded in and a
// precision, because FHIR JSON writes some timelike primitives without any
// zone at all. Reflection therefore sees `issued.valueUs`, `issued.timezone`
// and `issued.precision` where the document says
// `"issued": "2018-06-14T06:06:32.842+00:00"`. Precision is what says how much
// of the value was actually written -- rendering every one to seconds would
// invent a different string, and a different instant, for anything sub-second.
inline bool pb_timelike_text(const google::protobuf::Message& m, std::string* out) {
    const auto* d = m.GetDescriptor();
    const auto* f_us = d->FindFieldByName("value_us");
    const auto* f_tz = d->FindFieldByName("timezone");
    const auto* f_pr = d->FindFieldByName("precision");
    if (f_us == nullptr || f_tz == nullptr || f_pr == nullptr) return false;

    const auto* refl = m.GetReflection();
    const std::int64_t us = refl->GetInt64(m, f_us);
    std::string tz = refl->GetString(m, f_tz);
    const std::string precision = refl->GetEnum(m, f_pr)->name();

    // The zone is an OFFSET in FHIR JSON; "UTC" is FhirProto's spelling of +00:00.
    std::int64_t offset_s = 0;
    if (tz == "UTC" || tz.empty()) tz = "+00:00";
    if (tz != "Z" && tz.size() >= 6 && (tz[0] == '+' || tz[0] == '-')) {
        const int sign = tz[0] == '-' ? -1 : 1;
        offset_s = sign * (std::atoi(tz.substr(1, 2).c_str()) * 3600 +
                           std::atoi(tz.substr(4, 2).c_str()) * 60);
    }

    // Floor-divide: a negative microsecond count must not round toward zero.
    std::int64_t local = us + offset_s * 1000000LL;
    std::int64_t secs = local / 1000000LL;
    std::int64_t frac = local % 1000000LL;
    if (frac < 0) { frac += 1000000LL; --secs; }

    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    if (precision == "DAY" || precision == "YEAR" || precision == "MONTH") {
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        *out = buf;
        return true;
    }
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    *out = buf;
    if (precision == "MILLISECOND") {
        std::snprintf(buf, sizeof buf, ".%03d", static_cast<int>(frac / 1000));
        *out += buf;
    } else if (precision == "MICROSECOND") {
        std::snprintf(buf, sizeof buf, ".%06d", static_cast<int>(frac));
        *out += buf;
    }
    *out += tz;
    return true;
}

// RULE 4 -- Decimal is a JSON NUMBER. FhirProto stores it as a `string` field
// precisely so the original lexical form survives (FHIR treats trailing zeros
// as significant), but FHIR JSON writes it unquoted. Emitting it quoted, the
// way every other string-typed wrapper is emitted, made 1,263 Quantity values
// arrive as "93" where the document has 93 -- refused by the POCO setter, and
// counted as a missing value rather than a mis-spelled one.
inline bool pb_is_unquoted_primitive(const google::protobuf::Message& m) {
    const std::string& n = m.GetDescriptor()->name();
    return n == "Decimal";
}

// RULE 1 -- a choice element. `Observation.value[x]` is a nested message ValueX
// wrapping one oneof, so reflection sees `value.quantity.value`. FHIR JSON has
// no wrapper: the element is spelled `valueQuantity`. json_name carries the
// FHIR datatype where the proto field name differs -- `string_value` is
// declared [json_name = "string"], giving `valueString`, not `valueStringValue`.
inline bool pb_is_choice_wrapper(const google::protobuf::Message& m) {
    const auto* d = m.GetDescriptor();
    const std::string& n = d->name();
    return d->real_oneof_decl_count() == 1 && n.size() > 1 && n.back() == 'X' &&
           n != "Reference";
}

// RULE 2 -- a typed reference. FhirProto splits FHIR's single `reference`
// string into a oneof of per-resource ReferenceId fields, so `subject` arrives
// as `subject.patientId.value = "abc"` where the document says
// `subject.reference = "Patient/abc"`. The resource type is the field name
// minus its `_id` suffix, which is exactly what the field's
// (referenced_fhir_type) annotation states -- read here off the name so this
// needs no annotation-extension linkage.
inline bool pb_reference_text(const google::protobuf::Message& ref,
                              std::string* out) {
    if (ref.GetDescriptor()->name() != "Reference") return false;
    const auto* f = pb_oneof_set(ref);
    if (f == nullptr) return false;
    const auto* inner = f->message_type() != nullptr ? ref.GetReflection()
                            ->GetMessage(ref, f).GetDescriptor()->FindFieldByName("value")
                                                    : nullptr;
    if (inner == nullptr) return false;
    const auto& sub = ref.GetReflection()->GetMessage(ref, f);
    const std::string id = sub.GetReflection()->GetString(sub, inner);
    const std::string& name = f->name();
    if (name == "uri" || name == "fragment") { *out = id; return true; }
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "_id") == 0) {
        *out = pb_upper_camel(name.substr(0, name.size() - 3)) + "/" + id;
        return true;
    }
    return false;
}

inline void pb_walk(const google::protobuf::Message& msg, const std::string& path,
                    StreamFingerprint& fp) {
    const auto* refl = msg.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    refl->ListFields(msg, &fields);
    const bool in_reference = msg.GetDescriptor()->name() == "Reference";
    for (const auto* f : fields) {
        // Already emitted as `.reference` by RULE 2; walking it again would
        // add `subject.patientId.value`, a path no FHIR document has.
        if (in_reference && f->containing_oneof() != nullptr) continue;
        const std::string base = path + "." + pb_camel(f->name());
        const bool is_msg =
            f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
        const int count = f->is_repeated() ? refl->FieldSize(msg, f) : 1;
        for (int i = 0; i < count; ++i) {
            const std::string here =
                f->is_repeated() ? base + "[" + std::to_string(i) + "]" : base;
            if (is_msg) {
                const auto& sub = f->is_repeated() ? refl->GetRepeatedMessage(msg, f, i)
                                                   : refl->GetMessage(msg, f);
                // RULE 3: a timelike primitive renders as its FHIR string.
                std::string when;
                if (pb_timelike_text(sub, &when)) {
                    fp.add_leaf(here, safe_dump(nlohmann::json(when)));
                    continue;
                }

                const google::protobuf::FieldDescriptor* inner = nullptr;
                if (pb_is_primitive_wrapper(sub, &inner)) {
                    // RULE 4: Decimal is stored as a string but written as a number.
                    const bool istr =
                        !pb_is_unquoted_primitive(sub) &&
                        (inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                         inner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM);
                    fp.add_leaf(
                        here, pb_json_value(pb_scalar_string(sub, sub.GetReflection(), inner, 0),
                                            istr));
                    continue;
                }

                // RULE 2: a Reference collapses to one `reference` string.
                std::string ref_text;
                if (pb_reference_text(sub, &ref_text)) {
                    fp.add_leaf(here + ".reference", safe_dump(nlohmann::json(ref_text)));
                    // `display`, `type` and `identifier` are ordinary siblings
                    // and still belong in the output; only the oneof folded.
                    pb_walk(sub, here, fp);
                    continue;
                }

                // RULE 1: a choice wrapper folds into the element's FHIR name.
                if (pb_is_choice_wrapper(sub)) {
                    if (const auto* ch = pb_oneof_set(sub)) {
                        const std::string folded = here + pb_ucfirst(ch->json_name());
                        const auto& picked = sub.GetReflection()->GetMessage(sub, ch);
                        const google::protobuf::FieldDescriptor* pinner = nullptr;
                        std::string picked_when;
                        if (pb_timelike_text(picked, &picked_when)) {
                            fp.add_leaf(folded, safe_dump(nlohmann::json(picked_when)));
                        } else if (pb_is_primitive_wrapper(picked, &pinner)) {
                            const bool istr =
                                !pb_is_unquoted_primitive(picked) &&
                                (pinner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                                 pinner->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM);
                            fp.add_leaf(folded,
                                        pb_json_value(pb_scalar_string(picked, picked.GetReflection(),
                                                                       pinner, 0), istr));
                        } else {
                            pb_walk(picked, folded, fp);
                        }
                    }
                    continue;
                }

                pb_walk(sub, here, fp);
                continue;
            }
            const bool str = f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING ||
                             f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_ENUM;
            fp.units.push_back(
                canonical_leaf(here, pb_json_value(pb_scalar_string(msg, refl, f, i), str)));
        }
    }
}

// The resource's own key: type from the record marker, id from the wrapped
// `id.value`. A record with neither is not comparable and contributes nothing.
inline std::string pb_resource_key(const google::protobuf::Message& msg, char type) {
    const auto* refl = msg.GetReflection();
    const auto* desc = msg.GetDescriptor();
    std::string id;
    if (const auto* f = desc->FindFieldByName("id")) {
        if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
            refl->HasField(msg, f)) {
            const auto& sub = refl->GetMessage(msg, f);
            const google::protobuf::FieldDescriptor* inner = nullptr;
            if (pb_is_primitive_wrapper(sub, &inner))
                id = pb_scalar_string(sub, sub.GetReflection(), inner, 0);
        }
    }
    return resource_key(type == 'P' ? "Patient" : "Observation", id);
}

// Parse one length-prefixed record and emit its leaf fields. Returns false if
// the record does not parse at all -- then it contributes nothing and every
// field it held is reported missing, which is the honest accounting.
inline bool pb_emit_record(const std::vector<uint8_t>& wire, std::size_t pos,
                           uint32_t len, char type, StreamFingerprint& fp) {
    const auto emit = [&](const google::protobuf::Message& m) {
        const std::string key = pb_resource_key(m, type);
        if (key.empty()) return;
        pb_walk(m, key, fp);
    };
    if (type == 'P') {
        google::fhir::r4::core::Patient m;
        if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
        emit(m);
        return true;
    }
    google::fhir::r4::core::Observation m;
    if (!m.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len))) return false;
    emit(m);
    return true;
}

inline StreamFingerprint calc_stream_hash(const std::vector<uint8_t>& wire) {
  StreamFingerprint fp;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      pb_emit_record(wire, pos, len, static_cast<char>(wire[pos]), fp);
      pos += 5 + len;
    } else {
      ++pos;
    }
  }
  fp.finalize();
  return fp;
}

// ---------------------------------------------------------------------------
// DAMAGE MODEL -- structural positions eligible for a bit flip
// ---------------------------------------------------------------------------
// Protobuf's OWN structure, not just the container this benchmark wrapped
// around it.
//
// The previous model enumerated the 5-byte record framing and nothing else:
// 1,473 records x 5 = 7,365 positions, against 423,846 for FastFHIR. Protobuf's
// actual wire structure -- the field keys, the length prefixes of every
// embedded message -- was never corrupted, so the arm's recovery number
// described the durability of a header this benchmark invented rather than
// anything about protobuf.
//
// A protobuf field is a varint KEY (field_number << 3 | wire_type) followed by
// a payload whose shape the wire type decides. Structural bytes are the key
// varints and, for wire type 2, the length varints: damage there changes what
// the following bytes ARE. Payload bytes of a string or a varint value are
// content and stay untouched, matching every other arm.
//
// Embedded messages are found by descent, not by schema: a length-delimited
// payload that scans cleanly as a message is treated as one. That is the same
// heuristic protoc --decode_raw uses, and it is the only one available without
// linking the descriptors into the position enumerator.
inline bool pb_read_varint(const std::vector<uint8_t>& w, std::size_t& pos,
                           std::size_t end, std::uint64_t& out) {
  out = 0;
  int shift = 0;
  while (pos < end && shift <= 63) {
    const uint8_t b = w[pos++];
    out |= static_cast<std::uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

// Returns false if the range does not scan as a well-formed message. When
// `positions` is null the scan only validates, which is how a length-delimited
// payload is tested before recursing into it.
inline bool pb_scan_message(const std::vector<uint8_t>& w, std::size_t pos, std::size_t end,
                            std::vector<std::size_t>* positions, int depth) {
  if (depth > 12) return false;
  while (pos < end) {
    const std::size_t key_start = pos;
    std::uint64_t key = 0;
    if (!pb_read_varint(w, pos, end, key)) return false;
    const std::size_t key_end = pos;
    const unsigned wire_type = static_cast<unsigned>(key & 7);
    if ((key >> 3) == 0) return false;  // field number 0 is not legal

    std::size_t len_start = 0, len_end = 0, payload_end = 0;
    switch (wire_type) {
      case 0: {  // varint value -- content
        std::uint64_t v = 0;
        if (!pb_read_varint(w, pos, end, v)) return false;
        break;
      }
      case 1:                                   // 64-bit
        if (pos + 8 > end) return false;
        pos += 8;
        break;
      case 5:                                   // 32-bit
        if (pos + 4 > end) return false;
        pos += 4;
        break;
      case 2: {                                 // length-delimited
        len_start = pos;
        std::uint64_t len = 0;
        if (!pb_read_varint(w, pos, end, len)) return false;
        len_end = pos;
        if (len > static_cast<std::uint64_t>(end - pos)) return false;
        payload_end = pos + static_cast<std::size_t>(len);
        // Descend only if it reads as a message; a string would otherwise have
        // its bytes counted as structure.
        if (len > 0 && pb_scan_message(w, pos, payload_end, nullptr, depth + 1) &&
            positions != nullptr)
          pb_scan_message(w, pos, payload_end, positions, depth + 1);
        pos = payload_end;
        break;
      }
      default:
        return false;                           // 3/4 are deprecated groups
    }

    if (positions != nullptr) {
      for (std::size_t i = key_start; i < key_end; ++i) positions->push_back(i);
      for (std::size_t i = len_start; i < len_end; ++i) positions->push_back(i);
    }
  }
  return true;
}

inline std::vector<std::size_t> structural_positions(const std::vector<uint8_t>& wire) {
  std::vector<std::size_t> positions;
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    if (wire[pos] == 'P' || wire[pos] == 'O') {
      const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                           (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                           (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                           (static_cast<uint32_t>(wire[pos + 4]) << 24);
      // The container framing is real structure too -- it is what finds the
      // record boundaries -- so it stays eligible alongside the message's own.
      for (std::size_t j = 0; j < 5; ++j) positions.push_back(pos + j);
      const std::size_t body = pos + 5;
      if (body + len <= wire.size())
        pb_scan_message(wire, body, body + len, &positions, 0);
      pos = body + len;
    } else {
      ++pos;
    }
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
  for (std::size_t pos = 0; pos + 5 <= wire.size();) {
    const char type = static_cast<char>(wire[pos]);
    if (type != 'P' && type != 'O') {
      ++pos;
      continue;
    }
    const uint32_t len = static_cast<uint32_t>(wire[pos + 1]) |
                         (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                         (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                         (static_cast<uint32_t>(wire[pos + 4]) << 24);
    if (len > 0 && pos + 5 + len <= wire.size()) {
      bool ok = false;
      if (type == 'P') {
        google::fhir::r4::core::Patient patient;
        ok = patient.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len));
      } else {
        google::fhir::r4::core::Observation obs;
        ok = obs.ParseFromArray(wire.data() + pos + 5, static_cast<int>(len));
      }
      (void)ok;  // pb_emit_record re-parses and walks; see its contract.
      pb_emit_record(wire, pos, len, type, fp);
      pos += 5 + len;
    } else {
      ++pos;
      for (; pos + 5 <= wire.size(); ++pos) {
        if (wire[pos] != 'P' && wire[pos] != 'O')
          continue;
        const uint32_t nlen = static_cast<uint32_t>(wire[pos + 1]) |
                              (static_cast<uint32_t>(wire[pos + 2]) << 8) |
                              (static_cast<uint32_t>(wire[pos + 3]) << 16) |
                              (static_cast<uint32_t>(wire[pos + 4]) << 24);
        if (nlen > 0 && pos + 5 + nlen <= wire.size())
          break;
      }
    }
  }
  fp.finalize();
  return fp;
}

#endif  // ARM_GOOGLE_FHIR_CODEC_HPP
