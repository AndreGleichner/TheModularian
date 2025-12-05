#pragma once

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#define SPDLOG_USE_STD_FORMAT
#define SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_PREVENT_CHILD_FD
#define SPDLOG_LEVEL_NAMES                              \
    {                                                   \
        "TRC", "DBG", "INF", "WRN", "ERR", "CRT", "OFF" \
    }
#define SPDLOG_SHORT_LEVEL_NAMES          \
    {                                     \
        "T", "D", "I", "W", "E", "C", "O" \
    }
#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
