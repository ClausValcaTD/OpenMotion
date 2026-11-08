// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "../src/internal/image_loader.hpp"
#include "../src/internal/shell_quote.hpp"
#include "openmotion/media.hpp"
#include "openmotion/project.hpp"

namespace openmotion::renderer {

// Defined in src/renderer.cpp. Declared here because the public header is not
// in scope for this task's changes.
std::string build_video_codec_args(bool have_libx264);

}  // namespace openmotion::renderer

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

// True when ffmpeg is installed and lists libx264 among its encoders.
bool libx264_available() {
    if (!command_available("ffmpeg")) {
        return false;
    }
    const std::string command =
        "ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264";
    return std::system(command.c_str()) == 0;
}

// Runs `command` and returns its stdout (empty on failure).
std::string capture_command(const std::string& command) {
    std::string output;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return output;
    }
    char buffer[256];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe.get())) != 0) {
        output.append(buffer, read);
    }
    return output;
}

// Splices a minimal Exif APP1 segment carrying an IFD0 Orientation tag right
// after the JPEG SOI marker, so the file advertises a rotation without its
// stored pixel dimensions changing.
bool inject_exif_orientation(const std::filesystem::path& path, int orientation) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
    input.close();
    if (data.size() < 2 || static_cast<unsigned char>(data[0]) != 0xFF ||
        static_cast<unsigned char>(data[1]) != 0xD8) {
        return false;
    }

    std::vector<uint8_t> payload;
    const auto put16 = [&payload](uint16_t value) {
        payload.push_back(static_cast<uint8_t>(value & 0xFF));
        payload.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    const auto put32 = [&put16](uint32_t value) {
        put16(static_cast<uint16_t>(value & 0xFFFF));
        put16(static_cast<uint16_t>((value >> 16) & 0xFFFF));
    };
    payload.insert(payload.end(), {'E', 'x', 'i', 'f', 0, 0});  // Exif\0\0
    payload.insert(payload.end(), {'I', 'I'});                  // little-endian TIFF
    put16(0x002A);
    put32(8);       // IFD0 at offset 8
    put16(1);       // one entry
    put16(0x0112);  // Orientation
    put16(3);       // SHORT
    put32(1);       // count
    put16(static_cast<uint16_t>(orientation));
    put16(0);  // value padded to 4 bytes
    put32(0);  // no next IFD

    const std::size_t segment_length = payload.size() + 2;
    std::vector<uint8_t> segment = {0xFF, 0xE1, static_cast<uint8_t>((segment_length >> 8) & 0xFF),
                                    static_cast<uint8_t>(segment_length & 0xFF)};
    segment.insert(segment.end(), payload.begin(), payload.end());

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()), 2);
    output.write(reinterpret_cast<const char*>(segment.data()),
                 static_cast<std::streamsize>(segment.size()));
    output.write(data.data() + 2, static_cast<std::streamsize>(data.size() - 2));
    return static_cast<bool>(output);
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
    // An explicit size keeps the sampled pixel interior rather than an AA edge.
    image.width = 4.0;
    image.height = 4.0;
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

TEST_CASE("bilinear filtering of transparent edges stays premultiplied", "[renderer]") {
    // 2x2 texture: transparent red at (0,0), opaque white everywhere else. A
    // straight-alpha filter would pull the (undefined) red colour of the
    // transparent texel into the sample; premultiplied filtering must not.
    const std::vector<uint8_t> texels = {
        255, 0,   0,   0,    // (0,0) transparent red
        255, 255, 255, 255,  // (1,0) opaque white
        255, 255, 255, 255,  // (0,1) opaque white
        255, 255, 255, 255,  // (1,1) opaque white
    };

    // Canvas 4x4, the 2x2 image scaled 2x to 4x4 centred at (2,2). The pixel at
    // (1,1) lands between the transparent texel and an opaque one, so it is a
    // genuine blend rather than an exact texel hit.
    Project project = make_project(4, 4, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "edge";
    image.transform.x.constant = 2.0;
    image.transform.y.constant = 2.0;
    image.transform.scale_x.constant = 2.0;
    image.transform.scale_y.constant = 2.0;
    project.layers.push_back(image);

    const renderer::Frame& frame =
        rendered_with_image(project, 0, "edge", make_texture(2, 2, texels));

    // Blended over the black canvas with alpha ~0.44, premultiplied filtering
    // keeps the three channels equal; straight-alpha filtering would tint it
    // red (red ~112, green ~ blue ~49) and darken the green/blue channels.
    int red = 0, green = 0, blue = 0;
    pixel(frame, 1, 1, red, green, blue);
    CHECK(red == green);
    CHECK(green == blue);
    CHECK(red >= 100);
}

TEST_CASE("render_to_mp4 tags output as BT.709", "[renderer]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    Project project = make_project(16, 16, 10.0, 200);
    Layer solid = make_layer("bg", LayerType::Solid, 0, 200);
    solid.color = "#c82828";
    project.layers.push_back(solid);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("openmotion_bt709_" + std::to_string(::getpid()) + ".mp4");

    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string());
    REQUIRE_FALSE(result.has_value());

    const std::string probe = "ffprobe -v error -select_streams v:0 "
                              "-show_entries stream=color_space,color_primaries,color_transfer,"
                              "color_range -of default=noprint_wrappers=1 " +
                              openmotion::internal::shell_quote(output.string());
    const std::string info = capture_command(probe);

    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    CHECK(info.find("color_space=bt709") != std::string::npos);
    CHECK(info.find("color_primaries=bt709") != std::string::npos);
    CHECK(info.find("color_transfer=bt709") != std::string::npos);
    CHECK(info.find("color_range=tv") != std::string::npos);
}

