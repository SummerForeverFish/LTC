// position_store.hpp - 策略持仓持久化（JSON Lines）
//
// 职责：把每个策略的「开仓均价 + 持仓量(净)」写入同一个 JSON 文件，
//       以【策略名】为一级 key 做命名空间，多策略互不覆盖。
//       这是【当前策略自身】的持仓（只统计本策略成交），不是账户全部持仓。
//       落盘时重新读取最新文件、只改本策略那一份、再原子写回，避免多策略/多线程互相覆盖。
//
// 说明：手写极简 JSON 读写（无第三方依赖）。文件为 JSON Lines 格式，每行一条记录：
//   {"name":"策略名","vt":"rb2610.SHFE","volume":1.0,"avg_price":3119.0}
#pragma once
#include <string>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace ltc {

// 持仓快照：净持仓(正负表多/空) + 开仓均价
struct PositionInfo {
    double volume = 0.0;
    double avg_price = 0.0;
};

class PositionStore {
public:
    explicit PositionStore(const std::string& strategy_name,
                           const std::string& path = "strategy_position.json")
        : name_(strategy_name), path_(path) {
        load();
    }

    // 读取本策略某合约持仓（未记录则返回 {0,0}）
    PositionInfo get(const std::string& vt_symbol) const {
        auto it = data_.find(name_);
        if (it == data_.end()) return PositionInfo{};
        auto jt = it->second.find(vt_symbol);
        if (jt == it->second.end()) return PositionInfo{};
        return jt->second;
    }

    // 设置本策略某合约持仓并落盘（重读最新文件、只改本策略一份、原子写回）
    // persist_=false 时只更新内存（供回测等场景使用，避免把模拟持仓写进实盘账本）。
    void set(const std::string& vt_symbol, double volume, double avg_price) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto all = read_all();
        all[name_][vt_symbol] = PositionInfo{volume, avg_price};
        data_ = all;
        if (persist_) write_all(all);
    }

    // 是否落盘到 JSON 文件：回测引擎置 false，仅维护内存账本。
    void set_persist(bool b) { persist_ = b; }

private:
    // 扫描提取 "key": 后的字符串值
    static std::string find_str(const std::string& s, const std::string& key) {
        std::string tok = "\"" + key + "\":";
        auto p = s.find(tok);
        if (p == std::string::npos) return "";
        p += tok.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= s.size() || s[p] != '"') return "";
        p++;
        auto q = s.find('"', p);
        if (q == std::string::npos) return "";
        return s.substr(p, q - p);
    }
    // 扫描提取 "key": 后的数字（double）
    static double find_num(const std::string& s, const std::string& key) {
        std::string tok = "\"" + key + "\":";
        auto p = s.find(tok);
        if (p == std::string::npos) return 0.0;
        p += tok.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        try {
            size_t idx = 0;
            double v = std::stod(s.substr(p), &idx);
            return v;
        } catch (...) { return 0.0; }
    }

    void load() { data_ = read_all(); }

    // 读取全量：{策略名: {vt_symbol: PositionInfo}}
    std::map<std::string, std::map<std::string, PositionInfo>> read_all() const {
        std::map<std::string, std::map<std::string, PositionInfo>> all;
        std::ifstream f(path_);
        if (!f) return all;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::string nm = find_str(line, "name");
            std::string vt = find_str(line, "vt");
            if (nm.empty() || vt.empty()) continue;
            double vol = find_num(line, "volume");
            double ap  = find_num(line, "avg_price");
            all[nm][vt] = PositionInfo{vol, ap};
        }
        return all;
    }

    // 原子写回：先写临时文件再 rename 替换
    void write_all(const std::map<std::string, std::map<std::string, PositionInfo>>& all) const {
        std::string tmp = path_ + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return;
            for (const auto& kv : all) {
                for (const auto& kv2 : kv.second) {
                    f << "{\"name\":\"" << kv.first
                      << "\",\"vt\":\"" << kv2.first
                      << "\",\"volume\":" << kv2.second.volume
                      << ",\"avg_price\":" << kv2.second.avg_price
                      << "}\n";
                }
            }
        }
        // 注意：Windows 上 std::rename 在目标已存在时会失败（不替换），
        // 必须用 std::filesystem::rename（可原子覆盖已存在目标）。
        std::error_code ec;
        std::filesystem::rename(tmp.c_str(), path_.c_str(), ec);
    }

    bool persist_ = true;
    std::string name_;
    std::string path_;
    std::map<std::string, std::map<std::string, PositionInfo>> data_;
    mutable std::mutex mtx_;
};

} // namespace ltc
