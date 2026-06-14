#include "codenavigator/engine.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
}

void test_json() {
    const auto value = codenavigator::Json::parse(
        R"({"id":7,"ok":true,"items":["a","b"],"nested":{"name":"value"}})");
    require(value["id"].integer_or() == 7, "JSON integer parsing failed");
    require(value["ok"].boolean_or(), "JSON boolean parsing failed");
    require(value["items"].as_array().size() == 2, "JSON array parsing failed");
    require(codenavigator::Json::parse(value.dump())["nested"]["name"].string_or() == "value",
            "JSON round trip failed");
}

void test_index_and_queries() {
    const auto workspace = std::filesystem::temp_directory_path() / "codenavigator-engine-test";
    std::filesystem::remove_all(workspace);
    write(workspace / "math.cpp",
          "int add(int left, int right) {\n"
          "  return left + right;\n"
          "}\n");
    write(workspace / "main.cpp",
          "#include \"math.cpp\"\n"
          "int main() {\n"
          "  return add(1, 2);\n"
          "}\n");
    write(workspace / "service.py",
          "def load_user(user_id):\n"
          "    return user_id\n\n"
          "def handle_request(user_id):\n"
          "    return load_user(user_id)\n");

    codenavigator::Indexer first_indexer(workspace);
    auto [first_status, first_graph] = first_indexer.build();
    codenavigator::Indexer second_indexer(workspace);
    auto [second_status, second_graph] = second_indexer.build();

    require(first_status.generation == second_status.generation,
            "clean rebuilds must have identical generations");
    require(first_status.files == 3, "expected three indexed files");
    require(first_status.symbols >= 4, "expected extracted symbols");
    require(first_status.edges >= 6, "expected containment and relationship edges");

    codenavigator::IndexStore::save(first_status, first_graph);
    auto [loaded_status, loaded_graph] = codenavigator::IndexStore::load(workspace);
    require(loaded_status.generation == first_status.generation, "generation did not persist");
    require(loaded_graph.nodes().size() == first_graph.nodes().size(), "node count did not persist");
    require(loaded_graph.edges().size() == first_graph.edges().size(), "edge count did not persist");

    codenavigator::QueryEngine engine(std::move(loaded_status), std::move(loaded_graph));
    codenavigator::Json arguments = codenavigator::Json::object();
    arguments["query"] = "add";
    arguments["limit"] = 10;
    require(!engine.execute("locate", arguments)["matches"].as_array().empty(),
            "locate did not find add");

    arguments = codenavigator::Json::object();
    arguments["selector"] = "add";
    arguments["depth"] = 3;
    arguments["limit"] = 10;
    require(!engine.execute("impact", arguments)["affected"].as_array().empty(),
            "impact did not find main caller");

    arguments = codenavigator::Json::object();
    arguments["query"] = "load_user";
    arguments["budget"] = 160;
    arguments["depth"] = 2;
    arguments["limit"] = 10;
    const auto context = engine.execute("context", arguments);
    require(context["estimated_tokens"].integer_or() <= 160, "context exceeded token budget");
    require(!context["evidence"].as_array().empty(), "context returned no evidence");

    std::filesystem::remove_all(workspace);
}

}  // namespace

int main() {
    try {
        test_json();
        test_index_and_queries();
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
