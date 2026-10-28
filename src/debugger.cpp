#include "atlas/debugger.hpp"
#include "atlas/dbghelp_guard.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace atlas {
namespace {

std::string json_quote(std::string_view value) {
    std::string out = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '\"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) {
            out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

#ifdef _WIN32
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                       result.data(), count, nullptr, nullptr);
    return result;
}

std::string system_error(DWORD error) {
    return "Windows error " + std::to_string(error);
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

void resume_threads(std::vector<HANDLE>& threads) {
    for (HANDLE thread : threads) {
        ResumeThread(thread);
        CloseHandle(thread);
    }
    threads.clear();
}

bool suspend_other_threads(DWORD process_id, DWORD active_thread,
                           std::vector<HANDLE>& suspended) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.value, &entry)) return false;
    do {
        if (entry.th32OwnerProcessID != process_id ||
            entry.th32ThreadID == active_thread) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) { resume_threads(suspended); return false; }
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            resume_threads(suspended);
            return false;
        }
        suspended.push_back(thread);
    } while (Thread32Next(snapshot.value, &entry));
    return true;
}

struct Frame {
    std::uint64_t address = 0;
    std::string function;
    std::string file;
    std::uint32_t line = 0;
};

struct Variable {
    std::string name;
    std::string value;
    std::string storage;
};

struct Breakpoint {
    std::filesystem::path source;
    std::uint32_t requested_line = 0;
    std::uint32_t actual_line = 0;
    std::uint64_t address = 0;
    unsigned char original = 0;
    bool verified = false;
    std::string error;
};

struct Snapshot {
    std::string phase = "idle";
    std::string reason;
    std::string error;
    std::uint32_t pid = 0;
    std::uint32_t thread_id = 0;
    std::vector<Frame> frames;
    std::vector<Variable> variables;
    std::vector<Breakpoint> breakpoints;
};

std::optional<std::uint64_t> register_value(const CONTEXT& context, DWORD number) {
    switch (number) {
    case 328: return context.Rax;
    case 329: return context.Rbx;
    case 330: return context.Rcx;
    case 331: return context.Rdx;
    case 332: return context.Rsi;
    case 333: return context.Rdi;
    case 334: return context.Rbp;
    case 335: return context.Rsp;
    case 336: return context.R8;
    case 337: return context.R9;
    case 338: return context.R10;
    case 339: return context.R11;
    case 340: return context.R12;
    case 341: return context.R13;
    case 342: return context.R14;
    case 343: return context.R15;
    default: return std::nullopt;
    }
}

Frame resolve_frame(HANDLE process, std::uint64_t address) {
    Frame frame;
    frame.address = address;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol))
        frame.function.assign(symbol->Name, symbol->NameLen);
    IMAGEHLP_LINE64 source{};
    source.SizeOfStruct = sizeof(source);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddr64(process, address, &line_displacement, &source)) {
        if (source.FileName) frame.file = source.FileName;
        frame.line = source.LineNumber;
    }
    return frame;
}

struct LocalContext {
    HANDLE process;
    const CONTEXT* registers;
    std::vector<Variable>* output;
};

BOOL CALLBACK collect_local(PSYMBOL_INFO symbol, ULONG, PVOID raw) {
    auto& local = *static_cast<LocalContext*>(raw);
    if (!(symbol->Flags & (SYMFLAG_LOCAL | SYMFLAG_PARAMETER)) ||
        local.output->size() >= 32) return TRUE;
    Variable entry;
    entry.name.assign(symbol->Name, symbol->NameLen);
    std::uint64_t address = symbol->Address;
    if (symbol->Flags & SYMFLAG_REGISTER) {
        entry.storage = "register";
        if (const auto value = register_value(*local.registers, symbol->Register); value) {
            entry.value = std::to_string(*value);
        } else entry.value = "unavailable";
    } else {
        if (symbol->Flags & SYMFLAG_REGREL) {
            const auto base = register_value(*local.registers, symbol->Register);
            if (!base) { entry.value = "unavailable"; entry.storage = "register-relative"; }
            else address = *base + static_cast<std::int64_t>(symbol->Address);
            entry.storage = "register-relative";
        } else if (symbol->Flags & SYMFLAG_FRAMEREL) {
            address = local.registers->Rbp + static_cast<std::int64_t>(symbol->Address);
            entry.storage = "frame-relative";
        } else entry.storage = "memory";
        if (entry.value.empty()) {
            const SIZE_T size = std::min<SIZE_T>(symbol->Size, sizeof(std::uint64_t));
            std::uint64_t value = 0;
            SIZE_T read = 0;
            if (size && ReadProcessMemory(local.process,
                    reinterpret_cast<LPCVOID>(address), &value, size, &read) && read == size)
                entry.value = std::to_string(value);
            else entry.value = "unavailable";
        }
    }
    local.output->push_back(std::move(entry));
    return TRUE;
}
#endif

} // namespace

