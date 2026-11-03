// SPDX-License-Identifier: GPL-3.0-or-later
#include "internal/video_stream.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "internal/image_loader.hpp"
#include "internal/shell_quote.hpp"

namespace openmotion::internal {

namespace {

// Formats `index / fps` seconds with millisecond precision without relying on
// the process locale (which is not guaranteed to use '.' as the separator). The
// fraction is truncated, never rounded up: seeking to a time slightly *before*
// a frame's PTS still yields that frame, whereas seeking even a fraction past
// it can fall off the end of the clip.
std::string format_seconds(int64_t index, double fps) {
    double seconds = fps > 0.0 ? static_cast<double>(index) / fps : 0.0;
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    long long whole = static_cast<long long>(seconds);
    long long millis =
        static_cast<long long>(std::floor((seconds - static_cast<double>(whole)) * 1000.0));
    if (millis >= 1000) {
        millis -= 1000;
        whole += 1;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%lld.%03lld", whole, millis);
    return std::string(buffer);
}

// Renders a project fps for ffmpeg's `fps=` filter, e.g. 30 -> "30".
std::string format_fps(double fps) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", fps);
    return std::string(buffer);
}

}  // namespace

int64_t video_frame_index(int64_t start_ms, int64_t t_ms, double fps) {
    if (t_ms < start_ms || !(fps > 0.0) || !std::isfinite(fps)) {
        return 0;
    }
    const double index = static_cast<double>(t_ms - start_ms) * fps / 1000.0;
    if (!std::isfinite(index) || index <= 0.0) {
        return 0;
    }
    return static_cast<int64_t>(std::llround(index));
}

AdvancePlan plan_advance(int64_t current_index, int64_t wanted_index) {
    if (current_index < 0) {
        return AdvancePlan{Advance::Restart, 0};
    }
    if (wanted_index == current_index) {
        return AdvancePlan{Advance::Reuse, 0};
    }
    if (wanted_index == current_index + 1) {
        return AdvancePlan{Advance::ReadNext, 0};
    }
    if (wanted_index > current_index && wanted_index <= current_index + 30) {
        return AdvancePlan{Advance::SkipForward,
                           static_cast<int>(wanted_index - current_index - 1)};
    }
    return AdvancePlan{Advance::Restart, 0};
}

struct VideoStream::Decoder {
    std::FILE* pipe = nullptr;

    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder() {
        if (pipe != nullptr) {
            pclose(pipe);
        }
    }
};

VideoStream::VideoStream(Passkey) {}
VideoStream::~VideoStream() = default;

std::variant<std::unique_ptr<VideoStream>, renderer::RenderError> VideoStream::open(
    const std::string& path, double fps) {
    if (!(fps > 0.0) || !std::isfinite(fps)) {
        return renderer::RenderError{"cannot open video " + path + ": invalid fps"};
    }

    const std::variant<MediaInfo, renderer::RenderError> info = probe_media(path);
    if (const auto* error = std::get_if<renderer::RenderError>(&info)) {
        return *error;
    }
    const MediaInfo media = std::get<MediaInfo>(info);
    if (media.width <= 0 || media.height <= 0 || media.width > kMaxVideoDimension ||
        media.height > kMaxVideoDimension) {
        return renderer::RenderError{"cannot open video " + path + ": unsupported size"};
    }
    const int64_t frame_bytes =
        static_cast<int64_t>(media.width) * static_cast<int64_t>(media.height) * 4;
    if (frame_bytes <= 0 || frame_bytes > kMaxVideoFrameBytes) {
        return renderer::RenderError{"cannot open video " + path + ": frame too large"};
    }

    auto stream = std::unique_ptr<VideoStream>(new VideoStream(Passkey{}));
    stream->path_ = path;
    stream->fps_ = fps;
    stream->width_ = media.width;
    stream->height_ = media.height;
    stream->duration_ms_ = media.duration_ms;
    return stream;
}

std::variant<VideoFrame, renderer::RenderError> VideoStream::restart(int64_t wanted_index) {
    decoder_.reset();

    // A seek at or past the container duration decodes nothing, so clamp to the
    // last frame that can still be produced: floor(duration * fps / 1000) - 1.
    int64_t seek_index = wanted_index;
    if (duration_ms_ > 0) {
        const double frames = std::floor(static_cast<double>(duration_ms_) * fps_ / 1000.0);
        const int64_t last_index = static_cast<int64_t>(frames) - 1;
        if (seek_index > last_index) {
            seek_index = last_index > 0 ? last_index : 0;
        }
    }

    const std::string command = "ffmpeg -v error -ss " + format_seconds(seek_index, fps_) +
                                " -noautorotate -i " + shell_quote(path_) + " -an -vf \"fps=" +
                                format_fps(fps_) + "\" -f rawvideo -pix_fmt rgba -";

    std::FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return renderer::RenderError{"cannot decode video " + path_ + ": failed to run ffmpeg"};
    }
    auto decoder = std::make_unique<Decoder>();
    decoder->pipe = pipe;
    decoder_ = std::move(decoder);
    current_index_ = seek_index;
    exhausted_ = false;
    return pull_frame();
}

