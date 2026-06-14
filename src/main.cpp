#include "codenavigator/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using codenavigator::IndexStore;
using codenavigator::Indexer;
using codenavigator::Json;
using codenavigator::QueryEngine;

struct CommandLine {
    std::vector<std::string> positional;
    std::filesystem::path workspace = std::filesystem::current_path();
    std::size_t budget{1200};
    std::size_t depth{2};
    std::size_t limit{20};
    std::string precision{"ambiguous"};
    std::string language;
    bool watch{false};
    bool pretty{false};
};

std::size_t parse_size(const std::string& value, const std::string& option) {
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (...) {
        throw std::runtime_error("invalid value for " + option + ": " + value);
    }
}

CommandLine parse_arguments(int argc, char** argv) {
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto take_value = [&](const std::string& option) {
            if (++index >= argc) {
                throw std::runtime_error("missing value for " + option);
            }
            return std::string(argv[index]);
        };
        if (argument == "--workspace") command.workspace = take_value(argument);
        else if (argument == "--budget") command.budget = parse_size(take_value(argument), argument);
        else if (argument == "--depth") command.depth = parse_size(take_value(argument), argument);
        else if (argument == "--limit") command.limit = parse_size(take_value(argument), argument);
        else if (argument == "--precision") command.precision = take_value(argument);
        else if (argument == "--language") command.language = take_value(argument);
        else if (argument == "--watch") command.watch = true;
        else if (argument == "--pretty") command.pretty = true;
        else command.positional.push_back(argument);
    }
    return command;
}

Json query_arguments(const CommandLine& command) {
    Json arguments = Json::object();
    arguments["budget"] = static_cast<std::int64_t>(command.budget);
    arguments["depth"] = static_cast<std::int64_t>(command.depth);
    arguments["limit"] = static_cast<std::int64_t>(command.limit);
    arguments["precision"] = command.precision;
    arguments["language"] = command.language;
    return arguments;
}

QueryEngine load_engine(const std::filesystem::path& workspace) {
    auto [status, graph] = IndexStore::load(workspace);
    return QueryEngine(std::move(status), std::move(graph));
}

Json index_workspace(const std::filesystem::path& workspace) {
    Indexer indexer(workspace);
    auto [status, graph] = indexer.build();
    IndexStore::save(status, graph);
    return QueryEngine(std::move(status), std::move(graph)).status();
}

void print_help() {
    std::cout <<
        "CodeNavigator 0.1.0 - deterministic code intelligence for AI agents\n\n"
        "Usage:\n"
        "  codenavigator [PATH] [--watch]\n"
        "  codenavigator status [--workspace PATH]\n"
        "  codenavigator query OPERATION [ARGUMENTS] [OPTIONS]\n"
        "  codenavigator mcp [--workspace PATH]\n"
        "  codenavigator integrate --global\n"
        "  codenavigator doctor [--workspace PATH]\n\n"
        "Query operations:\n"
        "  workspace-map\n"
        "  locate QUERY\n"
        "  inspect SELECTOR\n"
        "  relations SELECTOR\n"
        "  trace FROM TO\n"
        "  impact SELECTOR\n"
        "  context QUERY\n\n"
        "Options: --budget N --depth N --limit N --precision LEVEL --language NAME\n";
}

Json tool_schema(const std::string& name, const std::string& description,
                 const std::vector<std::string>& required) {
    Json tool = Json::object();
    tool["name"] = name;
    tool["description"] = description;
    Json schema = Json::object();
    schema["type"] = "object";
    Json properties = Json::object();
    auto string_property = [](const std::string& description_text) {
        Json property = Json::object();
        property["type"] = "string";
        property["description"] = description_text;
        return property;
    };
    auto integer_property = [](const std::string& description_text, std::int64_t default_value) {
        Json property = Json::object();
        property["type"] = "integer";
        property["description"] = description_text;
        property["default"] = default_value;
        property["minimum"] = 1;
        return property;
    };
    properties["query"] = string_property("Search or task text.");
    properties["selector"] = string_property("Stable node ID, symbol name, qualified name, or path.");
    properties["from"] = string_property("Trace source selector.");
    properties["to"] = string_property("Trace target selector.");
    properties["budget"] = integer_property("Maximum approximate output tokens.", 1200);
    properties["depth"] = integer_property("Maximum graph traversal depth.", 2);
    properties["limit"] = integer_property("Maximum number of returned items.", 20);
    properties["precision"] = string_property("Maximum uncertainty: exact, structural, heuristic, or ambiguous.");
    properties["language"] = string_property("Optional language filter.");
    properties["root"] = string_property("Optional workspace-relative root filter.");
    schema["properties"] = std::move(properties);
    Json required_json = Json::array();
    for (const auto& field : required) required_json.push_back(field);
    schema["required"] = std::move(required_json);
    schema["additionalProperties"] = false;
    tool["inputSchema"] = std::move(schema);
    return tool;
}

