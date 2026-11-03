// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_INTERNAL_IMAGE_LOADER_HPP
#define OPENMOTION_INTERNAL_IMAGE_LOADER_HPP

#include <cstdint>
#include <string>
#include <variant>

#include "openmotion/renderer.hpp"

namespace openmotion::internal {

// Largest supported texture edge, and largest supported decoded size in bytes.
constexpr int kMaxImageDimension = 16384;
constexpr int64_t kMaxImageBytes = 512LL * 1024 * 1024;

// True when a width x height image is within the supported limits (positive,
// no edge over kMaxImageDimension, total RGBA size at most kMaxImageBytes).
bool is_supported_image_size(int width, int height);

// Decodes the image at `path` to straight-alpha RGBA by running the external
// ffprobe/ffmpeg executables. Never throws: every failure (missing file,
// missing tools, oversized image, short read) is reported as a RenderError
// whose message includes the path.
std::variant<renderer::Texture, renderer::RenderError> load_image(const std::string& path);

}  // namespace openmotion::internal

#endif  // OPENMOTION_INTERNAL_IMAGE_LOADER_HPP
