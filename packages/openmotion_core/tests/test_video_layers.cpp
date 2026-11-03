// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

#include "../src/internal/shell_quote.hpp"
#include "../src/internal/video_stream.hpp"
#include "openmotion/project.hpp"

namespace {

namespace project = openmotion::project;
namespace renderer = openmotion::renderer;
namespace internal = openmotion::internal;

using project::Layer;
using project::LayerType;
using project::Project;

constexpr int kClipWidth = 64;
constexpr int kClipHeight = 64;
constexpr double kClipFps = 30.0;
constexpr int64_t kClipDurationMs = 1000;  // 30 frames: 15 red, then 15 green

Project make_project(int width, int height, double fps, int64_t duration_ms) {
    Project project;
    project.version = 1;
    project.name = "video-test";
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

// A project whose single video layer is centred and drawn at native size, so
// the video fills the canvas and the centre pixel is an exact texel hit. The
// project spans exactly the layer, so its duration is end_ms - start_ms.
Project make_video_project(const std::string& source, int64_t start_ms, int64_t end_ms) {
    Project project = make_project(kClipWidth, kClipHeight, kClipFps, end_ms - start_ms);
    Layer layer = make_layer("clip", LayerType::Video, start_ms, end_ms);
    layer.source = source;
    layer.transform.x.constant = static_cast<double>(kClipWidth) / 2.0;
    layer.transform.y.constant = static_cast<double>(kClipHeight) / 2.0;
    project.layers.push_back(layer);
    return project;
}

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

std::filesystem::path temp_path(const std::string& stem, const std::string& extension) {
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(::getpid()) + extension);
}

// Generates a 64x64, 30 fps, 1-second clip: 15 red frames then 15 green frames.
// "lime" is used for the second half because ffmpeg's "green" is the dark
// #008000, which leaves little margin against the black canvas.
std::filesystem::path make_test_clip() {
    const std::filesystem::path path = temp_path("openmotion_clip", ".mp4");
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i \"color=c=red:s=" + std::to_string(kClipWidth) + "x" +
        std::to_string(kClipHeight) + ":r=" + std::to_string(static_cast<int>(kClipFps)) +
        ":d=0.5\" -f lavfi -i \"color=c=lime:s=" + std::to_string(kClipWidth) + "x" +
        std::to_string(kClipHeight) + ":r=" + std::to_string(static_cast<int>(kClipFps)) +
        ":d=0.5\" -filter_complex \"[0:v][1:v]concat=n=2:v=1:a=0[v]\" -map \"[v]\" "
        "-pix_fmt yuv420p -r " +
        std::to_string(static_cast<int>(kClipFps)) + " " +
        internal::shell_quote(path.string());
    REQUIRE(std::system(command.c_str()) == 0);
    return path;
}

renderer::Frame render_with(renderer::RenderCache& cache, const Project& project, int64_t t_ms) {
    const std::variant<renderer::Frame, renderer::RenderError> result =
        renderer::render_frame(project, t_ms, cache);
    REQUIRE(std::holds_alternative<renderer::Frame>(result));
    return std::get<renderer::Frame>(result);
}

void check_red(const renderer::Frame& frame) {
    int red = 0, green = 0, blue = 0;
    pixel(frame, frame.width / 2, frame.height / 2, red, green, blue);
    CHECK(red > 150);
    CHECK(green < 100);
    CHECK(blue < 100);
}

void check_green(const renderer::Frame& frame) {
    int red = 0, green = 0, blue = 0;
    pixel(frame, frame.width / 2, frame.height / 2, red, green, blue);
    CHECK(green > 150);
    CHECK(red < 100);
    CHECK(blue < 100);
}

void check_black(const renderer::Frame& frame) {
    int red = 0, green = 0, blue = 0;
    pixel(frame, frame.width / 2, frame.height / 2, red, green, blue);
    CHECK(red < 40);
    CHECK(green < 40);
    CHECK(blue < 40);
}

}  // namespace

TEST_CASE("plan_advance covers every advance rule", "[video]") {
    // No current frame yet: always a restart.
    CHECK(internal::plan_advance(-1, 0).action == internal::Advance::Restart);
    CHECK(internal::plan_advance(-1, 42).action == internal::Advance::Restart);

    // Same index reuses the held frame.
    CHECK(internal::plan_advance(7, 7).action == internal::Advance::Reuse);

    // The very next index reads one frame.
    CHECK(internal::plan_advance(7, 8).action == internal::Advance::ReadNext);

    // A small forward jump skips forward, reading wanted - current - 1 frames.
    const internal::AdvancePlan skip = internal::plan_advance(10, 15);
    CHECK(skip.action == internal::Advance::SkipForward);
    CHECK(skip.frames_to_skip == 4);

    // Boundary: +30 skips, +31 restarts.
    const internal::AdvancePlan at_limit = internal::plan_advance(10, 40);
    CHECK(at_limit.action == internal::Advance::SkipForward);
    CHECK(at_limit.frames_to_skip == 29);
    CHECK(internal::plan_advance(10, 41).action == internal::Advance::Restart);

    // Backwards is always a restart.
    CHECK(internal::plan_advance(40, 39).action == internal::Advance::Restart);
    CHECK(internal::plan_advance(40, 0).action == internal::Advance::Restart);
}

