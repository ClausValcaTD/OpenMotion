// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

namespace project = openmotion::project;

using project::Easing;
using project::Keyframe;
using project::Layer;
using project::LayerType;
using project::Project;
using project::ProjectError;
using project::Property;
using project::Transform;

bool properties_equal(const Property& lhs, const Property& rhs) {
    if (lhs.constant != rhs.constant) return false;
    if (lhs.keyframes.size() != rhs.keyframes.size()) return false;
    for (std::size_t i = 0; i < lhs.keyframes.size(); ++i) {
        const Keyframe& a = lhs.keyframes[i];
        const Keyframe& b = rhs.keyframes[i];
        if (a.time_ms != b.time_ms || a.value != b.value || a.easing != b.easing) return false;
        if (a.easing == Easing::CubicBezier && a.bezier != b.bezier) return false;
    }
    return true;
}

bool transforms_equal(const Transform& lhs, const Transform& rhs) {
    return properties_equal(lhs.x, rhs.x) && properties_equal(lhs.y, rhs.y) &&
           properties_equal(lhs.scale_x, rhs.scale_x) && properties_equal(lhs.scale_y, rhs.scale_y) &&
           properties_equal(lhs.rotation, rhs.rotation) && properties_equal(lhs.opacity, rhs.opacity);
}

bool layers_equal(const Layer& lhs, const Layer& rhs) {
    return lhs.id == rhs.id && lhs.type == rhs.type && lhs.name == rhs.name &&
           lhs.start_ms == rhs.start_ms && lhs.end_ms == rhs.end_ms &&
           transforms_equal(lhs.transform, rhs.transform) && lhs.color == rhs.color &&
           lhs.text == rhs.text && lhs.source == rhs.source && lhs.width == rhs.width &&
           lhs.height == rhs.height && lhs.in_ms == rhs.in_ms && lhs.volume == rhs.volume;
}

bool projects_equal(const Project& lhs, const Project& rhs) {
    if (lhs.version != rhs.version || lhs.name != rhs.name || lhs.width != rhs.width ||
        lhs.height != rhs.height || lhs.fps != rhs.fps || lhs.duration_ms != rhs.duration_ms) {
        return false;
    }
    if (lhs.layers.size() != rhs.layers.size()) return false;
    for (std::size_t i = 0; i < lhs.layers.size(); ++i) {
        if (!layers_equal(lhs.layers[i], rhs.layers[i])) return false;
    }
    return true;
}

const char* kValid = R"({
  "version": 1,
  "name": "Valid",
  "width": 640,
  "height": 480,
  "fps": 30.0,
  "duration_ms": 1000,
  "layers": [
    { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 1000, "color": "#000000" },
    { "id": "b", "type": "rect", "start_ms": 0, "end_ms": 500, "width": 10.0, "height": 10.0 }
  ]
})";

std::string with_replaced(std::string text, const std::string& from, const std::string& to) {
    const std::size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

bool is_error(const std::variant<Project, ProjectError>& result) {
    return std::holds_alternative<ProjectError>(result);
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t position = haystack.find(needle); position != std::string::npos;
         position = haystack.find(needle, position + needle.size())) {
        ++count;
    }
    return count;
}

Keyframe make_bezier_keyframe(int64_t time_ms, double value, double x1, double y1, double x2, double y2) {
    Keyframe keyframe{time_ms, value, Easing::CubicBezier};
    keyframe.bezier = {x1, y1, x2, y2};
    return keyframe;
}

Keyframe make_keyframe(int64_t time_ms, double value, Easing easing) {
    return Keyframe{time_ms, value, easing};
}

}  // namespace