std::variant<VideoFrame, renderer::RenderError> VideoStream::pull_frame() {
    if (decoder_ == nullptr || decoder_->pipe == nullptr) {
        return renderer::RenderError{"cannot decode video " + path_ + ": no decoder"};
    }
    const std::size_t frame_bytes =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4;

    VideoFrame frame;
    frame.width = width_;
    frame.height = height_;
    try {
        frame.rgba.resize(frame_bytes);
    } catch (const std::exception& error) {
        return renderer::RenderError{"cannot decode video " + path_ + ": " + error.what()};
    }

    std::size_t total = 0;
    while (total < frame_bytes) {
        const std::size_t got =
            std::fread(frame.rgba.data() + total, 1, frame_bytes - total, decoder_->pipe);
        if (got == 0) {
            break;
        }
        total += got;
    }

    if (total != frame_bytes) {
        // End of clip: hold the last fully decoded frame, if there is one.
        exhausted_ = true;
        decoder_.reset();
        if (last_.rgba.empty()) {
            return renderer::RenderError{"cannot decode video " + path_ + ": no frames"};
        }
        return last_;
    }

    last_ = std::move(frame);
    return last_;
}

std::variant<VideoFrame, renderer::RenderError> VideoStream::frame_at(int64_t wanted_index) {
    if (wanted_index < 0) {
        wanted_index = 0;
    }
    if (exhausted_) {
        // At or past the end: keep holding the final frame. Seeking back into
        // the clip must reopen the decoder, so exhaustion is only sticky while
        // the requested index does not move before the held one.
        if (wanted_index >= current_index_) {
            return last_;
        }
        exhausted_ = false;
    }

    const AdvancePlan plan = plan_advance(current_index_, wanted_index);
    switch (plan.action) {
        case Advance::Reuse:
            return last_;
        case Advance::ReadNext: {
            const std::variant<VideoFrame, renderer::RenderError> frame = pull_frame();
            if (std::holds_alternative<VideoFrame>(frame) && !exhausted_) {
                current_index_ = wanted_index;
            }
            return frame;
        }
        case Advance::SkipForward: {
            for (int i = 0; i < plan.frames_to_skip; ++i) {
                const std::variant<VideoFrame, renderer::RenderError> skipped = pull_frame();
                if (const auto* error = std::get_if<renderer::RenderError>(&skipped)) {
                    return *error;
                }
                if (exhausted_) {
                    return last_;  // clip ended before reaching the target
                }
            }
            const std::variant<VideoFrame, renderer::RenderError> frame = pull_frame();
            if (std::holds_alternative<VideoFrame>(frame) && !exhausted_) {
                current_index_ = wanted_index;
            }
            return frame;
        }
        case Advance::Restart:
            break;
    }
    return restart(wanted_index);
}

}  // namespace openmotion::internal
