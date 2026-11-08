// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/keyframe.hpp"

#include <algorithm>
#include <cmath>
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

// B(s) = 3(1-s)^2 s c1 + 3(1-s) s^2 c2 + s^3, for one axis with control
// points c1/c2 (x1/x2 or y1/y2). The curve starts at 0 and ends at 1.
double bezier_axis(double c1, double c2, double s) {
    const double one_minus = 1.0 - s;
    return 3.0 * one_minus * one_minus * s * c1 + 3.0 * one_minus * s * s * c2 + s * s * s;
}

double bezier_axis_derivative(double c1, double c2, double s) {
    const double one_minus = 1.0 - s;
    return 3.0 * one_minus * one_minus * c1 + 6.0 * one_minus * s * (c2 - c1) + 3.0 * s * s * (1.0 - c2);
}

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
        case project::Easing::CubicBezier:
            // No control points are available in this overload; the Keyframe
            // overload below carries them. Fall back to linear.
            return t;
    }
    return t;
}

double cubic_bezier_ease(double x1, double y1, double x2, double y2, double u) {
    if (std::isnan(u) || u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;

    // Solve Bx(s) = u for s in [0,1]. Bx is monotonic when x1, x2 are in
    // [0,1], which the loader guarantees.
    double s = u;
    bool converged = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const double error = bezier_axis(x1, x2, s) - u;
        const double slope = bezier_axis_derivative(x1, x2, s);
        if (std::abs(slope) < 1e-6) {
            break;
        }
        const double next = s - error / slope;
        if (!(next >= 0.0 && next <= 1.0)) {
            break;
        }
        s = next;
        if (std::abs(error) < 1e-12) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        // Bisection fallback; 60 halvings give far better than 1e-7 in s.
        double low = 0.0;
        double high = 1.0;
        for (int iteration = 0; iteration < 60; ++iteration) {
            s = 0.5 * (low + high);
            if (bezier_axis(x1, x2, s) < u) {
                low = s;
            } else {
                high = s;
            }
        }
        s = 0.5 * (low + high);
    }
    return bezier_axis(y1, y2, s);
}

double apply_easing(const project::Keyframe& k, double t) {
    if (k.easing == project::Easing::CubicBezier) {
        return cubic_bezier_ease(k.bezier[0], k.bezier[1], k.bezier[2], k.bezier[3], t);
    }
    return apply_easing(k.easing, t);
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
    const double eased = apply_easing(k0, u);
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
