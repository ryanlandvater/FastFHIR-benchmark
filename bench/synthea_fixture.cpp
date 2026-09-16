#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <simdjson.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <FF_Ingestor.hpp>

namespace bench {

// Split from the path loader so a wire that has been RECONSTRUCTED as FHIR
// JSON -- not read off disk -- can reach the same ingest. That is the shared
// back half of every arm's decoder: each arm rebuilds FHIR JSON from its own
// format, and this turns that into the POCO the comparison actually needs.
BundlePatient make_bundle_patient_from_json_text(std::string_view json,
                                                 std::string_view label) {
  simdjson::padded_string json_buffer(json);
  if (json_buffer.size() == 0) {
    throw std::runtime_error("Empty JSON payload: " + std::string(label));
  }
  const std::filesystem::path json_path{std::string(label)};

  const auto file_size = static_cast<std::size_t>(json_buffer.size());
  const std::string_view ingest_payload(json_buffer.data(), json_buffer.size());

  BundlePatient item{};
  const auto arena_size = std::max<std::size_t>(4096, file_size * static_cast<std::size_t>(2));
  item.memory = FastFHIR::Memory::create(arena_size);

  const FastFHIR::FF_Builder builder_handle = make_builder(item.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(builder_handle.get(), FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = *builder_handle,
        .source_type = FF_SOURCE_FHIR_JSON,
        // FILTER_NONE, not the FILTER_ALL_KNOWN default: that mode SUPPRESSES
        // profile-native and HL7-known-safe extension URLs -- a real size win
        // in production (a URL the profile already knows need not be stored),
        // but it means the URL is not in the trie and cannot be regenerated.
        // The benchmark corpus has to hold the whole document, or the JSON arm
        // cannot emit `"url"` and the arms stop encoding the same data.
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .json_string = ingest_payload,
        // padded_string guarantees SIMDJSON_PADDING slack past size(), so the
        // ingestor can parse in place instead of making a padded copy of the
        // whole document.
        .payload_capacity = json_buffer.size() + simdjson::SIMDJSON_PADDING,
    };
    result = ingestor.ingest(request, root, parsed_count);
  } catch (const std::exception& ex) {
    throw std::runtime_error("Ingestor exception for " + json_path.string() + ": " + ex.what());
  }

  if (!result) {
    throw std::runtime_error(
        "Ingestor failed for " + json_path.string() + ": code="
        + std::to_string(static_cast<int>(result.code)) + ", message=" + result.message);
  }
  if (!root) {
    throw std::runtime_error("Ingestor returned null root for " + json_path.string());
  }

  const auto root_node = root.as_node();
  detail::hydrate_bundle_resources(root_node, item);

  if (item.patient.id.empty()) {
    throw std::runtime_error("No Patient resource in ingested root for " + json_path.string());
  }

  (void)seal_stream(builder_handle, root, json_path.string());

  // Rebuild every interned URL (see BundlePatient::url_table). MUST run AFTER
  // seal_stream: Parser validates the FF_HEADER on construction, and the
  // header -- with the URL_DIR_OFFSET this needs -- is written by finalize().
  // Before the seal there is no magic to match and every ingest is skipped.
  {
    FastFHIR::Parser url_reader(item.memory);
    if (url_reader.has_url_directory()) {
      const auto* base = reinterpret_cast<const BYTE*>(item.memory.base());
      const auto dir = url_reader.url_directory();
      const uint32_t n = dir.entry_count(base);
      item.url_table.reserve(n);
      for (uint32_t i = 0; i < n; ++i)
        item.url_table.push_back(dir.get_url(base, i));

    }
  }
  if (std::getenv("BENCH_VALIDATE_INGEST")) {
    FastFHIR::Parser check(item.memory);
    const FF_Result vr = check.validate_FFHR_stream();
    std::fprintf(stderr, "[validate-ingest] %s: code=%d %s\n",
                 json_path.filename().string().c_str(), (int)vr.code, vr.message.c_str());
  }
  return item;
}

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path) {
  simdjson::padded_string buf;
  try {
    buf = simdjson::padded_string::load(json_path.string());
  } catch (const std::exception& ex) {
    throw std::runtime_error("Failed to load JSON payload: " + std::string(ex.what()));
  }
  return make_bundle_patient_from_json_text(std::string_view(buf.data(), buf.size()),
                                            json_path.string());
}

EnrichmentObservationFixture load_enrichment_observation_from_json(const std::filesystem::path& json_path) {
  simdjson::padded_string json_buffer;
  try {
    json_buffer = simdjson::padded_string::load(json_path.string());
  } catch (const std::exception& ex) {
    throw std::runtime_error("Failed to load JSON payload: " + std::string(ex.what()));
  }

  if (json_buffer.size() == 0) {
    throw std::runtime_error("Empty file: " + json_path.string());
  }

  const auto file_size = static_cast<std::size_t>(json_buffer.size());
  const std::string_view ingest_payload(json_buffer.data(), json_buffer.size());

    EnrichmentObservationFixture fixture{};
    fixture.memory = FastFHIR::Memory::create(
      std::max<std::size_t>(4096, file_size * static_cast<std::size_t>(2)));
  const FastFHIR::FF_Builder builder_handle = make_builder(fixture.memory);
  FastFHIR::Ingest::Ingestor ingestor;
  FastFHIR::Reflective::ObjectHandle root(builder_handle.get(), FF_NULL_OFFSET);
  size_t parsed_count = 0;

  FF_Result result{FF_FAILURE};
  try {
    FastFHIR::Ingest::IngestRequest request{
        .builder = *builder_handle,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN,
        .json_string = ingest_payload,
        .payload_capacity = json_buffer.size() + simdjson::SIMDJSON_PADDING,
    };
    result = ingestor.ingest(request, root, parsed_count);
  } catch (const std::exception& ex) {
    throw std::runtime_error("Ingestor exception for " + json_path.string() + ": " + ex.what());
  }

  if (!result || !root) {
    throw std::runtime_error(
        "Ingestor failed for " + json_path.string() + ": code="
        + std::to_string(static_cast<int>(result.code)) + ", message=" + result.message);
  }

  const auto root_node = root.as_node();

  if (!(root_node && root_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())) {
    throw std::runtime_error("Expected Observation resource in " + json_path.string());
  }

  // Seal the fixture's arena so the observation is a valid standalone stream:
  // enrich_fastfhir re-reads it via Parser (the in-memory lab observation)
  // for the live-stream append. Without the seal the arena has no FFHR header
  // and Parser throws "FF_HEADER magic bytes mismatch".
  (void)seal_stream(builder_handle, root, "enrichment observation fixture");

  fixture.observation = root_node.as<ObservationData>();
  return fixture;
}

}  // namespace bench
