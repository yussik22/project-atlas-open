#include "atlas/card_archive.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

namespace atlas {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view card_marker = "<!-- Project Atlas generated card -->";

bool ignored_directory(const fs::path& path) {
    const auto name = path.filename().string();
    static constexpr std::array<std::string_view, 10> ignored{
        ".git", ".venv", ".project-atlas", "__pycache__", "build", "dist",
        "node_modules", ".vendor", ".vs", "atlas-test-workspace"};
    for (const auto item : ignored) if (name == item) return true;
    return name.starts_with("build-") || name.starts_with("cmake-build-");
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && relative != "." &&
           *relative.begin() != "..";
}

std::expected<bool, std::string> has_atlas_marker(const fs::path& card) {
    std::ifstream input(card, std::ios::binary);
    if (!input) return std::unexpected("cannot read source card: " + card.string());
    std::string first_line;
    std::getline(input, first_line);
    if (input.bad()) return std::unexpected("cannot read source card: " + card.string());
    return first_line == card_marker;
}

std::expected<bool, std::string> identical_files(const fs::path& left,
                                                  const fs::path& right) {
    std::error_code ec;
    const auto left_size = fs::file_size(left, ec);
    if (ec) return std::unexpected("cannot stat source card: " + left.string());
    const auto right_size = fs::file_size(right, ec);
    if (ec) return std::unexpected("cannot stat archived card: " + right.string());
    if (left_size != right_size) return false;
    std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
    if (!a || !b) return std::unexpected("cannot verify archived card");
    std::array<char, 8192> x{}, y{};
    while (a && b) {
        a.read(x.data(), x.size());
        b.read(y.data(), y.size());
        if (a.gcount() != b.gcount() ||
            std::memcmp(x.data(), y.data(), static_cast<std::size_t>(a.gcount())) != 0)
            return false;
    }
    if (a.bad() || b.bad()) return std::unexpected("cannot verify archived card");
    return true;
}

std::expected<void, std::string> archive_card(const fs::path& base,
                                              const fs::path& card,
                                              const fs::path& archive_root) {
    const auto relative = card.lexically_relative(base);
    if (!within(card, base) || relative.empty())
        return std::unexpected("card is outside its generated area: " + card.string());
    std::error_code ec;
    const auto resolved_card = fs::weakly_canonical(card, ec);
    if (ec || !within(resolved_card, base))
        return std::unexpected("card resolves outside its generated area: " + card.string());
    auto destination = archive_root / relative;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return std::unexpected("cannot create card archive: " + ec.message());
    // A pre-existing symlink or junction must not redirect the archive outside
    // the project. Existing archived cards are never overwritten.
    const auto resolved_parent = fs::weakly_canonical(destination.parent_path(), ec);
    if (ec || !within(resolved_parent, base))
        return std::unexpected("card archive resolves outside its generated area");
    for (std::size_t suffix = 0;; ++suffix) {
        if (suffix) destination = fs::path((archive_root / relative).string() +
                                           "." + std::to_string(suffix));
        const auto status = fs::symlink_status(destination, ec);
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        if (ec) return std::unexpected("cannot inspect card archive: " + ec.message());
        if (status.type() != fs::file_type::not_found) continue;
        if (!fs::copy_file(card, destination, fs::copy_options::none, ec)) {
            if (ec == std::errc::file_exists) { ec.clear(); continue; }
            return std::unexpected("cannot archive source card: " + ec.message());
        }
        const auto same = identical_files(card, destination);
        if (!same || !*same)
            return std::unexpected(same ? "archived card differs from source" : same.error());
        if (!fs::remove(card, ec) || ec)
            return std::unexpected("archived card copied, but original could not be removed: " +
                                   card.string());
        return {};
    }
}

} // namespace

