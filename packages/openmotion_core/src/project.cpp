// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/project.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace openmotion::project {

namespace {

using json = nlohmann::json;

constexpr int kSupportedVersion = 1;

std::optional<Easing> easing_from_string(const std::string& text) {
    if (text == "linear") return Easing::Linear;
    if (text == "ease_in") return Easing::EaseIn;
    if (text == "ease_out") return Easing::EaseOut;
    if (text == "ease_in_out") return Easing::EaseInOut;
    if (text == "hold") return Easing::Hold;
    if (text == "cubic_bezier") return Easing::CubicBezier;
    return std::nullopt;
}

std::string easing_to_string(Easing easing) {
    switch (easing) {
        case Easing::Linear: return "linear";
        case Easing::EaseIn: return "ease_in";
        case Easing::EaseOut: return "ease_out";
        case Easing::EaseInOut: return "ease_in_out";
        case Easing::Hold: return "hold";
        case Easing::CubicBezier: return "cubic_bezier";
    }
    return "linear";
}

std::optional<BlendMode> blend_mode_from_string(const std::string& text) {
    if (text == "normal") return BlendMode::Normal;
    if (text == "multiply") return BlendMode::Multiply;
    if (text == "screen") return BlendMode::Screen;
    if (text == "overlay") return BlendMode::Overlay;
    if (text == "exclusion") return BlendMode::Exclusion;
    if (text == "color_burn") return BlendMode::ColorBurn;
    if (text == "linear_dodge") return BlendMode::LinearDodge;
    return std::nullopt;
}

std::string blend_mode_to_string(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "normal";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen: return "screen";
        case BlendMode::Overlay: return "overlay";
        case BlendMode::Exclusion: return "exclusion";
        case BlendMode::ColorBurn: return "color_burn";
        case BlendMode::LinearDodge: return "linear_dodge";
    }
    return "normal";
}

std::optional<LayerType> layer_type_from_string(const std::string& text) {
    if (text == "solid") return LayerType::Solid;
    if (text == "rect") return LayerType::Rect;
    if (text == "text") return LayerType::Text;
    if (text == "video") return LayerType::Video;
    if (text == "image") return LayerType::Image;
    if (text == "audio") return LayerType::Audio;
    if (text == "circle") return LayerType::Circle;
    if (text == "round_rect") return LayerType::RoundRect;
    return std::nullopt;
}

std::string layer_type_to_string(LayerType type) {
    switch (type) {
        case LayerType::Solid: return "solid";
        case LayerType::Rect: return "rect";
        case LayerType::Text: return "text";
        case LayerType::Video: return "video";
        case LayerType::Image: return "image";
        case LayerType::Audio: return "audio";
        case LayerType::Circle: return "circle";
        case LayerType::RoundRect: return "round_rect";
    }
    return "solid";
}

// Returns the numeric value of `key` when present and numeric.
std::optional<double> number_field(const json& node, const char* key) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_number()) {
        return std::nullopt;
    }
    return node[key].get<double>();
}

// Converting a non-finite or out-of-range double to an integer is undefined
// behaviour (and a UBSan trap), so guard every such conversion.
bool fits_int64(double value) {
    return std::isfinite(value) && value >= -9223372036854775808.0 &&
           value < 9223372036854775808.0;
}

bool fits_int(double value) {
    return std::isfinite(value) && value >= static_cast<double>(std::numeric_limits<int>::min()) &&
           value <= static_cast<double>(std::numeric_limits<int>::max());
}

