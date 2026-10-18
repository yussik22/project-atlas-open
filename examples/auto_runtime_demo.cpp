#include <cstddef>
#include <memory>

int main() {
    auto buffer = std::make_unique<std::byte[]>(64);
    buffer[0] = std::byte{0x2a};
    {
        auto temporary = std::make_unique<int[]>(8);
        temporary[0] = 7;
        if (temporary[0] != 7) return 1;
    }
    return buffer[0] == std::byte{0x2a} ? 0 : 1;
}
