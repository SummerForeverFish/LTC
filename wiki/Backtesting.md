# 回测

两套回测引擎，都**不走事件队列**、直接同步回调策略（确定性、可复现、无并发）：

| 引擎 | 头文件 | 驱动 | 撮合基准 | 适用 |
| --- | --- | --- | --- | --- |
| `BacktestEngine` | `backtest/backtest.hpp` | `on_bar`（分钟线 K） | 本根 K 撮合上一根挂单（next-bar） | 中低频 K 线策略 |
| `TickBacktestEngine` | `backtest/tick_backtest.hpp` | `on_tick`（逐笔 tick） | 下一笔 tick 撮合上一笔挂单（next-tick） | tick 级 / 高频策略 |

两者都实现 `OrderRouter`，被策略通过 `set_order_router` 注入，接管 `buy/sell` 下单；资金/持仓在引擎内自维护。

## BacktestEngine（bar 级）

```cpp
BacktestEngine engine;
engine.set_capital(1'000'000.0);     // 初始资金
engine.set_commission(0.0004);        // 手续费比例
engine.set_slippage(0.0);            // 每笔市价滑点（价格单位）
engine.set_size(1.0);                 // 合约乘数
engine.set_annualization(252);        // 年化因子（日线=252）
engine.load_csv("data/BTCUSDT.csv", "BTCUSDT.BINANCE_USDT", Interval::MINUTE, true);
engine.add_strategy(st);
engine.run();                         // 结束打印权益/收益率/成交数/Sharpe/持仓
const auto& eq = engine.equity_curve(); // vector<pair<ms, double>>
```

**CSV 列（大小写不敏感，首行表头可选）**：`datetime,open,high,low,close,volume,open_interest`，外加 `symbol`/`exchange` 可选；`vt_symbol` 由 `load_csv` 参数指定。`open<=0` 的行被过滤；`datetime` 缺失则按上一根 +60000ms 递推。

**撮合模型（next-bar，无未来函数）**：本根 K 线先撮合上一根留下的挂单——限价单看 `bar.low/high` 是否穿越限价、成交价取委托限价；市价单以 `bar.open` 成交并叠加不利方向滑点。

## TickBacktestEngine（tick 级）

```cpp
TickBacktestEngine engine;
engine.set_capital(1'000'000.0);
engine.set_commission(0.0004);
engine.load_tick_csv("data/BTCUSDT_tick.csv", "BTCUSDT.BINANCE_USDT", true);
engine.set_bar_interval(Interval::MINUTE); // 可选：把 tick 聚合成 1 分钟 K 并回调 on_bar
engine.add_strategy(st);
engine.run();
```

- **CSV 列**：`datetime,symbol,exchange,last_price,last_volume,bid_price_1,bid_volume_1,ask_price_1,ask_volume_1,open_interest,volume,limit_up,limit_down`（`exchange` 列经 `exchange_from_str` 映射到枚举，大小写不敏感）。`last_price<=0` 的行过滤；`datetime` 缺失按上一笔 +500ms 递推。
- `set_bar_interval(Interval)`：除直接喂 `on_tick` 外，还会用 `BarGenerator` 把 tick 聚合成指定周期 K 线并回调 `on_bar`（验证 K 线策略不必先离线转 CSV）。`Interval::NONE` 关闭（默认只收 tick）。
- **撮合模型（next-tick，无未来函数）**：下一笔 tick 的 `last_price` 穿越限价/市价滑点成交。

## 资金与绩效

- 实时权益 = 现金 + (多仓 − 空仓) × 最新价 × 合约乘数。
- 手续费按成交金额比例；开仓更新持仓量与持仓均价（加权），平仓按均价计算已实现盈亏并入现金；用 `min(volume, 持仓)` 防超平，残值清零。
- 结束打印：初始/最终权益、总收益率、`成交笔数`、`tick 总数`、年化 Sharpe、期末持仓。

## Python 用法

```python
import ltc as v
eng = v.BacktestEngine()
eng.load_csv("data/BTCUSDT.csv", "BTCUSDT.BINANCE_USDT")
eng.set_capital(1_000_000); eng.set_commission(0.0004)
eng.add_strategy(MyStrat("my"))
eng.run()
# eng.equity_curve() -> list[[ms, equity], ...]

# tick 回测
t = v.TickBacktestEngine()
t.load_tick_csv("data/BTCUSDT_tick.csv", "BTCUSDT.BINANCE_USDT")
t.set_bar_interval(v.Interval.MINUTE)
t.add_strategy(MyStrat("my"))
t.run()
```
