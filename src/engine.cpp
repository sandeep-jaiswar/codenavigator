#include "codenavigator/engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace codenavigator {
namespace {

constexpr std::string_view kIndexMagic = "CNAVIDX1";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
constexpr std::size_t kMaximumFileBytes = 4 * 1024 * 1024;

std::string lower_copy(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::uint64_t hash64(std::string_view value, std::uint64_t seed = kHashOffset) {
    std::uint64_t result = seed;
    for (const unsigned char character : value) {
        result ^= character;
        result *= kHashPrime;
    }
    return result;
}

StableId stable_id(std::string_view value) {
    return {
        hash64(value, kHashOffset),
        hash64(value, kHashOffset ^ 0x9e3779b97f4a7c15ULL),
    };
}

std::string normalized_path(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

std::string language_for(const std::filesystem::path& path) {
    static const std::unordered_map<std::string, std::string> languages{
        {".c", "c"},       {".h", "cpp"},      {".cc", "cpp"},    {".cpp", "cpp"},
        {".cxx", "cpp"},   {".hpp", "cpp"},    {".hh", "cpp"},    {".m", "objective-c"},
        {".mm", "objective-c++"}, {".cs", "csharp"}, {".py", "python"}, {".java", "java"},
        {".kt", "kotlin"}, {".kts", "kotlin"}, {".scala", "scala"}, {".js", "javascript"},
        {".jsx", "javascript"}, {".mjs", "javascript"}, {".cjs", "javascript"},
        {".ts", "typescript"}, {".tsx", "typescript"}, {".go", "go"}, {".rs", "rust"},
        {".rb", "ruby"},   {".php", "php"},    {".swift", "swift"}, {".dart", "dart"},
        {".lua", "lua"},   {".zig", "zig"},    {".sh", "bash"},    {".bash", "bash"},
        {".ex", "elixir"}, {".exs", "elixir"}, {".hs", "haskell"}, {".lhs", "haskell"},
    };
    const auto iterator = languages.find(lower_copy(path.extension().string()));
    return iterator == languages.end() ? std::string{} : iterator->second;
}

bool is_skipped_directory(std::string_view name) {
    static const std::unordered_set<std::string> skipped{
        ".git", ".hg", ".svn", ".idea", ".vscode", "node_modules", "vendor", "dist", "build",
        "target", "coverage", ".cache", ".gradle", ".next", ".nuxt", "__pycache__",
        "cmake-build-debug", "cmake-build-release", "cmake-build-relwithdebinfo",
    };
    return skipped.contains(std::string(name)) || name.starts_with("cmake-build-") ||
           name.starts_with("build-");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumFileBytes) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    return input ? content : std::string{};
}

std::vector<std::string> lines_of(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (!content.empty() && content.back() == '\n') {
        lines.emplace_back();
    }
    return lines;
}

struct DefinitionMatch {
    std::string name;
    std::string signature;
    std::string kind;
};

std::optional<DefinitionMatch> definition_for(const std::string& language, const std::string& line) {
    static const std::regex python(R"(^\s*(?:async\s+)?(def|class)\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    static const std::regex go(R"(^\s*(?:func\s+(?:\([^)]*\)\s*)?|type\s+)([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(|struct\b|interface\b))");
    static const std::regex rust(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(fn|struct|enum|trait|type|mod)\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    static const std::regex javascript(R"(^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?(function|class|interface|type|enum)\s+([A-Za-z_$][A-Za-z0-9_$]*))");
    static const std::regex javascript_arrow(R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*(?:async\s*)?(?:\([^)]*\)|[A-Za-z_$][A-Za-z0-9_$]*)\s*=>)");
    static const std::regex ruby(R"(^\s*(def|class|module)\s+(?:self\.)?([A-Za-z_][A-Za-z0-9_!?=]*))");
    static const std::regex php(R"(^\s*(?:(?:public|private|protected|static|final|abstract)\s+)*(function|class|interface|trait|enum)\s+([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex swift(R"(^\s*(?:(?:public|private|internal|open|final|static|class)\s+)*(func|class|struct|enum|protocol|actor)\s+([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex bash_a(R"(^\s*function\s+([A-Za-z_][A-Za-z0-9_]*)\s*)");
    static const std::regex bash_b(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\s*\{)");
    static const std::regex elixir(R"(^\s*(defmodule|defprotocol|defimpl|defp?|defmacro)\s+([A-Za-z_][A-Za-z0-9_.!?]*))");
    static const std::regex haskell(R"(^([a-z_][A-Za-z0-9_']*)\s*::)");
    static const std::regex c_class(R"(^\s*(?:(?:public|private|protected|internal|final|abstract|sealed|static)\s+)*(class|struct|interface|enum|record|trait)\s+([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex c_function(R"(^\s*(?:(?:public|private|protected|internal|virtual|static|inline|constexpr|consteval|extern|async|suspend|override|final)\s+)*(?:[A-Za-z_~][A-Za-z0-9_:<>,.?*&\[\]\s]*\s+)([A-Za-z_~][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:\{|=>|$))");

    std::smatch match;
    const std::string signature = trim_copy(line);
    auto result = [&](std::size_t kind_group, std::size_t name_group) -> std::optional<DefinitionMatch> {
        return DefinitionMatch{match[name_group].str(), signature, match[kind_group].str()};
    };

    if (language == "python" && std::regex_search(line, match, python)) {
        return result(1, 2);
    }
    if (language == "go" && std::regex_search(line, match, go)) {
        return DefinitionMatch{match[1].str(), signature, "symbol"};
    }
    if (language == "rust" && std::regex_search(line, match, rust)) {
        return result(1, 2);
    }
    if ((language == "javascript" || language == "typescript") &&
        std::regex_search(line, match, javascript)) {
        return result(1, 2);
    }
    if ((language == "javascript" || language == "typescript") &&
        std::regex_search(line, match, javascript_arrow)) {
        return DefinitionMatch{match[1].str(), signature, "function"};
    }
    if (language == "ruby" && std::regex_search(line, match, ruby)) {
        return result(1, 2);
    }
    if (language == "php" && std::regex_search(line, match, php)) {
        return result(1, 2);
    }
    if (language == "swift" && std::regex_search(line, match, swift)) {
        return result(1, 2);
    }
    if (language == "bash" && std::regex_search(line, match, bash_a)) {
        return DefinitionMatch{match[1].str(), signature, "function"};
    }
    if (language == "bash" && std::regex_search(line, match, bash_b)) {
        return DefinitionMatch{match[1].str(), signature, "function"};
    }
    if (language == "elixir" && std::regex_search(line, match, elixir)) {
        return result(1, 2);
    }
    if (language == "haskell" && std::regex_search(line, match, haskell)) {
        return DefinitionMatch{match[1].str(), signature, "function"};
    }
    if (std::regex_search(line, match, c_class)) {
        return result(1, 2);
    }
    if (std::regex_search(line, match, c_function)) {
        return DefinitionMatch{match[1].str(), signature, "function"};
    }
    return std::nullopt;
}

std::vector<std::string> imports_for(const std::string& language, const std::string& line) {
    static const std::array<std::regex, 7> patterns{
        std::regex(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])"),
        std::regex(R"(^\s*(?:from\s+([A-Za-z0-9_.]+)\s+import|import\s+([A-Za-z0-9_.]+)))"),
        std::regex(R"(^\s*import\b.*?from\s*["']([^"']+)["'])"),
        std::regex(R"(\brequire\s*\(\s*["']([^"']+)["']\s*\))"),
        std::regex(R"(^\s*(?:pub\s+)?use\s+([A-Za-z0-9_:]+))"),
        std::regex(R"(^\s*import\s+(?:\([^)]*\)\s*)?["']([^"']+)["'])"),
        std::regex(R"(^\s*(?:require|load|source)\s+["']?([^"'\s]+))"),
    };
    std::vector<std::string> results;
    for (const auto& pattern : patterns) {
        std::smatch match;
        if (!std::regex_search(line, match, pattern)) {
            continue;
        }
        for (std::size_t index = 1; index < match.size(); ++index) {
            if (match[index].matched && !match[index].str().empty()) {
                results.push_back(match[index].str());
                break;
            }
        }
    }
    if (language == "elixir") {
        static const std::regex alias_pattern(R"(^\s*(?:alias|import|use|require)\s+([A-Za-z0-9_.]+))");
        std::smatch match;
        if (std::regex_search(line, match, alias_pattern)) {
            results.push_back(match[1].str());
        }
    }
    return results;
}

std::vector<std::string> calls_for(const std::string& line) {
    static const std::regex call_pattern(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    static const std::unordered_set<std::string> excluded{
        "if", "for", "while", "switch", "catch", "return", "sizeof", "decltype", "alignof",
        "new", "delete", "throw", "match", "when", "with", "def", "class", "function",
        "func", "fn", "macro", "typeof", "defined", "assert",
    };
    std::vector<std::string> results;
    for (std::sregex_iterator iterator(line.begin(), line.end(), call_pattern), end; iterator != end; ++iterator) {
        const std::string name = (*iterator)[1].str();
        if (!excluded.contains(name)) {
            results.push_back(name);
        }
    }
    return results;
}

template <typename Value>
void write_value(std::ostream& output, const Value& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(Value));
}

template <typename Value>
Value read_value(std::istream& input) {
    Value value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(Value));
    if (!input) {
        throw std::runtime_error("truncated CodeNavigator index");
    }
    return value;
}

void write_string(std::ostream& output, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    write_value(output, size);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_string(std::istream& input) {
    const auto size = read_value<std::uint32_t>(input);
    if (size > 64 * 1024 * 1024) {
        throw std::runtime_error("invalid string in CodeNavigator index");
    }
    std::string value(size, '\0');
    input.read(value.data(), static_cast<std::streamsize>(size));
    if (!input) {
        throw std::runtime_error("truncated CodeNavigator index");
    }
    return value;
}

Json span_json(const SourceSpan& span) {
    Json result = Json::object();
    result["line_start"] = static_cast<std::int64_t>(span.line_start);
    result["column_start"] = static_cast<std::int64_t>(span.column_start);
    result["line_end"] = static_cast<std::int64_t>(span.line_end);
    result["column_end"] = static_cast<std::int64_t>(span.column_end);
    return result;
}

std::uint64_t unix_time() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

int precision_weight(Precision precision) {
    switch (precision) {
        case Precision::exact: return 1000;
        case Precision::structural: return 800;
        case Precision::heuristic: return 500;
        case Precision::ambiguous: return 250;
    }
    return 0;
}

bool precision_allowed(Precision value, Precision minimum) {
    return static_cast<int>(value) <= static_cast<int>(minimum);
}

}  // namespace

std::string StableId::str() const {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << high << std::setw(16) << low;
    return output.str();
}

std::optional<StableId> StableId::parse(std::string_view text) {
    if (text.size() != 32) {
        return std::nullopt;
    }
    StableId result;
    const auto high_result = std::from_chars(text.data(), text.data() + 16, result.high, 16);
    const auto low_result = std::from_chars(text.data() + 16, text.data() + 32, result.low, 16);
    if (high_result.ec != std::errc{} || low_result.ec != std::errc{}) {
        return std::nullopt;
    }
    return result;
}

std::string to_string(Precision precision) {
    switch (precision) {
        case Precision::exact: return "exact";
        case Precision::structural: return "structural";
        case Precision::heuristic: return "heuristic";
        case Precision::ambiguous: return "ambiguous";
    }
    return "ambiguous";
}

std::string to_string(NodeKind kind) {
    switch (kind) {
        case NodeKind::root: return "root";
        case NodeKind::file: return "file";
        case NodeKind::symbol: return "symbol";
    }
    return "symbol";
}

std::string to_string(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::contains: return "contains";
        case EdgeKind::imports: return "imports";
        case EdgeKind::calls: return "calls";
        case EdgeKind::references: return "references";
        case EdgeKind::inherits: return "inherits";
        case EdgeKind::implements: return "implements";
        case EdgeKind::tests: return "tests";
    }
    return "references";
}

Precision precision_from_string(std::string_view value) {
    if (value == "exact") return Precision::exact;
    if (value == "structural") return Precision::structural;
    if (value == "heuristic") return Precision::heuristic;
    return Precision::ambiguous;
}

void EvidenceGraph::add_node(Node node) {
    nodes_.push_back(std::move(node));
}

void EvidenceGraph::add_edge(Edge edge) {
    if (edge.source != edge.target) {
        edges_.push_back(std::move(edge));
    }
}

void EvidenceGraph::finalize() {
    std::sort(nodes_.begin(), nodes_.end(), [](const Node& left, const Node& right) {
        return left.id < right.id;
    });
    nodes_.erase(std::unique(nodes_.begin(), nodes_.end(), [](const Node& left, const Node& right) {
        return left.id == right.id;
    }), nodes_.end());

    std::sort(edges_.begin(), edges_.end(), [](const Edge& left, const Edge& right) {
        if (left.source != right.source) return left.source < right.source;
        if (left.target != right.target) return left.target < right.target;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.precision < right.precision;
    });
    edges_.erase(std::unique(edges_.begin(), edges_.end(), [](const Edge& left, const Edge& right) {
        return left.source == right.source && left.target == right.target && left.kind == right.kind;
    }), edges_.end());

    node_by_id_.clear();
    outgoing_.clear();
    incoming_.clear();
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        node_by_id_[nodes_[index].id.str()] = index;
    }
    for (std::size_t index = 0; index < edges_.size(); ++index) {
        outgoing_[edges_[index].source.str()].push_back(index);
        incoming_[edges_[index].target.str()].push_back(index);
    }
}

const std::vector<Node>& EvidenceGraph::nodes() const { return nodes_; }
const std::vector<Edge>& EvidenceGraph::edges() const { return edges_; }

const Node* EvidenceGraph::find(StableId id) const {
    const auto iterator = node_by_id_.find(id.str());
    return iterator == node_by_id_.end() ? nullptr : &nodes_[iterator->second];
}

std::vector<const Node*> EvidenceGraph::locate(std::string_view query, const QueryOptions& options) const {
    const std::string needle = lower_copy(query);
    struct Ranked {
        const Node* node;
        int score;
    };
    std::vector<Ranked> ranked;
    for (const Node& node : nodes_) {
        if (!precision_allowed(node.precision, options.minimum_precision)) {
            continue;
        }
        if (!options.language.empty() && node.language != options.language) {
            continue;
        }
        if (!options.root.empty() && node.path.rfind(options.root, 0) != 0) {
            continue;
        }
        const std::string name = lower_copy(node.name);
        const std::string qualified = lower_copy(node.qualified_name);
        const std::string path = lower_copy(node.path);
        int score = 0;
        if (name == needle || qualified == needle) score = 100000;
        else if (name.rfind(needle, 0) == 0) score = 80000;
        else if (qualified.find(needle) != std::string::npos) score = 60000;
        else if (path.find(needle) != std::string::npos) score = 40000;
        else if (lower_copy(node.signature).find(needle) != std::string::npos) score = 30000;
        if (score != 0) {
            score += precision_weight(node.precision);
            score += node.kind == NodeKind::symbol ? 100 : 0;
            ranked.push_back({&node, score});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.node->id < right.node->id;
    });
    std::vector<const Node*> result;
    for (std::size_t index = 0; index < ranked.size() && index < options.limit; ++index) {
        result.push_back(ranked[index].node);
    }
    return result;
}

std::vector<const Edge*> EvidenceGraph::outgoing(StableId id) const {
    std::vector<const Edge*> result;
    const auto iterator = outgoing_.find(id.str());
    if (iterator == outgoing_.end()) {
        return result;
    }
    for (const auto index : iterator->second) {
        result.push_back(&edges_[index]);
    }
    return result;
}

std::vector<const Edge*> EvidenceGraph::incoming(StableId id) const {
    std::vector<const Edge*> result;
    const auto iterator = incoming_.find(id.str());
    if (iterator == incoming_.end()) {
        return result;
    }
    for (const auto index : iterator->second) {
        result.push_back(&edges_[index]);
    }
    return result;
}

std::vector<const Node*> EvidenceGraph::trace(StableId source, StableId target, std::size_t max_depth) const {
    std::queue<std::pair<StableId, std::size_t>> queue;
    std::unordered_map<std::string, StableId> previous;
    std::unordered_set<std::string> visited;
    queue.push({source, 0});
    visited.insert(source.str());

    while (!queue.empty()) {
        const auto [current, depth] = queue.front();
        queue.pop();
        if (current == target) {
            break;
        }
        if (depth >= max_depth) {
            continue;
        }
        auto visit = [&](StableId next) {
            if (visited.insert(next.str()).second) {
                previous[next.str()] = current;
                queue.push({next, depth + 1});
            }
        };
        for (const Edge* edge : outgoing(current)) {
            visit(edge->target);
        }
        for (const Edge* edge : incoming(current)) {
            visit(edge->source);
        }
    }

    if (!visited.contains(target.str())) {
        return {};
    }
    std::vector<const Node*> path;
    StableId current = target;
    while (true) {
        if (const Node* node = find(current)) {
            path.push_back(node);
        }
        if (current == source) {
            break;
        }
        current = previous.at(current.str());
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<const Node*> EvidenceGraph::impact(StableId source, std::size_t max_depth, std::size_t limit) const {
    std::queue<std::pair<StableId, std::size_t>> queue;
    std::unordered_set<std::string> visited;
    std::vector<const Node*> result;
    queue.push({source, 0});
    visited.insert(source.str());
    while (!queue.empty() && result.size() < limit) {
        const auto [current, depth] = queue.front();
        queue.pop();
        if (depth >= max_depth) {
            continue;
        }
        for (const Edge* edge : incoming(current)) {
            if (!visited.insert(edge->source.str()).second) {
                continue;
            }
            if (const Node* node = find(edge->source)) {
                result.push_back(node);
            }
            queue.push({edge->source, depth + 1});
            if (result.size() >= limit) {
                break;
            }
        }
    }
    return result;
}

std::filesystem::path IndexStore::cache_root() {
#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(local_app_data) / "CodeNavigator";
    }
#else
    if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdg_cache) / "codenavigator";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".cache" / "codenavigator";
    }
#endif
    return std::filesystem::temp_directory_path() / "codenavigator";
}

std::filesystem::path IndexStore::workspace_cache(const std::filesystem::path& workspace) {
    const std::string canonical = normalized_path(std::filesystem::weakly_canonical(workspace));
    return cache_root() / stable_id(canonical).str();
}

std::filesystem::path IndexStore::index_path(const std::filesystem::path& workspace) {
    return workspace_cache(workspace) / "index.cnav";
}

void IndexStore::save(const IndexStatus& status, const EvidenceGraph& graph) {
    const auto directory = workspace_cache(status.workspace);
    std::filesystem::create_directories(directory);
    const auto final_path = directory / "index.cnav";
    const auto temporary_path = directory / "index.cnav.tmp";
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create index at " + temporary_path.string());
    }
    output.write(kIndexMagic.data(), static_cast<std::streamsize>(kIndexMagic.size()));
    write_value(output, kSchemaVersion);
    write_string(output, normalized_path(status.workspace));
    write_value(output, status.generation);
    write_value(output, status.indexed_at);
    write_value(output, status.source_bytes);
    write_value(output, static_cast<std::uint64_t>(graph.nodes().size()));
    write_value(output, static_cast<std::uint64_t>(graph.edges().size()));

    for (const Node& node : graph.nodes()) {
        write_value(output, node.id.high);
        write_value(output, node.id.low);
        write_value(output, node.kind);
        write_value(output, node.precision);
        write_string(output, node.name);
        write_string(output, node.qualified_name);
        write_string(output, node.language);
        write_string(output, node.path);
        write_string(output, node.signature);
        write_value(output, node.span);
    }
    for (const Edge& edge : graph.edges()) {
        write_value(output, edge.source.high);
        write_value(output, edge.source.low);
        write_value(output, edge.target.high);
        write_value(output, edge.target.low);
        write_value(output, edge.kind);
        write_value(output, edge.precision);
        write_value(output, edge.evidence);
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed writing CodeNavigator index");
    }
    output.close();
    std::error_code error;
    std::filesystem::rename(temporary_path, final_path, error);
    if (error) {
        std::filesystem::remove(final_path, error);
        error.clear();
        std::filesystem::rename(temporary_path, final_path, error);
    }
    if (error) {
        throw std::runtime_error("cannot publish CodeNavigator index: " + error.message());
    }
}

std::pair<IndexStatus, EvidenceGraph> IndexStore::load(const std::filesystem::path& workspace) {
    const auto path = index_path(workspace);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("no index for workspace; run `codenavigator .` first");
    }
    std::string magic(kIndexMagic.size(), '\0');
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != kIndexMagic || read_value<std::uint32_t>(input) != kSchemaVersion) {
        throw std::runtime_error("unsupported CodeNavigator index format");
    }
    IndexStatus status;
    status.workspace = read_string(input);
    status.index_path = path;
    status.generation = read_value<std::uint64_t>(input);
    status.indexed_at = read_value<std::uint64_t>(input);
    status.source_bytes = read_value<std::uint64_t>(input);
    const auto node_count = read_value<std::uint64_t>(input);
    const auto edge_count = read_value<std::uint64_t>(input);
    if (node_count > 100'000'000 || edge_count > 1'000'000'000) {
        throw std::runtime_error("invalid CodeNavigator index counts");
    }

    EvidenceGraph graph;
    for (std::uint64_t index = 0; index < node_count; ++index) {
        Node node;
        node.id.high = read_value<std::uint64_t>(input);
        node.id.low = read_value<std::uint64_t>(input);
        node.kind = read_value<NodeKind>(input);
        node.precision = read_value<Precision>(input);
        node.name = read_string(input);
        node.qualified_name = read_string(input);
        node.language = read_string(input);
        node.path = read_string(input);
        node.signature = read_string(input);
        node.span = read_value<SourceSpan>(input);
        graph.add_node(std::move(node));
    }
    for (std::uint64_t index = 0; index < edge_count; ++index) {
        Edge edge;
        edge.source.high = read_value<std::uint64_t>(input);
        edge.source.low = read_value<std::uint64_t>(input);
        edge.target.high = read_value<std::uint64_t>(input);
        edge.target.low = read_value<std::uint64_t>(input);
        edge.kind = read_value<EdgeKind>(input);
        edge.precision = read_value<Precision>(input);
        edge.evidence = read_value<SourceSpan>(input);
        graph.add_edge(std::move(edge));
    }
    graph.finalize();
    for (const Node& node : graph.nodes()) {
        if (node.kind == NodeKind::file) {
            ++status.files;
            ++status.languages[node.language];
        } else if (node.kind == NodeKind::symbol) {
            ++status.symbols;
        }
    }
    status.edges = graph.edges().size();
    return {std::move(status), std::move(graph)};
}

