// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "../src/internal/image_loader.hpp"
#include "../src/internal/shell_quote.hpp"
#include "openmotion/media.hpp"
#include "openmotion/project.hpp"

namespace {

namespace project = openmotion::project;
namespace renderer = openmotion::renderer;

using project::Layer;
using project::LayerType;
using project::Project;

Project make_project(int width, int height, double fps, int64_t duration_ms) {
    Project project;
    project.version = 1;
    project.name = "test";
    project.width = width;
    project.height = height;
    project.fps = fps;
    project.duration_ms = duration_ms;
    return project;
}

Layer make_layer(const std::string& id, LayerType type, int64_t start_ms, int64_t end_ms) {
    Layer layer;
    layer.id = id;
    layer.type = type;
    layer.name = id;
    layer.start_ms = start_ms;
    layer.end_ms = end_ms;
    layer.transform.scale_x.constant = 1.0;
    layer.transform.scale_y.constant = 1.0;
    layer.transform.opacity.constant = 1.0;
    return layer;
}

renderer::Frame rendered(const project::Project& p, int64_t t_ms) {
    const std::variant<renderer::Frame, renderer::RenderError> result = renderer::render_frame(p, t_ms);
    REQUIRE(std::holds_alternative<renderer::Frame>(result));
    return std::get<renderer::Frame>(result);
}

// Reads the RGB channels at (x, y) as ints for easy comparison.
void pixel(const renderer::Frame& frame, int x, int y, int& red, int& green, int& blue) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
         static_cast<std::size_t>(x)) *
        4;
    red = frame.rgba[index + 0];
    green = frame.rgba[index + 1];
    blue = frame.rgba[index + 2];
}

bool command_available(const std::string& name) {
    const std::string command = "command -v " + name + " > /dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

renderer::Texture make_texture(int width, int height, std::vector<uint8_t> rgba) {
    renderer::Texture texture;
    texture.width = width;
    texture.height = height;
    texture.rgba = std::move(rgba);
    return texture;
}

// Uniform white texture with the given alpha.
renderer::Texture make_uniform_texture(int width, int height, uint8_t alpha) {
    std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                              255);
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        rgba[i] = alpha;
    }
    return make_texture(width, height, std::move(rgba));
}

// Renders with a cache that already holds `texture` under `source`.
renderer::Frame rendered_with_image(const Project& project, int64_t t_ms, const std::string& source,
                                    renderer::Texture texture) {
    renderer::RenderCache cache;
    cache.images.emplace(source, std::move(texture));
    const std::variant<renderer::Frame, renderer::RenderError> result =
        renderer::render_frame(project, t_ms, cache);
    REQUIRE(std::holds_alternative<renderer::Frame>(result));
    return std::get<renderer::Frame>(result);
}

}  // namespace

TEST_CASE("a solid layer fills the whole canvas", "[renderer]") {
    Project project = make_project(16, 12, 30.0, 1000);
    Layer solid = make_layer("bg", LayerType::Solid, 0, 1000);
    solid.color = "#3366cc";
    project.layers.push_back(solid);

    const renderer::Frame& frame = rendered(project, 0);
    CHECK(frame.width == 16);
    CHECK(frame.height == 12);
    REQUIRE(frame.rgba.size() == 16u * 12u * 4u);

    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 16; ++x) {
            int red = 0, green = 0, blue = 0;
            pixel(frame, x, y, red, green, blue);
            CHECK(red == 0x33);
            CHECK(green == 0x66);
            CHECK(blue == 0xcc);
        }
    }
}

TEST_CASE("a rect draws inside its bounds and leaves the rest black", "[renderer]") {
    Project project = make_project(20, 20, 30.0, 1000);
    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#ff0000";
    rect.width = 6.0;
    rect.height = 4.0;
    rect.transform.x.constant = 10.0;
    rect.transform.y.constant = 10.0;
    project.layers.push_back(rect);

    const renderer::Frame& frame = rendered(project, 0);

    int red = 0, green = 0, blue = 0;
    pixel(frame, 10, 10, red, green, blue);  // inside (center)
    CHECK(red == 255);
    CHECK(green == 0);
    CHECK(blue == 0);

    pixel(frame, 0, 0, red, green, blue);  // outside
    CHECK(red == 0);
    CHECK(green == 0);
    CHECK(blue == 0);

    pixel(frame, 19, 19, red, green, blue);  // outside
    CHECK(red == 0);
    CHECK(green == 0);
    CHECK(blue == 0);
}