Json tools_list() {
    Json tools = Json::array();
    tools.push_back(tool_schema("workspace_map", "Summarize languages, entry points, and graph hubs.", {}));
    tools.push_back(tool_schema("locate", "Find symbols, files, identifiers, and signatures.", {"query"}));
    tools.push_back(tool_schema("inspect", "Inspect a node with source and direct evidence.", {"selector"}));
    tools.push_back(tool_schema("relations", "List incoming and outgoing code relationships.", {"selector"}));
    tools.push_back(tool_schema("trace", "Find a bounded evidence path between two nodes.", {"from", "to"}));
    tools.push_back(tool_schema("impact", "Find reverse dependencies affected by a node.", {"selector"}));
    tools.push_back(tool_schema("context", "Compile a minimal token-budgeted evidence capsule.", {"query"}));
    return tools;
}

Json mcp_result(const Json& data) {
    Json result = Json::object();
    Json content = Json::array();
    Json text = Json::object();
    text["type"] = "text";
    text["text"] = data.dump();
    content.push_back(std::move(text));
    result["content"] = std::move(content);
    result["structuredContent"] = data;
    result["isError"] = false;
    return result;
}

Json mcp_error(const std::string& message) {
    Json result = Json::object();
    Json content = Json::array();
    Json text = Json::object();
    text["type"] = "text";
    text["text"] = message;
    content.push_back(std::move(text));
    result["content"] = std::move(content);
    result["isError"] = true;
    return result;
}

void run_mcp(const std::filesystem::path& workspace) {
    QueryEngine engine = load_engine(workspace);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json response = Json::object();
        try {
            const Json request = Json::parse(line);
            if (!request.contains("id")) {
                continue;
            }
            response["jsonrpc"] = "2.0";
            response["id"] = request["id"];
            const std::string method = request["method"].string_or();
            if (method == "initialize") {
                Json result = Json::object();
                result["protocolVersion"] =
                    request["params"]["protocolVersion"].string_or("2025-06-18");
                Json capabilities = Json::object();
                capabilities["tools"] = Json::object();
                result["capabilities"] = std::move(capabilities);
                Json server_info = Json::object();
                server_info["name"] = "codenavigator";
                server_info["version"] = "0.1.0";
                result["serverInfo"] = std::move(server_info);
                response["result"] = std::move(result);
            } else if (method == "tools/list") {
                Json result = Json::object();
                result["tools"] = tools_list();
                response["result"] = std::move(result);
            } else if (method == "tools/call") {
                const std::string name = request["params"]["name"].string_or();
                try {
                    response["result"] = mcp_result(engine.execute(name, request["params"]["arguments"]));
                } catch (const std::exception& error) {
                    response["result"] = mcp_error(error.what());
                }
            } else if (method == "ping") {
                response["result"] = Json::object();
            } else {
                Json error = Json::object();
                error["code"] = -32601;
                error["message"] = "method not found";
                response["error"] = std::move(error);
            }
        } catch (const std::exception& error) {
            response = Json::object();
            response["jsonrpc"] = "2.0";
            Json failure = Json::object();
            failure["code"] = -32700;
            failure["message"] = error.what();
            response["error"] = std::move(failure);
        }
        std::cout << response.dump() << '\n' << std::flush;
    }
}

