#pragma once

#include <nlohmann/json.hpp>
#include "harness.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#if defined(ARM_FASTFHIR)
#include <FF_Bundle.hpp>
#include <FF_Observation.hpp>
#include <FastFHIR.hpp>
#elif defined(ARM_JSON)
#include <simdjson.h>
#elif defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"
#endif


// ---------------------------------------------------------------------------
// Per-arm namespace -- REQUIRED FOR CORRECTNESS, not style.
// ---------------------------------------------------------------------------
// Each arm compiles these headers with a different ARM_* macro, so the SAME
// type and function names get four DIFFERENT definitions across four
// translation units. That is a One Definition Rule violation: the linker keeps
// one definition of each inline function and destructor and discards the rest,
// and an object built with one layout gets destroyed with another. ASan caught
// it as heap corruption with a moving crash site (notes.md section 1).
//
// An inline namespace gives each arm its own mangled symbols while leaving
// every existing call site spelled exactly as before. See bench_test_4.hpp for
// the full account.
#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::test_3 {
inline namespace BENCH_ARM_NS {

struct QuerySummary {
  std::size_t patients = 0;
  std::string birthdate;
  std::size_t observations = 0;
  std::size_t loinc_2085_9_matches = 0;
  std::size_t obs_value_present = 0;
  std::size_t obs_value_quantity = 0;
  std::size_t obs_value_codeableconcept = 0;
  std::size_t obs_value_string = 0;
  std::size_t obs_value_code = 0;
  std::size_t obs_effective_datetime = 0;
  std::size_t obs_effective_period = 0;
  std::size_t obs_issued_present = 0;
  std::size_t obs_component_value_present = 0;
  std::size_t obs_component_value_quantity = 0;
  std::size_t obs_component_value_codeableconcept = 0;
  std::size_t obs_component_value_string = 0;
  std::size_t obs_component_value_code = 0;
};

// A SELECTIVE query's answer: "find the cholesterol results".
//
// Deliberately tiny. The point of the stage is that a selective query should
// touch almost nothing, so anything this struct demands is work every arm is
// forced to do and the measurement is only honest if that work is the minimum
// the question needs.
struct SelectiveSummary {
  std::size_t scanned = 0;   // records examined (the denominator)
  std::size_t matches = 0;   // Observations carrying LOINC 2085-9
  std::size_t values = 0;    // ... of those, how many had a value[x]
};

inline std::string format_selective_summary(const SelectiveSummary& s) {
  return "scanned=" + std::to_string(s.scanned) +
         " matches=" + std::to_string(s.matches) +
         " values=" + std::to_string(s.values);
}

// The resources this query actually reached. An arm whose scanner misses a
// segment returns FAST and returns FEW; without this the first is reported and
// the second is not.
inline std::int64_t query_entries(const QuerySummary& s) {
  return static_cast<std::int64_t>(s.patients + s.observations);
}

inline std::string format_query_summary(const QuerySummary& summary) {
  return "patients=" + std::to_string(summary.patients) +
         " birthdate=" + summary.birthdate +
         " observations=" + std::to_string(summary.observations) +
         " loinc_2085_9_matches=" + std::to_string(summary.loinc_2085_9_matches) +
         " obs_value_present=" + std::to_string(summary.obs_value_present) +
         " obs_value_quantity=" + std::to_string(summary.obs_value_quantity) +
         " obs_value_codeableconcept=" + std::to_string(summary.obs_value_codeableconcept) +
         " obs_value_string=" + std::to_string(summary.obs_value_string) +
         " obs_value_code=" + std::to_string(summary.obs_value_code) +
         " obs_effective_datetime=" + std::to_string(summary.obs_effective_datetime) +
         " obs_effective_period=" + std::to_string(summary.obs_effective_period) +
         " obs_issued_present=" + std::to_string(summary.obs_issued_present) +
         " obs_component_value_present=" + std::to_string(summary.obs_component_value_present) +
         " obs_component_value_quantity=" + std::to_string(summary.obs_component_value_quantity) +
         " obs_component_value_codeableconcept=" + std::to_string(summary.obs_component_value_codeableconcept) +
         " obs_component_value_string=" + std::to_string(summary.obs_component_value_string) +
         " obs_component_value_code=" + std::to_string(summary.obs_component_value_code);
}

constexpr std::string_view kCholesterolLoincCode = "2085-9";
constexpr std::string_view kLoincSystem = "http://loinc.org";

namespace detail {

enum class ValueKind {
  Quantity,
  CodeableConcept,
  String,
  Code,
};

struct QueryAccumulator {
  std::size_t patients = 0;
  std::string birthdate;
  std::size_t observations = 0;
  std::size_t loinc_2085_9_matches = 0;
  std::size_t obs_value_present = 0;
  std::size_t obs_value_quantity = 0;
  std::size_t obs_value_codeableconcept = 0;
  std::size_t obs_value_string = 0;
  std::size_t obs_value_code = 0;
  std::size_t obs_effective_datetime = 0;
  std::size_t obs_effective_period = 0;
  std::size_t obs_issued_present = 0;
  std::size_t obs_component_value_present = 0;
  std::size_t obs_component_value_quantity = 0;
  std::size_t obs_component_value_codeableconcept = 0;
  std::size_t obs_component_value_string = 0;
  std::size_t obs_component_value_code = 0;

  void note_patient(std::string_view candidate_birthdate) {
    ++patients;
    if (birthdate.empty() && !candidate_birthdate.empty()) {
      birthdate.assign(candidate_birthdate.begin(), candidate_birthdate.end());
    }
  }

  void note_observation() { ++observations; }

  void note_loinc_2085_9() { ++loinc_2085_9_matches; }

  void note_observation_value(ValueKind kind) {
    ++obs_value_present;
    switch (kind) {
      case ValueKind::Quantity:
        ++obs_value_quantity;
        break;
      case ValueKind::CodeableConcept:
        ++obs_value_codeableconcept;
        break;
      case ValueKind::String:
        ++obs_value_string;
        break;
      case ValueKind::Code:
        ++obs_value_code;
        break;
    }
  }

  void note_component_value(ValueKind kind) {
    ++obs_component_value_present;
    switch (kind) {
      case ValueKind::Quantity:
        ++obs_component_value_quantity;
        break;
      case ValueKind::CodeableConcept:
        ++obs_component_value_codeableconcept;
        break;
      case ValueKind::String:
        ++obs_component_value_string;
        break;
      case ValueKind::Code:
        ++obs_component_value_code;
        break;
    }
  }

  void note_effective_datetime() { ++obs_effective_datetime; }

  void note_effective_period() { ++obs_effective_period; }

  void note_issued() { ++obs_issued_present; }

  QuerySummary finalize() const {
    QuerySummary summary;
    summary.patients = patients;
    summary.birthdate = birthdate;
    summary.observations = observations;
    summary.loinc_2085_9_matches = loinc_2085_9_matches;
    summary.obs_value_present = obs_value_present;
    summary.obs_value_quantity = obs_value_quantity;
    summary.obs_value_codeableconcept = obs_value_codeableconcept;
    summary.obs_value_string = obs_value_string;
    summary.obs_value_code = obs_value_code;
    summary.obs_effective_datetime = obs_effective_datetime;
    summary.obs_effective_period = obs_effective_period;
    summary.obs_issued_present = obs_issued_present;
    summary.obs_component_value_present = obs_component_value_present;
    summary.obs_component_value_quantity = obs_component_value_quantity;
    summary.obs_component_value_codeableconcept = obs_component_value_codeableconcept;
    summary.obs_component_value_string = obs_component_value_string;
    summary.obs_component_value_code = obs_component_value_code;
    return summary;
  }
};

inline bool is_loinc_match(std::string_view system, std::string_view code) {
  return code == kCholesterolLoincCode && (system.empty() || system == kLoincSystem);
}

inline std::string segment_field(std::string_view segment, std::size_t one_based_index) {
  if (one_based_index == 0) {
    return "";
  }
  std::size_t current = 1;
  std::size_t start = 0;
  while (start <= segment.size()) {
    const auto sep = segment.find('|', start);
    const auto end = sep == std::string_view::npos ? segment.size() : sep;
    if (current == one_based_index) {
      return std::string(segment.substr(start, end - start));
    }
    if (sep == std::string_view::npos) {
      break;
    }
    start = sep + 1;
    ++current;
  }
  return "";
}

}  // namespace detail

#if defined(ARM_FASTFHIR)

#define TEST3_FF_BUNDLE_ENTRY FastFHIR::Fields::BUNDLE::ENTRY
#define TEST3_FF_ENTRY_RESOURCE FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE

inline std::optional<detail::ValueKind> value_kind_from_choice_tag(RECOVERY_TAG tag) {
  switch (tag) {
    case RECOVER_FF_QUANTITY:
      return detail::ValueKind::Quantity;
    case RECOVER_FF_CODEABLECONCEPT:
      return detail::ValueKind::CodeableConcept;
    case RECOVER_FF_STRING:
      return detail::ValueKind::String;
    case RECOVER_FF_CODE:
      return detail::ValueKind::Code;
    default:
      return std::nullopt;
  }
}

static inline QuerySummary query(std::string_view payload) {
  detail::QueryAccumulator acc;

  // Keep parser function-local in the query path to avoid persistent heap state.
  FastFHIR::Parser parser(payload.data(), payload.size());
  const auto root_node = parser.root();
  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())) {
    return acc.finalize();
  }

  auto entries = root_node[TEST3_FF_BUNDLE_ENTRY];
  if (!entries) {
    return acc.finalize();
  }

  for (auto& entry : entries.entries()) {
    auto resource = entry[TEST3_FF_ENTRY_RESOURCE];
    if (!resource) {
      continue;
    }
    auto resource_node = resource.as_node();
    if (!resource_node) {
      continue;
    }

    if (resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
      // birthDate is a packed date/time slot upstream, not a string -- see
      // read_text_field(). The local std::string must outlive note_patient().
      std::string birthdate;
      auto birth_date_entry = resource_node[FastFHIR::Fields::PATIENT::BIRTH_DATE];
      if (birth_date_entry) {
        birthdate = read_text_field(birth_date_entry.as_node());
      }
      acc.note_patient(birthdate);
      continue;
    }

    if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
      acc.note_observation();

      // Query via node lenses, NOT as<ObservationData>() -- nodes are views
      // over the bytestream, and materializing the whole observation POCO
      // deserializes every field the query never reads. This is the same
      // O(1) per-field access Test 2 demonstrates; the census below reads
      // only what it classifies.
      //
      // Does this observation carry LOINC 2085-9? code.coding[*].{system,code}
      // via the reflective API; index walks instead of entries() keep the
      // zero-allocation read path intact.
      bool matched = false;
      if (auto code_entry = resource_node[FastFHIR::Fields::OBSERVATION::CODE]) {
        auto cc = code_entry.as_node();
        if (cc && cc.is_object()) {
          if (auto coding_entry = cc[FastFHIR::Fields::CODEABLECONCEPT::CODING]) {
            const auto n_codings = coding_entry.size();
            for (std::size_t i = 0; i < n_codings; ++i) {
              const auto coding = coding_entry[i];
              std::string_view system_sv, code_sv;
              if (auto s = coding[FastFHIR::Fields::CODING::SYSTEM]) {
                system_sv = s.as<std::string_view>();
              }
              if (auto c = coding[FastFHIR::Fields::CODING::CODE]) {
                code_sv = c.as<std::string_view>();
              }
              if (detail::is_loinc_match(system_sv, code_sv)) {
                matched = true;
                break;
              }
            }
          }
        }
      }
      if (matched) {
        acc.note_loinc_2085_9();
      }

      // value[x] / effective[x] / component[x].value are choice slots: the
      // variant tag is the slot's own RECOVERY_TAG, read without expanding
      // the node. Same bytes the POCO deserializer would have copied.
      if (auto v = resource_node[FastFHIR::Fields::OBSERVATION::VALUE]) {
        if (const auto kind = value_kind_from_choice_tag(v.target_recovery)) {
          acc.note_observation_value(*kind);
        }
      }

      if (auto eff = resource_node[FastFHIR::Fields::OBSERVATION::EFFECTIVE]) {
        if (eff.target_recovery == RECOVER_FF_PERIOD) {
          acc.note_effective_period();
        } else {
          acc.note_effective_datetime();
        }
      }

      // issued is a packed date/time slot (FF_FIELD_DATETIME): decoding it is
      // the CAPI-4 zero-copy-reader gap, and the census only needs presence --
      // an absent slot reads as a falsy Entry, exactly the POCO's is_empty().
      if (resource_node[FastFHIR::Fields::OBSERVATION::ISSUED]) {
        acc.note_issued();
      }

      if (auto comp = resource_node[FastFHIR::Fields::OBSERVATION::COMPONENT]) {
        const auto n_components = comp.size();
        for (std::size_t i = 0; i < n_components; ++i) {
          if (auto cv = comp[i][FastFHIR::Fields::OBSERVATION_COMPONENT::VALUE]) {
            if (const auto kind = value_kind_from_choice_tag(cv.target_recovery)) {
              acc.note_component_value(*kind);
            }
          }
        }
      }
    }
  }

  return acc.finalize();
}

