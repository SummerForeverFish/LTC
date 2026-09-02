# 核心 API

所有类型在 `namespace ltc` 下，头文件位于 `include/ltc/`。Python 侧同名（见 [Python-Binding](Python-Binding.md)）。

## 枚举（`object.hpp`）

| 枚举 | 取值（节选） |
| --- | --- |
| `Exchange` | `NONE / OKX / BINANCE / BINANCE_USDT / SHFE / CFFEX / DCE / CZCE / INE / SSE / SZSE` |
| `Direction` | `NONE / LONG / SHORT` |
| `Offset` | `NONE / OPEN / CLOSE / CLOSETODAY / CLOSEYESTERDAY` |
| `OrderType` | `NONE / LIMIT / MARKET / STOP` |
| `Status` | `NONE / SUBMITTING / SUBMITTED / PARTTRADED / ALLTRADED / CANCELLED / CANCELLING / REJECTED` |
| `Interval` | `NONE / MINUTE / MINUTE3 / MINUTE5 / MINUTE15 / HOUR / HOUR4 / DAILY` |
| `Product` | `NONE / EQUITY / FUTURES / OPTION / INDEX / FOREX / SPOT / ...` |
| `OptionType` | `NONE / CALL / PUT` |

提供字符串互转：`exchange_to_str` / `exchange_from_str` / `direction_to_str` / `offset_to_str` / `ordertype_to_str` / `status_to_str`。

## 数据对象（`object.hpp`）

`TickData` / `BarData` / `OrderData` / `TradeData` / `PositionData` / `AccountData` / `ContractData` —— POD 风格的值类型，字段平铺、默认初始化，可在模块间安全拷贝。

关键约定：

- **`vt_symbol`** = `"symbol.exchange"`，例如 `"BTCUSDT.BINANCE_USDT"` / `"rb2609.SHFE"`，是订单路由、持仓缓存、事件分发的**唯一寻址主键**（`make_vt_symbol(symbol, exchange)`）。
- **`vt_orderid`** = `"gateway.orderref"`，撤单时据此路由回网关（`make_vt_orderid(gateway, oid)`）。
- `TickData.datetime` / `BarData.datetime` 为**毫秒时间戳**。

`OrderRequest`（下单意图）经网关转成 `OrderData`；`CancelRequest`（凭 `vt_orderid` 撤单）。

## EventEngine（`event.hpp`）

```cpp
EventEngine ee;
ee.start();                                  // 启动消费者线程
ee.put(Event{EventType::TICK, std::move(tk)}); // 无锁入队（满则背压）
ee.register_strategy(strategy_ptr);           // 热路径：按类型分桶，直调虚函数
ee.register_handler(EventType::LOG, [](const Event&){}); // 通用处理器
ee.stop();                                   // 停止并排空队列

ee.queue_capacity();      // 预分配槽总数
ee.approx_queue_size();   // 近似事件数
ee.dropped_events();      // 因满被丢弃的事件数（正常为 0）
ee.get_contract(vt);      // 查中央合约表，未找到返回 nullopt
ee.all_contracts();       // 全部已加载合约
ee.get_tick(vt);          // 查最新行情缓存（TICK 事件按 vt_symbol 覆盖），未收到返回 nullopt
```

## MainEngine（`engine.hpp`）—— 实盘总控

- 继承 `OrderRouter`：策略 `buy/sell/...` 最终落到 `MainEngine::send_order`，按交易所/默认网关路由到 `BaseGateway`。
- `start()` 顺序很关键：先注入策略依赖并注册事件回调 → 启动事件引擎线程 → 启动**定时器线程**（默认 500ms，可 `set_timer_interval`）→ 调 `on_init` / `on_start`。
- 定时器线程周期投递 `TIMER` 事件（payload 为毫秒时间戳）→ 策略 `on_timer` → 驱动算法交易模块。

```cpp
MainEngine engine;
auto gw = std::make_shared<CtpGateway>(engine.event_engine().get(), "CTP");
engine.add_gateway(gw);
engine.set_default_gateway("CTP");
engine.add_strategy(my_strategy);     // 以 name 为 key 的 map，可并行多策略
engine.connect_all(settings);         // settings: map<string,string>
engine.subscribe({"rb2609.SHFE"});
engine.start();
// ... run_until_stop / Ctrl-C ...
engine.stop();
```

`subscribe(vt_symbols)` 转发到默认接口；`connect_all` 调每个网关 `connect(settings)`。

## BaseStrategy（`strategy.hpp`）—— 策略基类

