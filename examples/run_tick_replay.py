# run_tick_replay.py - 用 Python 策略直接接收 C++ TickCsvGateway 直喂的 on_tick
# 用法:
#   python examples/run_tick_replay.py
#
# 职责：演示以 Python 策略接入 C++ MainEngine + TickCsvGateway，将 tick CSV
#       逐笔直喂策略的 on_tick（不聚合 K 线），做 tick 级双均线回放/模拟。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd；需 data/BTCUSDT_tick.csv（可用
#       gen_tick_data.py 生成）。
# 注意：live=False 时仅打印金叉/死叉信号，不下单。
import os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ltc as v


# tick 级快/慢均线策略：直接在 on_tick 的 last_price 上算均线，穿越即交易。
class TickMaStrategy(v.Strategy):
    """tick 级快/慢均线策略：直接在 on_tick 的 last_price 上算均线，穿越即交易。"""
    def __init__(self, name, fast=50, slow=200, vol=1.0, live=False):
        super().__init__(name)
        self.fast = fast
        self.slow = slow
        self.vol = vol
        self.live = live
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
            if self.live:
                px = tk.ask_price_1 if tk.ask_price_1 > 0 else tk.last_price * 1.0001
                self.buy(tk.vt_symbol, px, self.vol, v.OrderType.LIMIT)
            else:
                print(f"[DRY] tick 金叉 BUY @{tk.last_price:.2f}")
        elif self.pos > 0 and fma < sma:
            if self.live:
                px = tk.bid_price_1 if tk.bid_price_1 > 0 else tk.last_price * 0.9999
                self.sell(tk.vt_symbol, px, self.vol, v.OrderType.LIMIT)
            else:
                print(f"[DRY] tick 死叉 SELL @{tk.last_price:.2f}")


def main():
    # 构造主引擎并接入 tick CSV 直喂网关（前缀 TICK）
    eng = v.MainEngine()
    gw = v.TickCsvGateway(eng.event_engine(), "TICK")
    eng.add_gateway(gw)
    eng.set_default_gateway("TICK")
    # 注册策略实例（快线=50，慢线=200，单笔仓位=1.0，live=False 只打印信号）
    st = TickMaStrategy("PyTick_Demo", fast=50, slow=200, vol=1.0, live=False)
    eng.add_strategy(st)

    # 连接网关：回放 tick 文件，speed_ms=0 表示按原速（不加速）直喂
    eng.connect_all({"file": os.path.join(ROOT, "data", "BTCUSDT_tick.csv"), "speed_ms": "0"})
    eng.start()
    print("tick 直喂回放运行中(15秒)...")
    time.sleep(15)
    eng.stop()
    print("结束")


if __name__ == "__main__":
    main()
