// register_builtin.cpp - 内置策略注册单元
//
// 这是「唯一」把内置策略带入可执行文件的编译单元。main.cpp 不再 #include 任何策略头文件，
// 只通过 StrategyRegistry 按「类型名 + 参数」创建实例，因此新增/替换策略无需改动主程序。
//
// 想让主程序完全不含任何策略（纯插件模式）时，构建不链接本文件即可，
// 此时只能由 [plugins] 配置的外部 DLL 提供策略类型。

#include "ltc/core/strategy_registry.hpp"

#include "strategies/double_ma_strategy.hpp"
#include "strategies/ctp_demo_strategy.hpp"
#include "strategies/tick_demo_strategy.hpp"
#include "strategies/bollinger_strategy.hpp"

namespace ltc {
namespace {

// DoubleMA：双均线穿越策略（bar 驱动）。参数 fast/slow 为均线周期，vol 为固定手数。
LTC_REGISTER_STRATEGY(
    "DoubleMA", "双均线穿越(bar驱动)  参数: fast,slow,vol",
    [](const std::string& n, const StrategyParams& p) -> std::shared_ptr<BaseStrategy> {
        return std::make_shared<DoubleMaStrategy>(
            n, param_int(p, "fast", 10), param_int(p, "slow", 30),
            param_double(p, "vol", 1.0));
    });

// TickDemo：tick 直喂快慢均线策略。live 控制是否真实报单，fast/slow 默认 50/200，vol 固定手数。
LTC_REGISTER_STRATEGY(
    "TickDemo", "tick直喂快慢均线  参数: live,fast,slow,vol",
    [](const std::string& n, const StrategyParams& p) -> std::shared_ptr<BaseStrategy> {
        return std::make_shared<TickDemoStrategy>(
            n, param_bool(p, "live", false), param_int(p, "fast", 50),
            param_int(p, "slow", 200), param_double(p, "vol", 1.0));
    });

// CtpDemo：CTP tick 聚合分钟线双均线策略。live 即 dry-run 闸门，决定是否真正以限价单报单。
LTC_REGISTER_STRATEGY(
    "CtpDemo", "CTP tick聚合分钟线双均线  参数: live,fast,slow,vol",
    [](const std::string& n, const StrategyParams& p) -> std::shared_ptr<BaseStrategy> {
        return std::make_shared<CtpDemoStrategy>(
            n, param_bool(p, "live", false), param_int(p, "fast", 10),
            param_int(p, "slow", 30), param_double(p, "vol", 1.0));
    });

// Bollinger：布林带均值回归策略（bar 驱动）。参数 window 布林窗口、k 标准差倍数、vol 固定手数。
LTC_REGISTER_STRATEGY(
    "Bollinger", "布林带均值回归(bar驱动)  参数: window,k,vol",
    [](const std::string& n, const StrategyParams& p) -> std::shared_ptr<BaseStrategy> {
        return std::make_shared<BollingerStrategy>(
            n, param_int(p, "window", 20), param_double(p, "k", 2.0),
            param_double(p, "vol", 1.0));
    });

} // namespace
} // namespace ltc
