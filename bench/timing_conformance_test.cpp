#include "harness.hpp"

#include <FF_Patient.hpp>
#include <cctype>
#include <iostream>
#include <memory>
#include <string>

namespace
{

  std::string extract_field(const std::string &text, const std::string &key)
  {
    const std::string needle = key + "=";
    const auto start = text.find(needle);
    if (start == std::string::npos)
    {
      return "";
    }
    const auto value_start = start + needle.size();
    const auto value_end = text.find(' ', value_start);
    if (value_end == std::string::npos)
    {
      return text.substr(value_start);
    }
    return text.substr(value_start, value_end - value_start);
  }

  int extract_patients(const std::string &text)
  {
    const auto raw = extract_field(text, "patients");
    if (raw.empty())
    {
      return -1;
    }
    try
    {
      return std::stoi(raw);
    }
    catch (...)
    {
      return -1;
    }
  }

  std::string normalize_birthdate(std::string value)
  {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
    {
      if (std::isdigit(static_cast<unsigned char>(ch)))
      {
        out.push_back(ch);
      }
    }
    return out;
  }

  bool metrics_are_valid(const bench::ArmRunResult &run)
  {
    for (const auto &metric : run.metrics)
    {
      if (metric.duration_ns > 0)
      {
        continue;
      }
      return false;
    }
    return true;
  }

} // namespace

int main()
{
  bench::BundlePatient bp{};
  bp.memory = FastFHIR::Memory::create(4096);
  const FastFHIR::FF_Builder builder_handle = bench::make_builder(bp.memory, FHIR_VERSION_R5);

  PatientData patient{};
  patient.id = "patient-conformance";
  patient.birthdate = "1990-03-21";
  patient.gender = FF_AdministrativeGender::Male;
  patient.active = 1;

  auto patient_handle = builder_handle->append_obj(patient);
  const auto patient_view =
      bench::seal_stream(builder_handle, patient_handle, "conformance patient", FF_CHECKSUM_NONE);
  const int64_t patient_ffhr_bytes = static_cast<int64_t>(patient_view.size());

  bp.patient.id = patient.id;
  bp.patient.birthdate = patient.birthdate;
  bp.patient.gender = patient.gender;
  bp.patient.active = patient.active;

  bench::BundleBenchFixture fixture{};
  fixture.bundle.push_back(std::move(bp));
  fixture.target_size_bytes = patient_ffhr_bytes;
  fixture.actual_ingested_bytes = patient_ffhr_bytes;

  const auto fastfhir = bench::run_fastfhir_bundle(fixture);
  const auto json = bench::run_json_bundle(fixture);

  if (fastfhir.metrics.size() < 3 || json.metrics.size() < 3)
  {
    std::cerr << "timing conformance failed: expected test metrics from both arms\n";
    return 1;
  }

  if (!metrics_are_valid(fastfhir))
  {
    std::cerr << "timing conformance failed: FastFHIR metric duration invalid\n";
    return 1;
  }
  if (!metrics_are_valid(json))
  {
    std::cerr << "timing conformance failed: JSON metric duration invalid\n";
    return 1;
  }
  const int fast_patients = extract_patients(fastfhir.queried_value);
  const int json_patients = extract_patients(json.queried_value);

  if (fast_patients != 1 || json_patients != 1)
  {
    std::cerr << "timing conformance failed: expected patients=1 for both arms\n"
              << "  fastfhir: " << fastfhir.queried_value << "\n"
              << "  json:     " << json.queried_value << "\n";
    return 1;
  }

  const auto fast_birth = normalize_birthdate(extract_field(fastfhir.queried_value, "birthdate"));
  const auto json_birth = normalize_birthdate(extract_field(json.queried_value, "birthdate"));

  if (fast_birth.empty() || json_birth.empty() || fast_birth != json_birth)
  {
    std::cerr << "timing conformance failed: birthdate mismatch across FFHR/JSON arms\n"
              << "  fastfhir: " << fastfhir.queried_value << "\n"
              << "  json:     " << json.queried_value << "\n";
    return 1;
  }

  std::cout << "timing conformance passed\n";
  return 0;
}
