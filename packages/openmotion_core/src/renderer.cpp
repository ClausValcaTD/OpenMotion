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
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "internal/audio_mix.hpp"
#include "internal/blend.hpp"
#include "internal/image_loader.hpp"
#include "internal/shell_quote.hpp"
#include "internal/video_stream.hpp"
#include "openmotion/keyframe.hpp"

namespace openmotion::renderer {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp_unit(double value) {
    if (!(value > 0.0)) return 0.0;  // also maps NaN to 0
    if (value > 1.0) return 1.0;
    return value;
}

double clamp_alpha(double value) { return clamp_unit(value); }

// Resolves a layer source: an absolute path is used as is, otherwise it is
// joined onto the cache base directory (when one is set).
std::string resolve_source(const RenderCache& cache, const std::string& source) {
    if (source.empty() || source.front() == '/' || cache.base_dir.empty()) {
        return source;
    }
    std::string joined = cache.base_dir;
    if (joined.back() != '/') {
        joined += '/';
    }
    joined += source;
    return joined;
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

// Source-over blend of one straight-alpha channel pair onto an opaque canvas.
double blend_channel(double source, double destination, double alpha) {
    const double value = source * alpha + destination * (1.0 - alpha);
    if (value <= 0.0) return 0.0;
    if (value >= 255.0) return 255.0;
    return value;
}

// One channel (0..255) of a layer pixel blended onto the opaque canvas.
// `alpha` is the effective coverage (opacity * coverage * texture alpha) and
// `mode` the layer's blend mode. Normal keeps the original source-over
// formula byte-for-byte; every other mode mixes the separable blend function
// in [0,1] and scales back to 0..255.
double blend_over(double backdrop, double source, double alpha, project::BlendMode mode) {
    if (mode == project::BlendMode::Normal) {
        return blend_channel(source, backdrop, alpha);
    }
    const double cb = backdrop / 255.0;
    const double cs = source / 255.0;
    const double value = ((1.0 - alpha) * cb + alpha * internal::blend_channel(mode, cb, cs)) * 255.0;
    if (value <= 0.0) return 0.0;
    if (value >= 255.0) return 255.0;
    return value;
}

// Source-over blend of an opaque layer colour onto the opaque canvas.
void blend_pixel(Frame& frame, int x, int y, uint8_t red, uint8_t green, uint8_t blue,
                 double alpha, project::BlendMode mode) {
    if (alpha <= 0.0) return;
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return;
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
         static_cast<std::size_t>(x)) *
        4;
    frame.rgba[index + 0] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 0], red, alpha, mode)));
    frame.rgba[index + 1] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 1], green, alpha, mode)));
    frame.rgba[index + 2] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 2], blue, alpha, mode)));
    frame.rgba[index + 3] = 255;
}

// Source-over blend of a straight-alpha texture sample (0..255 channels) onto
// the opaque canvas. `alpha` already folds in the texture alpha and opacity.
void blend_sample(Frame& frame, int x, int y, double red, double green, double blue,
                  double alpha, project::BlendMode mode) {
    if (alpha <= 0.0) return;
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return;
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
         static_cast<std::size_t>(x)) *
        4;
    frame.rgba[index + 0] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 0], red, alpha, mode)));
    frame.rgba[index + 1] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 1], green, alpha, mode)));
    frame.rgba[index + 2] = static_cast<uint8_t>(
        std::lround(blend_over(frame.rgba[index + 2], blue, alpha, mode)));
    frame.rgba[index + 3] = 255;
}