Indexer::Indexer(std::filesystem::path workspace)
    : workspace_(std::filesystem::weakly_canonical(std::move(workspace))) {}

std::pair<IndexStatus, EvidenceGraph> Indexer::build() const {
    if (!std::filesystem::is_directory(workspace_)) {
        throw std::runtime_error("workspace is not a directory: " + workspace_.string());
    }

    struct FileRecord {
        std::filesystem::path absolute;
        std::string relative;
        std::string language;
        std::string content;
        std::uint64_t fingerprint{};
        StableId id;
    };
    std::vector<FileRecord> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        workspace_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        const auto& entry = *iterator;
        if (entry.is_directory(error) && is_skipped_directory(entry.path().filename().string())) {
            iterator.disable_recursion_pending();
        } else if (entry.is_regular_file(error)) {
            const std::string language = language_for(entry.path());
            if (!language.empty()) {
                std::string content = read_file(entry.path());
                if (!content.empty() || std::filesystem::file_size(entry.path(), error) == 0) {
                    const std::string relative = normalized_path(std::filesystem::relative(entry.path(), workspace_));
                    files.push_back({
                        entry.path(),
                        relative,
                        language,
                        std::move(content),
                        0,
                        stable_id("file|" + relative),
                    });
                    files.back().fingerprint = hash64(files.back().content);
                }
            }
        }
        iterator.increment(error);
    }
    std::sort(files.begin(), files.end(), [](const FileRecord& left, const FileRecord& right) {
        return left.relative < right.relative;
    });

    std::uint64_t generation = kHashOffset;
    for (const FileRecord& file : files) {
        generation = hash64(file.relative, generation);
        generation ^= file.fingerprint;
        generation *= kHashPrime;
    }

    EvidenceGraph graph;
    const std::string root_path = normalized_path(workspace_);
    const StableId root_id = stable_id("root|" + root_path);
    graph.add_node({
        root_id,
        NodeKind::root,
        Precision::exact,
        workspace_.filename().string(),
        root_path,
        {},
        ".",
        {},
        {},
    });

    struct PendingImport {
        StableId source;
        std::string specification;
        SourceSpan span;
    };
    struct PendingCall {
        StableId source;
        std::string name;
        SourceSpan span;
    };
    std::vector<PendingImport> pending_imports;
    std::vector<PendingCall> pending_calls;
    std::unordered_map<std::string, std::vector<StableId>> symbols_by_name;
    std::unordered_map<std::string, StableId> file_by_path;

    for (const FileRecord& file : files) {
        graph.add_node({
            file.id,
            NodeKind::file,
            Precision::exact,
            std::filesystem::path(file.relative).filename().string(),
            file.relative,
            file.language,
            file.relative,
            {},
            {},
        });
        graph.add_edge({root_id, file.id, EdgeKind::contains, Precision::exact, {}});
        file_by_path[file.relative] = file.id;
        const auto lines = lines_of(file.content);
        StableId current_scope = file.id;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            const std::uint32_t line_number = static_cast<std::uint32_t>(index + 1);
            const SourceSpan span{line_number, 1, line_number,
                                  static_cast<std::uint32_t>(lines[index].size() + 1)};
            const auto definition = definition_for(file.language, lines[index]);
            if (definition) {
                const std::string qualified = file.relative + "::" + definition->name;
                const StableId symbol_id = stable_id(
                    "symbol|" + file.relative + "|" + file.language + "|" + qualified + "|" + definition->signature);
                graph.add_node({
                    symbol_id,
                    NodeKind::symbol,
                    Precision::structural,
                    definition->name,
                    qualified,
                    file.language,
                    file.relative,
                    definition->signature,
                    span,
                });
                graph.add_edge({file.id, symbol_id, EdgeKind::contains, Precision::structural, span});
                symbols_by_name[definition->name].push_back(symbol_id);
                current_scope = symbol_id;
            }

            for (const std::string& specification : imports_for(file.language, lines[index])) {
                pending_imports.push_back({file.id, specification, span});
            }
            for (const std::string& call : calls_for(lines[index])) {
                if (!definition || call != definition->name) {
                    pending_calls.push_back({current_scope, call, span});
                }
            }
        }
    }

    auto resolve_import = [&](const std::string& specification) -> std::vector<StableId> {
        std::string normalized = specification;
        std::replace(normalized.begin(), normalized.end(), '.', '/');
        normalized.erase(std::remove(normalized.begin(), normalized.end(), ':'), normalized.end());
        while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
        std::vector<StableId> result;
        for (const auto& [path, id] : file_by_path) {
            const std::filesystem::path candidate(path);
            const std::string no_extension = normalized_path(candidate.parent_path() / candidate.stem());
            if (path == specification || no_extension.ends_with(normalized) ||
                candidate.stem().string() == std::filesystem::path(normalized).filename().string()) {
                result.push_back(id);
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    };

    for (const PendingImport& pending : pending_imports) {
        const auto targets = resolve_import(pending.specification);
        const Precision precision = targets.size() == 1 ? Precision::structural : Precision::ambiguous;
        for (const StableId target : targets) {
            graph.add_edge({pending.source, target, EdgeKind::imports, precision, pending.span});
        }
    }

    for (const PendingCall& pending : pending_calls) {
        const auto iterator = symbols_by_name.find(pending.name);
        if (iterator == symbols_by_name.end()) {
            continue;
        }
        const Precision precision = iterator->second.size() == 1 ? Precision::structural : Precision::ambiguous;
        for (const StableId target : iterator->second) {
            graph.add_edge({pending.source, target, EdgeKind::calls, precision, pending.span});
        }
    }

    graph.finalize();
    IndexStatus status;
    status.workspace = workspace_;
    status.index_path = IndexStore::index_path(workspace_);
    status.generation = generation;
    status.indexed_at = unix_time();
    status.files = files.size();
    status.edges = graph.edges().size();
    for (const FileRecord& file : files) {
        status.source_bytes += file.content.size();
        ++status.languages[file.language];
    }
    status.symbols = static_cast<std::size_t>(std::count_if(
        graph.nodes().begin(), graph.nodes().end(), [](const Node& node) {
            return node.kind == NodeKind::symbol;
        }));
    return {std::move(status), std::move(graph)};
}

QueryEngine::QueryEngine(IndexStatus status, EvidenceGraph graph)
    : status_(std::move(status)), graph_(std::move(graph)) {}

Json QueryEngine::status() const {
    Json result = Json::object();
    result["schema_version"] = 1;
    result["workspace"] = normalized_path(status_.workspace);
    result["index_path"] = normalized_path(status_.index_path);
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    result["indexed_at"] = static_cast<std::int64_t>(status_.indexed_at);
    result["files"] = static_cast<std::int64_t>(status_.files);
    result["symbols"] = static_cast<std::int64_t>(status_.symbols);
    result["edges"] = static_cast<std::int64_t>(status_.edges);
    result["source_bytes"] = static_cast<std::int64_t>(status_.source_bytes);
    result["fresh"] = true;
    Json languages = Json::object();
    for (const auto& [language, count] : status_.languages) {
        languages[language] = static_cast<std::int64_t>(count);
    }
    result["languages"] = std::move(languages);
    result["providers"] = Json::object();
    result["providers"]["structural"] = "builtin";
    result["providers"]["precise"] = "not-configured";
    return result;
}

Json QueryEngine::workspace_map(const QueryOptions& options) const {
    Json result = Json::object();
    result["status"] = status();
    Json hubs = Json::array();
    struct Hub {
        const Node* node;
        std::size_t degree;
    };
    std::vector<Hub> ranked;
    for (const Node& node : graph_.nodes()) {
        if (node.kind != NodeKind::symbol || !precision_allowed(node.precision, options.minimum_precision)) {
            continue;
        }
        ranked.push_back({&node, graph_.incoming(node.id).size() + graph_.outgoing(node.id).size()});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Hub& left, const Hub& right) {
        if (left.degree != right.degree) return left.degree > right.degree;
        return left.node->id < right.node->id;
    });
    for (std::size_t index = 0; index < ranked.size() && index < options.limit; ++index) {
        Json hub = node_json(*ranked[index].node, false);
        hub["degree"] = static_cast<std::int64_t>(ranked[index].degree);
        hubs.push_back(std::move(hub));
    }
    result["hubs"] = std::move(hubs);
    Json entry_points = Json::array();
    for (const Node& node : graph_.nodes()) {
        if (node.kind == NodeKind::symbol &&
            (node.name == "main" || node.name == "Main" || node.name == "app" || node.name == "Application")) {
            entry_points.push_back(node_json(node, false));
        }
    }
    result["entry_points"] = std::move(entry_points);
    result["changed_areas"] = Json::array();
    return result;
}

