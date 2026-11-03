// SPDX-License-Identifier: GPL-3.0-or-later
#include "internal/audio_mix.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace openmotion::internal {

std::string format_fixed(double value, int decimals) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 9) {
        decimals = 9;
    }
    const bool negative = value < 0.0;
    const double magnitude = negative ? -value : value;

    long long factor = 1;
    for (int i = 0; i < decimals; ++i) {
        factor *= 10;
    }
    const long long rounded = static_cast<long long>(magnitude * static_cast<double>(factor) + 0.5);
    const long long whole = rounded / factor;
    const long long fraction = rounded % factor;

    std::string fraction_text = std::to_string(fraction);
    if (fraction_text.size() < static_cast<std::size_t>(decimals)) {
        fraction_text.insert(0, static_cast<std::size_t>(decimals) - fraction_text.size(), '0');
    }

    std::string text = std::to_string(whole);
    if (decimals > 0) {
        text += '.';
        text += fraction_text;
    }
    if (negative) {
        text.insert(0, 1, '-');
    }
    return text;
}

std::string build_audio_filter_graph(const std::vector<AudioInput>& inputs) {
    if (inputs.empty()) {
        return "";
    }

    std::string graph;
    std::string mix_labels;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const AudioInput& input = inputs[i];
        if (!graph.empty()) {
            graph += ';';
        }
        const std::string stream = "[" + std::to_string(i + 1) + ":a]";
        const std::string label = "[a" + std::to_string(i) + "]";
        graph += stream + "adelay=" + std::to_string(input.start_ms) + "|" +
                 std::to_string(input.start_ms) + ",volume=" + format_fixed(input.volume, 6) + label;
        mix_labels += label;
    }

    graph += ";" + mix_labels + "amix=inputs=" + std::to_string(inputs.size()) +
             ":normalize=0:duration=longest[aout]";
    return graph;
}

}  // namespace openmotion::internal