void draw_solid(Frame& frame, uint8_t red, uint8_t green, uint8_t blue, double alpha,
                project::BlendMode mode) {
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            blend_pixel(frame, x, y, red, green, blue, alpha, mode);
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

// Axis-aligned bounding box of a rectangle scaled, rotated (clockwise) and
// centred on (center_x, center_y), plus the inverse-rotation parameters used to
// test each pixel centre. Shared by the Rect and Image draw paths.
struct Quad {
    double center_x = 0.0;
    double center_y = 0.0;
    double cos_a = 1.0;
    double sin_a = 0.0;
    double half_w = 0.0;  // signed, so a negative scale mirrors the texture
    double half_h = 0.0;
    double half_w_px = 0.0;  // |half_w| in output pixels, for edge coverage
    double half_h_px = 0.0;
    int x_begin = 0;
    int x_end = 0;
    int y_begin = 0;
    int y_end = 0;
};

Quad make_quad(const Frame& frame, double center_x, double center_y, double width, double height,
               double scale_x, double scale_y, double rotation_deg) {
    Quad quad;
    quad.center_x = center_x;
    quad.center_y = center_y;
    quad.half_w = width * scale_x * 0.5;
    quad.half_h = height * scale_y * 0.5;
    quad.half_w_px = std::abs(quad.half_w);
    quad.half_h_px = std::abs(quad.half_h);

    const double radians = rotation_deg * kPi / 180.0;
    quad.cos_a = std::cos(radians);
    quad.sin_a = std::sin(radians);

    const double corner_x[4] = {quad.half_w, quad.half_w, -quad.half_w, -quad.half_w};
    const double corner_y[4] = {quad.half_h, -quad.half_h, quad.half_h, -quad.half_h};

    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 4; ++i) {
        const double px = center_x + quad.cos_a * corner_x[i] - quad.sin_a * corner_y[i];
        const double py = center_y + quad.sin_a * corner_x[i] + quad.cos_a * corner_y[i];
        min_x = std::min(min_x, px);
        max_x = std::max(max_x, px);
        min_y = std::min(min_y, py);
        max_y = std::max(max_y, py);
    }

    // Expand by one pixel so the partially covered edge pixels are visited too.
    quad.x_begin = clamp_pixel(std::floor(min_x) - 1.0, frame.width);
    quad.x_end = clamp_pixel(std::ceil(max_x) + 1.0, frame.width);
    quad.y_begin = clamp_pixel(std::floor(min_y) - 1.0, frame.height);
    quad.y_end = clamp_pixel(std::ceil(max_y) + 1.0, frame.height);
    return quad;
}

// Visits every canvas pixel near the transformed quad, passing the pixel centre
// in the quad's unrotated local frame and its analytic edge coverage in [0,1].
// Coverage is the product of the per-axis coverage of the two edges; it is 1
// for interior pixels and falls to 0 within half a pixel outside the edge, so
// rotated and scaled edges are anti-aliased without supersampling.
template <typename Fn>
void for_each_quad_pixel(const Quad& quad, Fn&& visit) {
    for (int y = quad.y_begin; y < quad.y_end; ++y) {
        for (int x = quad.x_begin; x < quad.x_end; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - quad.center_x;
            const double dy = (static_cast<double>(y) + 0.5) - quad.center_y;
            const double local_x = quad.cos_a * dx + quad.sin_a * dy;
            const double local_y = -quad.sin_a * dx + quad.cos_a * dy;
            const double cov_x =
                clamp_unit(quad.half_w_px - std::abs(local_x) + 0.5);
            const double cov_y =
                clamp_unit(quad.half_h_px - std::abs(local_y) + 0.5);
            const double coverage = cov_x * cov_y;
            if (coverage > 0.0) {
                visit(x, y, local_x, local_y, coverage);
            }
        }
    }
}

void draw_rect(Frame& frame, double center_x, double center_y, double width, double height,
               double scale_x, double scale_y, double rotation_deg, uint8_t red, uint8_t green,
               uint8_t blue, double alpha, project::BlendMode mode) {
    const Quad quad =
        make_quad(frame, center_x, center_y, width, height, scale_x, scale_y, rotation_deg);
    for_each_quad_pixel(quad, [&](int x, int y, double, double, double coverage) {
        blend_pixel(frame, x, y, red, green, blue, alpha * coverage, mode);
    });
}

int clamp_texel(double value, int dimension) {
    if (std::isnan(value)) return 0;
    if (value <= 0.0) return 0;
    if (value >= static_cast<double>(dimension - 1)) return dimension - 1;
    return static_cast<int>(value);
}

struct Sample {
    double red = 0.0;    // premultiplied by alpha/255
    double green = 0.0;  // premultiplied by alpha/255
    double blue = 0.0;   // premultiplied by alpha/255
    double alpha = 0.0;
};

double texel_channel(const Texture& texture, int x, int y, int channel) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(texture.width) +
         static_cast<std::size_t>(x)) *
            4 +
        static_cast<std::size_t>(channel);
    return static_cast<double>(texture.rgba[index]);
}

// Bilinear weights of a continuous coordinate: the lower texel index (clamped
// to the edge) and its fractional weight.
struct AxisWeights {
    int low = 0;
    int high = 0;
    double frac = 0.0;
};

