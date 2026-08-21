// Unit tests for the parser half of src/app/rest.inl: the HTTP head parser,
// the flat-JSON parser, percent-decoding, busid/kind validation, bearer
// matching and response formatting. No winsock, no agent state.
//
// rest.inl is included here directly rather than through units.h because its
// CTM_REST_PARSER_ONLY guard makes the parser half self-contained -- it needs
// none of the device_log/device_config chain, and pulling the unguarded half
// in would drag winsock and the agent's session list into the test binary.

#define CTM_REST_PARSER_ONLY 1

#include "harness.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace rest_units {
#include "../src/app/rest.inl"
} // namespace rest_units

using namespace rest_units;

static void test_json_escape()
{
    ctmtest::section("rest: json escape");
    CTM_CHECK_EQ(rest_json_escape("plain"), "plain");
    CTM_CHECK_EQ(rest_json_escape("a\"b\\c"), "a\\\"b\\\\c");
    CTM_CHECK_EQ(rest_json_escape(std::string("x\ny")), "x\\u000ay");
    CTM_CHECK_EQ(rest_json_escape("caf\xc3\xa9"), "caf\xc3\xa9"); // UTF-8 passes through
}

static void test_flat_json_valid()
{
    ctmtest::section("rest: flat json (valid)");
    CtmRestJson json;
    std::string error;
    CTM_CHECK(rest_parse_flat_json("", &json, &error));
    CTM_CHECK(rest_parse_flat_json("   \r\n ", &json, &error));
    CTM_CHECK(rest_parse_flat_json("{}", &json, &error));

    json = CtmRestJson();
    CTM_CHECK(rest_parse_flat_json(
        "{\"kind\":\"ds5\", \"port\": 48100,\n \"busid\":\"ctm-1\", \"flag\":true, \"x\":null}",
        &json, &error));
    CTM_CHECK_EQ(json.strings["kind"], "ds5");
    CTM_CHECK_EQ(json.strings["busid"], "ctm-1");
    CTM_CHECK_EQ(json.numbers["port"], 48100);
    CTM_CHECK_EQ(json.bools["flag"], true);
    CTM_CHECK(json.strings.count("x") == 0 && json.numbers.count("x") == 0);

    json = CtmRestJson();
    CTM_CHECK(rest_parse_flat_json("{\"n\":-7}", &json, &error));
    CTM_CHECK_EQ(json.numbers["n"], -7);

    json = CtmRestJson();
    CTM_CHECK(rest_parse_flat_json("{\"s\":\"a\\u0041\\t\"}", &json, &error));
    CTM_CHECK_EQ(json.strings["s"], "aA\t");

    json = CtmRestJson();
    CTM_CHECK(rest_parse_flat_json("{\"s\":\"\\u00e9\"}", &json, &error)); // é -> 2-byte UTF-8
    CTM_CHECK_EQ(json.strings["s"], "\xc3\xa9");
}

static void test_flat_json_invalid()
{
    ctmtest::section("rest: flat json (invalid)");
    CtmRestJson json;
    std::string error;
    CTM_CHECK(!rest_parse_flat_json("[1,2]", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":{\"b\":1}}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":[1]}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":1.5}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":1e3}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":1} trailing", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":\"unterminated}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":-}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\" 1}", &json, &error));
    CTM_CHECK(!rest_parse_flat_json("{\"a\":\"\\ud83d\\ude00\"}", &json, &error)); // surrogates rejected
    CTM_CHECK(!rest_parse_flat_json("{\"a\":tru}", &json, &error));
}

static void test_percent_decode()
{
    ctmtest::section("rest: percent decode");
    std::string out;
    CTM_CHECK(rest_percent_decode("/api/v1/sessions/ctm-1", &out) && out == "/api/v1/sessions/ctm-1");
    CTM_CHECK(rest_percent_decode("/a%20b", &out) && out == "/a b");
    CTM_CHECK(rest_percent_decode("/a%2Fb", &out) && out == "/a/b");
    CTM_CHECK(!rest_percent_decode("/a%2", &out));
    CTM_CHECK(!rest_percent_decode("/a%zz", &out));
    CTM_CHECK(!rest_percent_decode("/a%00b", &out));
}

