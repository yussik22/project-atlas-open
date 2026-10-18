// Link this translation unit into an executable to record ordinary C++
// new/delete activity when ATLAS_RUNTIME_TRACE names an output file.
// Deliberately uses C allocation and fixed storage: the recorder must not
// recursively invoke the global operators it replaces.
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <iterator>
#include <malloc.h>
#include <new>

namespace {

constexpr std::size_t table_size = 32768;
// A fresh site consumes at least two events. Keep the site table larger than
// the number of distinct sites possible before the event cap is reached.
constexpr std::size_t max_sites = 8192;
constexpr std::size_t max_events = 10000;
struct Entry {
    void* pointer = nullptr;
    std::uint64_t id = 0;
    std::uint64_t site_id = 0;
    bool tombstone = false;
};
struct Site {
    void* address = nullptr;
    std::uint64_t id = 0;
};

Entry entries[table_size];
Site sites[max_sites];
std::atomic_flag guard = ATOMIC_FLAG_INIT;
FILE* output = nullptr;
bool initialized = false;
std::uint64_t next_id = 1;
std::uint64_t next_site_id = 1;
std::size_t events = 0;
bool saturated = false;
thread_local bool inside_recorder = false;

struct ScopedLock {
    ScopedLock() { while (guard.test_and_set(std::memory_order_acquire)) YieldProcessor(); }
    ~ScopedLock() { guard.clear(std::memory_order_release); }
};

void close_output() noexcept {
    ScopedLock lock;
    if (output) {
        std::fclose(output);
        output = nullptr;
    }
}

void initialize() noexcept {
    if (initialized) return;
    initialized = true;
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"ATLAS_RUNTIME_TRACE", path,
                                                 static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return;
    if (_wfopen_s(&output, path, L"wb") != 0 || !output) return;
    std::setvbuf(output, nullptr, _IONBF, 0);
    std::fputs("# atlas-trace-v1\n", output);
    std::atexit(close_output);
}

std::size_t slot_for(void* pointer) noexcept {
    auto value = reinterpret_cast<std::uintptr_t>(pointer);
    value ^= value >> 17;
    value *= 0x9e3779b97f4a7c15ULL;
    return static_cast<std::size_t>(value & (table_size - 1));
}

void record_limit() noexcept {
    if (!output || saturated) return;
    std::fprintf(output, "%zu\tcapture.truncated\tcapture\t10000\n", ++events);
    saturated = true;
}

void record_create(void* pointer, std::size_t bytes, void* call_site) noexcept {
    if (!pointer || inside_recorder) return;
    inside_recorder = true;
    {
        ScopedLock lock;
        initialize();
        if (output && !saturated) {
            Site* found_site = nullptr;
            Site* empty_site = nullptr;
            for (auto& site : sites) {
                if (site.address == call_site) { found_site = &site; break; }
                if (!site.address && !empty_site) empty_site = &site;
            }
            const bool new_site = !found_site && empty_site;
            const std::size_t needed = (new_site ? 1 : 0) + 1 +
                                       (found_site || new_site ? 1 : 0);
            if (events + needed >= max_events) {
                record_limit();
                inside_recorder = false;
                return;
            }
            const std::size_t start = slot_for(pointer);
            std::size_t first_tombstone = table_size;
            for (std::size_t probe = 0; probe < table_size; ++probe) {
                const auto slot = (start + probe) & (table_size - 1);
                auto& entry = entries[slot];
                if (entry.tombstone && first_tombstone == table_size)
                    first_tombstone = slot;
                if (!entry.pointer && !entry.tombstone) {
                    auto& target = entries[first_tombstone == table_size ? slot
                                                                          : first_tombstone];
                    if (new_site) {
                        empty_site->address = call_site;
                        empty_site->id = next_site_id++;
                        found_site = empty_site;
                        HMODULE module = nullptr;
                        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCWSTR>(call_site), &module);
                        if (module == GetModuleHandleW(nullptr)) {
                            const auto offset = reinterpret_cast<std::uintptr_t>(call_site) -
                                                reinterpret_cast<std::uintptr_t>(module);
                            std::fprintf(output,
                                         "%zu\tsite.create\tsite.%llu\texe+0x%llx\n",
                                         ++events,
                                         static_cast<unsigned long long>(found_site->id),
                                         static_cast<unsigned long long>(offset));
                        } else {
                            std::fprintf(output,
                                         "%zu\tsite.create\tsite.%llu\tmodule site %p\n",
                                         ++events,
                                         static_cast<unsigned long long>(found_site->id),
                                         call_site);
                        }
                    }
                    target = {pointer, next_id++, found_site ? found_site->id : 0, false};
                    std::fprintf(output, "%zu\tobj.create\theap.%llu\theap allocation %zu B\n",
                                 ++events, static_cast<unsigned long long>(target.id), bytes);
                    if (target.site_id)
                        std::fprintf(output,
                                     "%zu\tlink.add\tsite.%llu\tallocates:heap.%llu\n",
                                     ++events,
                                     static_cast<unsigned long long>(target.site_id),
                                     static_cast<unsigned long long>(target.id));
                    break;
                }
            }
        }
    }
    inside_recorder = false;
}