struct NativeDebugger::Impl {
#ifdef _WIN32
    struct Command {
        enum Kind { breakpoint, resume, step } kind;
        std::filesystem::path path;
        std::uint32_t line = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Command> commands;
    Snapshot snapshot;
    std::jthread worker;
    std::filesystem::path executable;

    void publish(const Snapshot& state) {
        std::lock_guard lock(mutex);
        snapshot = state;
    }

    static bool byte_at(HANDLE process, std::uint64_t address, unsigned char& byte) {
        SIZE_T read = 0;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                                 &byte, 1, &read) && read == 1;
    }

    static bool write_byte(HANDLE process, std::uint64_t address, unsigned char byte) {
        DWORD old = 0;
        auto* location = reinterpret_cast<void*>(address);
        if (!VirtualProtectEx(process, location, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
        SIZE_T written = 0;
        const bool ok = WriteProcessMemory(process, location, &byte, 1, &written) &&
                        written == 1 && FlushInstructionCache(process, location, 1);
        DWORD ignored = 0;
        VirtualProtectEx(process, location, 1, old, &ignored);
        return ok;
    }

    static void inspect(HANDLE process, DWORD thread_id, Snapshot& state) {
        state.thread_id = thread_id;
        state.frames.clear();
        state.variables.clear();
        Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                 THREAD_QUERY_INFORMATION, FALSE, thread_id));
        if (!thread.value) return;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread.value, &context)) return;
        state.frames.push_back(resolve_frame(process, context.Rip));
        STACKFRAME64 stack{};
        stack.AddrPC.Offset = context.Rip;
        stack.AddrFrame.Offset = context.Rbp;
        stack.AddrStack.Offset = context.Rsp;
        stack.AddrPC.Mode = stack.AddrFrame.Mode = stack.AddrStack.Mode = AddrModeFlat;
        CONTEXT unwind = context;
        for (int i = 0; i < 11; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread.value, &stack,
                             &unwind, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) || !stack.AddrPC.Offset) break;
            if (stack.AddrPC.Offset != state.frames.back().address)
                state.frames.push_back(resolve_frame(process, stack.AddrPC.Offset));
        }
        state.variables.push_back({"$rip", std::to_string(context.Rip), "register"});
        state.variables.push_back({"$rsp", std::to_string(context.Rsp), "register"});
        state.variables.push_back({"$rbp", std::to_string(context.Rbp), "register"});
        IMAGEHLP_STACK_FRAME scope{};
        scope.InstructionOffset = context.Rip;
        if (SymSetContext(process, &scope, nullptr)) {
            LocalContext locals{process, &context, &state.variables};
            SymEnumSymbols(process, 0, nullptr, collect_local, &locals);
        }
    }

    void run(std::stop_token stop) {
        Snapshot state;
        state.phase = "launching";
        publish(state);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        const auto command = L"\"" + executable.wstring() + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        const auto working_directory = executable.parent_path().wstring();
        if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                            FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW,
                            nullptr, working_directory.c_str(), &startup, &info)) {
            state.phase = "error";
            state.error = system_error(GetLastError());
            publish(state);
            return;
        }
        Handle process(info.hProcess), initial_thread(info.hThread);
        state.pid = info.dwProcessId;
        state.phase = "running";
        publish(state);
        DebugSetProcessKillOnExit(TRUE);
        std::lock_guard symbols_lock(dbghelp_mutex());
        const DWORD previous_symbol_options = SymGetOptions();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        const bool symbols_ready = SymInitializeW(process.value,
            working_directory.c_str(), FALSE) != FALSE;
        if (!symbols_ready) state.error = "PDB symbols unavailable: " + system_error(GetLastError());
        bool initial_break = false;
        bool paused = false;
        bool stepping = false;
        bool rearm_step = false;
        std::uint64_t rearm_address = 0;
        DWORD rearm_thread_id = 0;
        std::vector<HANDLE> suspended_threads;
        std::string step_file;
        std::uint32_t step_line = 0;
        std::uint32_t step_count = 0;
        DWORD pending_status = DBG_CONTINUE;
        DEBUG_EVENT event{};
        bool has_event = false;
        while (!stop.stop_requested()) {
            if (!has_event) {
                if (!WaitForDebugEventEx(&event, 100)) {
                    if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
                    state.phase = "error";
                    state.error = "WaitForDebugEventEx: " + system_error(GetLastError());
                    publish(state);
                    break;
                }
                has_event = true;
            }
            if (paused) {
                Command command_item;
                bool found = false;
                {
                    std::unique_lock lock(mutex);
                    condition.wait_for(lock, std::chrono::milliseconds(100),
                                       [&] { return !commands.empty() || stop.stop_requested(); });
                    if (!commands.empty()) {
                        command_item = std::move(commands.front());
                        commands.pop_front();
                        found = true;
                    }
                }
                if (!found) continue;
                if (command_item.kind == Command::breakpoint) {
                    Breakpoint bp;
                    bp.source = command_item.path;
                    bp.requested_line = command_item.line;
                    IMAGEHLP_LINE64 line{};
                    line.SizeOfStruct = sizeof(line);
                    LONG displacement = 0;
                    auto filename = utf8(command_item.path.wstring());
                    auto basename = utf8(command_item.path.filename().wstring());
                    bool resolved = symbols_ready && (SymGetLineFromName64(process.value,
                        nullptr, filename.data(), bp.requested_line, &displacement, &line) ||
                        SymGetLineFromName64(process.value, nullptr, basename.data(),
                            bp.requested_line, &displacement, &line));
                    if (!resolved) bp.error = "source line not found in loaded PDB";
                    else {
                        bp.address = line.Address;
                        bp.actual_line = line.LineNumber;
                        std::error_code source_error;
                        if (line.FileName &&
                            std::filesystem::is_regular_file(line.FileName, source_error) &&
                            !std::filesystem::equivalent(bp.source, line.FileName,
                                                         source_error)) {
                            bp.error = "PDB resolved a different source file";
                            state.breakpoints.push_back(std::move(bp));
                            publish(state);
                            continue;
                        }
                        const auto duplicate = std::find_if(state.breakpoints.begin(),
                            state.breakpoints.end(), [&](const Breakpoint& other) {
                                return other.verified && other.address == bp.address;
                            });
                        if (duplicate != state.breakpoints.end()) {
                            bp.error = "breakpoint already exists at this address";
                        } else if (!byte_at(process.value, bp.address, bp.original) ||
                                   !write_byte(process.value, bp.address, 0xcc)) {
                            bp.error = "cannot patch the breakpoint address";
                        } else bp.verified = true;
                    }
                    state.breakpoints.push_back(std::move(bp));
                    publish(state);
                    continue;
                }
                stepping = command_item.kind == Command::step;
                rearm_step = stepping;
                step_count = 0;
                step_file = state.frames.empty() ? "" : state.frames.front().file;
                step_line = state.frames.empty() ? 0 : state.frames.front().line;
                if (stepping || rearm_address) {
                    if (rearm_address && event.dwThreadId != rearm_thread_id) {
                        state.phase = "error";
                        state.error = "breakpoint rearm thread changed unexpectedly";
                        publish(state);
                        break;
                    }
                    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                             FALSE, event.dwThreadId));
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    if (!thread.value || !GetThreadContext(thread.value, &context)) {
                        state.phase = "error";
                        state.error = "cannot inspect paused thread";
                        publish(state);
                        break;
                    }
                    context.EFlags |= 0x100U;
                    if (!SetThreadContext(thread.value, &context)) {
                        state.phase = "error";
                        state.error = "cannot single-step paused thread";
                        publish(state);
                        break;
                    }
                }
                if (rearm_address && !suspend_other_threads(
                        event.dwProcessId, rearm_thread_id, suspended_threads)) {
                    state.phase = "error";
                    state.error = "cannot suspend other threads for breakpoint rearm";
                    publish(state);
                    break;
                }
                state.phase = "running";
                state.reason.clear();
                state.frames.clear();
                state.variables.clear();
                publish(state);
                paused = false;
                if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                                        pending_status)) {
                    state.phase = "error";
                    state.error = "cannot resume paused debug event: " +
                                  system_error(GetLastError());
                    publish(state);
                    break;
                }
                has_event = false;
                continue;
            }
            DWORD decision = DBG_CONTINUE;
            switch (event.dwDebugEventCode) {
            case CREATE_THREAD_DEBUG_EVENT:
                if (rearm_address && event.dwThreadId != rearm_thread_id) {
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                               event.dwThreadId);
                    if (!thread || SuspendThread(thread) == static_cast<DWORD>(-1)) {
                        if (thread) CloseHandle(thread);
                        state.phase = "error";
                        state.error = "cannot suspend new thread during breakpoint rearm";
                    } else suspended_threads.push_back(thread);
                }
                if (event.u.CreateThread.hThread)
                    CloseHandle(event.u.CreateThread.hThread);
                break;
            case CREATE_PROCESS_DEBUG_EVENT:
                if (symbols_ready) SymLoadModuleExW(process.value,
                    event.u.CreateProcessInfo.hFile, executable.c_str(), nullptr,
                    reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage),
                    0, nullptr, 0);
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (symbols_ready) SymLoadModuleExW(process.value,
                    event.u.LoadDll.hFile, nullptr, nullptr,
                    reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll),
                    0, nullptr, 0);
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT: {
                const auto code = event.u.Exception.ExceptionRecord.ExceptionCode;
                if (code == EXCEPTION_BREAKPOINT) {
                    const auto address = reinterpret_cast<std::uint64_t>(
                        event.u.Exception.ExceptionRecord.ExceptionAddress);
                    auto bp = std::find_if(state.breakpoints.begin(), state.breakpoints.end(),
                        [&](const Breakpoint& item) { return item.verified && item.address == address; });
                    if (bp != state.breakpoints.end()) {
                        if (rearm_address) {
                            // A second thread may have executed INT3 before the first
                            // breakpoint event was delivered. Defer it until the active
                            // thread executes the restored instruction and re-arms INT3.
                            Handle thread(OpenThread(THREAD_GET_CONTEXT |
                                                     THREAD_SET_CONTEXT |
                                                     THREAD_SUSPEND_RESUME,
                                                     FALSE, event.dwThreadId));
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_CONTROL;
                            if (!thread.value || !GetThreadContext(thread.value, &context)) {
                                state.phase = "error";
                                state.error = "cannot inspect concurrent breakpoint thread";
                                break;
                            }
                            context.Rip = address;
                            context.EFlags &= ~0x100U;
                            if (!SetThreadContext(thread.value, &context)) {
                                state.phase = "error";
                                state.error = "cannot rewind concurrent breakpoint thread";
                                break;
                            }
                            const bool already_suspended = std::ranges::any_of(
                                suspended_threads, [&](HANDLE other) {
                                    return GetThreadId(other) == event.dwThreadId;
                                });
                            if (!already_suspended) {
                                if (SuspendThread(thread.value) == static_cast<DWORD>(-1)) {
                                    state.phase = "error";
                                    state.error = "cannot defer concurrent breakpoint thread";
                                    break;
                                }
                                suspended_threads.push_back(thread.value);
                                thread.value = nullptr;
                            }
                            break;
                        }
                        if (!write_byte(process.value, bp->address, bp->original)) {
                            state.phase = "error";
                            state.error = "cannot restore instruction at breakpoint";
                            break;
                        }
                        Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                 FALSE, event.dwThreadId));
                        CONTEXT context{};
                        context.ContextFlags = CONTEXT_CONTROL;
                        if (!thread.value || !GetThreadContext(thread.value, &context)) {
                            state.phase = "error";
                            state.error = "cannot read thread context at breakpoint";
                            break;
                        }
                        context.Rip = address;
                        context.EFlags &= ~0x100U;
                        if (!SetThreadContext(thread.value, &context)) {
                            state.phase = "error";
                            state.error = "cannot rewind instruction pointer at breakpoint";
                            break;
                        }
                        rearm_address = address;
                        rearm_thread_id = event.dwThreadId;
                        state.reason = "breakpoint";
                        paused = true;
                    } else if (!initial_break) {
                        initial_break = true;
                        state.reason = "entry";
                        paused = true;
                    }
                } else if (code == EXCEPTION_SINGLE_STEP) {
                    if (rearm_address) {
                        if (event.dwThreadId != rearm_thread_id) {
                            state.phase = "error";
                            state.error = "unexpected thread single-stepped during breakpoint rearm";
                            break;
                        }
                        if (!write_byte(process.value, rearm_address, 0xcc)) {
                            state.phase = "error";
                            state.error = "cannot re-arm breakpoint";
                            break;
                        }
                        rearm_address = 0;
                        rearm_thread_id = 0;
                        resume_threads(suspended_threads);
                        if (!rearm_step && !stepping) {
                            Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                     FALSE, event.dwThreadId));
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_CONTROL;
                            if (!thread.value || !GetThreadContext(thread.value, &context)) {
                                state.phase = "error";
                                state.error = "cannot clear trap flag after breakpoint rearm";
                                break;
                            }
                            context.EFlags &= ~0x100U;
                            if (!SetThreadContext(thread.value, &context)) {
                                state.phase = "error";
                                state.error = "cannot update trap flag after breakpoint rearm";
                            }
                            break;
                        }
                    }
                    if (stepping) {
                        ++step_count;
                        const auto location = resolve_frame(process.value,
                            reinterpret_cast<std::uint64_t>(
                                event.u.Exception.ExceptionRecord.ExceptionAddress));
                        if (step_count >= 50000 ||
                            (location.line && (location.line != step_line ||
                                               location.file != step_file))) {
                            state.reason = step_count >= 50000 ? "step limit" : "step";
                            stepping = false;
                            paused = true;
                            Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                     FALSE, event.dwThreadId));
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_CONTROL;
                            if (thread.value && GetThreadContext(thread.value, &context)) {
                                context.EFlags &= ~0x100U;
                                SetThreadContext(thread.value, &context);
                            }
                        } else {
                            Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                                     FALSE, event.dwThreadId));
                            CONTEXT context{};
                            context.ContextFlags = CONTEXT_CONTROL;
                            if (thread.value && GetThreadContext(thread.value, &context)) {
                                context.EFlags |= 0x100U;
                                SetThreadContext(thread.value, &context);
                            }
                        }
                    }
                } else {
                    decision = DBG_EXCEPTION_NOT_HANDLED;
                    if (!event.u.Exception.dwFirstChance) {
                        state.reason = "exception " + std::to_string(code);
                        paused = true;
                    }
                }
                if (paused) {
                    pending_status = decision;
                    state.phase = "paused";
                    inspect(process.value, event.dwThreadId, state);
                    publish(state);
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                state.phase = "exited";
                state.reason = "exit code " + std::to_string(event.u.ExitProcess.dwExitCode);
                state.frames.clear();
                state.variables.clear();
                publish(state);
                break;
            default: break;
            }
            if (state.phase == "error") { publish(state); break; }
            if (paused) continue;
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, decision)) {
                state.phase = "error";
                state.error = "cannot continue debug event: " +
                              system_error(GetLastError());
                publish(state);
                break;
            }
            has_event = false;
            if (state.phase == "exited") break;
        }
        resume_threads(suspended_threads);
        if (state.phase != "exited" && (stop.stop_requested() || state.phase == "error")) {
            TerminateProcess(process.value, 0);
            if (has_event) ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            if (stop.stop_requested()) {
                state.phase = "stopped";
                state.reason = "stopped by Atlas";
            }
            state.frames.clear();
            state.variables.clear();
            publish(state);
        }
        if (symbols_ready) SymCleanup(process.value);
        SymSetOptions(previous_symbol_options);
    }
