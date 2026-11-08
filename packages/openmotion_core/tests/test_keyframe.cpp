// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/keyframe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "openmotion/project.hpp"

namespace {

namespace project = openmotion::project;
namespace keyframe = openmotion::keyframe;

using Catch::Approx;
using project::Easing;
using project::Keyframe;
using project::Layer;
using project::Property;
using project::Transform;

const Easing kAllEasings[] = {Easing::Linear, Easing::EaseIn, Easing::EaseOut,
                              Easing::EaseInOut, Easing::Hold};

Keyframe make_keyframe(int64_t time_ms, double value, Easing easing) {
    return Keyframe{time_ms, value, easing};
}

Keyframe make_bezier_keyframe(int64_t time_ms, double value, double x1, double y1, double x2, double y2) {
    Keyframe keyframe{time_ms, value, Easing::CubicBezier};
    keyframe.bezier = {x1, y1, x2, y2};
    return keyframe;
}

// Defaults matching the project.hpp loader: x=y=rotation=0, scale=1, opacity=1.
Transform make_default_transform() {
    Transform transform;
    transform.scale_x.constant = 1.0;
    transform.scale_y.constant = 1.0;
    transform.opacity.constant = 1.0;
    return transform;
}

}  // namespace

TEST_CASE("apply_easing endpoints, shape and clamping", "[keyframe]") {
    for (const Easing easing : kAllEasings) {
        CHECK(keyframe::apply_easing(easing, 0.0) == Approx(0.0));
        CHECK(keyframe::apply_easing(easing, 1.0) == Approx(1.0));
    }

    CHECK(keyframe::apply_easing(Easing::EaseIn, 0.5) < keyframe::apply_easing(Easing::Linear, 0.5));
    CHECK(keyframe::apply_easing(Easing::EaseOut, 0.5) > keyframe::apply_easing(Easing::Linear, 0.5));
    CHECK(keyframe::apply_easing(Easing::EaseInOut, 0.5) == Approx(0.5));
    CHECK(keyframe::apply_easing(Easing::Hold, 0.99) == Approx(0.0));

    CHECK(keyframe::apply_easing(Easing::Linear, -5.0) == Approx(0.0));
    CHECK(keyframe::apply_easing(Easing::Linear, 2.0) == Approx(1.0));
    CHECK(keyframe::apply_easing(Easing::Hold, -1.0) == Approx(0.0));
    CHECK(keyframe::apply_easing(Easing::Hold, 2.0) == Approx(1.0));
}

TEST_CASE("cubic_bezier_ease matches the identity and reference curves", "[keyframe]") {
    SECTION("identity curve (0,0,1,1) equals u") {
        for (int i = 0; i <= 20; ++i) {
            const double u = static_cast<double>(i) / 20.0;
            CHECK(keyframe::cubic_bezier_ease(0.0, 0.0, 1.0, 1.0, u) == Approx(u).margin(1e-6));
        }
    }

    SECTION("ease-in-out curve (0.42,0,0.58,1) is 0.5 at u = 0.5") {
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 0.5) == Approx(0.5).margin(1e-6));
    }

    SECTION("a symmetric curve satisfies f(u) + f(1-u) = 1") {
        for (int i = 0; i <= 20; ++i) {
            const double u = static_cast<double>(i) / 20.0;
            const double forward = keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, u);
            const double backward = keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 1.0 - u);
            CHECK(forward + backward == Approx(1.0).margin(1e-6));
        }
    }

    SECTION("endpoints are exactly 0 and 1") {
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 0.0) == 0.0);
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 1.0) == 1.0);
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, -3.0) == 0.0);
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 7.0) == 1.0);
    }

    SECTION("NaN progress maps to 0") {
        CHECK(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, std::nan("")) == 0.0);
    }

    SECTION("overshooting y control points leave 0..1 but stay finite") {
        bool below_zero = false;
        bool above_one = false;
        for (int i = 0; i <= 200; ++i) {
            const double u = static_cast<double>(i) / 200.0;
            const double value = keyframe::cubic_bezier_ease(0.25, -1.5, 0.75, 1.5, u);
            CHECK(std::isfinite(value));
            if (value < 0.0) below_zero = true;
            if (value > 1.0) above_one = true;
        }
        CHECK(below_zero);
        CHECK(above_one);
    }

    SECTION("degenerate control points stay finite") {
        for (int i = 0; i <= 20; ++i) {
            const double u = static_cast<double>(i) / 20.0;
            CHECK(std::isfinite(keyframe::cubic_bezier_ease(0.0, 0.0, 0.0, 0.0, u)));
            CHECK(std::isfinite(keyframe::cubic_bezier_ease(1.0, 1.0, 1.0, 1.0, u)));
        }
    }

    SECTION("monotonic non-decreasing when x and y stay within 0..1") {
        double previous = -1.0;
        for (int i = 0; i <= 200; ++i) {
            const double u = static_cast<double>(i) / 200.0;
            const double value = keyframe::cubic_bezier_ease(0.25, 0.1, 0.75, 0.9, u);
            CHECK(value >= previous - 1e-9);
            previous = value;
        }
    }
}

