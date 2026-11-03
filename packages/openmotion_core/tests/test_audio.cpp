// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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

#include "../src/internal/audio_mix.hpp"
#include "../src/internal/shell_quote.hpp"
#include "openmotion/project.hpp"

namespace {

namespace project = openmotion::project;
namespace renderer = openmotion::renderer;
namespace internal = openmotion::internal;

using project::Layer;
using project::LayerType;
using project::Project;

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr double kFps = 30.0;
constexpr int64_t kDurationMs = 1000;

Project make_project(int width, int height, double fps, int64_t duration_ms) {
    Project project;
    project.version = 1;
    project.name = "audio-test";
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

bool command_available(const std::string& name) {
    const std::string command = "command -v " + name + " > /dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

// Captures a command's stdout as a binary-safe string (may contain NUL bytes).
std::string capture_binary(const std::string& command) {
    std::string output;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return output;
    }
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe.get())) != 0) {
        output.append(buffer, read);
    }
    return output;
}

std::string capture_text(const std::string& command) {
    return capture_binary(command);
}

std::filesystem::path temp_path(const std::string& stem, const std::string& extension) {
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(::getpid()) + extension);
}

std::filesystem::path make_sine_wav(double seconds) {
    const std::filesystem::path path = temp_path("openmotion_sine", ".wav");
    // The default sine is quiet (peak ~1/8 full scale); volume=8 lifts it near
    // full scale so silence (near 0) and signal are far apart in the assertions.
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i \"sine=frequency=440:duration=" + std::to_string(seconds) +
        "\" -af volume=8 -c:a pcm_s16le " + internal::shell_quote(path.string());
    REQUIRE(std::system(command.c_str()) == 0);
    return path;
}

std::filesystem::path make_video_only_mp4() {
    const std::filesystem::path path = temp_path("openmotion_videoonly", ".mp4");
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i \"testsrc=size=32x32:rate=10:duration=1\" "
        "-pix_fmt yuv420p -an " +
        internal::shell_quote(path.string());
    REQUIRE(std::system(command.c_str()) == 0);
    return path;
}

// True when the container reports at least one stream of the given type.
bool has_stream_of_type(const std::string& path, const std::string& type) {
    const std::string command =
        "ffprobe -v error -show_entries stream=codec_type -of csv=p=0 " +
        internal::shell_quote(path);
    const std::string text = capture_text(command);
    return text.find(type) != std::string::npos;
}

double probe_duration_seconds(const std::string& path) {
    const std::string command = "ffprobe -v error -show_entries format=duration "
                                "-of default=noprint_wrappers=1:nokey=1 " +
                                internal::shell_quote(path);
    const std::string text = capture_text(command);
    REQUIRE_FALSE(text.empty());
    return std::stod(text);
}

// Peak absolute sample in the [from_ms, to_ms) window of a s16le mono PCM blob.
int peak_in_window(const std::string& pcm, int sample_rate, int from_ms, int to_ms) {
    const std::size_t first = static_cast<std::size_t>(from_ms) * sample_rate / 1000;
    const std::size_t last = static_cast<std::size_t>(to_ms) * sample_rate / 1000;
    const std::size_t sample_count = pcm.size() / 2;
    int peak = 0;
    for (std::size_t i = first; i < last && i < sample_count; ++i) {
        const int16_t sample =
            static_cast<int16_t>(static_cast<uint8_t>(pcm[i * 2]) |
                                 (static_cast<uint8_t>(pcm[i * 2 + 1]) << 8));
        const int magnitude = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

std::string decode_pcm(const std::string& path, int sample_rate) {
    const std::string command = "ffmpeg -v error -i " + internal::shell_quote(path) +
                                " -f s16le -ac 1 -ar " + std::to_string(sample_rate) + " -";
    return capture_binary(command);
}

renderer::RenderCache make_cache() {
    return renderer::RenderCache{};
}

}  // namespace

TEST_CASE("build_audio_filter_graph renders the expected graph", "[audio]") {
    SECTION("one input") {
        const std::vector<internal::AudioInput> inputs = {
            internal::AudioInput{"a.wav", 500, 1000, 0, 1.0}};
        CHECK(internal::build_audio_filter_graph(inputs) ==
              "[1:a]adelay=500|500,volume=1.000000[a0];"
              "[a0]amix=inputs=1:normalize=0:duration=longest[aout]");
    }

    SECTION("two inputs, distinct delay and volume") {
        const std::vector<internal::AudioInput> inputs = {
            internal::AudioInput{"a.wav", 0, 1000, 0, 1.0},
            internal::AudioInput{"b.wav", 250, 1000, 0, 0.5}};
        CHECK(internal::build_audio_filter_graph(inputs) ==
              "[1:a]adelay=0|0,volume=1.000000[a0];"
              "[2:a]adelay=250|250,volume=0.500000[a1];"
              "[a0][a1]amix=inputs=2:normalize=0:duration=longest[aout]");
    }

    SECTION("no inputs yields an empty graph") {
        CHECK(internal::build_audio_filter_graph({}).empty());
    }
}

TEST_CASE("render_to_mp4 mixes a delayed audio layer", "[audio]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path sine = make_sine_wav(2.0);
    const std::filesystem::path output = temp_path("openmotion_audio_out", ".mp4");

    Project project = make_project(kWidth, kHeight, kFps, kDurationMs);
    project.layers.push_back(make_layer("bg", LayerType::Solid, 0, kDurationMs));
    project.layers.back().color = "#202020";
    Layer audio = make_layer("clip", LayerType::Audio, 500, 1000);
    audio.source = sine.string();
    audio.in_ms = 0;
    audio.volume = 1.0;
    project.layers.push_back(audio);

    renderer::RenderCache cache = make_cache();
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string(), cache);
    REQUIRE_FALSE(result.has_value());

    CHECK(has_stream_of_type(output.string(), "audio"));
    CHECK(has_stream_of_type(output.string(), "video"));
    CHECK(probe_duration_seconds(output.string()) == Catch::Approx(1.0).margin(0.2));

    const std::string pcm = decode_pcm(output.string(), 8000);
    REQUIRE_FALSE(pcm.empty());
    const int silence_peak = peak_in_window(pcm, 8000, 0, 400);
    const int energy_peak = peak_in_window(pcm, 8000, 600, 900);
    CHECK(silence_peak < 1000);
    CHECK(energy_peak > 10000);

    std::error_code ignored;
    std::filesystem::remove(sine, ignored);
    std::filesystem::remove(output, ignored);
}