#else
    std::string phase = "unavailable";
#endif
};

NativeDebugger::NativeDebugger() : impl_(std::make_unique<Impl>()) {}
NativeDebugger::~NativeDebugger() {
#ifdef _WIN32
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->condition.notify_all();
        impl_->worker.join();
    }
#endif
}

bool NativeDebugger::launch(const std::filesystem::path& executable, std::string& error) {
#ifdef _WIN32
#if !defined(_M_X64) && !defined(__x86_64__)
    error = "the embedded debugger currently supports x64 Windows targets";
    return false;
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executable, ec) || executable.extension() != L".exe") {
        error = "choose an existing .exe file";
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        const auto& phase = impl_->snapshot.phase;
        if (phase == "launching" || phase == "running" || phase == "paused") {
            error = "a debug session is already active";
            return false;
        }
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->executable = std::filesystem::absolute(executable);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->commands.clear();
        impl_->snapshot = Snapshot{};
        impl_->snapshot.phase = "launching";
    }
    impl_->worker = std::jthread([this](std::stop_token stop) { impl_->run(stop); });
    return true;
#endif
#else
    (void)executable;
    error = "the embedded debugger currently requires Windows";
    return false;
#endif
}

bool NativeDebugger::breakpoint(const std::filesystem::path& source,
                                std::uint32_t line, std::string& error) {
#ifdef _WIN32
    std::error_code ec;
    if (!line || !std::filesystem::is_regular_file(source, ec)) {
        error = "choose an existing source file and positive line number";
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->snapshot.phase != "paused") {
        error = "pause the debuggee before adding a breakpoint";
        return false;
    }
    if (std::any_of(impl_->commands.begin(), impl_->commands.end(),
        [](const Impl::Command& command) {
            return command.kind != Impl::Command::breakpoint;
        })) {
        error = "a resume command is already queued";
        return false;
    }
    impl_->commands.push_back({Impl::Command::breakpoint,
        std::filesystem::absolute(source), line});
    impl_->condition.notify_one();
    return true;
#else
    (void)source; (void)line;
    error = "the embedded debugger currently requires Windows";
    return false;
#endif
}