TEST_CASE("the image loader ignores rotation tags", "[renderer]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    const std::filesystem::path jpeg =
        std::filesystem::temp_directory_path() /
        ("openmotion_rotated_" + std::to_string(::getpid()) + ".jpg");

    // 32x8 image with a green patch in the top-left corner.
    const std::string generate =
        "ffmpeg -v error -y -f lavfi -i color=c=red:s=32x8 "
        "-vf \"drawbox=x=0:y=0:w=8:h=8:color=green:t=fill\" -frames:v 1 " +
        openmotion::internal::shell_quote(jpeg.string());
    REQUIRE(std::system(generate.c_str()) == 0);

    // Splice in a minimal Exif APP1 block with Orientation = 6 (rotate 90 CW).
    REQUIRE(inject_exif_orientation(jpeg, 6));

    const std::variant<renderer::Texture, renderer::RenderError> loaded =
        openmotion::internal::load_image(jpeg.string());

    std::error_code ignored;
    std::filesystem::remove(jpeg, ignored);

    REQUIRE(std::holds_alternative<renderer::Texture>(loaded));
    const renderer::Texture& texture = std::get<renderer::Texture>(loaded);

    // The loader must keep the stored (un-rotated) dimensions, not the 8x32 the
    // rotation tag would imply.
    CHECK(texture.width == 32);
    CHECK(texture.height == 8);

    const auto channel = [&](int x, int y, int c) {
        return static_cast<int>(
            texture.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(texture.width) +
                          static_cast<std::size_t>(x)) *
                             4 +
                         static_cast<std::size_t>(c)]);
    };
    CHECK(channel(2, 2, 1) > 90);    // top-left patch stays green (#008000)
    CHECK(channel(2, 2, 0) < 60);
    CHECK(channel(28, 6, 0) > 150);  // far corner stays red
    CHECK(channel(28, 6, 1) < 60);
}

TEST_CASE("build_video_codec_args reflects libx264 availability", "[renderer]") {
    // With libx264 the encoder and quality are pinned explicitly.
    const std::string with = renderer::build_video_codec_args(true);
    CHECK(with.find("-c:v libx264") != std::string::npos);
    CHECK(with.find("-preset veryfast") != std::string::npos);
    CHECK(with.find("-crf 18") != std::string::npos);
    CHECK(with.find("-movflags +faststart") != std::string::npos);

    // Without it, nothing is added so ffmpeg keeps its default encoder.
    CHECK(renderer::build_video_codec_args(false).empty());
}

TEST_CASE("render_to_mp4 encodes H.264 as yuv420p and keeps the BT.709 tags", "[renderer]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe") || !libx264_available()) {
        SKIP("ffmpeg/ffprobe with libx264 are not available");
    }

    Project project = make_project(16, 16, 10.0, 200);
    Layer solid = make_layer("bg", LayerType::Solid, 0, 200);
    solid.color = "#20a040";
    project.layers.push_back(solid);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("openmotion_h264_" + std::to_string(::getpid()) + ".mp4");

    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string());
    REQUIRE_FALSE(result.has_value());

    const std::string probe =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=codec_name,pix_fmt,color_space,color_primaries,color_transfer,"
        "color_range -of default=noprint_wrappers=1 " +
        openmotion::internal::shell_quote(output.string());
    const std::string info = capture_command(probe);

    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    CHECK(info.find("codec_name=h264") != std::string::npos);
    CHECK(info.find("pix_fmt=yuv420p") != std::string::npos);
    CHECK(info.find("color_space=bt709") != std::string::npos);
    CHECK(info.find("color_primaries=bt709") != std::string::npos);
    CHECK(info.find("color_transfer=bt709") != std::string::npos);
    CHECK(info.find("color_range=tv") != std::string::npos);
}

