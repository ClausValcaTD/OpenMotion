// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_RENDERER_HPP
#define OPENMOTION_RENDERER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "openmotion/project.hpp"

namespace openmotion::renderer {

struct Frame {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // size = width*height*4, row-major, opaque
};

struct RenderError {
    std::string message;
};

// Renders the composition at time t_ms. Layers are drawn bottom to top; only
// layers where keyframe::is_layer_active() is true are drawn. The canvas
// starts opaque black.
std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms);

// Renders every frame of the project and pipes it to ffmpeg.
// Returns std::nullopt on success.
std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path);

}  // namespace openmotion::renderer

#endif  // OPENMOTION_RENDERER_HPP
