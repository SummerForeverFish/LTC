# run_tick_backtest_bars.py - BarGenerator(K线合成器) 演示：tick -> 1分钟K -> 5分钟K 双均线
# 用法:
#   python examples/run_tick_backtest_bars.py
#
# 职责：演示 vnpy 三段式 K 线合成在 Python 侧的用法——
#       ① on_tick 里喂 BarGenerator.update_tick（tick 合成 1 分钟 K）；
#       ② on_bar_1m 里接力 BarGenerator.update_bar（1 分钟 K 聚合成 5 分钟 K）；
#       ③ on_bar_5m 里跑双均线信号并下单。
# 另一条等价路径：让 TickBacktestEngine 自己合成（eng.set_bar_interval(v.Interval.MINUTE5)），
# 策略只需实现 on_bar，无需自持 BarGenerator，见文件末尾注释。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd；需 data/BTCUSDT_tick.csv。
import os, sys, datetime

# 中文乱码根因：C++ 引擎直接用 printf 输出中文（UTF-8 字节），而 Windows 控制台默认
# 代码页是 GBK(936)。ltc 模块加载时已把控制台切到 UTF-8(65001)，但 Python 的 stdout
# 编码仍可能按 GBK 写，导致错位。这里同步设置控制台代码页并 reconfigure stdout 为 UTF-8。
try:
    import ctypes
    kernel32 = ctypes.windll.kernel32
    kernel32.SetConsoleOutputCP(65001)
    kernel32.SetConsoleCP(65001)
except Exception:
    pass
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ltc as v


class BarMaStrategy(v.Strategy):
    """tick 经 BarGenerator 合成 5 分钟 K，在 5 分钟 K 上跑双均线（仅做多演示）。"""

    def __init__(self, name, fast=5, slow=20, vol=1.0):
        super().__init__(name)
        self.fast = fast
        self.slow = slow
        self.vol = vol
        self.closes = []      # 5 分钟 K 收盘价滑窗
        self.pos = 0.0
        # vnpy 三段式核心：自持一个 K 线合成器
        #   参数1：1 分钟 K 收口回调（本策略 on_bar_1m）
        #   参数2/3/4：聚合 5 根 1 分钟 K 收口成 5 分钟 K，回调 on_bar_5m
        self.bg = v.BarGenerator(self.on_bar_1m, 5, self.on_bar_5m, v.Interval.MINUTE)
        # 便捷等价写法（目标周期一步到位）：
        # self.bg = v.BarGenerator(self.on_bar_1m, v.Interval.MINUTE5, self.on_bar_5m)

    # 把毫秒时间戳格式化为可读 datetime。C++ 的 parse_datetime 按本地时区把 CSV 字符串
    # 转成 epoch，所以这里用本地时区回显才能与 CSV 原始时间（如 2024-01-01 00:00:00）对齐。
    @staticmethod
    def _fmt(ts):
        return datetime.datetime.fromtimestamp(ts / 1000.0).strftime("%Y-%m-%d %H:%M:%S")

    # ① tick 回调：逐笔喂入合成器，跨分钟时自动收口 1 分钟 K 并回调 on_bar_1m
    def on_tick(self, tk):
        self.bg.update_tick(tk)
        # print(f"[{self.name}] tick {self._fmt(tk.datetime)} {tk.last_price}")
    def on_bar(self, bar):
        print(f"[{self.name}] 5分钟K {self._fmt(bar.datetime)} {bar.close}")

    # ② 1 分钟 K 回调：接力喂入合成器，每满 15 根收口一根 15 分钟 K 并回调 on_bar_5m
    def on_bar_1m(self, bar):
        self.bg.update_bar(bar)
        
    def ma(self,n):
        cnt = min(n, len(self.closes))
        return sum(self.closes[-cnt:]) / cnt
    # ③ 5 分钟 K 回调（信号核心）：金叉开多 / 死叉平多
    def on_bar_5m(self, bar):
        print(f"[{self.name}] n分钟K {self._fmt(bar.datetime)} {bar.close}")
        self.closes.append(bar.close)
        if len(self.closes) > self.slow + 5:
            self.closes.pop(0)
        if len(self.closes) < self.slow:
            return

        fma, sma = self.ma(self.fast), self.ma(self.slow)
        if self.pos == 0 and fma > sma:
            self.buy(bar.vt_symbol, bar.close * 1.0001, self.vol, v.OrderType.LIMIT)
        elif self.pos > 0 and fma < sma:
            self.sell(bar.vt_symbol, bar.close * 0.9999, self.vol, v.OrderType.LIMIT)

    def on_trade(self, td):
        if td.offset == v.Offset.OPEN:
            self.pos += td.volume if td.direction == v.Direction.LONG else -td.volume
        else:
            self.pos = 0.0
        print(f"[{self.name}] 成交 {td.direction} {td.offset} @ {td.price} vol={td.volume} pos={self.pos}")


def main():
    # 构造 tick 级回测引擎（策略自持 BarGenerator，无需引擎合成）
    eng = v.TickBacktestEngine()
    eng.set_capital(1_000_000.0)
    eng.set_commission(0.0004)
    eng.set_slippage(0.0)
    eng.load_tick_csv(os.path.join(ROOT, "data", "BTCUSDT_tick.csv"), "")
    # 演示数据 data/BTCUSDT_tick.csv 仅覆盖 50 分钟（约 10 根 5 分钟 K），
    # 故均线窗口取小值；实盘/更长数据请按需调大（如 fast=5, slow=20）。
    eng.add_strategy(BarMaStrategy("PyBar5mMA", fast=2, slow=5, vol=1.0))
    # eng.set_bar_interval(v.Interval.MINUTE5) 
    eng.run()

    eq = eng.equity_curve()
    if eq:
        first, last = eq[0][1], eq[-1][1]
        print(f"期末权益={last:.2f}  总收益={(last - first) / first * 100:.2f}%  tick数={len(eq)}")


# ---- 等价替代：让引擎用内置 BarGenerator 合成 K 线 ----
# 若策略只需要目标周期 K（不需要同时看 1 分钟 K），可不自持 BarGenerator：
#   eng.set_bar_interval(v.Interval.MINUTE5)   # tick 自动聚合成 5 分钟 K 回调 on_bar
#   eng.run()
# 策略只需实现 on_bar(self, bar)，5 分钟 K 会直接推过来。

if __name__ == "__main__":
    main()
