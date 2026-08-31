// demo_plugin.cpp - 外部策略插件示例（独立编译成 DLL，由主程序在运行时加载）
//
// 演示第三方如何在「不改动主程序、也不重编主程序」的前提下扩展策略，只需三步：
//   1. 写一个继承 ltc::BaseStrategy 的类
//   2. 按 ltc/core/plugin_abi.h 的 C 接口把它导出
//   3. 在配置里 [plugins] 填 dll 路径、[strategy] 填 type 即可
//
// 编译（见 build.ps1）：
//   cl /std:c++17 /utf-8 /EHsc /O2 /LD /DLTC_PLUGIN_BUILD /I include plugins/demo_plugin.cpp

// LTC_PLUGIN_BUILD 必须在 include plugin_abi.h 之前定义：它控制 plugin_abi.h 中的
// LTC_PLUGIN_EXPORT 展开为 __declspec(dllexport)，从而把这些 C 函数导出供宿主 LoadLibrary。
// 注：plugin_abi.h 顶部可能已定义过 LTC_PLUGIN_API_VERSION，这里再 #define LTC_PLUGIN_BUILD
// 不会冲突；MSVC 因重复定义该宏可能报 C4005，属无害重定义警告，可忽略。
#define LTC_PLUGIN_BUILD   // 必须在 include plugin_abi.h 之前定义，才会 dllexport
#include "ltc/core/plugin_abi.h"

#include <deque>
#include <string>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/strategy_registry.hpp"   // parse_params / param_int / param_double / param_bool
#include "ltc/core/util.hpp"

using namespace ltc;

namespace {

// tick 动量策略：窗口内涨幅超过阈值买入，跌破负阈值卖出。
// 同时实现 on_tick 与 on_bar，因此 tick 级回测与 bar 级回测都能直接复用。
class MomentumTickStrategy : public BaseStrategy {
public:
    MomentumTickStrategy(const std::string& name, int window, double threshold,
                         double vol, bool live)
        : BaseStrategy(name), window_(window), threshold_(threshold),
          vol_(vol), live_(live) {}

    void on_init() override {
        Logger::log(Logger::Level::INFO,
                    name() + " [插件] 动量策略初始化 window=" + std::to_string(window_) +
                    " threshold=" + std::to_string(threshold_) +
                    " vol=" + std::to_string(vol_) + " live=" + std::to_string(live_));
    }

    void on_tick(const TickData& tk) override { feed(tk.last_price, tk.vt_symbol); }
    void on_bar(const BarData& bar) override  { feed(bar.close, bar.vt_symbol); }

    void on_trade(const TradeData& td) override {
        // 根据成交的方向/开平更新本地持仓估计（仅用于日志展示，非权威仓位）
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN)         pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN)   pos_ -= td.volume;
        else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE)   pos_ -= td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE)  pos_ -= td.volume;
        Logger::log(Logger::Level::INFO, name() + " [插件] 成交 " +
                    direction_to_str(td.direction) + " @" + std::to_string(td.price) +
                    " pos=" + std::to_string(pos_));
    }

private:
    // 统一的价格入口：不论 on_tick 还是 on_bar，都把最新价喂进来，策略逻辑只写一份
    void feed(double price, const std::string& vt) {
        if (price <= 0.0) return;
        hist_.push_back(price);                       // 滑动窗口：保留最近 window_ 个价格
        if ((int)hist_.size() > window_) hist_.pop_front();
        if ((int)hist_.size() < window_) return;      // 窗口未满，先积累样本

        double first = hist_.front();
        double mom = (first > 0.0) ? (price - first) / first : 0.0;   // 窗口内累计涨幅

        // 动量信号：空仓且涨幅超阈值 -> 做多；持多且跌幅超阈值 -> 平多
        if (pos_ == 0.0 && mom > threshold_) {
            if (live_) buy(vt, price, vol_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [插件/DRY] 动量转正 BUY @" +
                             std::to_string(price) + " mom=" + std::to_string(mom));
        } else if (pos_ > 0.0 && mom < -threshold_) {
            if (live_) sell(vt, price, vol_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [插件/DRY] 动量转负 SELL @" +
                             std::to_string(price) + " mom=" + std::to_string(mom));
        }
    }

    int window_;
    double threshold_, vol_;
    bool live_;
    std::deque<double> hist_;
    double pos_ = 0.0;
};

const char* kType = "MomentumTick";
const char* kDesc = "tick动量策略(外部插件示例)  参数: window,threshold,vol,live";

} // namespace

// ---------------------------------------------------------------- C ABI 导出

// ---------------------------------------------------------------- C ABI 导出
// 下列函数按 plugin_abi.h 约定的纯 C 接口导出；宿主用 GetProcAddress 取到后注册/创建策略。
// 纯 C 接口 + 裸指针避免跨 DLL 的 C++ 名称修饰/异常处理/CRT 不兼容问题。
extern "C" {

// 返回 ABI 版本号；宿主加载时会校验与自身 LTC_PLUGIN_API_VERSION 是否一致
LTC_PLUGIN_EXPORT int ltc_plugin_api_version(void) { return LTC_PLUGIN_API_VERSION; }

// 本插件导出的策略类型个数（此处仅 MomentumTick 一种）
LTC_PLUGIN_EXPORT int ltc_plugin_strategy_count(void) { return 1; }

// 第 index 个策略的类型名（宿主据此在 StrategyRegistry 中注册）
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_type(int index) {
    return index == 0 ? kType : nullptr;
}

// 第 index 个策略的描述（list/dump 时展示）
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_desc(int index) {
    return index == 0 ? kDesc : nullptr;
}

// 按类型名创建策略实例：仅接受本插件认识的 kType；解析 params 串为参数后 new 出对象，
// 返回裸指针。必须由配对调用 ltc_plugin_destroy 释放（保证在 DLL 侧析构）。
LTC_PLUGIN_EXPORT void* ltc_plugin_create(const char* type, const char* name,
                                              const char* params) {
    if (!type || std::string(type) != kType) return nullptr;

    StrategyParams p = parse_params(params ? std::string(params) : std::string());
    auto* s = new MomentumTickStrategy(name ? std::string(name) : std::string(kType),
                                       param_int(p, "window", 30),
                                       param_double(p, "threshold", 0.0002),
                                       param_double(p, "vol", 1.0),
                                       param_bool(p, "live", false));
    return static_cast<void*>(s);
}

// 销毁由 ltc_plugin_create 返回的对象。宿主会把本函数指针包进 shared_ptr 的自定义 deleter，
// 因此无论策略实例在何处析构，最终都回到 DLL 侧执行 delete，避免跨模块释放错配。
LTC_PLUGIN_EXPORT void ltc_plugin_destroy(void* obj) {
    delete static_cast<BaseStrategy*>(obj);
}

} // extern "C"