TEST_CASE("opacity blends the layer over black", "[renderer]") {
    Project project = make_project(8, 8, 30.0, 1000);
    Layer solid = make_layer("bg", LayerType::Solid, 0, 1000);
    solid.color = "#ffffff";
    solid.transform.opacity.constant = 0.5;
    project.layers.push_back(solid);

    const renderer::Frame& frame = rendered(project, 0);
    int red = 0, green = 0, blue = 0;
    pixel(frame, 4, 4, red, green, blue);
    CHECK(red == Catch::Approx(127).margin(2));
    CHECK(green == Catch::Approx(127).margin(2));
    CHECK(blue == Catch::Approx(127).margin(2));
}

TEST_CASE("rotating a non-square rect by 90 degrees swaps its extents", "[renderer]") {
    Project project = make_project(40, 40, 30.0, 1000);
    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#00ff00";
    rect.width = 20.0;
    rect.height = 6.0;
    rect.transform.x.constant = 20.0;
    rect.transform.y.constant = 20.0;
    rect.transform.rotation.constant = 90.0;
    project.layers.push_back(rect);

    const renderer::Frame& frame = rendered(project, 0);

    // Before rotation the rect is wide (20) and short (6); after 90 degrees it
    // should be tall (20) and narrow (6): a pixel 7.5px above the centre is
    // covered, while a pixel 7.5px to the side is not.
    int red = 0, green = 0, blue = 0;
    pixel(frame, 20, 12, red, green, blue);  // 7.5 above centre -> inside
    CHECK(green == 255);

    pixel(frame, 12, 20, red, green, blue);  // 7.5 to the left -> outside
    CHECK(green == 0);
}

TEST_CASE("inactive layers are not drawn", "[renderer]") {
    Project project = make_project(8, 8, 30.0, 1000);
    Layer solid = make_layer("bg", LayerType::Solid, 100, 200);
    solid.color = "#ffffff";
    project.layers.push_back(solid);

    int red = 0, green = 0, blue = 0;

    pixel(rendered(project, 99), 4, 4, red, green, blue);
    CHECK(red == 0);  // before start

    pixel(rendered(project, 100), 4, 4, red, green, blue);
    CHECK(red == 255);  // start is inclusive

    pixel(rendered(project, 199), 4, 4, red, green, blue);
    CHECK(red == 255);  // still inside

    pixel(rendered(project, 200), 4, 4, red, green, blue);
    CHECK(red == 0);  // end is exclusive
}

TEST_CASE("invalid colors and extreme geometry never crash", "[renderer]") {
    SECTION("invalid color returns a RenderError") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer solid = make_layer("bg", LayerType::Solid, 0, 1000);
        solid.color = "not-a-color";
        project.layers.push_back(solid);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::RenderError>(result));
        CHECK_FALSE(std::get<renderer::RenderError>(result).message.empty());
    }

    SECTION("huge, negative and non-finite coordinates are clipped") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = 4.0;
        rect.height = 4.0;
        rect.transform.x.constant = std::numeric_limits<double>::quiet_NaN();
        rect.transform.y.constant = -1.0e9;
        project.layers.push_back(rect);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::Frame>(result));
        const renderer::Frame& frame = std::get<renderer::Frame>(result);
        REQUIRE(frame.rgba.size() == 8u * 8u * 4u);
    }

    SECTION("a rect with non-positive size draws nothing") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = 0.0;
        rect.height = 10.0;
        rect.transform.x.constant = 4.0;
        rect.transform.y.constant = 4.0;
        project.layers.push_back(rect);

        const renderer::Frame& frame = rendered(project, 0);
        int red = 0, green = 0, blue = 0;
        pixel(frame, 4, 4, red, green, blue);
        CHECK(red == 0);
    }
}

TEST_CASE("hello.omproj renders at 1280x720 and the rect moves", "[renderer]") {
    const std::string path = std::string(OPENMOTION_EXAMPLE_DIR) + "/hello.omproj";
    const std::variant<Project, project::ProjectError> loaded = project::load_from_file(path);
    REQUIRE(std::holds_alternative<Project>(loaded));
    const Project& project = std::get<Project>(loaded);

    const renderer::Frame& at_zero = rendered(project, 0);
    const renderer::Frame& at_mid = rendered(project, 1500);

    CHECK(at_zero.width == 1280);
    CHECK(at_zero.height == 720);
    CHECK(at_mid.width == 1280);
    CHECK(at_mid.height == 720);

    // The background solid is "#101018" (red 16); the rect is "#e04030" at
    // opacity 0.75, which blends to red 172. At t=0 the rect centre x is 0
    // (covering canvas x in [0, 100)), and at t=1500 it is 400 (covering
    // [300, 500)), so a pixel near x=50 is rect at t=0 but background at
    // t=1500, and vice versa near x=450.
    int red = 0, green = 0, blue = 0;

    pixel(at_zero, 50, 360, red, green, blue);
    CHECK(red == Catch::Approx(172).margin(2));
    pixel(at_mid, 50, 360, red, green, blue);
    CHECK(red == Catch::Approx(16).margin(2));

    pixel(at_zero, 450, 360, red, green, blue);
    CHECK(red == Catch::Approx(16).margin(2));
    pixel(at_mid, 450, 360, red, green, blue);
    CHECK(red == Catch::Approx(172).margin(2));
}

