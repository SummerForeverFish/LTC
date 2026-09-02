# 算法交易下单模块（Algo Trading）

C++ 实现的算法交易下单模块（`include/ltc/algo/algo_base.hpp`），移植自
[`LS/TradeAgent/Algo`](../../../LS_rpc_api/LS/TradeAgent/Algo) 的 Python 算法框架。
**事件驱动、非阻塞**：由策略的 `on_timer` 统一驱动，把「目标持仓 − 当前持仓」的调整量
拆成**多笼、多轮**逐步下单，自动追单、撤单、补单，直到达到目标或达到最大轮数。

## 一、为什么需要算法模块

裸 `buy/sell` 一次下满目标量有两大问题：

1. **冲击成本**：大单一次性打出去会把价格打穿，成交价远差于预期；
2. **没有容错**：一次全成/全不成的极端情况（涨跌停、流动性差）无法处理。

算法模块把大单**切碎**按节奏放出（TWAP/VWAP/Iceberg），或按**中间价**埋伏（MidPeg），
并在未成交时**自动撤单重试**（多轮 epoch），显著改善成交质量与稳定性。

## 二、四种算法

| 算法 | 类名 | 定价策略 | 适用场景 |
| --- | --- | --- | --- |
| TWAP | `algo::TWAPAlgo` | 激进追价：买=min(卖一×1.01, 涨停价)、卖=max(买一×0.99, 跌停价)，**向上/下取整到最小价位** | 追求尽快成交、对价格不敏感的调仓 |
| VWAP | `algo::VPAlgo` | 按成交量占比拆单（默认均分），对手价±滑点 | 希望跟随当日成交量分布、控制市场冲击 |
| Iceberg | `algo::IcebergAlgo` | 对手价±滑点，不追价 | 隐藏大单意图、逐笼吸筹/出货 |
| MidPeg | `algo::MidPegAlgo` | 买卖中间价±滑点 | 不急于成交、以更优价格挂单 |

所有算法共享同一套**任务生命周期**，仅「本笼量」与「本笼价」两个策略点可定制
（`_slice_volume` / `_slice_price`，自定义算法只需继承 `AlgoBase` 覆写这两个虚函数）。

## 三、AlgoBase 公共接口

```cpp
class AlgoBase {
public:
    explicit AlgoBase(BaseStrategy* strategy);   // 绑定驱动它的策略

    void on_timer(int64_t now_ms);               // 策略 on_timer 里调用，驱动到期任务
    bool is_active(const std::string& vt_symbol) const;  // 某合约算法是否运行中
    void stop(const std::string& vt_symbol);     // 停止：停任务 + 撤该合约活跃委托
    void stop_all();                             // 停止全部
    // 子类提供 start(vt_symbol, target_pos, price, slip_point, chase_time, n_intervals, epochs)
};
```

### 任务状态机（一轮的完整流程）

```
start(vt_symbol, target_pos, ...)
  │  _init_task：查合约（缺省用默认规则）→ 建任务
  ▼
_start_round：读当前持仓 get_strategy_pos
  ├─ |pos−target|≈0        → _finish_task("完成")
  ├─ 越过目标(超买/超卖)    → _finish_task("完成(超买/超卖)")，不反转
  ├─ epoch ≥ max_epochs    → _finish_task("达到最大轮数")
  ├─ 调整量 < min_volume   → _finish_task("剩余调整量不足最小单位")
  └─ 否则定方向/总量/剩余 → _execute_slice（第 1 笼）
        │
        ▼  （每笼间隔 chase_time）
_execute_slice：算量(_slice_volume) → 算价(_slice_price) → buy/sell
  ├─ 持仓已达目标          → _all_done
  ├─ 剩余>0 且 笼数未满     → 安排下一笼
  └─ 否则                 → _all_done（等 chase_time）
        │
        ▼
_on_execute（定时器触发）：
  ├─ 仍有剩余量            → _execute_slice（继续下笼）
  └─ 笼已下完              → 撤活跃单 → 无单则 _on_round_done → 下一轮
```

## 四、在策略中使用

### 最简路径（算法已内置到 `BaseStrategy` 基类）

写策略只需**一行调用**，`on_timer` 驱动、持仓维护、停止撤单全由基类自动完成：

```cpp
class MyStrategy : public BaseStrategy {
public:
    void on_start() override {
        // 唯一一行：TWAP 把持仓调到 5（目标净持仓，正=多、负=空）
        send_target_pos_twap("rb2610.SHFE", 5.0);
    }
    // 无需写 on_timer / on_stop
};
```

基类统一入口（C++ 与 Python `ltc` 绑定一致）：

| 方法 | 算法 | 默认参数 |
| --- | --- | --- |
| `send_target_pos_twap(vt, target, ...)` | TWAP | chase=30, slices=3, epochs=8 |
| `send_target_pos_vp(vt, target, ..., volume_profile)` | VWAP | chase=30, slices=3, epochs=8 |
| `send_target_pos_iceberg(vt, target, ...)` | Iceberg | chase=10, slices=5, epochs=8 |
| `send_target_pos_midpeg(vt, target, ...)` | MidPeg | chase=10, slices=3, epochs=8 |
| `stop_algo(vt)` / `stop_all_algos()` | — | 停算法 + 撤活跃委托 |