TEST_CASE("hello.omproj loads with the expected fields", "[project]") {
    const std::string path = std::string(OPENMOTION_EXAMPLE_DIR) + "/hello.omproj";
    const std::variant<Project, ProjectError> result = project::load_from_file(path);
    REQUIRE(std::holds_alternative<Project>(result));

    const Project& loaded = std::get<Project>(result);
    CHECK(loaded.version == 1);
    CHECK(loaded.name == "Hello OpenMotion");
    CHECK(loaded.width == 1280);
    CHECK(loaded.height == 720);
    CHECK(loaded.fps == 30.0);
    CHECK(loaded.duration_ms == 3000);
    REQUIRE(loaded.layers.size() == 2);

    CHECK(loaded.layers[0].id == "bg");
    CHECK(loaded.layers[0].type == LayerType::Solid);
    CHECK(loaded.layers[0].color == "#101018");

    const Layer& box = loaded.layers[1];
    CHECK(box.id == "box");
    CHECK(box.type == LayerType::Rect);
    REQUIRE(box.transform.x.keyframes.size() == 3);
    CHECK(box.transform.x.keyframes[0].time_ms == 0);
    CHECK(box.transform.x.keyframes[0].easing == Easing::Linear);
    CHECK(box.transform.x.keyframes[1].value == 400.0);
    CHECK(box.transform.x.keyframes[1].easing == Easing::EaseInOut);
    CHECK(box.transform.opacity.keyframes.empty());
    CHECK(box.transform.opacity.constant == 0.75);
}

TEST_CASE("project JSON round trips through to_json_string", "[project]") {
    const std::string path = std::string(OPENMOTION_EXAMPLE_DIR) + "/hello.omproj";
    const std::variant<Project, ProjectError> first = project::load_from_file(path);
    REQUIRE(std::holds_alternative<Project>(first));

    const std::string encoded = project::to_json_string(std::get<Project>(first));
    const std::variant<Project, ProjectError> second = project::load_from_string(encoded);
    REQUIRE(std::holds_alternative<Project>(second));

    CHECK(projects_equal(std::get<Project>(first), std::get<Project>(second)));
}

TEST_CASE("validation rejects invalid projects", "[project]") {
    SECTION("missing version") {
        const std::string text =
            R"({ "name": "x", "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0 })";
        CHECK(is_error(project::load_from_string(text)));
    }

    SECTION("unsupported version") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"version\": 1", "\"version\": 2"))));
    }

    SECTION("non-positive width") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"width\": 640", "\"width\": 0"))));
    }

    SECTION("non-positive height") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"height\": 480", "\"height\": -3"))));
    }

    SECTION("non-positive fps") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"fps\": 30.0", "\"fps\": 0"))));
    }

    SECTION("negative duration") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"duration_ms\": 1000", "\"duration_ms\": -1"))));
    }

    SECTION("unknown layer type") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"type\": \"solid\"", "\"type\": \"blob\""))));
    }

    SECTION("unknown easing") {
        const std::string text = R"({
          "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
          "layers": [
            { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 0,
              "transform": { "x": { "keyframes": [ { "t": 0, "v": 1, "easing": "bouncy" } ] } } }
          ]
        })";
        CHECK(is_error(project::load_from_string(text)));
    }

    SECTION("layer ends before it starts") {
        CHECK(is_error(project::load_from_string(
            with_replaced(kValid, "\"start_ms\": 0, \"end_ms\": 500", "\"start_ms\": 500, \"end_ms\": 100"))));
    }

    SECTION("duplicate layer ids") {
        CHECK(is_error(project::load_from_string(with_replaced(kValid, "\"id\": \"b\"", "\"id\": \"a\""))));
    }

    SECTION("keyframes not sorted by time") {
        const std::string text = R"({
          "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
          "layers": [
            { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 0,
              "transform": { "x": { "keyframes": [
                { "t": 100, "v": 1, "easing": "linear" },
                { "t": 50, "v": 2, "easing": "linear" } ] } } }
          ]
        })";
        CHECK(is_error(project::load_from_string(text)));
    }

    SECTION("opacity outside 0..1 is allowed") {
        const std::string text = R"({
          "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
          "layers": [
            { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 0,
              "transform": { "opacity": 5.0 } }
          ]
        })";
        const std::variant<Project, ProjectError> result = project::load_from_string(text);
        REQUIRE(std::holds_alternative<Project>(result));
        CHECK(std::get<Project>(result).layers[0].transform.opacity.constant == 5.0);
    }
}

