#pragma once

enum class LogLevel {
    Info,
    Warn
};

inline FILE* g_log_file = nullptr;

inline std::filesystem::path get_logs_path() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return std::filesystem::path("logs");
    }
    std::filesystem::path base_path(base);
    SDL_free((void*)base);
    return base_path / "logs";
}

inline void ensure_log_file() {
    if (g_log_file) {
        return;
    }

    try {
        auto logs_dir = get_logs_path();
        std::filesystem::create_directories(logs_dir);

        auto log_file_path = logs_dir / "latest.log";
        g_log_file = std::fopen(log_file_path.string().c_str(), "w");
    } catch (...) {
        return;
    }
}

inline void close_log_file() {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

inline std::string current_time_string() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(2) << tm.tm_hour << ":"
        << std::setw(2) << tm.tm_min  << ":"
        << std::setw(2) << tm.tm_sec;
    return oss.str();
}

inline const char* short_file_name(const char* path) {
    if (!path) return "unknown";

    const char* slash1 = std::strrchr(path, '/');
    const char* slash2 = std::strrchr(path, '\\');
    const char* base = nullptr;
    if (slash1 && slash2) {
        base = (slash1 > slash2) ? slash1 + 1 : slash2 + 1;
    } else if (slash1) {
        base = slash1 + 1;
    } else if (slash2) {
        base = slash2 + 1;
    } else {
        base = path;
    }

    static thread_local std::string cached;
    cached = std::filesystem::path(base).stem().string();
    return cached.c_str();
}

inline void log_message(const char* channel, LogLevel level, const std::string& msg) {
    ensure_log_file();

    std::string time_str = current_time_string();

    const char* level_str = nullptr;
    const char* color_start = "";
    const char* color_reset = "\033[0m";

    switch (level) {
        case LogLevel::Info:
            level_str = "INFO";
            color_start = "";
            break;
        case LogLevel::Warn:
            level_str = "WARN";
            color_start = "\033[33;1m";
            break;
    }

    std::ostringstream oss;
    oss << "[" << time_str << "] [" << channel << "/" << level_str << "]: " << msg;
    const std::string line = oss.str();

    std::ostream& out = std::cout;
    out << color_start << line << color_reset << std::endl;

    if (g_log_file) {
        std::fprintf(g_log_file, "%s\n", line.c_str());
        std::fflush(g_log_file);
    }
}

inline std::string vformat(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int len = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (len <= 0) return {};

    std::string buf(len, '\0');
    std::vsnprintf(buf.data(), buf.size() + 1, fmt, args);
    return buf;
}

inline void println_impl(const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(short_file_name(file), LogLevel::Info, msg);
}

inline void warnln_impl(const char* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(short_file_name(file), LogLevel::Warn, msg);
}

inline void devprintln_impl(const char* file, const char* fmt, ...) {
    if (!isDev) return;

    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(short_file_name(file), LogLevel::Info, msg);
}

inline void devwarnln_impl(const char* file, const char* fmt, ...) {
    if (!isDev) return;

    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(short_file_name(file), LogLevel::Warn, msg);
}

#define println(fmt, ...) \
    println_impl(__FILE__, fmt, ##__VA_ARGS__)

#define warnln(fmt, ...) \
    warnln_impl(__FILE__, fmt, ##__VA_ARGS__)

#define devprintln(fmt, ...) \
    devprintln_impl(__FILE__, fmt, ##__VA_ARGS__)

#define devwarnln(fmt, ...) \
    devwarnln_impl(__FILE__, fmt, ##__VA_ARGS__)

inline void println_channel(const char* channel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(channel, LogLevel::Info, msg);
}

inline void warnln_channel(const char* channel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(channel, LogLevel::Warn, msg);
}

inline void devprintln_channel(const char* channel, const char* fmt, ...) {
    if (!isDev) return;

    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(channel, LogLevel::Info, msg);
}

inline void devwarnln_channel(const char* channel, const char* fmt, ...) {
    if (!isDev) return;

    va_list args;
    va_start(args, fmt);
    std::string msg = vformat(fmt, args);
    va_end(args);

    log_message(channel, LogLevel::Warn, msg);
}

inline int exitp(int code) {
    close_log_file();
    return code;
}