// SELECTIVE query -- the shape a clinical system actually runs.
//
// Three questions, cheapest first, and a `continue` after each so a non-match
// costs only the questions it failed:
//   1. is this record an Observation?   -- the tuple's own RECOVERY_TAG, which
//      is read from the parent slot WITHOUT dereferencing the block
//   2. does it carry LOINC 2085-9?      -- code.coding[*], stopping at the
//      first match
//   3. only then, does it have a value? -- one slot read on the ~2% that match
//
// Nothing else is touched: no value-kind classification, no effective[x], no
// issued, no components. That is the difference from query() above, whose
// 17-field census cannot skip anything.
static inline SelectiveSummary query_selective(std::string_view payload) {
  SelectiveSummary out;

  FastFHIR::Parser parser(payload.data(), payload.size());
  const auto root_node = parser.root();
  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::BUNDLE>())) return out;
  auto entries = root_node[TEST3_FF_BUNDLE_ENTRY];
  if (!entries) return out;

  for (auto& entry : entries.entries()) {
    auto resource = entry[TEST3_FF_ENTRY_RESOURCE];
    if (!resource) continue;
    ++out.scanned;

    // 1. Type, from the tag half of the 10-byte {offset, tag} tuple --
    //    WITHOUT following the offset (upstream API-1,
    //    Entry::concrete_recovery()). A non-Observation is rejected here and
    //    its block is never touched.
    //
    //    Before API-1 this needed as_node() first, because
    //    Entry::target_recovery carries the STATIC child_recovery for a
    //    resource slot (RECOVER_FF_RESOURCE, the polymorphic base) and only a
    //    choice slot resolved the runtime tag. That cost a dereference per
    //    record to read something already sitting in the slot.
    if (resource.concrete_recovery() != RECOVER_FF_OBSERVATION) continue;

    auto resource_node = resource.as_node();
    if (!resource_node) continue;

    // 2. The code. First match wins; a miss ends this record.
    bool matched = false;
    if (auto code_entry = resource_node[FastFHIR::Fields::OBSERVATION::CODE]) {
      auto cc = code_entry.as_node();
      if (cc && cc.is_object()) {
        if (auto coding_entry = cc[FastFHIR::Fields::CODEABLECONCEPT::CODING]) {
          const auto n = coding_entry.size();
          for (std::size_t i = 0; i < n && !matched; ++i) {
            const auto coding = coding_entry[i];
            std::string_view system_sv, code_sv;
            if (auto sy = coding[FastFHIR::Fields::CODING::SYSTEM])
              system_sv = sy.as<std::string_view>();
            if (auto c = coding[FastFHIR::Fields::CODING::CODE])
              code_sv = c.as<std::string_view>();
            matched = detail::is_loinc_match(system_sv, code_sv);
          }
        }
      }
    }
    if (!matched) continue;
    ++out.matches;

    // 3. Only the matches pay for this.
    if (resource_node[FastFHIR::Fields::OBSERVATION::VALUE]) ++out.values;
  }
  return out;
}