AxisWeights axis_weights(double texel, int dimension) {
    const double base = std::floor(texel - 0.5);
    AxisWeights weights;
    weights.low = clamp_texel(base, dimension);
    weights.high = clamp_texel(base + 1.0, dimension);
    weights.frac = (texel - 0.5) - base;
    return weights;
}

// Filters one straight channel (used for alpha) over the four texels around
// (tx, ty).
double sample_channel(const Texture& texture, const AxisWeights& x_weights,
                      const AxisWeights& y_weights, int channel) {
    const double top = texel_channel(texture, x_weights.low, y_weights.low, channel) *
                           (1.0 - x_weights.frac) +
                       texel_channel(texture, x_weights.high, y_weights.low, channel) *
                           x_weights.frac;
    const double bottom = texel_channel(texture, x_weights.low, y_weights.high, channel) *
                              (1.0 - x_weights.frac) +
                          texel_channel(texture, x_weights.high, y_weights.high, channel) *
                              x_weights.frac;
    return top * (1.0 - y_weights.frac) + bottom * y_weights.frac;
}

// Filters one colour channel with every texel premultiplied by its own alpha,
// so fully transparent texels contribute no colour to the result.
double sample_premultiplied_channel(const Texture& texture, const AxisWeights& x_weights,
                                    const AxisWeights& y_weights, int channel) {
    const auto weighted = [&](int x, int y) {
        return texel_channel(texture, x, y, channel) * texel_channel(texture, x, y, 3) / 255.0;
    };
    const double top = weighted(x_weights.low, y_weights.low) * (1.0 - x_weights.frac) +
                       weighted(x_weights.high, y_weights.low) * x_weights.frac;
    const double bottom = weighted(x_weights.low, y_weights.high) * (1.0 - x_weights.frac) +
                          weighted(x_weights.high, y_weights.high) * x_weights.frac;
    return top * (1.0 - y_weights.frac) + bottom * y_weights.frac;
}

// Bilinear sample at continuous texel coordinates (tx, ty) in
// [0, width) x [0, height), clamped to the edge. Filtering happens in
// premultiplied alpha and the colour is un-premultiplied afterwards, so a
// fully transparent texel never bleeds its (undefined) colour into opaque
// neighbours -- the dark or coloured halos seen around transparent PNG edges.
Sample sample_bilinear(const Texture& texture, double tx, double ty) {
    const AxisWeights x_weights = axis_weights(tx, texture.width);
    const AxisWeights y_weights = axis_weights(ty, texture.height);

    const double alpha = sample_channel(texture, x_weights, y_weights, 3);
    Sample sample;
    sample.alpha = alpha;
    if (alpha <= 0.0) {
        return sample;  // fully transparent: colour is irrelevant
    }
    const double scale = 255.0 / alpha;  // un-premultiply
    sample.red = sample_premultiplied_channel(texture, x_weights, y_weights, 0) * scale;
    sample.green = sample_premultiplied_channel(texture, x_weights, y_weights, 1) * scale;
    sample.blue = sample_premultiplied_channel(texture, x_weights, y_weights, 2) * scale;
    return sample;
}

void draw_image(Frame& frame, const Texture& texture, double center_x, double center_y,
                double width, double height, double scale_x, double scale_y, double rotation_deg,
                double opacity, project::BlendMode mode) {
    if (texture.width <= 0 || texture.height <= 0) {
        return;
    }
    const Quad quad =
        make_quad(frame, center_x, center_y, width, height, scale_x, scale_y, rotation_deg);
    if (quad.half_w == 0.0 || quad.half_h == 0.0) {
        return;
    }
    for_each_quad_pixel(quad, [&](int x, int y, double local_x, double local_y, double coverage) {
        const double u = (local_x + quad.half_w) / (2.0 * quad.half_w);
        const double v = (local_y + quad.half_h) / (2.0 * quad.half_h);
        const Sample sample =
            sample_bilinear(texture, u * static_cast<double>(texture.width),
                            v * static_cast<double>(texture.height));
        blend_sample(frame, x, y, sample.red, sample.green, sample.blue,
                     sample.alpha / 255.0 * opacity * coverage, mode);
    });
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

// Runs `command` and captures stdout, returning nullopt when the process cannot
// be started or exits non-zero.
std::optional<std::string> run_command(const std::string& command) {
    std::unique_ptr<std::FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        return std::nullopt;
    }
    std::string output;
    char buffer[256];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe.get())) != 0) {
        output.append(buffer, read);
    }
    if (pclose(pipe.release()) != 0) {
        return std::nullopt;
    }
    return output;
}