bool NativeDebugger::resume(bool single_step, std::string& error) {
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    if (impl_->snapshot.phase != "paused") {
        error = "the debuggee is not paused";
        return false;
    }
    if (std::any_of(impl_->commands.begin(), impl_->commands.end(),
        [](const Impl::Command& command) {
            return command.kind != Impl::Command::breakpoint;
        })) {
        error = "a resume command is already queued";
        return false;
    }
    impl_->commands.push_back({single_step ? Impl::Command::step : Impl::Command::resume, {}, 0});
    impl_->condition.notify_one();
    return true;
#else
    (void)single_step;
    error = "the embedded debugger currently requires Windows";
    return false;
#endif
}

bool NativeDebugger::stop(std::string& error) {
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    if (impl_->snapshot.phase != "running" && impl_->snapshot.phase != "paused" &&
        impl_->snapshot.phase != "launching") {
        error = "no active debug session";
        return false;
    }
    impl_->worker.request_stop();
    impl_->condition.notify_all();
    return true;
#else
    error = "the embedded debugger currently requires Windows";
    return false;
#endif
}

bool NativeDebugger::active() const {
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    const auto& phase = impl_->snapshot.phase;
    return phase == "launching" || phase == "running" || phase == "paused";
#else
    return false;
#endif
}