std::variant<Property, ProjectError> property_from_json(const json& node,
                                                        const std::string& context) {
    Property property;
    if (node.is_number()) {
        property.constant = node.get<double>();
        return property;
    }
    if (!node.is_object() || !node.contains("keyframes") || !node["keyframes"].is_array()) {
        return ProjectError{context + ": property must be a number or a {\"keyframes\": [...]} object"};
    }

    for (const json& entry : node["keyframes"]) {
        if (!entry.is_object() || !entry.contains("t") || !entry.contains("v")) {
            return ProjectError{context + ": each keyframe needs numeric 't' and 'v'"};
        }
        if (!entry["t"].is_number() || !entry["v"].is_number()) {
            return ProjectError{context + ": each keyframe needs numeric 't' and 'v'"};
        }

        Keyframe keyframe;
        if (!fits_int64(entry["t"].get<double>())) {
            return ProjectError{context + ": keyframe 't' is out of range"};
        }
        keyframe.time_ms = static_cast<int64_t>(entry["t"].get<double>());
        keyframe.value = entry["v"].get<double>();
        keyframe.easing = Easing::Linear;

        if (entry.contains("easing")) {
            if (!entry["easing"].is_string()) {
                return ProjectError{context + ": keyframe 'easing' must be a string"};
            }
            const std::optional<Easing> easing = easing_from_string(entry["easing"].get<std::string>());
            if (!easing.has_value()) {
                return ProjectError{context + ": unknown easing '" + entry["easing"].get<std::string>() + "'"};
            }
            keyframe.easing = *easing;
        }

        // "bezier" is only meaningful for cubic_bezier keyframes; on any other
        // easing it is ignored (the default control points stay in place).
        if (keyframe.easing == Easing::CubicBezier) {
            if (!entry.contains("bezier")) {
                return ProjectError{context + ": cubic_bezier easing needs a 'bezier' array"};
            }
            const json& bezier = entry["bezier"];
            if (!bezier.is_array() || bezier.size() != 4) {
                return ProjectError{context + ": 'bezier' must be an array of 4 numbers"};
            }
            for (std::size_t i = 0; i < 4; ++i) {
                if (!bezier[i].is_number()) {
                    return ProjectError{context + ": 'bezier' must be an array of 4 numbers"};
                }
                const double coordinate = bezier[i].get<double>();
                if (!std::isfinite(coordinate)) {
                    return ProjectError{context + ": 'bezier' values must be finite"};
                }
                keyframe.bezier[i] = coordinate;
            }
            if (keyframe.bezier[0] < 0.0 || keyframe.bezier[0] > 1.0 ||
                keyframe.bezier[2] < 0.0 || keyframe.bezier[2] > 1.0) {
                return ProjectError{context + ": 'bezier' x1 and x2 must be within [0, 1]"};
            }
            if (std::abs(keyframe.bezier[1]) > 10.0 || std::abs(keyframe.bezier[3]) > 10.0) {
                return ProjectError{context + ": 'bezier' y1 and y2 must be within [-10, 10]"};
            }
        }

        if (!property.keyframes.empty() && keyframe.time_ms < property.keyframes.back().time_ms) {
            return ProjectError{context + ": keyframes must be sorted by time_ms"};
        }
        property.keyframes.push_back(keyframe);
    }
    return property;
}

// Transform defaults: x=0, y=0, scale_x=1, scale_y=1, rotation=0, opacity=1.
Transform default_transform() {
    Transform transform;
    transform.scale_x.constant = 1.0;
    transform.scale_y.constant = 1.0;
    transform.opacity.constant = 1.0;
    return transform;
}

std::variant<Transform, ProjectError> transform_from_json(const json& node,
                                                          const std::string& context) {
    if (!node.is_object()) {
        return ProjectError{context + ": transform must be an object"};
    }

    Transform transform = default_transform();

    const struct {
        const char* key;
        Property* property;
    } fields[] = {
        {"x", &transform.x},
        {"y", &transform.y},
        {"scale_x", &transform.scale_x},
        {"scale_y", &transform.scale_y},
        {"rotation", &transform.rotation},
        {"opacity", &transform.opacity},
    };

    for (const auto& field : fields) {
        if (!node.contains(field.key)) {
            continue;
        }
        std::variant<Property, ProjectError> result =
            property_from_json(node[field.key], context + "." + field.key);
        if (const auto* error = std::get_if<ProjectError>(&result)) {
            return *error;
        }
        *field.property = std::get<Property>(result);
    }
    return transform;
}

