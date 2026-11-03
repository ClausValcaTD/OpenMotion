// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "internal/shell_quote.hpp"
#include "openmotion/keyframe.hpp"

namespace openmotion::renderer {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp_alpha(double value) {
    if (!(value > 0.0)) return 0.0;  // also maps NaN to 0
    if (value > 1.0) return 1.0;
    return value;
}

// Parses "#RRGGBB". Returns false for anything else.
bool parse_color(const std::string& text, uint8_t& red, uint8_t& green, uint8_t& blue) {
    if (text.size() != 7 || text[0] != '#') {
        return false;
    }
    const auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int digits[6];
    for (int i = 0; i < 6; ++i) {
        digits[i] = hex_value(text[static_cast<std::size_t>(i) + 1]);
        if (digits[i] < 0) {
            return false;
        }
    }
    red = static_cast<uint8_t>(digits[0] * 16 + digits[1]);
    green = static_cast<uint8_t>(digits[2] * 16 + digits[3]);
    blue = static_cast<uint8_t>(digits[4] * 16 + digits[5]);
    return true;
}

uint8_t blend_channel(uint8_t source, uint8_t destination, double alpha, double inverse) {
    const double value =
        static_cast<double>(source) * alpha + static_cast<double>(destination) * inverse;
    if (value <= 0.0) return 0;
    if (value >= 255.0) return 255;
    return static_cast<uint8_t>(std::lround(value));
}

// Source-over blend of an opaque layer colour onto the opaque canvas.
void blend_pixel(Frame& frame, int x, int y, uint8_t red, uint8_t green, uint8_t blue,
                 double alpha) {
    if (alpha <= 0.0) return;
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return;
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
         static_cast<std::size_t>(x)) *
        4;
    const double inverse = 1.0 - alpha;
    frame.rgba[index + 0] = blend_channel(red, frame.rgba[index + 0], alpha, inverse);
    frame.rgba[index + 1] = blend_channel(green, frame.rgba[index + 1], alpha, inverse);
    frame.rgba[index + 2] = blend_channel(blue, frame.rgba[index + 2], alpha, inverse);
    frame.rgba[index + 3] = 255;
}

void draw_solid(Frame& frame, uint8_t red, uint8_t green, uint8_t blue, double alpha) {
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            blend_pixel(frame, x, y, red, green, blue, alpha);
        }
    }
}

// Converts a (possibly non-finite) coordinate into a clamped canvas index.
int clamp_pixel(double value, int limit) {
    if (std::isnan(value)) return 0;
    if (value <= 0.0) return 0;
    if (value >= static_cast<double>(limit)) return limit;
    return static_cast<int>(value);
}

void draw_rect(Frame& frame, double center_x, double center_y, double width, double height,
               double scale_x, double scale_y, double rotation_deg, uint8_t red, uint8_t green,
               uint8_t blue, double alpha) {
    const double half_w = width * scale_x * 0.5;
    const double half_h = height * scale_y * 0.5;
    const double radians = rotation_deg * kPi / 180.0;
    const double cos_a = std::cos(radians);
    const double sin_a = std::sin(radians);

    const double corner_x[4] = {half_w, half_w, -half_w, -half_w};
    const double corner_y[4] = {half_h, -half_h, half_h, -half_h};

    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 4; ++i) {
        const double px = center_x + cos_a * corner_x[i] - sin_a * corner_y[i];
        const double py = center_y + sin_a * corner_x[i] + cos_a * corner_y[i];
        min_x = std::min(min_x, px);
        max_x = std::max(max_x, px);
        min_y = std::min(min_y, py);
        max_y = std::max(max_y, py);
    }

    const int x_begin = clamp_pixel(std::floor(min_x), frame.width);
    const int x_end = clamp_pixel(std::ceil(max_x), frame.width);
    const int y_begin = clamp_pixel(std::floor(min_y), frame.height);
    const int y_end = clamp_pixel(std::ceil(max_y), frame.height);

    const double abs_half_w = std::abs(half_w);
    const double abs_half_h = std::abs(half_h);

    for (int y = y_begin; y < y_end; ++y) {
        for (int x = x_begin; x < x_end; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - center_x;
            const double dy = (static_cast<double>(y) + 0.5) - center_y;
            const double local_x = cos_a * dx + sin_a * dy;
            const double local_y = -sin_a * dx + cos_a * dy;
            if (std::abs(local_x) <= abs_half_w && std::abs(local_y) <= abs_half_h) {
                blend_pixel(frame, x, y, red, green, blue, alpha);
            }
        }
    }
}

