# 常见问题（FAQ）

## Python 绑定没有加 GIL 吗？

**加了，而且是「自动加」的那一种，不需要手写。**

策略 `on_*` 走的是 nanobind 的 **trampoline**（`NB_TRAMPOLINE` + `NB_OVERRIDE`）。链路是：

- `ltc.cpp` 的 `PyStrategy` 用 `NB_OVERRIDE` 把虚函数调用转发给 Python 子类；
- `NB_OVERRIDE` → `trampoline_enter()` → `attach_tstate()`，在**常规构建**下内部调用 `PyGILState_Ensure()` 自动获取 GIL；离开时 `trampoline_leave()` → `detach_tstate()` → `PyGILState_Release()` 释放。

因此 CTP 行情/交易线程 → `EventEngine` 消费者线程 → Python 策略 `on_tick` 这一整条链路是 **GIL 安全的**，无需在 C++ 里手写 `gil_scoped_acquire`。

需要注意的两点：

1. **`register_handler`（通用处理器）这条路径目前没有 trampoline 的 GIL 保护**，但它在项目里**只接受 C++ `std::function`**（未暴露给 Python），所以不涉及 Python 回调。如果你以后把 `register_handler` 暴露给 Python、让用户传 Python 回调，那些回调在 C++ 线程上被直接调用时会**拿不到 GIL → 崩溃**，需在调用处包 `nb::gil_scoped_acquire`。
2. **`PyGILState_Ensure` 意味着 C++ 线程会阻塞等 GIL**：所有 Python 回调在 CPython 下串行跑在 GIL 上（这是 CPython 的必然，不是 bug）。所以 `on_tick` 里的 Python 逻辑要轻量；重活尽量放进 C++ 策略/插件。

## 回测和实盘的策略能共用吗？

能。策略只依赖 `BaseStrategy` 与 `OrderRouter` 抽象；实盘由 `MainEngine` 实现 `OrderRouter`，回测由 `BacktestEngine` / `TickBacktestEngine` 实现。**同一份策略代码即可回测也可实盘**。

## 多策略并行？

- `live_csv` / `live_binance` / `live_ctp` / `tick_csv` 模式用 `MainEngine`，`strategies_[name]=st` 是 map，**可同时挂多个**（`[strategy]` + `[strategy.2]` … 最多 16 个）。
- `backtest` / `tick_backtest` 是单指针 `strategy_`，**只跑最后一个**（写多个段不报错，但只生效最后一个）。
- 同一引擎下多策略**共享一个账户、同一标的净持仓**，没有「每策略独立组合」隔离。各 `[strategy]` 的 `name` 必须唯一（`MainEngine` 以 `name` 为 key，同名后者覆盖前者）。

## CTP 实盘怎么跑？

1. `config/ctp_settings.ini.example` 复制为 `ctp_settings.ini`，填 `md_front` / `td_front` / `broker_id` / `user_id` / `password`（看穿式认证填 `auth_code` / `app_id`）。
2. `build/ltc.exe live_ctp config/ctp_settings.ini` 或 `python examples/run_live_ctp.py`。
3. 默认（`live_trading=0`）只打印买卖信号不下单；置 `1` 才真实报单。

当前环境无经纪商凭证，仅验证到「编译通过 + 接口可构造 + 参数异常安全返回」。

## 数据 CSV 格式？

- **Bar（分钟线）**：`datetime,open,high,low,close,volume,open_interest`（首行表头可选，大小写不敏感），`vt_symbol` 由 `load_csv` 参数指定。
- **Tick**：`datetime,symbol,exchange,last_price,last_volume,bid_price_1,bid_volume_1,ask_price_1,ask_volume_1,open_interest,volume,limit_up,limit_down`，`exchange` 经 `exchange_from_str` 映射、大小写不敏感。

`datetime` 缺失会自动递推（bar 上一根 +60000ms，tick 上一笔 +500ms）；`open<=0` / `last_price<=0` 的脏行被过滤。

## 为什么策略用 `vt_symbol` 而不是 `symbol`？

`vt_symbol = "symbol.exchange"`（如 `rb2609.SHFE`）是订单路由、持仓缓存、事件分发的**全局唯一主键**，能区分同名合约在不同交易所的实例。下单、订阅、撤单、查合约都传 `vt_symbol`。`vt_orderid = "gateway.orderref"` 同理用于撤单路由。

## 已知限制

- 持仓由策略在 `on_trade` **自维护**，未与柜台实际持仓对账。
- CTP SPI 回调未强制切回事件线程；未做断线重连、查询分页/节流。
- 回测撮合为简洁模型（无手续费阶梯、无滑点随量变化、无涨跌停约束）。
- 回测引擎单策略；多策略并行仅 `MainEngine` 模式支持。