TEST_CASE("video_frame_index maps timeline time to source frames", "[video]") {
    // 30 fps: one frame every 33.33 ms.
    CHECK(internal::video_frame_index(0, 0, 30.0) == 0);
    CHECK(internal::video_frame_index(0, 33, 30.0) == 1);
    CHECK(internal::video_frame_index(0, 66, 30.0) == 2);
    CHECK(internal::video_frame_index(0, 1000, 30.0) == 30);

    // 29.97 fps: 1001/30000 second per frame; 1000 ms is ~29.97 -> 30.
    CHECK(internal::video_frame_index(0, 0, 29.97) == 0);
    CHECK(internal::video_frame_index(0, 1001, 29.97) == 30);
    CHECK(internal::video_frame_index(0, 500, 29.97) == 15);  // 14.985 -> 15

    // 24 fps: one frame every 41.67 ms.
    CHECK(internal::video_frame_index(0, 0, 24.0) == 0);
    CHECK(internal::video_frame_index(0, 42, 24.0) == 1);
    CHECK(internal::video_frame_index(0, 1000, 24.0) == 24);

    // The clip clock is relative to the layer start.
    CHECK(internal::video_frame_index(1000, 1000, 30.0) == 0);
    CHECK(internal::video_frame_index(1000, 1700, 30.0) == 21);
    CHECK(internal::video_frame_index(1000, 500, 30.0) == 0);  // before start
}

TEST_CASE("a video layer plays red then green", "[video]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path clip = make_test_clip();
    const Project project = make_video_project(clip.string(), 0, 2000);

    renderer::RenderCache cache;
    check_red(render_with(cache, project, 0));
    check_red(render_with(cache, project, 400));   // frame 12, still red
    check_green(render_with(cache, project, 700));  // frame 21, green

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);
}

TEST_CASE("sequential and reverse access agree", "[video]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path clip = make_test_clip();
    const Project project = make_video_project(clip.string(), 0, 2000);

    std::vector<int64_t> times;
    for (int64_t t = 0; t <= 990; t += 33) {
        times.push_back(t);
    }

    std::vector<std::vector<int>> forward;
    {
        renderer::RenderCache cache;
        for (int64_t t : times) {
            const renderer::Frame frame = render_with(cache, project, t);
            int red = 0, green = 0, blue = 0;
            pixel(frame, frame.width / 2, frame.height / 2, red, green, blue);
            forward.push_back({red, green, blue});
        }
    }

    std::vector<std::vector<int>> reverse(times.size());
    {
        renderer::RenderCache cache;
        for (std::size_t i = times.size(); i-- > 0;) {
            const renderer::Frame frame = render_with(cache, project, times[i]);
            int red = 0, green = 0, blue = 0;
            pixel(frame, frame.width / 2, frame.height / 2, red, green, blue);
            reverse[i] = {red, green, blue};
        }
    }

    REQUIRE(forward.size() == reverse.size());
    for (std::size_t i = 0; i < forward.size(); ++i) {
        CHECK(forward[i] == reverse[i]);
    }

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);
}

TEST_CASE("a delayed video layer stays hidden until its start", "[video]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path clip = make_test_clip();
    const Project project = make_video_project(clip.string(), 1000, 3000);

    renderer::RenderCache cache;
    check_black(render_with(cache, project, 500));  // not active yet
    check_black(render_with(cache, project, 999));
    check_red(render_with(cache, project, 1000));  // first frame

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);
}

TEST_CASE("past the clip end the last frame is held", "[video]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path clip = make_test_clip();
    const Project project = make_video_project(clip.string(), 0, 10000);

    renderer::RenderCache cache;
    check_green(render_with(cache, project, 5000));  // way past 1s
    check_green(render_with(cache, project, 9000));  // still holding
    check_red(render_with(cache, project, 100));     // seeking back replays the clip

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);
}

TEST_CASE("a missing video file is reported with its path", "[video]") {
    const std::string missing = "/nonexistent/openmotion/definitely_absent.mp4";
    const Project project = make_video_project(missing, 0, 1000);

    const std::variant<renderer::Frame, renderer::RenderError> result =
        renderer::render_frame(project, 0);
    REQUIRE(std::holds_alternative<renderer::RenderError>(result));
    CHECK(std::get<renderer::RenderError>(result).message.find(missing) != std::string::npos);
}

TEST_CASE("render_to_mp4 matches the project duration with a video layer", "[video]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path clip = make_test_clip();
    const Project project = make_video_project(clip.string(), 0, 1000);

    const std::filesystem::path output = temp_path("openmotion_video_out", ".mp4");
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string());
    REQUIRE_FALSE(result.has_value());

    const std::string probe = "ffprobe -v error -show_entries format=duration "
                              "-of default=noprint_wrappers=1:nokey=1 " +
                              internal::shell_quote(output.string());
    const std::string text = capture_command(probe);
    REQUIRE_FALSE(text.empty());

    const double duration = std::stod(text);
    CHECK(duration == Catch::Approx(1.0).margin(0.2));

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);
    std::filesystem::remove(output, ignored);
}
