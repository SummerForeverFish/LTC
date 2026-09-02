# LTC —— vnpy 风格 C++17 交易框架

参考 vnpy 设计的事件驱动交易框架。零 UI、核心零外部依赖，支持**回测**与**实盘**，  
并可通过 nanobind 把接口暴露给 Python，让 Python 策略直接驱动回测 / 实盘。

## 目录结构

```
LTC/
├── CMakeLists.txt          # 纯回测构建(零依赖)；实盘见 build.ps1
├── build.ps1 / build_py.ps1# MSVC 直编：主程序(含CTP) / 策略插件DLL / Python 绑定
├── main.cpp                # 入口：ltc run <cfg.ini> 配置驱动；list 列策略；兼容旧模式参数
├── include/ltc/
│   ├── core/               # 引擎核心（无交易所依赖）
│   │   ├── object.hpp      # 数据对象 Tick/Bar/Order/Trade/Position/Account/Contract + 枚举
│   │   ├── event.hpp       # 事件引擎（单消费者线程 + 队列）
│   │   ├── util.hpp        # 工具：Logger、毫秒时间转换、CSV 拆分
│   │   ├── gateway.hpp     # BaseGateway 基类（on_tick/on_bar/on_order/on_trade）
│   │   ├── strategy.hpp    # BaseStrategy 基类（buy/sell/short/cover/cancel）
│   │   ├── engine.hpp      # MainEngine / OrderRouter（路由 send/cancel 到网关）
│   │   ├── config.hpp      # ini 配置解析（[section] / # 注释 / UTF-8 BOM）
│   │   ├── strategy_registry.hpp # 策略工厂与注册表（按类型名创建策略）
│   │   ├── plugin_abi.h    # 策略插件的 C 接口约定（宿主与插件共享）
│   │   └── plugin_loader.hpp     # 运行时 LoadLibrary 加载外部策略 DLL
│   ├── gateway/            # 交易所接入（均继承 BaseGateway）
│   │   ├── crypto.hpp      # HMAC/SHA256（币安签名用）
│   │   ├── csv_gateway.hpp # CSV 回放（模拟盘链路，推 on_bar）
│   │   ├── binance_gateway.hpp # 币安 U 本位（默认 Stub 传输，零依赖可跑）
│   │   ├── ctp_gateway.hpp # 上期技术 CTP 期货实盘（MdApi + TraderApi）
│   │   └── tick_csv_gateway.hpp # tick 直喂回放（推 on_tick，tick 级撮合）
│   ├── backtest/           # 回测引擎
│   │   ├── backtest.hpp    # BacktestEngine（CSV 驱动，逐根回放撮合）
│   │   └── tick_backtest.hpp # TickBacktestEngine（tick 直喂，tick 级撮合）
│   └── algo/               # 算法交易下单模块（on_timer 驱动，拆单/追单/撤单）
│       └── algo_base.hpp   # AlgoBase 基类 + TWAP / VP(VWAP) / Iceberg / MidPeg
├── strategies/             # 策略（由 register_builtin.cpp 注册进 exe；可整体不链接本目录走纯插件模式）
│   ├── double_ma_strategy.hpp   # 双均线（bar 驱动）
│   ├── bollinger_strategy.hpp   # 布林带均值回归（bar 驱动，内置版）
│   ├── ctp_demo_strategy.hpp    # CTP tick 聚合成分钟线后双均线（带 dry-run 安全闸）
│   ├── tick_demo_strategy.hpp   # tick 直喂演示（在 tick.last_price 上算快/慢均线）
│   ├── twap_demo_strategy.hpp   # TWAP 算法拆单演示（调 algo/TWAPAlgo，on_timer 驱动）
│   └── register_builtin.cpp     # 内置策略注册单元（唯一把策略带入 exe 的编译单元）
├── plugins/                # 外部策略插件：独立编译成 DLL，运行时按配置加载（主程序无需重编）
│   ├── demo_plugin.cpp     # 插件示例，导出 MomentumTick（动量 tick 策略）
│   ├── bollinger_plugin.cpp# 布林带插件：tick 聚合 1 分钟 K 线后做均值回归（含 BarGenerator 用法）
│   └── my_ma_plugin.cpp   # 最小可运行插件模板（双均线 tick 策略），教学用
├── bindings/
│   └── ltc.cpp        # nanobind 绑定（Python 可回测 / 实盘 / tick / 加载插件）
├── config/
│   ├── run.ini             # 运行配置（推荐入口）：模式 / 插件 / 策略 / 参数
│   ├── run_builtin.ini     # 仅用内置策略（TickDemo / DoubleMA）的配置示例
│   ├── run_bollinger.ini   # 内置 Bollinger 策略 bar 回测示例
│   ├── run_bollinger_plugin.ini        # 用 bollinger_plugin.dll 加载布林带（bar 回测）
│   ├── run_bollinger_plugin_tick.ini   # 同上，但用 tick 数据（插件内 BarGenerator 合成 K 线）
│   ├── run_demo_plugin.ini # 用 demo_plugin.dll（MomentumTick）的配置
│   ├── run_myplugin.ini    # 用 my_ma_plugin.dll 的配置
│   ├── run_tickcsv.ini     # tick 直喂回放（验证零拷贝事件队列路径）
│   ├── run_twap_demo_tick.ini   # TWAP 算法拆单演示（tick 直喂回放）
│   ├── ctp_settings.ini         # 实盘配置（含账号密码，已被 .gitignore 忽略）
│   └── ctp_settings.ini.example # 配置模板
├── examples/
│   ├── run_backtest.py      # Python 驱动回测
│   ├── run_live.py          # Python 驱动 CSV 回放
│   ├── run_live_ctp.py      # Python 驱动 CTP 实盘
│   ├── gen_tick_data.py     # 生成合成 tick 数据 -> data/BTCUSDT_tick.csv
│   ├── run_tick_replay.py   # Python 策略直收 on_tick（TickCsvGateway）
│   ├── run_tick_backtest.py # Python 策略 tick 级回测
│   └── run_multi_strategy.py # Python 多策略同跑（MainEngine + 多个 add_strategy）
└── data/
    ├── BTCUSDT.csv         # 回测样本数据（分钟 K 线）
    └── BTCUSDT_tick.csv    # tick 样本数据（由 gen_tick_data.py 生成）
```

