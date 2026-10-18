#include "atlas/capture.hpp"
#include "atlas/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cwchar>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace atlas {

#ifdef _WIN32
namespace {

std::string utf8(const wchar_t* value) {
    if (!value) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), bytes,
                        nullptr, nullptr);
    result.pop_back();
    return result;
}

bool enrich_sites(const std::filesystem::path& executable, Trace& trace) {
    constexpr DWORD64 synthetic_base = 0x100000000ULL;
    const HANDLE process = GetCurrentProcess();
    const auto search_path = executable.parent_path().wstring();
    if (!SymInitializeW(process, search_path.c_str(), FALSE)) return false;
    SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    const auto loaded = SymLoadModuleExW(process, nullptr, executable.c_str(),
                                         nullptr, synthetic_base, 0, nullptr, 0);
    bool changed = false;
    if (loaded) {
        for (auto& event : trace) {
            if (event.operation != "site.create" || !event.value.starts_with("exe+0x"))
                continue;
            std::uint64_t offset = 0;
            const auto hex = std::string_view(event.value).substr(6);
            const auto [end, error] = std::from_chars(
                hex.data(), hex.data() + hex.size(), offset, 16);
            if (error != std::errc{} || end != hex.data() + hex.size()) continue;
            alignas(SYMBOL_INFOW) std::byte buffer[sizeof(SYMBOL_INFOW) +
                                                    2048 * sizeof(wchar_t)]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbol->MaxNameLen = 2048;
            DWORD64 displacement = 0;
            if (!SymFromAddrW(process, loaded + offset, &displacement, symbol)) continue;
            std::string label = utf8(symbol->Name);
            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddrW64(process, loaded + offset,
                                     &line_displacement, &line)) {
                if (line.FileName) {
                    const auto filename = std::filesystem::path(line.FileName)
                                              .filename().wstring();
                    label += " @ " + utf8(filename.c_str()) + ':' +
                             std::to_string(line.LineNumber);
                }
            }
            label += " (" + event.value + ')';
            std::replace_if(label.begin(), label.end(),
                            [](unsigned char c) { return c < 0x20U || c == 0x7fU; }, ' ');
            event.value = std::move(label);
            changed = true;
        }
        SymUnloadModule64(process, loaded);
    }
    SymCleanup(process);
    return changed;
}

} // namespace
#endif

std::expected<CaptureResult, std::string> capture_runtime(
    const std::filesystem::path& executable,
    const std::filesystem::path& output,
    std::stop_token stop) {
#ifndef _WIN32
    (void)executable;
    (void)output;
    (void)stop;
    return std::unexpected("automatic capture currently requires Windows");
#else
    namespace fs = std::filesystem;
    try {
        const auto program = fs::absolute(executable);
        const auto trace_path = fs::absolute(output);
        if (!fs::is_regular_file(program))
            return std::unexpected("capture executable does not exist: " + program.string());
        if (fs::exists(trace_path))
            return std::unexpected("capture output already exists: " + trace_path.string());
        fs::create_directories(trace_path.parent_path());

        std::vector<std::wstring> environment;
        LPWCH inherited = GetEnvironmentStringsW();
        if (!inherited) return std::unexpected("cannot read process environment");
        for (const wchar_t* current = inherited; *current;
             current += std::wcslen(current) + 1) {
            std::wstring entry(current);
            if (_wcsnicmp(entry.c_str(), L"ATLAS_RUNTIME_TRACE=", 20) != 0)
                environment.push_back(std::move(entry));
        }
        FreeEnvironmentStringsW(inherited);
        environment.push_back(L"ATLAS_RUNTIME_TRACE=" + trace_path.wstring());
        std::sort(environment.begin(), environment.end(),
                  [](const std::wstring& a, const std::wstring& b) {
                      return _wcsicmp(a.c_str(), b.c_str()) < 0;
                  });
        std::wstring block;
        for (const auto& entry : environment) {
            block += entry;
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        std::wstring command = L"\"" + program.wstring() + L"\"";
        const auto working_dir = program.parent_path().wstring();
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr,
                            FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                            block.data(), working_dir.c_str(), &startup, &process))
            return std::unexpected("cannot start capture executable (Windows error " +
                                   std::to_string(GetLastError()) + ')');
        CloseHandle(process.hThread);
        for (;;) {
            const DWORD waited = WaitForSingleObject(process.hProcess, 100);
            if (waited == WAIT_OBJECT_0) break;
            if (waited != WAIT_TIMEOUT) {
                CloseHandle(process.hProcess);
                return std::unexpected("error waiting for capture executable");
            }
            if (stop.stop_requested()) {
                CloseHandle(process.hProcess);
                return std::unexpected("capture stopped; target process remains running");
            }
        }
        DWORD exit_code = 0;
        const BOOL got_exit = GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hProcess);
        if (!got_exit) return std::unexpected("cannot read capture exit code");
        if (!fs::exists(trace_path))
            return std::unexpected("target produced no trace; link auto_runtime.cpp "
                                   "into its executable before capturing");
        auto trace = read_trace(trace_path);
        if (!trace) return std::unexpected(trace.error());
        if (auto replay = replay_runtime(*trace, trace->size()); !replay)
            return std::unexpected(replay.error());
        const bool truncated = !trace->empty() &&
            trace->back().operation == "capture.truncated";
        if (enrich_sites(program, *trace)) {
            const auto temporary = fs::path(trace_path.wstring() + L".symbolized.tmp");
            const auto raw_copy = fs::path(trace_path.wstring() + L".raw");
            if (!fs::exists(temporary) && !fs::exists(raw_copy)) {
                {
                    std::ofstream stream(temporary, std::ios::binary);
                    if (stream) {
                        TraceWriter writer(stream);
                        for (const auto& event : *trace)
                            writer.record(event.operation, event.resource, event.value);
                    }
                }
                if (fs::exists(temporary) &&
                    !ReplaceFileW(trace_path.c_str(), temporary.c_str(),
                                  raw_copy.c_str(), 0, nullptr, nullptr)) {
                    fs::remove(temporary);
                }
            }
        }
        return CaptureResult{trace_path, exit_code, truncated};
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
#endif
}

} // namespace atlas
