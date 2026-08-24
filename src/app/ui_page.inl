// Serving the settings page.
//
// ⭐ WHY THE AGENT SERVES IT. Someone given only ctm-usbip.exe has no tools
// folder, so a page loaded from a file path does not exist for them. Serving it
// from the binary means the exe is the whole product.
//
// Three things fall out of it that were awkward before:
//   - the page cannot go stale against the agent, because the agent serves it
//   - same-origin, so the CORS handling exists only for other callers now
//   - a phone on the LAN reaches it by browsing to the address, with no file
//     to copy onto the device
//
// ⚠️ AND ONE THING IS LOST. A file:// page opens whether or not the agent is
// running, so its "can't reach the listener" screen could greet you on a dead
// agent. Served over HTTP there is nothing to load, and you get the browser's
// own error page instead. That screen now only appears when the agent dies
// AFTER the page is open.

#pragma once

namespace ctm_ui_page {

// ⭐ DISK WINS WHEN IT IS THERE.
//
// Development edits tools/controller-config-test-client.html and reloads; being
// forced to rebuild for a CSS change would make the page far slower to work on.
// A released exe has no such file and falls through to the embedded copy.
//
// ⚠️ Which means a user who happens to have a tools folder beside the agent
// gets that page instead of the built-in one. Deliberate -- it is the same rule
// that makes development work -- but it is a surprise worth knowing about, so
// the log says which one was served.
inline bool read_from_disk(std::string *out)
{
    std::ifstream file("tools\\controller-config-test-client.html", std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream all;
    all << file.rdbuf();
    *out = all.str();
    return !out->empty();
}

inline std::string current(bool *fromDisk)
{
    std::string disk;
    if (read_from_disk(&disk)) {
        *fromDisk = true;
        return disk;
    }
    *fromDisk = false;
    return std::string(reinterpret_cast<const char *>(kHtmlBytes), kHtmlLength);
}

// The HTTP response. Not JSON, so it does not go through rest_http_response.
//
// ⛔ NO CACHING. The page is served from a file that changes while you work on
// it, and a browser holding yesterday's copy is exactly the confusion this
// whole arrangement exists to remove.
inline std::string http_response()
{
    bool fromDisk = false;
    const std::string body = current(&fromDisk);
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/html; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

} // namespace ctm_ui_page
