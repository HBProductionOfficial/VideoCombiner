#pragma once

// A deliberately small JSON reader: objects, arrays, strings, numbers, bools
// and null, which is everything a config file needs. Kept header-only so the
// project builds with no third-party dependencies.

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vc::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : type_(Type::Null) {}
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double n) : type_(Type::Number), num_(n) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    explicit Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }

    bool asBool(bool fallback = false) const {
        if (type_ == Type::Bool) return bool_;
        if (type_ == Type::Number) return num_ != 0;
        return fallback;
    }
    double asNumber(double fallback = 0) const {
        return type_ == Type::Number ? num_ : fallback;
    }
    int asInt(int fallback = 0) const {
        return type_ == Type::Number ? static_cast<int>(num_) : fallback;
    }
    const std::string& asString() const {
        static const std::string empty;
        return type_ == Type::String ? str_ : empty;
    }
    const Array& asArray() const {
        static const Array empty;
        return type_ == Type::Array ? arr_ : empty;
    }
    const Object& asObject() const {
        static const Object empty;
        return type_ == Type::Object ? obj_ : empty;
    }

    /// Member lookup that yields a null Value when absent, so callers can chain
    /// without checking at every step.
    const Value& operator[](const std::string& key) const {
        static const Value null;
        if (type_ != Type::Object) return null;
        auto it = obj_.find(key);
        return it == obj_.end() ? null : it->second;
    }

    bool has(const std::string& key) const {
        return type_ == Type::Object && obj_.count(key) > 0;
    }

private:
    Type type_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    Array arr_;
    Object obj_;
};

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& what) : std::runtime_error(what) {}
};

namespace detail {

struct Parser {
    const std::string& text;
    size_t pos = 0;

    explicit Parser(const std::string& t) : text(t) {}

    [[noreturn]] void fail(const std::string& what) const {
        // Report a line number: "unexpected character" alone is useless when
        // someone is hand-editing a config file.
        size_t line = 1;
        for (size_t i = 0; i < pos && i < text.size(); ++i) {
            if (text[i] == '\n') ++line;
        }
        throw ParseError("line " + std::to_string(line) + ": " + what);
    }

    void skipWhitespace() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
                // Not standard JSON, but people leave notes in config files.
                while (pos < text.size() && text[pos] != '\n') ++pos;
            } else {
                break;
            }
        }
    }

    char peek() {
        skipWhitespace();
        if (pos >= text.size()) fail("unexpected end of input");
        return text[pos];
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++pos;
    }

    Value parse() {
        Value v = parseValue();
        skipWhitespace();
        if (pos != text.size()) fail("trailing content after top-level value");
        return v;
    }

    Value parseValue() {
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return Value(parseString());
            case 't': case 'f': return parseBool();
            case 'n': return parseNull();
            default: return parseNumber();
        }
    }

    Value parseObject() {
        expect('{');
        Object obj;
        if (peek() == '}') { ++pos; return Value(std::move(obj)); }
        while (true) {
            std::string key = parseString();
            expect(':');
            obj[key] = parseValue();
            char c = peek();
            if (c == ',') { ++pos; continue; }
            if (c == '}') { ++pos; break; }
            fail("expected ',' or '}'");
        }
        return Value(std::move(obj));
    }

    Value parseArray() {
        expect('[');
        Array arr;
        if (peek() == ']') { ++pos; return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parseValue());
            char c = peek();
            if (c == ',') { ++pos; continue; }
            if (c == ']') { ++pos; break; }
            fail("expected ',' or ']'");
        }
        return Value(std::move(arr));
    }

    std::string parseString() {
        if (peek() != '"') fail("expected a string");
        ++pos;
        std::string out;
        while (true) {
            if (pos >= text.size()) fail("unterminated string");
            char c = text[pos++];
            if (c == '"') break;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos >= text.size()) fail("unterminated escape");
            char esc = text[pos++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > text.size()) fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = text[pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= unsigned(h - 'A' + 10);
                        else fail("bad hex digit in \\u escape");
                    }
                    // UTF-8 encode. Surrogate pairs are left as-is; config files
                    // holding astral-plane characters are not a real case here.
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: fail("unknown escape character");
            }
        }
        return out;
    }

    Value parseBool() {
        if (text.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
        if (text.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
        fail("expected true or false");
    }

    Value parseNull() {
        if (text.compare(pos, 4, "null") == 0) { pos += 4; return Value(); }
        fail("expected null");
    }

    Value parseNumber() {
        size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
        bool digits = false;
        while (pos < text.size()) {
            char c = text[pos];
            if ((c >= '0' && c <= '9')) { digits = true; ++pos; }
            else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') { ++pos; }
            else break;
        }
        if (!digits) fail("expected a value");
        try {
            return Value(std::stod(text.substr(start, pos - start)));
        } catch (const std::exception&) {
            pos = start;
            fail("malformed number");
        }
    }
};

}  // namespace detail

inline Value parse(const std::string& text) {
    detail::Parser parser(text);
    return parser.parse();
}

}  // namespace vc::json