TEST_CASE("render_to_mp4 writes a playable clip", "[renderer]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    Project project = make_project(64, 64, 10.0, 500);
    Layer solid = make_layer("bg", LayerType::Solid, 0, 500);
    solid.color = "#204060";
    project.layers.push_back(solid);

    const std::filesystem::path clip =
        std::filesystem::temp_directory_path() /
        ("openmotion_render_" + std::to_string(::getpid()) + ".mp4");

    const std::optional<renderer::RenderError> error =
        renderer::render_to_mp4(project, clip.string());
    REQUIRE_FALSE(error.has_value());

    const std::variant<openmotion::media::VideoMetadata, openmotion::media::MediaError> metadata =
        openmotion::media::open_video(clip.string());

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);

    REQUIRE(std::holds_alternative<openmotion::media::VideoMetadata>(metadata));
    const openmotion::media::VideoMetadata& info =
        std::get<openmotion::media::VideoMetadata>(metadata);
    CHECK(info.width == 64);
    CHECK(info.height == 64);
    CHECK(info.duration_ms > 300);
    CHECK(info.duration_ms < 700);
}

TEST_CASE("an image layer draws a cached texture 1:1", "[renderer]") {
    Project project = make_project(8, 8, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "quad";
    image.transform.x.constant = 4.0;
    image.transform.y.constant = 4.0;
    project.layers.push_back(image);

    // 2x2 texture: red, green, blue, white (row-major).
    const std::vector<uint8_t> texels = {
        255, 0,   0,   255,  // (0,0) red
        0,   255, 0,   255,  // (1,0) green
        0,   0,   255, 255,  // (0,1) blue
        255, 255, 255, 255,  // (1,1) white
    };

    const renderer::Frame& frame =
        rendered_with_image(project, 0, "quad", make_texture(2, 2, texels));

    int red = 0, green = 0, blue = 0;
    pixel(frame, 3, 3, red, green, blue);  // top-left quadrant -> red
    CHECK(red == 255);
    CHECK(green == 0);
    CHECK(blue == 0);

    pixel(frame, 4, 3, red, green, blue);  // top-right quadrant -> green
    CHECK(red == 0);
    CHECK(green == 255);
    CHECK(blue == 0);

    pixel(frame, 3, 4, red, green, blue);  // bottom-left quadrant -> blue
    CHECK(red == 0);
    CHECK(green == 0);
    CHECK(blue == 255);

    pixel(frame, 4, 4, red, green, blue);  // bottom-right quadrant -> white
    CHECK(red == 255);
    CHECK(green == 255);
    CHECK(blue == 255);
}

TEST_CASE("image opacity and texture alpha combine multiplicatively", "[renderer]") {
    Project project = make_project(4, 4, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "white";
    image.transform.x.constant = 2.0;
    image.transform.y.constant = 2.0;
    image.transform.opacity.constant = 0.5;
    project.layers.push_back(image);

    const renderer::Frame& frame =
        rendered_with_image(project, 0, "white", make_uniform_texture(1, 1, 128));

    int red = 0, green = 0, blue = 0;
    pixel(frame, 2, 2, red, green, blue);
    // alpha = 0.5 * (128/255) = 0.251 -> 255 * 0.251 ~= 64.
    CHECK(red == Catch::Approx(64).margin(2));
    CHECK(green == Catch::Approx(64).margin(2));
    CHECK(blue == Catch::Approx(64).margin(2));
}

TEST_CASE("image scale doubles the drawn size and rotation swaps extents", "[renderer]") {
    SECTION("scale 2x doubles the size") {
        Project project = make_project(16, 16, 30.0, 1000);
        Layer image = make_layer("img", LayerType::Image, 0, 1000);
        image.source = "white";
        image.transform.x.constant = 8.0;
        image.transform.y.constant = 8.0;
        image.transform.scale_x.constant = 2.0;
        image.transform.scale_y.constant = 2.0;
        project.layers.push_back(image);

        // Native 2x2 at scale 2 covers 4x4, i.e. x,y in [6, 10).
        const renderer::Frame& frame =
            rendered_with_image(project, 0, "white", make_uniform_texture(2, 2, 255));

        int red = 0, green = 0, blue = 0;
        pixel(frame, 7, 8, red, green, blue);  // inside
        CHECK(red == 255);
        pixel(frame, 5, 8, red, green, blue);  // outside
        CHECK(red == 0);
        pixel(frame, 8, 10, red, green, blue);  // outside (y >= 10)
        CHECK(red == 0);
    }

    SECTION("rotation 90 swaps a non-square image's extents") {
        Project project = make_project(32, 32, 30.0, 1000);
        Layer image = make_layer("img", LayerType::Image, 0, 1000);
        image.source = "wide";
        image.transform.x.constant = 16.0;
        image.transform.y.constant = 16.0;
        image.transform.rotation.constant = 90.0;
        project.layers.push_back(image);

        // Native 8x4 (wide). After 90 degrees it should be 4 wide and 8 tall.
        const renderer::Frame& frame =
            rendered_with_image(project, 0, "wide", make_uniform_texture(8, 4, 255));

        int red = 0, green = 0, blue = 0;
        pixel(frame, 16, 12, red, green, blue);  // 4 above centre -> inside
        CHECK(red == 255);
        pixel(frame, 12, 16, red, green, blue);  // 4 to the left -> outside
        CHECK(red == 0);
    }
}

TEST_CASE("a fully transparent texture leaves the canvas unchanged", "[renderer]") {
    Project project = make_project(4, 4, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "clear";
    image.transform.x.constant = 2.0;
    image.transform.y.constant = 2.0;
    project.layers.push_back(image);

    const renderer::Frame& frame =
        rendered_with_image(project, 0, "clear", make_uniform_texture(2, 2, 0));

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int red = 0, green = 0, blue = 0;
            pixel(frame, x, y, red, green, blue);
            CHECK(red == 0);
            CHECK(green == 0);
            CHECK(blue == 0);
        }
    }
}

TEST_CASE("a missing image file reports an error with the path", "[renderer]") {
    Project project = make_project(4, 4, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "/nonexistent/openmotion_missing_image.png";
    project.layers.push_back(image);

    const std::variant<renderer::Frame, renderer::RenderError> result =
        renderer::render_frame(project, 0);
    REQUIRE(std::holds_alternative<renderer::RenderError>(result));
    const std::string message = std::get<renderer::RenderError>(result).message;
    CHECK(message.find(image.source) != std::string::npos);
}

TEST_CASE("oversized image dimensions are rejected", "[renderer]") {
    CHECK(openmotion::internal::is_supported_image_size(1, 1));
    CHECK(openmotion::internal::is_supported_image_size(1920, 1080));
    // 8192 x 8192 RGBA is 256 MB: within both the edge and byte limits.
    CHECK(openmotion::internal::is_supported_image_size(8192, 8192));

    CHECK_FALSE(openmotion::internal::is_supported_image_size(0, 10));
    CHECK_FALSE(openmotion::internal::is_supported_image_size(10, 0));
    CHECK_FALSE(openmotion::internal::is_supported_image_size(-1, 10));
    CHECK_FALSE(openmotion::internal::is_supported_image_size(16385, 1));
    CHECK_FALSE(openmotion::internal::is_supported_image_size(1, 16385));
    // 16384 x 16384 is 1 GB: the edges are allowed but the byte cap is not.
    CHECK_FALSE(openmotion::internal::is_supported_image_size(16384, 16384));
    // 20000 x 20000 exceeds the edge limit outright.
    CHECK_FALSE(openmotion::internal::is_supported_image_size(20000, 20000));
}

TEST_CASE("an image loaded through ffmpeg renders", "[renderer]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    const std::filesystem::path png =
        std::filesystem::temp_directory_path() /
        ("openmotion_image_" + std::to_string(::getpid()) + ".png");

    const std::string generate =
        "ffmpeg -v error -y -f lavfi -i color=c=red:s=16x16 -frames:v 1 " +
        openmotion::internal::shell_quote(png.string());
    REQUIRE(std::system(generate.c_str()) == 0);

    Project project = make_project(16, 16, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = png.string();
    image.transform.x.constant = 8.0;
    image.transform.y.constant = 8.0;
    project.layers.push_back(image);

    const std::variant<renderer::Frame, renderer::RenderError> result =
        renderer::render_frame(project, 0);

    std::error_code ignored;
    std::filesystem::remove(png, ignored);

    REQUIRE(std::holds_alternative<renderer::Frame>(result));
    const renderer::Frame& frame = std::get<renderer::Frame>(result);
    CHECK(frame.width == 16);
    CHECK(frame.height == 16);

    int red = 0, green = 0, blue = 0;
    pixel(frame, 8, 8, red, green, blue);
    CHECK(red > 200);
    CHECK(green < 60);
    CHECK(blue < 60);
}