## 构建

| 目标           | 命令                                      | 说明                                             |
| ------------ | --------------------------------------- | ---------------------------------------------- |
| 回测主程序(含 CTP) | `powershell -File build.ps1`            | MSVC 直编，已内置 CTP 头/lib 路径，产出 `build/ltc.exe`  |
| 策略插件 DLL    | `powershell -File build.ps1`（插件段自动构建 `demo`/`bollinger`/`my_ma`） | 每个插件独立产出 `plugins/*.dll`，主程序无需重编；单独重编见下文命令 |
| Python 绑定    | `powershell -File build_py.ps1`         | 需先 `pip install nanobind`，产出根目录 `ltc.pyd` |
| 纯回测(CMake)   | `cmake -B build && cmake --build build` | 零依赖；如需含 CTP 加 `-DLTC_CTP_DIR=<CTP_API_DIR>`  |

> 沙箱无 `cmd.exe` 且 D: 盘 SDK 不在默认注册表，故用 `build.ps1` 直接调 `cl.exe` 是最稳路径。
>
> **单独编译某个插件**（需先配好 MSVC 环境：`PATH`/`INCLUDE`/`LIB`，见 `build.ps1` 顶部；
> 务必带 `/DWIN32_LEAN_AND_MEAN /DNOMINMAX` 防止 `windows.h` 的 `min/max/ERROR` 宏污染标准库）：
> ```powershell
> cl /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
>    /LD /DLTC_PLUGIN_BUILD /I . /I include `
>    /Fe:plugins\bollinger_plugin.dll /Fo:build\ plugins\bollinger_plugin.cpp
> ```



## 运行

```
build/ltc.exe run config/run.ini                # 推荐：模式/插件/策略全部由配置决定
build/ltc.exe run config/run_builtin.ini        # 仅用内置策略的配置示例
build/ltc.exe list                              # 列出已注册策略（含插件提供的类型）
build/ltc.exe backtest                          # 旧模式参数仍可用（取该模式默认策略）
build/ltc.exe tick_csv                          # tick 直喂回放(模拟盘, 推 on_tick)
build/ltc.exe tick_backtest                     # tick 级回测(tick 直喂 + tick 级撮合)
build/ltc.exe live_ctp config/ctp_settings.ini   # CTP 实盘(需账号)
build/ltc.exe run config/run_twap_demo_tick.ini  # TWAP 算法拆单演示（tick 直喂回放）
python examples/run_live_ctp.py                   # Python 驱动 CTP(读 config/ctp_settings.ini)
python examples/run_tick_replay.py                # Python 策略直收 on_tick
python examples/run_tick_backtest.py              # Python 策略 tick 级回测
python examples/gen_tick_data.py                  # 生成 data/BTCUSDT_tick.csv
```

## 策略插件化（配置驱动加载）

`main.cpp` **不再 include 任何策略**，策略一律按「类型名 + 参数」在运行时创建。两条来源用法完全一致：

| 来源 | 注册方式 | 换策略是否要重编主程序 |
| --- | --- | --- |
| 内置策略 | `strategies/register_builtin.cpp` 里用 `LTC_REGISTER_STRATEGY` 自注册 | 需要（把该 .cpp 一起链接即可） |
| 外部插件 DLL | 配置 `[plugins]` 填 dll 路径，运行时 `LoadLibrary` 后注册其导出类型 | **不需要** |

配置示例（`config/run_bollinger_plugin.ini`，用插件加载布林带策略）：

```ini
[run]
mode = backtest
data_file = data/BTCUSDT.csv
vt_symbol = BTCUSDT.BINANCE_USDT