// True when `path` exists and its first audio stream is a decodable audio
// stream. Used to fail early (before ffmpeg starts) with a path-bearing error.
bool has_audio_stream(const std::string& path) {
    const std::string command =
        "ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of json " +
        internal::shell_quote(path);
    const std::optional<std::string> output = run_command(command);
    if (!output.has_value()) {
        return false;
    }
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*output);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!document.contains("streams") || !document["streams"].is_array() ||
        document["streams"].empty()) {
        return false;
    }
    const nlohmann::json& stream = document["streams"].front();
    return stream.contains("codec_type") && stream["codec_type"].is_string() &&
           stream["codec_type"].get<std::string>() == "audio";
}

// Renders `value` milliseconds as a locale-independent seconds string, e.g.
// 500 -> "0.500".
std::string format_ms_as_seconds(int64_t value) {
    return internal::format_fixed(static_cast<double>(value) / 1000.0, 3);
}

// Asks the installed ffmpeg whether it can encode with libx264. Any failure
// (ffmpeg missing, non-zero exit, no match) simply reports "not available".
bool detect_libx264() {
    const std::optional<std::string> output =
        run_command("ffmpeg -hide_banner -encoders 2>/dev/null");
    return output.has_value() && output->find("libx264") != std::string::npos;
}

// Detection runs at most once per process; the result is cached.
bool have_libx264() {
    static const bool detected = detect_libx264();
    return detected;
}

}  // namespace

std::string build_video_codec_args(bool have_libx264) {
    // With libx264 available, pin the encoder and quality explicitly. Without
    // it, add nothing and let ffmpeg pick its default encoder, exactly as the
    // renderer did before. The fragment is space-delimited so it splices
    // cleanly between the other arguments and the output path.
    if (have_libx264) {
        return " -c:v libx264 -preset veryfast -crf 18 -movflags +faststart ";
    }
    return "";
}

std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms) {
    RenderCache cache;
    return render_frame(p, t_ms, cache);
}

std::variant<Frame, RenderError> render_frame(const project::Project& p, int64_t t_ms,
                                              RenderCache& cache) {
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

        const keyframe::EvaluatedTransform transform =
            keyframe::evaluate_transform(layer.transform, t_ms);
        const double alpha = clamp_alpha(transform.opacity);

        if (layer.type == project::LayerType::Image) {
            const Texture* texture = nullptr;
            if (const auto cached = cache.images.find(layer.source); cached != cache.images.end()) {
                texture = &cached->second;
            } else {
                std::variant<Texture, RenderError> loaded =
                    internal::load_image(resolve_source(cache, layer.source));
                if (const auto* error = std::get_if<RenderError>(&loaded)) {
                    return *error;
                }
                texture = &cache.images.emplace(layer.source, std::move(std::get<Texture>(loaded)))
                               .first->second;
            }

            const double width =
                layer.width > 0.0 && layer.height > 0.0 ? layer.width : texture->width;
            const double height =
                layer.width > 0.0 && layer.height > 0.0 ? layer.height : texture->height;
            draw_image(frame, *texture, transform.x, transform.y, width, height, transform.scale_x,
                       transform.scale_y, transform.rotation, alpha, layer.blend);
            continue;
        }

        // Video layers: layer.source is a video file (resolved against
        // cache.base_dir like images). The clip starts at layer.start_ms; the
        // source time for timeline time t_ms is (t_ms - start_ms), with no trim
        // or speed. The frame index is round((t_ms - start_ms) * project_fps /
        // 1000) and ffmpeg resamples the source to the project fps. Geometry is
        // identical to Image layers; when the clip ends the last frame is held.
        if (layer.type == project::LayerType::Video) {
            auto stream_it = cache.videos.find(layer.id);
            if (stream_it == cache.videos.end()) {
                std::variant<std::unique_ptr<internal::VideoStream>, RenderError> opened =
                    internal::VideoStream::open(resolve_source(cache, layer.source), p.fps);
                if (const auto* error = std::get_if<RenderError>(&opened)) {
                    return *error;
                }
                std::shared_ptr<internal::VideoStream> stream =
                    std::move(std::get<std::unique_ptr<internal::VideoStream>>(opened));
                stream_it = cache.videos.emplace(layer.id, std::move(stream)).first;
            }

            const int64_t wanted_index =
                internal::video_frame_index(layer.start_ms, t_ms, p.fps);
            std::variant<internal::VideoFrame, RenderError> decoded =
                stream_it->second->frame_at(wanted_index);
            if (const auto* error = std::get_if<RenderError>(&decoded)) {
                return *error;
            }
            internal::VideoFrame source_frame =
                std::move(std::get<internal::VideoFrame>(decoded));

            Texture texture;
            texture.width = source_frame.width;
            texture.height = source_frame.height;
            texture.rgba = std::move(source_frame.rgba);

            const bool explicit_size = layer.width > 0.0 && layer.height > 0.0;
            const double width = explicit_size ? layer.width : texture.width;
            const double height = explicit_size ? layer.height : texture.height;
            draw_image(frame, texture, transform.x, transform.y, width, height, transform.scale_x,
                       transform.scale_y, transform.rotation, alpha, layer.blend);
            continue;
        }

        // Audio layers carry no pixels; they are mixed into the output by
        // render_to_mp4 instead of being drawn.
        if (layer.type == project::LayerType::Audio) {
            continue;
        }

        if (layer.type != project::LayerType::Solid && layer.type != project::LayerType::Rect) {
            continue;  // text layers are not rendered yet
        }

        uint8_t red = 0, green = 0, blue = 0;
        if (!parse_color(layer.color, red, green, blue)) {
            return RenderError{"invalid color '" + layer.color + "' on layer " + layer.id};
        }

        if (layer.type == project::LayerType::Solid) {
            draw_solid(frame, red, green, blue, alpha, layer.blend);
        } else if (layer.width > 0.0 && layer.height > 0.0) {
            draw_rect(frame, transform.x, transform.y, layer.width, layer.height, transform.scale_x,
                      transform.scale_y, transform.rotation, red, green, blue, alpha, layer.blend);
        }
    }
    return frame;
}

