# run_backtest.py - 用 Python 写的策略直接驱动 C++ 回测引擎
#
# 职责：演示如何以 Python 子类化 ltc.Strategy，注册到 C++ 回测引擎做
#       K 线级（分钟）双均线回测，并输出权益曲线。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd（import ltc 即加载 C++ 绑定）。
# 用法：python examples/run_backtest.py
import sys, os
# 中文乱码根因：C++ 引擎直接用 printf 输出中文（UTF-8 字节），而 Windows 控制台
# 默认代码页是 GBK(936)，会把 UTF-8 字节误读成 GBK。仅靠 Python 的
# sys.stdout.reconfigure 管不到 C++ 的 stdout，必须切换控制台输出代码页为 UTF-8。
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


# 双均线策略（分钟 K 线）：快线上穿慢线买入、下穿卖出，持仓由 on_trade 维护。
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
        print(f"[trade] {td.direction} @{td.price:.2f} vol={td.volume} pos={self.pos}")


if __name__ == "__main__":
    # 构造回测引擎
    eng = v.BacktestEngine()
    # 初始资金 100 万
    eng.set_capital(1_000_000.0)
    # 单边手续费率 0.04%
    eng.set_commission(0.0004)
    # 加载分钟数据 CSV：vt_symbol 指定合约，Interval.MINUTE 为周期，True 表示首行为表头
    eng.load_csv(os.path.join(_ROOT, "data", "BTCUSDT.csv"),
                 "BTCUSDT.BINANCE_USDT", v.Interval.MINUTE, True)
    # 注册策略实例（快线=10，慢线=30，单笔仓位=1.0）
    eng.add_strategy(DoubleMa("DoubleMA_py", 10, 30, 1.0))
    # 运行回测
    eng.run()
    # 取权益曲线（点序列），打印点数与末值
    eq = eng.equity_curve()
    print(f"[info] 权益曲线点数: {len(eq)}, 末值: {eq[-1][1]:.2f}" if eq else "[info] 无权益曲线")
