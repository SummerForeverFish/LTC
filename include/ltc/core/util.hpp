// util.hpp - 日志与时间工具 (C++17, 无外部依赖)
//
// 职责：提供框架运行所需的最小基础设施——轻量日志器、单调/墙钟时间戳、日期字符串解析与
//       格式化、CSV 行拆分与字符串转数值。这些工具被网关、事件引擎、回测与数据加载共享。
//
// 关键设计：now_ns 用 steady_clock（单调、不受系统校时影响）做低延迟路径时延测量；
//           parse_datetime/format_datetime 用本地时区(tm)，与行情/成交流水的时间戳口径一致。
#pragma once
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <vector>

namespace ltc {
// ltc 命名空间：日志、时间、CSV 等通用工具函数的集合。

// 简单日志器：把 [级别] 消息打到 stdout。作为最小可用实现，后续可整体替换为 spdlog 等。
// 级别：INFO(正常) / WARNING(告警) / ERR(错误) / DEBUG(调试)，仅影响前缀标签。
struct Logger {
    enum class Level { INFO, WARNING, ERR, DEBUG };
    static void log(Level lv, const std::string& msg) {
        std::string tag = lv == Level::INFO ? "INFO" :
                          lv == Level::WARNING ? "WARN" :
                          lv == Level::ERR ? "ERROR" : "DEBUG";
        std::cout << "[" << tag << "] " << msg << std::endl;
    }
};

// 当前毫秒时间戳（wall-clock）：自 1970-01-01 起的毫秒数，用于事件 datetime 与日志时间。
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 当前纳秒时间戳（steady_clock，单调）：不受 NTP 校时回拨影响，专用于低延迟路径的时延测量与 benchmark。
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 解析日期字符串为毫秒时间戳（本地时区）。支持 "YYYY-MM-DD HH:MM:SS" 及带毫秒的
// "YYYY-MM-DD HH:MM:SS.fff"，'T' 与空格等价。缺字段时按 0 补齐，不抛异常。
inline int64_t parse_datetime(const std::string& s) {
    // 支持 "YYYY-MM-DD HH:MM:SS" 和带毫秒
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, ms = 0;
    std::string t = s;
    // 把 'T' 或空格统一
    for (char& c : t) if (c == 'T') c = ' ';
    // 拆分毫秒
    std::string frac;
    auto dot = t.find('.');
    if (dot != std::string::npos) { frac = t.substr(dot + 1); t = t.substr(0, dot); }
    std::istringstream iss(t);
    char dash1, dash2, space, colon1, colon2;
    iss >> y >> dash1 >> mo >> dash2 >> d >> space >> h >> colon1 >> mi >> colon2 >> se;
    if (dash1 != '-' || dash2 != '-') {
        // 可能只有 "YYYYMMDD HHMMSS"
    }
    if (!frac.empty()) ms = std::stoi(frac.substr(0, 3) + std::string(3 - (int)std::min(frac.size(), (size_t)3), '0'));
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    tm.tm_isdst = 0;
    auto tt = std::mktime(&tm); // 本地时间
    return (int64_t)tt * 1000 + ms;
}

// 毫秒时间戳 -> "YYYY-MM-DD HH:MM:SS"（本地时区，截断到秒；毫秒部分被丢弃）。用于日志与人类可读展示。
inline std::string format_datetime(int64_t ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 简易 CSV 行拆分：按逗号切分，双引号内逗号视为字段内容（in_q 切换状态），返回各字段。
// 不处理引号转义，足以解析常规行情/合约 CSV。
inline std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') { in_q = !in_q; }
        else if (c == ',' && !in_q) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// 字符串 -> double：空串或非法内容返回 0.0（捕获异常，绝不抛出），供 CSV 数值列安全转换。
inline double to_double(const std::string& s) {
    try { return s.empty() ? 0.0 : std::stod(s); } catch (...) { return 0.0; }
}

} // namespace ltc
