// plugin_loader.hpp - 运行时动态加载外部策略插件（Windows DLL / Linux so）
//
// 职责：在运行时把外部插件（.dll/.so）里的策略类型注册进 StrategyRegistry，使主程序
//       用「类型名」创建策略时与内置策略完全无差别，从而实现策略热插拔、零重编。
//
// 关键设计：
//   - 插件按 plugin_abi.h 的纯 C 接口导出 ltc_plugin_* 系列函数（api_version/count/type/
//     desc/create/destroy）；宿主 GetProcAddress 取函数指针后调用。
//   - 关键约束：策略对象由插件 new、也必须由插件 delete。这里把 destroy 函数指针包进
//     shared_ptr 的自定义 deleter，因此宿主持有的 shared_ptr 析构时会回到插件侧释放，
//     保证「谁分配谁释放」，跨 CRT 也安全。
//   - 插件 DLL 在 PluginLoader 生命周期内保持加载；unload_all() 必须在由插件创建的
//     策略实例全部释放后再调用，否则析构会跳进已卸载的代码段而崩溃。
//
// 与其他模块关系：
//   - 依赖 plugin_abi.h（C ABI）、strategy_registry.hpp（注册）、util.hpp（join 等）。
//   - 加载后策略经 StrategyRegistry::create 产出，再交给 MainEngine::add_strategy。
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>

#include "ltc/core/strategy.hpp"
#include "ltc/core/strategy_registry.hpp"
#include "ltc/core/util.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX            /* 避免 windows.h 的 min/max 宏污染 std::min */
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include "ltc/core/plugin_abi.h"

namespace ltc {

// 插件加载器：进程内单例。负责打开动态库、校验 ABI 版本、枚举并注册策略类型、持有库句柄。
class PluginLoader {
public:
#ifdef _WIN32
    using Handle = HMODULE;
#else
    using Handle = void*;
#endif

    static PluginLoader& instance() {
        static PluginLoader p;
        return p;
    }

    // 加载插件并注册其导出的全部策略类型；失败时通过 err 返回原因
    bool load(const std::string& path, std::string* err = nullptr) {
        Handle h = open(path);
        if (!h) {
            if (err) *err = open_error(path);
            return false;
        }

        auto f_ver = (ltc_api_version_fn)sym(h, "ltc_plugin_api_version");
        auto f_cnt = (ltc_count_fn)    sym(h, "ltc_plugin_strategy_count");
        auto f_typ = (ltc_type_fn)     sym(h, "ltc_plugin_strategy_type");
        auto f_des = (ltc_desc_fn)     sym(h, "ltc_plugin_strategy_desc");
        auto f_new = (ltc_create_fn)   sym(h, "ltc_plugin_create");
        auto f_del = (ltc_destroy_fn)  sym(h, "ltc_plugin_destroy");

        if (!f_ver || !f_cnt || !f_typ || !f_new || !f_del) {
            if (err) *err = "插件缺少必需的导出函数(ltc_plugin_*): " + path;
            close(h);
            return false;
        }

        int ver = f_ver();
        if (ver != LTC_PLUGIN_API_VERSION) {
            if (err) *err = "插件 ABI 版本不匹配: " + path +
                            " (插件=" + std::to_string(ver) +
                            ", 宿主=" + std::to_string(LTC_PLUGIN_API_VERSION) + ")";
            close(h);
            return false;
        }

        std::vector<std::string> types;
        int n = f_cnt();
        for (int i = 0; i < n; ++i) {
            const char* t = f_typ(i);
            if (!t || !*t) continue;
            std::string type(t);
            const char* d = f_des ? f_des(i) : nullptr;
            std::string desc = d ? std::string(d) : (type + " (插件策略)");

            StrategyRegistry::instance().register_strategy(
                type,
                [f_new, f_del, type](const std::string& name, const StrategyParams& params)
                    -> std::shared_ptr<BaseStrategy> {
                    // 把参数 map 还原成 "k=v,k=v" 串传给插件（C 接口只认字符串）
                    std::string ps;
                    for (const auto& kv : params) {
                        if (!ps.empty()) ps += ",";
                        ps += kv.first + "=" + kv.second;
                    }
                    void* obj = f_new(type.c_str(), name.c_str(), ps.c_str());
                    if (!obj) return nullptr;
                    auto* s = static_cast<BaseStrategy*>(obj);
                    // 谁分配谁释放：析构时回调插件侧的 destroy
                    return std::shared_ptr<BaseStrategy>(s, [f_del](BaseStrategy* p) {
                        f_del(static_cast<void*>(p));
                    });
                },
                desc,
                "plugin:" + path);

            types.push_back(type);
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            handles_.push_back({path, h});
        }
        Logger::log(Logger::Level::INFO,
                    "已加载策略插件: " + path + "  提供类型[" + join(types) + "]");
        return true;
    }

    // 卸载全部插件（务必先确保由插件创建的策略实例都已释放）
    void unload_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& e : handles_) close(e.handle);
        handles_.clear();
    }

    std::vector<std::string> loaded() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::string> out;
        for (const auto& e : handles_) out.push_back(e.path);
        return out;
    }

private:
    PluginLoader() = default;
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    static Handle open(const std::string& path) {
#ifdef _WIN32
        return ::LoadLibraryA(path.c_str());
#else
        return ::dlopen(path.c_str(), RTLD_NOW);
#endif
    }

    static void close(Handle h) {
        if (!h) return;
#ifdef _WIN32
        ::FreeLibrary(h);
#else
        ::dlclose(h);
#endif
    }

    static void* sym(Handle h, const char* name) {
#ifdef _WIN32
        return reinterpret_cast<void*>(::GetProcAddress(h, name));
#else
        return ::dlsym(h, name);
#endif
    }

    static std::string open_error(const std::string& path) {
#ifdef _WIN32
        return "无法加载插件: " + path + " (GetLastError=" +
               std::to_string(::GetLastError()) + ")";
#else
        const char* e = ::dlerror();
        return "无法加载插件: " + path + (e ? (std::string(" - ") + e) : std::string());
#endif
    }

    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) {
            if (!s.empty()) s += ", ";
            s += x;
        }
        return s;
    }

    struct Entry {
        std::string path;
        Handle handle;
    };

    mutable std::mutex mtx_;
    std::vector<Entry> handles_;
};

} // namespace ltc
