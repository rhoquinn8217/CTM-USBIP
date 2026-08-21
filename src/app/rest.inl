// Optional HTTP/JSON control API for the agent (`--rest <port>`). This is a
// sibling of the plaintext control channel in agent.inl, not a replacement:
// the TV keeps speaking BRIDGE_START/STOP on the agent port, while the REST
// listener serves tooling (curl, scripts, dashboards) on a second port.
//
// Threading: the REST listener lives in run_agent's select() loop and requests
// are handled inline on the agent loop thread — the only thread allowed to
// start/stop sessions (see the reap-queue comment at the top of agent.inl).
// Handlers may therefore call start_bridge_session / stop_bridge_session /
// stop_all_bridge_sessions directly, exactly like handle_agent_client does.
// Do NOT move rest_handle_client onto its own thread without also routing
// session lifecycle changes back through the loop.
//
// Security posture: binds 127.0.0.1 unless --rest-lan; optional bearer token
// via --rest-token. No CORS headers are sent on purpose — without them a
// browser cannot make cross-origin JSON POSTs or DELETEs to this port, which
// blocks drive-by CSRF against a loopback listener. Same-origin tooling and
// anything that isn't a browser (curl, scripts) is unaffected.
//
// The parser half of this file is host-testable: tests/rest_parser_test.cpp
// (a suite in the build-tests.ps1 harness) compiles it with
// REST_PARSER_ONLY; everything that touches winsock or agent state stays
// behind that guard at the bottom.

// ---------------------------------------------------------------------------
// Pure helpers (no winsock, no agent state) — covered by the host-side tests.
// ---------------------------------------------------------------------------

struct RestRequest {
    std::string method;
    std::string path;   // percent-decoded, query stripped
    std::string query;  // raw, without '?'
    std::map<std::string, std::string> headers; // keys lowercased, values trimmed
    std::string body;
};

// Flat JSON object: string / integer / bool members only. Nested objects,
// arrays and floats are rejected with an error message — every body this API
// accepts is flat, and refusing the rest keeps the parser small and auditable.
struct RestJson {
    std::map<std::string, std::string> strings;
    std::map<std::string, long long> numbers;
    std::map<std::string, bool> bools;
};

static std::string rest_json_escape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", u);
            out += buf;
        } else {
            out += c; // UTF-8 bytes pass through unescaped (valid JSON)
        }
    }
    return out;
}

static void rest_skip_ws(const std::string &s, size_t *i)
{
    while (*i < s.size() &&
           (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n')) {
        ++*i;
    }
}

static bool rest_parse_hex4(const std::string &s, size_t i, unsigned *value)
{
    if (i + 4 > s.size()) return false;
    unsigned v = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char c = s[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
    }
    *value = v;
    return true;
}

static bool rest_parse_json_string(const std::string &s, size_t *i, std::string *out, std::string *error)
{
    if (*i >= s.size() || s[*i] != '"') {
        if (error) *error = "expected string";
        return false;
    }
    ++*i;
    out->clear();
    while (*i < s.size()) {
        const char c = s[*i];
        if (c == '"') {
            ++*i;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            if (error) *error = "raw control character in string";
            return false;
        }
        if (c != '\\') {
            out->push_back(c);
            ++*i;
            continue;
        }
        ++*i;
        if (*i >= s.size()) break;
        const char esc = s[(*i)++];
        switch (esc) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
            unsigned cp = 0;
            if (!rest_parse_hex4(s, *i, &cp)) {
                if (error) *error = "bad \\u escape";
                return false;
            }
            *i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                // Surrogate pairs never appear in the flat ASCII-ish bodies
                // this API takes; reject instead of silently mangling.
                if (error) *error = "surrogate \\u escapes not supported";
                return false;
            }
            if (cp < 0x80) {
                out->push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default:
            if (error) *error = "bad escape";
            return false;
        }
    }
    if (error) *error = "unterminated string";
    return false;
}