std::variant<Layer, ProjectError> layer_from_json(const json& node, std::size_t index) {
    if (!node.is_object()) {
        return ProjectError{"layer " + std::to_string(index) + ": must be an object"};
    }

    Layer layer;
    layer.transform = default_transform();
    if (!node.contains("id") || !node["id"].is_string() || node["id"].get<std::string>().empty()) {
        return ProjectError{"layer " + std::to_string(index) + ": missing or empty 'id'"};
    }
    layer.id = node["id"].get<std::string>();
    const std::string context = "layer " + std::to_string(index) + " (" + layer.id + ")";

    if (!node.contains("type") || !node["type"].is_string()) {
        return ProjectError{context + ": missing 'type'"};
    }
    const std::optional<LayerType> type = layer_type_from_string(node["type"].get<std::string>());
    if (!type.has_value()) {
        return ProjectError{context + ": unknown layer type '" + node["type"].get<std::string>() + "'"};
    }
    layer.type = *type;

    layer.name = (node.contains("name") && node["name"].is_string()) ? node["name"].get<std::string>() : "";
    const double start_ms = number_field(node, "start_ms").value_or(0.0);
    const double end_ms = number_field(node, "end_ms").value_or(0.0);
    if (!fits_int64(start_ms) || !fits_int64(end_ms)) {
        return ProjectError{context + ": start_ms/end_ms is out of range"};
    }
    layer.start_ms = static_cast<int64_t>(start_ms);
    layer.end_ms = static_cast<int64_t>(end_ms);
    layer.color = (node.contains("color") && node["color"].is_string()) ? node["color"].get<std::string>() : "";
    layer.text = (node.contains("text") && node["text"].is_string()) ? node["text"].get<std::string>() : "";
    layer.source = (node.contains("source") && node["source"].is_string()) ? node["source"].get<std::string>() : "";
    layer.width = number_field(node, "width").value_or(0.0);
    layer.height = number_field(node, "height").value_or(0.0);
    layer.radius = number_field(node, "radius").value_or(0.0);
    // 1e400 parses as infinity, so the finiteness check is what rejects it.
    if (!std::isfinite(layer.radius) || layer.radius < 0.0) {
        return ProjectError{context + ": radius must be finite and not negative"};
    }
    const double in_ms = number_field(node, "in_ms").value_or(0.0);
    if (!fits_int64(in_ms)) {
        return ProjectError{context + ": in_ms is out of range"};
    }
    layer.in_ms = static_cast<int64_t>(in_ms);
    layer.volume = number_field(node, "volume").value_or(1.0);

    if (layer.in_ms < 0) {
        return ProjectError{context + ": in_ms must not be negative"};
    }
    if (!std::isfinite(layer.volume) || layer.volume < 0.0 || layer.volume > 10.0) {
        return ProjectError{context + ": volume must be between 0 and 10"};
    }

    if (layer.end_ms < layer.start_ms) {
        return ProjectError{context + ": end_ms must not be before start_ms"};
    }

    if (node.contains("blend")) {
        if (!node["blend"].is_string()) {
            return ProjectError{context + ": 'blend' must be a string"};
        }
        const std::optional<BlendMode> blend =
            blend_mode_from_string(node["blend"].get<std::string>());
        if (!blend.has_value()) {
            return ProjectError{context + ": unknown blend mode '" +
                                node["blend"].get<std::string>() + "'"};
        }
        layer.blend = *blend;
    }

    if (node.contains("transform")) {
        std::variant<Transform, ProjectError> transform = transform_from_json(node["transform"], context + ".transform");
        if (const auto* error = std::get_if<ProjectError>(&transform)) {
            return *error;
        }
        layer.transform = std::get<Transform>(transform);
    }
    return layer;
}

std::variant<Project, ProjectError> parse_project(const json& root) {
    if (!root.is_object()) {
        return ProjectError{"top-level JSON value must be an object"};
    }

    if (!root.contains("version") || !root["version"].is_number_integer()) {
        return ProjectError{"missing or invalid 'version'"};
    }
    const int version = root["version"].get<int>();
    if (version != kSupportedVersion) {
        return ProjectError{"unsupported version " + std::to_string(version) + " (only " +
                            std::to_string(kSupportedVersion) + " is supported)"};
    }

    Project project;
    project.version = version;
    project.name = (root.contains("name") && root["name"].is_string()) ? root["name"].get<std::string>() : "";

    const std::optional<double> width = number_field(root, "width");
    if (!width.has_value() || !fits_int(*width)) {
        return ProjectError{"missing or invalid 'width'"};
    }
    project.width = static_cast<int>(*width);
    if (project.width <= 0) {
        return ProjectError{"'width' must be positive"};
    }

    const std::optional<double> height = number_field(root, "height");
    if (!height.has_value() || !fits_int(*height)) {
        return ProjectError{"missing or invalid 'height'"};
    }
    project.height = static_cast<int>(*height);
    if (project.height <= 0) {
        return ProjectError{"'height' must be positive"};
    }

    const std::optional<double> fps = number_field(root, "fps");
    if (!fps.has_value()) {
        return ProjectError{"missing or invalid 'fps'"};
    }
    project.fps = *fps;
    if (project.fps <= 0.0) {
        return ProjectError{"'fps' must be positive"};
    }

    const std::optional<double> duration = number_field(root, "duration_ms");
    if (!duration.has_value() || !fits_int64(*duration)) {
        return ProjectError{"missing or invalid 'duration_ms'"};
    }
    project.duration_ms = static_cast<int64_t>(*duration);
    if (project.duration_ms < 0) {
        return ProjectError{"'duration_ms' must not be negative"};
    }

    if (root.contains("layers")) {
        if (!root["layers"].is_array()) {
            return ProjectError{"'layers' must be an array"};
        }
        std::set<std::string> ids;
        for (std::size_t index = 0; index < root["layers"].size(); ++index) {
            std::variant<Layer, ProjectError> layer = layer_from_json(root["layers"][index], index);
            if (const auto* error = std::get_if<ProjectError>(&layer)) {
                return *error;
            }
            if (!ids.insert(std::get<Layer>(layer).id).second) {
                return ProjectError{"duplicate layer id '" + std::get<Layer>(layer).id + "'"};
            }
            project.layers.push_back(std::get<Layer>(layer));
        }
    }
    return project;
}