TEST_CASE("anti-aliasing: a pixel-aligned rect matches the hard-edged result", "[renderer][aa]") {
    Project project = make_project(20, 20, 30.0, 1000);
    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#ff0000";
    rect.width = 6.0;
    rect.height = 4.0;
    rect.transform.x.constant = 10.0;
    rect.transform.y.constant = 10.0;
    project.layers.push_back(rect);

    // The rect spans x in [7, 13), y in [8, 12), which is aligned to the pixel
    // grid, so every pixel is either fully covered or untouched: the analytic
    // coverage reproduces the old inside/outside test exactly.
    const renderer::Frame& frame = rendered(project, 0);

    int red = 0, green = 0, blue = 0;
    pixel(frame, 10, 10, red, green, blue);  // interior
    CHECK(red == 255);
    pixel(frame, 7, 8, red, green, blue);  // top-left corner pixel
    CHECK(red == 255);
    pixel(frame, 12, 11, red, green, blue);  // bottom-right corner pixel
    CHECK(red == 255);
    pixel(frame, 6, 10, red, green, blue);  // one column left of the rect
    CHECK(red == 0);
    pixel(frame, 13, 10, red, green, blue);  // one column right of the rect
    CHECK(red == 0);
    pixel(frame, 10, 7, red, green, blue);  // one row above the rect
    CHECK(red == 0);
    pixel(frame, 10, 12, red, green, blue);  // one row below the rect
    CHECK(red == 0);
}

TEST_CASE("anti-aliasing: a rect edge at x = 10.5 is half covered in column 10", "[renderer][aa]") {
    Project project = make_project(24, 24, 30.0, 1000);
    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#ffffff";
    rect.width = 4.0;  // left edge at 12.5 - 2.0 = 10.5
    rect.height = 10.0;
    rect.transform.x.constant = 12.5;
    rect.transform.y.constant = 12.0;
    project.layers.push_back(rect);

    const renderer::Frame& frame = rendered(project, 0);

    int red = 0, green = 0, blue = 0;
    pixel(frame, 10, 12, red, green, blue);  // centre 10.5 sits on the left edge
    CHECK(red == Catch::Approx(127).margin(2));
    pixel(frame, 14, 12, red, green, blue);  // centre 14.5 sits on the right edge
    CHECK(red == Catch::Approx(127).margin(2));
    pixel(frame, 11, 12, red, green, blue);  // fully inside
    CHECK(red == 255);
    pixel(frame, 9, 12, red, green, blue);  // fully outside
    CHECK(red == 0);
}

TEST_CASE("anti-aliasing: a 45-degree rotated square has soft edges and area coverage",
          "[renderer][aa]") {
    Project project = make_project(100, 100, 30.0, 1000);
    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#ffffff";
    rect.width = 40.0;
    rect.height = 40.0;
    rect.transform.x.constant = 50.0;
    rect.transform.y.constant = 50.0;
    rect.transform.rotation.constant = 45.0;
    project.layers.push_back(rect);

    const renderer::Frame& frame = rendered(project, 0);

    double total = 0.0;
    bool saw_partial = false;
    bool saw_full = false;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            int red = 0, green = 0, blue = 0;
            pixel(frame, x, y, red, green, blue);
            total += static_cast<double>(red) / 255.0;
            if (red > 0 && red < 255) saw_partial = true;
            if (red == 255) saw_full = true;
        }
    }

    CHECK(saw_partial);  // the rotated edges are anti-aliased
    CHECK(saw_full);     // the interior stays solid
    // The summed coverage approximates the square's area (40 * 40 = 1600).
    CHECK(total == Catch::Approx(1600.0).epsilon(0.01));
}

TEST_CASE("anti-aliasing: an image layer's edges are partially blended", "[renderer][aa]") {
    Project project = make_project(8, 8, 30.0, 1000);
    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "white";
    // A half-pixel offset puts the 2x2 texture's edges between pixel centres.
    image.transform.x.constant = 4.5;
    image.transform.y.constant = 4.5;
    project.layers.push_back(image);

    const renderer::Frame& frame =
        rendered_with_image(project, 0, "white", make_uniform_texture(2, 2, 255));

    int red = 0, green = 0, blue = 0;
    pixel(frame, 4, 4, red, green, blue);  // interior texel, unchanged
    CHECK(red == 255);
    pixel(frame, 3, 4, red, green, blue);  // left edge, about half covered
    CHECK(red > 100);
    CHECK(red < 160);
    pixel(frame, 2, 4, red, green, blue);  // fully outside
    CHECK(red == 0);
}