// Empty / whitespace-only body parses as an empty object (POST /restart with
// no body is legal).
static bool rest_parse_flat_json(const std::string &body, RestJson *out, std::string *error)
{
    size_t i = 0;
    rest_skip_ws(body, &i);
    if (i >= body.size()) {
        return true;
    }
    if (body[i] != '{') {
        if (error) *error = "body must be a JSON object";
        return false;
    }
    ++i;
    rest_skip_ws(body, &i);
    if (i < body.size() && body[i] == '}') {
        ++i;
    } else {
        for (;;) {
            rest_skip_ws(body, &i);
            std::string key;
            if (!rest_parse_json_string(body, &i, &key, error)) {
                return false;
            }
            rest_skip_ws(body, &i);
            if (i >= body.size() || body[i] != ':') {
                if (error) *error = "expected ':' after key";
                return false;
            }
            ++i;
            rest_skip_ws(body, &i);
            if (i >= body.size()) {
                if (error) *error = "truncated object";
                return false;
            }
            const char c = body[i];
            if (c == '"') {
                std::string value;
                if (!rest_parse_json_string(body, &i, &value, error)) {
                    return false;
                }
                out->strings[key] = value;
            } else if (c == '{' || c == '[') {
                if (error) *error = "nested values not supported";
                return false;
            } else if (body.compare(i, 4, "true") == 0) {
                out->bools[key] = true;
                i += 4;
            } else if (body.compare(i, 5, "false") == 0) {
                out->bools[key] = false;
                i += 5;
            } else if (body.compare(i, 4, "null") == 0) {
                i += 4; // accepted and ignored
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                size_t start = i;
                if (body[i] == '-') ++i;
                while (i < body.size() && body[i] >= '0' && body[i] <= '9') ++i;
                if (i == start || (body[start] == '-' && i == start + 1)) {
                    if (error) *error = "bad number";
                    return false;
                }
                if (i < body.size() && (body[i] == '.' || body[i] == 'e' || body[i] == 'E')) {
                    if (error) *error = "integers only";
                    return false;
                }
                out->numbers[key] = strtoll(body.c_str() + start, nullptr, 10);
            } else {
                if (error) *error = "bad value";
                return false;
            }
            rest_skip_ws(body, &i);
            if (i < body.size() && body[i] == ',') {
                ++i;
                continue;
            }
            if (i < body.size() && body[i] == '}') {
                ++i;
                break;
            }
            if (error) *error = "expected ',' or '}'";
            return false;
        }
    }
    rest_skip_ws(body, &i);
    if (i != body.size()) {
        if (error) *error = "trailing data after object";
        return false;
    }
    return true;
}

static bool rest_percent_decode(const std::string &in, std::string *out)
{
    out->clear();
    out->reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '%') {
            out->push_back(c);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        unsigned hi = 0, lo = 0;
        auto hex = [](char h, unsigned *v) {
            if (h >= '0' && h <= '9') { *v = static_cast<unsigned>(h - '0'); return true; }
            if (h >= 'a' && h <= 'f') { *v = static_cast<unsigned>(h - 'a' + 10); return true; }
            if (h >= 'A' && h <= 'F') { *v = static_cast<unsigned>(h - 'A' + 10); return true; }
            return false;
        };
        if (!hex(in[i + 1], &hi) || !hex(in[i + 2], &lo)) return false;
        const unsigned value = (hi << 4) | lo;
        if (value == 0) return false;
        out->push_back(static_cast<char>(value));
        i += 2;
    }
    return true;
}

// Parses the head (request line + headers), i.e. everything before CRLFCRLF.
static bool rest_parse_head(const std::string &head, RestRequest *req, std::string *error)
{
    size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = head.size();
    const std::string requestLine = head.substr(0, lineEnd);

    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        if (error) *error = "malformed request line";
        return false;
    }
    req->method = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = requestLine.substr(sp2 + 1);
    if (req->method.empty() || target.empty() || target[0] != '/' ||
        version.compare(0, 7, "HTTP/1.") != 0) {
        if (error) *error = "malformed request line";
        return false;
    }
    const size_t q = target.find('?');
    if (q != std::string::npos) {
        req->query = target.substr(q + 1);
        target.resize(q);
    }
    if (!rest_percent_decode(target, &req->path)) {
        if (error) *error = "bad percent-encoding in path";
        return false;
    }

    size_t pos = lineEnd;
    while (pos < head.size()) {
        if (head.compare(pos, 2, "\r\n") == 0) pos += 2;
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        if (next == pos) { pos = next; continue; }
        const std::string line = head.substr(pos, next - pos);
        pos = next;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            if (error) *error = "malformed header";
            return false;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t vs = 0;
        while (vs < value.size() && (value[vs] == ' ' || value[vs] == '\t')) ++vs;
        size_t ve = value.size();
        while (ve > vs && (value[ve - 1] == ' ' || value[ve - 1] == '\t')) --ve;
        req->headers[key] = value.substr(vs, ve - vs);
    }
    return true;
}

// Same rule the plaintext BRIDGE_START effectively enforces: 1..31 printable
// ASCII, no spaces (istringstream tokenization can't produce one anyway).
static bool rest_valid_busid(const std::string &busid)
{
    if (busid.empty() || busid.size() > 31) return false;
    for (const char c : busid) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) return false;
    }
    return true;
}

