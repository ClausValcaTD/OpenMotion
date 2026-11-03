// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/keyframe.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace openmotion::keyframe {

namespace {

double clamp_unit(double t) {
    if (t < 0.0) return 0.0;
    if (t > 1.0) return 1.0;
    return t;
}

double cube(double v) { return v * v * v; }

}  // namespace

double apply_easing(project::Easing easing, double t) {
    t = clamp_unit(t);
    switch (easing) {
        case project::Easing::Linear:
            return t;
        case project::Easing::EaseIn:
            return cube(t);
        case project::Easing::EaseOut:
            return 1.0 - cube(1.0 - t);
        case project::Easing::EaseInOut:
            return t < 0.5 ? 4.0 * cube(t) : 1.0 - cube(-2.0 * t + 2.0) / 2.0;
        case project::Easing::Hold:
            return t >= 1.0 ? 1.0 : 0.0;
    }
    return t;
}

double evaluate(const project::Property& p, int64_t t_ms) {
    const std::vector<project::Keyframe>& keyframes = p.keyframes;
    if (keyframes.empty()) {
        return p.constant;
    }

    // First keyframe strictly after t_ms; earlier keyframes are <= t_ms.
    const auto after = std::upper_bound(
        keyframes.begin(), keyframes.end(), t_ms,
        [](int64_t time, const project::Keyframe& keyframe) { return time < keyframe.time_ms; });

    if (after == keyframes.begin()) {
        return keyframes.front().value;
    }
    if (after == keyframes.end()) {
        return keyframes.back().value;
    }

    const project::Keyframe& k0 = *(after - 1);
    const project::Keyframe& k1 = *after;
    const int64_t span = k1.time_ms - k0.time_ms;
    if (span <= 0) {
        // Equal-time keyframes: no segment to interpolate, hold the earlier value.
        return k0.value;
    }
    const double u = static_cast<double>(t_ms - k0.time_ms) / static_cast<double>(span);
    const double eased = apply_easing(k0.easing, u);
    return k0.value + (k1.value - k0.value) * eased;
}

EvaluatedTransform evaluate_transform(const project::Transform& tr, int64_t t_ms) {
    return EvaluatedTransform{
        evaluate(tr.x, t_ms),
        evaluate(tr.y, t_ms),
        evaluate(tr.scale_x, t_ms),
        evaluate(tr.scale_y, t_ms),
        evaluate(tr.rotation, t_ms),
        evaluate(tr.opacity, t_ms),
    };
}

bool is_layer_active(const project::Layer& layer, int64_t t_ms) {
    return layer.start_ms <= t_ms && t_ms < layer.end_ms;
}

}  // namespace openmotion::keyframe