TEST_CASE("anti-aliasing: non-finite and huge geometry never crashes", "[renderer][aa]") {
    SECTION("a NaN width draws nothing") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = std::numeric_limits<double>::quiet_NaN();
        rect.height = 4.0;
        rect.transform.x.constant = 4.0;
        rect.transform.y.constant = 4.0;
        project.layers.push_back(rect);

        const renderer::Frame& frame = rendered(project, 0);
        int red = 0, green = 0, blue = 0;
        pixel(frame, 4, 4, red, green, blue);
        CHECK(red == 0);
    }

    SECTION("a NaN scale draws nothing but does not crash") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = 4.0;
        rect.height = 4.0;
        rect.transform.x.constant = 4.0;
        rect.transform.y.constant = 4.0;
        rect.transform.scale_x.constant = std::numeric_limits<double>::quiet_NaN();
        project.layers.push_back(rect);

        const renderer::Frame& frame = rendered(project, 0);
        int red = 0, green = 0, blue = 0;
        pixel(frame, 4, 4, red, green, blue);
        CHECK(red == 0);
    }

    SECTION("a NaN rotation does not crash") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = 4.0;
        rect.height = 4.0;
        rect.transform.x.constant = 4.0;
        rect.transform.y.constant = 4.0;
        rect.transform.rotation.constant = std::numeric_limits<double>::quiet_NaN();
        project.layers.push_back(rect);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::Frame>(result));
        REQUIRE(std::get<renderer::Frame>(result).rgba.size() == 8u * 8u * 4u);
    }

    SECTION("a huge rect is clipped to the canvas") {
        Project project = make_project(8, 8, 30.0, 1000);
        Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
        rect.color = "#ffffff";
        rect.width = 1.0e9;
        rect.height = 1.0e9;
        rect.transform.x.constant = 4.0;
        rect.transform.y.constant = 4.0;
        project.layers.push_back(rect);

        const renderer::Frame& frame = rendered(project, 0);
        REQUIRE(frame.rgba.size() == 8u * 8u * 4u);
        int red = 0, green = 0, blue = 0;
        pixel(frame, 4, 4, red, green, blue);
        CHECK(red == 255);  // the canvas is fully covered
    }
}

TEST_CASE("blend modes combine a layer with the canvas below", "[renderer][blend]") {
    // A mid-gray (#808080) backdrop, then a same-colour layer on top with a
    // blend mode. With full coverage and opacity the result is exactly
    // blend_channel(mode, 128/255, 128/255) * 255.
    Project project = make_project(4, 4, 30.0, 1000);
    Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
    backdrop.color = "#808080";
    project.layers.push_back(backdrop);

    Layer top = make_layer("top", LayerType::Solid, 0, 1000);
    top.color = "#808080";
    top.blend = project::BlendMode::Multiply;
    project.layers.push_back(top);

    int red = 0, green = 0, blue = 0;
    pixel(rendered(project, 0), 2, 2, red, green, blue);
    CHECK(red == Catch::Approx(64).margin(2));
    CHECK(green == Catch::Approx(64).margin(2));
    CHECK(blue == Catch::Approx(64).margin(2));

    project.layers[1].blend = project::BlendMode::Screen;
    pixel(rendered(project, 0), 2, 2, red, green, blue);
    CHECK(red == Catch::Approx(192).margin(2));
    CHECK(green == Catch::Approx(192).margin(2));
    CHECK(blue == Catch::Approx(192).margin(2));
}

TEST_CASE("blend modes with opacity 0.5 interpolate toward the backdrop", "[renderer][blend]") {
    Project project = make_project(4, 4, 30.0, 1000);
    Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
    backdrop.color = "#808080";
    project.layers.push_back(backdrop);

    Layer top = make_layer("top", LayerType::Solid, 0, 1000);
    top.color = "#808080";
    top.blend = project::BlendMode::Multiply;
    top.transform.opacity.constant = 0.5;
    project.layers.push_back(top);

    // result = 0.5 * backdrop + 0.5 * multiply = 0.5 * 128 + 0.5 * 64 = 96.
    int red = 0, green = 0, blue = 0;
    pixel(rendered(project, 0), 2, 2, red, green, blue);
    CHECK(red == Catch::Approx(96).margin(2));
    CHECK(green == Catch::Approx(96).margin(2));
    CHECK(blue == Catch::Approx(96).margin(2));
}