static bool rest_valid_kind(const std::string &kind)
{
    // Mirror of the kind list in handle_agent_client — keep the two in sync.
    return kind == "ds4" || kind == "ds5" || kind == "ds5_usb" || kind == "ds5e_usb" ||
           kind == "hid" || kind == "puck" || kind == "xbox";
}

static bool rest_bearer_matches(const std::string &headerValue, const std::string &token)
{
    static const char prefix[] = "bearer ";
    const size_t prefixLen = sizeof(prefix) - 1;
    if (headerValue.size() < prefixLen) return false;
    for (size_t i = 0; i < prefixLen; ++i) {
        if (std::tolower(static_cast<unsigned char>(headerValue[i])) != prefix[i]) return false;
    }
    size_t start = prefixLen;
    while (start < headerValue.size() && (headerValue[start] == ' ' || headerValue[start] == '\t')) ++start;
    size_t end = headerValue.size();
    while (end > start && (headerValue[end - 1] == ' ' || headerValue[end - 1] == '\t')) --end;
    const std::string candidate = headerValue.substr(start, end - start);
    // Length-independent-ish compare so a listener on the LAN doesn't leak the
    // token one matching prefix byte at a time.
    unsigned char diff = candidate.size() == token.size() ? 0 : 1;
    const size_t n = candidate.size() < token.size() ? candidate.size() : token.size();
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(candidate[i] ^ token[i]);
    }
    return diff == 0;
}

static const char *rest_reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

static std::string rest_http_response(int status, const std::string &jsonBody,
                                      const std::string &extraHeaders = std::string())
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + rest_reason_phrase(status) + "\r\n";
    out += "Connection: close\r\n";
    out += extraHeaders;
    if (status == 204) {
        out += "Content-Length: 0\r\n\r\n";
        return out;
    }
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n\r\n";
    out += jsonBody;
    return out;
}

static std::string rest_error_response(int status, const std::string &message,
                                       const std::string &extraHeaders = std::string())
{
    return rest_http_response(status, "{\"error\":\"" + rest_json_escape(message) + "\"}", extraHeaders);
}

#if !defined(REST_PARSER_ONLY)

// ---------------------------------------------------------------------------
// Agent glue + winsock transport (excluded from the host-side parser tests).
// ---------------------------------------------------------------------------

// Set from wmain before run_agent starts; never written afterwards.
static uint16_t g_rest_port = 0;          // 0 = REST disabled
static bool g_rest_bind_lan = false;      // default loopback-only
static std::string g_rest_token;          // empty = no auth

static std::chrono::steady_clock::time_point g_rest_agent_start;

struct RestSessionSnapshot {
    std::string busid;
    std::string kind;
    uint16_t port = 0;
    bool ready = false;
    std::string lastError;
};

// Defined in agent.inl (included after this file).
// Read-only view of the bridge sessions, for GET /status and GET /sessions.
//
// ⓘ DECLARED here, DEFINED in rest_sessions.inl. It reads g_agent_sessions,
// which agent.inl owns, and agent.inl calls into this file -- so the two
// cannot both be included first. Splitting the definition into its own file
// after agent.inl keeps the code OURS without fighting that cycle.
static std::vector<RestSessionSnapshot> collect_bridge_session_snapshots();
static bool start_bridge_session(const std::string &kind, uint16_t port, const std::wstring &busId, std::wstring *error);
static bool stop_bridge_session(const std::wstring &busId);
static void stop_all_bridge_sessions();

static std::string rest_session_json(const RestSessionSnapshot &snap)
{
    std::string out = "{\"busid\":\"" + rest_json_escape(snap.busid) + "\"";
    out += ",\"kind\":\"" + rest_json_escape(snap.kind) + "\"";
    out += ",\"port\":" + std::to_string(snap.port);
    out += ",\"ready\":" + std::string(snap.ready ? "true" : "false");
    out += ",\"last_error\":\"" + rest_json_escape(snap.lastError) + "\"}";
    return out;
}

static std::string rest_handle_status(uint16_t agentPort)
{
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_rest_agent_start).count();
    const std::vector<RestSessionSnapshot> sessions = collect_bridge_session_snapshots();
    std::string body = "{\"version\":\"" + rest_json_escape(CTM_VERSION_DISPLAY) + "\"";
    body += ",\"transport\":\"" + std::string(g_use_enet.load() ? "enet" : "tcp") + "\"";
    body += ",\"control_port\":" + std::to_string(agentPort);
    body += ",\"usbip_port\":" + std::to_string(kDefaultUsbipPort);
    body += ",\"uptime_seconds\":" + std::to_string(uptime);
    body += ",\"session_count\":" + std::to_string(sessions.size()) + "}";
    return rest_http_response(200, body);
}