Json QueryEngine::locate(const std::string& query, const QueryOptions& options) const {
    Json result = Json::object();
    result["query"] = query;
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    Json matches = Json::array();
    for (const Node* node : graph_.locate(query, options)) {
        matches.push_back(node_json(*node, false));
    }
    result["matches"] = std::move(matches);
    return result;
}

const Node* QueryEngine::resolve_one(const std::string& selector) const {
    if (const auto id = StableId::parse(selector)) {
        if (const Node* node = graph_.find(*id)) {
            return node;
        }
    }
    QueryOptions options;
    options.limit = 1;
    const auto matches = graph_.locate(selector, options);
    return matches.empty() ? nullptr : matches.front();
}

Json QueryEngine::inspect(const std::string& selector, const QueryOptions& options) const {
    const Node* node = resolve_one(selector);
    if (node == nullptr) {
        throw std::runtime_error("symbol or file not found: " + selector);
    }
    Json result = node_json(*node, true, options.budget * 4);
    Json incoming = Json::array();
    Json outgoing = Json::array();
    for (const Edge* edge : graph_.incoming(node->id)) incoming.push_back(edge_json(*edge));
    for (const Edge* edge : graph_.outgoing(node->id)) outgoing.push_back(edge_json(*edge));
    result["incoming"] = std::move(incoming);
    result["outgoing"] = std::move(outgoing);
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    return result;
}

