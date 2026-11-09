// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "openmotion/am_import.hpp"
#include "openmotion/project.hpp"

namespace {

namespace am_import = openmotion::am_import;
namespace project = openmotion::project;

const char* severity_label(am_import::Issue::Severity severity) {
    switch (severity) {
        case am_import::Issue::Severity::Info: return "info";
        case am_import::Issue::Severity::Warning: return "warning";
        case am_import::Issue::Severity::Skipped: return "skipped";
    }
    return "info";
}

// The same file, even through a symlink or a different path spelling.
bool same_file(const std::string& a, const std::string& b) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(a, b, error);
    if (!error) {
        return equivalent;
    }
    return std::filesystem::absolute(a) == std::filesystem::absolute(b);
}

void print_report(std::ostream& out, const am_import::ImportReport& report) {
    std::size_t info = 0;
    std::size_t warning = 0;
    std::size_t skipped = 0;
    for (const am_import::Issue& issue : report.issues) {
        switch (issue.severity) {
            case am_import::Issue::Severity::Info: ++info; break;
            case am_import::Issue::Severity::Warning: ++warning; break;
            case am_import::Issue::Severity::Skipped: ++skipped; break;
        }
    }
    out << "import report: " << info << " info, " << warning << " warning, " << skipped
        << " skipped\n";
    for (const am_import::Issue& issue : report.issues) {
        if (issue.severity == am_import::Issue::Severity::Info) {
            continue;
        }
        out << "  " << severity_label(issue.severity);
        if (!issue.layer.empty()) {
            out << " [" << issue.layer << "]";
        }
        out << ": " << issue.message << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    std::string report_path;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--report") {
            if (i + 1 >= argc) {
                std::cerr << "om-import-am: --report needs a path\n";
                return 1;
            }
            report_path = argv[++i];
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        std::cerr << "usage: om-import-am <input.xml> <output.omproj> [--report report.json]\n";
        return 1;
    }
    input_path = positional[0];
    output_path = positional[1];

    if (same_file(input_path, output_path)) {
        std::cerr << "om-import-am: refusing to overwrite the input file\n";
        return 1;
    }

    const std::variant<am_import::ImportResult, am_import::ImportError> imported =
        am_import::import_am_file(input_path);
    if (const auto* error = std::get_if<am_import::ImportError>(&imported)) {
        std::cerr << "om-import-am: " << input_path << ": " << error->message << "\n";
        return 1;
    }
    const am_import::ImportResult& result = std::get<am_import::ImportResult>(imported);

    const std::optional<project::ProjectError> saved =
        project::save_to_file(result.project, output_path);
    if (saved.has_value()) {
        std::cerr << "om-import-am: " << saved->message << "\n";
        return 1;
    }

    print_report(std::cerr, result.report);

    if (!report_path.empty()) {
        std::ofstream report_file(report_path, std::ios::binary | std::ios::trunc);
        if (!report_file) {
            std::cerr << "om-import-am: cannot open report for writing: " << report_path << "\n";
            return 1;
        }
        report_file << am_import::report_to_json(result.report) << '\n';
        if (!report_file) {
            std::cerr << "om-import-am: failed to write report: " << report_path << "\n";
            return 1;
        }
    }

    std::cout << "wrote " << output_path << " (" << result.project.layers.size() << " layers)\n";
    return 0;
}
