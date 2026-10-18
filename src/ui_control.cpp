#include "atlas/ui_control.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace atlas {
namespace {

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20U) {
                result += "\\u00";
                result += hex[c >> 4U];
                result += hex[c & 15U];
            } else result += static_cast<char>(c);
        }
    }
    return result + '"';
}

#ifdef _WIN32

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring pipe_name(std::uint32_t pid) {
    return L"\\\\.\\pipe\\ProjectAtlas.UI." + std::to_wstring(pid);
}

template <std::size_t N>
std::wstring_view bounded(const wchar_t (&text)[N]) {
    const auto end = std::find(std::begin(text), std::end(text), L'\0');
    return {text, static_cast<std::size_t>(end - std::begin(text))};
}

template <std::size_t N>
bool copy_text(wchar_t (&target)[N], std::wstring_view value) {
    if (value.size() >= N) return false;
    std::copy(value.begin(), value.end(), target);
    target[value.size()] = L'\0';
    return true;
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::vector<std::byte> token_user(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) return {};
    std::vector<std::byte> result(bytes);
    if (!GetTokenInformation(token.value, TokenUser, result.data(), bytes, &bytes))
        return {};
    return result;
}

bool same_user(HANDLE pipe) {
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid)) return false;
    Handle client(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, client_pid));
    if (client.value == INVALID_HANDLE_VALUE || !client.value) return false;
    const auto self = token_user(GetCurrentProcess());
    const auto other = token_user(client.value);
    if (self.empty() || other.empty()) return false;
    const auto* a = reinterpret_cast<const TOKEN_USER*>(self.data());
    const auto* b = reinterpret_cast<const TOKEN_USER*>(other.data());
    return EqualSid(a->User.Sid, b->User.Sid) != FALSE;
}

bool overlapped_operation(HANDLE pipe, std::stop_token stop,
                          const std::function<BOOL(OVERLAPPED*, DWORD*)>& start,
                          DWORD& transferred) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value || event.value == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    if (start(&operation, &transferred)) return true;
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const DWORD waited = WaitForSingleObject(event.value, 100);
        if (waited == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
        if (waited != WAIT_TIMEOUT) break;
    }
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(event.value, INFINITE);
    GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    return false;
}

bool connect_pipe(HANDLE pipe, std::stop_token stop) {
    DWORD ignored = 0;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value || event.value == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    while (!stop.stop_requested()) {
        const DWORD waited = WaitForSingleObject(event.value, 100);
        if (waited == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &operation, &ignored, FALSE) != FALSE;
        if (waited != WAIT_TIMEOUT) break;
    }
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(event.value, INFINITE);
    GetOverlappedResult(pipe, &operation, &ignored, FALSE);
    return false;
}

BOOL CALLBACK collect_window(HWND window, LPARAM data) {
    wchar_t name[128]{};
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
        std::wstring_view(name) != L"ProjectAtlasNativeWindow") return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid) reinterpret_cast<std::vector<std::uint32_t>*>(data)->push_back(pid);
    return TRUE;
}

#endif

} // namespace

std::vector<std::uint32_t> running_ui_instances() {
#ifdef _WIN32
    std::vector<std::uint32_t> pids;
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&pids));
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
#else
    return {};
#endif
}

std::expected<UiReply, std::string> send_ui_command(
    UiAction action, std::wstring_view path, std::uint64_t value,
    std::wstring_view second_path, std::optional<std::uint32_t> pid) {
#ifdef _WIN32
    const auto pids = running_ui_instances();
    if (pid && std::find(pids.begin(), pids.end(), *pid) == pids.end())
        return std::unexpected("Atlas GUI PID is not running");
    if (!pid && pids.empty()) return std::unexpected("Atlas GUI is not running");
    if (!pid && pids.size() != 1)
        return std::unexpected("multiple Atlas GUI instances; supply a PID from ui list");
    UiCommand command;
    command.action = action;
    command.value = value;
    if (!copy_text(command.path, path))
        return std::unexpected("UI path exceeds 4095 characters");
    if (!copy_text(command.second_path, second_path))
        return std::unexpected("second UI path exceeds 4095 characters");
    UiReply reply;
    DWORD received = 0;
    const auto name = pipe_name(pid.value_or(pids.front()));
    const DWORD timeout = action == UiAction::generate_cards ? 30000U : 3000U;
    if (!CallNamedPipeW(name.c_str(), &command, sizeof(command), &reply,
                        sizeof(reply), &received, timeout))
        return std::unexpected("Atlas GUI control channel unavailable (Windows error " +
                               std::to_string(GetLastError()) + ')');
    if (received != sizeof(reply) || reply.magic != ui_protocol_magic ||
        reply.version != ui_protocol_version)
        return std::unexpected("Atlas GUI returned an incompatible control response");
    return reply;
#else
    (void)action; (void)path; (void)value; (void)second_path; (void)pid;
    return std::unexpected("Atlas GUI control currently requires Windows");
#endif
}

