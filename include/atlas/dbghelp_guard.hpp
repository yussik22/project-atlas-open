#pragma once

#include <mutex>

namespace atlas {

// DbgHelp has process-wide state and must not be called concurrently.
inline std::mutex& dbghelp_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace atlas
