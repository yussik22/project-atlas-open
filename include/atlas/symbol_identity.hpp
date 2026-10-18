#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atlas {

// Deterministic byte hash for generated artifacts, not a unique symbol identity.
// Persist the original Clang USR as the key and check for collisions before
// replacing it with this shorter presentation ID.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (unsigned char byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

[[nodiscard]] inline std::string mermaid_symbol_id(std::string_view identity) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "C_0000000000000000";
    auto value = fnv1a64(identity);
    for (std::size_t position = result.size(); position-- > 2;) {
        result[position] = digits[value & 0xfU];
        value >>= 4U;
    }
    return result;
}

static_assert(fnv1a64("") == 0xcbf29ce484222325ULL);
static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);

} // namespace atlas
