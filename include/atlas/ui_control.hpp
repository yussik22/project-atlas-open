#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

constexpr std::uint32_t ui_protocol_magic = 0x534c5441; // ATLS
constexpr std::uint32_t ui_protocol_version = 5;

enum class UiAction : std::uint32_t {
    status = 1,
    load_trace,
    set_step,
    select_tab,
    scan_project,
    record_run,
    focus_file,
    focus_symbol,
    compare_facts,
    generate_cards,
    debug_launch,
    debug_breakpoint,
    debug_continue,
    debug_step,
    debug_stop,
    show_window,
    uml_overview,
    uml_focus,
};

// Fixed-size, versioned local IPC packets. No pointers cross process boundaries.
struct UiCommand {
    std::uint32_t magic = ui_protocol_magic;
    std::uint32_t version = ui_protocol_version;
    UiAction action = UiAction::status;
    std::uint32_t reserved = 0;
    std::uint64_t value = 0;
    wchar_t path[4096]{};
    wchar_t second_path[4096]{};
};

struct UiReply {
    std::uint32_t magic = ui_protocol_magic;
    std::uint32_t version = ui_protocol_version;
    std::uint32_t ok = 0;
    std::uint32_t pid = 0;
    std::uint32_t active_tab = 0;
    std::uint32_t scanning = 0;
    std::uint32_t recording = 0;
    std::uint32_t trace_loaded = 0;
    std::uint32_t window_visible = 0;
    std::uint32_t uml_overview = 0;
    std::uint32_t uml_class_count = 0;
    std::uint32_t uml_link_count = 0;
    std::uint32_t uml_page = 0;
    std::uint64_t step = 0;
    std::uint64_t total_steps = 0;
    std::uint64_t node_count = 0;
    std::uint64_t link_count = 0;
    wchar_t project_root[4096]{};
    wchar_t trace_path[4096]{};
    wchar_t selected_file[4096]{};
    wchar_t selected_symbol[1024]{};
    wchar_t status[1024]{};
    wchar_t error[512]{};
    wchar_t debug_json[16384]{};
};

[[nodiscard]] std::vector<std::uint32_t> running_ui_instances();
[[nodiscard]] std::expected<UiReply, std::string> send_ui_command(
    UiAction action, std::wstring_view path = {}, std::uint64_t value = 0,
    std::wstring_view second_path = {},
    std::optional<std::uint32_t> pid = std::nullopt);
void serve_ui_commands(std::stop_token stop, std::uint32_t pid,
                       const std::function<UiReply(const UiCommand&)>& handle);
[[nodiscard]] std::string ui_reply_json(const UiReply& reply);
[[nodiscard]] std::string ui_instances_json(const std::vector<std::uint32_t>& pids);

} // namespace atlas