[plugins]
bollinger = plugins/bollinger_plugin.dll     ; 运行时加载，导出类型 BollingerPlugin

[strategy]
name = BBPlug
type = BollingerPlugin                      ; 内置类型: DoubleMA/TickDemo/CtpDemo/Bollinger/TwapDemo
params = window=20,k=2.0,vol=1.0,live=1
```

换策略只改 `type` 与 `params`，主程序不动。

### 多策略同时运行

`run_config` 会收集 `[strategy]` + `[strategy.2]`…`[strategy.16]`（最多 16 个）段，逐个 `add_strategy`。
**但能否真正并行运行取决于 mode**——因为两类引擎的 `add_strategy` 实现不同：

| mode | 引擎 | 多策略支持 | 说明 |
| --- | --- | --- | --- |
| `live_csv` / `live_binance` / `live_ctp` / `tick_csv` | `MainEngine` | ✅ | `strategies_[name]=st` 的 map，可同时挂多个 |
| `backtest`（bar 回测） | `BacktestEngine` | ❌ | `strategy_ = st` 单指针，多段会被覆盖，**只跑最后一个** |
| `tick_backtest` | `TickBacktestEngine` | ❌ | 同上，单指针只保留最后一个 |

> 注意：`backtest` / `tick_backtest` 当前只支持**单策略**；写多个 `[strategy.N]` 段不会报错，但只会生效最后一个。
> 若需要回测并行多策略，需把两个回测引擎的 `add_strategy` 改为 `vector` 并逐个驱动（暂未实现）。

**同一引擎下多策略共享一个账户**：所有策略共用同一资金、同一标的净持仓。多个策略交易同一 `vt_symbol` 时，
它们的买卖会互相影响对方持仓/盈亏——目前**没有「每策略独立组合」隔离**。请确保各 `[strategy]` 的 `name` 唯一
（`MainEngine` 以 `name` 为 key，同名后者覆盖前者）。

`MainEngine` 模式下的多策略配置示例：

```ini
[run]
mode = live_csv
data_file = data/BTCUSDT.csv

[strategy]
type = DoubleMA
name = MA_fast            ; name 必须唯一
params = fast=5,slow=20,vol=1.0