Json QueryEngine::relations(const std::string& selector, const QueryOptions& options) const {
    const Node* node = resolve_one(selector);
    if (node == nullptr) {
        throw std::runtime_error("symbol or file not found: " + selector);
    }
    Json result = Json::object();
    result["subject"] = node_json(*node, false);
    Json incoming = Json::array();
    Json outgoing = Json::array();
    std::size_t count = 0;
    for (const Edge* edge : graph_.incoming(node->id)) {
        if (count++ >= options.limit) break;
        Json relation = edge_json(*edge);
        if (const Node* related = graph_.find(edge->source)) relation["node"] = node_json(*related, false);
        incoming.push_back(std::move(relation));
    }
    count = 0;
    for (const Edge* edge : graph_.outgoing(node->id)) {
        if (count++ >= options.limit) break;
        Json relation = edge_json(*edge);
        if (const Node* related = graph_.find(edge->target)) relation["node"] = node_json(*related, false);
        outgoing.push_back(std::move(relation));
    }
    result["incoming"] = std::move(incoming);
    result["outgoing"] = std::move(outgoing);
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    return result;
}

Json QueryEngine::trace(const std::string& from, const std::string& to, const QueryOptions& options) const {
    const Node* source = resolve_one(from);
    const Node* target = resolve_one(to);
    if (source == nullptr || target == nullptr) {
        throw std::runtime_error("trace endpoints could not be resolved");
    }
    Json result = Json::object();
    Json path = Json::array();
    for (const Node* node : graph_.trace(source->id, target->id, options.depth)) {
        path.push_back(node_json(*node, false));
    }
    result["from"] = node_json(*source, false);
    result["to"] = node_json(*target, false);
    result["path"] = std::move(path);
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    return result;
}