static std::string rest_handle_sessions_post(const RestRequest &req)
{
    RestJson json;
    std::string jsonError;
    if (!rest_parse_flat_json(req.body, &json, &jsonError)) {
        return rest_error_response(400, jsonError);
    }
    const auto kindIt = json.strings.find("kind");
    const auto busidIt = json.strings.find("busid");
    const auto portIt = json.numbers.find("port");
    if (kindIt == json.strings.end() || busidIt == json.strings.end() || portIt == json.numbers.end()) {
        return rest_error_response(400, "kind (string), port (number) and busid (string) are required");
    }
    if (!rest_valid_kind(kindIt->second)) {
        return rest_error_response(400, "kind must be one of ds4, ds5, ds5_usb, ds5e_usb, hid, puck, xbox");
    }
    if (portIt->second < 1024 || portIt->second > 65535) {
        return rest_error_response(400, "port must be 1024..65535");
    }
    if (!rest_valid_busid(busidIt->second)) {
        return rest_error_response(400, "busid must be 1..31 printable ASCII characters");
    }
    std::wstring error;
    const std::wstring busId = widen_ascii(busidIt->second.c_str(), busidIt->second.size());
    if (!start_bridge_session(kindIt->second, static_cast<uint16_t>(portIt->second), busId, &error)) {
        const std::string err = narrow_ascii(error);
        return rest_error_response(500, err.empty() ? "bridge start failed" : err);
    }
    // Async on purpose: the session worker loads maps and waits for the client
    // after this returns (mirrors "OK bridge starting"). Poll GET
    // /api/v1/sessions/{busid} for ready/last_error. Idempotent: a busid that
    // already exists is left untouched and still answers 202.
    std::string body = "{\"busid\":\"" + rest_json_escape(busidIt->second) + "\"";
    body += ",\"kind\":\"" + rest_json_escape(kindIt->second) + "\"";
    body += ",\"port\":" + std::to_string(portIt->second);
    body += ",\"status\":\"starting\"}";
    return rest_http_response(202, body);
}

static std::string rest_handle_restart(const RestRequest &req)
{
    RestJson json;
    std::string jsonError;
    if (!rest_parse_flat_json(req.body, &json, &jsonError)) {
        return rest_error_response(400, jsonError);
    }
    std::string mode = "soft";
    const auto modeIt = json.strings.find("mode");
    if (modeIt != json.strings.end()) mode = modeIt->second;
    if (mode == "hard") {
        if (!request_service_restart()) {
            return rest_error_response(409, "hard restart requires service mode");
        }
        return rest_http_response(202, "{\"status\":\"service restarting\"}");
    }
    if (mode != "soft") {
        return rest_error_response(400, "mode must be \"soft\" or \"hard\"");
    }
    stop_all_bridge_sessions();
    return rest_http_response(200, "{\"status\":\"bridges reset\"}");
}

static std::string rest_route(const RestRequest &req, uint16_t agentPort)
{
    if (!g_rest_token.empty()) {
        const auto authIt = req.headers.find("authorization");
        if (authIt == req.headers.end() || !rest_bearer_matches(authIt->second, g_rest_token)) {
            return rest_error_response(401, "missing or invalid bearer token",
                                       "WWW-Authenticate: Bearer\r\n");
        }
    }
    if (req.method == "OPTIONS") {
        return rest_http_response(204, "", "Allow: GET, POST, DELETE, OPTIONS\r\n");
    }

    if (req.path == "/api/v1/status") {
        if (req.method != "GET") {
            return rest_error_response(405, "method not allowed", "Allow: GET, OPTIONS\r\n");
        }
        return rest_handle_status(agentPort);
    }

    if (req.path == "/api/v1/sessions") {
        if (req.method == "GET") {
            const std::vector<RestSessionSnapshot> sessions = collect_bridge_session_snapshots();
            std::string body = "[";
            for (size_t i = 0; i < sessions.size(); ++i) {
                if (i) body += ",";
                body += rest_session_json(sessions[i]);
            }
            body += "]";
            return rest_http_response(200, body);
        }
        if (req.method == "POST") {
            return rest_handle_sessions_post(req);
        }
        return rest_error_response(405, "method not allowed", "Allow: GET, POST, OPTIONS\r\n");
    }

    static const char kSessionsPrefix[] = "/api/v1/sessions/";
    if (req.path.compare(0, sizeof(kSessionsPrefix) - 1, kSessionsPrefix) == 0) {
        const std::string busid = req.path.substr(sizeof(kSessionsPrefix) - 1);
        if (!rest_valid_busid(busid) || busid.find('/') != std::string::npos) {
            return rest_error_response(404, "unknown path");
        }
        if (req.method == "GET") {
            const std::vector<RestSessionSnapshot> sessions = collect_bridge_session_snapshots();
            for (const RestSessionSnapshot &snap : sessions) {
                if (snap.busid == busid) {
                    return rest_http_response(200, rest_session_json(snap));
                }
            }
            return rest_error_response(404, "no such session");
        }
        if (req.method == "DELETE") {
            const std::wstring busId = widen_ascii(busid.c_str(), busid.size());
            if (stop_bridge_session(busId)) {
                return rest_http_response(200,
                    "{\"busid\":\"" + rest_json_escape(busid) + "\",\"stopped\":true}");
            }
            return rest_error_response(404, "no such session");
        }
        return rest_error_response(405, "method not allowed", "Allow: GET, DELETE, OPTIONS\r\n");
    }

    if (req.path == "/api/v1/restart") {
        if (req.method != "POST") {
            return rest_error_response(405, "method not allowed", "Allow: POST, OPTIONS\r\n");
        }
        return rest_handle_restart(req);
    }

    return rest_error_response(404, "unknown path");
}