[strategy.2]
type = Bollinger
name = BB_slow            ; 内置或插件类型皆可，此处为内置 Bollinger
params = window=20,k=2.0,vol=1.0
```

### 写自己的插件

继承 `BaseStrategy`，按 `core/plugin_abi.h` 的 C 接口导出
（`ltc_plugin_api_version / count / strategy_type / strategy_desc / create / destroy`）；
在 `include "ltc/core/plugin_abi.h"` **之前** `#define LTC_PLUGIN_BUILD`，用它展开 `LTC_PLUGIN_EXPORT` 为 `__declspec(dllexport)`。
完整范例见 `plugins/demo_plugin.cpp`、`plugins/bollinger_plugin.cpp`（含「tick 聚合 1 分钟 K 线」的 `BarGenerator` 用法）、`plugins/my_ma_plugin.cpp`。
编译命令见上方「构建」表的插件单独编译说明。

> 跨 DLL 边界一律走 C 接口：策略对象由插件 `new`、也由插件 `destroy` 释放
> （宿主把它包进 `shared_ptr` 并挂自定义 deleter 回调插件侧），避免 CRT/ABI 不一致导致崩溃。
> 插件 DLL 由 `PluginLoader` 持有，务必在策略实例全部释放后再 `unload_all()`。

Python 侧同样支持：

```python
import ltc as v
err = v.PluginLoader.load("plugins/demo_plugin.dll")   # 返回空串表示成功
print(v.StrategyRegistry.list())   # ['CtpDemo', 'DoubleMA', 'MomentumTick', 'TickDemo']
st = v.StrategyRegistry.create("MomentumTick", "M", {"window":"30","vol":"1.0","live":"1"})
eng = v.TickBacktestEngine(); eng.load_tick_csv("data/BTCUSDT_tick.csv", "")
eng.add_strategy(st); eng.run()
```

## CTP 实盘接入

`config/ctp_settings.ini`（模板见 `ctp_settings.ini.example`）填好行情/交易前置、经纪商代码与账号密码。  
`live_trading=0` 时只打印买卖信号不下单；置 `1` 才真实报单。当前环境无经纪商凭证，  
已验证到「编译通过 + 接口可构造 + 参数异常安全返回」这一层，真实成交需你自备账号。

## Tick 直喂（tick 级回放 / 回测）

vs 原有 `CsvReplayGateway`（推 `on_bar`）与 `BacktestEngine`（喂 `on_bar`），新增的两条链路**直接喂 `on_tick`**，  
不经过 K 线聚合，便于验证 tick 级 / 高频策略：

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| `TickCsvGateway` | `gateway/tick_csv_gateway.hpp` | 读 tick CSV，回放并直推 `on_tick`；内置 tick 级撮合（限价按 `last_price` 穿越、市价按 `last_price`+滑点） |
| `TickBacktestEngine` | `backtest/tick_backtest.hpp` | 载入 tick CSV，驱动 `on_tick`，订单在**下一笔 tick** 撮合（无未来函数），维护权益曲线 |
| `TickDemoStrategy` | `strategies/tick_demo_strategy.hpp` | 在 `tick.last_price` 上算快/慢均线，穿越即交易；`live=false` 只打印信号 |

tick CSV 列：`datetime,symbol,exchange,last_price,last_volume,bid_price_1,bid_volume_1,ask_price_1,ask_volume_1,open_interest,volume`  
（`exchange` 列会被映射为 `Exchange` 枚举；列名大小写不敏感）。两种链路均已验证到「编译通过 + 直喂可运行 + tick 级撮合成交」。

## 算法交易下单模块

内置 `include/ltc/algo/algo_base.hpp`，移植自 `LS/TradeAgent/Algo` 的 Python 算法框架。**事件驱动、非阻塞**，
由策略的 `on_timer` 统一驱动：把「目标持仓 − 当前持仓」的调整量拆成**多笼、多轮**逐步下单，自动追单、撤单、补单，
直到达到目标持仓或达到最大轮数。