std::string NativeDebugger::snapshot_json() const {
#ifdef _WIN32
    Snapshot state;
    {
        std::lock_guard lock(impl_->mutex);
        state = impl_->snapshot;
    }
    std::ostringstream out;
    out << "{\"phase\":" << json_quote(state.phase)
        << ",\"reason\":" << json_quote(state.reason)
        << ",\"error\":" << json_quote(state.error)
        << ",\"pid\":" << state.pid
        << ",\"thread_id\":" << state.thread_id
        << ",\"frames\":[";
    for (std::size_t i = 0; i < state.frames.size(); ++i) {
        const auto& frame = state.frames[i];
        if (i) out << ',';
        out << "{\"address\":" << frame.address
            << ",\"function\":" << json_quote(frame.function)
            << ",\"file\":" << json_quote(frame.file)
            << ",\"line\":" << frame.line << '}';
    }
    out << "],\"variables\":[";
    for (std::size_t i = 0; i < state.variables.size(); ++i) {
        const auto& variable = state.variables[i];
        if (i) out << ',';
        out << "{\"name\":" << json_quote(variable.name)
            << ",\"value\":" << json_quote(variable.value)
            << ",\"storage\":" << json_quote(variable.storage) << '}';
    }
    out << "],\"breakpoints\":[";
    for (std::size_t i = 0; i < state.breakpoints.size(); ++i) {
        const auto& bp = state.breakpoints[i];
        if (i) out << ',';
        out << "{\"source\":" << json_quote(utf8(bp.source.wstring()))
            << ",\"requested_line\":" << bp.requested_line
            << ",\"actual_line\":" << bp.actual_line
            << ",\"address\":" << bp.address
            << ",\"verified\":" << (bp.verified ? "true" : "false")
            << ",\"error\":" << json_quote(bp.error) << '}';
    }
    out << "]}";
    return out.str();
#else
    return "{\"phase\":\"unavailable\"}";
#endif
}