void integrate_global() {
    std::filesystem::path config_root;
#ifdef _WIN32
    if (const char* app_data = std::getenv("APPDATA")) config_root = app_data;
#else
    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME")) config_root = xdg_config;
    else if (const char* home = std::getenv("HOME")) config_root = std::filesystem::path(home) / ".config";
#endif
    if (config_root.empty()) {
        throw std::runtime_error("cannot determine user configuration directory");
    }
    const auto directory = config_root / "codenavigator";
    std::filesystem::create_directories(directory);
    const auto path = directory / "mcp.json";
    Json config = Json::object();
    config["name"] = "codenavigator";
    config["command"] = "codenavigator";
    Json arguments = Json::array();
    arguments.push_back("mcp");
    arguments.push_back("--workspace");
    arguments.push_back(".");
    config["args"] = std::move(arguments);
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output << config.dump() << '\n';
    std::cout << "Wrote portable MCP registration template: " << path << '\n';
    std::cout << "Point your AI client's MCP configuration at this command template.\n";
}

void run_doctor(const std::filesystem::path& workspace) {
    Json result = Json::object();
    result["workspace"] = std::filesystem::weakly_canonical(workspace).generic_string();
    result["workspace_readable"] = std::filesystem::is_directory(workspace);
    result["cache_root"] = IndexStore::cache_root().generic_string();
    std::error_code error;
    std::filesystem::create_directories(IndexStore::cache_root(), error);
    result["cache_writable"] = !error;
    result["index_exists"] = std::filesystem::exists(IndexStore::index_path(workspace));
    result["structural_provider"] = "builtin";
    result["tree_sitter_provider"] = "not-built";
    result["scip_provider"] = "not-built";
    result["clang_provider"] = "not-built";
    std::cout << result.dump() << '\n';
}

void watch_workspace(const std::filesystem::path& workspace, std::uint64_t initial_generation) {
    std::cerr << "watching " << workspace << " (polling fallback, Ctrl-C to stop)\n";
    std::uint64_t generation = initial_generation;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        Indexer indexer(workspace);
        auto [status, graph] = indexer.build();
        if (status.generation != generation) {
            IndexStore::save(status, graph);
            generation = status.generation;
            std::cerr << "published generation " << generation << '\n';
        }
    }
}

int run(const CommandLine& command) {
    if (command.positional.empty() || command.positional.front() == "--help" ||
        command.positional.front() == "help") {
        print_help();
        return 0;
    }
    if (command.positional.front() == "--version" || command.positional.front() == "version") {
        std::cout << "codenavigator 0.1.0\n";
        return 0;
    }

    const std::string action = command.positional.front();
    if (action == "status") {
        std::cout << load_engine(command.workspace).status().dump() << '\n';
        return 0;
    }
    if (action == "mcp") {
        run_mcp(command.workspace);
        return 0;
    }
    if (action == "integrate") {
        if (std::find(command.positional.begin(), command.positional.end(), "--global") ==
            command.positional.end()) {
            throw std::runtime_error("only `codenavigator integrate --global` is supported");
        }
        integrate_global();
        return 0;
    }
    if (action == "doctor") {
        run_doctor(command.workspace);
        return 0;
    }
    if (action == "query") {
        if (command.positional.size() < 2) {
            throw std::runtime_error("query operation is required");
        }
        const std::string operation = command.positional[1];
        Json arguments = query_arguments(command);
        if (operation == "locate" || operation == "context") {
            if (command.positional.size() < 3) throw std::runtime_error(operation + " requires a query");
            arguments["query"] = command.positional[2];
        } else if (operation == "inspect" || operation == "relations" || operation == "impact") {
            if (command.positional.size() < 3) throw std::runtime_error(operation + " requires a selector");
            arguments["selector"] = command.positional[2];
        } else if (operation == "trace") {
            if (command.positional.size() < 4) throw std::runtime_error("trace requires FROM and TO");
            arguments["from"] = command.positional[2];
            arguments["to"] = command.positional[3];
        }
        std::cout << load_engine(command.workspace).execute(operation, arguments).dump() << '\n';
        return 0;
    }

    std::filesystem::path workspace =
        action == "." ? std::filesystem::current_path() : std::filesystem::path(action);
    const Json result = index_workspace(workspace);
    std::cout << result.dump() << '\n';
    if (command.watch) {
        watch_workspace(workspace, static_cast<std::uint64_t>(result["generation"].integer_or()));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "codenavigator: " << error.what() << '\n';
        return 1;
    }
}