| 算法 | 类名 | 定价策略 |
| --- | --- | --- |
| TWAP（时间加权平均价） | `algo::TWAPAlgo` | 激进追价：买=min(卖一×1.01, 涨停价)、卖=max(买一×0.99, 跌停价)，尽量保证成交 |
| VWAP（成交量加权平均价） | `algo::VPAlgo` | 按成交量占比拆单（默认均分），对手价±滑点，不追价 |
| Iceberg（冰山） | `algo::IcebergAlgo` | 大单拆小单逐笼下，不追价，对手价±滑点 |
| MidPeg（中间价） | `algo::MidPegAlgo` | 买卖中间价委托，不追价 |

### 配套基础设施（算法依赖）

| 能力 | 新增实现 | 说明 |
| --- | --- | --- |
| 最新行情查询 | `EventEngine::get_tick(vt_symbol)` | TICK 事件到达时按 vt_symbol 缓存；策略/算法在定时器节拍内查最新价 |
| 活跃委托跟踪 | `BaseStrategy::handle_order` + `has_active_orders(vt_symbol)` | ORDER 事件自动维护「未终态委托」集合（SUBMITTING/SUBMITTED/PARTTRADED） |
| 批量撤单 | `BaseStrategy::cancel_symbol(vt_symbol)` | 一键撤掉某合约全部活跃委托 |
| 定时器驱动 | `MainEngine` 定时器线程（默认 500ms，可 `set_timer_interval`） | 周期投递 TIMER 事件 → 策略 `on_timer` → 算法 `on_timer` |
| 策略日志 | `BaseStrategy::write_log(msg)` | 自动带策略名前缀 |

### 用法（在策略内，最简路径）

算法交易已内置到 `BaseStrategy` 基类，策略**只需调用一行**，拆单/追单/撤单/多轮全部自动完成：

```cpp
class MyStrategy : public BaseStrategy {
public:
    void on_start() override {
        // 唯一一行：TWAP 把持仓调到 5（拆 4 笼、最多 6 轮、笼间隔 5 秒）
        send_target_pos_twap("rb2610.SHFE", /*target_pos=*/5.0,
                             /*price=*/0.0, /*slip_point=*/0,
                             /*chase_time=*/5.0, /*n_intervals=*/4, /*epochs=*/6);
    }
    // 无需写 on_timer / on_stop：基类 handle_timer 自动驱动算法、handle_stop 自动撤单
};
```

基类提供统一入口（`on_timer` 由框架自动驱动，策略不写任何拆单逻辑）：

| 方法 | 算法 | 说明 |
| --- | --- | --- |
| `send_target_pos_twap(vt, target, ...)` | TWAP | 激进追价，保证成交 |
| `send_target_pos_vp(vt, target, ..., volume_profile)` | VWAP | 按占比拆单（缺省均分） |
| `send_target_pos_iceberg(vt, target, ...)` | Iceberg | 大单拆小单，不追价 |
| `send_target_pos_midpeg(vt, target, ...)` | MidPeg | 中间价委托，不追价 |
| `stop_algo(vt)` / `stop_all_algos()` | — | 停算法并撤活跃委托（`handle_stop` 已自动调用） |

参数：`target_pos` 目标持仓（净持仓，正负表多空）、`price` 指定价（0=按行情）、
`slip_point` 滑点（pricetick 倍数）、`chase_time` 笼间隔（秒）、`n_intervals` 笼数、`epochs` 最大轮数。

> 框架自动维护：`handle_timer` 驱动算法 → `handle_trade` 更新持仓账本（JSON 持久化）
> → `handle_order` 维护活跃委托 → `handle_stop` 停算法并撤单。策略无需任何额外代码。

### 演示运行（内置 `TwapDemo`）

`strategies/twap_demo_strategy.hpp`：启动即把持仓调至 `target_pos`，`live=0` 只打日志不下单。

```ini
[strategy]
name = TwapDemo
type = TwapDemo
params = vt_symbol=BTCUSDT.BINANCE_USDT,target_pos=5,price=0,slip_point=0,chase_time=2,n_intervals=3,epochs=3,live=1
```

