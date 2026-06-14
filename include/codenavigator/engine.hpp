#pragma once

#include "codenavigator/json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace codenavigator {

enum class Precision : std::uint8_t {
    exact,
    structural,
    heuristic,
    ambiguous,
};

enum class NodeKind : std::uint8_t {
    root,
    file,
    symbol,
};

enum class EdgeKind : std::uint8_t {
    contains,
    imports,
    calls,
    references,
    inherits,
    implements,
    tests,
};

struct StableId {
    std::uint64_t high{};
    std::uint64_t low{};

    auto operator<=>(const StableId&) const = default;
    [[nodiscard]] std::string str() const;
    static std::optional<StableId> parse(std::string_view text);
};

struct SourceSpan {
    std::uint32_t line_start{1};
    std::uint32_t column_start{1};
    std::uint32_t line_end{1};
    std::uint32_t column_end{1};
};

struct Node {
    StableId id;
    NodeKind kind{NodeKind::symbol};
    Precision precision{Precision::structural};
    std::string name;
    std::string qualified_name;
    std::string language;
    std::string path;
    std::string signature;
    SourceSpan span;
};

struct Edge {
    StableId source;
    StableId target;
    EdgeKind kind{EdgeKind::references};
    Precision precision{Precision::structural};
    SourceSpan evidence;
};

struct IndexStatus {
    std::filesystem::path workspace;
    std::filesystem::path index_path;
    std::uint64_t generation{};
    std::uint64_t indexed_at{};
    std::size_t files{};
    std::size_t symbols{};
    std::size_t edges{};
    std::uint64_t source_bytes{};
    std::unordered_map<std::string, std::size_t> languages;
};

struct QueryOptions {
    std::size_t budget{1200};
    std::size_t depth{2};
    std::size_t limit{20};
    Precision minimum_precision{Precision::ambiguous};
    std::string language;
    std::string root;
};

class EvidenceGraph {
public:
    void add_node(Node node);
    void add_edge(Edge edge);
    void finalize();

    [[nodiscard]] const std::vector<Node>& nodes() const;
    [[nodiscard]] const std::vector<Edge>& edges() const;
    [[nodiscard]] const Node* find(StableId id) const;
    [[nodiscard]] std::vector<const Node*> locate(std::string_view query, const QueryOptions& options) const;
    [[nodiscard]] std::vector<const Edge*> outgoing(StableId id) const;
    [[nodiscard]] std::vector<const Edge*> incoming(StableId id) const;
    [[nodiscard]] std::vector<const Node*> trace(StableId source, StableId target, std::size_t max_depth) const;
    [[nodiscard]] std::vector<const Node*> impact(StableId source, std::size_t max_depth, std::size_t limit) const;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, std::size_t> node_by_id_;
    std::unordered_map<std::string, std::vector<std::size_t>> outgoing_;
    std::unordered_map<std::string, std::vector<std::size_t>> incoming_;
};

class IndexStore {
public:
    static std::filesystem::path cache_root();
    static std::filesystem::path workspace_cache(const std::filesystem::path& workspace);
    static std::filesystem::path index_path(const std::filesystem::path& workspace);
    static void save(const IndexStatus& status, const EvidenceGraph& graph);
    static std::pair<IndexStatus, EvidenceGraph> load(const std::filesystem::path& workspace);
};

class Indexer {
public:
    explicit Indexer(std::filesystem::path workspace);
    [[nodiscard]] std::pair<IndexStatus, EvidenceGraph> build() const;

private:
    std::filesystem::path workspace_;
};

class QueryEngine {
public:
    QueryEngine(IndexStatus status, EvidenceGraph graph);

    [[nodiscard]] Json status() const;
    [[nodiscard]] Json workspace_map(const QueryOptions& options) const;
    [[nodiscard]] Json locate(const std::string& query, const QueryOptions& options) const;
    [[nodiscard]] Json inspect(const std::string& selector, const QueryOptions& options) const;
    [[nodiscard]] Json relations(const std::string& selector, const QueryOptions& options) const;
    [[nodiscard]] Json trace(const std::string& from, const std::string& to, const QueryOptions& options) const;
    [[nodiscard]] Json impact(const std::string& selector, const QueryOptions& options) const;
    [[nodiscard]] Json context(const std::string& query, const QueryOptions& options) const;
    [[nodiscard]] Json execute(const std::string& operation, const Json& arguments) const;

private:
    [[nodiscard]] const Node* resolve_one(const std::string& selector) const;
    [[nodiscard]] Json node_json(const Node& node, bool include_snippet, std::size_t snippet_budget = 0) const;
    [[nodiscard]] Json edge_json(const Edge& edge) const;
    [[nodiscard]] std::string snippet(const Node& node, std::size_t character_budget) const;

    IndexStatus status_;
    EvidenceGraph graph_;
};

[[nodiscard]] std::string to_string(Precision precision);
[[nodiscard]] std::string to_string(NodeKind kind);
[[nodiscard]] std::string to_string(EdgeKind kind);
[[nodiscard]] Precision precision_from_string(std::string_view value);

}  // namespace codenavigator
