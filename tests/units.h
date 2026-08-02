// Brings the units under test into scope for the test binary.
//
// WHY THIS FILE EXISTS. The .inl files are not self-contained and cannot be:
// main.cpp includes them all INSIDE AN ANONYMOUS NAMESPACE, so a standard
// header written at the top of one of them would land inside that namespace
// and fail to compile. The only workable shape is the one main.cpp already
// uses -- standard headers outside, our files inside -- which this mirrors.
//
// !! KEEP THE ORDER BELOW MATCHING main.cpp. device_log defines the logging
// !! the other three call; device_config defines the lookups the overrides and
// !! the gains call.
//
// WHAT IS DELIBERATELY ABSENT. Only files with no dependency on the earlier
// include chain are here. ds5_apply_settings.inl needs the backend type,
// agent_session_sweep.inl needs the agent's session list, and config_watcher
// .inl spawns a thread -- pulling any of them in would drag in ENet, sockets
// and the map runtime, i.e. most of the program, to test a few dozen lines.
//
// If a needed header is ever added to main.cpp and not here, this file fails
// to compile with a plain missing-symbol error. Noisy, not silent.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>

namespace units {

#include "log/device_log.inl"
#include "config/device_config.inl"
#include "audio/audio_gain.inl"
#include "audio/ds5_output_overrides.inl"

}  // namespace units
