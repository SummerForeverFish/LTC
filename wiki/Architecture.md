# 架构

LTC 是标准的事件驱动框架，分三层：**接口层（Gateway）**、**事件层（EventEngine）**、**策略层（BaseStrategy）**；下单通过 **OrderRouter** 抽象反向回到接口层。

## 1. EventEngine（事件层）

`include/ltc/core/event.hpp` + `src/event_engine.cpp`

- 后台一个消费者线程从**有界无锁 MPMC 环形队列**（`ring_queue.hpp`，Vyukov 算法，预分配 65536 槽）出队并 `dispatch`。
- 载荷用 `std::variant<...>`（`EventPayload`）取代 `std::any`：内联存储、读时返回 `const&` → **零拷贝**，无堆分配、无 RTTI。
- 分派按事件类型**扁平分桶**（`tick_strategies_` / `bar_strategies_` / ...），热路径直接遍历调策略虚函数，**无 map 查找、无 `std::function` 间接**。
- 通用处理器 `register_handler(EventType, Handler)` 按类型分桶（日志 / 监控等）。

事件类型（`EventType`）：`TICK / BAR / ORDER / TRADE / POSITION / ACCOUNT / CONTRACT / LOG / TIMER`。`Event` 的 `as<T>()` 按类型返回 `const T&`，类型不符抛 `bad_variant_access`。

典型链路：

```
gateway.on_tick(TickData) -> ee.put(Event{TICK, move(tick)})
   -> 无锁入队
   -> 引擎线程出队 -> dispatch -> strategy.on_tick(const TickData&)   # 零拷贝
```

队列满时 `put` 自旋退让（背压），**不丢事件**。暴露 `queue_capacity()` / `approx_queue_size()` / `dropped_events()` 用于运行期监控水位。

## 2. 数据 / 回调流

```
                ┌─────────────── Gateway / 回测引擎 ───────────────┐
                │  on_tick / on_bar / on_order / on_trade / on_... │
                └───────────────────────┬──────────────────────────┘
                                        │ Event{type, payload}
                                        ▼
                              EventEngine (无锁队列)
                                        │ dispatch（按类型分桶）
                                        ▼
                          Strategy.on_xxx(const T&)   ← 策略响应
                                        │ buy/sell/short/cover
                                        ▼
                              OrderRouter (接口抽象)
                                        │ send_order / cancel_order
                                        ▼
                        MainEngine(实盘) 或 BacktestEngine(回测)
                                        │
                                        ▼
                              Gateway.send_order → 交易所 / 模拟撮合
```

## 3. 线程模型

| 路径 | 线程 | 说明 |
| --- | --- | --- |
| 网关推送 / 回测喂数据 | 网关线程 / 主线程 | `ee.put(Event)` 无锁入队 |
| `dispatch` | `EventEngine` 消费者线程（单线程） | 调用 `Strategy::on_*` 虚函数 |
| SPI 回调（CTP） | CTP 行情/交易线程 | 组装数据后 `on_*` → `ee.put`（`event.hpp` 已知限制：SPI 回调目前直接回调网关，未强制切回事件线程） |

> **Python 回调的 GIL**：策略 `on_*` 走 nanobind trampoline，`trampoline_enter()` 内部 `attach_tstate()` → `PyGILState_Ensure` 自动获取 GIL，离开时 `detach_tstate()` 释放。因此 CTP/消费者线程回调 Python 策略是 GIL 安全的，无需手写 GIL 代码（详见 [FAQ](FAQ.md)）。

## 4. 回测路径（确定性）

`BacktestEngine` / `TickBacktestEngine` **不经由 EventEngine**：

- 注入 `OrderRouter` 后直接同步调用 `strategy->on_bar` / `on_tick` / `on_order` / `on_trade`。
- 撮合模型：本根 K 线撮合上一根挂单（next-bar），或下一笔 tick 撮合上一笔挂单（next-tick），**无未来函数**。
- 资金/持仓在引擎内自维护，结束时打印权益、收益率、成交笔数、年化 Sharpe。