Json QueryEngine::impact(const std::string& selector, const QueryOptions& options) const {
    const Node* source = resolve_one(selector);
    if (source == nullptr) {
        throw std::runtime_error("impact source could not be resolved");
    }
    Json result = Json::object();
    result["source"] = node_json(*source, false);
    Json affected = Json::array();
    for (const Node* node : graph_.impact(source->id, options.depth, options.limit)) {
        affected.push_back(node_json(*node, false));
    }
    result["affected"] = std::move(affected);
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    return result;
}

Json QueryEngine::context(const std::string& query, const QueryOptions& options) const {
    QueryOptions seed_options = options;
    seed_options.limit = std::min<std::size_t>(options.limit, 8);
    const auto seeds = graph_.locate(query, seed_options);
    struct Candidate {
        const Node* node;
        std::size_t distance;
        std::int64_t score;
    };
    std::vector<Candidate> candidates;
    std::queue<std::pair<const Node*, std::size_t>> queue;
    std::unordered_set<std::string> visited;
    std::unordered_map<std::string, std::int64_t> seed_bonus;
    for (std::size_t index = 0; index < seeds.size(); ++index) {
        const Node* seed = seeds[index];
        queue.push({seed, 0});
        visited.insert(seed->id.str());
        seed_bonus[seed->id.str()] = 1'000'000 -
            static_cast<std::int64_t>(index * 10'000);
    }
    while (!queue.empty() && candidates.size() < options.limit * 8) {
        const auto [node, distance] = queue.front();
        queue.pop();
        const std::size_t degree = graph_.incoming(node->id).size() + graph_.outgoing(node->id).size();
        const std::int64_t numerator = static_cast<std::int64_t>(precision_weight(node->precision)) * 1000;
        const std::int64_t denominator = static_cast<std::int64_t>(distance + degree / 8 + 1);
        candidates.push_back({
            node,
            distance,
            numerator / denominator + seed_bonus[node->id.str()],
        });
        if (distance >= options.depth) continue;
        auto enqueue = [&](StableId id) {
            if (visited.insert(id.str()).second) {
                if (const Node* related = graph_.find(id)) queue.push({related, distance + 1});
            }
        };
        for (const Edge* edge : graph_.outgoing(node->id)) enqueue(edge->target);
        for (const Edge* edge : graph_.incoming(node->id)) enqueue(edge->source);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.node->id < right.node->id;
    });

    Json result = Json::object();
    result["query"] = query;
    result["generation"] = static_cast<std::int64_t>(status_.generation);
    result["fresh"] = true;
    result["content_trust"] = "untrusted_repository_evidence";
    result["budget"] = static_cast<std::int64_t>(options.budget);
    Json evidence = Json::array();
    Json omitted = Json::array();
    std::size_t omitted_count = 0;
    std::size_t used_tokens = 0;
    std::unordered_set<std::string> included_files;
    for (const Candidate& candidate : candidates) {
        const std::size_t remaining = options.budget > used_tokens ? options.budget - used_tokens : 0;
        if (remaining < 24) {
            ++omitted_count;
            if (omitted.as_array().size() < 8) omitted.push_back(candidate.node->id.str());
            continue;
        }
        const std::size_t node_budget = std::min<std::size_t>(remaining, candidate.distance == 0 ? 400 : 180);
        Json item = node_json(*candidate.node, true, node_budget * 4);
        const std::string source = item["snippet"].string_or();
        const std::size_t estimated = 20 + (source.size() + 3) / 4;
        if (estimated > remaining) {
            ++omitted_count;
            if (omitted.as_array().size() < 8) omitted.push_back(candidate.node->id.str());
            continue;
        }
        item["role"] = candidate.distance == 0 ? "seed" : "related";
        item["distance"] = static_cast<std::int64_t>(candidate.distance);
        item["score"] = candidate.score;
        evidence.push_back(std::move(item));
        used_tokens += estimated;
        included_files.insert(candidate.node->path);
        if (evidence.as_array().size() >= options.limit) break;
    }
    result["estimated_tokens"] = static_cast<std::int64_t>(used_tokens);
    result["evidence"] = std::move(evidence);
    result["omitted"] = std::move(omitted);
    result["omitted_count"] = static_cast<std::int64_t>(omitted_count);
    result["complete"] = omitted_count == 0;
    result["continuation_cursor"] = result["complete"].boolean_or() ? Json(nullptr) : Json("increase-budget");
    return result;
}