static void rest_send_all(SOCKET sock, const std::string &data)
{
    size_t offset = 0;
    while (offset < data.size()) {
        const int n = send(sock, data.c_str() + offset,
                           static_cast<int>(data.size() - offset), 0);
        if (n <= 0) return;
        offset += static_cast<size_t>(n);
    }
}

static SOCKET rest_open_listener(std::wstring *error)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        if (error) *error = wsa_error_message(L"REST socket failed");
        return INVALID_SOCKET;
    }
    // SO_EXCLUSIVEADDRUSE, not SO_REUSEADDR: with SO_REUSEADDR another local
    // process could bind the same port and race us for connections (and see
    // bearer tokens). The agent's own control port keeps SO_REUSEADDR for its
    // established rebind-after-restart behavior; the REST port has no such
    // requirement — on Windows a cleanly closed listener rebinds without it.
    BOOL exclusive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(g_rest_bind_lan ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons(g_rest_port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(sock, 4) == SOCKET_ERROR) {
        if (error) *error = wsa_error_message(L"REST bind/listen failed");
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

static void rest_handle_client(SOCKET client, uint16_t agentPort)
{
    // Bounded I/O so a stalled peer can't wedge the agent loop for long. The
    // plaintext channel has the same inline-blocking shape; here we at least
    // put a ceiling on it.
    DWORD timeoutMs = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

    constexpr size_t kMaxHead = 16 * 1024;
    constexpr size_t kMaxBody = 8 * 1024;

    std::string raw;
    size_t headEnd = std::string::npos;
    while (raw.size() < kMaxHead) {
        char buf[2048];
        const int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break; // closed, reset, or receive timeout
        raw.append(buf, static_cast<size_t>(n));
        headEnd = raw.find("\r\n\r\n");
        if (headEnd != std::string::npos) break;
    }
    if (headEnd == std::string::npos) {
        rest_send_all(client, rest_error_response(
            raw.size() >= kMaxHead ? 413 : 408,
            raw.size() >= kMaxHead ? "request head too large" : "incomplete request"));
        return;
    }

    RestRequest req;
    std::string parseError;
    if (!rest_parse_head(raw.substr(0, headEnd), &req, &parseError)) {
        rest_send_all(client, rest_error_response(400, parseError));
        return;
    }

    size_t contentLength = 0;
    const auto lengthIt = req.headers.find("content-length");
    if (lengthIt != req.headers.end()) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(lengthIt->second.c_str(), &end, 10);
        if (end == lengthIt->second.c_str() || *end != '\0') {
            rest_send_all(client, rest_error_response(400, "bad content-length"));
            return;
        }
        contentLength = parsed;
    }
    if (contentLength > kMaxBody) {
        rest_send_all(client, rest_error_response(413, "body too large"));
        return;
    }
    if (req.headers.count("transfer-encoding") != 0) {
        rest_send_all(client, rest_error_response(400, "chunked bodies not supported"));
        return;
    }

    req.body = raw.substr(headEnd + 4);
    while (req.body.size() < contentLength) {
        char buf[2048];
        const int n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) {
            rest_send_all(client, rest_error_response(408, "incomplete body"));
            return;
        }
        req.body.append(buf, static_cast<size_t>(n));
    }
    req.body.resize(contentLength);

    rest_send_all(client, rest_route(req, agentPort));
}

#endif // !REST_PARSER_ONLY