TEST_CASE("malformed JSON returns an error instead of crashing", "[project]") {
    CHECK(is_error(project::load_from_string("{ this is not json")));
    CHECK(is_error(project::load_from_string("[1, 2, 3]")));
    CHECK(is_error(project::load_from_string("")));

    // Out-of-range numbers must be rejected, not converted (UB / UBSan trap).
    CHECK(is_error(project::load_from_string(
        with_replaced(kValid, "\"duration_ms\": 1000", "\"duration_ms\": 1e300"))));
    CHECK(is_error(project::load_from_string(
        with_replaced(kValid, "\"width\": 640", "\"width\": 1e300"))));
}

TEST_CASE("save_to_file and load_from_file round trip", "[project]") {
    const std::variant<Project, ProjectError> loaded = project::load_from_string(kValid);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const Project original = std::get<Project>(loaded);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("omproj_roundtrip_" + std::to_string(::getpid()) + ".omproj");

    CHECK_FALSE(project::save_to_file(original, path.string()).has_value());

    const std::variant<Project, ProjectError> reloaded = project::load_from_file(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(std::holds_alternative<Project>(reloaded));
    CHECK(projects_equal(original, std::get<Project>(reloaded)));

    // Layers without a "transform" key fall back to scale/opacity defaults of 1.
    CHECK(std::get<Project>(reloaded).layers[0].transform.scale_x.constant == 1.0);
    CHECK(std::get<Project>(reloaded).layers[0].transform.opacity.constant == 1.0);
}

TEST_CASE("audio layer JSON round trips", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "audio", "start_ms": 0, "end_ms": 1000,
          "source": "music.wav", "in_ms": 250, "volume": 0.5 }
      ]
    })";
    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));

    const Layer& layer = std::get<Project>(loaded).layers[0];
    CHECK(layer.type == LayerType::Audio);
    CHECK(layer.in_ms == 250);
    CHECK(layer.volume == 0.5);

    const std::variant<Project, ProjectError> reloaded =
        project::load_from_string(project::to_json_string(std::get<Project>(loaded)));
    REQUIRE(std::holds_alternative<Project>(reloaded));
    CHECK(projects_equal(std::get<Project>(loaded), std::get<Project>(reloaded)));

    // in_ms/volume are always written for audio layers.
    const std::string encoded = project::to_json_string(std::get<Project>(reloaded));
    CHECK(encoded.find("\"in_ms\"") != std::string::npos);
    CHECK(encoded.find("\"volume\"") != std::string::npos);
}

TEST_CASE("audio layer defaults apply when in_ms and volume are missing", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "audio", "start_ms": 0, "end_ms": 1000, "source": "music.wav" }
      ]
    })";
    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));

    const Layer& layer = std::get<Project>(loaded).layers[0];
    CHECK(layer.in_ms == 0);
    CHECK(layer.volume == 1.0);
}

TEST_CASE("non-audio layers omit in_ms and volume", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 1000, "color": "#000000" }
      ]
    })";
    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));

    const std::string encoded = project::to_json_string(std::get<Project>(loaded));
    CHECK(encoded.find("\"in_ms\"") == std::string::npos);
    CHECK(encoded.find("\"volume\"") == std::string::npos);
}

TEST_CASE("audio layer validation rejects invalid values", "[project]") {
    const std::string base = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "audio", "start_ms": 0, "end_ms": 1000,
          "source": "music.wav", "in_ms": 0, "volume": 1.0 }
      ]
    })";

    SECTION("negative in_ms") {
        CHECK(is_error(project::load_from_string(with_replaced(base, "\"in_ms\": 0", "\"in_ms\": -1"))));
    }

    SECTION("volume below zero") {
        CHECK(is_error(project::load_from_string(with_replaced(base, "\"volume\": 1.0", "\"volume\": -0.1"))));
    }

    SECTION("volume above ten") {
        CHECK(is_error(project::load_from_string(with_replaced(base, "\"volume\": 1.0", "\"volume\": 10.1"))));
    }

    SECTION("volume far out of range") {
        CHECK(is_error(project::load_from_string(with_replaced(base, "\"volume\": 1.0", "\"volume\": 1e300"))));
    }
}

