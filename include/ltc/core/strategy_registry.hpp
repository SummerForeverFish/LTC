// strategy_registry.hpp - 策略工厂与注册表（策略插件化的核心）
//
// 职责：维护「类型名 -> 创建器」映射，是主程序创建策略的唯一入口。主程序不 #include 任何
//       具体策略类，只通过「类型名 + 实例名 + 参数」在运行时造出策略实例。
//
// 关键设计：
//   - 两条注册路径最终都汇入 create()：
//       1) 内置策略：独立编译单元用 LTC_REGISTER_STRATEGY 宏自注册（编译期带入 exe）
//       2) 外部插件：PluginLoader 加载 .dll 后，把插件导出的策略类型注册进来（运行时加载）
//   - 参数用 parse_params 从 "k=v,k=v" 串解析成 StrategyParams，再经 param_int/double/
//     bool/str 带默认值读取，保证缺参/解析失败也不崩。
//
// 与其他模块关系：
//   - 创建出的实例交给 MainEngine::add_strategy 接入实盘；
//   - PluginLoader 把插件侧策略类型 register_strategy 进来，与内置策略无差别。
#pragma once
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <sstream>
#include <cctype>
#include <algorithm>

#include "ltc/core/strategy.hpp"

namespace ltc {

// 策略参数：来自配置文件 params 串解析出的 key=value 集合
using StrategyParams = std::map<std::string, std::string>;

// 策略创建器：给定实例名与参数，产出一个策略实例
using StrategyCreator = std::function<std::shared_ptr<BaseStrategy>(
    const std::string& name, const StrategyParams& params)>;

inline std::string trim_str(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// 解析 "fast=10,slow=30,vol=1.0" 形式的参数串
inline StrategyParams parse_params(const std::string& s) {
    StrategyParams out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto p = item.find('=');
        if (p == std::string::npos) continue;
        std::string k = trim_str(item.substr(0, p));
        std::string v = trim_str(item.substr(p + 1));
        if (!k.empty()) out[k] = v;
    }
    return out;
}

// ---- 参数读取辅助（缺 key 或解析失败时返回默认值，保证配置容错）----
inline int param_int(const StrategyParams& p, const std::string& k, int def) {
    auto it = p.find(k);
    if (it == p.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

inline double param_double(const StrategyParams& p, const std::string& k, double def) {
    auto it = p.find(k);
    if (it == p.end()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}

inline bool param_bool(const StrategyParams& p, const std::string& k, bool def) {
    auto it = p.find(k);
    if (it == p.end()) return def;
    std::string v = to_lower_str(trim_str(it->second));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

inline std::string param_str(const StrategyParams& p, const std::string& k,
                             const std::string& def) {
    auto it = p.find(k);
    return it == p.end() ? def : it->second;
}

// 策略注册表（进程内单例）。主程序只依赖本类，不依赖任何具体策略类型。
class StrategyRegistry {
public:
    // 进程内单例（Meyers 单例）：首次调用时构造，C++11 起保证线程安全。
    static StrategyRegistry& instance() {
        static StrategyRegistry r;
        return r;
    }

    // 注册一个策略类型：type 为类型名，creator 为工厂，desc 描述，source 标记来源
    // （builtin 或 plugin:xxx.dll）。加锁保证多线程加载插件安全。
    void register_strategy(const std::string& type, StrategyCreator creator,
                           const std::string& desc = "",
                           const std::string& source = "builtin") {
        std::lock_guard<std::mutex> lk(mtx_);
        creators_[type] = std::move(creator);
        info_[type] = std::make_pair(desc, source);
    }

    bool has(const std::string& type) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return creators_.count(type) > 0;
    }

    // 按类型名创建策略；类型不存在时返回 nullptr（由调用方报错）
    std::shared_ptr<BaseStrategy> create(const std::string& type, const std::string& name,
                                         const StrategyParams& params) const {
        StrategyCreator c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = creators_.find(type);
            if (it == creators_.end()) return nullptr;
            c = it->second;
        }
        return c(name, params);
    }

    std::vector<std::string> list() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::string> out;
        for (const auto& kv : creators_) out.push_back(kv.first);
        return out;
    }

    std::string describe(const std::string& type) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = info_.find(type);
        if (it == info_.end()) return "";
        return type + "  " + it->second.first + "  [" + it->second.second + "]";
    }

    // 全部策略清单（每行一条），便于启动时打印可选策略
    std::string dump() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string out;
        for (const auto& kv : creators_) {
            auto it = info_.find(kv.first);
            std::string desc = (it == info_.end()) ? "" : it->second.first;
            std::string src  = (it == info_.end()) ? "builtin" : it->second.second;
            out += "  " + kv.first + "  " + desc + "  [" + src + "]\n";
        }
        return out;
    }

private:
    mutable std::mutex mtx_;
    std::map<std::string, StrategyCreator> creators_;
    // type -> (描述, 来源：builtin 或 plugin:xxx.dll)
    std::map<std::string, std::pair<std::string, std::string>> info_;
};

// 静态自注册器：利用全局对象构造完成的副作用完成注册
struct StrategyRegistrar {
    StrategyRegistrar(const std::string& type, StrategyCreator creator,
                      const std::string& desc = "", const std::string& source = "builtin") {
        StrategyRegistry::instance().register_strategy(type, std::move(creator), desc, source);
    }
};

} // namespace ltc

#define LTC_CAT_IMPL(a, b) a##b
#define LTC_CAT(a, b) LTC_CAT_IMPL(a, b)

// 注册一个策略类型。用法：
//   LTC_REGISTER_STRATEGY("DoubleMA", "双均线穿越 fast/slow/vol",
//       [](const std::string& n, const ::ltc::StrategyParams& p)
//           -> std::shared_ptr<::ltc::BaseStrategy> {
//           return std::make_shared<DoubleMaStrategy>(n, param_int(p,"fast",10), ...);
//       });
#define LTC_REGISTER_STRATEGY(type, desc, creator) \
    static ::ltc::StrategyRegistrar LTC_CAT(ltc_reg_, __COUNTER__)((type), (creator), (desc))
