#pragma once

#include <ostream>
#include <stdexcept>
#include <string_view>

namespace atlas {

// Opt-in probe for tests and debug builds. Never put stream I/O in a hot loop.
class ObservationWriter {
public:
    explicit ObservationWriter(std::ostream& output) noexcept : output_(output) {}

    void record(std::string_view key, std::string_view value) {
        if (key.empty() || key.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
                != std::string_view::npos) {
            throw std::invalid_argument("fact key must contain only letters, digits, _, . or -");
        }
        if (value.find_first_of("\r\n") != std::string_view::npos) {
            throw std::invalid_argument("fact value must be one line");
        }
        output_ << key << '=' << value << '\n';
        if (!output_) {
            throw std::runtime_error("could not write observation");
        }
    }

private:
    std::ostream& output_;
};

} // namespace atlas