TEST_CASE("cubic_bezier easing JSON round trips", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "transform": { "x": { "keyframes": [
            { "t": 0, "v": 0.0, "easing": "cubic_bezier", "bezier": [0.42, 0.0, 0.58, 1.0] },
            { "t": 1000, "v": 100.0, "easing": "linear" } ] } } }
      ]
    })";

    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const Project& project_value = std::get<Project>(loaded);

    const Property& x = project_value.layers[0].transform.x;
    REQUIRE(x.keyframes.size() == 2);
    CHECK(x.keyframes[0].easing == Easing::CubicBezier);
    CHECK(x.keyframes[0].bezier[0] == 0.42);
    CHECK(x.keyframes[0].bezier[1] == 0.0);
    CHECK(x.keyframes[0].bezier[2] == 0.58);
    CHECK(x.keyframes[0].bezier[3] == 1.0);

    const std::string encoded = project::to_json_string(project_value);
    const std::variant<Project, ProjectError> reloaded = project::load_from_string(encoded);
    REQUIRE(std::holds_alternative<Project>(reloaded));
    CHECK(projects_equal(project_value, std::get<Project>(reloaded)));

    // "bezier" is emitted for the cubic_bezier keyframe and nowhere else.
    const std::string expected_bezier =
        "            {\n"
        "              \"bezier\": [\n"
        "                0.42,\n"
        "                0.0,\n"
        "                0.58,\n"
        "                1.0\n"
        "              ],\n"
        "              \"easing\": \"cubic_bezier\",\n"
        "              \"t\": 0,\n"
        "              \"v\": 0.0\n"
        "            }";
    CHECK(encoded.find(expected_bezier) != std::string::npos);
    CHECK(encoded.find("\"cubic_bezier\"") != std::string::npos);
    CHECK(encoded.find("\"easing\": \"linear\"") != std::string::npos);
    // Exactly one keyframe carries control points.
    CHECK(count_occurrences(encoded, "\"bezier\": [") == 1);
}

TEST_CASE("other easings serialize without a bezier field", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "transform": { "x": { "keyframes": [
            { "t": 0, "v": 0.0, "easing": "ease_in" },
            { "t": 500, "v": 1.0, "easing": "ease_out" },
            { "t": 1000, "v": 2.0, "easing": "hold" } ] } } }
      ]
    })";

    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const std::string encoded = project::to_json_string(std::get<Project>(loaded));

    CHECK(encoded.find("bezier") == std::string::npos);
    // The serialized keyframe objects are exactly as before this change.
    CHECK(encoded.find(R"(            {
              "easing": "ease_in",
              "t": 0,
              "v": 0.0
            })") != std::string::npos);
    CHECK(encoded.find(R"(            {
              "easing": "ease_out",
              "t": 500,
              "v": 1.0
            })") != std::string::npos);
    CHECK(encoded.find(R"(            {
              "easing": "hold",
              "t": 1000,
              "v": 2.0
            })") != std::string::npos);
}

TEST_CASE("bezier control points on a non-cubic_bezier keyframe are ignored", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "transform": { "x": { "keyframes": [
            { "t": 0, "v": 0.0, "easing": "ease_in", "bezier": [0.1, 0.2, 0.3, 0.4] } ] } } }
      ]
    })";

    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const Keyframe& keyframe = std::get<Project>(loaded).layers[0].transform.x.keyframes[0];
    CHECK(keyframe.easing == Easing::EaseIn);
    // The default control points survive; the stray "bezier" is not read.
    CHECK(keyframe.bezier == std::array<double, 4>{0.0, 0.0, 1.0, 1.0});
}

