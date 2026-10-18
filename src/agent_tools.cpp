#include "atlas/agent_tools.hpp"
#include "atlas/interactions.hpp"
#include "atlas/symbol_identity.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace atlas {
namespace {

std::string path_text(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

std::string hash_text(std::uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool within_root(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative)
        if (part == "..") return false;
    return true;
}

bool valid_utf8(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) { ++i; continue; }
        const std::size_t width = lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 :
            lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!width || i + width > bytes.size()) return false;
        for (std::size_t j = 1; j < width; ++j)
            if ((static_cast<unsigned char>(bytes[i + j]) & 0xc0) != 0x80)
                return false;
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        if ((lead == 0xe0 && second < 0xa0) ||
            (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) ||
            (lead == 0xf4 && second >= 0x90)) return false;
        i += width;
    }
    return true;
}

} // namespace

std::expected<std::string, std::string> agent_source_json(
    const fs::path& root, const fs::path& relative_path,
    std::size_t start_line, std::size_t line_count,
    const std::optional<std::string_view>& expected_hash) {
    if (!within_root(relative_path) || !start_line || !line_count || line_count > 80)
        return std::unexpected("source requires a relative file and 1..80 lines");
    const auto extension = relative_path.extension();
    if (extension != ".c" && extension != ".cc" && extension != ".cpp" &&
        extension != ".cxx" && extension != ".h" && extension != ".hpp" &&
        extension != ".hxx" && extension != ".inl" && extension != ".tpp" &&
        extension != ".ixx")
        return std::unexpected("source requires a C/C++ file");
    std::error_code error;
    const auto canonical_root = fs::canonical(root, error);
    if (error) return std::unexpected("project root is unavailable");
    const auto canonical_file = fs::canonical(canonical_root / relative_path, error);
    if (error || !fs::is_regular_file(canonical_file))
        return std::unexpected("source file is unavailable");
    const auto inside = canonical_file.lexically_relative(canonical_root);
    if (!within_root(inside)) return std::unexpected("source file escapes project root");
    const auto size = fs::file_size(canonical_file, error);
    if (error || size > 2 * 1024 * 1024)
        return std::unexpected("source file exceeds 2 MiB limit");
    std::ifstream input(canonical_file, std::ios::binary);
    if (!input) return std::unexpected("cannot read source file");
    const std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (!valid_utf8(bytes)) return std::unexpected("source file is not UTF-8");
    const auto hash = hash_text(fnv1a64(bytes));
    if (expected_hash && *expected_hash != hash)
        return std::unexpected("source hash changed; refresh Atlas context");
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        auto line = std::string_view(bytes).substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (bytes.empty()) lines.emplace_back();
    if (start_line > lines.size()) return std::unexpected("source start line is past EOF");
    std::ostringstream prefix;
    prefix << "{\"schema\":\"project-atlas.source.v1\",\"operation\":\"source\""
           << ",\"path\":" << quote(path_text(inside))
           << ",\"content_hash_fnv1a64\":" << quote(hash)
           << ",\"total_lines\":" << lines.size()
           << ",\"start_line\":" << start_line
           << ",\"requested_lines\":" << line_count << ",\"lines\":[";
    std::string output = prefix.str();
    std::size_t returned = 0;
    const auto available = std::min(line_count, lines.size() - start_line + 1);
    for (std::size_t i = 0; i < available; ++i) {
        const auto item = "{\"line\":" + std::to_string(start_line + i) +
            ",\"text\":" + quote(lines[start_line + i - 1]) + '}';
        if (output.size() + item.size() + 80 > 16 * 1024) break;
        if (returned) output += ',';
        output += item;
        ++returned;
    }
    if (!returned) return std::unexpected("source line exceeds 16 KiB response limit");
    output += "],\"returned_lines\":" + std::to_string(returned) +
        ",\"truncated\":" + (returned < available ? "true" : "false") + '}';
    return output;
}

std::expected<std::string, std::string> agent_file_relations_json(
    const ProjectScan& scan, const fs::path& relative_path, std::size_t max_edges) {
    if (!within_root(relative_path) || !max_edges || max_edges > 200)
        return std::unexpected("relations require a relative file and 1..200 edges");
    const auto normalized = relative_path.lexically_normal();
    const auto found = std::ranges::find_if(scan.files, [&](const SourceFile& file) {
        return file.relative_path == normalized;
    });
    if (found == scan.files.end())
        return std::unexpected("file is not in the current Atlas scan");
    const auto graph = build_interaction_graph(scan);
    const auto node_id = "file:" + path_text(normalized);
    const auto node = std::ranges::find_if(graph.nodes, [&](const InteractionNode& item) {
        return item.id == node_id;
    });
    if (node == graph.nodes.end()) return std::unexpected("file node is unavailable");
    const auto index = static_cast<std::size_t>(node - graph.nodes.begin());
    std::size_t total = 0, returned = 0;
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.file-relations.v1\""
        << ",\"operation\":\"file-relations\",\"path\":" << quote(path_text(normalized))
        << ",\"content_hash_fnv1a64\":" << quote(hash_text(found->content_hash))
        << ",\"parsed\":" << (found->parsed ? "true" : "false")
        << ",\"clang_errors\":" << found->error_diagnostic_count
        << ",\"clang_warnings\":" << found->warning_diagnostic_count
        << ",\"edges\":[";
    for (const auto& edge : graph.edges) {
        if (edge.from != index && edge.to != index) continue;
        ++total;
        if (returned >= max_edges) continue;
        if (returned) out << ',';
        const auto direction = edge.from == index && edge.to == index ? "internal" :
            edge.from == index ? "outgoing" : "incoming";
        const auto other = edge.from == index ? edge.to : edge.from;
        out << "{\"direction\":" << quote(direction)
            << ",\"kind\":" << quote(interaction_kind_name(edge.kind))
            << ",\"other_file\":" << quote(graph.nodes[other].label)
            << ",\"detail\":" << quote(edge.detail)
            << ",\"count\":" << edge.count
            << ",\"first_line\":" << edge.first_line
            << ",\"first_column\":" << edge.first_column
            << ",\"dynamic_dispatch\":" << (edge.dynamic_dispatch ? "true" : "false")
            << ",\"indirect\":" << (edge.indirect ? "true" : "false") << '}';
        ++returned;
    }
    out << "],\"edge_count\":" << total
        << ",\"returned_edges\":" << returned
        << ",\"truncated\":" << (returned < total ? "true" : "false")
        << ",\"limitations\":\"Direct AST relations only; missing or dynamic calls and runtime effects are not inferred\"}";
    return out.str();
}

} // namespace atlas
