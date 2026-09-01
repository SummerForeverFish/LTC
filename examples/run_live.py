# run_live.py - 用 Python 写的策略直接驱动 C++ 实盘主引擎(CSV 回放/模拟盘)
#
# 职责：演示用 Python 策略接入 C++ MainEngine，通过 CsvReplayGateway 以
#       指定速率回放 CSV 行情（模拟盘/回放，不接真实交易所）。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd；data/BTCUSDT.csv 需已存在。
# 用法：python examples/run_live.py
import sys, os, time
# 将脚本所在目录加入导入路径，确保能 import 到 ltc 模块。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ltc as v


# 双均线策略（分钟 K 线）：快线上穿慢线买入、下穿卖出，持仓由 on_trade 维护。
class DoubleMa(v.Strategy):
    def __init__(self, name, fast=5, slow=20, vol=1.0):
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
    # 构造主引擎（事件驱动），并接入 CSV 回放网关（前缀 CSV）
    eng = v.MainEngine()
    gw = v.CsvReplayGateway(eng.event_engine(), "CSV")
    eng.add_gateway(gw)
    eng.set_default_gateway("CSV")
    # 注册策略实例（快线=5，慢线=20，单笔仓位=1.0）
    eng.add_strategy(DoubleMa("DoubleMA_LIVE_py", 5, 20, 1.0))
    # 连接网关：回放文件、合约、回放速度（每根 bar 间隔毫秒，100ms 加速回放）
    eng.connect_all({
        "file": "data/BTCUSDT.csv",
        "vt_symbol": "BTCUSDT.BINANCE_USDT",
        "speed_ms": "100",
    })
    eng.start()
    print("[info] 模拟盘运行中(15秒)...")
    time.sleep(15)
    eng.stop()
    print("[info] 已停止")