#undef TEST3_FF_BUNDLE_ENTRY
#undef TEST3_FF_ENTRY_RESOURCE

#elif defined(ARM_JSON)

#define TEST3_JSON_KEY_ENTRY "entry"
#define TEST3_JSON_KEY_RESOURCE "resource"
#define TEST3_JSON_KEY_RESOURCE_TYPE "resourceType"
#define TEST3_JSON_KEY_PATIENT "Patient"
#define TEST3_JSON_KEY_OBSERVATION "Observation"

static inline QuerySummary query(const std::string& payload) {
  detail::QueryAccumulator acc;

  simdjson::dom::parser json_parser;
  auto doc = json_parser.parse(payload);
  if (doc.error()) {
    return acc.finalize();
  }

  auto entries = doc[TEST3_JSON_KEY_ENTRY];
  if (!entries.is_array()) {
    return acc.finalize();
  }

  for (auto entry : entries) {
    if (!entry.is_object()) {
      continue;
    }

    auto resource = entry[TEST3_JSON_KEY_RESOURCE];
    if (!resource.is_object()) {
      continue;
    }

    auto res_type = resource[TEST3_JSON_KEY_RESOURCE_TYPE];
    if (!res_type.is_string()) {
      continue;
    }

    const std::string_view type_str = res_type.get_c_str().value_unsafe();

    if (type_str == TEST3_JSON_KEY_PATIENT) {
      auto bd = resource["birthDate"];
      if (bd.is_string()) {
        acc.note_patient(bd.get_c_str().value_unsafe());
      } else {
        acc.note_patient({});
      }
      continue;
    }

    if (type_str == TEST3_JSON_KEY_OBSERVATION) {
      acc.note_observation();

      auto code = resource["code"];
      if (code.is_object()) {
        auto coding = code["coding"];
        if (coding.is_array()) {
          bool matched = false;
          for (auto c : coding) {
            if (!c.is_object()) {
              continue;
            }
            auto code_val = c["code"];
            if (!code_val.is_string()) {
              continue;
            }
            std::string_view code_sv = code_val.get_c_str().value_unsafe();
            std::string_view system_sv;
            auto system_val = c["system"];
            if (system_val.is_string()) {
              system_sv = system_val.get_c_str().value_unsafe();
            }
            if (detail::is_loinc_match(system_sv, code_sv)) {
              matched = true;
              break;
            }
          }
          if (matched) {
            acc.note_loinc_2085_9();
          }
        }
      }

      if (resource["valueQuantity"].is_object()) {
        acc.note_observation_value(detail::ValueKind::Quantity);
      } else if (resource["valueCodeableConcept"].is_object()) {
        acc.note_observation_value(detail::ValueKind::CodeableConcept);
      } else if (resource["valueString"].is_string()) {
        acc.note_observation_value(detail::ValueKind::String);
      } else if (resource["valueCode"].is_string()) {
        acc.note_observation_value(detail::ValueKind::Code);
      }

      if (resource["effectiveDateTime"].is_string()) {
        acc.note_effective_datetime();
      }
      if (resource["effectivePeriod"].is_object()) {
        acc.note_effective_period();
      }
      if (resource["issued"].is_string()) {
        acc.note_issued();
      }

      auto components = resource["component"];
      if (components.is_array()) {
        for (auto comp : components) {
          if (!comp.is_object()) {
            continue;
          }
          if (comp["valueQuantity"].is_object()) {
            acc.note_component_value(detail::ValueKind::Quantity);
          } else if (comp["valueCodeableConcept"].is_object()) {
            acc.note_component_value(detail::ValueKind::CodeableConcept);
          } else if (comp["valueString"].is_string()) {
            acc.note_component_value(detail::ValueKind::String);
          } else if (comp["valueCode"].is_string()) {
            acc.note_component_value(detail::ValueKind::Code);
          }
        }
      }
    }
  }

  return acc.finalize();
}

