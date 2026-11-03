// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_KEYFRAME_HPP
#define OPENMOTION_KEYFRAME_HPP

#include <cstdint>

#include "openmotion/project.hpp"

namespace openmotion::keyframe {

// Maps t in [0,1] through an easing curve. Input is clamped to [0,1].
//   Linear:    t
//   EaseIn:    t*t*t
//   EaseOut:   1 - (1-t)^3
//   EaseInOut: t < 0.5 ? 4*t^3 : 1 - ((-2*t + 2)^3) / 2
//   Hold:      0 for t < 1, 1 only when t >= 1 (step function)
double apply_easing(project::Easing easing, double t);

// Value of a property at time t_ms.
//  - No keyframes: return p.constant.
//  - t_ms before the first keyframe: first keyframe's value.
//  - t_ms at/after the last keyframe: last keyframe's value.
//  - Otherwise find the segment [k0, k1] with k0.time_ms <= t_ms < k1.time_ms
//    and return lerp(k0.value, k1.value, apply_easing(k0.easing, u))
//    where u = (t_ms - k0.time_ms) / (k1.time_ms - k0.time_ms).
//    The easing of a segment is the easing stored on its START keyframe.
//  - Two keyframes with equal time_ms must not divide by zero.
//  - Uses binary search (std::upper_bound), not a linear scan.
double evaluate(const project::Property& p, int64_t t_ms);

struct EvaluatedTransform {
    double x, y, scale_x, scale_y, rotation, opacity;
};

EvaluatedTransform evaluate_transform(const project::Transform& tr, int64_t t_ms);

// True when layer.start_ms <= t_ms < layer.end_ms.
bool is_layer_active(const project::Layer& layer, int64_t t_ms);

}  // namespace openmotion::keyframe

#endif  // OPENMOTION_KEYFRAME_HPP
