// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_INTERNAL_VIDEO_STREAM_HPP
#define OPENMOTION_INTERNAL_VIDEO_STREAM_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "openmotion/renderer.hpp"

namespace openmotion::internal {

// Largest supported frame edge and frame size, matching the image limits.
constexpr int kMaxVideoDimension = 16384;
constexpr int64_t kMaxVideoFrameBytes = 64LL * 1024 * 1024;

// One decoded source frame, resampled to the project fps by ffmpeg.
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // straight-alpha, width*height*4
};

// Frame index for a clip that starts playing at `start_ms`, at timeline time
// `t_ms`, sampled at the project fps: round((t - start_ms) * fps / 1000).
// Returns 0 when t_ms is before start_ms.
int64_t video_frame_index(int64_t start_ms, int64_t t_ms, double fps);

// How a decode position is reached from the current one.
enum class Advance { Reuse, ReadNext, SkipForward, Restart };

struct AdvancePlan {
    Advance action;
    int frames_to_skip;
};

// Plans how to move from `current_index` to `wanted_index`. Pure: no I/O and no
// hidden state. Rules:
//   - no current frame (current_index < 0)             -> Restart
//   - wanted == current                                -> Reuse
//   - wanted == current + 1                            -> ReadNext
//   - current < wanted <= current + 30                 -> SkipForward
//   - wanted < current, or a jump beyond +30 forward   -> Restart
AdvancePlan plan_advance(int64_t current_index, int64_t wanted_index);

// Sequential reader over one video file, decoding to RGBA at a fixed fps. The
// renderer owns it through a shared_ptr so the public renderer header only
// needs a forward declaration. Non-copyable.
class VideoStream {
public:
    ~VideoStream();
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    // Probes the native size and prepares a stream that will resample to `fps`.
    // Never throws; every failure is a RenderError whose message includes the
    // path. The decoder is started lazily on the first frame request.
    static std::variant<std::unique_ptr<VideoStream>, renderer::RenderError> open(
        const std::string& path, double fps);

    // Returns the frame for `wanted_index`, advancing, skipping or restarting
    // the decoder as needed. On end of clip the last decoded frame is held.
    std::variant<VideoFrame, renderer::RenderError> frame_at(int64_t wanted_index);

    int width() const { return width_; }
    int height() const { return height_; }

    // Token only VideoStream itself can construct, so the public constructor
    // below cannot be used to build an un-opened stream.
    struct Passkey {
    private:
        Passkey() = default;
        friend class VideoStream;
    };
    explicit VideoStream(Passkey);

private:
    struct Decoder;

    std::variant<VideoFrame, renderer::RenderError> restart(int64_t wanted_index);
    // Reads one frame from the open decoder into `last_`. On a short read the
    // clip is treated as finished: `exhausted_` is set and the held frame (or
    // an error when nothing was ever decoded) is returned.
    std::variant<VideoFrame, renderer::RenderError> pull_frame();

    std::string path_;
    double fps_ = 0.0;
    int width_ = 0;
    int height_ = 0;
    int64_t duration_ms_ = 0;           // 0 when the container reports none
    std::unique_ptr<Decoder> decoder_;  // nullptr when no process is open
    int64_t current_index_ = -1;        // index of the frame held in last_
    VideoFrame last_;                   // most recently decoded frame
    bool exhausted_ = false;            // clip ended: keep holding last_
};

}  // namespace openmotion::internal

#endif  // OPENMOTION_INTERNAL_VIDEO_STREAM_HPP
