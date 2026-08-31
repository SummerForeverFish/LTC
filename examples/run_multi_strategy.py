# run_multi_strategy.py - 用 Python 让多个策略在同一引擎里同时运行
#
# 职责：演示 build.ps1 / build_py.ps1 导出的 MainEngine 如何同时挂多个策略。
#       关键：多策略只支持 MainEngine 类（live_csv / live_binance / live_ctp / tick_csv）；
#       BacktestEngine / TickBacktestEngine 的 add_strategy 是单指针，多挂只生效最后一个。
#       两个策略共用同一账户/净持仓（目前无「每策略独立组合」隔离）。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd（import ltc 即加载 C++ 绑定）。
# 用法：python examples/run_multi_strategy.py
import sys, os, time
# 中文乱码根因：C++ 引擎直接用 printf 输出中文（UTF-8 字节），而 Windows 控制台
# 默认代码页是 GBK(936)。仅靠 Python 的 sys.stdout.reconfigure 管不到 C++ 的 stdout，
# 必须切换控制台输出代码页为 UTF-8。
try:
    import ctypes
    kernel32 = ctypes.windll.kernel32
    kernel32.SetConsoleOutputCP(65001)  # UTF-8
    kernel32.SetConsoleCP(65001)
except Exception:
    pass
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass
# 将脚本所在目录（examples/）及其上级（vncpp/，ltc.pyd 所在）加入导入路径，
# 确保无论从哪个工作目录运行都能 import 到 ltc 模块。
_EXAMPLES_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_EXAMPLES_DIR)  # vncpp/
sys.path.insert(0, _EXAMPLES_DIR)
sys.path.insert(0, _ROOT)
import ltc as v


# ---- 策略 A：双均线（分钟 K 线），Python 子类实现 ----
class DoubleMa(v.Strategy):
    def __init__(self, name, fast=10, slow=30, vol=1.0):
        super().__init__(name)
        self.fast, self.slow, self.vol = fast, slow, vol
        self.closes, self.pos = [], 0.0

    def on_bar(self, bar):
        self.closes.append(bar.close)
        if len(self.closes) > self.slow + 5:
            self.closes.pop(0)
        if len(self.closes) < self.slow:
            return
        fast_ma = sum(self.closes[-self.fast:]) / self.fast
        slow_ma = sum(self.closes[-self.slow:]) / self.slow
        if self.pos == 0 and fast_ma > slow_ma:
            self.buy(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)
        elif self.pos > 0 and fast_ma < slow_ma:
            self.sell(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)

    def on_trade(self, td):
        if td.direction == v.Direction.LONG and td.offset == v.Offset.OPEN:
            self.pos += td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.OPEN:
            self.pos -= td.volume
        elif td.direction == v.Direction.LONG and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume


# ---- 策略 B：布林带均值回归（分钟 K 线），Python 子类实现（无需插件 DLL 也能跑）----
class Bollinger(v.Strategy):
    def __init__(self, name, window=20, k=2.0, vol=1.0):
        super().__init__(name)
        self.window, self.k, self.vol = window, k, vol
        self.closes, self.pos = [], 0.0

    def _sma(self, n):
        cnt = min(n, len(self.closes))
        return sum(self.closes[-cnt:]) / cnt if cnt else 0.0

    def _std(self, n, mu):
        cnt = min(n, len(self.closes))
        if cnt <= 1:
            return 0.0
        acc = sum((x - mu) ** 2 for x in self.closes[-cnt:])
        return (acc / cnt) ** 0.5

    def on_bar(self, bar):
        self.closes.append(bar.close)
        if len(self.closes) > self.window + 5:
            self.closes.pop(0)
        if len(self.closes) < self.window:
            return
        mid = self._sma(self.window)
        sigma = self._std(self.window, mid)
        upper, lower = mid + self.k * sigma, mid - self.k * sigma
        if self.pos == 0.0:
            if bar.close < lower:
                self.buy(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)
            elif bar.close > upper:
                self.short_(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)
        elif self.pos > 0.0 and bar.close > mid:
            self.sell(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)
        elif self.pos < 0.0 and bar.close < mid:
            self.cover(bar.vt_symbol, bar.close, self.vol, v.OrderType.MARKET)

    def on_trade(self, td):
        if td.direction == v.Direction.LONG and td.offset == v.Offset.OPEN:
            self.pos += td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.OPEN:
            self.pos -= td.volume
        elif td.direction == v.Direction.LONG and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume


if __name__ == "__main__":
    print("[info] 已注册策略类型:", v.StrategyRegistry.list())

    # MainEngine 支持多策略；回放网关（推 on_bar，模拟撮合）。
    # tick 模式可换成 v.TickCsvGateway(eng.event_engine(), "TICK")，这样驱动 on_tick。
    eng = v.MainEngine()
    gw = v.CsvReplayGateway(eng.event_engine(), "CSV")
    eng.add_gateway(gw)
    eng.set_default_gateway("CSV")

    # 挂多个策略：name 必须唯一（MainEngine 以 name 为 key，同名后者覆盖前者）。
    eng.add_strategy(DoubleMa("MA_fast", fast=5, slow=20, vol=1.0))
    eng.add_strategy(Bollinger("BB_slow", window=20, k=2.0, vol=1.0))

    # ===== 可选：再挂一个「插件 DLL」提供的策略（DLL 不存在时跳过，不影响上面两个）=====
    # err = v.PluginLoader.load("plugins/bollinger_plugin.dll")
    # if not err:
    #     st = v.StrategyRegistry.create("BollingerPlugin", "BB_plugin",
    #                                    {"window": "20", "k": "2.0", "vol": "1.0", "live": "1"})
    #     eng.add_strategy(st)
    # else:
    #     print("[warn] 插件未加载:", err)
    # =================================================================================

    eng.connect_all({"file": os.path.join(_ROOT, "data", "BTCUSDT.csv"),
                     "vt_symbol": "BTCUSDT.BINANCE_USDT",
                     "speed_ms": "0"})   # speed_ms=0 表示全速回放
    eng.start()
    print("[info] 多策略运行中（3 秒）...")
    time.sleep(3)
    eng.stop()
    print("[info] 已停止。注意：多策略共用同一账户/净持仓，会互相影响对方持仓与盈亏。")
