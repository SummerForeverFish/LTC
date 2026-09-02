# 编写策略

策略需继承 `BaseStrategy`，重写生命周期与事件回调，用 `buy/sell/short_/cover/cancel` 发单/撤单。策略**不感知实盘还是回测**——下单通过注入的 `OrderRouter` 路由。

三种来源，写法完全一致（只是注册方式不同）：

| 来源 | 注册方式 | 主程序是否要重编 |
| --- | --- | --- |
| C++ 内置 | `LTC_REGISTER_STRATEGY` 自注册（编译期带入 exe） | 需把该 .cpp 一起链接 |
| C++ 插件 DLL | 导出 `ltc_plugin_*` C 接口，运行时 `LoadLibrary` 注册 | **不需要** |
| Python | `class MyStrat(v.Strategy)` 继承，走 nanobind trampoline | 不需要 |

## 1. C++ 内置策略（bar 驱动示例）

```cpp
// my_strategy.hpp
#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"

class MyMaStrategy : public ltc::BaseStrategy {
public:
    MyMaStrategy(const std::string& name, int fast = 10, int slow = 30, double vol = 1.0)
        : BaseStrategy(name), fast_(fast), slow_(slow), vol_(vol) {}

    void on_bar(const ltc::BarData& bar) override {
        closes_.push_back(bar.close);
        if ((int)closes_.size() > slow_ + 5) closes_.pop_front();
        if ((int)closes_.size() < slow_) return;
        double fast_ma = ma(fast_), slow_ma = ma(slow_);
        if (pos_ == 0 && fast_ma > slow_ma)
            buy(bar.vt_symbol, bar.close, vol_, ltc::OrderType::MARKET);   // 金叉开多
        else if (pos_ > 0 && fast_ma < slow_ma)
            sell(bar.vt_symbol, bar.close, vol_, ltc::OrderType::MARKET);  // 死叉平多
    }
    void on_trade(const ltc::TradeData& td) override {
        // 策略自行维护持仓（vnpy 风格）：按 direction/offset 更新 pos_
        if (td.direction == ltc::Direction::LONG && td.offset == ltc::Offset::OPEN) pos_ += td.volume;
        else if (td.direction == ltc::Direction::SHORT && td.offset == ltc::Offset::OPEN) pos_ -= td.volume;
        else if (td.direction == ltc::Direction::LONG && td.offset == ltc::Offset::CLOSE) pos_ -= td.volume;
        else if (td.direction == ltc::Direction::SHORT && td.offset == ltc::Offset::CLOSE) pos_ -= td.volume;
    }
    void on_order(const ltc::OrderData& o) override {
        if (o.status == ltc::Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " 委托被拒: " + o.vt_orderid);
    }
private:
    double ma(int n) const { /* 末尾 n 个收盘价简单平均 */ }
    int fast_, slow_; double vol_; std::deque<double> closes_; double pos_ = 0.0;
};
```

在某编译单元用宏注册（无需 `main` 参与，全局对象构造时自注册）：

```cpp
#include "ltc/core/strategy_registry.hpp"
LTC_REGISTER_STRATEGY("MyMA", "双均线 fast/slow/vol",
    [](const std::string& n, const ltc::StrategyParams& p)
        -> std::shared_ptr<ltc::BaseStrategy> {
        return std::make_shared<MyMaStrategy>(n,
            param_int(p, "fast", 10),
            param_int(p, "slow", 30),
            param_double(p, "vol", 1.0));
    });
```

参数从 ini 的 `params = fast=10,slow=30,vol=1.0` 解析成 `StrategyParams`（`parse_params`），用 `param_int` / `param_double` / `param_bool` / `param_str` 带默认值读取，缺参/解析失败也不崩。

内置策略最终由 `strategies/register_builtin.cpp` 链接进 exe（见 `main.cpp` 不再 include 任何策略）。

## 2. Python 策略

```python
import ltc as v

class MyStrat(v.Strategy):
    def __init__(self, name):
        super().__init__(name)
        # BarGenerator：tick -> 1 分钟 K -> 5 分钟 K
        self.bg = v.BarGenerator(self.on_bar_1m, 5, self.on_bar_5m, v.Interval.MINUTE)

    def on_tick(self, tick):
        self.bg.update_tick(tick)        # 喂 tick

    def on_bar_1m(self, bar):
        self.bg.update_bar(bar)          # K 线接力

    def on_bar_5m(self, bar):
        # 策略逻辑：用 bar.close 算指标后下单
        self.buy(bar.vt_symbol, bar.close, 1.0, v.OrderType.LIMIT)

    def on_trade(self, td):
        print("trade", td.vt_symbol, td.price, td.volume)

# 回测
eng = v.BacktestEngine()
eng.load_csv("data/BTCUSDT.csv", "BTCUSDT.BINANCE_USDT")
eng.add_strategy(MyStrat("my"))
eng.run()
```

Python 侧的 `buy/sell/short/cover/cancel/subscribe/get_contract` 与 C++ 一一对应；`on_*` 回调通过 nanobind trampoline 被 C++ 事件循环调用（GIL 已自动获取，见 [FAQ](FAQ.md)）。

## 3. 插件策略

见 [Plugins](Plugins.md)：把上面的策略类放进独立 DLL 工程，按 `plugin_abi.h` 导出纯 C 接口即可，主程序运行时加载，**无需重编**。

## 注意事项

- 持仓 `pos_` 由策略在 `on_trade` 自维护，**未与柜台实际持仓对账**；异常成交会导致 pos 漂移（内置示例已在注释中标注）。
- `on_tick` 内调用 `BarGenerator::update_tick` 聚合 K 线时，仍是同步调用，仍在当前 GIL 作用域内，无需额外 GIL 处理。
- 同一引擎下多策略**共享一个账户/同一标的净持仓**，目前没有「每策略独立组合」隔离。