std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path) {
    RenderCache cache;
    return render_to_mp4(p, out_path, cache);
}

std::optional<RenderError> render_to_mp4(const project::Project& p, const std::string& out_path,
                                         RenderCache& cache) {
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

    // Collect and validate the audio layers before starting ffmpeg, so a missing
    // file or a file without audio fails fast with an error that names the path.
    std::vector<internal::AudioInput> audio_inputs;
    for (const project::Layer& layer : p.layers) {
        if (layer.type != project::LayerType::Audio) {
            continue;
        }
        const std::string path = resolve_source(cache, layer.source);
        if (!has_audio_stream(path)) {
            return RenderError{"audio layer " + layer.id + ": no audio stream in " + path};
        }
        internal::AudioInput input;
        input.path = path;
        input.start_ms = layer.start_ms;
        input.end_ms = layer.end_ms;
        input.in_ms = layer.in_ms;
        input.volume = layer.volume;
        audio_inputs.push_back(std::move(input));
    }

    std::string command =
        "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s " + std::to_string(p.width) + "x" +
        std::to_string(p.height) + " -r " + format_fps(p.fps) + " -i -";

    // One input per audio layer, trimmed to the layer's source range.
    for (const internal::AudioInput& input : audio_inputs) {
        command += " -ss " + format_ms_as_seconds(input.in_ms) + " -t " +
                   format_ms_as_seconds(input.end_ms - input.start_ms) + " -i " +
                   internal::shell_quote(input.path);
    }

    if (audio_inputs.empty()) {
        // Unchanged silent path: no audio stream in the output.
        command +=
            " -an -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2,"
            "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p\""
            " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ";
    } else {
        command += " -filter_complex \"" + internal::build_audio_filter_graph(audio_inputs) +
                   "\" -map 0:v -map \"[aout]\" -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2,"
                   "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p\""
                   " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
                   " -c:a aac -b:a 192k -t " +
                   format_ms_as_seconds(p.duration_ms) + " ";
    }
    command += build_video_codec_args(have_libx264());
    command += internal::shell_quote(out_path);

    ScopedSigpipeIgnore sigpipe_guard;
    std::unique_ptr<std::FILE, PipeCloser> pipe(popen(command.c_str(), "w"));
    if (!pipe) {
        return RenderError{"failed to run ffmpeg"};
    }

    // The cache is reused for every frame so each image is decoded only once.
    for (int64_t i = 0; i < frame_count; ++i) {
        const int64_t t_ms =
            static_cast<int64_t>(std::llround(static_cast<double>(i) * 1000.0 / p.fps));
        const std::variant<Frame, RenderError> rendered = render_frame(p, t_ms, cache);
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
