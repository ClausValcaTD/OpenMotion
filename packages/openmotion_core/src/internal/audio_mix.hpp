// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_INTERNAL_AUDIO_MIX_HPP
#define OPENMOTION_INTERNAL_AUDIO_MIX_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace openmotion::internal {

// One audio layer to mix, flattened out of the project so the filter-graph
// builder stays pure (no project, no I/O) and easy to unit-test.
struct AudioInput {
    std::string path;
    int64_t start_ms = 0;  // where the layer starts on the timeline
    int64_t end_ms = 0;    // where it ends (only used for the input duration)
    int64_t in_ms = 0;     // offset into the source file
    double volume = 1.0;   // linear gain, 0..10
};

// Builds the ffmpeg -filter_complex graph that delays, gains and mixes every
// audio layer. Input k is the (k+1)-th ffmpeg input (input 0 is the raw video
// pipe), so its stream is [k+1:a]. Each layer becomes
//   [k+1:a]adelay=<start_ms>|<start_ms>,volume=<volume>[aK]
// and the labels are summed by
//   amix=inputs=N:normalize=0:duration=longest[aout]
// Numbers are formatted locale-independently. Returns "" for no inputs.
std::string build_audio_filter_graph(const std::vector<AudioInput>& inputs);

// Formats `value` with a fixed number of decimals using only integer
// conversions, so the decimal separator is always '.' regardless of the
// process locale. Shared by the graph builder and the ffmpeg command assembly.
std::string format_fixed(double value, int decimals);

}  // namespace openmotion::internal

#endif  // OPENMOTION_INTERNAL_AUDIO_MIX_HPP
