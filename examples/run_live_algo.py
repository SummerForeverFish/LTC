# run_live_algo.py - Python 策略使用算法交易（基类内置，最简写法）
#
# 算法交易已内置到 v.Strategy 基类：策略只需在信号处调用一行
#   self.send_target_pos_twap(vt_symbol, target_pos, ...)，
# 拆单/追单/撤单/多轮由框架自动完成：
#   - MainEngine 定时器线程投递 TIMER -> handle_timer 自动驱动算法 -> 再转调用户 on_timer；
#   - 持仓账本自动维护，算法据 get_strategy_pos 判断是否继续下单；
#   - 停止时 handle_stop 自动停算法并撤活跃委托。
# 链路：MainEngine + TickCsvGateway（tick 回放，真实撮合成交）。
# 用法：
#   python examples/run_live_algo.py            # 20 秒 tick 回放演示
#   python examples/run_live_algo.py 30         # 自定义秒数
import sys, os, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ltc as v


class AlgoDemo(v.Strategy):
    """tick 驱动：启动后把持仓调到 target_pos（TWAP 拆单，自动执行）"""
    def __init__(self, name, vt_symbol="BTCUSDT.BINANCE_USDT",
                 target_pos=5.0, live=True):
        super().__init__(name)
        self.vt_symbol = vt_symbol
        self.target_pos = target_pos
        self.live = live

    def on_init(self):
        self.subscribe(self.vt_symbol)
        print(f"[algo] 初始化 {self.vt_symbol} 目标持仓={self.target_pos}")

    def on_start(self):
        print(f"[algo] 启动 TWAP (live={self.live})")
        if self.live:
            # 唯一一行调用：拆 3 笼、最多 3 轮、笼间隔 2 秒
            self.send_target_pos_twap(self.vt_symbol, self.target_pos,
                                      price=0.0, slip_point=0,
                                      chase_time=2.0,
                                      n_intervals=3, epochs=3)
        else:
            print("[algo/DRY] 不真实报单")

    def on_order(self, o):
        if o.status == v.Status.REJECTED:
            print(f"[algo] 委托被拒: {o.vt_orderid}")

    # 无需 on_timer / stop_all：基类 handle_timer / handle_stop 自动处理


if __name__ == "__main__":
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 20

    eng = v.MainEngine()
    gw = v.TickCsvGateway(eng.event_engine(), "TICK")
    eng.add_gateway(gw)
    eng.set_default_gateway("TICK")
    eng.add_strategy(AlgoDemo("AlgoPyDemo"))
    eng.connect_all({"file": os.path.join(ROOT, "data", "BTCUSDT_tick.csv"),
                     "speed_ms": "0"})
    eng.start()
    print(f"[info] 算法 TWAP 拆单运行中({secs}秒)...")
    time.sleep(secs)
    eng.stop()
    print("[info] 已停止")
