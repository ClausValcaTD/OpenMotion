// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OPENMOTION_INTERNAL_SHELL_QUOTE_HPP
#define OPENMOTION_INTERNAL_SHELL_QUOTE_HPP

#include <string>

namespace openmotion::internal {

// Wraps `value` in single quotes for /bin/sh, escaping embedded quotes so a
// filename cannot inject extra shell commands.
inline std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

}  // namespace openmotion::internal

#endif  // OPENMOTION_INTERNAL_SHELL_QUOTE_HPP