static void test_head_parse()
{
    ctmtest::section("rest: http head parse");
    CtmRestRequest req;
    std::string error;
    const std::string head =
        "POST /api/v1/sessions?verbose=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:48055\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length:  42 \r\n"
        "Authorization: Bearer sekrit\r\n";
    CTM_CHECK(rest_parse_head(head, &req, &error));
    CTM_CHECK_EQ(req.method, "POST");
    CTM_CHECK_EQ(req.path, "/api/v1/sessions");
    CTM_CHECK_EQ(req.query, "verbose=1");
    CTM_CHECK_EQ(req.headers["host"], "127.0.0.1:48055");
    CTM_CHECK_EQ(req.headers["content-length"], "42");
    CTM_CHECK_EQ(req.headers["authorization"], "Bearer sekrit");

    req = CtmRestRequest();
    CTM_CHECK(rest_parse_head("GET /api/v1/status HTTP/1.1", &req, &error));
    CTM_CHECK(req.method == "GET" && req.path == "/api/v1/status" && req.query.empty());

    req = CtmRestRequest();
    CTM_CHECK(!rest_parse_head("GARBAGE", &req, &error));
    CTM_CHECK(!rest_parse_head("GET  HTTP/1.1", &req, &error));
    CTM_CHECK(!rest_parse_head("GET /x SPDY/3", &req, &error));
    CTM_CHECK(!rest_parse_head("GET no-slash HTTP/1.1", &req, &error));
    CTM_CHECK(!rest_parse_head("GET /a%zz HTTP/1.1", &req, &error));
    CTM_CHECK(!rest_parse_head("GET /x HTTP/1.1\r\nBadHeaderNoColon\r\n", &req, &error));
}

static void test_busid_and_kind()
{
    ctmtest::section("rest: busid/kind validation");
    CTM_CHECK(rest_valid_busid("ctm-1"));
    CTM_CHECK(rest_valid_busid("a"));
    CTM_CHECK(rest_valid_busid(std::string(31, 'x')));
    CTM_CHECK(!rest_valid_busid(""));
    CTM_CHECK(!rest_valid_busid(std::string(32, 'x')));
    CTM_CHECK(!rest_valid_busid("has space"));
    CTM_CHECK(!rest_valid_busid("ctrl\x01"));
    CTM_CHECK(!rest_valid_busid("caf\xc3\xa9"));

    // Mirror of the kind list in handle_agent_client. If a kind is added there
    // and not in rest_valid_kind (or vice versa), this is the test that fails.
    CTM_CHECK(rest_valid_kind("ds4") && rest_valid_kind("ds5") && rest_valid_kind("hid") &&
              rest_valid_kind("puck") && rest_valid_kind("xbox"));
    CTM_CHECK(rest_valid_kind("ds5_usb") && rest_valid_kind("ds5e_usb"));
    CTM_CHECK(!rest_valid_kind("DS5") && !rest_valid_kind("switch") && !rest_valid_kind(""));
}

static void test_bearer()
{
    ctmtest::section("rest: bearer matching");
    CTM_CHECK(rest_bearer_matches("Bearer sekrit", "sekrit"));
    CTM_CHECK(rest_bearer_matches("bearer sekrit", "sekrit"));
    CTM_CHECK(rest_bearer_matches("BEARER   sekrit  ", "sekrit"));
    CTM_CHECK(!rest_bearer_matches("Bearer sekri", "sekrit"));
    CTM_CHECK(!rest_bearer_matches("Bearer sekrits", "sekrit"));
    CTM_CHECK(!rest_bearer_matches("Basic sekrit", "sekrit"));
    CTM_CHECK(!rest_bearer_matches("Bearer ", "sekrit"));
    CTM_CHECK(!rest_bearer_matches("", "sekrit"));
}

static void test_response_format()
{
    ctmtest::section("rest: response formatting");
    const std::string ok = rest_http_response(200, "{\"a\":1}");
    CTM_CHECK(ok.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CTM_CHECK(ok.find("Content-Type: application/json\r\n") != std::string::npos);
    CTM_CHECK(ok.find("Content-Length: 7\r\n") != std::string::npos);
    CTM_CHECK(ok.find("Connection: close\r\n") != std::string::npos);
    CTM_CHECK_EQ(ok.substr(ok.size() - 7), "{\"a\":1}");

    const std::string noContent = rest_http_response(204, "", "Allow: GET\r\n");
    CTM_CHECK(noContent.rfind("HTTP/1.1 204 No Content\r\n", 0) == 0);
    CTM_CHECK(noContent.find("Allow: GET\r\n") != std::string::npos);
    CTM_CHECK(noContent.find("Content-Length: 0\r\n\r\n") == noContent.size() - 21);

    const std::string err = rest_error_response(404, "no such \"thing\"");
    CTM_CHECK(err.find("{\"error\":\"no such \\\"thing\\\"\"}") != std::string::npos);
}

int run_rest_parser_tests()
{
    test_json_escape();
    test_flat_json_valid();
    test_flat_json_invalid();
    test_percent_decode();
    test_head_parse();
    test_busid_and_kind();
    test_bearer();
    test_response_format();
    return 0;
}
