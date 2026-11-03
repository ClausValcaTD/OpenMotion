// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/media.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <variant>

#include <unistd.h>

namespace {

bool command_available(const std::string& name) {
    const std::string command = "command -v " + name + " > /dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

double parsed_fps(const std::string& fraction) {
    const std::variant<double, openmotion::media::MediaError> result =
        openmotion::media::parse_frame_rate(fraction);
    REQUIRE(std::holds_alternative<double>(result));
    return std::get<double>(result);
}

}  // namespace

TEST_CASE("frame rate fractions are parsed", "[media]") {
    CHECK(parsed_fps("30000/1001") == Catch::Approx(29.97).epsilon(0.001));
    CHECK(parsed_fps("25/1") == Catch::Approx(25.0));

    const std::variant<double, openmotion::media::MediaError> zero =
        openmotion::media::parse_frame_rate("0/0");
    CHECK(std::holds_alternative<openmotion::media::MediaError>(zero));
}

TEST_CASE("open_video reports a missing file as an error", "[media]") {
    const std::variant<openmotion::media::VideoMetadata, openmotion::media::MediaError> result =
        openmotion::media::open_video("/nonexistent/openmotion_missing_clip.mp4");
    REQUIRE(std::holds_alternative<openmotion::media::MediaError>(result));
    CHECK_FALSE(std::get<openmotion::media::MediaError>(result).message.empty());
}

TEST_CASE("open_video reads metadata from a generated clip", "[media]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    const std::filesystem::path clip =
        std::filesystem::temp_directory_path() /
        ("openmotion_test_" + std::to_string(::getpid()) + ".mp4");

    const std::string generate =
        "ffmpeg -hide_banner -loglevel error -f lavfi "
        "-i testsrc=size=320x240:rate=25 -t 1 -c:v mpeg4 -pix_fmt yuv420p -y '" +
        clip.string() + "'";
    REQUIRE(std::system(generate.c_str()) == 0);

    const std::variant<openmotion::media::VideoMetadata, openmotion::media::MediaError> result =
        openmotion::media::open_video(clip.string());

    std::error_code ignored;
    std::filesystem::remove(clip, ignored);

    REQUIRE(std::holds_alternative<openmotion::media::VideoMetadata>(result));
    const openmotion::media::VideoMetadata& metadata =
        std::get<openmotion::media::VideoMetadata>(result);
    CHECK(metadata.width == 320);
    CHECK(metadata.height == 240);
    CHECK(metadata.fps == Catch::Approx(25.0).epsilon(0.01));
    CHECK(metadata.duration_ms > 800);
    CHECK(metadata.duration_ms < 1200);
}