void record_destroy(void* pointer) noexcept {
    if (!pointer || inside_recorder) return;
    inside_recorder = true;
    {
        ScopedLock lock;
        if (output && !saturated) {
            const std::size_t start = slot_for(pointer);
            for (std::size_t probe = 0; probe < table_size; ++probe) {
                auto& entry = entries[(start + probe) & (table_size - 1)];
                if (!entry.pointer && !entry.tombstone) break;
                if (entry.pointer == pointer) {
                    if (events + (entry.site_id ? 2 : 1) >= max_events) {
                        record_limit();
                        break;
                    }
                    if (entry.site_id)
                        std::fprintf(output,
                                     "%zu\tlink.remove\tsite.%llu\tallocates:heap.%llu\n",
                                     ++events,
                                     static_cast<unsigned long long>(entry.site_id),
                                     static_cast<unsigned long long>(entry.id));
                    std::fprintf(output, "%zu\tobj.destroy\theap.%llu\t-\n",
                                 ++events, static_cast<unsigned long long>(entry.id));
                    entry.pointer = nullptr;
                    entry.tombstone = true;
                    break;
                }
            }
        }
    }
    inside_recorder = false;
}

void* allocate(std::size_t bytes, std::size_t alignment, void* call_site) {
    void* pointer = alignment ? _aligned_malloc(bytes ? bytes : 1, alignment)
                              : std::malloc(bytes ? bytes : 1);
    if (!pointer) throw std::bad_alloc{};
    record_create(pointer, bytes, call_site);
    return pointer;
}

void deallocate(void* pointer, bool aligned) noexcept {
    record_destroy(pointer);
    if (aligned) _aligned_free(pointer);
    else std::free(pointer);
}

} // namespace

void* operator new(std::size_t bytes) { return allocate(bytes, 0, _ReturnAddress()); }
void* operator new[](std::size_t bytes) { return allocate(bytes, 0, _ReturnAddress()); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment), _ReturnAddress());
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment), _ReturnAddress());
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes, 0, _ReturnAddress()); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes, 0, _ReturnAddress()); } catch (...) { return nullptr; }
}
void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try { return allocate(bytes, static_cast<std::size_t>(alignment), _ReturnAddress()); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try { return allocate(bytes, static_cast<std::size_t>(alignment), _ReturnAddress()); }
    catch (...) { return nullptr; }
}
void operator delete(void* pointer) noexcept { deallocate(pointer, false); }
void operator delete[](void* pointer) noexcept { deallocate(pointer, false); }
void operator delete(void* pointer, std::size_t) noexcept { deallocate(pointer, false); }
void operator delete[](void* pointer, std::size_t) noexcept { deallocate(pointer, false); }
void operator delete(void* pointer, std::align_val_t) noexcept { deallocate(pointer, true); }
void operator delete[](void* pointer, std::align_val_t) noexcept { deallocate(pointer, true); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    deallocate(pointer, true);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    deallocate(pointer, true);
}
