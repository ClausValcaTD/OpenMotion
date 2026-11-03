// SPDX-License-Identifier: GPL-3.0-or-later
#include "internal/image_loader.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "internal/shell_quote.hpp"

namespace openmotion::internal {

namespace {

struct PipeCloser {
    void operator()(std::FILE* pipe) const {
        if (pipe != nullptr) {
            pclose(pipe);
        }
    }
};

// Runs `command` through the shell and captures stdout. Returns std::nullopt
// when the process cannot be started or exits with a non-zero status.
std::optional<std::string> run_command(const std::string& command) {
    std::unique_ptr<std::FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
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

std::variant<std::pair<int, int>, renderer::RenderError> probe_size(const std::string& path) {
    const std::string command =
        "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of json " +
        shell_quote(path);

    const std::optional<std::string> output = run_command(command);
    if (!output.has_value()) {
        return renderer::RenderError{"cannot read image " + path + ": ffprobe failed"};
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*output);
    } catch (const nlohmann::json::exception&) {
        return renderer::RenderError{"cannot read image " + path + ": invalid ffprobe output"};
    }

    if (!document.contains("streams") || !document["streams"].is_array() ||
        document["streams"].empty()) {
        return renderer::RenderError{"cannot read image " + path + ": no image stream"};
    }
    const nlohmann::json& stream = document["streams"].front();
    if (!stream.contains("width") || !stream.contains("height")) {
        return renderer::RenderError{"cannot read image " + path + ": missing dimensions"};
    }
    return std::pair<int, int>{stream["width"].get<int>(), stream["height"].get<int>()};
}

std::variant<std::vector<uint8_t>, renderer::RenderError> decode_rgba(const std::string& path,
                                                                     int64_t byte_count) {
    // -noautorotate keeps the decoded pixel layout identical to the probed
    // width/height: a file carrying a rotation tag is not silently transposed,
    // so the raw RGBA buffer always matches the dimensions we sized it for.
    const std::string command = "ffmpeg -v error -noautorotate -i " + shell_quote(path) +
                                " -frames:v 1 -f rawvideo -pix_fmt rgba -";

    std::unique_ptr<std::FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        return renderer::RenderError{"cannot decode image " + path + ": failed to run ffmpeg"};
    }

    std::vector<uint8_t> data;
    try {
        data.resize(static_cast<std::size_t>(byte_count));
    } catch (const std::exception& error) {
        return renderer::RenderError{"cannot decode image " + path + ": " + error.what()};
    }

    std::size_t total = 0;
    while (total < data.size()) {
        const std::size_t got = std::fread(data.data() + total, 1, data.size() - total, pipe.get());
        if (got == 0) {
            break;
        }
        total += got;
    }
    const bool short_read = total != data.size();
    const int status = pclose(pipe.release());
    if (short_read) {
        return renderer::RenderError{"cannot decode image " + path + ": short read"};
    }
    if (status != 0) {
        return renderer::RenderError{"cannot decode image " + path + ": ffmpeg failed"};
    }
    return data;
}

}  // namespace

bool is_supported_image_size(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width > kMaxImageDimension || height > kMaxImageDimension) {
        return false;
    }
    const int64_t bytes = static_cast<int64_t>(width) * static_cast<int64_t>(height) * 4;
    return bytes > 0 && bytes <= kMaxImageBytes;
}

std::variant<MediaInfo, renderer::RenderError> probe_media(const std::string& path) {
    const std::string command =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height:format=duration -of json " +
        shell_quote(path);

    const std::optional<std::string> output = run_command(command);
    if (!output.has_value()) {
        return renderer::RenderError{"cannot read media " + path + ": ffprobe failed"};
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*output);
    } catch (const nlohmann::json::exception&) {
        return renderer::RenderError{"cannot read media " + path + ": invalid ffprobe output"};
    }

    if (!document.contains("streams") || !document["streams"].is_array() ||
        document["streams"].empty()) {
        return renderer::RenderError{"cannot read media " + path + ": no video stream"};
    }
    const nlohmann::json& stream = document["streams"].front();
    if (!stream.contains("width") || !stream.contains("height")) {
        return renderer::RenderError{"cannot read media " + path + ": missing dimensions"};
    }

    MediaInfo info;
    info.width = stream["width"].get<int>();
    info.height = stream["height"].get<int>();

    // Duration arrives as a string (seconds) and is absent for images.
    if (document.contains("format") && document["format"].is_object() &&
        document["format"].contains("duration") && document["format"]["duration"].is_string()) {
        try {
            const double seconds = std::stod(document["format"]["duration"].get<std::string>());
            if (std::isfinite(seconds) && seconds > 0.0) {
                info.duration_ms = static_cast<int64_t>(seconds * 1000.0);
            }
        } catch (const std::exception&) {
            // Leave the duration at 0 (unknown) rather than failing the probe.
        }
    }
    return info;
}

std::variant<renderer::Texture, renderer::RenderError> load_image(const std::string& path) {
    try {
        const std::variant<std::pair<int, int>, renderer::RenderError> size = probe_size(path);
        if (const auto* error = std::get_if<renderer::RenderError>(&size)) {
            return *error;
        }
        const std::pair<int, int> dimensions = std::get<std::pair<int, int>>(size);
        if (!is_supported_image_size(dimensions.first, dimensions.second)) {
            return renderer::RenderError{"cannot decode image " + path + ": unsupported size"};
        }

        const int64_t byte_count =
            static_cast<int64_t>(dimensions.first) * static_cast<int64_t>(dimensions.second) * 4;
        std::variant<std::vector<uint8_t>, renderer::RenderError> decoded =
            decode_rgba(path, byte_count);
        if (const auto* error = std::get_if<renderer::RenderError>(&decoded)) {
            return *error;
        }

        renderer::Texture texture;
        texture.width = dimensions.first;
        texture.height = dimensions.second;
        texture.rgba = std::move(std::get<std::vector<uint8_t>>(decoded));
        return texture;
    } catch (const std::exception& error) {
        return renderer::RenderError{"cannot decode image " + path + ": " + error.what()};
    }
}

}  // namespace openmotion::internal