std::expected<std::size_t, std::string> archive_orphan_source_cards(
    const fs::path& project_root, const fs::path& report_output) {
    std::error_code ec;
    const auto root = fs::weakly_canonical(project_root, ec);
    if (ec || !fs::is_directory(root))
        return std::unexpected("project root is not a directory");
    const auto output = fs::weakly_canonical(report_output, ec);
    if (ec) return std::unexpected("cannot resolve report output");
    std::vector<fs::path> orphans;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::unexpected("cannot inspect project cards: " + ec.message());
    const fs::recursive_directory_iterator end;
    const auto suffix = fs::path(".agent.md").native();
    while (it != end) {
        const auto entry = *it;
        const auto path = entry.path();
        if (entry.is_directory(ec)) {
            const auto resolved = fs::weakly_canonical(path, ec);
            if (ec) return std::unexpected("cannot resolve project directory: " + path.string());
            if (entry.is_symlink(ec) || !within(resolved, root) ||
                ignored_directory(path) || path == output)
                it.disable_recursion_pending();
        } else if (!entry.is_symlink(ec) && entry.is_regular_file(ec)) {
            const auto name = path.filename().native();
            if (name.ends_with(suffix)) {
                auto source = path.parent_path() /
                    fs::path(name.substr(0, name.size() - suffix.size()));
                if (!fs::exists(source, ec)) {
                    if (ec) return std::unexpected("cannot inspect source: " + ec.message());
                    const auto marked = has_atlas_marker(path);
                    if (!marked) return std::unexpected(marked.error());
                    if (*marked) orphans.push_back(path);
                }
            }
        }
        if (ec) return std::unexpected("cannot inspect project cards: " + ec.message());
        it.increment(ec);
        if (ec) return std::unexpected("cannot continue card scan: " + ec.message());
    }
    for (const auto& card : orphans) {
        if (auto result = archive_card(root, card,
                root / ".project-atlas" / "orphans" / "source"); !result)
            return std::unexpected(result.error());
    }
    return orphans.size();
}

std::expected<std::size_t, std::string> archive_orphan_report_cards(
    const fs::path& project_root, const fs::path& report_output) {
    std::error_code ec;
    const auto root = fs::weakly_canonical(project_root, ec);
    if (ec || !fs::is_directory(root))
        return std::unexpected("project root is not a directory");
    const auto output = fs::weakly_canonical(report_output, ec);
    if (ec || !fs::is_directory(output))
        return std::unexpected("report output is not a directory");
    std::vector<fs::path> orphans;
    fs::recursive_directory_iterator it(output, fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::unexpected("cannot inspect report cards: " + ec.message());
    const fs::recursive_directory_iterator end;
    const auto card_suffix = fs::path(".agent.md").native();
    const auto summary_suffix = fs::path(".md").native();
    while (it != end) {
        const auto entry = *it;
        const auto path = entry.path();
        if (entry.is_directory(ec)) {
            const auto resolved = fs::weakly_canonical(path, ec);
            if (ec) return std::unexpected("cannot resolve report directory: " + path.string());
            if (entry.is_symlink(ec) || !within(resolved, output) ||
                path == output / ".orphans")
                it.disable_recursion_pending();
        } else if (!entry.is_symlink(ec) && entry.is_regular_file(ec)) {
            const auto relative = path.lexically_relative(output);
            const auto name = path.filename().native();
            fs::path source_relative;
            if (name.ends_with(card_suffix) && relative.begin()->native() !=
                    fs::path("summaries").native()) {
                source_relative = relative.parent_path() /
                    fs::path(name.substr(0, name.size() - card_suffix.size()));
            } else if (name.ends_with(summary_suffix) &&
                       relative.begin()->native() == fs::path("summaries").native()) {
                source_relative = relative.lexically_relative("summaries").parent_path() /
                    fs::path(name.substr(0, name.size() - summary_suffix.size()));
            }
            if (!source_relative.empty() && !fs::exists(root / source_relative, ec)) {
                if (ec) return std::unexpected("cannot inspect source: " + ec.message());
                const auto marked = has_atlas_marker(path);
                if (!marked) return std::unexpected(marked.error());
                if (*marked) orphans.push_back(path);
            }
        }
        if (ec) return std::unexpected("cannot inspect report cards: " + ec.message());
        it.increment(ec);
        if (ec) return std::unexpected("cannot continue report scan: " + ec.message());
    }
    for (const auto& card : orphans) {
        if (auto result = archive_card(output, card, output / ".orphans"); !result)
            return std::unexpected(result.error());
    }
    return orphans.size();
}

} // namespace atlas