TEST_CASE("a blend mode interpolates across an anti-aliased edge", "[renderer][blend]") {
    Project project = make_project(24, 24, 30.0, 1000);
    Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
    backdrop.color = "#808080";
    project.layers.push_back(backdrop);

    Layer rect = make_layer("box", LayerType::Rect, 0, 1000);
    rect.color = "#808080";
    rect.blend = project::BlendMode::Multiply;
    rect.width = 4.0;  // left edge at 12.5 - 2.0 = 10.5
    rect.height = 10.0;
    rect.transform.x.constant = 12.5;
    rect.transform.y.constant = 12.0;
    project.layers.push_back(rect);

    const renderer::Frame& frame = rendered(project, 0);
    int red = 0, green = 0, blue = 0;

    pixel(frame, 12, 12, red, green, blue);  // fully covered -> multiply 64
    CHECK(red == Catch::Approx(64).margin(2));

    pixel(frame, 10, 12, red, green, blue);  // half covered -> 0.5 * 128 + 0.5 * 64 = 96
    CHECK(red == Catch::Approx(96).margin(2));

    pixel(frame, 9, 12, red, green, blue);  // outside the rect -> untouched backdrop
    CHECK(red == Catch::Approx(128).margin(2));
}

TEST_CASE("layer order decides what a blend mode sees", "[renderer][blend]") {
    // Top is drawn after bottom, so only the second layer blends.
    Project project = make_project(4, 4, 30.0, 1000);
    Layer first = make_layer("a", LayerType::Solid, 0, 1000);
    first.color = "#808080";
    first.blend = project::BlendMode::Multiply;  // blends with the black canvas -> black
    project.layers.push_back(first);

    Layer second = make_layer("b", LayerType::Solid, 0, 1000);
    second.color = "#808080";
    second.blend = project::BlendMode::Screen;  // blends with black -> #808080
    project.layers.push_back(second);

    int red = 0, green = 0, blue = 0;
    pixel(rendered(project, 0), 2, 2, red, green, blue);
    // multiply over black = 0, then screen over black = 0 + 128 - 0 = 128.
    CHECK(red == Catch::Approx(128).margin(2));
    CHECK(green == Catch::Approx(128).margin(2));
    CHECK(blue == Catch::Approx(128).margin(2));

    // Swapping the order changes the result: screen over black = 128, then
    // multiply over 128 = 128 * 128 / 255 ~= 64.
    std::swap(project.layers[0], project.layers[1]);
    pixel(rendered(project, 0), 2, 2, red, green, blue);
    CHECK(red == Catch::Approx(64).margin(2));
}

TEST_CASE("a blend mode applies to an image layer", "[renderer][blend]") {
    Project project = make_project(8, 8, 30.0, 1000);
    Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
    backdrop.color = "#808080";
    project.layers.push_back(backdrop);

    Layer image = make_layer("img", LayerType::Image, 0, 1000);
    image.source = "gray";
    image.blend = project::BlendMode::Multiply;
    image.width = 8.0;   // explicit size keeps the sampled pixel interior
    image.height = 8.0;
    image.transform.x.constant = 4.0;
    image.transform.y.constant = 4.0;
    project.layers.push_back(image);

    // A uniform #808080 texture: multiply over the #808080 canvas -> ~64.
    const std::vector<uint8_t> gray = {128, 128, 128, 255};
    const renderer::Frame& frame =
        rendered_with_image(project, 0, "gray", make_texture(1, 1, gray));

    int red = 0, green = 0, blue = 0;
    pixel(frame, 4, 4, red, green, blue);
    CHECK(red == Catch::Approx(64).margin(2));
    CHECK(green == Catch::Approx(64).margin(2));
    CHECK(blue == Catch::Approx(64).margin(2));
}

TEST_CASE("a Normal-mode frame is byte-identical with the blend field omitted", "[renderer][blend]") {
    // The same two-layer project, once with "blend" omitted from every layer
    // and once with it written explicitly as "normal". The frames must match
    // byte for byte, which also pins Normal to the pre-blend output.
    const std::string without = R"({
      "version": 1, "width": 24, "height": 18, "fps": 30.0, "duration_ms": 1000,
      "layers": [
        { "id": "bg", "type": "solid", "start_ms": 0, "end_ms": 1000, "color": "#203040" },
        { "id": "box", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "color": "#e04030", "width": 10.0, "height": 8.0,
          "transform": { "x": 11.5, "y": 9.0, "opacity": 0.6 } }
      ]
    })";
    const std::string with_normal = R"({
      "version": 1, "width": 24, "height": 18, "fps": 30.0, "duration_ms": 1000,
      "layers": [
        { "id": "bg", "type": "solid", "start_ms": 0, "end_ms": 1000, "color": "#203040",
          "blend": "normal" },
        { "id": "box", "type": "rect", "start_ms": 0, "end_ms": 1000,
          "color": "#e04030", "width": 10.0, "height": 8.0, "blend": "normal",
          "transform": { "x": 11.5, "y": 9.0, "opacity": 0.6 } }
      ]
    })";

    const std::variant<Project, project::ProjectError> a = project::load_from_string(without);
    const std::variant<Project, project::ProjectError> b = project::load_from_string(with_normal);
    REQUIRE(std::holds_alternative<Project>(a));
    REQUIRE(std::holds_alternative<Project>(b));

    const renderer::Frame& frame_a = rendered(std::get<Project>(a), 0);
    const renderer::Frame& frame_b = rendered(std::get<Project>(b), 0);
    CHECK(frame_a.rgba == frame_b.rgba);
}

