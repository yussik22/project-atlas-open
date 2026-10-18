#include "atlas/atlas.hpp"
#include "atlas/runtime.hpp"
#include "atlas/capture.hpp"
#include "atlas/ui_control.hpp"
#include "atlas/debugger.hpp"
#include "atlas/uml.hpp"
#include "atlas/interactions.hpp"
#include "atlas/interaction_presentation.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr wchar_t main_class[] = L"ProjectAtlasNativeWindow";
constexpr UINT scan_done_message = WM_APP + 1;
constexpr UINT capture_done_message = WM_APP + 2;
constexpr UINT ui_command_message = WM_APP + 3;
int smoke_exit_code = 0;
constexpr int id_root = 100, id_browse = 101, id_scan = 102, id_cards = 103;
constexpr int id_search = 104, id_tree = 105, id_tabs = 106, id_open = 107;
constexpr int id_expected = 108, id_actual = 109, id_browse_expected = 110;
constexpr int id_browse_actual = 111, id_compare = 112, id_results = 113;
constexpr int id_runtime_path = 114, id_runtime_browse = 115, id_runtime_load = 116;
constexpr int id_runtime_prev = 117, id_runtime_next = 118, id_runtime_timeline = 119;
constexpr int id_runtime_canvas = 120;
constexpr int id_runtime_exe = 121, id_runtime_exe_browse = 122;
constexpr int id_runtime_record = 123;
constexpr int id_debug_exe = 124, id_debug_browse = 125, id_debug_launch = 126;
constexpr int id_debug_file = 127, id_debug_file_browse = 128, id_debug_line = 129;
constexpr int id_debug_break = 130, id_debug_continue = 131, id_debug_step = 132;
constexpr int id_debug_stop = 133;
constexpr int id_uml_overview = 134, id_uml_focus = 135;
constexpr int id_uml_prev = 136, id_uml_next = 137, id_diagram = 138;
constexpr int id_uml_all = 139;
constexpr int id_interaction_list = 140;
constexpr int id_interaction_filter = 141;

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return L"[invalid UTF-8]";
    std::wstring result(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), n);
    return result;
}

std::wstring path_text(const fs::path& path) { return path.wstring(); }

std::wstring window_text(HWND window) {
    const int n = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(window, result.data(), n + 1);
    result.resize(static_cast<std::size_t>(n));
    return result;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring read_source(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"Could not open source file.";
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 2 * 1024 * 1024) return L"Source exceeds the 2 MiB preview limit. Open it in your editor.";
    return wide(data);
}