json property_to_json(const Property& property) {
    if (property.keyframes.empty()) {
        return property.constant;
    }
    json keyframes = json::array();
    for (const Keyframe& keyframe : property.keyframes) {
        json entry{
            {"t", keyframe.time_ms},
            {"v", keyframe.value},
            {"easing", easing_to_string(keyframe.easing)},
        };
        // Only cubic_bezier keyframes carry control points, so every other
        // easing keeps serializing byte-identically to before.
        if (keyframe.easing == Easing::CubicBezier) {
            entry["bezier"] = keyframe.bezier;
        }
        keyframes.push_back(std::move(entry));
    }
    return json{{"keyframes", keyframes}};
}

json transform_to_json(const Transform& transform) {
    return json{
        {"x", property_to_json(transform.x)},
        {"y", property_to_json(transform.y)},
        {"scale_x", property_to_json(transform.scale_x)},
        {"scale_y", property_to_json(transform.scale_y)},
        {"rotation", property_to_json(transform.rotation)},
        {"opacity", property_to_json(transform.opacity)},
    };
}

json layer_to_json(const Layer& layer) {
    json node{
        {"id", layer.id},
        {"type", layer_type_to_string(layer.type)},
        {"name", layer.name},
        {"start_ms", layer.start_ms},
        {"end_ms", layer.end_ms},
        {"transform", transform_to_json(layer.transform)},
        {"color", layer.color},
        {"text", layer.text},
        {"source", layer.source},
        {"width", layer.width},
        {"height", layer.height},
    };
    // in_ms/volume only apply to audio layers, so they are written for those and
    // omitted for every other type to keep the other layers' JSON unchanged.
    if (layer.type == LayerType::Audio) {
        node["in_ms"] = layer.in_ms;
        node["volume"] = layer.volume;
    }
    // "radius" only applies to round_rect layers; every other type omits it so
    // its JSON stays byte-identical.
    if (layer.type == LayerType::RoundRect) {
        node["radius"] = layer.radius;
    }
    // "blend" is written only when it is not the default, so a layer without a
    // blend mode serializes exactly as it did before the field existed.
    if (layer.blend != BlendMode::Normal) {
        node["blend"] = blend_mode_to_string(layer.blend);
    }
    return node;
}

json project_to_json(const Project& project) {
    json layers = json::array();
    for (const Layer& layer : project.layers) {
        layers.push_back(layer_to_json(layer));
    }
    return json{
        {"version", project.version},
        {"name", project.name},
        {"width", project.width},
        {"height", project.height},
        {"fps", project.fps},
        {"duration_ms", project.duration_ms},
        {"layers", layers},
    };
}

}  // namespace

std::variant<Project, ProjectError> load_from_string(const std::string& text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& error) {
        return ProjectError{std::string{"malformed JSON: "} + error.what()};
    }

    try {
        return parse_project(root);
    } catch (const json::exception& error) {
        return ProjectError{std::string{"invalid project JSON: "} + error.what()};
    } catch (const std::exception& error) {
        return ProjectError{std::string{"unexpected error: "} + error.what()};
    }
}

std::variant<Project, ProjectError> load_from_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return ProjectError{"cannot open file: " + path};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return ProjectError{"failed to read file: " + path};
    }
    return load_from_string(buffer.str());
}

std::string to_json_string(const Project& project) {
    return project_to_json(project).dump(2);
}

std::optional<ProjectError> save_to_file(const Project& project, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return ProjectError{"cannot open file for writing: " + path};
    }
    output << to_json_string(project) << '\n';
    if (!output) {
        return ProjectError{"failed to write file: " + path};
    }
    output.close();
    if (!output) {
        return ProjectError{"failed to close file: " + path};
    }
    return std::nullopt;
}

}  // namespace openmotion::project