生命周期回调（按需重写）：`on_init` / `on_start` / `on_stop`。
事件回调：`on_tick(const TickData&)` / `on_bar(const BarData&)` / `on_order(const OrderData&)` / `on_trade(const TradeData&)` / `on_contract(const ContractData&)` / `on_timer(int64_t)`。

交易辅助（vnpy 风格，统一走 `OrderRouter`）：

```cpp
std::string buy (vt_symbol, price, volume, type=LIMIT);  // 开多
std::string sell(vt_symbol, price, volume, type=LIMIT);  // 平多
std::string short_(vt_symbol, price, volume, type=LIMIT); // 开空
std::string cover(vt_symbol, price, volume, type=LIMIT); // 平空
void cancel(vt_orderid);
void subscribe(vt_symbol);          // 转发到 OrderRouter
auto c = get_contract(vt_symbol);    // optional<ContractData>
const std::string& name();
```

持仓与算法辅助：

```cpp
auto p = get_strategy_pos(vt_symbol);   // PositionInfo{volume, avg_price}，本策略持仓（框架按成交自动维护，JSON 持久化）
void save_position(vt_symbol, volume, avg_price); // 手动覆盖持仓账本（通常无需）
auto t = get_tick(vt_symbol);           // optional<TickData>，最新行情缓存
bool has_active_orders(vt_symbol);      // 该合约是否有未终态委托
void cancel_symbol(vt_symbol);          // 批量撤掉该合约全部活跃委托
void write_log(msg);                    // 带策略名前缀的日志
```

算法交易统一入口（内置基类，策略一行调用，`on_timer` 自动驱动）：

```cpp
bool send_target_pos_twap(vt, target, ...);      // TWAP（激进追价）
bool send_target_pos_vp(vt, target, ..., volume_profile={}); // VWAP（按占比拆单）
bool send_target_pos_iceberg(vt, target, ...);   // Iceberg（大单拆小单）
bool send_target_pos_midpeg(vt, target, ...);    // MidPeg（中间价）
void stop_algo(vt);                              // 停某合约算法 + 撤活跃委托
void stop_all_algos();                           // 停全部算法
```

框架层自动维护（策略无需重写）：

- `handle_trade(td)`：成交入口，先按成交更新持仓账本，再转调 `on_trade`。
- `handle_order(o)`：委托入口，先维护活跃委托集合（SUBMITTING/SUBMITTED/PARTTRADED 视为活跃，
  终态自动移除），再转调 `on_order`。实盘 `EventEngine` 与回测引擎均走这两个入口。
- `handle_timer(t)`：定时器入口，先驱动本策略算法（若启动过 `send_target_pos_*`），再转调 `on_timer`。
- `handle_stop()`：停止入口，先停全部算法并撤单，再转调 `on_stop`（`MainEngine::stop` 调用）。

返回值 `vt_orderid` 用于后续 `cancel`。

## OrderRouter（`gateway.hpp`）

```cpp
struct OrderRouter {
    virtual std::string send_order(const OrderRequest&) = 0;
    virtual void cancel_order(const CancelRequest&) = 0;
    virtual void subscribe(const std::vector<std::string>&) {}  // 回测留空
    virtual ~OrderRouter();
};
```

实盘实现为 `MainEngine`，回测实现为 `BacktestEngine` / `TickBacktestEngine` —— 因此**同一套策略代码可在回测与实盘间无缝切换**。

## BarGenerator（`bar_generator.hpp`）—— K 线合成

vnpy 风格三段式：tick → 1 分钟 K → N 分钟/N 小时/日 K。

```cpp
BarGenerator bg(
    [this](const BarData& b){ on_bar1m(b); },   // 1 分钟 K 收口回调
    5,                                           // window
    [this](const BarData& b){ on_bar5m(b); },    // 窗口 K 收口回调
    Interval::MINUTE);                           // 聚合单位
// 实盘：on_tick 里 feed
void on_tick(const TickData& t) override { bg.update_tick(t); }
// K 线接力
void on_bar1m(const BarData& b) { bg.update_bar(b); }
// 数据流结束务必收口
bg.finish();
```

便捷构造 `BarGenerator(on_bar, Interval::MINUTE5, on_window_bar)` 内部自动拆解 `(window, unit)`。tick 价格取 bid/ask 中间价（均有效时），否则用 `last_price`；成交量优先累计量差分、回退 `last_volume`。无未来函数：窗口 K 在边界上收口，每窗口恰好包含 `window` 根源 K。
