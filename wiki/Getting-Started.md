# 快速开始

## 环境

- Windows + MSVC（`cl.exe`、`link.exe`、`Lib.exe`），建议 VS2022 的 VC 工具集
- Python 3.12+（Python 绑定需要 `nanobind`：`pip install nanobind`）
- CTP 实盘需要上期技术 CTP API（`ThostFtdc*` 头与 `win64/*.lib/*.dll`）

> 当前构建脚本（`build.ps1` / `build_py.ps1`）直接调用 `cl.exe`，绕开需要在注册表里找 SDK 的 `cmake`，是最稳的构建路径。

## 构建

| 目标 | 命令 | 产出 |
| --- | --- | --- |
| 主程序（回测 + CTP 实盘） | `powershell -File build.ps1` | `build/ltc.exe` |
| 策略插件 DLL | `build.ps1` 会自动构建 `demo` / `bollinger` / `my_ma` | `plugins/*.dll` |
| Python 绑定 | `powershell -File build_py.ps1` | 根目录 `ltc.pyd` |
| 纯回测（CMake，零依赖） | `cmake -B build && cmake --build build` | `build/ltc`（无 CTP） |

单独编译某个插件（需先在环境里配好 `PATH`/`INCLUDE`/`LIB`，见 `build.ps1` 顶部；**务必带 `/DWIN32_LEAN_AND_MEAN /DNOMINMAX`**，防止 `windows.h` 的 `min/max/ERROR` 宏污染标准库）：

```powershell
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
   /LD /DLTC_PLUGIN_BUILD /I . /I include `
   /Fe:plugins\bollinger_plugin.dll /Fo:build\ plugins\bollinger_plugin.cpp
```

## 运行

### C++ 主程序

```text
build/ltc.exe run config/run.ini                  # 推荐：模式/插件/策略全由配置决定
build/ltc.exe run config/run_builtin.ini          # 仅用内置策略的示例
build/ltc.exe list                                # 列出已注册策略（含插件提供）
build/ltc.exe bench                               # 零拷贝事件队列吞吐/延迟微基准
build/ltc.exe backtest                            # 旧模式：bar 级回测（默认 DoubleMA）
build/ltc.exe tick_csv                            # tick 直喂回放（模拟盘, 推 on_tick）
build/ltc.exe tick_backtest                       # tick 级回测（tick 直喂 + tick 级撮合）
build/ltc.exe live_ctp config/ctp_settings.ini    # CTP 实盘（需账号）
```

### Python 绑定

```text
python examples/run_backtest.py        # Python 驱动 bar 回测
python examples/run_live.py            # Python 驱动 CSV 回放
python examples/run_live_ctp.py        # Python 驱动 CTP 实盘（读 ctp_settings.ini）
python examples/run_tick_replay.py     # Python 策略直收 on_tick
python examples/run_tick_backtest.py   # Python 策略 tick 级回测
python examples/gen_tick_data.py       # 生成 data/BTCUSDT_tick.csv
```

## 配置（ini）

推荐用 `ltc run config/<x>.ini`，三段最关键：

```ini
[run]
mode = backtest|live_csv|live_binance|live_ctp|tick_csv|tick_backtest
data_file = data/BTCUSDT.csv          ; tick 模式默认 data/BTCUSDT_tick.csv
vt_symbol = BTCUSDT.BINANCE_USDT
capital = 1000000
commission = 0.0004
slippage = 0.0
speed_ms = 0                          ; CSV 回放节奏(ms)，0=全速
run_seconds = 15                      ; 模拟盘/实盘自动退出秒数(<=0 无限, 按 Ctrl-C 停)

[plugins]
bollinger = plugins/bollinger_plugin.dll   ; 运行时加载，导出策略类型

[strategy]
name = BBPlug
type = BollingerPlugin                  ; 内置或插件类型名
params = window=20,k=2.0,vol=1.0,live=1

[strategy.2]                            ; 最多 16 个，仅 MainEngine 模式并行
type = DoubleMA
name = MA_fast
params = fast=5,slow=20,vol=1.0
```

`[ctp]` 段用于 CTP 实盘（含 `md_front` / `td_front` / `broker_id` / `user_id` / `password` / `auth_code` 等）。配置模板见 `config/ctp_settings.ini.example`。

> `live_trading=0` 时 CTP 策略只打印买卖信号不下单；置 `1` 才真实报单。模板里的账号密码已被 `.gitignore` 忽略。
