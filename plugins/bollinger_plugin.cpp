// bollinger_plugin.cpp - 布林带均值回归策略（外部插件，独立编译成 DLL）
//
// 与内置 Bollinger 策略逻辑一致，但作为「外部插件」分发：主程序无需重编，
// 只需在配置 [plugins] 填入本 DLL、[strategy] 填 type=BollingerPlugin 即可加载运行/回测。
//
// tick 生成 K 线模式：策略只实现 on_tick，把逐笔 tick 喂给 BarGenerator 聚合成 1 分钟 K 线，
// 收口时回调本策略 on_bar（布林带信号核心）。因此 tick 级回测(tick_backtest)与实盘(tick 行情)
// 都能直接复用；若运行于 bar 级回测(mode=backtest)，引擎直接喂 on_bar，同样生效。
//
// 编译（build.ps1 的插件段会自动构建本 DLL）。若手动编译，需先设置 MSVC 环境
// (PATH/INCLUDE/LIB，见 build.ps1 顶部)，完整命令为：
//   cl /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX
//      /LD /DLTC_PLUGIN_BUILD /I . /I include
//      /Fe:plugins/bollinger_plugin.dll /Fo:build\ plugins/bollinger_plugin.cpp

// LTC_PLUGIN_BUILD 必须在 include plugin_abi.h 之前定义，才会 dllexport 下列 C 函数。
// MSVC 因重复定义宏可能报 C4005，属无害重定义警告，可忽略。
#define LTC_PLUGIN_BUILD
#include "ltc/core/plugin_abi.h"

#include <deque>
#include <numeric>
#include <cmath>
#include <string>
#include <functional>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/strategy_registry.hpp"   // parse_params / param_int / param_double / param_bool
#include "ltc/core/util.hpp"
#include "ltc/core/bar_generator.hpp"        // BarGenerator：tick -> 1 分钟 K 线

using namespace ltc;

namespace {

// 布林带 Bar 策略：维护收盘价滑窗，计算布林带三轨，触轨做均值回归。
//   - 空仓且 close<下轨 -> 开多；空仓且 close>上轨 -> 开空
//   - 持多且 close>中轨 -> 平多；持空且 close<中轨 -> 平空
// 信号核心在 on_bar；on_tick 仅把 tick 喂给 BarGenerator 合成 1 分钟 K 线。
class BollingerStrategy : public BaseStrategy {
public:
    BollingerStrategy(const std::string& name, int window, double k, double vol, bool live)
        : BaseStrategy(name), window_(window), k_(k), vol_(vol), live_(live),
          // K 线合成器：tick -> 1 分钟 Bar，收口后回调本策略 on_bar（信号核心）
          bg_(std::bind(&BollingerStrategy::on_bar, this, std::placeholders::_1)) {}

    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " [插件] 布林带初始化 window=" +
                    std::to_string(window_) + " k=" + std::to_string(k_) +
                    " vol=" + std::to_string(vol_) + " live=" + std::to_string(live_));
    }

    // tick 回调：逐笔喂给 BarGenerator，跨分钟时其内部收口 1 分钟 Bar 并同步回调 on_bar。
    // 这是「tick 生成 K 线」模式的入口；bar 级回测/tick 直喂都走这条线（或引擎直接喂 on_bar）。
    void on_tick(const TickData& tk) override {
        bg_.update_tick(tk);
        //Logger::log(Logger::Level::INFO, name() + " [插件] 逐笔收口 tick=" );
    }

    // K线回调（信号核心）：由 BarGenerator 收口 1 分钟 Bar 后回调，或直接由 bar 级回测引擎调用。
    void on_bar(const BarData& bar) override {
        closes_.push_back(bar.close);
        if ((int)closes_.size() > window_ + 5) closes_.pop_front();
        if ((int)closes_.size() < window_) return;

        double mid = sma(window_);
        double sigma = stddev(window_, mid);
        double upper = mid + k_ * sigma;
        double lower = mid - k_ * sigma;

        if (pos_ == 0.0) {
            if (bar.close < lower) {
                if (live_) buy(bar.vt_symbol, bar.close, vol_, OrderType::MARKET);
                else log_dry("触下轨 BUY", bar.close);
            } else if (bar.close > upper) {
                if (live_) short_(bar.vt_symbol, bar.close, vol_, OrderType::MARKET);
                else log_dry("触上轨 SHORT", bar.close);
            }
        } else if (pos_ > 0.0 && bar.close > mid) {
            if (live_) sell(bar.vt_symbol, bar.close, vol_, OrderType::MARKET);
            else log_dry("回中轨 SELL", bar.close);
        } else if (pos_ < 0.0 && bar.close < mid) {
            if (live_) cover(bar.vt_symbol, bar.close, vol_, OrderType::MARKET);
            else log_dry("回中轨 COVER", bar.close);
        }
    }

    void on_trade(const TradeData& td) override {
        if (td.direction == Direction::LONG  && td.offset == Offset::OPEN)        pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN)    pos_ -= td.volume;
        else if (td.direction == Direction::LONG  && td.offset == Offset::CLOSE)   pos_ += td.volume;  // 平空：买回，净持仓回正
        else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE)   pos_ -= td.volume;  // 平多：卖出，净持仓回零
        Logger::log(Logger::Level::INFO, name() + " [插件] 成交 " +
                    direction_to_str(td.direction) + " @" + std::to_string(td.price) +
                    " vol=" + std::to_string(td.volume) + " pos=" + std::to_string(pos_));
    }

    void on_order(const OrderData& o) override {
        if (o.status == Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " [插件] 委托被拒: " + o.vt_orderid);
    }

