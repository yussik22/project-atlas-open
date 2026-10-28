#include <cstddef>
#include <cstdlib>
#include <memory>
#include <windows.h>

int main() {
    std::unique_ptr<std::byte[]> buffer(new std::byte[64]);
    buffer[0] = std::byte{0x2a};
    if (GetEnvironmentVariableW(L"ATLAS_AUTO_DEMO_CRASH", nullptr, 0) > 0)
        std::_Exit(12);
    {
        auto temporary = std::make_unique<int[]>(8);
        temporary[0] = 7;
        if (temporary[0] != 7) return 1;
    }
    return buffer[0] == std::byte{0x2a} ? 0 : 1;
}