// Sums the red channel over the whole frame, in units of full-white pixels.
// With a white shape on a black canvas this is the shape's covered area.
double covered_area(const renderer::Frame& frame) {
    double total = 0.0;
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        total += static_cast<double>(frame.rgba[i]) / 255.0;
    }
    return total;
}

TEST_CASE("a circle fills its centre, leaves its box corner, and covers pi r^2", "[renderer]") {
    Project project = make_project(160, 160, 30.0, 1000);
    Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
    circle.color = "#ffffff";
    circle.width = 100.0;  // radius 50
    circle.height = 100.0;
    circle.transform.x.constant = 80.0;
    circle.transform.y.constant = 80.0;
    project.layers.push_back(circle);

    const renderer::Frame& frame = rendered(project, 0);
    int red = 0, green = 0, blue = 0;

    pixel(frame, 80, 80, red, green, blue);
    CHECK(red == 255);  // centre is fully inside

    // The bounding-box corner (30,30) is far outside the inscribed circle.
    pixel(frame, 30, 30, red, green, blue);
    CHECK(red == 0);

    const double area = covered_area(frame);
    const double expected = 3.14159265358979323846 * 50.0 * 50.0;
    CHECK(area == Catch::Approx(expected).epsilon(0.01));
}

TEST_CASE("an ellipse matches its width x height box", "[renderer]") {
    Project project = make_project(160, 160, 30.0, 1000);
    Layer ellipse = make_layer("e", LayerType::Circle, 0, 1000);
    ellipse.color = "#ffffff";
    ellipse.width = 80.0;  // hx = 40
    ellipse.height = 40.0; // hy = 20
    ellipse.transform.x.constant = 80.0;
    ellipse.transform.y.constant = 80.0;
    project.layers.push_back(ellipse);

    const renderer::Frame& frame = rendered(project, 0);
    int red = 0, green = 0, blue = 0;

    pixel(frame, 80, 80, red, green, blue);
    CHECK(red == 255);

    pixel(frame, 118, 80, red, green, blue);  // local_x = 38.5, well inside
    CHECK(red == 255);
    pixel(frame, 121, 80, red, green, blue);  // local_x = 41.5 > 40
    CHECK(red == 0);

    pixel(frame, 80, 98, red, green, blue);   // local_y = 18.5, well inside
    CHECK(red == 255);
    pixel(frame, 80, 101, red, green, blue);  // local_y = 21.5 > 20
    CHECK(red == 0);
}

TEST_CASE("a round_rect with radius 0 matches a rect", "[renderer]") {
    auto build = [](LayerType type) {
        Project project = make_project(80, 80, 30.0, 1000);
        Layer layer = make_layer("s", type, 0, 1000);
        layer.color = "#3a7bd5";
        layer.width = 20.0;
        layer.height = 10.0;
        layer.radius = 0.0;
        layer.transform.x.constant = 40.0;  // pixel-aligned edges at 30/50 and 35/45
        layer.transform.y.constant = 40.0;
        project.layers.push_back(layer);
        return project;
    };

    const renderer::Frame& rect = rendered(build(LayerType::Rect), 0);
    const renderer::Frame& round = rendered(build(LayerType::RoundRect), 0);

    REQUIRE(rect.rgba.size() == round.rgba.size());
    for (std::size_t i = 0; i < rect.rgba.size(); ++i) {
        CHECK(std::abs(static_cast<int>(rect.rgba[i]) - static_cast<int>(round.rgba[i])) <= 1);
    }
}