void serve_ui_commands(std::stop_token stop, std::uint32_t pid,
                       const std::function<UiReply(const UiCommand&)>& handle) {
#ifdef _WIN32
    const auto name = pipe_name(pid);
    Handle pipe(CreateNamedPipeW(name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(UiReply), sizeof(UiCommand), 3000, nullptr));
    if (pipe.value == INVALID_HANDLE_VALUE) return;
    while (!stop.stop_requested()) {
        if (!connect_pipe(pipe.value, stop)) {
            DisconnectNamedPipe(pipe.value);
            continue;
        }
        if (stop.stop_requested()) break;
        UiCommand command;
        DWORD received = 0;
        const bool read = overlapped_operation(pipe.value, stop,
            [&](OVERLAPPED* operation, DWORD* count) {
                return ReadFile(pipe.value, &command, sizeof(command), count, operation);
            }, received);
        if (!read || received != sizeof(command) || stop.stop_requested()) {
            DisconnectNamedPipe(pipe.value);
            continue;
        }
        UiReply reply;
        reply.pid = pid;
        if (!same_user(pipe.value)) {
            copy_text(reply.error, L"client does not match the GUI user");
        } else if (command.magic != ui_protocol_magic ||
                   command.version != ui_protocol_version ||
                   std::find(std::begin(command.path), std::end(command.path), L'\0') ==
                       std::end(command.path) ||
                   std::find(std::begin(command.second_path), std::end(command.second_path), L'\0') ==
                       std::end(command.second_path)) {
            copy_text(reply.error, L"invalid UI control request");
        } else {
            try { reply = handle(command); }
            catch (...) { copy_text(reply.error, L"UI command failed unexpectedly"); }
        }
        DWORD written = 0;
        const bool sent = overlapped_operation(pipe.value, stop,
            [&](OVERLAPPED* operation, DWORD* count) {
                return WriteFile(pipe.value, &reply, sizeof(reply), count, operation);
            }, written);
        if (sent && written == sizeof(reply)) FlushFileBuffers(pipe.value);
        DisconnectNamedPipe(pipe.value);
    }
#else
    (void)stop; (void)pid; (void)handle;
#endif
}

std::string ui_reply_json(const UiReply& reply) {
#ifdef _WIN32
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.ui.v1\",\"operation\":\"ui\","
        << "\"ok\":" << (reply.ok ? "true" : "false")
        << ",\"pid\":" << reply.pid
        << ",\"active_tab\":" << reply.active_tab
        << ",\"scanning\":" << (reply.scanning ? "true" : "false")
        << ",\"recording\":" << (reply.recording ? "true" : "false")
        << ",\"trace_loaded\":" << (reply.trace_loaded ? "true" : "false")
        << ",\"window_visible\":" << (reply.window_visible ? "true" : "false")
        << ",\"uml_overview\":" << (reply.uml_overview ? "true" : "false")
        << ",\"uml_class_count\":" << reply.uml_class_count
        << ",\"uml_link_count\":" << reply.uml_link_count
        << ",\"uml_page\":" << reply.uml_page
        << ",\"step\":" << reply.step
        << ",\"total_steps\":" << reply.total_steps
        << ",\"node_count\":" << reply.node_count
        << ",\"link_count\":" << reply.link_count
        << ",\"project_root\":" << json_string(utf8(bounded(reply.project_root)))
        << ",\"trace_path\":" << json_string(utf8(bounded(reply.trace_path)))
        << ",\"selected_file\":" << json_string(utf8(bounded(reply.selected_file)))
        << ",\"selected_symbol\":" << json_string(utf8(bounded(reply.selected_symbol)))
        << ",\"status\":" << json_string(utf8(bounded(reply.status)))
        << ",\"error\":" << json_string(utf8(bounded(reply.error)))
        << ",\"debug\":";
    const auto debug = utf8(bounded(reply.debug_json));
    out << (debug.empty() ? "null" : debug) << '}';
    return out.str();
#else
    (void)reply;
    return "{}";
#endif
}

std::string ui_instances_json(const std::vector<std::uint32_t>& pids) {
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.ui.v1\",\"operation\":\"ui_list\",\"pids\":[";
    for (std::size_t i = 0; i < pids.size(); ++i) {
        if (i) out << ',';
        out << pids[i];
    }
    out << "]}";
    return out.str();
}

} // namespace atlas