// Ignoring SIGPIPE lets a broken ffmpeg pipe surface as a short fwrite rather
// than killing the process.
class ScopedSigpipeIgnore {
public:
    ScopedSigpipeIgnore() : previous_(std::signal(SIGPIPE, SIG_IGN)) {}
    ~ScopedSigpipeIgnore() {
        if (previous_ != SIG_ERR) {
            std::signal(SIGPIPE, previous_);
        }
    }
    ScopedSigpipeIgnore(const ScopedSigpipeIgnore&) = delete;
    ScopedSigpipeIgnore& operator=(const ScopedSigpipeIgnore&) = delete;

private:
    using Handler = void (*)(int);
    Handler previous_;
};

struct PipeCloser {
    void operator()(std::FILE* pipe) const {
        if (pipe != nullptr) {
            pclose(pipe);
        }
    }
};

std::string format_fps(double fps) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", fps);
    return std::string(buffer);
}

}  // namespace

std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms) {
    if (p.width <= 0 || p.height <= 0) {
        return RenderError{"invalid canvas size"};
    }
    const int64_t pixel_count = static_cast<int64_t>(p.width) * static_cast<int64_t>(p.height);
    if (pixel_count <= 0 || pixel_count > (int64_t{1} << 28)) {
        return RenderError{"canvas is too large"};
    }

    Frame frame;
    frame.width = p.width;
    frame.height = p.height;
    try {
        frame.rgba.assign(static_cast<std::size_t>(pixel_count) * 4, 0);
    } catch (const std::exception& error) {
        return RenderError{std::string{"cannot allocate frame buffer: "} + error.what()};
    }
    for (std::size_t i = 3; i < frame.rgba.size(); i += 4) {
        frame.rgba[i] = 255;  // opaque black canvas
    }

    for (const project::Layer& layer : p.layers) {
        if (!keyframe::is_layer_active(layer, t_ms)) {
            continue;
        }
        if (layer.type != project::LayerType::Solid && layer.type != project::LayerType::Rect) {
            continue;  // text / video / image layers are not rendered yet
        }

        uint8_t red = 0, green = 0, blue = 0;
        if (!parse_color(layer.color, red, green, blue)) {
            return RenderError{"invalid color '" + layer.color + "' on layer " + layer.id};
        }

        const keyframe::EvaluatedTransform transform =
            keyframe::evaluate_transform(layer.transform, t_ms);
        const double alpha = clamp_alpha(transform.opacity);

        if (layer.type == project::LayerType::Solid) {
            draw_solid(frame, red, green, blue, alpha);
        } else if (layer.width > 0.0 && layer.height > 0.0) {
            draw_rect(frame, transform.x, transform.y, layer.width, layer.height, transform.scale_x,
                      transform.scale_y, transform.rotation, red, green, blue, alpha);
        }
    }
    return frame;
}

std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path) {
    if (!(p.fps > 0.0) || !std::isfinite(p.fps)) {
        return RenderError{"invalid fps"};
    }
    if (p.duration_ms <= 0) {
        return RenderError{"invalid duration"};
    }
    // Guard the double->int64 conversion: a huge duration*fps product is UB.
    const double frames_exact =
        static_cast<double>(p.duration_ms) * p.fps / 1000.0;
    if (!std::isfinite(frames_exact) || frames_exact > 1.0e9) {
        return RenderError{"project produces too many frames"};
    }
    const int64_t frame_count = static_cast<int64_t>(std::floor(frames_exact));
    if (frame_count <= 0) {
        return RenderError{"project produces no frames"};
    }

    const std::string command =
        "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s " + std::to_string(p.width) + "x" +
        std::to_string(p.height) + " -r " + format_fps(p.fps) +
        " -i - -an -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -pix_fmt yuv420p " +
        internal::shell_quote(out_path);

    ScopedSigpipeIgnore sigpipe_guard;
    std::unique_ptr<std::FILE, PipeCloser> pipe(popen(command.c_str(), "w"));
    if (!pipe) {
        return RenderError{"failed to run ffmpeg"};
    }

    for (int64_t i = 0; i < frame_count; ++i) {
        const int64_t t_ms =
            static_cast<int64_t>(std::llround(static_cast<double>(i) * 1000.0 / p.fps));
        const std::variant<Frame, RenderError> rendered = render_frame(p, t_ms);
        if (const auto* error = std::get_if<RenderError>(&rendered)) {
            return *error;
        }
        const Frame& frame = std::get<Frame>(rendered);
        if (std::fwrite(frame.rgba.data(), 1, frame.rgba.size(), pipe.get()) != frame.rgba.size()) {
            return RenderError{"failed to write frame to ffmpeg"};
        }
    }

    const int status = pclose(pipe.release());
    if (status != 0) {
        return RenderError{"ffmpeg exited with a non-zero status"};
    }
    return std::nullopt;
}

}  // namespace openmotion::renderer
