// config.hpp - 轻量 ini 配置解析（支持 [section]、# 与 ; 注释、UTF-8 BOM）
//
// 职责：读取 vnpy 风格的 ini 配置（[section] 分组 + key=value），供主程序与策略取参数。
//
// 关键设计：
//   - 内部 data_ 为 section -> (key->value) 两层 map，SectionMap 与 StrategyParams 同构，
//     故某个 section 可直接整段当作策略参数传入 StrategyRegistry。
//   - 所有 get_* 在 key 缺失或解析失败时返回默认值，配置容错；自动跳过空行与 #/; 注释，
//     并剥离首行 UTF-8 BOM 以免 key 被污染。
//
// 与其他模块关系：
//   - 主程序 load 后，用 section()/get() 取出 [gateway]/[strategy] 配置；
//   - 策略参数 section 经 parse_params 转成 StrategyParams，再交给 StrategyRegistry::create。
#pragma once
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>

namespace ltc {

// 与 StrategyParams 同构，便于直接把某个 section 当作策略参数传入
using SectionMap = std::map<std::string, std::string>;

class IniConfig {
public:
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f) { last_error_ = "配置文件不存在: " + path; return false; }

        std::string line, cur;
        bool first = true;
        while (std::getline(f, line)) {
            if (first) { // 去掉 UTF-8 BOM
                if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                    (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                    line.erase(0, 3);
                first = false;
            }
            std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;

            if (t[0] == '[') { // [section]
                auto e = t.find(']');
                cur = (e == std::string::npos) ? trim(t.substr(1))
                                               : trim(t.substr(1, e - 1));
                data_[cur]; // 确保空 section 也存在
                continue;
            }

            auto p = t.find('=');
            if (p == std::string::npos) continue;
            std::string k = trim(t.substr(0, p));
            std::string v = trim(t.substr(p + 1));
            if (!k.empty()) data_[cur][k] = v;
        }
        last_error_.clear();
        return true;
    }

    bool has_section(const std::string& sec) const { return data_.count(sec) > 0; }

    // 取出整个 section（key=value 映射）；不存在返回空 map。可直接当 StrategyParams 用。
    SectionMap section(const std::string& sec) const {
        auto it = data_.find(sec);
        return (it == data_.end()) ? SectionMap{} : it->second;
    }

    // 读取某 section 下的 key；缺失 section/key 或值为空时返回 def（容错）。
    std::string get(const std::string& sec, const std::string& key,
                    const std::string& def = "") const {
        auto si = data_.find(sec);
        if (si == data_.end()) return def;
        auto ki = si->second.find(key);
        return (ki == si->second.end()) ? def : ki->second;
    }

    int get_int(const std::string& sec, const std::string& key, int def) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    }

    double get_double(const std::string& sec, const std::string& key, double def) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        try { return std::stod(v); } catch (...) { return def; }
    }

    bool get_bool(const std::string& sec, const std::string& key, bool def) const {
        std::string v = get(sec, key);
        if (v.empty()) return def;
        std::string lv = v;
        std::transform(lv.begin(), lv.end(), lv.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (lv == "1" || lv == "true" || lv == "yes" || lv == "on") return true;
        if (lv == "0" || lv == "false" || lv == "no" || lv == "off") return false;
        return def;
    }

    const std::string& last_error() const { return last_error_; }

private:
    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    std::map<std::string, SectionMap> data_;
    std::string last_error_;
};

} // namespace ltc