TEST_CASE("volume 0.0 yields silence for the whole clip", "[audio]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path sine = make_sine_wav(2.0);
    const std::filesystem::path output = temp_path("openmotion_audio_mute", ".mp4");

    Project project = make_project(kWidth, kHeight, kFps, kDurationMs);
    project.layers.push_back(make_layer("bg", LayerType::Solid, 0, kDurationMs));
    project.layers.back().color = "#202020";
    Layer audio = make_layer("clip", LayerType::Audio, 0, kDurationMs);
    audio.source = sine.string();
    audio.volume = 0.0;
    project.layers.push_back(audio);

    renderer::RenderCache cache = make_cache();
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string(), cache);
    REQUIRE_FALSE(result.has_value());

    const std::string pcm = decode_pcm(output.string(), 8000);
    REQUIRE_FALSE(pcm.empty());
    CHECK(peak_in_window(pcm, 8000, 0, 1000) < 1000);

    std::error_code ignored;
    std::filesystem::remove(sine, ignored);
    std::filesystem::remove(output, ignored);
}

TEST_CASE("a missing audio file is reported with its path", "[audio]") {
    const std::string missing = "/nonexistent/openmotion/definitely_absent.wav";
    Project project = make_project(kWidth, kHeight, kFps, kDurationMs);
    project.layers.push_back(make_layer("bg", LayerType::Solid, 0, kDurationMs));
    Layer audio = make_layer("clip", LayerType::Audio, 0, kDurationMs);
    audio.source = missing;
    project.layers.push_back(audio);

    renderer::RenderCache cache = make_cache();
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, temp_path("openmotion_audio_bad", ".mp4").string(), cache);
    REQUIRE(result.has_value());
    CHECK(result->message.find(missing) != std::string::npos);
}

TEST_CASE("a source without an audio stream is reported with its path", "[audio]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path video_only = make_video_only_mp4();

    Project project = make_project(kWidth, kHeight, kFps, kDurationMs);
    project.layers.push_back(make_layer("bg", LayerType::Solid, 0, kDurationMs));
    Layer audio = make_layer("clip", LayerType::Audio, 0, kDurationMs);
    audio.source = video_only.string();
    project.layers.push_back(audio);

    renderer::RenderCache cache = make_cache();
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, temp_path("openmotion_audio_noaud", ".mp4").string(), cache);
    REQUIRE(result.has_value());
    CHECK(result->message.find(video_only.string()) != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(video_only, ignored);
}

TEST_CASE("a project without audio layers has no audio stream", "[audio]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }
    const std::filesystem::path output = temp_path("openmotion_silent", ".mp4");

    Project project = make_project(kWidth, kHeight, kFps, kDurationMs);
    project.layers.push_back(make_layer("bg", LayerType::Solid, 0, kDurationMs));
    project.layers.back().color = "#202020";

    renderer::RenderCache cache = make_cache();
    const std::optional<renderer::RenderError> result =
        renderer::render_to_mp4(project, output.string(), cache);
    REQUIRE_FALSE(result.has_value());

    CHECK(has_stream_of_type(output.string(), "video"));
    CHECK_FALSE(has_stream_of_type(output.string(), "audio"));

    std::error_code ignored;
    std::filesystem::remove(output, ignored);
}
