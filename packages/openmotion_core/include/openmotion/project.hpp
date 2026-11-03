// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_PROJECT_HPP
#define OPENMOTION_PROJECT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace openmotion::project {

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, Hold };

struct Keyframe {
    int64_t time_ms;
    double  value;
    Easing  easing;
};

// A property is either a constant or an animated track.
struct Property {
    double constant = 0.0;
    std::vector<Keyframe> keyframes;  // empty => use `constant`
};

struct Transform {
    Property x, y, scale_x, scale_y, rotation, opacity;
};

enum class LayerType { Solid, Rect, Text, Video, Image, Audio };

struct Layer {
    std::string id;
    LayerType   type;
    std::string name;
    int64_t     start_ms;
    int64_t     end_ms;
    Transform   transform;
    std::string color;   // "#RRGGBB", used by Solid/Rect
    std::string text;    // used by Text
    std::string source;  // file path, used by Video/Image/Audio
    double      width = 0, height = 0;  // used by Rect
    int64_t     in_ms = 0;              // source offset, used by Audio
    double      volume = 1.0;           // 0..10, used by Audio
};

struct Project {
    int         version;  // format version, currently 1
    std::string name;
    int         width, height;
    double      fps;
    int64_t     duration_ms;
    std::vector<Layer> layers;  // bottom to top
};

struct ProjectError {
    std::string message;
};

std::variant<Project, ProjectError> load_from_string(const std::string& json);

std::variant<Project, ProjectError> load_from_file(const std::string& path);

std::string to_json_string(const Project& p);  // pretty-printed, indent 2

std::optional<ProjectError> save_to_file(const Project& p, const std::string& path);

}  // namespace openmotion::project

#endif  // OPENMOTION_PROJECT_HPP