TEST_CASE("a round_rect with a half-pixel offset matches a rect away from the corners",
          "[renderer]") {
    auto build = [](LayerType type) {
        Project project = make_project(80, 80, 30.0, 1000);
        Layer layer = make_layer("s", type, 0, 1000);
        layer.color = "#ffffff";
        layer.width = 20.0;
        layer.height = 20.0;
        layer.radius = 0.0;
        layer.transform.x.constant = 40.5;  // edges fall on pixel centres
        layer.transform.y.constant = 40.5;
        project.layers.push_back(layer);
        return project;
    };

    const renderer::Frame& rect = rendered(build(LayerType::Rect), 0);
    const renderer::Frame& round = rendered(build(LayerType::RoundRect), 0);

    // Only the corner pixels can differ once the edges are misaligned, so skip
    // any pixel within 1.5 px of one of the four corners.
    const double hx = 10.0, hy = 10.0;
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 80; ++x) {
            const double lx = (static_cast<double>(x) + 0.5) - 40.5;
            const double ly = (static_cast<double>(y) + 0.5) - 40.5;
            double nearest = std::numeric_limits<double>::infinity();
            for (double cx : {-hx, hx}) {
                for (double cy : {-hy, hy}) {
                    nearest = std::min(nearest, std::hypot(lx - cx, ly - cy));
                }
            }
            if (nearest <= 1.5) {
                continue;
            }
            const std::size_t index =
                (static_cast<std::size_t>(y) * 80u + static_cast<std::size_t>(x)) * 4;
            CHECK(std::abs(static_cast<int>(rect.rgba[index]) -
                           static_cast<int>(round.rgba[index])) <= 1);
        }
    }
}

TEST_CASE("a round_rect with a real radius rounds its corners", "[renderer]") {
    auto build = [](LayerType type) {
        Project project = make_project(80, 80, 30.0, 1000);
        Layer layer = make_layer("s", type, 0, 1000);
        layer.color = "#ffffff";
        layer.width = 40.0;
        layer.height = 40.0;
        layer.radius = 20.0;
        layer.transform.x.constant = 40.0;
        layer.transform.y.constant = 40.0;
        project.layers.push_back(layer);
        return project;
    };

    const renderer::Frame& rect = rendered(build(LayerType::Rect), 0);
    const renderer::Frame& round = rendered(build(LayerType::RoundRect), 0);
    int red = 0, green = 0, blue = 0;

    // The box corner is filled by a rect but empty for the rounded one.
    pixel(rect, 20, 20, red, green, blue);
    CHECK(red == 255);
    pixel(round, 20, 20, red, green, blue);
    CHECK(red == 0);

    // The edge midpoints stay filled.
    pixel(round, 40, 20, red, green, blue);
    CHECK(red > 200);
    pixel(round, 20, 40, red, green, blue);
    CHECK(red > 200);
}

TEST_CASE("a huge round_rect radius clamps to a pill", "[renderer]") {
    Project project = make_project(80, 80, 30.0, 1000);
    Layer pill = make_layer("p", LayerType::RoundRect, 0, 1000);
    pill.color = "#ffffff";
    pill.width = 20.0;
    pill.height = 20.0;
    pill.radius = 1.0e9;
    pill.transform.x.constant = 40.0;
    pill.transform.y.constant = 40.0;
    project.layers.push_back(pill);

    const renderer::Frame& frame = rendered(project, 0);
    int red = 0, green = 0, blue = 0;
    pixel(frame, 40, 40, red, green, blue);
    CHECK(red == 255);
    pixel(frame, 20, 20, red, green, blue);  // clamped corner is empty
    CHECK(red == 0);
}

TEST_CASE("circle and round_rect scale, rotate and fade", "[renderer]") {
    SECTION("scale 2 doubles the circle's extent") {
        auto build = [](double scale) {
            Project project = make_project(80, 80, 30.0, 1000);
            Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
            circle.color = "#ffffff";
            circle.width = 10.0;  // radius 5
            circle.height = 10.0;
            circle.transform.x.constant = 40.0;
            circle.transform.y.constant = 40.0;
            circle.transform.scale_x.constant = scale;
            circle.transform.scale_y.constant = scale;
            project.layers.push_back(circle);
            return project;
        };
        int red = 0, green = 0, blue = 0;
        pixel(rendered(build(1.0), 0), 47, 40, red, green, blue);  // 7.5 px out: outside
        CHECK(red == 0);
        pixel(rendered(build(2.0), 0), 47, 40, red, green, blue);  // inside the scaled circle
        CHECK(red == 255);
    }

    SECTION("a 90-degree rotation swaps a non-square round_rect's extents") {
        Project project = make_project(80, 80, 30.0, 1000);
        Layer bar = make_layer("b", LayerType::RoundRect, 0, 1000);
        bar.color = "#ffffff";
        bar.width = 40.0;  // hx = 20
        bar.height = 10.0; // hy = 5
        bar.radius = 0.0;
        bar.transform.x.constant = 40.0;
        bar.transform.y.constant = 40.0;
        bar.transform.rotation.constant = 90.0;
        project.layers.push_back(bar);

        int red = 0, green = 0, blue = 0;
        const renderer::Frame& frame = rendered(project, 0);
        pixel(frame, 40, 55, red, green, blue);  // now along the long axis
        CHECK(red == 255);
        pixel(frame, 55, 40, red, green, blue);  // now along the short axis
        CHECK(red == 0);
    }

    SECTION("opacity 0.5 halves the blended value") {
        Project project = make_project(40, 40, 30.0, 1000);
        Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
        backdrop.color = "#808080";
        project.layers.push_back(backdrop);

        Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
        circle.color = "#808080";
        circle.blend = project::BlendMode::Multiply;
        circle.width = 40.0;
        circle.height = 40.0;
        circle.transform.x.constant = 20.0;
        circle.transform.y.constant = 20.0;
        circle.transform.opacity.constant = 0.5;
        project.layers.push_back(circle);

        // 0.5 * 128 + 0.5 * (multiply 128,128 = 64) = 96.
        int red = 0, green = 0, blue = 0;
        pixel(rendered(project, 0), 20, 20, red, green, blue);
        CHECK(red == Catch::Approx(96).margin(2));
    }
}

