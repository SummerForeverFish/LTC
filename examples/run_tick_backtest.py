# run_tick_backtest.py - tick 级回测：Python 策略 on_tick 直喂 + tick 级撮合
# 用法:
#   python examples/run_tick_backtest.py
#
# 职责：演示用 Python 策略接入 C++ TickBacktestEngine，对 tick CSV 做逐笔
#       直喂 + tick 级撮合回测，并输出期末权益与收益率。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd；需 data/BTCUSDT_tick.csv。
import os, sys

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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ltc as v


# tick 级快/慢均线策略（回测模式，live=True 真正撮合成交）。
class TickMaStrategy(v.Strategy):
    """tick 级快/慢均线策略（回测模式，live=True 真正撮合成交）。"""
    def __init__(self, name, fast=50, slow=200, vol=1.0):
        super().__init__(name)
        self.fast = fast
        self.slow = slow
        self.vol = vol
        self.prices = []
        self.pos = 0.0

    def _ma(self, n):
        cnt = min(n, len(self.prices))
        return sum(self.prices[-cnt:]) / cnt if cnt else 0.0

    def on_tick(self, tk):
        if tk.last_price <= 0:
            return
        self.prices.append(tk.last_price)
        if len(self.prices) > self.slow + 8:
            self.prices.pop(0)
        if len(self.prices) < self.slow:
            return
        fma, sma = self._ma(self.fast), self._ma(self.slow)
        if self.pos == 0 and fma > sma:
            px = tk.ask_price_1 if tk.ask_price_1 > 0 else tk.last_price * 1.0001
            self.buy(tk.vt_symbol, px, self.vol, v.OrderType.LIMIT)
        elif self.pos > 0 and fma < sma:
            px = tk.bid_price_1 if tk.bid_price_1 > 0 else tk.last_price * 0.9999
            self.sell(tk.vt_symbol, px, self.vol, v.OrderType.LIMIT)

    def on_trade(self, td):
        if td.direction == v.Direction.LONG and td.offset == v.Offset.OPEN:
            self.pos += td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.OPEN:
            self.pos -= td.volume
        elif td.direction == v.Direction.LONG and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume
        elif td.direction == v.Direction.SHORT and td.offset == v.Offset.CLOSE:
            self.pos -= td.volume


def main():
    # 构造 tick 级回测引擎
    eng = v.TickBacktestEngine()
    # 初始资金 100 万
    eng.set_capital(1_000_000.0)
    # 单边手续费率 0.04%
    eng.set_commission(0.0004)
    # 滑点设为 0
    eng.set_slippage(0.0)
    # 加载 tick CSV：第二个参数为合约过滤（空串=全部）
    eng.load_tick_csv(os.path.join(ROOT, "data", "BTCUSDT_tick.csv"), "")
    # 注册策略实例（快线=50，慢线=200，单笔仓位=1.0）
    st = TickMaStrategy("PyTickMA", fast=50, slow=200, vol=1.0)
    eng.add_strategy(st)
    # 运行回测（逐笔撮合）
    eng.run()

    # 取权益曲线，打印期末权益与总收益率
    eq = eng.equity_curve()
    if eq:
        first, last = eq[0][1], eq[-1][1]
        print(f"期末权益={last:.2f}  总收益={(last - first) / first * 100:.2f}%  tick数={len(eq)}")


if __name__ == "__main__":
    main()