TEST_CASE("apply_easing overloads handle cubic_bezier", "[keyframe]") {
    SECTION("the Easing overload has no control points, so it stays linear") {
        CHECK(keyframe::apply_easing(Easing::CubicBezier, 0.25) == Approx(0.25));
        CHECK(keyframe::apply_easing(Easing::CubicBezier, 0.75) == Approx(0.75));
        CHECK(keyframe::apply_easing(Easing::CubicBezier, 0.0) == Approx(0.0));
        CHECK(keyframe::apply_easing(Easing::CubicBezier, 1.0) == Approx(1.0));
    }

    SECTION("the Keyframe overload uses the stored control points") {
        const Keyframe keyframe = make_bezier_keyframe(0, 0.0, 0.42, 0.0, 0.58, 1.0);
        CHECK(keyframe::apply_easing(keyframe, 0.5) ==
              Approx(keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 0.5)));
    }

    SECTION("non-bezier keyframes fall back to the Easing overload") {
        const Keyframe keyframe = make_keyframe(0, 0.0, Easing::EaseIn);
        CHECK(keyframe::apply_easing(keyframe, 0.5) == Approx(0.125));
    }
}

TEST_CASE("evaluate shapes a segment with cubic_bezier easing", "[keyframe]") {
    Property property;
    property.keyframes = {make_bezier_keyframe(0, 0.0, 0.42, 0.0, 0.58, 1.0),
                          make_keyframe(1000, 100.0, Easing::Linear)};

    // Midpoint of the segment: lerp(0, 100, cubic_bezier_ease(0.42,0,0.58,1, 0.5)).
    const double eased = keyframe::cubic_bezier_ease(0.42, 0.0, 0.58, 1.0, 0.5);
    CHECK(keyframe::evaluate(property, 500) == Approx(0.0 + 100.0 * eased));

    // A non-symmetric curve must move the midpoint away from a linear ramp.
    Property asymmetric;
    asymmetric.keyframes = {make_bezier_keyframe(0, 0.0, 0.9, 0.0, 1.0, 0.1),
                            make_keyframe(1000, 100.0, Easing::Linear)};
    CHECK(keyframe::evaluate(asymmetric, 500) != Approx(50.0));
    CHECK(keyframe::evaluate(asymmetric, 500) ==
          Approx(100.0 * keyframe::cubic_bezier_ease(0.9, 0.0, 1.0, 0.1, 0.5)));

    // The keyframes bounding the segment are still exact.
    CHECK(keyframe::evaluate(property, 0) == Approx(0.0));
    CHECK(keyframe::evaluate(property, 1000) == Approx(100.0));
}

TEST_CASE("evaluate covers constants, bounds, keyframes and segments", "[keyframe]") {
    SECTION("constant property returns its value") {
        Property property;
        property.constant = 42.5;
        CHECK(keyframe::evaluate(property, -1000) == Approx(42.5));
        CHECK(keyframe::evaluate(property, 0) == Approx(42.5));
        CHECK(keyframe::evaluate(property, 9999) == Approx(42.5));
    }

    SECTION("before first and after last keyframe") {
        Property property;
        property.keyframes = {make_keyframe(100, 10.0, Easing::Linear),
                              make_keyframe(500, 30.0, Easing::Linear)};
        CHECK(keyframe::evaluate(property, 0) == Approx(10.0));
        CHECK(keyframe::evaluate(property, 100) == Approx(10.0));
        CHECK(keyframe::evaluate(property, 500) == Approx(30.0));
        CHECK(keyframe::evaluate(property, 100000) == Approx(30.0));
    }

    SECTION("exact keyframe times on a three-keyframe track") {
        Property property;
        property.keyframes = {make_keyframe(0, 0.0, Easing::EaseInOut),
                              make_keyframe(1000, 100.0, Easing::Linear),
                              make_keyframe(2000, 50.0, Easing::Linear)};
        CHECK(keyframe::evaluate(property, 0) == Approx(0.0));
        CHECK(keyframe::evaluate(property, 1000) == Approx(100.0));
        CHECK(keyframe::evaluate(property, 2000) == Approx(50.0));
    }

    SECTION("midpoint of a linear segment") {
        Property property;
        property.keyframes = {make_keyframe(0, 0.0, Easing::Linear),
                              make_keyframe(1000, 100.0, Easing::Linear)};
        CHECK(keyframe::evaluate(property, 250) == Approx(25.0));
        CHECK(keyframe::evaluate(property, 500) == Approx(50.0));
        CHECK(keyframe::evaluate(property, 750) == Approx(75.0));
    }

    SECTION("segment easing comes from its start keyframe") {
        Property property;
        property.keyframes = {make_keyframe(0, 0.0, Easing::EaseIn),
                              make_keyframe(1000, 100.0, Easing::Linear)};
        // u = 0.5 -> ease_in(0.5) = 0.125 -> 0 + 100 * 0.125.
        CHECK(keyframe::evaluate(property, 500) == Approx(12.5));
    }

    SECTION("hold segment stays at the start value until the next keyframe") {
        Property property;
        property.keyframes = {make_keyframe(0, 0.0, Easing::Hold),
                              make_keyframe(1000, 100.0, Easing::Linear)};
        CHECK(keyframe::evaluate(property, 1) == Approx(0.0));
        CHECK(keyframe::evaluate(property, 500) == Approx(0.0));
        CHECK(keyframe::evaluate(property, 999) == Approx(0.0));
        CHECK(keyframe::evaluate(property, 1000) == Approx(100.0));
    }
}