Json QueryEngine::execute(const std::string& operation, const Json& arguments) const {
    QueryOptions options;
    options.budget = static_cast<std::size_t>(std::max<std::int64_t>(64, arguments["budget"].integer_or(1200)));
    options.depth = static_cast<std::size_t>(std::clamp<std::int64_t>(arguments["depth"].integer_or(2), 1, 16));
    options.limit = static_cast<std::size_t>(std::clamp<std::int64_t>(arguments["limit"].integer_or(20), 1, 200));
    options.minimum_precision = precision_from_string(arguments["precision"].string_or("ambiguous"));
    options.language = arguments["language"].string_or();
    options.root = arguments["root"].string_or();

    if (operation == "workspace_map" || operation == "workspace-map") return workspace_map(options);
    if (operation == "locate") return locate(arguments["query"].string_or(), options);
    if (operation == "inspect") return inspect(arguments["selector"].string_or(arguments["query"].string_or()), options);
    if (operation == "relations") return relations(arguments["selector"].string_or(arguments["query"].string_or()), options);
    if (operation == "trace") return trace(arguments["from"].string_or(), arguments["to"].string_or(), options);
    if (operation == "impact") return impact(arguments["selector"].string_or(arguments["query"].string_or()), options);
    if (operation == "context") return context(arguments["query"].string_or(), options);
    if (operation == "status") return status();
    throw std::runtime_error("unknown query operation: " + operation);
}

