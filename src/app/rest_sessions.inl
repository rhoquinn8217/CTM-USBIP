// Definition of the REST session snapshot.
//
// WHY THIS FILE EXISTS. The function reads g_agent_sessions, which agent.inl
// owns; agent.inl in turn calls rest_open_listener and rest_handle_client. So
// neither file can be included before the other. rest.inl DECLARES it, this
// file DEFINES it, and this file is included after agent.inl.
//
// ⭐ The alternative was leaving the function inside agent.inl, which is an
// upstream file. "The data it reads lives there" is not a reason for our code
// to live there -- that is how a fork accumulates edits it cannot later
// separate, and it is a mistake this project has made before.

#pragma once

static std::vector<RestSessionSnapshot> collect_bridge_session_snapshots()
{
    std::vector<RestSessionSnapshot> out;
    std::lock_guard<std::mutex> lock(g_agent_sessions_mutex);
    out.reserve(g_agent_sessions.size());
    for (const auto &session : g_agent_sessions) {
        RestSessionSnapshot snap;
        snap.busid = session->busIdAscii;
        snap.kind = session->kind;
        snap.port = session->port;
        snap.ready = session->ready.load();
        {
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            snap.lastError = narrow_ascii(session->lastError);
        }
        out.push_back(std::move(snap));
    }
    return out;
}
