// The minimal JSON reader for the OCS and status.php answers (core/include/pbc/json_min.h). Strict RFC 8259, total on any input:
// a tree or an error, never a crash or a hang (constitution Principle I: parsers of untrusted input have hostile-input tests).
#include <string>

#include "pbc/json_min.h"
#include "test_support.h"

using pbc::JsonLimits;
using pbc::JsonValue;
using pbc::parse_json;

namespace {
bool ok(const std::string& s, const JsonLimits& l = JsonLimits()) { return parse_json(s, l).ok(); }
}  // namespace

PB_TEST(it_reads_objects_arrays_strings_numbers_and_literals) {
    const auto r = parse_json(R"({"installed":true,"maintenance":false,"version":"31.0.4.1","n":-12,"f":1.5e3,"none":null,"list":[1,"two",{"k":[]}]})");
    PB_CHECK(r.ok());
    const JsonValue& v = r.value;
    PB_CHECK(v.type == JsonValue::Type::Object);
    PB_CHECK(v.get("installed") && v.get("installed")->type == JsonValue::Type::Bool && v.get("installed")->boolean);
    PB_CHECK(v.get("maintenance") && !v.get("maintenance")->boolean);
    PB_CHECK_EQ(v.get("version")->text, std::string("31.0.4.1"));
    PB_CHECK_EQ(*v.get("n")->as_int(), int64_t(-12));
    PB_CHECK(v.get("f")->type == JsonValue::Type::Number);
    PB_CHECK(!v.get("f")->as_int().has_value());   // not an integer literal
    PB_CHECK(v.get("none")->type == JsonValue::Type::Null);
    PB_CHECK_EQ(v.get("list")->items.size(), size_t(3));
    PB_CHECK_EQ(v.get("list")->items[1].text, std::string("two"));
    PB_CHECK(v.get("list")->items[2].get("k")->type == JsonValue::Type::Array);
    PB_CHECK(v.get("absent") == nullptr);
    PB_CHECK(v.items.empty());
}

PB_TEST(string_escapes_are_decoded_to_utf8) {
    const auto r = parse_json(R"(["a\nb","q\"q","b\\s","sl\/ash","\u00e4","\u20ac","\ud83d\ude00","\u0041"])");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.items[0].text, std::string("a\nb"));
    PB_CHECK_EQ(r.value.items[1].text, std::string("q\"q"));
    PB_CHECK_EQ(r.value.items[2].text, std::string("b\\s"));
    PB_CHECK_EQ(r.value.items[3].text, std::string("sl/ash"));
    PB_CHECK_EQ(r.value.items[4].text, std::string("\xC3\xA4"));
    PB_CHECK_EQ(r.value.items[5].text, std::string("\xE2\x82\xAC"));
    PB_CHECK_EQ(r.value.items[6].text, std::string("\xF0\x9F\x98\x80"));
    PB_CHECK_EQ(r.value.items[7].text, std::string("A"));
}

PB_TEST(integers_are_range_checked) {
    PB_CHECK_EQ(*parse_json("9223372036854775807").value.as_int(), int64_t(9223372036854775807ll));
    PB_CHECK(!parse_json("9223372036854775808").value.as_int().has_value());
    PB_CHECK_EQ(*parse_json("-9223372036854775808").value.as_int(), int64_t(-9223372036854775807ll - 1));
    PB_CHECK(parse_json("1e999").ok());                        // a literal is kept as text
    PB_CHECK(!parse_json("1e999").value.as_int().has_value());
    PB_CHECK_EQ(*parse_json("0").value.as_int(), int64_t(0));
    PB_CHECK(!parse_json("\"12\"").value.as_int().has_value());   // a string is not a number
}

PB_TEST(duplicate_keys_keep_every_member_and_get_returns_the_first) {
    const auto r = parse_json(R"({"a":1,"a":2})");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.members.size(), size_t(2));
    PB_CHECK_EQ(*r.value.get("a")->as_int(), int64_t(1));
}

PB_TEST(malformed_input_is_an_error_not_a_guess) {
    for (const char* bad : {
             "", "   ", "{", "}", "[", "]", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1,]", "[,1]", "{,}", "{'a':1}", "[1 2]",
             "tru", "nul", "falsy", "NaN", "Infinity", "-", "+1", ".5", "1.", "01", "-01", "1e", "1e+", "0x10",
             "\"abc", "\"a\\x\"", "\"a\\u12\"", "\"a\\uZZZZ\"", "\"\\ud83d\"", "\"\\ude00\"", "\"\\ud83dX\"",
             "{\"a\":1} x", "[1]]", "{\"a\":1}{\"b\":2}", "// c\n1", "/* c */1", "[1,2,3",
         })
        PB_CHECK(!ok(bad));
    PB_CHECK(!ok(std::string("\"a\x01z\"")));      // a raw control character inside a string
    PB_CHECK(!ok(std::string("\"a\nz\"")));
    PB_CHECK(!ok(std::string("{\"a\0b\":1}", 9)));  // NUL
    PB_CHECK_EQ(parse_json("x").error.key, std::string("protocol_error"));
}

PB_TEST(limits_for_size_depth_and_node_count_hold) {
    JsonLimits l;
    l.max_bytes = 20;
    PB_CHECK(ok("[1,2,3]", l));
    PB_CHECK(!ok("[1,2,3,4,5,6,7,8,9,10,11,12]", l));
    JsonLimits d;
    d.max_depth = 32;
    PB_CHECK(ok(std::string(32, '[') + std::string(32, ']'), d));
    PB_CHECK(!ok(std::string(33, '[') + std::string(33, ']'), d));
    PB_CHECK(!ok(std::string(100000, '[')));   // far too deep and truncated: no stack overflow
    JsonLimits n;
    n.max_nodes = 5;
    PB_CHECK(ok("[1,2,3,4]", n));
    PB_CHECK(!ok("[1,2,3,4,5,6]", n));
}

PB_TEST(every_prefix_of_a_valid_document_is_handled) {
    const std::string doc = R"({"ocs":{"meta":{"status":"ok","statuscode":100},"data":{"version":{"major":31,"string":"31.0.4"},"list":[1,2,{"x":"\u00e4"}]}}})";
    PB_CHECK(ok(doc));
    for (size_t i = 0; i < doc.size(); ++i) PB_CHECK(!ok(doc.substr(0, i)));   // a cut-off document is never accepted
}

PB_TEST(a_leading_byte_order_mark_is_skipped) {
    PB_CHECK(ok("\xEF\xBB\xBF{\"a\":1}"));
    PB_CHECK(!ok("\xEF\xBB{\"a\":1}"));
}

PB_TEST_MAIN()
