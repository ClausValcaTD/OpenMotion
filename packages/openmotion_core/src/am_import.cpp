// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/am_import.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

namespace openmotion::am_import {

namespace {

using json = nlohmann::json;

constexpr double kMaxSceneDimension = 16384.0;
constexpr double kSafeInt64 = 9.0e18;

// ---- string and number helpers ---------------------------------------------

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// Parses a finite decimal number, rejecting trailing garbage. Locale-independent.
bool parse_number(const std::string& text, double& out) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    double value = 0.0;
    const char* first = trimmed.data();
    const char* last = trimmed.data() + trimmed.size();
    const std::from_chars_result result =
        std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != last || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool to_int64(double value, int64_t& out) {
    if (!std::isfinite(value) || value < -kSafeInt64 || value > kSafeInt64) {
        return false;
    }
    out = static_cast<int64_t>(std::llround(value));
    return true;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::vector<std::string> split_ws(const std::string& text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(text.substr(start, i - start));
        }
    }
    return tokens;
}

// Reads component `axis` of a comma-separated vector, or the whole value when
// `axis` is negative.
bool parse_component(const std::string& text, int axis, double& out) {
    if (axis < 0) {
        return parse_number(text, out);
    }
    const std::vector<std::string> parts = split(text, ',');
    if (static_cast<std::size_t>(axis) >= parts.size()) {
        return false;
    }
    return parse_number(parts[static_cast<std::size_t>(axis)], out);
}

std::string attribute(const pugi::xml_node& node, const char* name) {
    const pugi::xml_attribute attr = node.attribute(name);
    if (attr.empty()) {
        return {};
    }
    return std::string(attr.value());
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#AARRGGBB" -> "#RRGGBB" plus alpha in [0,1].
bool parse_argb(const std::string& text, std::string& rgb, double& alpha) {
    if (text.size() != 9 || text[0] != '#') {
        return false;
    }
    int digits[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 1; i < text.size(); ++i) {
        const int digit = hex_digit(text[i]);
        if (digit < 0) {
            return false;
        }
        digits[i - 1] = digit;
    }
    rgb = "#" + text.substr(3, 6);
    alpha = static_cast<double>(digits[0] * 16 + digits[1]) / 255.0;
    return true;
}

std::string severity_name(Issue::Severity severity) {
    switch (severity) {
        case Issue::Severity::Info: return "info";
        case Issue::Severity::Warning: return "warning";
        case Issue::Severity::Skipped: return "skipped";
    }
    return "info";
}

std::string label_of(const pugi::xml_node& node) {
    std::string label = attribute(node, "label");
    if (label.empty()) {
        label = attribute(node, "id");
    }
    return label;
}

pugi::xml_node find_property(const pugi::xml_node& container, const char* name) {
    for (pugi::xml_node child : container.children("property")) {
        if (std::strcmp(child.attribute("name").value(), name) == 0) {
            return child;
        }
    }
    return {};
}

// AM keyframe `t` (a fraction of the layer duration) -> frame index.
bool frame_from_t(double t, double duration_ms, double fps, int64_t& frame_out) {
    const double local_ms = t * duration_ms;
    const double frames = local_ms * fps / 1000.0 - 0.5;
    double rounded = std::round(frames);
    if (!std::isfinite(rounded) || rounded > kSafeInt64) {
        return false;
    }
    if (rounded < 0.0) {
        rounded = 0.0;
    }
    frame_out = static_cast<int64_t>(rounded);
    return true;
}

bool time_from_frame(int64_t frame, double fps, int64_t start_ms, int64_t& time_out) {
    const double ms = std::round(static_cast<double>(frame) * 1000.0 / fps);
    if (!std::isfinite(ms) || ms < -kSafeInt64 || ms > kSafeInt64) {
        return false;
    }
    const int64_t add = static_cast<int64_t>(ms);
    if (add > 0 && start_ms > std::numeric_limits<int64_t>::max() - add) {
        return false;
    }
    if (add < 0 && start_ms < std::numeric_limits<int64_t>::min() - add) {
        return false;
    }
    time_out = start_ms + add;
    return true;
}

// ---- converter -------------------------------------------------------------

struct Converter {
    project::Project project;
    ImportReport report;
    std::string error;
    std::vector<std::string> used_ids;
    double fps = 30.0;
    int64_t scene_duration = 0;

    bool fail(std::string message) {
        error = std::move(message);
        return false;
    }

    void add_issue(Issue::Severity severity, const std::string& layer, std::string message) {
        report.issues.push_back(Issue{severity, layer, std::move(message)});
    }

    void skip(const std::string& layer, std::string message) {
        add_issue(Issue::Severity::Skipped, layer, std::move(message));
    }

    std::string unique_id(const std::string& raw) {
        std::string base = "am_" + raw;
        if (base == "am_") {
            base = "am_layer";
        }
        std::string candidate = base;
        int suffix = 2;
        while (std::find(used_ids.begin(), used_ids.end(), candidate) != used_ids.end()) {
            candidate = base + "_" + std::to_string(suffix);
            ++suffix;
        }
        used_ids.push_back(candidate);
        return candidate;
    }

    // Parses one axis of a `<property>` (constant or keyframe track). `axis` is
    // the component index within a vector value, or -1 for a scalar.
    bool read_property(const pugi::xml_node& container, const char* name, int axis,
                       const std::string& label, int64_t start_ms, double duration_ms,
                       project::Property& out);

    project::Easing parse_easing(const std::string& text, std::array<double, 4>& bezier,
                                 const std::string& label);

    bool convert_shape(const pugi::xml_node& node);
    bool convert_layer(const pugi::xml_node& node);

    std::variant<ImportResult, ImportError> run(const pugi::xml_document& doc);
};

project::Easing Converter::parse_easing(const std::string& text, std::array<double, 4>& bezier,
                                        const std::string& label) {
    if (text.empty()) {
        return project::Easing::Linear;  // [assumed] a missing `e` means linear
    }
    const std::vector<std::string> tokens = split_ws(text);
    if (!tokens.empty() && tokens[0] == "cubicBezier") {
        double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
        if (tokens.size() == 5 && parse_number(tokens[1], a) && parse_number(tokens[2], b) &&
            parse_number(tokens[3], c) && parse_number(tokens[4], d) && a >= 0.0 && a <= 1.0 &&
            c >= 0.0 && c <= 1.0 && std::abs(b) <= 10.0 && std::abs(d) <= 10.0) {
            bezier = {a, b, c, d};
            return project::Easing::CubicBezier;
        }
        add_issue(Issue::Severity::Warning, label,
                  "cubicBezier easing out of range; using linear");
        return project::Easing::Linear;
    }
    if (!tokens.empty() && tokens[0] == "elastic") {
        add_issue(Issue::Severity::Warning, label,
                  "elastic easing semantics unknown; using linear");
        return project::Easing::Linear;
    }
    add_issue(Issue::Severity::Warning, label,
              "unknown easing '" + text + "'; using linear");
    return project::Easing::Linear;
}

bool Converter::read_property(const pugi::xml_node& container, const char* name, int axis,
                              const std::string& label, int64_t start_ms, double duration_ms,
                              project::Property& out) {
    const pugi::xml_node prop = find_property(container, name);
    if (prop.empty()) {
        return true;  // absent: the caller keeps the field's default
    }

    std::vector<pugi::xml_node> kfs;
    for (pugi::xml_node kf : prop.children("kf")) {
        kfs.push_back(kf);
    }
    if (kfs.size() > kMaxKeyframesPerProperty) {
        return fail(label + ": property '" + name + "' has more than " +
                    std::to_string(kMaxKeyframesPerProperty) + " keyframes");
    }

    if (kfs.empty()) {
        double value = 0.0;
        if (!parse_component(attribute(prop, "value"), axis, value)) {
            return fail(label + ": property '" + name + "' has a missing or non-finite value");
        }
        out = project::Property{};
        out.constant = value;
        return true;
    }
    if (kfs.size() == 1) {
        double value = 0.0;
        if (!parse_component(attribute(kfs[0], "v"), axis, value)) {
            return fail(label + ": property '" + name + "' has a non-finite keyframe value");
        }
        out = project::Property{};
        out.constant = value;
        return true;
    }

    struct Raw {
        double t;
        double v;
        std::string e;
    };
    std::vector<Raw> raw;
    raw.reserve(kfs.size());
    for (const pugi::xml_node& kf : kfs) {
        Raw entry{0.0, 0.0, {}};
        if (!parse_number(attribute(kf, "t"), entry.t) ||
            !parse_component(attribute(kf, "v"), axis, entry.v)) {
            return fail(label + ": property '" + name + "' has a non-finite keyframe");
        }
        entry.e = attribute(kf, "e");
        raw.push_back(std::move(entry));
    }
    // Keyframes are NOT stored in time order; sort by t, keeping file order for
    // equal t so the "keep the last" rule below is deterministic.
    std::stable_sort(raw.begin(), raw.end(),
                     [](const Raw& lhs, const Raw& rhs) { return lhs.t < rhs.t; });

    struct Mapped {
        int64_t frame;
        int64_t time_ms;
        double v;
        std::string e;
    };
    std::vector<Mapped> mapped;
    mapped.reserve(raw.size());
    bool had_duplicate = false;
    for (const Raw& entry : raw) {
        int64_t frame = 0;
        int64_t time_ms = 0;
        if (!frame_from_t(entry.t, duration_ms, fps, frame) ||
            !time_from_frame(frame, fps, start_ms, time_ms)) {
            return fail(label + ": property '" + name + "' keyframe time is out of range");
        }
        if (!mapped.empty() && mapped.back().frame == frame) {
            mapped.back() = Mapped{frame, time_ms, entry.v, entry.e};
            had_duplicate = true;
        } else {
            mapped.push_back(Mapped{frame, time_ms, entry.v, entry.e});
        }
    }
    if (had_duplicate) {
        add_issue(Issue::Severity::Warning, label,
                  "two keyframes landed on the same frame; kept the last");
    }

    project::Property result;
    result.keyframes.reserve(mapped.size());
    for (std::size_t i = 0; i < mapped.size(); ++i) {
        project::Keyframe kf;
        kf.time_ms = mapped[i].time_ms;
        kf.value = mapped[i].v;
        kf.easing = project::Easing::Linear;
        // AM stores `e` on the keyframe the motion ARRIVES at, so our easing for
        // keyframe i is the easing of the next AM keyframe. The last is linear.
        if (i + 1 < mapped.size()) {
            std::array<double, 4> bezier = {0.0, 0.0, 1.0, 1.0};
            kf.easing = parse_easing(mapped[i + 1].e, bezier, label);
            kf.bezier = bezier;
        }
        result.keyframes.push_back(kf);
    }
    out = std::move(result);
    return true;
}

bool Converter::convert_shape(const pugi::xml_node& node) {
    const std::string label = label_of(node);

    double start_value = 0.0;
    double end_value = static_cast<double>(scene_duration);
    const std::string start_attr = attribute(node, "startTime");
    const std::string end_attr = attribute(node, "endTime");
    if ((!start_attr.empty() && !parse_number(start_attr, start_value)) ||
        (!end_attr.empty() && !parse_number(end_attr, end_value))) {
        return fail(label + ": layer startTime/endTime is not a finite number");
    }
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    if (!to_int64(start_value, start_ms) || !to_int64(end_value, end_ms)) {
        return fail(label + ": layer startTime/endTime is out of range");
    }
    if (end_ms < start_ms) {
        skip(label, "end time is before start time");
        return true;
    }
    if (attribute(node, "hidden") == "true") {
        skip(label, "layer is hidden");
        return true;
    }

    const std::string fill_type = attribute(node, "fillType");
    const std::string shape = attribute(node, "s");
    if (fill_type == "none") {
        skip(label, "fillType 'none' has no fill to import");
        return true;
    }
    if (shape == ".line" || !node.child("path").empty()) {
        skip(label, "shapes of this kind are not supported yet");
        return true;
    }

    project::Layer layer;
    layer.id = unique_id(attribute(node, "id"));
    layer.name = label;
    layer.start_ms = start_ms;
    layer.end_ms = end_ms;

    if (fill_type == "media") {
        layer.type = project::LayerType::Image;
        layer.source = label;  // the only portable identifier is the file name
        add_issue(Issue::Severity::Warning, label,
                  "mediaFillMode is ignored; the image must sit next to the .omproj");
    } else if (shape == ".rect") {
        layer.type = project::LayerType::Rect;
    } else if (shape == ".circle") {
        layer.type = project::LayerType::Circle;
    } else if (shape == ".roundrect") {
        layer.type = project::LayerType::RoundRect;
        layer.radius = 0.0;
        add_issue(Issue::Severity::Warning, label, "corner radius semantics unknown");
    } else {
        skip(label, "unsupported shape kind '" + shape + "'");
        return true;
    }

    // Size holds HALF-extents, so the full size is twice the stored value.
    double size_x = 0.0;
    double size_y = 0.0;
    bool has_size = false;
    const pugi::xml_node size_prop = find_property(node, "size");
    if (!size_prop.empty()) {
        std::string value = attribute(size_prop, "value");
        if (value.empty()) {
            const pugi::xml_node kf = size_prop.child("kf");
            if (!kf.empty()) {
                value = attribute(kf, "v");
            }
        }
        if (!value.empty()) {
            if (!parse_component(value, 0, size_x) || !parse_component(value, 1, size_y)) {
                return fail(label + ": size is not a pair of finite numbers");
            }
            has_size = true;
        }
    }
    const bool is_shape = layer.type == project::LayerType::Rect ||
                          layer.type == project::LayerType::Circle ||
                          layer.type == project::LayerType::RoundRect;
    if (is_shape && !has_size) {
        skip(label, "size property is missing");
        return true;
    }
    if (has_size) {
        layer.width = 2.0 * size_x;
        layer.height = 2.0 * size_y;
    }

    // Fill colour.
    double alpha = 1.0;
    if (fill_type == "gradient") {
        std::string rgb;
        double gradient_alpha = 1.0;
        if (parse_argb(attribute(node.child("gradient"), "startColor"), rgb, gradient_alpha)) {
            layer.color = rgb;
        } else {
            layer.color = "#FFFFFF";
        }
        add_issue(Issue::Severity::Warning, label, "gradient geometry unverified");
    } else if (layer.type != project::LayerType::Image) {
        std::string rgb;
        double fill_alpha = 1.0;
        if (parse_argb(attribute(node.child("fillColor"), "value"), rgb, fill_alpha)) {
            layer.color = rgb;
            alpha = fill_alpha;
        } else {
            layer.color = "#FFFFFF";
        }
    }

    // Transform.
    project::Transform transform;
    transform.x.constant = 0.0;
    transform.y.constant = 0.0;
    transform.scale_x.constant = 1.0;
    transform.scale_y.constant = 1.0;
    transform.rotation.constant = 0.0;
    transform.opacity.constant = 1.0;

    const pugi::xml_node transform_node = node.child("transform");
    const double duration_ms = static_cast<double>(end_ms - start_ms);
    if (!read_property(transform_node, "location", 0, label, start_ms, duration_ms, transform.x) ||
        !read_property(transform_node, "location", 1, label, start_ms, duration_ms, transform.y) ||
        !read_property(transform_node, "scale", 0, label, start_ms, duration_ms,
                       transform.scale_x) ||
        !read_property(transform_node, "scale", 1, label, start_ms, duration_ms,
                       transform.scale_y) ||
        !read_property(transform_node, "rotation", -1, label, start_ms, duration_ms,
                       transform.rotation) ||
        !read_property(transform_node, "opacity", -1, label, start_ms, duration_ms,
                       transform.opacity)) {
        return false;
    }

    // A non-ff alpha multiplies into opacity, for constants and keyframes alike.
    if (alpha != 1.0) {
        if (transform.opacity.keyframes.empty()) {
            transform.opacity.constant *= alpha;
        } else {
            for (project::Keyframe& kf : transform.opacity.keyframes) {
                kf.value *= alpha;
            }
        }
    }

    const pugi::xml_node pivot = find_property(transform_node, "pivot");
    if (!pivot.empty()) {
        double pivot_x = 0.0;
        double pivot_y = 0.0;
        if (parse_component(attribute(pivot, "value"), 0, pivot_x) &&
            parse_component(attribute(pivot, "value"), 1, pivot_y) &&
            (pivot_x != 0.0 || pivot_y != 0.0)) {
            add_issue(Issue::Severity::Warning, label, "pivot ignored");
        }
    }
    layer.transform = std::move(transform);

    // Blend mode.
    const std::string blending = attribute(node, "blending");
    if (blending == "mask") {
        add_issue(Issue::Severity::Warning, label, "masks unsupported");
    } else if (blending == "screen") {
        layer.blend = project::BlendMode::Screen;
    } else if (blending == "multiply") {
        layer.blend = project::BlendMode::Multiply;
    } else if (blending == "exclude") {
        layer.blend = project::BlendMode::Exclusion;
    } else if (blending == "color-burn") {
        layer.blend = project::BlendMode::ColorBurn;
    } else if (blending == "linear-dodge") {
        layer.blend = project::BlendMode::LinearDodge;
    } else if (!blending.empty() && blending != "none") {
        add_issue(Issue::Severity::Warning, label,
                  "unknown blend mode '" + blending + "'; using normal");
    }

    if (!attribute(node, "parent").empty()) {
        add_issue(Issue::Severity::Warning, label,
                  "parent transforms not applied; position may be wrong");
    }
    if (attribute(node, "clippingMask") == "true") {
        add_issue(Issue::Severity::Warning, label, "clipping masks unsupported");
    }

    // Effects are never converted.
    for (pugi::xml_node effect : node.children("effect")) {
        const std::string effect_id = attribute(effect, "id");
        add_issue(Issue::Severity::Info, label, "effect '" + effect_id + "' is not converted");
    }

    project.layers.push_back(std::move(layer));
    return true;
}

bool Converter::convert_layer(const pugi::xml_node& node) {
    const std::string element = node.name();
    const std::string label = label_of(node);
    if (element == "shape") {
        return convert_shape(node);
    }
    if (element == "text") {
        skip(label, "text layers are not supported yet");
        return true;
    }
    if (element == "nullobj") {
        skip(label, "null object layers have no geometry to import");
        return true;
    }
    if (element == "embedScene") {
        std::size_t nested = 0;
        const pugi::xml_node nested_scene = node.child("scene");
        if (!nested_scene.empty()) {
            for (pugi::xml_node child : nested_scene.children()) {
                if (child.type() == pugi::node_element && std::strcmp(child.name(), "bookmark") != 0) {
                    ++nested;
                }
            }
        }
        skip(label, "nested composition (embedScene) with " + std::to_string(nested) +
                        " layer(s) is not supported yet");
        return true;
    }
    skip(label, "unsupported element <" + element + ">");
    return true;
}

std::variant<ImportResult, ImportError> Converter::run(const pugi::xml_document& doc) {
    const pugi::xml_node scene = doc.document_element();
    if (scene.empty() || std::strcmp(scene.name(), "scene") != 0) {
        return ImportError{"root element must be <scene>"};
    }

    double width = 0.0;
    double height = 0.0;
    double fps_value = 0.0;
    double total_time = 0.0;
    if (!parse_number(attribute(scene, "width"), width) || width <= 0.0 ||
        width > kMaxSceneDimension) {
        return ImportError{"scene width must be a number in (0, 16384]"};
    }
    if (!parse_number(attribute(scene, "height"), height) || height <= 0.0 ||
        height > kMaxSceneDimension) {
        return ImportError{"scene height must be a number in (0, 16384]"};
    }
    if (!parse_number(attribute(scene, "fps"), fps_value) || fps_value <= 0.0) {
        return ImportError{"scene fps must be greater than 0"};
    }
    if (!parse_number(attribute(scene, "totalTime"), total_time) || total_time < 0.0) {
        return ImportError{"scene totalTime must be a number >= 0"};
    }
    int64_t duration = 0;
    if (!to_int64(total_time, duration)) {
        return ImportError{"scene totalTime is out of range"};
    }

    project.version = 1;
    project.name = attribute(scene, "title");
    project.width = static_cast<int>(std::llround(width));
    project.height = static_cast<int>(std::llround(height));
    project.fps = fps_value;
    project.duration_ms = duration;
    fps = fps_value;
    scene_duration = duration;

    // Layers appear in file order. AM order is [assumed] bottom to top, which
    // matches our layer vector, so no reordering is done.
    std::size_t layer_count = 0;
    std::size_t bookmark_count = 0;
    for (pugi::xml_node child : scene.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (std::strcmp(child.name(), "bookmark") == 0) {
            ++bookmark_count;
            continue;
        }
        ++layer_count;
        if (layer_count > kMaxLayers) {
            return ImportError{"scene has more than " + std::to_string(kMaxLayers) + " layers"};
        }
        if (!convert_layer(child)) {
            return ImportError{error};
        }
    }
    if (bookmark_count > 0) {
        add_issue(Issue::Severity::Info, "",
                  std::to_string(bookmark_count) + " bookmark marker(s) are not supported yet");
    }

    // The project must always be loadable; validate by a serialize/reload round
    // trip so a converter bug surfaces as an ImportError instead of a bad file.
    const std::string encoded = project::to_json_string(project);
    const std::variant<project::Project, project::ProjectError> reloaded =
        project::load_from_string(encoded);
    if (const auto* reload_error = std::get_if<project::ProjectError>(&reloaded)) {
        return ImportError{"produced project failed validation: " + reload_error->message};
    }

    return ImportResult{std::move(project), std::move(report)};
}

}  // namespace

bool input_size_ok(std::size_t size, std::size_t limit) {
    return size <= limit;
}

std::variant<ImportResult, ImportError> import_am_xml(const std::string& xml_text) {
    if (!input_size_ok(xml_text.size())) {
        return ImportError{"input exceeds the " + std::to_string(kMaxInputBytes) +
                           " byte limit"};
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml_text.data(), xml_text.size());
    if (result.status != pugi::status_ok) {
        return ImportError{std::string{"XML parse error: "} + result.description()};
    }
    // The public API never throws: an allocation failure or any other standard
    // exception is reported like a parse error.
    try {
        Converter converter;
        return converter.run(doc);
    } catch (const std::exception& error) {
        return ImportError{std::string{"unexpected error: "} + error.what()};
    }
}

std::variant<ImportResult, ImportError> import_am_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ImportError{"cannot open file: " + path};
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (input.bad()) {
        return ImportError{"failed to read file: " + path};
    }
    return import_am_xml(contents);
}

std::string report_to_json(const ImportReport& report) {
    json issues = json::array();
    for (const Issue& issue : report.issues) {
        issues.push_back(json{{"severity", severity_name(issue.severity)},
                              {"layer", issue.layer},
                              {"message", issue.message}});
    }
    json out;
    out["issues"] = issues;
    return out.dump(2);
}

}  // namespace openmotion::am_import
