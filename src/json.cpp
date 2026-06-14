#include "codenavigator/json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace codenavigator {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse() {
        skip_space();
        Json value = parse_value();
        skip_space();
        if (position_ != text_.size()) {
            throw std::runtime_error("unexpected trailing JSON content");
        }
        return value;
    }

private:
    Json parse_value() {
        skip_space();
        if (position_ >= text_.size()) {
            throw std::runtime_error("unexpected end of JSON");
        }
        const char current = text_[position_];
        if (current == '{') {
            return parse_object();
        }
        if (current == '[') {
            return parse_array();
        }
        if (current == '"') {
            return Json(parse_string());
        }
        if (current == 't' && consume("true")) {
            return Json(true);
        }
        if (current == 'f' && consume("false")) {
            return Json(false);
        }
        if (current == 'n' && consume("null")) {
            return Json(nullptr);
        }
        return parse_number();
    }

    Json parse_object() {
        ++position_;
        Json result = Json::object();
        skip_space();
        if (take('}')) {
            return result;
        }
        while (true) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') {
                throw std::runtime_error("expected JSON object key");
            }
            std::string key = parse_string();
            skip_space();
            if (!take(':')) {
                throw std::runtime_error("expected ':' after JSON object key");
            }
            result[key] = parse_value();
            skip_space();
            if (take('}')) {
                return result;
            }
            if (!take(',')) {
                throw std::runtime_error("expected ',' in JSON object");
            }
        }
    }

    Json parse_array() {
        ++position_;
        Json result = Json::array();
        skip_space();
        if (take(']')) {
            return result;
        }
        while (true) {
            result.push_back(parse_value());
            skip_space();
            if (take(']')) {
                return result;
            }
            if (!take(',')) {
                throw std::runtime_error("expected ',' in JSON array");
            }
        }
    }

    std::string parse_string() {
        ++position_;
        std::string result;
        while (position_ < text_.size()) {
            const char current = text_[position_++];
            if (current == '"') {
                return result;
            }
            if (current != '\\') {
                result.push_back(current);
                continue;
            }
            if (position_ >= text_.size()) {
                throw std::runtime_error("invalid JSON escape");
            }
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                    if (position_ + 4 > text_.size()) {
                        throw std::runtime_error("invalid JSON unicode escape");
                    }
                    result.append("\\u");
                    result.append(text_.substr(position_, 4));
                    position_ += 4;
                    break;
                default: throw std::runtime_error("unsupported JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    Json parse_number() {
        const std::size_t start = position_;
        if (text_[position_] == '-') {
            ++position_;
        }
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        bool floating = false;
        if (position_ < text_.size() && text_[position_] == '.') {
            floating = true;
            ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }
        const std::string token = text_.substr(start, position_ - start);
        if (token.empty() || token == "-") {
            throw std::runtime_error("invalid JSON number");
        }
        if (floating) {
            return Json(std::stod(token));
        }
        std::int64_t value{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("invalid JSON integer");
        }
        return Json(value);
    }

    bool consume(const std::string& token) {
        if (text_.compare(position_, token.size(), token) != 0) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    bool take(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_space() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    const std::string& text_;
    std::size_t position_{};
};

std::string escape_json(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character);
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

std::string dump_value(const Json::Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return "null";
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* floating = std::get_if<double>(&value)) {
        if (!std::isfinite(*floating)) {
            return "null";
        }
        std::ostringstream output;
        output << std::setprecision(15) << *floating;
        return output.str();
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return '"' + escape_json(*text) + '"';
    }
    if (const auto* array = std::get_if<Json::Array>(&value)) {
        std::string output = "[";
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index != 0) {
                output += ',';
            }
            output += (*array)[index].dump();
        }
        return output + ']';
    }
    const auto& object = std::get<Json::Object>(value);
    std::string output = "{";
    bool first = true;
    for (const auto& [key, item] : object) {
        if (!first) {
            output += ',';
        }
        first = false;
        output += '"' + escape_json(key) + "\":" + item.dump();
    }
    return output + '}';
}

}  // namespace

Json::Json() : value_(nullptr) {}
Json::Json(std::nullptr_t) : value_(nullptr) {}
Json::Json(bool value) : value_(value) {}
Json::Json(int value) : value_(static_cast<std::int64_t>(value)) {}
Json::Json(std::int64_t value) : value_(value) {}
Json::Json(double value) : value_(value) {}
Json::Json(const char* value) : value_(std::string(value)) {}
Json::Json(std::string value) : value_(std::move(value)) {}
Json::Json(Array value) : value_(std::move(value)) {}
Json::Json(Object value) : value_(std::move(value)) {}

Json Json::array() { return Json(Array{}); }
Json Json::object() { return Json(Object{}); }
Json Json::parse(const std::string& text) { return Parser(text).parse(); }
std::string Json::dump() const { return dump_value(value_); }
bool Json::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_object() const { return std::holds_alternative<Object>(value_); }
bool Json::is_array() const { return std::holds_alternative<Array>(value_); }

bool Json::contains(const std::string& key) const {
    const auto* object = std::get_if<Object>(&value_);
    return object != nullptr && object->contains(key);
}

std::string Json::string_or(const std::string& fallback) const {
    const auto* value = std::get_if<std::string>(&value_);
    return value == nullptr ? fallback : *value;
}

std::int64_t Json::integer_or(std::int64_t fallback) const {
    const auto* value = std::get_if<std::int64_t>(&value_);
    return value == nullptr ? fallback : *value;
}

bool Json::boolean_or(bool fallback) const {
    const auto* value = std::get_if<bool>(&value_);
    return value == nullptr ? fallback : *value;
}

Json& Json::operator[](const std::string& key) {
    if (!is_object()) {
        value_ = Object{};
    }
    return std::get<Object>(value_)[key];
}

const Json& Json::operator[](const std::string& key) const {
    static const Json null_value;
    const auto* object = std::get_if<Object>(&value_);
    if (object == nullptr) {
        return null_value;
    }
    const auto iterator = object->find(key);
    return iterator == object->end() ? null_value : iterator->second;
}

void Json::push_back(Json value) {
    if (!is_array()) {
        value_ = Array{};
    }
    std::get<Array>(value_).push_back(std::move(value));
}

const Json::Array& Json::as_array() const {
    static const Array empty;
    const auto* array = std::get_if<Array>(&value_);
    return array == nullptr ? empty : *array;
}

const Json::Object& Json::as_object() const {
    static const Object empty;
    const auto* object = std::get_if<Object>(&value_);
    return object == nullptr ? empty : *object;
}

}  // namespace codenavigator