private:
    // live=0 时只打 DRY 日志不下单；live=1 才真正报单。
    void log_dry(const std::string& sig, double price) {
        Logger::log(Logger::Level::INFO, name() + " [插件/DRY] " + sig + " @" + std::to_string(price));
    }

    double sma(int n) const {
        int cnt = std::min(n, (int)closes_.size());
        double sum = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return cnt ? sum / cnt : 0.0;
    }

    double stddev(int n, double mu) const {
        int cnt = std::min(n, (int)closes_.size());
        if (cnt <= 1) return 0.0;
        double acc = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) { double d = *it - mu; acc += d * d; }
        return std::sqrt(acc / cnt);
    }

    int window_;
    double k_, vol_;
    bool live_;
    std::deque<double> closes_;
    double pos_ = 0.0;
    BarGenerator bg_;   // vnpy 风格 K 线合成器：tick -> 1 分钟 Bar
};

const char* kType = "BollingerPlugin";
const char* kDesc = "布林带均值回归(外部插件,tick合成K线)  参数: window,k,vol,live";

} // namespace

extern "C" {

LTC_PLUGIN_EXPORT int ltc_plugin_api_version(void) { return LTC_PLUGIN_API_VERSION; }
LTC_PLUGIN_EXPORT int ltc_plugin_strategy_count(void) { return 1; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_type(int i) { return i == 0 ? kType : nullptr; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_desc(int i) { return i == 0 ? kDesc : nullptr; }

LTC_PLUGIN_EXPORT void* ltc_plugin_create(const char* type, const char* name, const char* params) {
    if (!type || std::string(type) != kType) return nullptr;
    StrategyParams p = parse_params(params ? std::string(params) : std::string());
    auto* s = new BollingerStrategy(name ? std::string(name) : std::string(kType),
                                    param_int(p, "window", 20),
                                    param_double(p, "k", 2.0),
                                    param_double(p, "vol", 1.0),
                                    param_bool(p, "live", false));
    return static_cast<void*>(s);
}

LTC_PLUGIN_EXPORT void ltc_plugin_destroy(void* obj) {
    delete static_cast<BaseStrategy*>(obj);
}

} // extern "C"
