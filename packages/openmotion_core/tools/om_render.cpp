// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

#include "openmotion/project.hpp"
#include "openmotion/renderer.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: om-render <input.omproj> <output.mp4>\n";
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];

    const std::variant<openmotion::project::Project, openmotion::project::ProjectError> loaded =
        openmotion::project::load_from_file(input_path);
    if (const auto* error = std::get_if<openmotion::project::ProjectError>(&loaded)) {
        std::cerr << "om-render: " << input_path << ": " << error->message << "\n";
        return 1;
    }
    const openmotion::project::Project& project = std::get<openmotion::project::Project>(loaded);

    const std::optional<openmotion::renderer::RenderError> result =
        openmotion::renderer::render_to_mp4(project, output_path);
    if (result.has_value()) {
        std::cerr << "om-render: " << result->message << "\n";
        return 1;
    }

    const int64_t frame_count = static_cast<int64_t>(
        std::floor(static_cast<double>(project.duration_ms) * project.fps / 1000.0));
    std::cout << "wrote " << output_path << " (" << frame_count << " frames)\n";
    return 0;
}