TEST_CASE("duplicate-time keyframes stay finite and do not crash", "[keyframe]") {
    Property property;
    property.keyframes = {make_keyframe(100, 1.0, Easing::Linear),
                          make_keyframe(100, 2.0, Easing::Linear),
                          make_keyframe(200, 3.0, Easing::Linear)};

    for (const int64_t time : {int64_t{0}, int64_t{100}, int64_t{150}, int64_t{200}, int64_t{500}}) {
        CHECK(std::isfinite(keyframe::evaluate(property, time)));
    }
    CHECK(keyframe::evaluate(property, 0) == Approx(1.0));
    CHECK(keyframe::evaluate(property, 150) == Approx(2.5));
    CHECK(keyframe::evaluate(property, 500) == Approx(3.0));
}

TEST_CASE("evaluate_transform evaluates each property", "[keyframe]") {
    Transform transform = make_default_transform();
    transform.x.keyframes = {make_keyframe(0, 0.0, Easing::Linear),
                             make_keyframe(1000, 100.0, Easing::Linear)};
    transform.opacity.constant = 0.25;

    const keyframe::EvaluatedTransform at_zero = keyframe::evaluate_transform(transform, 0);
    CHECK(at_zero.x == Approx(0.0));
    CHECK(at_zero.y == Approx(0.0));
    CHECK(at_zero.scale_x == Approx(1.0));
    CHECK(at_zero.scale_y == Approx(1.0));
    CHECK(at_zero.rotation == Approx(0.0));
    CHECK(at_zero.opacity == Approx(0.25));

    const keyframe::EvaluatedTransform mid = keyframe::evaluate_transform(transform, 500);
    CHECK(mid.x == Approx(50.0));
    CHECK(mid.scale_x == Approx(1.0));
    CHECK(mid.opacity == Approx(0.25));
}

TEST_CASE("is_layer_active uses inclusive start and exclusive end", "[keyframe]") {
    Layer layer;
    layer.start_ms = 100;
    layer.end_ms = 200;

    CHECK_FALSE(keyframe::is_layer_active(layer, 99));
    CHECK(keyframe::is_layer_active(layer, 100));
    CHECK(keyframe::is_layer_active(layer, 150));
    CHECK(keyframe::is_layer_active(layer, 199));
    CHECK_FALSE(keyframe::is_layer_active(layer, 200));
    CHECK_FALSE(keyframe::is_layer_active(layer, 201));
}

TEST_CASE("hello.omproj animated Rect x evaluates as expected", "[keyframe]") {
    const std::string path = std::string(OPENMOTION_EXAMPLE_DIR) + "/hello.omproj";
    const std::variant<project::Project, project::ProjectError> loaded = project::load_from_file(path);
    REQUIRE(std::holds_alternative<project::Project>(loaded));

    const project::Project& project_value = std::get<project::Project>(loaded);
    REQUIRE(project_value.layers.size() == 2);
    const Layer& box = project_value.layers[1];
    REQUIRE(box.id == "box");
    REQUIRE(box.transform.x.keyframes.size() == 3);

    CHECK(keyframe::evaluate(box.transform.x, 0) == Approx(0.0));
    CHECK(keyframe::evaluate(box.transform.x, 3000) == Approx(800.0));
    // t=750 is the midpoint of the linear segment [0,1500]: 0 + 400 * 0.5.
    CHECK(keyframe::evaluate(box.transform.x, 750) == Approx(200.0));
}
