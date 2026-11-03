// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/media.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace openmotion::media {

namespace {

// Wraps `value` in single quotes for /bin/sh, escaping embedded quotes so a
// filename cannot inject extra shell commands.
std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

std::variant<double, MediaError> parse_fraction(const std::string& fraction) {
    const std::size_t slash = fraction.find('/');
    if (slash == std::string::npos) {
        return MediaError{"frame rate is not a fraction: " + fraction};
    }
    try {
        const double numerator = std::stod(fraction.substr(0, slash));
        const double denominator = std::stod(fraction.substr(slash + 1));
        if (denominator == 0.0) {
            return MediaError{"frame rate has a zero denominator: " + fraction};
        }
        const double fps = numerator / denominator;
        if (!(fps > 0.0)) {
            return MediaError{"frame rate is not positive: " + fraction};
        }
        return fps;
    } catch (const std::exception&) {
        return MediaError{"cannot parse frame rate: " + fraction};
    }
}

struct PipeCloser {
    void operator()(FILE* pipe) const {
        if (pipe != nullptr) {
            pclose(pipe);
        }
    }
};

// Runs `command` through the shell and captures stdout. Returns std::nullopt
// when the process cannot be started or exits with a non-zero status.
std::optional<std::string> run_command(const std::string& command) {
    std::unique_ptr<FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        return std::nullopt;
    }

    std::string output;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe.get())) != 0) {
        output.append(buffer, read);
    }
    if (std::ferror(pipe.get()) != 0) {
        return std::nullopt;
    }
    if (pclose(pipe.release()) != 0) {
        return std::nullopt;
    }
    return output;
}

std::variant<VideoMetadata, MediaError> read_metadata(const std::string& output) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(output);
    } catch (const nlohmann::json::exception& error) {
        return MediaError{std::string{"invalid ffprobe output: "} + error.what()};
    }

    if (!document.contains("streams") || !document["streams"].is_array() ||
        document["streams"].empty()) {
        return MediaError{"ffprobe reported no video stream"};
    }
    const nlohmann::json& stream = document["streams"].front();
    if (!stream.contains("width") || !stream.contains("height") ||
        !stream.contains("r_frame_rate")) {
        return MediaError{"ffprobe output is missing stream fields"};
    }

    const std::variant<double, MediaError> fps = parse_fraction(stream["r_frame_rate"].get<std::string>());
    if (const auto* error = std::get_if<MediaError>(&fps)) {
        return *error;
    }

    int64_t duration_ms = 0;
    if (document.contains("format") && document["format"].is_object() &&
        document["format"].contains("duration")) {
        const std::string duration = document["format"]["duration"].get<std::string>();
        try {
            duration_ms = static_cast<int64_t>(std::llround(std::stod(duration) * 1000.0));
        } catch (const std::exception&) {
            return MediaError{"cannot parse duration: " + duration};
        }
    }

    return VideoMetadata{
        duration_ms,
        std::get<double>(fps),
        stream["width"].get<int>(),
        stream["height"].get<int>(),
    };
}

}  // namespace

std::variant<double, MediaError> parse_frame_rate(const std::string& fraction) {
    return parse_fraction(fraction);
}

std::variant<VideoMetadata, MediaError> open_video(const std::string& path) {
    try {
        const std::string command =
            "ffprobe -v error -select_streams v:0 "
            "-show_entries stream=width,height,r_frame_rate:format=duration "
            "-of json " + shell_quote(path);

        const std::optional<std::string> output = run_command(command);
        if (!output.has_value()) {
            return MediaError{"failed to run ffprobe"};
        }
        return read_metadata(*output);
    } catch (const std::exception& error) {
        return MediaError{std::string{"unexpected ffprobe failure: "} + error.what()};
    }
}

}  // namespace openmotion::media
