// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/internal/blend.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace {

namespace project = openmotion::project;

using project::BlendMode;

// The full list of modes, so the table and the grid cover every enumerator.
const BlendMode kAllModes[] = {
    BlendMode::Normal, BlendMode::Multiply,  BlendMode::Screen, BlendMode::Overlay,
    BlendMode::Exclusion, BlendMode::ColorBurn, BlendMode::LinearDodge,
};

double blend(BlendMode mode, double cb, double cs) {
    return openmotion::internal::blend_channel(mode, cb, cs);
}

void check_blend(BlendMode mode, double cb, double cs, double expected) {
    CHECK(blend(mode, cb, cs) == Catch::Approx(expected).margin(1e-9));
}

}  // namespace

TEST_CASE("blend_channel normal is the source", "[blend]") {
    check_blend(BlendMode::Normal, 0.3, 0.7, 0.7);
    check_blend(BlendMode::Normal, 0.0, 0.0, 0.0);
    check_blend(BlendMode::Normal, 1.0, 0.25, 0.25);
    check_blend(BlendMode::Normal, 0.9, 0.0, 0.0);
}

TEST_CASE("blend_channel multiply is cb * cs", "[blend]") {
    check_blend(BlendMode::Multiply, 0.5, 0.5, 0.25);
    check_blend(BlendMode::Multiply, 1.0, 0.4, 0.4);
    check_blend(BlendMode::Multiply, 0.0, 0.9, 0.0);
    check_blend(BlendMode::Multiply, 0.25, 0.8, 0.2);
}

TEST_CASE("blend_channel screen is cb + cs - cb * cs", "[blend]") {
    check_blend(BlendMode::Screen, 0.5, 0.5, 0.75);
    check_blend(BlendMode::Screen, 0.0, 0.4, 0.4);
    check_blend(BlendMode::Screen, 1.0, 0.3, 1.0);
    check_blend(BlendMode::Screen, 0.25, 0.25, 0.4375);
}

TEST_CASE("blend_channel overlay has two branches at cb = 0.5", "[blend]") {
    // cb <= 0.5: 2 * cb * cs
    check_blend(BlendMode::Overlay, 0.25, 0.5, 0.25);
    check_blend(BlendMode::Overlay, 0.5, 0.5, 0.5);
    check_blend(BlendMode::Overlay, 0.0, 0.6, 0.0);
    // cb > 0.5: 1 - 2 * (1 - cb) * (1 - cs)
    check_blend(BlendMode::Overlay, 0.75, 0.5, 0.75);
    check_blend(BlendMode::Overlay, 1.0, 0.5, 1.0);
    check_blend(BlendMode::Overlay, 0.6, 0.0, 0.2);
}

TEST_CASE("blend_channel exclusion is cb + cs - 2 * cb * cs", "[blend]") {
    check_blend(BlendMode::Exclusion, 0.5, 0.5, 0.5);
    check_blend(BlendMode::Exclusion, 0.0, 0.4, 0.4);
    check_blend(BlendMode::Exclusion, 1.0, 0.4, 0.6);
    check_blend(BlendMode::Exclusion, 0.25, 0.5, 0.5);
}

TEST_CASE("blend_channel color burn handles cb = 1, cs = 0 and the clamp", "[blend]") {
    check_blend(BlendMode::ColorBurn, 1.0, 0.5, 1.0);   // cb >= 1 short-circuits to 1
    check_blend(BlendMode::ColorBurn, 0.5, 0.0, 0.0);   // cs <= 0 short-circuits to 0
    check_blend(BlendMode::ColorBurn, 0.5, 1.0, 0.5);   // (1 - cb) / cs = 0.5
    check_blend(BlendMode::ColorBurn, 0.8, 0.5, 0.6);   // (1 - cb) / cs = 0.4
    check_blend(BlendMode::ColorBurn, 0.0, 0.5, 0.0);   // (1 - cb) / cs = 2 -> min clamps to 1
    check_blend(BlendMode::ColorBurn, 0.5, 0.5, 0.0);   // (1 - cb) / cs = 1 -> 1 - 1 = 0
}

TEST_CASE("blend_channel linear dodge clamps at one", "[blend]") {
    check_blend(BlendMode::LinearDodge, 0.2, 0.3, 0.5);
    check_blend(BlendMode::LinearDodge, 0.7, 0.8, 1.0);  // 1.5 clamps to 1
    check_blend(BlendMode::LinearDodge, 1.0, 1.0, 1.0);
    check_blend(BlendMode::LinearDodge, 0.0, 0.0, 0.0);
}

TEST_CASE("blend_channel returns cb for a NaN input", "[blend]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (BlendMode mode : kAllModes) {
        // A NaN source leaves the backdrop untouched.
        CHECK(blend(mode, 0.4, nan) == Catch::Approx(0.4).margin(1e-12));
        // A NaN backdrop is returned as-is (which is still NaN).
        CHECK(std::isnan(blend(mode, nan, 0.4)));
    }
}

TEST_CASE("blend_channel stays within [0, 1] over the whole grid", "[blend]") {
    for (BlendMode mode : kAllModes) {
        for (int i = 0; i <= 10; ++i) {
            for (int j = 0; j <= 10; ++j) {
                const double cb = static_cast<double>(i) / 10.0;
                const double cs = static_cast<double>(j) / 10.0;
                const double value = blend(mode, cb, cs);
                CHECK(value >= 0.0);
                CHECK(value <= 1.0);
            }
        }
    }
}