TEST_CASE("cubic_bezier validation rejects invalid bezier fields", "[project]") {
    const std::string prefix = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
      "layers": [
        { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 0,
          "transform": { "x": { "keyframes": [ )";
    const std::string suffix = R"( ] } } }
      ]
    })";

    auto with_keyframe = [&](const std::string& keyframe) {
        return prefix + keyframe + suffix;
    };

    SECTION("missing bezier") {
        CHECK(is_error(project::load_from_string(
            with_keyframe(R"({ "t": 0, "v": 1, "easing": "cubic_bezier" })"))));
    }

    SECTION("bezier is not an array") {
        CHECK(is_error(project::load_from_string(
            with_keyframe(R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": 1 })"))));
    }

    SECTION("bezier has the wrong length") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, 1.0] })"))));
    }

    SECTION("bezier contains a non-number") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, 1.0, "x"] })"))));
    }

    SECTION("bezier contains a non-finite value") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, 1.0, 1e400] })"))));
    }

    SECTION("x1 below zero") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [-0.1, 0.0, 1.0, 1.0] })"))));
    }

    SECTION("x1 above one") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [1.1, 0.0, 1.0, 1.0] })"))));
    }

    SECTION("x2 below zero") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, -0.5, 1.0] })"))));
    }

    SECTION("x2 above one") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, 1.5, 1.0] })"))));
    }

    SECTION("y1 far out of range") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 11.0, 1.0, 1.0] })"))));
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, -10.5, 1.0, 1.0] })"))));
    }

    SECTION("y2 far out of range") {
        CHECK(is_error(project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.0, 0.0, 1.0, 10.5] })"))));
    }

    SECTION("overshooting y within the limit is accepted") {
        const std::variant<Project, ProjectError> result = project::load_from_string(with_keyframe(
            R"({ "t": 0, "v": 1, "easing": "cubic_bezier", "bezier": [0.25, -1.5, 0.75, 1.5] })"));
        REQUIRE(std::holds_alternative<Project>(result));
        const Keyframe& keyframe = std::get<Project>(result).layers[0].transform.x.keyframes[0];
        CHECK(keyframe.bezier[1] == -1.5);
        CHECK(keyframe.bezier[3] == 1.5);
    }
}

TEST_CASE("cubic_bezier keyframe round trips through save_to_file", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 1000,
      "layers": [
        { "id": "a", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "transform": { "opacity": { "keyframes": [
            { "t": 0, "v": 0.0, "easing": "cubic_bezier", "bezier": [0.9, 0.1, 1.0, 0.5] },
            { "t": 1000, "v": 1.0, "easing": "linear" } ] } } }
      ]
    })";

    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const Project original = std::get<Project>(loaded);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("omproj_bezier_" + std::to_string(::getpid()) + ".omproj");

    CHECK_FALSE(project::save_to_file(original, path.string()).has_value());

    const std::variant<Project, ProjectError> reloaded = project::load_from_file(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(std::holds_alternative<Project>(reloaded));
    CHECK(projects_equal(original, std::get<Project>(reloaded)));
}

TEST_CASE("a hand-built cubic_bezier project serializes and reloads", "[project]") {
    Project project;
    project.version = 1;
    project.name = "Bezier";
    project.width = 10;
    project.height = 10;
    project.fps = 1.0;
    project.duration_ms = 1000;

    Layer layer;
    layer.id = "a";
    layer.type = LayerType::Rect;
    layer.start_ms = 0;
    layer.end_ms = 1000;
    layer.transform = Transform{};
    layer.transform.scale_x.constant = 1.0;
    layer.transform.scale_y.constant = 1.0;
    layer.transform.opacity.constant = 1.0;
    layer.transform.x.keyframes = {make_bezier_keyframe(0, 0.0, 0.42, 0.0, 0.58, 1.0),
                                   make_keyframe(1000, 100.0, Easing::Linear)};
    project.layers.push_back(layer);

    const std::string encoded = project::to_json_string(project);
    CHECK(encoded.find("\"cubic_bezier\"") != std::string::npos);
    CHECK(encoded.find("bezier") != std::string::npos);

    const std::variant<Project, ProjectError> reloaded = project::load_from_string(encoded);
    REQUIRE(std::holds_alternative<Project>(reloaded));
    CHECK(projects_equal(project, std::get<Project>(reloaded)));
}

