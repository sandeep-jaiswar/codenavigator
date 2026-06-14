#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace codenavigator {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(int value);
    Json(std::int64_t value);
    Json(double value);
    Json(const char* value);
    Json(std::string value);
    Json(Array value);
    Json(Object value);

    static Json array();
    static Json object();
    static Json parse(const std::string& text);

    [[nodiscard]] std::string dump() const;
    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_object() const;
    [[nodiscard]] bool is_array() const;
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] std::string string_or(const std::string& fallback = {}) const;
    [[nodiscard]] std::int64_t integer_or(std::int64_t fallback = 0) const;
    [[nodiscard]] bool boolean_or(bool fallback = false) const;

    Json& operator[](const std::string& key);
    const Json& operator[](const std::string& key) const;
    void push_back(Json value);
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] const Object& as_object() const;

private:
    Value value_;
};

}  // namespace codenavigator
