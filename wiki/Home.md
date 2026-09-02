# LTC Wiki

**LTC** 是一个参考 [vnpy](https://www.vnpy.com) 设计的**事件驱动交易框架**，核心用 C++17 编写：核心引擎零 UI、零外部依赖，并可通过 **nanobind** 把接口暴露给 Python，让 Python 策略直接驱动回测 / 实盘。支持**回测**与**实盘**两类场景。

## 关键特性

- **事件驱动内核**：`EventEngine` 单消费者线程 + 零拷贝无锁环形队列（`std::variant` 载荷 + Vyukov MPMC 队列），热路径按事件类型直调策略虚函数，无 map 查找、无 `std::function` 间接。
- **多运行模式**：bar 级回测、tick 级回测/回放、CSV 模拟盘、币安(Stub) 链路、CTP 期货实盘。
- **策略插件化**：策略按「类型名 + 参数」在运行时创建；外部策略可编译成 DLL、运行时 `LoadLibrary` 加载，**主程序零重编**。
- **多语言策略**：C++ 内置策略、C++ 插件策略、Python 策略（继承 `v.Strategy`，走 nanobind trampoline）。
- **K 线合成**：`BarGenerator`（vnpy 风格三段式），tick → 1 分钟 K → N 分钟/N 小时/日 K。
- **确定性回测撮合**：next-bar / next-tick 成交模型，天然避免未来函数；手续费按比例、滑点按不利方向施加。

## 架构一句话

```
Gateway / 回测引擎
   │  on_tick / on_bar / on_order / on_trade(...)
   ▼
EventEngine  (无锁环形队列, 单消费者线程)
   │  dispatch（按事件类型分桶）
   ▼
Strategy.on_tick / on_bar / ...   ← 策略在此响应
   │  buy / sell / short / cover / cancel
   ▼
OrderRouter (MainEngine 实盘 / BacktestEngine 回测)
   │
   ▼
Gateway.send_order → 交易所 / 模拟撮合
```

回测引擎（确定性）**不走事件队列**，直接同步回调策略；实盘（`MainEngine` + `EventEngine`）才走队列 + 独立线程。

## 文档导航

| 页面 | 内容 |
| --- | --- |
| [Getting-Started](Getting-Started.md) | 构建（C++ / Python）、运行、ini 配置 |
| [Architecture](Architecture.md) | 事件引擎、数据流、线程模型、零拷贝队列 |
| [Core-API](Core-API.md) | 数据对象、枚举、`EventEngine`/`MainEngine`/`BaseStrategy`/`BarGenerator` |
| [Writing-Strategies](Writing-Strategies.md) | C++ 内置策略、Python 策略、插件策略写法 |
| [Plugins](Plugins.md) | 插件 C ABI、编写与加载外部 DLL |
| [Gateways](Gateways.md) | CSV / 币安 / CTP / TickCsv 各接口 |
| [Backtesting](Backtesting.md) | bar 回测与 tick 回测的 API 与撮合模型 |
| [Algo-Trading](Algo-Trading.md) | 算法交易下单模块：TWAP/VWAP/Iceberg/MidPeg、任务状态机、参数速查 |
| [Python-Binding](Python-Binding.md) | nanobind 模块 `ltc` 的完整 API 与示例 |
| [Performance](Performance.md) | 零拷贝队列改造点与吞吐基准 |
| [FAQ](FAQ.md) | GIL、多策略、已知限制 |

## 当前内置 / 示例策略

- `DoubleMA` / `Bollinger`（bar 驱动，内置）
- `CtpDemo`（CTP tick 聚合分钟线后双均线，内置，带 dry-run 安全闸）
- `TickDemo`（tick 直喂，在 `tick.last_price` 上算快/慢均线，内置）
- `TwapDemo`（TWAP 算法拆单演示，内置，`on_timer` 驱动）
- 插件示例：`MomentumTick`（`demo_plugin`）、`BollingerPlugin`（`bollinger_plugin`）、`MACross`（`my_ma_plugin`）