框架自动管线：`handle_timer`（TIMER 事件）→ 驱动算法 → `handle_trade` 更新持仓账本 →
`handle_order` 维护活跃委托 → `handle_stop`（停止时）停算法撤单。

### 进阶：直接持有算法对象（精细控制）

需要同时挂多个算法、或按信号分别控制时，可自行持有 `AlgoBase` 派生对象：

```cpp
#include "ltc/algo/algo_base.hpp"

class MyStrategy : public BaseStrategy {
    algo::MidPegAlgo algo_{this};              // 绑定当前策略
public:
    void on_timer(int64_t t) override { algo_.on_timer(t); }
    void on_start() override {
        algo_.start("rb2610.SHFE", 5.0);       // 目标净持仓 5
    }
    void on_stop() override { algo_.stop_all(); }
};
```

要点：

- **一个策略可挂多个算法对象**（不同合约用同一算法、或同一合约换算法），互不干扰。
- 算法**按合约（vt_symbol）隔离任务**：同一 `AlgoBase` 可同时管理多个合约，各合约独立进度。
- 在 `on_start` / 信号回调里 `start()`；重复 `start` 同合约会自动先 `stop` 旧的。
- **持仓来源**：`get_strategy_pos`（框架按成交自动维护，JSON 持久化，重启恢复）。
  算法本身不维护持仓，`on_trade` 不需要任何额外代码。

## 五、依赖的基础设施

算法运行依赖框架以下新增能力（对策略/算法透明）：

| 能力 | 位置 | 说明 |
| --- | --- | --- |
| 最新行情缓存 `get_tick` | `EventEngine` | TICK 事件到达时按 vt_symbol 覆盖缓存，供定时器节拍内查价 |
| 活跃委托跟踪 `handle_order` | `BaseStrategy` | ORDER 事件自动维护 `active_orderids_`（未终态入、终态出） |
| 查询/批量撤单 `has_active_orders` / `cancel_symbol` | `BaseStrategy` | 撤掉某合约全部活跃委托 |
| 定时器生产者 | `MainEngine` | 独立线程每 500ms（可 `set_timer_interval`）投递 TIMER 事件 |
| 策略日志 `write_log` | `BaseStrategy` | 自动带策略名前缀，算法日志统一走它 |

> 回测引擎（`BacktestEngine` / `TickBacktestEngine`）也已让委托/成交走 `handle_order`/`handle_trade`，
> 因此活跃委托跟踪与持仓账本在回测中口径与实盘一致；但**回测不驱动 `on_timer`**，
> 故算法目前主要用于实盘（`MainEngine` 链路，含 `tick_csv` 回放模式）。
> 如需在回测中验证算法，可自行在引擎里周期调用 `strategy_->on_timer(now_ms)`。

## 六、参数速查（start 公共参数）

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `target_pos` | 目标净持仓（正多负空） | —（必填） |
| `price` | 指定委托价，0=按行情 | 0 |
| `slip_point` | 滑点（pricetick 倍数） | 0 |
| `chase_time` | 笼间隔/追单间隔（秒） | 各算法不同（TWAP/VP 30，Iceberg/MidPeg 10） |
| `n_intervals` | 拆单笼数 | 3（Iceberg 默认 5） |
| `epochs` | 最大轮数（每轮重新评估剩余调整量） | 8 |

`VPAlgo::start` 额外支持 `volume_profile`（各笼权重数组，自动归一化；缺省均分）。

## 七、运行演示

```bash
build/ltc.exe run config/run_twap_demo_tick.ini
```

`TwapDemo` 策略（`strategies/twap_demo_strategy.hpp`）启动即把 `BTCUSDT.BINANCE_USDT` 持仓调到 5，
拆 3 笼、最多 3 轮、笼间隔 2 秒；`live=1` 真实报单，`live=0` 仅打日志。

### Python 端使用（ltc 绑定）

四种算法已暴露到 `ltc` 模块，Python 策略用法与 C++ 完全同构：

```python
import ltc as v

class MyStrat(v.Strategy):
    def __init__(self, name):
        super().__init__(name)
        self.twap = v.TWAPAlgo(self)          # 绑定当前策略
    def on_start(self):
        self.twap.start(self.vt_symbol, 5.0,
                        chase_time=10, n_intervals=4, epochs=6)
    def on_timer(self, t):
        self.twap.on_timer(t)                 # 定时器节拍驱动
    def on_stop(self):
        self.twap.stop_all()
```

可运行示例：`examples/run_live_algo.py`（`MainEngine` + `TickCsvGateway` tick 回放，
TWAP 拆单会真实撮合成交、自动多轮补单，最后「达到最大轮数」收尾）。

## 八、A 股 → 期货适配说明

移植自 A 股算法框架，做了如下**期货化**处理：

- **去掉** A 股专属：`pre_close` 涨跌停、科创板 688 最小 200 手、9:25-9:30 集合竞价不撤单窗口；
- **改用** 期货通用：TWAP 激进价用 tick 的 `limit_up/limit_down` 封顶；最小下单量取合约 `min_volume`；
- **合约缺失**时降级默认规则（`pricetick=0.01`、`min_volume=1`）并打告警，便于 CSV 回放。

如需 A 股规则（科创板、竞价窗口），可在子类覆写 `_slice_price` / `_get_min_volume` 扩展。