// SELECTIVE query. Same three questions as the FastFHIR arm, same early-outs.
//
// The structural difference this stage exists to expose: simdjson must
// MATERIALISE the document before the first question can be asked. The DOM
// parse is unconditional and pays for every byte, including the ~98% of
// records the query is about to reject. Early-out saves the field reads, not
// the parse.
static inline SelectiveSummary query_selective(const std::string& payload) {
  SelectiveSummary out;
  simdjson::dom::parser json_parser;
  auto doc = json_parser.parse(payload);
  if (doc.error()) return out;
  auto entries = doc[TEST3_JSON_KEY_ENTRY];
  if (!entries.is_array()) return out;

  for (auto entry : entries) {
    auto resource = entry[TEST3_JSON_KEY_RESOURCE];
    if (!resource.is_object()) continue;
    ++out.scanned;

    auto res_type = resource[TEST3_JSON_KEY_RESOURCE_TYPE];
    if (!res_type.is_string()) continue;
    if (std::string_view(res_type.get_c_str().value_unsafe()) != "Observation") continue;

    bool matched = false;
    auto code = resource["code"];
    if (code.is_object()) {
      auto coding = code["coding"];
      if (coding.is_array()) {
        for (auto c : coding) {
          if (!c.is_object()) continue;
          auto sy = c["system"];
          auto cd = c["code"];
          std::string_view system_sv =
              sy.is_string() ? std::string_view(sy.get_c_str().value_unsafe()) : std::string_view{};
          std::string_view code_sv =
              cd.is_string() ? std::string_view(cd.get_c_str().value_unsafe()) : std::string_view{};
          if (detail::is_loinc_match(system_sv, code_sv)) { matched = true; break; }
        }
      }
    }
    if (!matched) continue;
    ++out.matches;

    if (resource["valueQuantity"].is_object() ||
        resource["valueCodeableConcept"].is_object() ||
        resource["valueString"].is_string() || resource["valueCode"].is_string())
      ++out.values;
  }
  return out;
}