void set_font(HWND window, HFONT font) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND control(HWND parent, const wchar_t* cls, const wchar_t* title, DWORD style,
             int id, DWORD extra = 0) {
    if (std::wstring_view(cls) != L"STATIC") style |= WS_TABSTOP;
    // Tab pages are sibling controls. Clip their paint to the controls above
    // them, otherwise a tab repaint can erase buttons and owner-drawn content.
    return CreateWindowExW(extra, cls, title,
                           style | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

std::optional<fs::path> pick_path(HWND owner, bool folder) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                result.emplace(name);
                CoTaskMemFree(name);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

struct TreeItem {
    std::size_t file = 0;
    std::optional<std::size_t> symbol;
};

struct ScanResult {
    std::expected<atlas::ProjectScan, std::string> value;
};

struct CaptureFinished {
    std::expected<atlas::CaptureResult, std::string> value;
};

struct UiDispatch {
    atlas::UiCommand command;
    atlas::UiReply reply;
    std::atomic_bool cancelled = false;
};

class App;
LRESULT CALLBACK uml_canvas_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

template <std::size_t N>
bool copy_ui_text(wchar_t (&target)[N], std::wstring_view value) {
    if (value.size() >= N) return false;
    std::copy(value.begin(), value.end(), target);
    target[value.size()] = L'\0';
    return true;
}

class App {
public:
    explicit App(HWND window) : window_(window) { create_controls(); }
    ~App() {
        if (diagram_) RemoveWindowSubclass(diagram_, uml_canvas_proc, 1);
        KillTimer(window_, 1);
        if (ui_server_.joinable()) ui_server_.request_stop();
        if (ui_server_.joinable()) ui_server_.join();
        if (capture_worker_.joinable()) capture_worker_.request_stop();
        if (capture_worker_.joinable()) capture_worker_.join();
        if (worker_.joinable()) worker_.join();
        if (font_) DeleteObject(font_);
        if (mono_font_) DeleteObject(mono_font_);
    }

    void layout();
    void command(int id, int notification);
    void notify(const NMHDR* header);
    void scan_finished(ScanResult* result);
    void capture_finished(CaptureFinished* result);
    void start_ui_server();
    atlas::UiReply handle_ui_command(const atlas::UiCommand& command);
    void draw_diagram(const DRAWITEMSTRUCT& item);
    void draw_uml_overview(const DRAWITEMSTRUCT& item);
    void diagram_clicked();
    LRESULT canvas_input(HWND canvas, UINT message, WPARAM wparam, LPARAM lparam);
    void draw_runtime(const DRAWITEMSTRUCT& item);
    void set_runtime_step(std::size_t step);
    void refresh_interactions();
    void show_interaction_detail(std::size_t edge_index);
    void refresh_debug_view();
    void open_project(const fs::path& root, bool smoke,
                      std::optional<std::pair<fs::path, fs::path>> facts = std::nullopt,
                      std::optional<fs::path> runtime = std::nullopt) {
        smoke_ = smoke;
        smoke_facts_ = std::move(facts);
        smoke_runtime_ = std::move(runtime);
        SetWindowTextW(root_, root.c_str());
        PostMessageW(window_, WM_COMMAND, MAKEWPARAM(id_scan, BN_CLICKED), 0);
    }
    void request_close() {
        if (!scanning_ && !recording_) { DestroyWindow(window_); return; }
        close_requested_ = true;
        if (capture_worker_.joinable()) capture_worker_.request_stop();
        EnableWindow(window_, FALSE);
        set_status(L"Finishing the active operation before closing…");
    }
    void set_status(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }
    void resize_min(MINMAXINFO* info) const {
        info->ptMinTrackSize = POINT{850, 580};
    }

private:
    HWND window_ = nullptr;
    HWND root_ = nullptr, browse_ = nullptr, scan_ = nullptr, cards_ = nullptr;
    HWND search_ = nullptr, tree_ = nullptr, tabs_ = nullptr, open_ = nullptr;
    HWND detail_ = nullptr, diagram_ = nullptr, expected_ = nullptr, actual_ = nullptr;
    HWND uml_overview_button_ = nullptr, uml_focus_button_ = nullptr;
    HWND uml_prev_ = nullptr, uml_next_ = nullptr, uml_all_ = nullptr;
    HWND browse_expected_ = nullptr, browse_actual_ = nullptr, compare_ = nullptr;
    HWND results_ = nullptr, status_ = nullptr;
    HWND runtime_path_ = nullptr, runtime_browse_ = nullptr, runtime_load_ = nullptr;
    HWND runtime_exe_ = nullptr, runtime_exe_browse_ = nullptr, runtime_record_ = nullptr;
    HWND runtime_prev_ = nullptr, runtime_next_ = nullptr, runtime_timeline_ = nullptr;
    HWND runtime_step_label_ = nullptr, runtime_canvas_ = nullptr;
    HWND debug_exe_ = nullptr, debug_browse_ = nullptr, debug_launch_ = nullptr;
    HWND debug_file_ = nullptr, debug_file_browse_ = nullptr, debug_line_ = nullptr;
    HWND debug_break_ = nullptr, debug_continue_ = nullptr, debug_step_ = nullptr;
    HWND debug_stop_ = nullptr, debug_output_ = nullptr;
    HWND interaction_summary_ = nullptr, interaction_list_ = nullptr;
    HWND interaction_detail_ = nullptr, interaction_filter_ = nullptr;
    HFONT font_ = nullptr, mono_font_ = nullptr;
    std::jthread worker_;
    std::jthread capture_worker_;
    std::jthread ui_server_;
    std::mutex ui_dispatch_mutex_;
    std::vector<std::shared_ptr<UiDispatch>> ui_dispatches_;
    std::optional<atlas::ProjectScan> project_;
    std::optional<atlas::UmlGraph> uml_graph_;
    std::optional<atlas::InteractionGraph> interaction_graph_;
    std::vector<std::size_t> interaction_row_edges_;
    std::vector<std::pair<RECT, TreeItem>> uml_hits_;
    bool uml_overview_ = true;
    std::size_t uml_page_ = 0;
    bool uml_include_isolated_ = false;
    bool uml_fit_pending_ = true;
    double uml_scale_ = 1.0;
    double uml_offset_x_ = 0.0, uml_offset_y_ = 0.0;
    RECT uml_minimap_{};
    double uml_minimap_scale_ = 0.0;
    bool uml_dragging_ = false;
    bool uml_moved_ = false;
    POINT uml_last_mouse_{};
    std::vector<std::unique_ptr<TreeItem>> tree_items_;
    std::optional<TreeItem> selected_;
    bool rebuilding_tree_ = false;
    int active_tab_ = 0;
    bool scanning_ = false;
    bool recording_ = false;
    bool smoke_ = false;
    bool close_requested_ = false;
    std::optional<std::pair<fs::path, fs::path>> smoke_facts_;
    std::optional<fs::path> smoke_runtime_;
    std::optional<atlas::Trace> runtime_trace_;
    std::optional<atlas::RuntimeSnapshot> runtime_snapshot_;
    atlas::NativeDebugger debugger_;
    std::wstring debug_rendered_;
    std::size_t runtime_step_ = 0;

    void create_controls();
    void start_scan();
    void rebuild_tree();
    void select_tree_item(HTREEITEM item);
    bool focus_tree_item(const TreeItem& target);
    void update_detail();
    void update_tab();
    void compare_facts();
    void load_runtime();
    void install_runtime_trace(atlas::Trace trace);
    void record_runtime();
    void generate_cards();
    HTREEITEM add_tree(HTREEITEM parent, const std::wstring& title,
                       std::optional<TreeItem> data = std::nullopt);
    void show_error(const std::wstring& title, const std::wstring& message) {
        if (smoke_) {
            set_status(title + L": " + message);
            smoke_exit_code = 7;
            return;
        }
        MessageBoxW(window_, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
    }
};

void App::create_controls() {
    const UINT dpi = GetDpiForWindow(window_);
    font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Segoe UI");
    mono_font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH, L"Consolas");
    root_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id_root, WS_EX_CLIENTEDGE);
    browse_ = control(window_, L"BUTTON", L"Choose project…", BS_PUSHBUTTON, id_browse);
    scan_ = control(window_, L"BUTTON", L"Scan", BS_DEFPUSHBUTTON, id_scan);
    cards_ = control(window_, L"BUTTON", L"Generate .md cards", BS_PUSHBUTTON, id_cards);
    search_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id_search, WS_EX_CLIENTEDGE);
    tree_ = control(window_, WC_TREEVIEWW, L"", WS_BORDER | TVS_HASBUTTONS |
                    TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, id_tree, WS_EX_CLIENTEDGE);
    tabs_ = control(window_, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, id_tabs);
    open_ = control(window_, L"BUTTON", L"Open in editor", BS_PUSHBUTTON, id_open);
    detail_ = control(window_, L"EDIT", L"Choose a C++ project, then Scan.",
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                      WS_VSCROLL | WS_HSCROLL, 0, WS_EX_CLIENTEDGE);
    diagram_ = control(window_, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY |
                       WS_BORDER, id_diagram);
    SetWindowSubclass(diagram_, uml_canvas_proc, 1,
                      reinterpret_cast<DWORD_PTR>(this));
    uml_overview_button_ = control(window_, L"BUTTON", L"Map / reset",
                                   BS_PUSHBUTTON, id_uml_overview);
    uml_focus_button_ = control(window_, L"BUTTON", L"Selected class",
                                BS_PUSHBUTTON, id_uml_focus);
    uml_prev_ = control(window_, L"BUTTON", L"Zoom −",
                        BS_PUSHBUTTON, id_uml_prev);
    uml_next_ = control(window_, L"BUTTON", L"Zoom +",
                        BS_PUSHBUTTON, id_uml_next);
    uml_all_ = control(window_, L"BUTTON", L"Show all classes",
                       BS_PUSHBUTTON, id_uml_all);
    expected_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id_expected, WS_EX_CLIENTEDGE);
    actual_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id_actual, WS_EX_CLIENTEDGE);
    browse_expected_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON, id_browse_expected);
    browse_actual_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON, id_browse_actual);
    compare_ = control(window_, L"BUTTON", L"Compare facts", BS_PUSHBUTTON, id_compare);
    results_ = control(window_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
                       LVS_SHOWSELALWAYS, id_results, WS_EX_CLIENTEDGE);
    runtime_exe_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                           id_runtime_exe, WS_EX_CLIENTEDGE);
    runtime_exe_browse_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON,
                                  id_runtime_exe_browse);
    runtime_record_ = control(window_, L"BUTTON", L"Record run", BS_PUSHBUTTON,
                              id_runtime_record);
    runtime_path_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                            id_runtime_path, WS_EX_CLIENTEDGE);
    runtime_browse_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON, id_runtime_browse);
    runtime_load_ = control(window_, L"BUTTON", L"Load trace", BS_PUSHBUTTON, id_runtime_load);
    runtime_prev_ = control(window_, L"BUTTON", L"Previous", BS_PUSHBUTTON, id_runtime_prev);
    runtime_next_ = control(window_, L"BUTTON", L"Next", BS_PUSHBUTTON, id_runtime_next);
    runtime_timeline_ = control(window_, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS,
                                id_runtime_timeline);
    runtime_step_label_ = control(window_, L"STATIC", L"Record a run or load a runtime .trace.",
                                  SS_LEFT, 0);
    runtime_canvas_ = control(window_, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER,
                              id_runtime_canvas);
    debug_exe_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                         id_debug_exe, WS_EX_CLIENTEDGE);
    debug_browse_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON, id_debug_browse);
    debug_launch_ = control(window_, L"BUTTON", L"Launch", BS_PUSHBUTTON, id_debug_launch);
    debug_file_ = control(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                          id_debug_file, WS_EX_CLIENTEDGE);
    debug_file_browse_ = control(window_, L"BUTTON", L"Browse…", BS_PUSHBUTTON,
                                 id_debug_file_browse);
    debug_line_ = control(window_, L"EDIT", L"3", WS_BORDER | ES_NUMBER,
                          id_debug_line, WS_EX_CLIENTEDGE);
    debug_break_ = control(window_, L"BUTTON", L"Breakpoint", BS_PUSHBUTTON, id_debug_break);
    debug_continue_ = control(window_, L"BUTTON", L"Continue", BS_PUSHBUTTON,
                              id_debug_continue);
    debug_step_ = control(window_, L"BUTTON", L"Step line", BS_PUSHBUTTON, id_debug_step);
    debug_stop_ = control(window_, L"BUTTON", L"Stop", BS_PUSHBUTTON, id_debug_stop);
    debug_output_ = control(window_, L"EDIT", L"Launch a debug build with PDB symbols.",
                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                            WS_VSCROLL | WS_HSCROLL, 0, WS_EX_CLIENTEDGE);
    interaction_summary_ = control(window_, L"STATIC",
        L"Scan a project to inspect interaction evidence.", SS_LEFT, 0);
    interaction_filter_ = control(window_, L"BUTTON", L"Selected file only",
        BS_AUTOCHECKBOX, id_interaction_filter);
    interaction_list_ = control(window_, WC_LISTVIEWW, L"",
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, id_interaction_list,
        WS_EX_CLIENTEDGE);
    interaction_detail_ = control(window_, L"EDIT",
        L"Select an evidence row for its scope and limitations.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        0, WS_EX_CLIENTEDGE);
    status_ = control(window_, L"STATIC", L"Ready. Choose a project folder.", SS_LEFT, 0);

    for (HWND h : {root_, browse_, scan_, cards_, search_, tree_, tabs_, open_,
                   detail_, diagram_, uml_overview_button_, uml_focus_button_,
                   uml_prev_, uml_next_, uml_all_, expected_, actual_, browse_expected_,
                   browse_actual_, compare_, results_, runtime_exe_,
                   runtime_exe_browse_, runtime_record_, runtime_path_, runtime_browse_,
                   runtime_load_, runtime_prev_, runtime_next_, runtime_timeline_,
                   runtime_step_label_, runtime_canvas_, debug_exe_, debug_browse_,
                   debug_launch_, debug_file_, debug_file_browse_, debug_line_,
                   debug_break_, debug_continue_, debug_step_, debug_stop_,
                   debug_output_, interaction_summary_, interaction_filter_, interaction_list_,
                   interaction_detail_, status_}) set_font(h, font_);
    set_font(detail_, mono_font_);
    set_font(debug_output_, mono_font_);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter files or classes"));
    SendMessageW(expected_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Expected .facts"));
    SendMessageW(actual_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Actual .facts"));
    SendMessageW(runtime_path_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Runtime .trace file"));
    SendMessageW(runtime_exe_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Instrumented C++ executable (.exe)"));
    SendMessageW(debug_exe_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Debug-build executable (.exe)"));
    SendMessageW(debug_file_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Source file for breakpoint"));
    for (const wchar_t* name : {L"Overview", L"Source", L"Expected / actual", L"UML",
                                L"Runtime map", L"Debugger", L"Interactions"}) {
        TCITEMW tab{};
        tab.mask = TCIF_TEXT;
        tab.pszText = const_cast<LPWSTR>(name);
        TabCtrl_InsertItem(tabs_, TabCtrl_GetItemCount(tabs_), &tab);
    }
    ListView_SetExtendedListViewStyle(results_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Key", L"Expected", L"Actual", L"Result"};
        const int widths[] = {180, 180, 180, 115};
        col.pszText = const_cast<LPWSTR>(names[i]);
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(results_, i, &col);
    }
    ListView_SetExtendedListViewStyle(interaction_list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const wchar_t* evidence_names[] = {L"Evidence", L"From", L"To", L"Detail",
                                       L"Count", L"Location"};
    const int evidence_widths[] = {110, 190, 190, 245, 65, 110};
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(evidence_names[i]);
        col.cx = evidence_widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(interaction_list_, i, &col);
    }
    EnableWindow(cards_, FALSE);
    EnableWindow(open_, FALSE);
    EnableWindow(interaction_filter_, FALSE);
    EnableWindow(runtime_prev_, FALSE);
    EnableWindow(runtime_next_, FALSE);
    update_tab();
    layout();
    SetTimer(window_, 1, 250, nullptr);
}

void App::layout() {
    RECT r{};
    GetClientRect(window_, &r);
    const int width = r.right, height = r.bottom;
    const int margin = 14, gap = 8, row = 30;
    const int left = std::max(270, std::min(340, width / 3));
    const int right_x = margin + left + 14;
    const int right_w = std::max(100, width - right_x - margin);
    MoveWindow(root_, margin, margin, std::max(80, width - 488), row, TRUE);
    MoveWindow(browse_, width - 460, margin, 142, row, TRUE);
    MoveWindow(scan_, width - 310, margin, 75, row, TRUE);
    MoveWindow(cards_, width - 227, margin, 213, row, TRUE);
    MoveWindow(search_, margin, 58, left, row, TRUE);
    MoveWindow(tree_, margin, 96, left, std::max(100, height - 138), TRUE);
    MoveWindow(tabs_, right_x, 58, right_w, std::max(100, height - 101), TRUE);
    // The tab frame is a backdrop; page controls must stay above it in z-order.
    SetWindowPos(tabs_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveWindow(open_, width - 150, 61, 122, 26, TRUE);
    const int body_y = 102, body_h = std::max(100, height - 149);
    MoveWindow(detail_, right_x + 12, body_y, right_w - 24, body_h, TRUE);
    MoveWindow(uml_overview_button_, right_x + 12, body_y, 112, row, TRUE);
    MoveWindow(uml_focus_button_, right_x + 132, body_y, 124, row, TRUE);
    MoveWindow(uml_prev_, right_x + 268, body_y, 84, row, TRUE);
    MoveWindow(uml_next_, right_x + 360, body_y, 84, row, TRUE);
    MoveWindow(uml_all_, right_x + 452, body_y, 138, row, TRUE);
    MoveWindow(diagram_, right_x + 12, body_y + 38, right_w - 24,
               std::max(60, body_h - 38), TRUE);
    MoveWindow(expected_, right_x + 12, body_y, right_w - 126, row, TRUE);
    MoveWindow(browse_expected_, width - 119, body_y, 95, row, TRUE);
    MoveWindow(actual_, right_x + 12, body_y + row + gap, right_w - 126, row, TRUE);
    MoveWindow(browse_actual_, width - 119, body_y + row + gap, 95, row, TRUE);
    MoveWindow(compare_, right_x + 12, body_y + 2 * (row + gap), 140, row, TRUE);
    MoveWindow(results_, right_x + 12, body_y + 3 * row + 2 * gap + 8,
               right_w - 24, std::max(50, body_h - 3 * row - 2 * gap - 8), TRUE);
    MoveWindow(runtime_exe_, right_x + 12, body_y, right_w - 238, row, TRUE);
    MoveWindow(runtime_exe_browse_, width - 225, body_y, 95, row, TRUE);
    MoveWindow(runtime_record_, width - 122, body_y, 98, row, TRUE);
    MoveWindow(runtime_path_, right_x + 12, body_y + 38, right_w - 238, row, TRUE);
    MoveWindow(runtime_browse_, width - 225, body_y + 38, 95, row, TRUE);
    MoveWindow(runtime_load_, width - 122, body_y + 38, 98, row, TRUE);
    MoveWindow(runtime_prev_, right_x + 12, body_y + 78, 86, row, TRUE);
    MoveWindow(runtime_next_, right_x + 106, body_y + 78, 70, row, TRUE);
    MoveWindow(runtime_timeline_, right_x + 188, body_y + 76,
               std::max(100, right_w - 200), 35, TRUE);
    MoveWindow(runtime_step_label_, right_x + 12, body_y + 116, right_w - 24, 22, TRUE);
    MoveWindow(runtime_canvas_, right_x + 12, body_y + 144, right_w - 24,
               std::max(50, body_h - 144), TRUE);
    MoveWindow(debug_exe_, right_x + 12, body_y, right_w - 238, row, TRUE);
    MoveWindow(debug_browse_, width - 225, body_y, 95, row, TRUE);
    MoveWindow(debug_launch_, width - 122, body_y, 98, row, TRUE);
    MoveWindow(debug_file_, right_x + 12, body_y + 38, right_w - 302, row, TRUE);
    MoveWindow(debug_file_browse_, width - 289, body_y + 38, 80, row, TRUE);
    MoveWindow(debug_line_, width - 201, body_y + 38, 64, row, TRUE);
    MoveWindow(debug_break_, width - 129, body_y + 38, 105, row, TRUE);
    MoveWindow(debug_continue_, right_x + 12, body_y + 78, 100, row, TRUE);
    MoveWindow(debug_step_, right_x + 120, body_y + 78, 100, row, TRUE);
    MoveWindow(debug_stop_, right_x + 228, body_y + 78, 100, row, TRUE);
    MoveWindow(debug_output_, right_x + 12, body_y + 116, right_w - 24,
               std::max(50, body_h - 116), TRUE);
    MoveWindow(interaction_summary_, right_x + 12, body_y, right_w - 24, 52, TRUE);
    MoveWindow(interaction_filter_, right_x + 12, body_y + 56,
               162, 24, TRUE);
    MoveWindow(interaction_list_, right_x + 12, body_y + 88, right_w - 24,
               std::max(60, body_h - 182), TRUE);
    MoveWindow(interaction_detail_, right_x + 12, body_y + body_h - 86,
               right_w - 24, 86, TRUE);
    MoveWindow(status_, margin, height - 30, width - 2 * margin, 20, TRUE);
    if (active_tab_ == 3)
        RedrawWindow(diagram_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_FRAME);
}

HTREEITEM App::add_tree(HTREEITEM parent, const std::wstring& title,
                        std::optional<TreeItem> data) {
    TVINSERTSTRUCTW entry{};
    entry.hParent = parent;
    entry.hInsertAfter = TVI_LAST;
    entry.item.mask = TVIF_TEXT | TVIF_PARAM;
    entry.item.pszText = const_cast<LPWSTR>(title.c_str());
    if (data) {
        tree_items_.push_back(std::make_unique<TreeItem>(*data));
        entry.item.lParam = reinterpret_cast<LPARAM>(tree_items_.back().get());
    }
    return TreeView_InsertItem(tree_, &entry);
}

void App::rebuild_tree() {
    rebuilding_tree_ = true;
    const bool was_filtered =
        SendMessageW(interaction_filter_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    TreeView_DeleteAllItems(tree_);
    tree_items_.clear();
    selected_.reset();
    SendMessageW(interaction_filter_, BM_SETCHECK, BST_UNCHECKED, 0);
    EnableWindow(interaction_filter_, FALSE);
    if (!project_) { rebuilding_tree_ = false; return; }
    const std::wstring filter = lower(window_text(search_));
    const HTREEITEM files = add_tree(TVI_ROOT, L"FILES");
    const HTREEITEM types = add_tree(TVI_ROOT, L"CLASSES / STRUCTS");
    std::map<std::wstring, HTREEITEM> folders;
    std::size_t file_count = 0, class_count = 0;
    for (std::size_t i = 0; i < project_->files.size(); ++i) {
        const auto& file = project_->files[i];
        const std::wstring relative = path_text(file.relative_path);
        bool file_match = filter.empty() || lower(relative).find(filter) != std::wstring::npos;
        bool symbol_match = false;
        for (const auto& symbol : file.symbols) {
            const auto& name = symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name;
            if (lower(wide(name)).find(filter) != std::wstring::npos) symbol_match = true;
        }
        if (file_match || symbol_match) {
            HTREEITEM parent = files;
            fs::path progressive;
            for (const auto& part : file.relative_path.parent_path()) {
                progressive /= part;
                const std::wstring key = progressive.wstring();
                if (!folders.contains(key)) folders[key] = add_tree(parent, part.wstring());
                parent = folders[key];
            }
            add_tree(parent, file.relative_path.filename().wstring(), TreeItem{i, std::nullopt});
            ++file_count;
        }
        for (std::size_t j = 0; j < file.symbols.size(); ++j) {
            const auto& symbol = file.symbols[j];
            const auto& name = symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name;
            if (!filter.empty() && !file_match &&
                lower(wide(name)).find(filter) == std::wstring::npos) continue;
            const auto class_node = add_tree(types, wide(name), TreeItem{i, j});
            add_tree(class_node, relative, TreeItem{i, std::nullopt});
            ++class_count;
        }
    }
    TreeView_Expand(tree_, files, TVE_EXPAND);
    TreeView_Expand(tree_, types, TVE_EXPAND);
    if (file_count == 0 && class_count == 0) add_tree(files, L"No matching files");
    std::wostringstream info;
    info << project_->files.size() << L" source files · " << class_count
         << L" visible classes / structs";
    std::size_t errors = 0, warnings = 0, failures = 0;
    for (const auto& file : project_->files) {
        errors += file.error_diagnostic_count;
        warnings += file.warning_diagnostic_count;
        if (!file.parsed) ++failures;
    }
    info << L" · " << errors << L" Clang errors · " << warnings
         << L" warnings · " << failures << L" AST failures";
    set_status(info.str());
    rebuilding_tree_ = false;
    if (!interaction_graph_ || was_filtered) refresh_interactions();
    update_detail();
}

void App::start_scan() {
    if (scanning_ || recording_) return;
    const fs::path root(window_text(root_));
    std::error_code path_error;
    if (root.empty() || !fs::is_directory(root, path_error)) {
        if (smoke_) { smoke_exit_code = 2; DestroyWindow(window_); return; }
        show_error(L"Project folder", L"Choose an existing C++ project folder first.");
        return;
    }
    if (worker_.joinable()) worker_.join();
    if (project_) {
        const auto canonical = fs::weakly_canonical(root, path_error);
        if (path_error || canonical != project_->root) {
            runtime_trace_.reset();
            runtime_snapshot_.reset();
            runtime_step_ = 0;
            SetWindowTextW(runtime_path_, L"");
            SetWindowTextW(runtime_step_label_, L"Load a trace for this project.");
        }
    }
    std::optional<fs::path> db;
    for (const auto& candidate : {root, root / "build", root / "build-fixed",
                                  root / "build-clang", root / "build-ninja"}) {
        if (fs::exists(candidate / "compile_commands.json", path_error)) { db = candidate; break; }
    }
    project_.reset();
    uml_graph_.reset();
    interaction_graph_.reset();
    interaction_row_edges_.clear();
    ListView_DeleteAllItems(interaction_list_);
    SendMessageW(interaction_filter_, BM_SETCHECK, BST_UNCHECKED, 0);
    uml_hits_.clear();
    uml_minimap_ = RECT{};
    uml_minimap_scale_ = 0.0;
    uml_page_ = 0;
    uml_overview_ = true;
    uml_fit_pending_ = true;
    TreeView_DeleteAllItems(tree_);
    tree_items_.clear();
    selected_.reset();
    scanning_ = true;
    EnableWindow(scan_, FALSE);
    EnableWindow(cards_, FALSE);
    EnableWindow(open_, FALSE);
    EnableWindow(root_, FALSE);
    EnableWindow(browse_, FALSE);
    EnableWindow(search_, FALSE);
    SetWindowTextW(detail_, L"Scanning with libclang…\r\n\r\nThe window remains responsive.");
    set_status(L"Scanning C++ source files…");
    const HWND target = window_;
    worker_ = std::jthread([target, root, db] {
        std::expected<atlas::ProjectScan, std::string> value;
        try {
            value = atlas::scan_project(root, db);
        } catch (const std::exception& error) {
            value = std::unexpected(std::string("scan exception: ") + error.what());
        } catch (...) {
            value = std::unexpected("unknown scan exception");
        }
        auto result = std::make_unique<ScanResult>(ScanResult{std::move(value)});
        if (PostMessageW(target, scan_done_message, 0,
                         reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}

void App::scan_finished(ScanResult* raw) {
    std::unique_ptr<ScanResult> result(raw);
    if (worker_.joinable()) worker_.join();
    scanning_ = false;
    EnableWindow(scan_, TRUE);
    EnableWindow(root_, TRUE);
    EnableWindow(browse_, TRUE);
    EnableWindow(search_, TRUE);
    if (close_requested_) { DestroyWindow(window_); return; }
    if (!result->value) {
        const auto message = wide(result->value.error());
        set_status(L"Scan failed: " + message);
        SetWindowTextW(detail_, message.c_str());
        if (smoke_) { smoke_exit_code = 3; DestroyWindow(window_); return; }
        show_error(L"Scan failed", message);
        return;
    }
    project_ = std::move(*result->value);
    uml_graph_ = atlas::build_uml_graph(*project_);
    uml_fit_pending_ = true;
    EnableWindow(cards_, TRUE);
    rebuild_tree();
    if (smoke_runtime_) {
        SetWindowTextW(runtime_path_, smoke_runtime_->c_str());
        load_runtime();
        active_tab_ = 4;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
    }
    if (smoke_) {
        smoke_exit_code = project_->files.empty() || TreeView_GetCount(tree_) <= 2 ? 4 : 0;
        for (const auto& file : project_->files) {
            if (!file.parsed || file.error_diagnostic_count) smoke_exit_code = 6;
        }
        if (smoke_facts_) {
            SetWindowTextW(expected_, smoke_facts_->first.c_str());
            SetWindowTextW(actual_, smoke_facts_->second.c_str());
            compare_facts();
            if (ListView_GetItemCount(results_) == 0 && smoke_exit_code == 0)
                smoke_exit_code = 5;
        }
        if (smoke_runtime_) {
            if ((!runtime_snapshot_ || runtime_snapshot_->nodes.size() != 3 ||
                 runtime_snapshot_->links.size() != 2 || active_tab_ != 4 ||
                 TabCtrl_GetCurSel(tabs_) != 4) && smoke_exit_code == 0) {
                smoke_exit_code = 8;
            }
            RECT tab_bounds{}, canvas_bounds{}, load_bounds{};
            GetWindowRect(tabs_, &tab_bounds);
            GetWindowRect(runtime_canvas_, &canvas_bounds);
            GetWindowRect(runtime_load_, &load_bounds);
            const bool controls_in_page =
                canvas_bounds.left >= tab_bounds.left &&
                canvas_bounds.right <= tab_bounds.right &&
                canvas_bounds.top > tab_bounds.top &&
                canvas_bounds.bottom <= tab_bounds.bottom &&
                load_bounds.left >= tab_bounds.left &&
                load_bounds.right <= tab_bounds.right &&
                load_bounds.top > tab_bounds.top &&
                load_bounds.bottom <= tab_bounds.bottom;
            if ((!controls_in_page ||
                 !(GetWindowLongPtrW(runtime_canvas_, GWL_STYLE) & WS_VISIBLE) ||
                 !(GetWindowLongPtrW(runtime_load_, GWL_STYLE) & WS_VISIBLE) ||
                 !(GetWindowLongPtrW(runtime_record_, GWL_STYLE) & WS_VISIBLE) ||
                 GetWindow(tabs_, GW_HWNDNEXT) != nullptr) && smoke_exit_code == 0)
                smoke_exit_code = 9;
        }
        active_tab_ = 5;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        RECT debug_bounds{}, tab_bounds{};
        GetWindowRect(debug_output_, &debug_bounds);
        GetWindowRect(tabs_, &tab_bounds);
        if ((TabCtrl_GetItemCount(tabs_) != 7 ||
             TabCtrl_GetCurSel(tabs_) != 5 ||
             !(GetWindowLongPtrW(debug_output_, GWL_STYLE) & WS_VISIBLE) ||
             !(GetWindowLongPtrW(debug_launch_, GWL_STYLE) & WS_VISIBLE) ||
             debug_bounds.left < tab_bounds.left ||
             debug_bounds.right > tab_bounds.right ||
             debug_bounds.bottom > tab_bounds.bottom) && smoke_exit_code == 0)
            smoke_exit_code = 10;
        active_tab_ = 6;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        RECT evidence_bounds{}, detail_bounds{}, evidence_tab_bounds{};
        GetWindowRect(interaction_list_, &evidence_bounds);
        GetWindowRect(interaction_detail_, &detail_bounds);
        GetWindowRect(tabs_, &evidence_tab_bounds);
        if ((ListView_GetItemCount(interaction_list_) == 0 ||
             !(GetWindowLongPtrW(interaction_list_, GWL_STYLE) & WS_VISIBLE) ||
             evidence_bounds.left < evidence_tab_bounds.left ||
             evidence_bounds.right > evidence_tab_bounds.right ||
             detail_bounds.top < evidence_bounds.bottom ||
             detail_bounds.bottom > evidence_tab_bounds.bottom) &&
            smoke_exit_code == 0) smoke_exit_code = 11;
        DestroyWindow(window_);
    }
}

void App::select_tree_item(HTREEITEM item) {
    TVITEMW data{};
    data.mask = TVIF_PARAM;
    data.hItem = item;
    if (TreeView_GetItem(tree_, &data) && data.lParam) {
        selected_ = *reinterpret_cast<TreeItem*>(data.lParam);
    } else selected_.reset();
    if (active_tab_ == 3 && uml_overview_ && !uml_include_isolated_ &&
        uml_graph_ && selected_ && selected_->symbol) {
        for (std::size_t i = 0; i < uml_graph_->nodes.size(); ++i) {
            const auto& node = uml_graph_->nodes[i];
            if (node.file != selected_->file || node.symbol != *selected_->symbol)
                continue;
            const bool linked = std::ranges::any_of(uml_graph_->edges,
                [i](const atlas::UmlEdge& edge) {
                    return edge.from == i || edge.to == i;
                });
            if (!linked) uml_fit_pending_ = true;
            break;
        }
    }
    EnableWindow(open_, selected_.has_value());
    EnableWindow(interaction_filter_, selected_.has_value());
    if (SendMessageW(interaction_filter_, BM_GETCHECK, 0, 0) == BST_CHECKED)
        refresh_interactions();
    update_detail();
    InvalidateRect(diagram_, nullptr, TRUE);
}

bool App::focus_tree_item(const TreeItem& target) {
    if (!window_text(search_).empty()) SetWindowTextW(search_, L"");
    std::vector<HTREEITEM> stack;
    for (auto item = TreeView_GetRoot(tree_); item;
         item = TreeView_GetNextSibling(tree_, item)) stack.push_back(item);
    while (!stack.empty()) {
        auto item = stack.back();
        stack.pop_back();
        TVITEMW data{};
        data.mask = TVIF_PARAM;
        data.hItem = item;
        if (TreeView_GetItem(tree_, &data) && data.lParam) {
            const auto* entry = reinterpret_cast<const TreeItem*>(data.lParam);
            if (entry->file == target.file && entry->symbol == target.symbol) {
                TreeView_SelectItem(tree_, item);
                TreeView_EnsureVisible(tree_, item);
                select_tree_item(item);
                return true;
            }
        }
        for (auto child = TreeView_GetChild(tree_, item); child;
             child = TreeView_GetNextSibling(tree_, child)) stack.push_back(child);
    }
    return false;
}

void App::update_detail() {
    if (active_tab_ == 2 || active_tab_ == 3 || active_tab_ == 4 ||
        active_tab_ == 5 || active_tab_ == 6) return;
    if (!project_ || !selected_) {
        SetWindowTextW(detail_, project_ ? L"Select a source file or class on the left."
                                           : L"Choose a C++ project, then Scan.");
        return;
    }
    const auto& file = project_->files[selected_->file];
    if (active_tab_ == 1) {
        const auto path = project_->root / file.relative_path;
        SetWindowTextW(detail_, read_source(path).c_str());
        return;
    }
    std::wostringstream out;
    out << L"SOURCE\r\n" << file.relative_path.wstring() << L"\r\n\r\n"
        << L"AST parse: " << (file.parsed ? L"complete" : L"failed") << L"\r\n"
        << L"Clang errors: " << file.error_diagnostic_count << L"; warnings: "
        << file.warning_diagnostic_count << L"\r\n"
        << L"Compile command: " << (file.used_compile_command ? L"from compile_commands.json" : L"default flags")
        << L"\r\n\r\n";
    if (selected_->symbol) {
        const auto& symbol = file.symbols[*selected_->symbol];
        const auto& name = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
        out << L"SELECTED TYPE\r\n" << wide(symbol.kind) << L" " << wide(name)
            << L" (line " << symbol.line << L")\r\n";
        out << L"Clang USR: " << wide(symbol.usr) << L"\r\n";
        if (!symbol.base.empty()) out << L"Base: " << wide(symbol.base) << L"\r\n";
        out << L"\r\n";
    }
    out << L"DECLARED TYPES\r\n";
    if (file.symbols.empty()) out << L"None detected\r\n";
    for (const auto& symbol : file.symbols) {
        const auto& name = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
        out << L"  " << wide(symbol.kind) << L" " << wide(name)
            << L"  ·  line " << symbol.line;
        if (!symbol.base.empty()) out << L"  :  " << wide(symbol.base);
        out << L"\r\n";
    }
    const auto project_calls = std::ranges::count_if(file.calls,
        [](const atlas::CallSite& call) { return !call.callee_file.empty(); });
    out << L"\r\nSTATIC CALLS (THIS FILE)\r\n"
        << project_calls << L" call sites into this project; "
        << file.calls.size() - static_cast<std::size_t>(project_calls)
        << L" external or unresolved. Dynamic dispatch may have other targets.\r\n";
    std::map<std::pair<std::string, std::string>,
             std::pair<std::size_t, std::size_t>> call_pairs;
    for (const auto& call : file.calls) {
        if (call.callee_file.empty()) continue;
        auto result = call_pairs.try_emplace(
            std::pair{call.caller_name, call.callee_name}, call.line, 0);
        ++result.first->second.second;
    }
    std::size_t shown_calls = 0;
    for (const auto& [names, sites] : call_pairs) {
        if (shown_calls++ >= 35) continue;
        out << L"  " << wide(names.first) << L" → " << wide(names.second)
            << L"  ·  line " << sites.first << L"  ·  " << sites.second
            << L" site(s)\r\n";
    }
    if (call_pairs.size() > 35)
        out << L"  … " << call_pairs.size() - 35
            << L" more pairs; use agent find for every call site.\r\n";
    out << L"\r\nINCLUDES\r\n";
    if (file.includes.empty()) out << L"None detected\r\n";
    for (const auto& include : file.includes) out << L"  " << wide(include) << L"\r\n";
    out << L"\r\nDIAGNOSTICS\r\n";
    if (file.diagnostics.empty()) out << L"No Clang warnings or errors\r\n";
    for (const auto& diagnostic : file.diagnostics) out << L"  " << wide(diagnostic) << L"\r\n";
    SetWindowTextW(detail_, out.str().c_str());
}

void App::update_tab() {
    const bool detail = active_tab_ == 0 || active_tab_ == 1;
    const bool compare = active_tab_ == 2;
    const bool runtime = active_tab_ == 4;
    const bool debugging = active_tab_ == 5;
    const bool interactions = active_tab_ == 6;
    ShowWindow(detail_, detail ? SW_SHOW : SW_HIDE);
    ShowWindow(diagram_, active_tab_ == 3 ? SW_SHOW : SW_HIDE);
    for (HWND h : {uml_overview_button_, uml_focus_button_, uml_prev_, uml_next_, uml_all_})
        ShowWindow(h, active_tab_ == 3 ? SW_SHOW : SW_HIDE);
    EnableWindow(uml_focus_button_, selected_ && selected_->symbol);
    EnableWindow(uml_prev_, uml_overview_);
    EnableWindow(uml_next_, uml_overview_);
    EnableWindow(uml_all_, uml_overview_);
    for (HWND h : {expected_, actual_, browse_expected_, browse_actual_, compare_, results_})
        ShowWindow(h, compare ? SW_SHOW : SW_HIDE);
    for (HWND h : {runtime_exe_, runtime_exe_browse_, runtime_record_,
                   runtime_path_, runtime_browse_, runtime_load_, runtime_prev_,
                   runtime_next_, runtime_timeline_, runtime_step_label_, runtime_canvas_})
        ShowWindow(h, runtime ? SW_SHOW : SW_HIDE);
    for (HWND h : {debug_exe_, debug_browse_, debug_launch_, debug_file_,
                   debug_file_browse_, debug_line_, debug_break_, debug_continue_,
                   debug_step_, debug_stop_, debug_output_})
        ShowWindow(h, debugging ? SW_SHOW : SW_HIDE);
    for (HWND h : {interaction_summary_, interaction_filter_,
                   interaction_list_, interaction_detail_})
        ShowWindow(h, interactions ? SW_SHOW : SW_HIDE);
    ShowWindow(open_, detail ? SW_SHOW : SW_HIDE);
    if (detail) update_detail();
    if (active_tab_ == 3)
        RedrawWindow(diagram_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_FRAME);
    if (runtime) InvalidateRect(runtime_canvas_, nullptr, TRUE);
    if (debugging) refresh_debug_view();
    RedrawWindow(window_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void App::refresh_debug_view() {
    if (active_tab_ != 5) return;
    const auto rendered = wide(debugger_.snapshot_text());
    if (rendered != debug_rendered_) {
        debug_rendered_ = rendered;
        SetWindowTextW(debug_output_, rendered.c_str());
        set_status(wide(debugger_.summary()));
    }
}

void App::record_runtime() {
    if (recording_ || scanning_) return;
    const fs::path executable(window_text(runtime_exe_));
    std::error_code error;
    if (!fs::is_regular_file(executable, error)) {
        show_error(L"Automatic capture", L"Choose an existing instrumented .exe first.");
        return;
    }
    if (capture_worker_.joinable()) capture_worker_.join();
    const fs::path project_root(window_text(root_));
    const auto base = fs::is_directory(project_root, error) ? project_root
                                                            : executable.parent_path();
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto output = base / ".project-atlas" / "captures" /
        (L"runtime-" + std::to_wstring(stamp) + L".trace");
    recording_ = true;
    EnableWindow(runtime_record_, FALSE);
    EnableWindow(scan_, FALSE);
    set_status(L"Recording C++ allocations; waiting for the program to exit…");
    const HWND target = window_;
    capture_worker_ = std::jthread([target, executable, output](std::stop_token stop) {
        auto value = atlas::capture_runtime(executable, output, stop);
        auto result = std::make_unique<CaptureFinished>(CaptureFinished{std::move(value)});
        if (PostMessageW(target, capture_done_message, 0,
                         reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}

void App::capture_finished(CaptureFinished* raw) {
    std::unique_ptr<CaptureFinished> result(raw);
    if (capture_worker_.joinable()) capture_worker_.join();
    recording_ = false;
    EnableWindow(runtime_record_, TRUE);
    EnableWindow(scan_, TRUE);
    if (close_requested_) { DestroyWindow(window_); return; }
    if (!result->value) {
        set_status(L"Capture failed: " + wide(result->value.error()));
        show_error(L"Automatic capture", wide(result->value.error()));
        return;
    }
    SetWindowTextW(runtime_path_, result->value->trace_path.c_str());
    active_tab_ = 4;
    TabCtrl_SetCurSel(tabs_, active_tab_);
    update_tab();
    load_runtime();
    if (result->value->truncated)
        set_status(L"Capture reached 10,000 events; later state was not recorded.");
    else if (result->value->exit_code != 0)
        set_status(L"Capture loaded; target exited with code " +
                   std::to_wstring(result->value->exit_code));
}

void App::start_ui_server() {
    const HWND target = window_;
    ui_server_ = std::jthread([this, target](std::stop_token stop) {
        atlas::serve_ui_commands(stop, GetCurrentProcessId(),
            [this, target](const atlas::UiCommand& command) {
                auto dispatch = std::make_shared<UiDispatch>();
                dispatch->command = command;
                {
                    std::lock_guard lock(ui_dispatch_mutex_);
                    ui_dispatches_.push_back(dispatch);
                }
                DWORD_PTR delivered = 0;
                const UINT timeout = command.action == atlas::UiAction::generate_cards
                    ? 30000U : 3000U;
                const BOOL handled = SendMessageTimeoutW(
                    target, ui_command_message, 0,
                    reinterpret_cast<LPARAM>(dispatch.get()),
                    SMTO_ABORTIFHUNG, timeout, &delivered) != 0;
                if (!handled) {
                    dispatch->cancelled.store(true);
                    atlas::UiReply reply;
                    reply.pid = GetCurrentProcessId();
                    copy_ui_text(reply.error,
                                 L"GUI did not answer in time; command outcome is unknown");
                    return reply;
                }
                atlas::UiReply reply = dispatch->reply;
                std::lock_guard lock(ui_dispatch_mutex_);
                std::erase(ui_dispatches_, dispatch);
                return reply;
            });
    });
}

atlas::UiReply App::handle_ui_command(const atlas::UiCommand& command) {
    atlas::UiReply reply;
    reply.pid = GetCurrentProcessId();
    std::wstring problem;
    const fs::path argument(command.path);
    std::error_code path_error;
    switch (command.action) {
    case atlas::UiAction::status:
        break;
    case atlas::UiAction::show_window:
        ShowWindow(window_, SW_SHOWNORMAL);
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        break;
    case atlas::UiAction::uml_overview:
        uml_overview_ = true;
        uml_fit_pending_ = true;
        active_tab_ = 3;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        break;
    case atlas::UiAction::uml_focus:
        if (!selected_ || !selected_->symbol) {
            problem = L"select a class or struct before opening its UML card";
            break;
        }
        uml_overview_ = false;
        active_tab_ = 3;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        break;
    case atlas::UiAction::load_trace: {
        if (!fs::is_regular_file(argument, path_error) || argument.extension() != L".trace") {
            problem = L"choose an existing .trace file";
            break;
        }
        auto trace = atlas::read_trace(argument);
        if (!trace) { problem = wide(trace.error()); break; }
        if (trace->size() > 10000) { problem = L"trace exceeds the 10,000-step UI limit"; break; }
        if (auto replay = atlas::replay_runtime(*trace, trace->size()); !replay) {
            problem = wide(replay.error()); break;
        }
        SetWindowTextW(runtime_path_, argument.c_str());
        active_tab_ = 4;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        install_runtime_trace(std::move(*trace));
        break;
    }
    case atlas::UiAction::set_step:
        if (!runtime_trace_) problem = L"load a runtime trace first";
        else if (command.value > runtime_trace_->size())
            problem = L"step exceeds the loaded trace length";
        else set_runtime_step(static_cast<std::size_t>(command.value));
        break;
    case atlas::UiAction::select_tab:
        if (command.value > 6) problem = L"tab must be between 0 and 6";
        else {
            active_tab_ = static_cast<int>(command.value);
            TabCtrl_SetCurSel(tabs_, active_tab_);
            update_tab();
        }
        break;
    case atlas::UiAction::scan_project:
        if (scanning_ || recording_) { problem = L"Atlas is busy"; break; }
        if (!fs::is_directory(argument, path_error)) {
            problem = L"choose an existing project directory"; break;
        }
        SetWindowTextW(root_, argument.c_str());
        start_scan();
        break;
    case atlas::UiAction::record_run:
        if (scanning_ || recording_) { problem = L"Atlas is busy"; break; }
        if (!fs::is_regular_file(argument, path_error) || argument.extension() != L".exe") {
            problem = L"choose an existing instrumented .exe"; break;
        }
        SetWindowTextW(runtime_exe_, argument.c_str());
        record_runtime();
        break;
    case atlas::UiAction::focus_file: {
        if (scanning_ || !project_) { problem = L"scan the project first"; break; }
        std::optional<std::size_t> match;
        for (std::size_t i = 0; i < project_->files.size(); ++i) {
            std::error_code error;
            if (fs::equivalent(argument, project_->root / project_->files[i].relative_path, error)) {
                match = i;
                break;
            }
        }
        if (!match) { problem = L"file is not in the current scanned project"; break; }
        if (!focus_tree_item(TreeItem{*match, std::nullopt})) {
            problem = L"file is not visible in the navigation tree"; break;
        }
        active_tab_ = 1;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        break;
    }
    case atlas::UiAction::focus_symbol: {
        if (scanning_ || !project_) { problem = L"scan the project first"; break; }
        const std::wstring_view query(command.path);
        if (query.empty()) { problem = L"provide a Clang USR or qualified symbol name"; break; }
        std::optional<TreeItem> match;
        bool matched_usr = false;
        bool ambiguous = false;
        for (std::size_t i = 0; i < project_->files.size(); ++i) {
            for (std::size_t j = 0; j < project_->files[i].symbols.size(); ++j) {
                const auto& symbol = project_->files[i].symbols[j];
                const bool is_usr = !symbol.usr.empty() && wide(symbol.usr) == query;
                const bool is_name = wide(symbol.qualified_name) == query;
                if (!is_usr && !is_name) continue;
                if (is_usr && !matched_usr) { match = TreeItem{i, j}; matched_usr = true; ambiguous = false; }
                else if (!matched_usr) {
                    if (match) ambiguous = true;
                    else match = TreeItem{i, j};
                }
            }
        }
        if (!match) { problem = L"symbol is not in the current scanned project"; break; }
        if (ambiguous) { problem = L"symbol name is ambiguous; use its Clang USR"; break; }
        if (!focus_tree_item(*match)) { problem = L"symbol is not visible in the navigation tree"; break; }
        active_tab_ = 0;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        break;
    }
    case atlas::UiAction::compare_facts: {
        const fs::path actual_path(command.second_path);
        if (argument.extension() != L".facts" || actual_path.extension() != L".facts") {
            problem = L"choose two .facts files"; break;
        }
        auto expected = atlas::read_facts(argument);
        if (!expected) { problem = wide(expected.error()); break; }
        auto actual = atlas::read_facts(actual_path);
        if (!actual) { problem = wide(actual.error()); break; }
        SetWindowTextW(expected_, argument.c_str());
        SetWindowTextW(actual_, actual_path.c_str());
        active_tab_ = 2;
        TabCtrl_SetCurSel(tabs_, active_tab_);
        update_tab();
        compare_facts();
        break;
    }
    case atlas::UiAction::generate_cards: {
        if (scanning_ || !project_) { problem = L"scan the project first"; break; }
        const auto output = project_->root / ".project-atlas" / "generated";
        auto written = atlas::write_project_report(*project_, output, true);
        if (!written) { problem = wide(written.error()); break; }
        set_status(L"Generated .agent.md cards beside source files; index in " + output.wstring());
        break;
    }
    case atlas::UiAction::debug_launch: {
        std::string error;
        if (!debugger_.launch(argument, error)) problem = wide(error);
        else {
            SetWindowTextW(debug_exe_, argument.c_str());
            active_tab_ = 5;
            TabCtrl_SetCurSel(tabs_, active_tab_);
            update_tab();
            set_status(wide(debugger_.summary()));
        }
        break;
    }
    case atlas::UiAction::debug_breakpoint: {
        std::string error;
        if (command.value == 0 || command.value > UINT32_MAX ||
            !debugger_.breakpoint(argument, static_cast<std::uint32_t>(command.value), error))
            problem = error.empty() ? L"invalid breakpoint line" : wide(error);
        else {
            SetWindowTextW(debug_file_, argument.c_str());
            SetWindowTextW(debug_line_, std::to_wstring(command.value).c_str());
            set_status(L"Breakpoint submitted; inspect debug.breakpoints for verification");
        }
        break;
    }
    case atlas::UiAction::debug_continue:
    case atlas::UiAction::debug_step: {
        std::string error;
        if (!debugger_.resume(command.action == atlas::UiAction::debug_step, error))
            problem = wide(error);
        else set_status(L"Debugger resume submitted");
        break;
    }
    case atlas::UiAction::debug_stop: {
        std::string error;
        if (!debugger_.stop(error)) problem = wide(error);
        else set_status(L"Debugger stop submitted");
        break;
    }
    default:
        problem = L"unknown UI action";
        break;
    }
    reply.ok = problem.empty() ? 1U : 0U;
    copy_ui_text(reply.error, problem);
    reply.active_tab = static_cast<std::uint32_t>(active_tab_);
    reply.scanning = scanning_ ? 1U : 0U;
    reply.recording = recording_ ? 1U : 0U;
    reply.trace_loaded = runtime_trace_ ? 1U : 0U;
    reply.window_visible = IsWindowVisible(window_) ? 1U : 0U;
    reply.uml_overview = uml_overview_ ? 1U : 0U;
    reply.uml_class_count = uml_graph_ ? static_cast<std::uint32_t>(uml_graph_->nodes.size()) : 0U;
    reply.uml_link_count = uml_graph_ ? static_cast<std::uint32_t>(uml_graph_->edges.size()) : 0U;
    reply.uml_page = static_cast<std::uint32_t>(uml_page_ + 1);
    reply.step = runtime_step_;
    reply.total_steps = runtime_trace_ ? runtime_trace_->size() : 0;
    reply.node_count = runtime_snapshot_ ? runtime_snapshot_->nodes.size() : 0;
    reply.link_count = runtime_snapshot_ ? runtime_snapshot_->links.size() : 0;
    std::wstring selected_file;
    std::wstring selected_symbol;
    if (project_ && selected_ && selected_->file < project_->files.size()) {
        const auto& file = project_->files[selected_->file];
        selected_file = file.relative_path.wstring();
        if (selected_->symbol && *selected_->symbol < file.symbols.size()) {
            const auto& symbol = file.symbols[*selected_->symbol];
            selected_symbol = wide(symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name);
        }
    }
    if (!copy_ui_text(reply.project_root, window_text(root_)) ||
        !copy_ui_text(reply.trace_path, window_text(runtime_path_)) ||
        !copy_ui_text(reply.selected_file, selected_file) ||
        !copy_ui_text(reply.selected_symbol, selected_symbol) ||
        !copy_ui_text(reply.debug_json, wide(debugger_.snapshot_json())) ||
        !copy_ui_text(reply.status, window_text(status_))) {
        reply.ok = 0;
        copy_ui_text(reply.error, L"GUI state exceeds the control protocol limit");
    }
    return reply;
}

void App::load_runtime() {
    const fs::path path(window_text(runtime_path_));
    auto trace = atlas::read_trace(path);
    if (!trace) {
        show_error(L"Runtime trace", wide(trace.error()));
        return;
    }
    if (trace->size() > 10000) {
        show_error(L"Runtime trace", L"The interactive view supports up to 10,000 steps. Use the CLI for longer traces.");
        return;
    }
    if (auto validated = atlas::replay_runtime(*trace, trace->size()); !validated) {
        show_error(L"Runtime trace", wide(validated.error()));
        return;
    }
    install_runtime_trace(std::move(*trace));
}

void App::install_runtime_trace(atlas::Trace trace) {
    runtime_trace_ = std::move(trace);
    runtime_snapshot_.reset();
    SendMessageW(runtime_timeline_, TBM_SETRANGE, TRUE,
                 MAKELPARAM(0, static_cast<int>(runtime_trace_->size())));
    std::size_t initial = runtime_trace_->size();
    for (std::size_t i = initial; i > 0; --i) {
        if ((*runtime_trace_)[i - 1].operation == "link.add") {
            initial = i;
            break;
        }
    }
    set_runtime_step(initial);
}

void App::set_runtime_step(std::size_t step) {
    if (!runtime_trace_ || step > runtime_trace_->size()) return;
    auto snapshot = atlas::replay_runtime(*runtime_trace_, step);
    if (!snapshot) {
        show_error(L"Runtime map", wide(snapshot.error()));
        return;
    }
    runtime_snapshot_ = std::move(*snapshot);
    interaction_graph_.reset();
    refresh_interactions();
    runtime_step_ = step;
    SendMessageW(runtime_timeline_, TBM_SETPOS, TRUE, static_cast<LPARAM>(step));
    EnableWindow(runtime_prev_, step > 0);
    EnableWindow(runtime_next_, step < runtime_trace_->size());
    std::wstring description = L"Step " + std::to_wstring(step) + L" / " +
        std::to_wstring(runtime_trace_->size());
    if (step > 0) {
        const auto& event = (*runtime_trace_)[step - 1];
        description += L"   ·   " + wide(event.operation) + L"  " +
                       wide(event.resource) + L"  " + wide(event.value);
    } else {
        description += L"   ·   before the first event";
    }
    SetWindowTextW(runtime_step_label_, description.c_str());
    set_status(std::to_wstring(runtime_snapshot_->nodes.size()) + L" active nodes · " +
               std::to_wstring(runtime_snapshot_->links.size()) + L" links");
    InvalidateRect(runtime_canvas_, nullptr, TRUE);
}

void App::refresh_interactions() {
    if (!interaction_graph_) {
        atlas::ProjectScan empty;
        interaction_graph_ = atlas::build_interaction_graph(
            project_ ? *project_ : empty, runtime_snapshot_);
    }
    const auto presentation = atlas::present_interactions(
        *interaction_graph_, selected_ ? std::optional(selected_->file) : std::nullopt,
        SendMessageW(interaction_filter_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ListView_DeleteAllItems(interaction_list_);
    interaction_row_edges_.clear();
    for (const auto& entry : presentation.rows) {
        LVITEMW row{};
        row.mask = LVIF_TEXT;
        row.iItem = static_cast<int>(interaction_row_edges_.size());
        auto kind = wide(entry.kind);
        row.pszText = kind.data();
        const int inserted = ListView_InsertItem(interaction_list_, &row);
        if (inserted < 0) continue;
        interaction_row_edges_.push_back(entry.edge_index);
        const auto set_cell = [&](int column, const std::wstring& value) {
            ListView_SetItemText(interaction_list_, inserted, column,
                                 const_cast<LPWSTR>(value.c_str()));
        };
        set_cell(1, wide(entry.from));
        set_cell(2, wide(entry.to));
        set_cell(3, wide(entry.detail));
        set_cell(4, wide(entry.count));
        set_cell(5, wide(entry.location));
    }
    SetWindowTextW(interaction_summary_, wide(presentation.summary).c_str());
    SetWindowTextW(interaction_detail_,
        L"AST evidence is not observed execution. Runtime IDs are separate until source mapping is proven. Select a row for details; double-click AST evidence to open its file.");
}

void App::show_interaction_detail(std::size_t edge_index) {
    if (!interaction_graph_ || edge_index >= interaction_row_edges_.size()) return;
    const auto text = atlas::interaction_detail_text(
        *interaction_graph_, interaction_row_edges_[edge_index]);
    SetWindowTextW(interaction_detail_, wide(text).c_str());
}

void App::compare_facts() {
    const auto expected_path = fs::path(window_text(expected_));
    const auto actual_path = fs::path(window_text(actual_));
    const auto expected = atlas::read_facts(expected_path);
    if (!expected) { show_error(L"Expected facts", wide(expected.error())); return; }
    const auto actual = atlas::read_facts(actual_path);
    if (!actual) { show_error(L"Actual facts", wide(actual.error())); return; }
    const auto comparison = atlas::compare(*expected, *actual);
    ListView_DeleteAllItems(results_);
    std::size_t different = 0;
    for (std::size_t i = 0; i < comparison.entries.size(); ++i) {
        const auto& difference = comparison.entries[i];
        const wchar_t* label = L"Match";
        switch (difference.kind) {
        case atlas::DifferenceKind::match: break;
        case atlas::DifferenceKind::mismatch: label = L"Mismatch"; ++different; break;
        case atlas::DifferenceKind::missing_actual: label = L"Missing actual"; ++different; break;
        case atlas::DifferenceKind::unexpected_actual: label = L"Unexpected"; ++different; break;
        }
        std::wstring key = wide(difference.key);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = key.data();
        ListView_InsertItem(results_, &item);
        std::wstring exp = difference.expected ? wide(*difference.expected) : L"—";
        std::wstring act = difference.actual ? wide(*difference.actual) : L"—";
        ListView_SetItemText(results_, static_cast<int>(i), 1, exp.data());
        ListView_SetItemText(results_, static_cast<int>(i), 2, act.data());
        ListView_SetItemText(results_, static_cast<int>(i), 3, const_cast<LPWSTR>(label));
    }
    set_status(std::to_wstring(comparison.entries.size()) + L" facts compared · " +
               std::to_wstring(different) + L" differences");
}

void App::generate_cards() {
    if (!project_ || scanning_) return;
    const auto output = project_->root / ".project-atlas" / "generated";
    auto written = atlas::write_project_report(*project_, output, true);
    if (!written) {
        show_error(L"Could not generate cards", wide(written.error()));
        return;
    }
    set_status(L"Generated .agent.md cards beside source files; index in " + output.wstring());
}

void App::command(int id, int notification) {
    const auto debug_action = [&](atlas::UiAction action, std::wstring_view path = {},
                                  std::uint64_t value = 0) {
        atlas::UiCommand request;
        request.action = action;
        request.value = value;
        if (!copy_ui_text(request.path, path)) {
            show_error(L"Debugger", L"The selected path is too long.");
            return;
        }
        const auto reply = handle_ui_command(request);
        if (!reply.ok) show_error(L"Debugger", reply.error);
        refresh_debug_view();
    };
    switch (id) {
    case id_uml_overview:
        uml_overview_ = true;
        uml_fit_pending_ = true;
        update_tab();
        InvalidateRect(diagram_, nullptr, TRUE);
        break;
    case id_uml_focus:
        if (selected_ && selected_->symbol) {
            uml_overview_ = false;
            update_tab();
            InvalidateRect(diagram_, nullptr, TRUE);
        }
        break;
    case id_uml_prev:
        uml_scale_ = std::max(0.16, uml_scale_ / 1.25);
        uml_fit_pending_ = false;
        InvalidateRect(diagram_, nullptr, TRUE);
        break;
    case id_uml_next:
        uml_scale_ = std::min(3.0, uml_scale_ * 1.25);
        uml_fit_pending_ = false;
        InvalidateRect(diagram_, nullptr, TRUE);
        break;
    case id_uml_all:
        uml_include_isolated_ = !uml_include_isolated_;
        SetWindowTextW(uml_all_, uml_include_isolated_ ? L"Linked classes only"
                                                    : L"Show all classes");
        uml_fit_pending_ = true;
        InvalidateRect(diagram_, nullptr, TRUE);
        break;
    case id_diagram:
        if (notification == STN_DBLCLK) {
            diagram_clicked();
            if (selected_ && selected_->symbol) {
                uml_overview_ = false;
                update_tab();
            }
        }
        break;
    case id_browse:
        if (auto path = pick_path(window_, true)) SetWindowTextW(root_, path->c_str());
        break;
    case id_scan: start_scan(); break;
    case id_cards: generate_cards(); break;
    case id_search:
        if (notification == EN_CHANGE && project_) rebuild_tree();
        break;
    case id_open:
        if (project_ && selected_) {
            const auto path = project_->root / project_->files[selected_->file].relative_path;
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path.c_str(),
                                                         nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                show_error(L"Open source", L"Windows could not open this file in its associated editor.");
        }
        break;
    case id_browse_expected:
        if (auto path = pick_path(window_, false)) SetWindowTextW(expected_, path->c_str());
        break;
    case id_browse_actual:
        if (auto path = pick_path(window_, false)) SetWindowTextW(actual_, path->c_str());
        break;
    case id_compare: compare_facts(); break;
    case id_runtime_browse:
        if (auto path = pick_path(window_, false)) SetWindowTextW(runtime_path_, path->c_str());
        break;
    case id_runtime_exe_browse:
        if (auto path = pick_path(window_, false)) SetWindowTextW(runtime_exe_, path->c_str());
        break;
    case id_runtime_record: record_runtime(); break;
    case id_runtime_load: load_runtime(); break;
    case id_runtime_prev:
        if (runtime_trace_ && runtime_step_ > 0) set_runtime_step(runtime_step_ - 1);
        break;
    case id_runtime_next:
        if (runtime_trace_ && runtime_step_ < runtime_trace_->size())
            set_runtime_step(runtime_step_ + 1);
        break;
    case id_debug_browse:
        if (auto path = pick_path(window_, false)) SetWindowTextW(debug_exe_, path->c_str());
        break;
    case id_debug_file_browse:
        if (auto path = pick_path(window_, false)) SetWindowTextW(debug_file_, path->c_str());
        break;
    case id_debug_launch:
        debug_action(atlas::UiAction::debug_launch, window_text(debug_exe_));
        break;
    case id_debug_break: {
        const auto line_text = window_text(debug_line_);
        try {
            std::size_t consumed = 0;
            const auto line = std::stoull(line_text, &consumed);
            if (!line || line > UINT32_MAX || consumed != line_text.size()) {
                show_error(L"Debugger", L"Enter a positive source line number.");
                break;
            }
            debug_action(atlas::UiAction::debug_breakpoint,
                         window_text(debug_file_), line);
        } catch (...) { show_error(L"Debugger", L"Enter a positive source line number."); }
        break;
    }
    case id_debug_continue: debug_action(atlas::UiAction::debug_continue); break;
    case id_debug_step: debug_action(atlas::UiAction::debug_step); break;
    case id_debug_stop: debug_action(atlas::UiAction::debug_stop); break;
    case id_interaction_filter:
        if (notification == BN_CLICKED) refresh_interactions();
        break;
    default: break;
    }
}

void App::notify(const NMHDR* header) {
    if (header->idFrom == id_tree && header->code == TVN_SELCHANGEDW &&
        !rebuilding_tree_) {
        const auto* changed = reinterpret_cast<const NMTREEVIEWW*>(header);
        select_tree_item(changed->itemNew.hItem);
    } else if (header->idFrom == id_tabs && header->code == TCN_SELCHANGE) {
        active_tab_ = TabCtrl_GetCurSel(tabs_);
        update_tab();
    } else if (header->idFrom == id_interaction_list &&
               header->code == LVN_ITEMCHANGED) {
        const int row = ListView_GetNextItem(interaction_list_, -1, LVNI_SELECTED);
        if (row >= 0) show_interaction_detail(static_cast<std::size_t>(row));
    } else if (header->idFrom == id_interaction_list &&
               header->code == NM_DBLCLK && interaction_graph_ && project_) {
        const int row = ListView_GetNextItem(interaction_list_, -1, LVNI_SELECTED);
        if (row < 0 || static_cast<std::size_t>(row) >= interaction_row_edges_.size()) return;
        const auto& edge = interaction_graph_->edges[
            interaction_row_edges_[static_cast<std::size_t>(row)]];
        const auto& node = interaction_graph_->nodes[edge.from];
        if (!node.file_index) return;
        if (focus_tree_item(TreeItem{*node.file_index, std::nullopt})) {
            active_tab_ = 1;
            TabCtrl_SetCurSel(tabs_, active_tab_);
            update_tab();
        }
    }
}

void App::draw_diagram(const DRAWITEMSTRUCT& item) {
    if (uml_overview_) { draw_uml_overview(item); return; }
    uml_hits_.clear();
    HDC dc = item.hDC;
    RECT area = item.rcItem;
    FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    if (!project_ || !selected_ || !selected_->symbol) {
        DrawTextW(dc, L"Select a class or struct to inspect its members and inheritance.",
                  -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const auto& symbol = project_->files[selected_->file].symbols[*selected_->symbol];
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width < 360 || height < 240) {
        DrawTextW(dc, L"Enlarge the window to see the class diagram.",
                  -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const std::wstring name = wide(symbol.qualified_name.empty()
        ? symbol.name : symbol.qualified_name);
    std::vector<std::wstring> fields, methods;
    for (const auto& member : symbol.members) {
        const wchar_t visibility = member.access == "public" ? L'+'
            : member.access == "protected" ? L'#' : L'-';
        std::wstring label(1, visibility);
        label += L"  ";
        if (member.kind == "static_field") label += L"{static} ";
        label += wide(member.signature);
        if (member.kind == "field" || member.kind == "static_field")
            fields.push_back(std::move(label));
        else methods.push_back(std::move(label));
    }
    const int base_count = static_cast<int>(symbol.base_qualified_names.size());
    const int base_top = area.top + 25;
    const int card_top_limit = area.top + (base_count ? 145 : 25);
    const int card_bottom_limit = area.bottom - 50;
    const int max_rows = std::clamp((card_bottom_limit - card_top_limit - 96) / 19, 2, 18);
    int field_count = std::min(static_cast<int>(fields.size()),
        methods.empty() ? max_rows : std::max(1, max_rows / 2));
    int method_count = std::min(static_cast<int>(methods.size()), max_rows - field_count);
    field_count += std::min(static_cast<int>(fields.size()) - field_count,
                            max_rows - field_count - method_count);
    const int field_rows = std::max(1, field_count);
    const int method_rows = std::max(1, method_count);
    const int card_height = 44 + 22 + field_rows * 19 + 22 + method_rows * 19 + 8;
    const int card_width = std::min(width - 60, 540);
    const int cx = (area.left + area.right) / 2;
    const int card_top = card_top_limit + std::max(0,
        (card_bottom_limit - card_top_limit - card_height) / 2);
    RECT card{cx - card_width / 2, card_top, cx + card_width / 2,
              card_top + card_height};
    if (base_count) {
        const int shown = std::min(base_count, 3);
        const int gap = 12;
        const int base_width = std::min(220, (width - 60 - (shown - 1) * gap) / shown);
        const int group_width = shown * base_width + (shown - 1) * gap;
        const int group_left = cx - group_width / 2;
        const int branch_y = card.top - 31;
        MoveToEx(dc, cx, card.top, nullptr);
        LineTo(dc, cx, branch_y);
        for (int i = 0; i < shown; ++i) {
            RECT base{group_left + i * (base_width + gap), base_top,
                      group_left + i * (base_width + gap) + base_width,
                      base_top + 48};
            const int base_cx = (base.left + base.right) / 2;
            MoveToEx(dc, cx, branch_y, nullptr);
            LineTo(dc, base_cx, branch_y);
            MoveToEx(dc, base_cx, branch_y, nullptr);
            LineTo(dc, base_cx, base.bottom + 10);
            POINT arrow[]{{base_cx, base.bottom}, {base_cx - 7, base.bottom + 11},
                          {base_cx + 7, base.bottom + 11}};
            HGDIOBJ old_brush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
            Polygon(dc, arrow, 3);
            SelectObject(dc, old_brush);
            HBRUSH pale = CreateSolidBrush(RGB(240, 245, 250));
            FillRect(dc, &base, pale);
            DeleteObject(pale);
            Rectangle(dc, base.left, base.top, base.right, base.bottom);
            RECT text{base.left + 8, base.top + 4, base.right - 8, base.bottom - 4};
            const auto label = wide(symbol.base_qualified_names[i]);
            DrawTextW(dc, label.c_str(), -1, &text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (base_count > shown) {
            const auto more = L"+" + std::to_wstring(base_count - shown) + L" more bases";
            RECT note{area.left + 12, base_top + 54, area.right - 12, base_top + 75};
            DrawTextW(dc, more.c_str(), -1, &note, DT_CENTER | DT_SINGLELINE);
        }
    }
    HBRUSH header = CreateSolidBrush(RGB(230, 240, 252));
    RECT title{card.left, card.top, card.right, card.top + 44};
    FillRect(dc, &title, header);
    DeleteObject(header);
    Rectangle(dc, card.left, card.top, card.right, card.bottom);
    RECT name_rect{card.left + 12, card.top + 8, card.right - 12, card.top + 36};
    const auto heading = wide(symbol.kind) + L"  " + name;
    DrawTextW(dc, heading.c_str(), -1, &name_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    auto rule = [&](int y) {
        MoveToEx(dc, card.left, y, nullptr);
        LineTo(dc, card.right, y);
    };
    rule(card.top + 44);
    int y = card.top + 48;
    RECT section{card.left + 12, y, card.right - 12, y + 20};
    DrawTextW(dc, L"ATTRIBUTES", -1, &section, DT_LEFT | DT_SINGLELINE);
    y += 22;
    for (int i = 0; i < field_rows; ++i, y += 19) {
        const std::wstring label = i < field_count ? fields[i] : L"(none)";
        RECT row{card.left + 16, y, card.right - 16, y + 19};
        DrawTextW(dc, label.c_str(), -1, &row, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    rule(y + 1);
    y += 5;
    section = RECT{card.left + 12, y, card.right - 12, y + 20};
    DrawTextW(dc, L"OPERATIONS", -1, &section, DT_LEFT | DT_SINGLELINE);
    y += 22;
    for (int i = 0; i < method_rows; ++i, y += 19) {
        const std::wstring label = i < method_count ? methods[i] : L"(none)";
        RECT row{card.left + 16, y, card.right - 16, y + 19};
        DrawTextW(dc, label.c_str(), -1, &row, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    std::wstring caption = L"Clang AST  ·  " + std::to_wstring(base_count) +
        L" bases  ·  " + std::to_wstring(fields.size()) + L" attributes  ·  " +
        std::to_wstring(methods.size()) + L" operations";
    if (field_count < static_cast<int>(fields.size()) ||
        method_count < static_cast<int>(methods.size())) caption += L"  ·  some members hidden";
    RECT caption_rect{area.left + 20, area.bottom - 35, area.right - 20, area.bottom - 10};
    DrawTextW(dc, caption.c_str(), -1, &caption_rect,
              DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void App::draw_uml_overview(const DRAWITEMSTRUCT& item) {
    const int width = item.rcItem.right - item.rcItem.left;
    const int height = item.rcItem.bottom - item.rcItem.top;
    if (width <= 0 || height <= 0) return;
    HDC buffer = CreateCompatibleDC(item.hDC);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(item.hDC, width, height) : nullptr;
    HGDIOBJ old_bitmap = nullptr;
    if (buffer && bitmap) old_bitmap = SelectObject(buffer, bitmap);
    HDC dc = bitmap ? buffer : item.hDC;
    RECT area{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(RGB(250, 252, 255));
    FillRect(dc, &area, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    uml_hits_.clear();
    uml_minimap_ = RECT{};
    uml_minimap_scale_ = 0.0;
    if (!project_ || !uml_graph_ || uml_graph_->nodes.empty() ||
        width < 320 || height < 160) {
        DrawTextW(dc, L"Scan a project or enlarge the window to see its class graph.",
                  -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        std::optional<std::size_t> chosen;
        if (selected_ && selected_->symbol) {
            for (std::size_t i = 0; i < uml_graph_->nodes.size(); ++i) {
                const auto& node = uml_graph_->nodes[i];
                if (node.file == selected_->file && node.symbol == *selected_->symbol) {
                    chosen = i;
                    break;
                }
            }
        }
        const auto scene = atlas::layout_uml_graph(*uml_graph_,
                                                    uml_include_isolated_, chosen);
        if (uml_fit_pending_) {
            uml_scale_ = std::clamp(std::min(
                static_cast<double>(width - 40) / std::max(scene.width, 1),
                static_cast<double>(height - 90) / std::max(scene.height, 1)),
                0.65, 1.25);
            uml_offset_x_ = std::max(8.0, (width - scene.width * uml_scale_) / 2);
            uml_offset_y_ = 44.0 + std::max(0.0,
                (height - 90 - scene.height * uml_scale_) / 2);
            uml_fit_pending_ = false;
        }
        const auto box_rect = [&](const atlas::UmlRect& rect) {
            return RECT{
                static_cast<LONG>(std::lround(uml_offset_x_ + rect.left * uml_scale_)),
                static_cast<LONG>(std::lround(uml_offset_y_ + rect.top * uml_scale_)),
                static_cast<LONG>(std::lround(uml_offset_x_ + rect.right * uml_scale_)),
                static_cast<LONG>(std::lround(uml_offset_y_ + rect.bottom * uml_scale_))};
        };
        std::vector<RECT> positions(uml_graph_->nodes.size());
        for (const auto& placed : scene.nodes)
            positions[placed.node] = box_rect(placed.rect);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, 1, 34, width - 1, height - 34);
        for (const auto index : scene.edges) {
            const auto& edge = uml_graph_->edges[index];
            const bool inheritance = edge.relation == atlas::UmlRelation::inheritance;
            const auto from = inheritance ? edge.to : edge.from;
            const auto to = inheritance ? edge.from : edge.to;
            const RECT a = positions[from], b = positions[to];
            const int ax = (a.left + a.right) / 2, ay = (a.top + a.bottom) / 2;
            const int bx = (b.left + b.right) / 2, by = (b.top + b.bottom) / 2;
            const bool horizontal = std::abs(ax - bx) > 70;
            const int direction = horizontal ? (bx > ax ? 1 : -1)
                                             : (by > ay ? 1 : -1);
            const POINT start = horizontal ? POINT{direction > 0 ? a.right : a.left, ay}
                                           : POINT{ax, direction > 0 ? a.bottom : a.top};
            const POINT tip = horizontal ? POINT{direction > 0 ? b.left : b.right, by}
                                         : POINT{bx, direction > 0 ? b.top : b.bottom};
            const bool highlighted = chosen && (*chosen == from || *chosen == to);
            const COLORREF color = highlighted
                ? (inheritance ? RGB(46, 105, 171) : RGB(174, 105, 29))
                : (inheritance ? RGB(147, 178, 207) : RGB(212, 186, 156));
            HPEN pen = CreatePen(PS_SOLID, highlighted ? 2 : 1, color);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            const int curve = std::max(30, static_cast<int>(horizontal ?
                std::abs(tip.x - start.x) / 2 : std::abs(tip.y - start.y) / 2));
            POINT path[4] = {start, start, tip, tip};
            if (horizontal) { path[1].x += direction * curve; path[2].x -= direction * curve; }
            else { path[1].y += direction * curve; path[2].y -= direction * curve; }
            PolyBezier(dc, path, 4);
            const int arrow = std::clamp(static_cast<int>(8 * uml_scale_), 4, 9);
            POINT head[3]{};
            if (horizontal) {
                head[0] = tip;
                head[1] = {tip.x - direction * arrow, tip.y - arrow / 2};
                head[2] = {tip.x - direction * arrow, tip.y + arrow / 2};
            } else {
                head[0] = tip;
                head[1] = {tip.x - arrow / 2, tip.y - direction * arrow};
                head[2] = {tip.x + arrow / 2, tip.y - direction * arrow};
            }
            HGDIOBJ old_brush = SelectObject(dc,
                inheritance ? GetStockObject(WHITE_BRUSH) : GetStockObject(DC_BRUSH));
            if (!inheritance) SetDCBrushColor(dc, color);
            Polygon(dc, head, 3);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        for (const auto& placed : scene.nodes) {
            const auto& node = uml_graph_->nodes[placed.node];
            const RECT box = positions[placed.node];
            const bool active = chosen == placed.node;
            bool neighbor = !chosen;
            for (const auto index : scene.edges) {
                const auto& edge = uml_graph_->edges[index];
                if (chosen && ((edge.from == *chosen && edge.to == placed.node) ||
                               (edge.to == *chosen && edge.from == placed.node))) {
                    neighbor = true;
                    break;
                }
            }
            HBRUSH fill = CreateSolidBrush(active ? RGB(217, 235, 255) :
                neighbor ? RGB(255, 255, 255) : RGB(242, 245, 249));
            FillRect(dc, &box, fill);
            DeleteObject(fill);
            HBRUSH border = CreateSolidBrush(active ? RGB(35, 105, 185) :
                neighbor ? RGB(137, 157, 180) : RGB(207, 216, 225));
            FrameRect(dc, &box, border);
            DeleteObject(border);
            if (box.right - box.left >= 75 && box.bottom - box.top >= 28) {
                std::string label = node.label;
                if (const auto sep = label.rfind("::"); sep != std::string::npos)
                    label.erase(0, sep + 2);
                RECT title{box.left + 7, box.top + 5, box.right - 7, box.top + 27};
                SetTextColor(dc, active ? RGB(17, 68, 132) : RGB(31, 48, 70));
                const auto text = wide(label);
                DrawTextW(dc, text.c_str(), -1, &title,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (box.bottom - box.top > 44) {
                    RECT file{box.left + 7, box.top + 28, box.right - 7, box.bottom - 4};
                    SetTextColor(dc, RGB(93, 108, 125));
                    const auto path = project_->files[node.file].relative_path.filename().wstring();
                    DrawTextW(dc, path.c_str(), -1, &file,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
            uml_hits_.emplace_back(box, TreeItem{node.file, node.symbol});
        }
        RestoreDC(dc, saved);
        if (width >= 500 && height >= 260 && !scene.nodes.empty()) {
            const int mini_width = std::clamp(width / 5, 150, 210);
            const int mini_height = std::clamp(height / 5, 105, 145);
            uml_minimap_ = RECT{width - mini_width - 14,
                height - mini_height - 44, width - 14, height - 44};
            constexpr int inset = 8;
            uml_minimap_scale_ = std::min(
                static_cast<double>(mini_width - inset * 2) / std::max(scene.width, 1),
                static_cast<double>(mini_height - inset * 2) / std::max(scene.height, 1));
            HBRUSH mini_background = CreateSolidBrush(RGB(246, 249, 253));
            FillRect(dc, &uml_minimap_, mini_background);
            DeleteObject(mini_background);
            HBRUSH mini_border = CreateSolidBrush(RGB(137, 157, 180));
            FrameRect(dc, &uml_minimap_, mini_border);
            DeleteObject(mini_border);
            const auto mini_x = [&](double x) {
                return uml_minimap_.left + inset +
                    static_cast<int>(std::lround(x * uml_minimap_scale_));
            };
            const auto mini_y = [&](double y) {
                return uml_minimap_.top + inset +
                    static_cast<int>(std::lround(y * uml_minimap_scale_));
            };
            std::vector<POINT> centers(uml_graph_->nodes.size());
            for (const auto& placed : scene.nodes)
                centers[placed.node] = POINT{
                    mini_x((placed.rect.left + placed.rect.right) / 2.0),
                    mini_y((placed.rect.top + placed.rect.bottom) / 2.0)};
            HPEN mini_edge = CreatePen(PS_SOLID, 1, RGB(188, 202, 216));
            HGDIOBJ old_pen = SelectObject(dc, mini_edge);
            for (const auto index : scene.edges) {
                const auto& edge = uml_graph_->edges[index];
                MoveToEx(dc, centers[edge.from].x, centers[edge.from].y, nullptr);
                LineTo(dc, centers[edge.to].x, centers[edge.to].y);
            }
            SelectObject(dc, old_pen);
            DeleteObject(mini_edge);
            HBRUSH mini_node = CreateSolidBrush(RGB(108, 137, 168));
            HBRUSH mini_selected = CreateSolidBrush(RGB(36, 105, 185));
            for (const auto& placed : scene.nodes) {
                const auto center = centers[placed.node];
                RECT dot{center.x - 2, center.y - 2, center.x + 3, center.y + 3};
                FillRect(dc, &dot, chosen == placed.node ? mini_selected : mini_node);
            }
            DeleteObject(mini_node);
            DeleteObject(mini_selected);
            const auto clamp_x = [&](double value) {
                return mini_x(std::clamp(value, 0.0,
                    static_cast<double>(scene.width)));
            };
            const auto clamp_y = [&](double value) {
                return mini_y(std::clamp(value, 0.0,
                    static_cast<double>(scene.height)));
            };
            const RECT view{
                clamp_x(-uml_offset_x_ / uml_scale_),
                clamp_y((34.0 - uml_offset_y_) / uml_scale_),
                clamp_x((width - uml_offset_x_) / uml_scale_),
                clamp_y((height - 34.0 - uml_offset_y_) / uml_scale_)};
            if (view.right > view.left && view.bottom > view.top) {
                HPEN viewport = CreatePen(PS_SOLID, 2, RGB(35, 105, 185));
                old_pen = SelectObject(dc, viewport);
                HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(dc, view.left, view.top, view.right, view.bottom);
                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                DeleteObject(viewport);
            }
        }
        HBRUSH band = CreateSolidBrush(RGB(250, 252, 255));
        RECT top{0, 0, width, 34}, bottom{0, height - 34, width, height};
        FillRect(dc, &top, band);
        FillRect(dc, &bottom, band);
        DeleteObject(band);
        const auto heading = L"CLASS GRAPH  ·  " +
            std::to_wstring(uml_graph_->nodes.size()) + L" types  ·  " +
            std::to_wstring(scene.edges.size()) + L" links";
        RECT heading_rect{14, 8, width - 12, 30};
        SetTextColor(dc, RGB(28, 47, 69));
        DrawTextW(dc, heading.c_str(), -1, &heading_rect,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        const auto footer = L"Blue △ inheritance  ·  amber → field type  ·  " +
            std::to_wstring(scene.omitted_count) + L" isolated hidden  ·  " +
            L"click a class, double-click for members, drag / wheel or click the mini-map";
        RECT footer_rect{14, height - 27, width - 12, height - 6};
        SetTextColor(dc, RGB(84, 100, 117));
        DrawTextW(dc, footer.c_str(), -1, &footer_rect,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (bitmap) BitBlt(item.hDC, item.rcItem.left, item.rcItem.top,
                       width, height, dc, 0, 0, SRCCOPY);
    if (old_bitmap) SelectObject(buffer, old_bitmap);
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
}

void App::diagram_clicked() {
    if (!uml_overview_) return;
    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(diagram_, &point)) return;
    for (const auto& [box, target] : uml_hits_) {
        if (!PtInRect(&box, point)) continue;
        if (focus_tree_item(target)) update_tab();
        return;
    }
}

LRESULT App::canvas_input(HWND canvas, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!uml_overview_ || active_tab_ != 3) return DefSubclassProc(canvas, message, wparam, lparam);
    const POINT point{static_cast<short>(LOWORD(lparam)),
                      static_cast<short>(HIWORD(lparam))};
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
        uml_minimap_scale_ > 0.0 && PtInRect(&uml_minimap_, point)) {
        RECT bounds{};
        GetClientRect(canvas, &bounds);
        const double scene_x = (point.x - uml_minimap_.left - 8) / uml_minimap_scale_;
        const double scene_y = (point.y - uml_minimap_.top - 8) / uml_minimap_scale_;
        uml_offset_x_ = (bounds.right - bounds.left) / 2.0 - scene_x * uml_scale_;
        uml_offset_y_ = (bounds.bottom - bounds.top) / 2.0 - scene_y * uml_scale_;
        uml_fit_pending_ = false;
        InvalidateRect(canvas, nullptr, FALSE);
        return 0;
    }
    switch (message) {
    case WM_LBUTTONDOWN:
        uml_dragging_ = true;
        uml_moved_ = false;
        uml_last_mouse_ = point;
        SetCapture(canvas);
        return 0;
    case WM_MOUSEMOVE:
        if (uml_dragging_ && (wparam & MK_LBUTTON)) {
            const int dx = point.x - uml_last_mouse_.x;
            const int dy = point.y - uml_last_mouse_.y;
            if (dx || dy) {
                if (std::abs(dx) + std::abs(dy) > 2) uml_moved_ = true;
                uml_offset_x_ += dx;
                uml_offset_y_ += dy;
                uml_fit_pending_ = false;
                uml_last_mouse_ = point;
                InvalidateRect(canvas, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (uml_dragging_) {
            uml_dragging_ = false;
            ReleaseCapture();
            if (!uml_moved_) diagram_clicked();
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        diagram_clicked();
        if (selected_ && selected_->symbol) {
            uml_overview_ = false;
            update_tab();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT client{static_cast<short>(LOWORD(lparam)),
                     static_cast<short>(HIWORD(lparam))};
        ScreenToClient(canvas, &client);
        const double before = uml_scale_;
        const int delta = static_cast<short>(HIWORD(wparam));
        uml_scale_ = std::clamp(before * (delta > 0 ? 1.15 : 1.0 / 1.15),
                                0.16, 3.0);
        uml_offset_x_ = client.x - (client.x - uml_offset_x_) * uml_scale_ / before;
        uml_offset_y_ = client.y - (client.y - uml_offset_y_) * uml_scale_ / before;
        uml_fit_pending_ = false;
        InvalidateRect(canvas, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        uml_dragging_ = false;
        break;
    default:
        break;
    }
    return DefSubclassProc(canvas, message, wparam, lparam);
}

LRESULT CALLBACK uml_canvas_proc(HWND canvas, UINT message, WPARAM wparam,
                                 LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(canvas, uml_canvas_proc, 1);
        return DefSubclassProc(canvas, message, wparam, lparam);
    }
    return reinterpret_cast<App*>(data)->canvas_input(canvas, message, wparam, lparam);
}

void App::draw_runtime(const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    RECT area = item.rcItem;
    FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font_);
    if (!runtime_snapshot_) {
        DrawTextW(dc, L"Record an instrumented executable or load a runtime .trace.",
                  -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width < 360 || height < 140) {
        DrawTextW(dc, L"Enlarge the window to see the runtime map.",
                  -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    RECT variables_title{area.left + 20, area.top + 16, area.left + width / 2, area.top + 42};
    RECT objects_title{area.left + width / 2, area.top + 16, area.right - 20, area.top + 42};
    DrawTextW(dc, L"VARIABLES / CALL SITES", -1, &variables_title, DT_LEFT | DT_SINGLELINE);
    DrawTextW(dc, L"OBJECTS", -1, &objects_title, DT_RIGHT | DT_SINGLELINE);
    if (runtime_snapshot_->nodes.empty()) {
        RECT message{area.left + 20, area.top + 50, area.right - 20, area.bottom - 15};
        DrawTextW(dc, L"No active nodes at this step. Move back on the timeline to inspect their lifetimes.",
                  -1, &message, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    const int box_width = std::clamp(width / 3, 140, 240);
    const int max_rows = std::max(1, (height - 105) / 62);
    std::map<std::string, RECT> boxes;
    int variable_row = 0, object_row = 0;
    for (const auto& node : runtime_snapshot_->nodes) {
        const bool left_column = node.kind != atlas::RuntimeNodeKind::object;
        int& row = left_column ? variable_row : object_row;
        if (row >= max_rows) { ++row; continue; }
        const int x = left_column
            ? area.left + 20 : area.right - 20 - box_width;
        const int y = area.top + 55 + row * 62;
        boxes.emplace(node.id, RECT{x, y, x + box_width, y + 44});
        ++row;
    }
    for (const auto& link : runtime_snapshot_->links) {
        const auto source = boxes.find(link.source), target = boxes.find(link.target);
        if (source == boxes.end() || target == boxes.end()) continue;
        const int x1 = source->second.right, y1 = (source->second.top + source->second.bottom) / 2;
        const int x2 = target->second.left, y2 = (target->second.top + target->second.bottom) / 2;
        const COLORREF color = link.relation == "owns" ? RGB(156, 86, 28)
                               : link.relation == "points_to" ? RGB(40, 95, 160)
                               : RGB(90, 96, 105);
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HGDIOBJ previous = SelectObject(dc, pen);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
        const int arrow_back = x2 >= x1 ? -7 : 7;
        LineTo(dc, x2 + arrow_back, y2 - 4);
        MoveToEx(dc, x2, y2, nullptr);
        LineTo(dc, x2 + arrow_back, y2 + 4);
        SelectObject(dc, previous);
        DeleteObject(pen);
        const auto relation = wide(link.relation);
        RECT label{std::min(x1, x2), (y1 + y2) / 2 - 18,
                   std::max(x1, x2), (y1 + y2) / 2};
        SetTextColor(dc, color);
        DrawTextW(dc, relation.c_str(), -1, &label,
                  DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    for (const auto& node : runtime_snapshot_->nodes) {
        const auto found = boxes.find(node.id);
        if (found == boxes.end()) continue;
        const RECT box = found->second;
        HBRUSH brush = CreateSolidBrush(
            node.kind == atlas::RuntimeNodeKind::site ? RGB(243, 238, 223)
            : node.kind == atlas::RuntimeNodeKind::variable ? RGB(231, 240, 250)
                                                       : RGB(232, 245, 237));
        FillRect(dc, &box, brush);
        DeleteObject(brush);
        FrameRect(dc, &box, reinterpret_cast<HBRUSH>(COLOR_WINDOWFRAME + 1));
        RECT title{box.left + 8, box.top + 4, box.right - 8, box.top + 23};
        RECT id{box.left + 8, box.top + 23, box.right - 8, box.bottom - 3};
        SetTextColor(dc, RGB(32, 39, 48));
        const auto name = wide(node.label);
        DrawTextW(dc, name.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, RGB(79, 88, 97));
        const auto identity = wide(node.id);
        DrawTextW(dc, identity.c_str(), -1, &id, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (variable_row > max_rows || object_row > max_rows) {
        RECT note{area.left + 20, area.bottom - 30, area.right - 20, area.bottom - 8};
        SetTextColor(dc, RGB(80, 80, 80));
        DrawTextW(dc, L"More nodes are active than fit in this view; inspect the full graph via the CLI.",
                  -1, &note, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        try {
            app = new App(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->start_ui_server();
            return 0;
        } catch (...) { return -1; }
    case WM_SIZE: if (app) app->layout(); return 0;
    case WM_TIMER: if (app && wparam == 1) app->refresh_debug_view(); return 0;
    case WM_GETMINMAXINFO: if (app) app->resize_min(reinterpret_cast<MINMAXINFO*>(lparam)); return 0;
    case WM_COMMAND: if (app) app->command(LOWORD(wparam), HIWORD(wparam)); return 0;
    case WM_HSCROLL:
        if (app && reinterpret_cast<HWND>(lparam) == GetDlgItem(window, id_runtime_timeline)) {
            app->set_runtime_step(static_cast<std::size_t>(SendMessageW(
                reinterpret_cast<HWND>(lparam), TBM_GETPOS, 0, 0)));
            return 0;
        }
        break;
    case WM_CLOSE: if (app) app->request_close(); return 0;
    case WM_NOTIFY: if (app) app->notify(reinterpret_cast<const NMHDR*>(lparam)); return 0;
    case WM_DRAWITEM:
        if (app) {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (item.CtlID == id_runtime_canvas) app->draw_runtime(item);
            else app->draw_diagram(item);
        }
        return TRUE;
    case scan_done_message:
        if (app) app->scan_finished(reinterpret_cast<ScanResult*>(lparam));
        else delete reinterpret_cast<ScanResult*>(lparam);
        return 0;
    case capture_done_message:
        if (app) app->capture_finished(reinterpret_cast<CaptureFinished*>(lparam));
        else delete reinterpret_cast<CaptureFinished*>(lparam);
        return 0;
    case ui_command_message:
        if (app) {
            auto* dispatch = reinterpret_cast<UiDispatch*>(lparam);
            if (dispatch && !dispatch->cancelled.load()) {
                try { dispatch->reply = app->handle_ui_command(dispatch->command); }
                catch (const std::exception& error) {
                    dispatch->reply.pid = GetCurrentProcessId();
                    copy_ui_text(dispatch->reply.error, wide(error.what()));
                } catch (...) {
                    dispatch->reply.pid = GetCurrentProcessId();
                    copy_ui_text(dispatch->reply.error, L"unexpected GUI command failure");
                }
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F5 && app) { app->command(id_scan, 0); return 0; }
        break;
    case WM_DESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete app;
        PostQuitMessage(smoke_exit_code);
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES |
        ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = main_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&cls)) return 1;
    HWND window = CreateWindowExW(0, main_class, L"Project Atlas — C++ project navigator",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1160, 760,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool smoke = false;
    std::optional<fs::path> startup_project;
    std::optional<std::pair<fs::path, fs::path>> smoke_facts;
    std::optional<fs::path> smoke_runtime;
    if (argv) {
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--smoke") {
            smoke = true;
            startup_project = fs::path(argv[2]);
            if (argc >= 5) smoke_facts = std::pair{fs::path(argv[3]), fs::path(argv[4])};
            if (argc >= 6) smoke_runtime = fs::path(argv[5]);
        } else if (argc >= 4 && std::wstring_view(argv[1]) == L"--runtime-demo") {
            startup_project = fs::path(argv[2]);
            smoke_runtime = fs::path(argv[3]);
        } else if (argc >= 2) {
            startup_project = fs::path(argv[1]);
        }
        LocalFree(argv);
    }
    if (startup_project) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        app->open_project(*startup_project, smoke, smoke_facts, smoke_runtime);
    }
    if (smoke && !startup_project) {
        DestroyWindow(window);
        CoUninitialize();
        return 2;
    }
    ShowWindow(window, smoke ? SW_HIDE : show);
    UpdateWindow(window);
    ACCEL refresh_accelerator{};
    refresh_accelerator.fVirt = FVIRTKEY;
    refresh_accelerator.key = VK_F5;
    refresh_accelerator.cmd = id_scan;
    HACCEL accelerators = CreateAcceleratorTableW(&refresh_accelerator, 1);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!accelerators || !TranslateAcceleratorW(window, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (accelerators) DestroyAcceleratorTable(accelerators);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
