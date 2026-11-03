// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_MEDIA_HPP
#define OPENMOTION_MEDIA_HPP

#include <cstdint>
#include <string>
#include <variant>

namespace openmotion::media {

struct VideoMetadata {
    int64_t duration_ms;
    double  fps;
    int     width;
    int     height;
};

struct MediaError {
    std::string message;
};

// Parses an ffprobe frame-rate fraction such as "30000/1001" or "25/1".
// A zero denominator or malformed input yields a MediaError.
std::variant<double, MediaError> parse_frame_rate(const std::string& fraction);

// Runs the external `ffprobe` executable on `path` and returns metadata for
// the first video stream. Never throws: every failure is reported as a
// MediaError.
std::variant<VideoMetadata, MediaError> open_video(const std::string& path);

}  // namespace openmotion::media

#endif  // OPENMOTION_MEDIA_HPP