TEST_CASE("blend round trips for every mode", "[project]") {
    const struct {
        const char* json_name;
        project::BlendMode mode;
    } cases[] = {
        {"normal", project::BlendMode::Normal},
        {"multiply", project::BlendMode::Multiply},
        {"screen", project::BlendMode::Screen},
        {"overlay", project::BlendMode::Overlay},
        {"exclusion", project::BlendMode::Exclusion},
        {"color_burn", project::BlendMode::ColorBurn},
        {"linear_dodge", project::BlendMode::LinearDodge},
    };

    for (const auto& test : cases) {
        const std::string text = std::string(R"({
          "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
          "layers": [
            { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 0,
              "color": "#ffffff", "blend": ")" ) + test.json_name + R"(" }
          ]
        })";

        const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
        REQUIRE(std::holds_alternative<Project>(loaded));
        CHECK(std::get<Project>(loaded).layers[0].blend == test.mode);

        const std::string encoded = project::to_json_string(std::get<Project>(loaded));
        const std::variant<Project, ProjectError> reloaded = project::load_from_string(encoded);
        REQUIRE(std::holds_alternative<Project>(reloaded));
        CHECK(std::get<Project>(reloaded).layers[0].blend == test.mode);
        CHECK(projects_equal(std::get<Project>(loaded), std::get<Project>(reloaded)));
    }
}

TEST_CASE("blend is omitted for Normal and written for the others", "[project]") {
    const std::string base = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
      "layers": [
        { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 0, "color": "#ffffff" }
      ]
    })";

    // A default-constructed layer is Normal, so no "blend" key is emitted and
    // the JSON stays byte-identical to a project that never had the field.
    const std::variant<Project, ProjectError> normal = project::load_from_string(base);
    REQUIRE(std::holds_alternative<Project>(normal));
    const std::string encoded_normal = project::to_json_string(std::get<Project>(normal));
    CHECK(encoded_normal.find("blend") == std::string::npos);

    // The same project with an explicit non-Normal mode does emit the key.
    const std::variant<Project, ProjectError> multiply = project::load_from_string(
        with_replaced(base, "\"color\": \"#ffffff\"", "\"color\": \"#ffffff\", \"blend\": \"multiply\""));
    REQUIRE(std::holds_alternative<Project>(multiply));
    const std::string encoded_multiply = project::to_json_string(std::get<Project>(multiply));
    CHECK(encoded_multiply.find("\"blend\": \"multiply\"") != std::string::npos);
}

TEST_CASE("an unknown blend name is a ProjectError", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
      "layers": [
        { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 0, "blend": "glow" }
      ]
    })";
    const std::variant<Project, ProjectError> result = project::load_from_string(text);
    REQUIRE(std::holds_alternative<ProjectError>(result));
    CHECK(std::get<ProjectError>(result).message.find("glow") != std::string::npos);
}

TEST_CASE("a non-string blend value is a ProjectError", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
      "layers": [
        { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 0, "blend": 3 }
      ]
    })";
    CHECK(is_error(project::load_from_string(text)));
}

TEST_CASE("a blend mode leaves the rest of the layer JSON unchanged", "[project]") {
    const std::string text = R"({
      "version": 1, "width": 10, "height": 10, "fps": 1.0, "duration_ms": 0,
      "layers": [
        { "id": "a", "type": "solid", "start_ms": 0, "end_ms": 0,
          "color": "#112233", "blend": "screen" }
      ]
    })";

    const std::variant<Project, ProjectError> loaded = project::load_from_string(text);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const std::string encoded = project::to_json_string(std::get<Project>(loaded));

    // Every other field is emitted exactly as before; only "blend" is new.
    CHECK(encoded.find(R"("id": "a")") != std::string::npos);
    CHECK(encoded.find(R"("type": "solid")") != std::string::npos);
    CHECK(encoded.find(R"("color": "#112233")") != std::string::npos);
    CHECK(encoded.find(R"("blend": "screen")") != std::string::npos);

    // A Normal layer serializes with no "blend" at all.
    const std::variant<Project, ProjectError> plain = project::load_from_string(
        with_replaced(text, ", \"blend\": \"screen\"", ""));
    REQUIRE(std::holds_alternative<Project>(plain));
    CHECK(project::to_json_string(std::get<Project>(plain)).find("blend") == std::string::npos);
}