#undef TEST3_JSON_KEY_ENTRY
#undef TEST3_JSON_KEY_RESOURCE
#undef TEST3_JSON_KEY_RESOURCE_TYPE
#undef TEST3_JSON_KEY_PATIENT
#undef TEST3_JSON_KEY_OBSERVATION

#elif defined(ARM_GOOGLE_FHIR)

inline uint32_t decode_u32_le_t3(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

static inline QuerySummary query(const std::string& payload) {
  detail::QueryAccumulator acc;

  // Hoist outside the loop for memory reuse.
  google::fhir::r4::core::Patient patient;
  google::fhir::r4::core::Observation observation;

  std::size_t pos = 0;
  while (pos + 5 <= payload.size()) {
    const char record_type = payload[pos];
    const uint32_t record_len = decode_u32_le_t3(payload.data() + pos + 1);
    pos += 5;

    if (pos + record_len > payload.size()) {
      break;
    }

    const char* record_data = payload.data() + pos;
    pos += record_len;

    if (record_type == 'P') {
      patient.Clear();
      if (patient.ParseFromArray(record_data, static_cast<int>(record_len))) {
        std::string bd;
        if (patient.has_birth_date()) {
          // Google FHIR stores dates as epoch microseconds.
          // Using to_string to mimic extraction cost without full ISO8601 formatting overhead.
          bd = std::to_string(patient.birth_date().value_us());
        }
        acc.note_patient(bd);
      }
    } else if (record_type == 'O') {
      observation.Clear();
      if (observation.ParseFromArray(record_data, static_cast<int>(record_len))) {
        acc.note_observation();

        if (observation.has_code()) {
          bool matched = false;
          for (const auto& coding : observation.code().coding()) {
            if (detail::is_loinc_match(coding.system().value(), coding.code().value())) {
              matched = true;
              break;
            }
          }
          if (matched) acc.note_loinc_2085_9();
        }

        if (observation.has_value()) {
          const auto& val = observation.value();
          if (val.has_quantity()) {
            acc.note_observation_value(detail::ValueKind::Quantity);
          } else if (val.has_codeable_concept()) {
            acc.note_observation_value(detail::ValueKind::CodeableConcept);
          } else if (val.has_string_value()) {
            acc.note_observation_value(detail::ValueKind::String);
          }
          // Note: FHIR R4 Observation.value[x] does not natively use "valueCode"
          // so there is no .has_code() equivalent here in Google FHIR.
        }

        if (observation.has_effective()) {
          const auto& eff = observation.effective();
          if (eff.has_date_time()) {
            acc.note_effective_datetime();
          } else if (eff.has_period()) {
            acc.note_effective_period();
          }
        }

        if (observation.has_issued()) {
          acc.note_issued();
        }

        for (const auto& comp : observation.component()) {
          if (comp.has_value()) {
            const auto& cval = comp.value();
            if (cval.has_quantity()) {
              acc.note_component_value(detail::ValueKind::Quantity);
            } else if (cval.has_codeable_concept()) {
              acc.note_component_value(detail::ValueKind::CodeableConcept);
            } else if (cval.has_string_value()) {
              acc.note_component_value(detail::ValueKind::String);
            }
          }
        }
      }
    }
  }

  return acc.finalize();
}

// SELECTIVE query. Protobuf's TLV framing lets the type byte reject a record
// without ParseFromArray -- a genuine early-out, and the closest structural
// analogue to FastFHIR's tag check. What it cannot skip is parsing the records
// that ARE Observations, because a protobuf field is only reachable after the
// message is decoded.
static inline SelectiveSummary query_selective(const std::string& payload) {
  SelectiveSummary out;
  google::fhir::r4::core::Observation observation;

  std::size_t pos = 0;
  while (pos + 5 <= payload.size()) {
    const char record_type = payload[pos];
    const uint32_t record_len = decode_u32_le_t3(payload.data() + pos + 1);
    pos += 5;
    if (pos + record_len > payload.size()) break;
    ++out.scanned;

    if (record_type != 'O') { pos += record_len; continue; }  // type, pre-parse
    if (!observation.ParseFromArray(payload.data() + pos, static_cast<int>(record_len))) {
      pos += record_len;
      continue;
    }
    pos += record_len;

    bool matched = false;
    for (const auto& coding : observation.code().coding()) {
      if (detail::is_loinc_match(coding.system().value(), coding.code().value())) {
        matched = true;
        break;
      }
    }
    if (!matched) continue;
    ++out.matches;
    if (observation.has_value()) ++out.values;
  }
  return out;
}

#elif defined(ARM_HL7V2)

static inline QuerySummary query(const std::string& payload) {
  // ── Parse into full MessageTree ──────────────────────────────────────
  // Matches the cost model of all other arms: parse first, then query.
  // Previously this arm used a raw line scan (no parse cost), which made
  // HL7v2 test 3 results unfairly faster than FFHR / JSON / Google FHIR.
  auto messages = hl7v2::parse_batch(payload);

  detail::QueryAccumulator acc;

  for (const auto& msg : messages) {
    for (const auto& seg : msg.tree.segments) {
      if (seg.name == "PID") {
        hl7v2::PidView pid(seg);
        acc.note_patient(pid.birth_date());
      } else if (seg.name == "OBX") {
        acc.note_observation();

        hl7v2::ObxView obx(seg);

        // LOINC 2085-9 match: OBX-3 observation_id is the first component
        // of the OBX-3 field (built by observation_code_id() during Test 1).
        const auto obs_id = obx.observation_id();
        if (obs_id == kCholesterolLoincCode) {
          acc.note_loinc_2085_9();
        }

        // ── Value classification (OBX-2 value type -> ValueKind) ──────────
        //
        // CWE IS A CodeableConcept, not a `code`. The CDC converter's
        // OBXValue.yml routes both through the same rule --
        // `obx-value-cwe: condition ... = "CWE" or "CE"` -> CWE.yml -- and a
        // plain FHIR `code` is v2's IS, which is a separate rule. Mapping CWE
        // to Code put all 26 CodeableConcept values in this corpus into the
        // wrong bucket, and the cross-arm parity check reported it as two
        // mismatches (codeableconcept 26->0, code 0->26) rather than one.
        const auto vt = obx.value_type();
        const auto val = obx.value();
        if (!val.empty()) {
          if (vt == "NM") {
            acc.note_observation_value(detail::ValueKind::Quantity);
          } else if (vt == "CE" || vt == "CWE" || vt == "CNE") {
            acc.note_observation_value(detail::ValueKind::CodeableConcept);
          } else if (vt == "IS") {
            acc.note_observation_value(detail::ValueKind::Code);
          } else if (vt == "ST" || vt == "FT" || vt == "TX") {
            acc.note_observation_value(detail::ValueKind::String);
          }
        }
      } else if (seg.name == "ZFX" && seg.fields.size() >= 2) {
        // fields[] excludes the segment name, so ZFX-1 is fields[0].
        // effective, issued and component have no native OBX field, so the
        // arm carries them in its ZFX passthrough. This scanner used to skip
        // ZFX entirely and leave those four counters at zero -- 316 effective
        // date/times and 316 issued instants reported as absent from a wire
        // that carried every one of them.
        const std::string name = hl7v2::unescape_text(seg.fields[0].val);
        const std::string payload = hl7v2::unescape_text(seg.fields[1].val);
        nlohmann::json j;
        try { j = nlohmann::json::parse(payload); }
        catch (const std::exception&) { continue; }

        const auto kind_of = [](const std::string& key) {
          if (key.size() > 5 && key.compare(key.size() - 8, 8, "Quantity") == 0)
            return detail::ValueKind::Quantity;
          if (key.find("CodeableConcept") != std::string::npos)
            return detail::ValueKind::CodeableConcept;
          if (key.size() > 4 && key.compare(key.size() - 6, 6, "String") == 0)
            return detail::ValueKind::String;
          return detail::ValueKind::Code;
        };

        if (name == "observation.effective[x]" && j.is_object() && !j.empty()) {
          const std::string k = j.begin().key();
          if (k.find("Period") != std::string::npos) acc.note_effective_period();
          else                                       acc.note_effective_datetime();
        } else if (name == "observation.issued") {
          acc.note_issued();
        } else if (name == "observation.component" && j.is_array()) {
          for (const auto& comp : j) {
            if (!comp.is_object()) continue;
            for (auto it = comp.begin(); it != comp.end(); ++it) {
              if (it.key().rfind("value", 0) == 0 && it.key() != "value") {
                acc.note_component_value(kind_of(it.key()));
                break;
              }
            }
          }
        }
      }
    }
  }

  return acc.finalize();
}

// SELECTIVE query. v2 has no record type to test before reading, so the
// "is this an Observation" question is answered by the segment name -- which
// is the same self-identifying-record property that made its recovery work.
// A non-OBX line is rejected on three bytes.
static inline SelectiveSummary query_selective(const std::string& payload) {
  SelectiveSummary out;
  std::size_t start = 0;
  while (start < payload.size()) {
    std::size_t end = payload.find('\r', start);
    if (end == std::string::npos) end = payload.size();
    const std::string_view seg(payload.data() + start, end - start);
    start = end + 1;
    if (seg.size() < 4) continue;
    ++out.scanned;
    if (seg.compare(0, 3, "OBX") != 0) continue;   // three bytes, no parse

    // OBX-3 is the observation identifier (code^display^system). segment_field
    // is 1-based INCLUDING the segment name, so OBX-3 is index 4 -- index 3 is
    // OBX-2, the value type, which is why an earlier version matched nothing.
    const std::string field3 = detail::segment_field(seg, 4);
    const auto caret = field3.find('^');
    const std::string_view code_sv =
        caret == std::string::npos ? std::string_view(field3)
                                   : std::string_view(field3).substr(0, caret);
    if (code_sv != kCholesterolLoincCode) continue;
    ++out.matches;
    if (!detail::segment_field(seg, 6).empty()) ++out.values;  // OBX-5
  }
  return out;
}

#endif

}  // inline namespace BENCH_ARM_NS
}  // namespace bench::test_3