std::string NativeDebugger::summary() const {
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    const auto& state = impl_->snapshot;
    if (state.phase == "paused" && !state.frames.empty()) {
        const auto& frame = state.frames.front();
        return "Debug paused (" + state.reason + ") at " + frame.file + ':' +
               std::to_string(frame.line);
    }
    return "Debug " + state.phase + (state.error.empty() ? "" : ": " + state.error);
#else
    return "Embedded debugger unavailable";
#endif
}

std::string NativeDebugger::snapshot_text() const {
#ifdef _WIN32
    Snapshot state;
    {
        std::lock_guard lock(impl_->mutex);
        state = impl_->snapshot;
    }
    std::ostringstream out;
    out << "State: " << state.phase;
    if (!state.reason.empty()) out << " (" << state.reason << ')';
    out << "\r\nProcess: " << state.pid << "   Thread: " << state.thread_id;
    if (!state.error.empty()) out << "\r\nError: " << state.error;
    out << "\r\n\r\nCALL STACK\r\n";
    if (state.frames.empty()) out << "No paused stack\r\n";
    for (std::size_t i = 0; i < state.frames.size(); ++i) {
        const auto& frame = state.frames[i];
        out << '#' << i << "  " << (frame.function.empty() ? "<unknown>" : frame.function)
            << "  " << frame.file << ':' << frame.line << "\r\n";
    }
    out << "\r\nVARIABLES (raw unsigned values for simple scalar locals)\r\n";
    if (state.variables.empty()) out << "Available while paused\r\n";
    for (const auto& variable : state.variables)
        out << variable.name << " = " << variable.value << " [" << variable.storage << "]\r\n";
    out << "\r\nBREAKPOINTS\r\n";
    if (state.breakpoints.empty()) out << "None\r\n";
    for (const auto& bp : state.breakpoints)
        out << utf8(bp.source.wstring()) << ':' << bp.requested_line << " -> "
            << (bp.verified ? "verified" : bp.error) << " (line " << bp.actual_line << ")\r\n";
    return out.str();
#else
    return "Embedded debugger currently requires Windows";
#endif
}

} // namespace atlas
