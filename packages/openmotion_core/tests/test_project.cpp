// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/project.hpp"

#include <catch2/catch_test_macros.hpp>

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
           lhs.height == rhs.height;
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
