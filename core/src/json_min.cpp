#include "pbc/json_min.h"

namespace pbc {

const JsonValue* JsonValue::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

std::optional<int64_t> JsonValue::as_int() const {
    if (type != Type::Number || text.empty()) return std::nullopt;
    size_t i = 0;
    const bool negative = text[0] == '-';
    if (negative) i = 1;
    if (i >= text.size()) return std::nullopt;
    uint64_t magnitude = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return std::nullopt;   // a fraction or an exponent
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) return std::nullopt;
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        if (magnitude > static_cast<uint64_t>(INT64_MAX) + 1) return std::nullopt;
        return static_cast<int64_t>(0 - magnitude);
    }
    if (magnitude > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<int64_t>(magnitude);
}

namespace {

class Parser {
public:
    Parser(std::string_view s, const JsonLimits& l) : s_(s), limits_(l) {}

    Result<JsonValue> run() {
        Result<JsonValue> r;
        if (s_.size() > limits_.max_bytes) return failure("the document is larger than the limit");
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
        skip_ws();
        if (!value(&r.value, 1)) return failure(detail_);
        skip_ws();
        if (i_ != s_.size()) return failure("text after the document");
        return r;
    }

private:
    Result<JsonValue> failure(const std::string& detail) const {
        Result<JsonValue> r;
        r.error = make_error(ErrorClass::Protocol, "protocol_error", detail);
        return r;
    }
    bool fail(const char* detail) {
        if (detail_.empty()) detail_ = detail;
        return false;
    }
    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool literal(const char* word) {
        const std::string_view w(word);
        if (s_.compare(i_, w.size(), w) != 0) return false;
        i_ += w.size();
        return true;
    }

    bool value(JsonValue* out, int depth) {
        if (++nodes_ > limits_.max_nodes) return fail("too many values");
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out->type = JsonValue::Type::String;
            return string(&out->text);
        }
        if (c == 't') {
            if (!literal("true")) return fail("not a value");
            out->type = JsonValue::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return fail("not a value");
            out->type = JsonValue::Type::Bool;
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return fail("not a value");
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("not a value");
    }

    bool number(JsonValue* out) {
        const size_t start = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return fail("bad number");
        if (s_[i_] == '0') {
            ++i_;
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            return fail("bad number");
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad number");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad number");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        out->type = JsonValue::Type::Number;
        out->text.assign(s_.substr(start, i_ - start));
        return true;
    }

    static void append_utf8(std::string* out, uint32_t cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(uint32_t* out) {
        if (i_ + 4 > s_.size()) return fail("bad escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad escape");
        }
        i_ += 4;
        *out = v;
        return true;
    }

    bool string(std::string* out) {
        ++i_;   // the opening quote
        out->clear();
        for (;;) {
            if (i_ >= s_.size()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                continue;
            }
            if (i_ >= s_.size()) return fail("unterminated string");
            const char e = s_[i_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {   // a high surrogate needs its low partner
                        if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return fail("lone surrogate");
                        i_ += 2;
                        uint32_t low = 0;
                        if (!hex4(&low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail("lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    bool array(JsonValue* out, int depth) {
        if (depth > limits_.max_depth) return fail("nested too deeply");
        out->type = JsonValue::Type::Array;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            out->items.emplace_back();
            skip_ws();
            if (!value(&out->items.back(), depth + 1)) return false;
            skip_ws();
            if (i_ >= s_.size()) return fail("unexpected end");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool object(JsonValue* out, int depth) {
        if (depth > limits_.max_depth) return fail("nested too deeply");
        out->type = JsonValue::Type::Object;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected a key");
            std::string key;
            if (!string(&key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
            ++i_;
            skip_ws();
            out->members.emplace_back(std::move(key), JsonValue());
            if (!value(&out->members.back().second, depth + 1)) return false;
            skip_ws();
            if (i_ >= s_.size()) return fail("unexpected end");
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected , or }");
        }
    }

    std::string_view s_;
    JsonLimits limits_;
    size_t i_ = 0;
    size_t nodes_ = 0;
    std::string detail_;
};

}  // namespace

Result<JsonValue> parse_json(std::string_view json, const JsonLimits& limits) { return Parser(json, limits).run(); }

}  // namespace pbc
