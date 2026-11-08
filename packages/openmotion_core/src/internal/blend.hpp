// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_INTERNAL_BLEND_HPP
#define OPENMOTION_INTERNAL_BLEND_HPP

#include <algorithm>
#include <cmath>

#include "openmotion/project.hpp"

namespace openmotion::internal {

namespace blend_detail {

inline double clamp_unit(double value) {
    if (value <= 0.0) return 0.0;
    if (value >= 1.0) return 1.0;
    return value;
}

}  // namespace blend_detail

// Separable blend function for one channel, following the standard
// compositing-and-blending definitions. `cb` is the backdrop channel and `cs`
// the source channel, both in [0, 1]. The result is clamped to [0, 1]. A NaN
// input returns the backdrop unchanged, so a broken source value never
// corrupts the canvas. Pure and inline: no I/O, no allocation.
inline double blend_channel(project::BlendMode mode, double cb, double cs) {
    if (std::isnan(cb) || std::isnan(cs)) {
        return cb;
    }
    double result = cs;
    switch (mode) {
        case project::BlendMode::Normal:
            result = cs;
            break;
        case project::BlendMode::Multiply:
            result = cb * cs;
            break;
        case project::BlendMode::Screen:
            result = cb + cs - cb * cs;
            break;
        case project::BlendMode::Overlay:
            result = cb <= 0.5 ? 2.0 * cb * cs : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
            break;
        case project::BlendMode::Exclusion:
            result = cb + cs - 2.0 * cb * cs;
            break;
        case project::BlendMode::ColorBurn:
            if (cb >= 1.0) {
                result = 1.0;
            } else if (cs <= 0.0) {
                result = 0.0;
            } else {
                result = 1.0 - std::min(1.0, (1.0 - cb) / cs);
            }
            break;
        case project::BlendMode::LinearDodge:
            result = std::min(1.0, cb + cs);
            break;
    }
    return blend_detail::clamp_unit(result);
}

}  // namespace openmotion::internal

#endif  // OPENMOTION_INTERNAL_BLEND_HPP
