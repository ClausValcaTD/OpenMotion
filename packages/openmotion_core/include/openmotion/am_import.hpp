// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_AM_IMPORT_HPP
#define OPENMOTION_AM_IMPORT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "openmotion/project.hpp"

// Importer for Alight Motion XML exports (best effort, v0).
//
// The converter reads a `<scene>` document and maps what it understands onto an
// OpenMotion `project::Project`. Everything it cannot represent is reported in
// an `ImportReport` rather than aborting the import. The parser treats every
// attribute and text node as untrusted data: nothing is executed, evaluated or
// loaded from the file.
//
// See `docs/AM_XML_NOTES.md` for the verified structure of the source format and
// which mapping rules are implemented.
namespace openmotion::am_import {

// One thing worth telling the user about the import. `layer` is the layer's
// label or id (may be empty for scene-level issues); `message` is free text.
struct Issue {
    enum class Severity { Info, Warning, Skipped };
    Severity severity;
    std::string layer;
    std::string message;
};

struct ImportReport {
    std::vector<Issue> issues;
};

struct ImportError {
    std::string message;
};

struct ImportResult {
    project::Project project;
    ImportReport report;
};

// Parses `xml_text` and converts it. Never throws: malformed XML, a wrong root
// element, an out-of-range scene, and every other failure come back as an
// ImportError.
std::variant<ImportResult, ImportError> import_am_xml(const std::string& xml_text);

// Reads `path` and calls import_am_xml on its contents.
std::variant<ImportResult, ImportError> import_am_file(const std::string& path);

// Serializes the report to JSON (nlohmann, indent 2).
std::string report_to_json(const ImportReport& report);

// The input size limit, in bytes (50 MB). Exposed so tests can exercise the
// check without allocating the whole limit.
constexpr std::size_t kMaxInputBytes = 50u * 1024u * 1024u;

// The layer-count limit.
constexpr std::size_t kMaxLayers = 5000u;

// The keyframe-count limit for a single property.
constexpr std::size_t kMaxKeyframesPerProperty = 10000u;

// True when an input of `size` bytes is within `limit`. A separate function so
// the size check can be unit-tested with a small limit instead of a 50 MB
// allocation; import_am_xml calls it with the default.
bool input_size_ok(std::size_t size, std::size_t limit = kMaxInputBytes);

}  // namespace openmotion::am_import

#endif  // OPENMOTION_AM_IMPORT_HPP
