# Python 绑定（nanobind）

`bindings/ltc.cpp` 用 nanobind 把核心引擎暴露为 `ltc` 扩展模块（`ltc.pyd`）。策略可继承 `v.Strategy` 用 Python 写，并通过 trampoline 被 C++ 事件循环回调。

```python
import ltc as v   # 模块加载时自动 SetConsoleOutputCP(65001)，避免中文乱码
```

## 枚举

`v.Exchange / Direction / Offset / OrderType / Status / Interval / Product / OptionType`，取值与 C++ 一致（如 `v.Exchange.SHFE`、`v.Interval.MINUTE5`、`v.Direction.LONG`）。

## 数据对象

`v.TickData / BarData / OrderData / TradeData / PositionData / AccountData / ContractData` —— 均可 `v.TickData()` 构造，字段 `def_rw` 可读写（如 `tick.last_price`、`bar.close`、`order.vt_orderid`）。

## 策略基类

```python
class MyStrat(v.Strategy):
    def __init__(self, name):
        super().__init__(name)
    def on_init(self): ...
    def on_start(self): ...
    def on_stop(self): ...
    def on_tick(self, tick): ...
    def on_bar(self, bar): ...
    def on_order(self, order): ...
    def on_trade(self, trade): ...
    def on_timer(self, t): ...
    def on_contract(self, c): ...

    # 交易辅助（与 C++ 同名）
    def do(self):
        self.buy(vt_symbol, price, vol, v.OrderType.LIMIT)
        self.sell(vt_symbol, price, vol)
        self.short(vt_symbol, price, vol)
        self.cover(vt_symbol, price, vol)
        self.cancel(vt_orderid)
        self.subscribe(vt_symbol)
        c = self.get_contract(vt_symbol)   # 未找到返回 None
        print(self.name)
```

## BarGenerator

```python
# 三段式：tick -> 1分钟K -> 5分钟K
self.bg = v.BarGenerator(self.on_bar_1m, 5, self.on_bar_5m, v.Interval.MINUTE)
# 便捷：目标周期直接给
self.bg = v.BarGenerator(self.on_bar_1m, v.Interval.MINUTE5, self.on_bar_5m)
self.bg.update_tick(tick)   # 在 on_tick 里喂
self.bg.update_bar(bar)     # 在 on_bar_1m 里接力
self.bg.finish()            # 结束时收口
```

## 引擎与对象

| 类 | 主要方法 |
| --- | --- |
| `v.EventEngine` | `queue_capacity()` / `approx_queue_size()` / `dropped_events()` / `get_contract(vt)` / `all_contracts()` |
| `v.BacktestEngine` | `load_csv(path, vt, interval=MINUTE, has_header=True)` / `add_strategy` / `set_capital` / `set_commission` / `set_slippage` / `set_size` / `set_annualization` / `run` / `equity_curve` |
| `v.TickBacktestEngine` | `load_tick_csv(path, vt="", has_header=True)` / `set_bar_interval(itv)` / `add_strategy` / `run` / `equity_curve` |
| `v.MainEngine` | `add_gateway(gw)` / `set_default_gateway(name)` / `add_strategy(st)` / `connect_all(settings)` / `subscribe(vt_list)` / `start` / `stop` / `is_running` / `event_engine` |
| `v.BaseGateway` / `CsvReplayGateway` / `BinanceGateway` / `CtpGateway` / `TickCsvGateway` | 构造 `(event_engine, name="CSV"/"BINANCE"/"CTP"/"TICK")` |

## 注册表与插件加载器

```python
v.StrategyRegistry.list()            # 已注册类型列表
v.StrategyRegistry.has(type)
v.StrategyRegistry.describe(type)
v.StrategyRegistry.dump()
# 按类型名 + 实例名 + 参数字典创建（内置或插件类型皆可）
st = v.StrategyRegistry.create("MACross", "M", {"fast":"30","vol":"1.0","live":"1"})

err = v.PluginLoader.load("plugins/my_ma_plugin.dll")  # 空串=成功
v.PluginLoader.loaded()          # 已加载的插件路径
v.PluginLoader.unload_all()
```

## ini 配置解析

```python
cfg = v.IniConfig()
cfg.load("config/run.ini")
cfg.has_section("run"); cfg.section("plugins")
cfg.get("run", "mode", "tick_backtest")
cfg.get_int("run", "run_seconds", 15)
cfg.get_double("run", "capital", 1_000_000.0)
cfg.get_bool("ctp", "live_trading", False)
cfg.last_error()
```

## 完整示例

```python
import ltc as v

class MyStrat(v.Strategy):
    def __init__(self, name):
        super().__init__(name)
        self.bg = v.BarGenerator(self.on_bar_1m, 5, self.on_bar_5m, v.Interval.MINUTE)
    def on_tick(self, tick): self.bg.update_tick(tick)
    def on_bar_1m(self, bar): self.bg.update_bar(bar)
    def on_bar_5m(self, bar):
        self.buy(bar.vt_symbol, bar.close, 1.0, v.OrderType.LIMIT)

# 1) 内置策略回测
eng = v.BacktestEngine()
eng.load_csv("data/BTCUSDT.csv", "BTCUSDT.BINANCE_USDT")
eng.add_strategy(MyStrat("my"))
eng.run()

# 2) 多策略 + MainEngine（实盘/模拟盘）
v.PluginLoader.load("plugins/demo_plugin.dll")
me = v.MainEngine()
me.add_gateway(v.TickCsvGateway(me.event_engine(), "TICK"))
me.set_default_gateway("TICK")
me.add_strategy(v.StrategyRegistry.create("MomentumTick", "M", {"window":"30","vol":"1.0","live":"0"}))
me.connect_all({"file": "data/BTCUSDT_tick.csv", "speed_ms": "0"})
me.start()
```

> 模块加载时 `set_leak_warnings(false)`，避免 trampoline 跨语言持有对象的引用泄漏提示刷屏。
