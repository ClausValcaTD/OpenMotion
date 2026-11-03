// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_RENDERER_HPP
#define OPENMOTION_RENDERER_HPP

#include <cstdint>
#include <map>
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

struct Texture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // straight (non-premultiplied) alpha
};

struct RenderError {
    std::string message;
};

struct RenderCache {
    std::string base_dir;                   // for relative sources
    std::map<std::string, Texture> images;  // key = layer.source
};

// Renders the composition at time t_ms. Layers are drawn bottom to top; only
// layers where keyframe::is_layer_active() is true are drawn. The canvas
// starts opaque black.
std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms);

// Same as above, but image layers are resolved and decoded through `cache` so a
// single texture is loaded once and reused across frames.
std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms,
                                              RenderCache& cache);

// Renders every frame of the project and pipes it to ffmpeg, creating one
// RenderCache and reusing it for all frames.
// Returns std::nullopt on success.
std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path);

// Same as above, but uses the caller's `cache` so relative image sources can be
// resolved against cache.base_dir.
std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path,
                                         RenderCache& cache);

}  // namespace openmotion::renderer

#endif  // OPENMOTION_RENDERER_HPP