```bash
build/ltc.exe run config/run_twap_demo_tick.ini    # tick 直喂回放，观察拆单/追单/多轮流程
```

### Python 端使用

基类统一入口（`send_target_pos_twap` 等）同样暴露到 `ltc` 绑定，Python 策略写法与 C++ 一致，也是**一行调用**：

```python
import ltc as v

class MyStrat(v.Strategy):
    def __init__(self, name):
        super().__init__(name)
        self.vt_symbol = "rb2610.SHFE"
    def on_start(self):
        # 唯一一行：TWAP 把持仓调到 5，拆单/追单/撤单/多轮自动完成
        self.send_target_pos_twap(self.vt_symbol, 5.0, chase_time=10, n_intervals=4, epochs=6)
    # 无需 on_timer / stop_all：基类 handle_timer / handle_stop 自动处理
```

完整可运行示例：`examples/run_live_algo.py`（tick 回放 + TWAP 拆单，真实撮合成交后自动多轮补单）：

```bash
python examples/run_live_algo.py [秒数]
```

> 进阶：也可直接持有算法对象（`v.TWAPAlgo(self)` 等）做更精细控制，见 `wiki/Algo-Trading.md`。

### 与参考实现（LS/TradeAgent/Algo）的差异——A 股 → 期货适配

- 参考实现中 A 股专属逻辑（`pre_close` 涨跌停、科创板 688 最小 200 手、9:25-9:30 集合竞价窗口）
  已替换为期货通用版本：TWAP 激进价用 tick 的 `limit_up/limit_down` 封顶，最小下单量取合约 `min_volume`。
- 合约元信息缺失时降级为默认规则（`pricetick=0.01`、`min_volume=1`）并打告警，便于 CSV 回放等无合约场景。
- 新增的 Python 策略辅助方法（`get_tick` / `has_active_orders` / `cancel_symbol` / `write_log`）
  已暴露到 `ltc` 绑定，Python 策略同样可用。

## 低延迟 / 高频说明

事件驱动 + 零拷贝无锁事件队列已落地（见 `include/ltc/core/ring_queue.hpp` 与 `event.hpp` / `src/event_engine.cpp`）：

| 改造点 | 旧实现 | 新实现 | 收益 |
| --- | --- | --- | --- |
| 事件载荷 | `std::any`（类型擦除 + 堆分配 + RTTI） | `std::variant`（内联存储，读时返回 `const&`） | 零拷贝读取、无堆分配 |
| 事件传输 | `std::queue<Event>` + `std::mutex` + `condvar` | 有界无锁 **MPMC 环形队列**（Vyukov 算法，预分配、无 ABA、缓存行隔离） | 多生产者无锁入队、消费者零分配出队 |
| 分派 | `unordered_map<EventType, vector<function>>` 查找 | 按类型**扁平分桶** + 热路径 `register_strategy` 直调虚函数 | 无 map 查找、无 `std::function` 间接 |
| 网关推送 | `on_tick(const T&)` 拷贝进 `any` | `on_tick(T)` 按值 + `std::move` 进 variant | 局部对象零拷贝入队 |
| 线程模型 | 条件变量阻塞唤醒 | 忙轮询 + 空闲 `yield` 退让 | 低延迟、空闲不占满核 |

**吞吐基准**（`ltc bench`，1 生产者 / 1 消费者，预分配 65536 槽）：约 **8.6 M events/sec**，队列满时自旋退让背压、不丢事件。  
`EventEngine` 暴露 `queue_capacity()` / `approx_queue_size()` / `dropped_events()` 用于运行期监控水位。

**tick 直喂**链路（`TickCsvGateway` → `EventEngine` → 策略 `on_tick`）已打通零拷贝路径，适合 tick 级 / 高频策略验证。  
仍待进一步的高频改造：异步下单（WebSocket 而非阻塞 REST）、网关推送/撮合批处理、以及把策略热路径限留在 C++（插件化已支持 C++ 策略）。