TEST_CASE("blend modes apply to circle and round_rect layers", "[renderer][blend]") {
    SECTION("a circle blends at its centre") {
        Project project = make_project(40, 40, 30.0, 1000);
        Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
        backdrop.color = "#808080";
        project.layers.push_back(backdrop);

        Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
        circle.color = "#808080";
        circle.width = 30.0;
        circle.height = 30.0;
        circle.transform.x.constant = 20.0;
        circle.transform.y.constant = 20.0;
        circle.blend = project::BlendMode::Multiply;
        project.layers.push_back(circle);

        int red = 0, green = 0, blue = 0;
        pixel(rendered(project, 0), 20, 20, red, green, blue);
        CHECK(red == Catch::Approx(64).margin(2));

        project.layers[1].blend = project::BlendMode::Screen;
        pixel(rendered(project, 0), 20, 20, red, green, blue);
        CHECK(red == Catch::Approx(192).margin(2));
    }

    SECTION("a round_rect edge pixel interpolates between backdrop and blend") {
        Project project = make_project(80, 80, 30.0, 1000);
        Layer backdrop = make_layer("bg", LayerType::Solid, 0, 1000);
        backdrop.color = "#808080";
        project.layers.push_back(backdrop);

        Layer box = make_layer("r", LayerType::RoundRect, 0, 1000);
        box.color = "#808080";
        box.blend = project::BlendMode::Multiply;
        box.width = 40.0;
        box.height = 40.0;
        box.radius = 8.0;
        box.transform.x.constant = 40.5;  // left edge lands on a pixel centre
        box.transform.y.constant = 40.5;
        project.layers.push_back(box);

        int red = 0, green = 0, blue = 0;
        const renderer::Frame& frame = rendered(project, 0);
        pixel(frame, 40, 40, red, green, blue);  // fully covered -> multiply 64
        CHECK(red == Catch::Approx(64).margin(2));

        pixel(frame, 20, 40, red, green, blue);  // half covered -> ~96
        CHECK(red > 64);
        CHECK(red < 128);
        CHECK(red == Catch::Approx(96).margin(4));
    }
}

TEST_CASE("non-finite circle and round_rect geometry never crashes", "[renderer]") {
    SECTION("NaN width on a circle") {
        Project project = make_project(16, 16, 30.0, 1000);
        Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
        circle.color = "#ffffff";
        circle.width = std::numeric_limits<double>::quiet_NaN();
        circle.height = 8.0;
        circle.transform.x.constant = 8.0;
        circle.transform.y.constant = 8.0;
        project.layers.push_back(circle);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::Frame>(result));
        REQUIRE(std::get<renderer::Frame>(result).rgba.size() == 16u * 16u * 4u);
    }

    SECTION("infinite radius on a round_rect") {
        Project project = make_project(16, 16, 30.0, 1000);
        Layer box = make_layer("r", LayerType::RoundRect, 0, 1000);
        box.color = "#ffffff";
        box.width = 8.0;
        box.height = 8.0;
        box.radius = std::numeric_limits<double>::infinity();
        box.transform.x.constant = 8.0;
        box.transform.y.constant = 8.0;
        project.layers.push_back(box);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::Frame>(result));
        REQUIRE(std::get<renderer::Frame>(result).rgba.size() == 16u * 16u * 4u);
    }

    SECTION("NaN transform on a circle") {
        Project project = make_project(16, 16, 30.0, 1000);
        Layer circle = make_layer("c", LayerType::Circle, 0, 1000);
        circle.color = "#ffffff";
        circle.width = 8.0;
        circle.height = 8.0;
        circle.transform.x.constant = std::numeric_limits<double>::quiet_NaN();
        circle.transform.rotation.constant = std::numeric_limits<double>::infinity();
        project.layers.push_back(circle);

        const std::variant<renderer::Frame, renderer::RenderError> result =
            renderer::render_frame(project, 0);
        REQUIRE(std::holds_alternative<renderer::Frame>(result));
    }
}
