// my_ma_plugin.cpp - 最小可运行策略模板（外部插件）
//
// 演示「如何写一个 C++ 策略并测试」：
//   1. 继承 ltc::BaseStrategy，实现 on_tick / on_bar / on_trade 等回调
//   2. 用 LTC_PLUGIN_* C 接口导出（主程序无需重新编译）
//   3. 在 config 里 [plugins] + [strategy] 指定，即可运行/回测
//
// 编译：
//   cl /std:c++17 /utf-8 /EHsc /O2 /LD /DLTC_PLUGIN_BUILD /I include plugins/my_ma_plugin.cpp /Fe:plugins/my_ma_plugin.dll

// LTC_PLUGIN_BUILD 必须在 include plugin_abi.h 之前定义，使 LTC_PLUGIN_EXPORT 展开为
// __declspec(dllexport) 导出下列 C 函数。MSVC 可能因重复定义宏报 C4005，属无害警告。
#define LTC_PLUGIN_BUILD
#include "ltc/core/plugin_abi.h"

#include <deque>
#include <string>
#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/strategy_registry.hpp"
#include "ltc/core/util.hpp"

using namespace ltc;

namespace {

// 双均线 tick 策略：快线上穿慢线开多，下穿平多
class MaCrossStrategy : public BaseStrategy {
public:
    MaCrossStrategy(const std::string& name, int fast, int slow, double vol, bool live)
        : BaseStrategy(name), fast_(fast), slow_(slow), vol_(vol), live_(live) {}

    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " [插件] 双均线初始化 fast=" +
                    std::to_string(fast_) + " slow=" + std::to_string(slow_) +
                    " vol=" + std::to_string(vol_) + " live=" + std::to_string(live_));
    }

    void on_tick(const TickData& tk) override { feed(tk.last_price, tk.vt_symbol); }
    void on_bar(const BarData& bar) override  { feed(bar.close, bar.vt_symbol); }

    void on_trade(const TradeData& td) override {
        // 依据成交流水更新本地持仓估计（仅日志用，非权威仓位）
        if (td.direction == Direction::LONG   && td.offset == Offset::OPEN)  pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN)  pos_ -= td.volume;
        else if (td.direction == Direction::LONG   && td.offset == Offset::CLOSE) pos_ -= td.volume;
        else if (td.direction == Direction::SHORT  && td.offset == Offset::CLOSE) pos_ -= td.volume;
        Logger::log(Logger::Level::INFO, name() + " [插件] 成交 " +
                    direction_to_str(td.direction) + " @" + std::to_string(td.price) +
                    " vol=" + std::to_string(td.volume) + " pos=" + std::to_string(pos_));
    }

    void on_order(const OrderData& o) override {
        // 仅关注被拒委托，便于在日志中快速发现下单失败（如超仓/价格越界）
        if (o.status == Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " [插件] 委托被拒: " + o.vt_orderid);
    }

private:
    void feed(double price, const std::string& vt) {
        if (price <= 0.0) return;
        buf_.push_back(price);                          // 维护价格环形缓冲，最多保留 slow_+8
        if ((int)buf_.size() > slow_ + 8) buf_.pop_front();
        if ((int)buf_.size() < slow_) return;           // 样本不足慢线窗口，先积累

        double fast_ma = ma(fast_);
        double slow_ma = ma(slow_);

        // 双均线信号：快线由下穿上穿慢线（金叉）开多；持多时快线下穿慢线（死叉）平多
        if (pos_ == 0.0 && fast_ma > slow_ma) {
            if (live_) buy(vt, price, vol_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [DRY] 金叉 BUY @" +
                             std::to_string(price));
        } else if (pos_ > 0.0 && fast_ma < slow_ma) {
            if (live_) sell(vt, price, vol_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [DRY] 死叉 SELL @" +
                             std::to_string(price));
        }
    }

    // 取最近 n 个价格的算术平均（n 超过缓冲长度时按实际长度计算）
    double ma(int n) const {
        int cnt = std::min(n, (int)buf_.size());
        double sum = 0;
        auto it = buf_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return cnt ? sum / cnt : 0.0;
    }

    int fast_, slow_;
    double vol_;
    bool live_;
    std::deque<double> buf_;
    double pos_ = 0.0;
};

const char* kType = "MACross";
const char* kDesc = "双均线tick策略(最小模板)  参数: fast,slow,vol,live";

} // namespace

extern "C" {

// 以下为 plugin_abi.h 约定的纯 C 导出函数；宿主按名称 GetProcAddress 后注册/创建策略。
// 纯 C 接口 + 裸指针规避跨 DLL 的 C++ ABI/异常/CRT 不兼容问题。
LTC_PLUGIN_EXPORT int ltc_plugin_api_version(void) { return LTC_PLUGIN_API_VERSION; }
LTC_PLUGIN_EXPORT int ltc_plugin_strategy_count(void) { return 1; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_type(int i) { return i == 0 ? kType : nullptr; }
LTC_PLUGIN_EXPORT const char* ltc_plugin_strategy_desc(int i) { return i == 0 ? kDesc : nullptr; }

// 按类型名创建策略实例：仅接受本插件类型 kType；解析 params 后 new 出对象并返回裸指针。
LTC_PLUGIN_EXPORT void* ltc_plugin_create(const char* type, const char* name, const char* params) {
    if (!type || std::string(type) != kType) return nullptr;
    StrategyParams p = parse_params(params ? std::string(params) : std::string());
    auto* s = new MaCrossStrategy(name ? std::string(name) : std::string(kType),
                                  param_int(p, "fast", 30),
                                  param_int(p, "slow", 120),
                                  param_double(p, "vol", 1.0),
                                  param_bool(p, "live", false));
    return static_cast<void*>(s);
}

// 销毁由 ltc_plugin_create 返回的对象。宿主把本函数指针包进 shared_ptr 的自定义 deleter，
// 使策略实例无论在何处析构都回到 DLL 侧执行 delete，避免跨模块释放错配。
LTC_PLUGIN_EXPORT void ltc_plugin_destroy(void* obj) {
    delete static_cast<BaseStrategy*>(obj);
}

} // extern "C"