Json QueryEngine::node_json(const Node& node, bool include_snippet, std::size_t snippet_budget) const {
    Json result = Json::object();
    result["id"] = node.id.str();
    result["kind"] = to_string(node.kind);
    result["precision"] = to_string(node.precision);
    result["name"] = node.name;
    result["qualified_name"] = node.qualified_name;
    result["language"] = node.language;
    result["path"] = node.path;
    result["signature"] = node.signature;
    result["span"] = span_json(node.span);
    if (include_snippet) {
        result["snippet"] = snippet(node, snippet_budget);
    }
    return result;
}

Json QueryEngine::edge_json(const Edge& edge) const {
    Json result = Json::object();
    result["source"] = edge.source.str();
    result["target"] = edge.target.str();
    result["kind"] = to_string(edge.kind);
    result["precision"] = to_string(edge.precision);
    result["evidence"] = span_json(edge.evidence);
    return result;
}

std::string QueryEngine::snippet(const Node& node, std::size_t character_budget) const {
    if (node.kind == NodeKind::root || node.path.empty() || character_budget == 0) {
        return {};
    }
    const auto path = status_.workspace / node.path;
    const std::string content = read_file(path);
    if (content.empty()) {
        return {};
    }
    const auto lines = lines_of(content);
    if (lines.empty()) {
        return {};
    }
    const std::size_t start = node.kind == NodeKind::file
        ? 0
        : static_cast<std::size_t>(node.span.line_start > 2 ? node.span.line_start - 2 : 0);
    const std::size_t end = node.kind == NodeKind::file
        ? lines.size()
        : std::min(lines.size(), start + 14);
    std::ostringstream output;
    for (std::size_t index = start; index < end; ++index) {
        const std::string rendered = std::to_string(index + 1) + ": " + lines[index] + "\n";
        if (output.tellp() >= 0 &&
            static_cast<std::size_t>(output.tellp()) + rendered.size() > character_budget) {
            break;
        }
        output << rendered;
    }
    return output.str();
}

}  // namespace codenavigator
