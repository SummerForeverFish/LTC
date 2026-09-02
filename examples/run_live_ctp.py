# run_live_ctp.py - 用 Python 策略直接驱动 C++ CTP 期货实盘主引擎
# 用法:
#   python run_live_ctp.py                       # 默认读取 config/ctp_settings.ini
#   python run_live_ctp.py path/to/my.ini        # 指定配置文件
#
# 职责：演示用 Python 策略接入 C++ MainEngine + CtpGateway，做 tick 驱动的
#       双均线（聚合 1 分钟 K 线）实盘/仿真交易。
# 前置：需先执行 build_py.ps1 生成 ltc.pyd，并把 thostmduserapi_se.dll /
#       thosttraderapi_se.dll 放到项目根目录 (或系统 PATH)；并配置好 ctp_settings.ini。
# 注意：live_trading=0 时仅打印金叉/死叉信号（DRY），=1 才真实下单。
#
# 持仓管理：由框架（C++ PositionStore）按【策略名】命名空间自动维护「开仓均价 + 持仓量」，
#           每次成交时由 BaseStrategy::handle_trade 自动落盘到 strategy_position.json，
#           重启仍能恢复，多策略互不干扰。Python 策略直接 self.get_strategy_pos(vt_symbol)
#           读取当前策略自身持仓（非账户全部持仓）；有持仓只平仓、不开仓。
import sys, os, time
# 项目根目录(ltc/)：ltc.pyd 与 thost*_se.dll 放在这里，便于直接 import
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ltc as v

from datetime import datetime, timezone, timedelta
# 期货是北京时间，用固定 UTC+8，避免依赖本机时区
CST = timezone(timedelta(hours=8))
def fmt_ms(ms):
    return datetime.fromtimestamp(ms / 1000, CST).strftime("%Y-%m-%d %H:%M:%S")


class CtpDemo(v.Strategy):
    """tick 驱动双均线：把 tick 聚合成 1 分钟 K 线后做金叉/死叉。"""
    def __init__(self, name, live=False, fast=10, slow=30, vol=1.0, vt_symbol="rb2610.SHFE"):
        super().__init__(name)
        self.live, self.fast, self.slow, self.vol = live, fast, slow, vol
        self.vt_symbol = vt_symbol
        self.closes, self.pos = [], 0.0
        self.cur_min, self.last_close = 0, 0.0
        # 持仓由框架按【策略名】命名空间自动持久化到 strategy_position.json（C++ PositionStore），
        # 重启仍能恢复；多策略互不干扰。这里仅把启动时的持仓载入内存。
        pos = self.get_strategy_pos(self.vt_symbol)
        self.pos = pos.volume
        print(f"[position] 策略[{name}] 载入 {self.vt_symbol} 持仓={self.pos:.0f} 开仓均价={pos.avg_price:.2f}")

    def on_init(self):
        self.subscribe(self.vt_symbol)
        self.bg = v.BarGenerator(self.on_bar1, 2, self.on_bar2, v.Interval.MINUTE)

    def on_tick(self, tk):
        #print(tk.datetime,tk.last_price)
        minute = tk.datetime - (tk.datetime % 60000)
        if self.cur_min != minute:
            if self.cur_min != 0 and self.last_close > 0.0:
                bar = v.BarData()
                bar.symbol, bar.exchange, bar.vt_symbol = tk.symbol, tk.exchange, tk.vt_symbol
                bar.interval = v.Interval.MINUTE
                bar.datetime = self.cur_min
                bar.close = self.last_close
                self.on_bar(bar)
            self.cur_min = minute
        self.last_close = tk.last_price
        self.bg.update_tick(tk)

    # def on_bar(self,bar):
    #     print('1m', fmt_ms(bar.datetime), bar.close)

    def on_bar2(self,bar2):
        print('2m', fmt_ms(bar2.datetime), bar2.close)

    def on_bar1(self, bar):
        self.bg.update_bar(bar)
        print('bar', fmt_ms(bar.datetime), bar.close)
        self.closes.append(bar.close)
        if len(self.closes) > self.slow + 5:
            self.closes.pop(0)
        if len(self.closes) < self.slow:
            return
        fast_ma = sum(self.closes[-self.fast:]) / self.fast
        slow_ma = sum(self.closes[-self.slow:]) / self.slow

        # 当前策略持仓（非账户持仓）：净持仓 != 0 即视为有持仓
        pos = self.get_strategy_pos(bar.vt_symbol).volume
        has_pos = abs(pos) > 1e-9

        if not has_pos and fast_ma > slow_ma:
            # 无持仓 + 金叉 -> 开多
            if self.live:
                self.buy(bar.vt_symbol, bar.close, self.vol, v.OrderType.LIMIT)
            else:
                print(f"[DRY] 金叉 BUY {bar.vt_symbol} close={bar.close:.2f}")
        elif has_pos and fast_ma < slow_ma:
            # 有持仓 + 死叉 -> 只平仓，不开新仓
            if pos > 0:
                if self.live:
                    self.sell(bar.vt_symbol, bar.close, self.vol, v.OrderType.LIMIT)
                else:
                    print(f"[DRY] 死叉 SELL(平多) {bar.vt_symbol} close={bar.close:.2f}")
            else:  # pos < 0
                if self.live:
                    self.cover(bar.vt_symbol, bar.close, self.vol, v.OrderType.LIMIT)
                else:
                    print(f"[DRY] 死叉 COVER(平空) {bar.vt_symbol} close={bar.close:.2f}")
        # 其余情况（有持仓且仍金叉 / 无持仓且死叉）：不开仓，也不平仓

    def on_trade(self, td):
        # 持仓账本由框架在分发 on_trade 前自动维护（C++ BaseStrategy::handle_trade），
        # 这里直接读取本策略持仓即可（只统计本策略成交，非账户全部持仓）。
        p = self.get_strategy_pos(td.vt_symbol)
        self.pos = p.volume
        print(f"[trade] {td.direction} @{td.price:.2f} vol={td.volume} pos={self.pos:.0f} avg={p.avg_price:.2f}")

    def on_order(self, o):
        if o.status == v.Status.REJECTED:
            print(f"[rejected] {o.vt_orderid}")


# 极简 INI 解析：忽略空行与 # 注释，按 "key=value" 写入字典（不依赖 configparser）。
def load_ini(path):
    cfg = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            k, val = line.split("=", 1)
            cfg[k.strip()] = val.strip()
    return cfg


if __name__ == "__main__":
    # 解析配置文件：未指定则用项目根 config/ctp_settings.ini
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "config", "ctp_settings.ini")
    cfg = load_ini(cfg_path)
    # 从 INI 读取运行参数（含是否真实交易 live_trading）
    live = cfg.get("live_trading", "0") == "1"
    fast = int(cfg.get("fast", "10"))
    slow = int(cfg.get("slow", "30"))
    vol = float(cfg.get("fixed_volume", "1.0"))
    secs = int(cfg.get("run_seconds", "30"))
    vt = cfg.get("vt_symbol", "rb2610.SHFE")

    # 构造主引擎并接入 CTP 网关（前缀 CTP）
    eng = v.MainEngine()
    gw = v.CtpGateway(eng.event_engine(), "CTP")
    eng.add_gateway(gw)
    eng.set_default_gateway("CTP")
    # 注册策略实例（live 控制是否真实下单）。
    # 注意：多个策略时请用不同的 name，JSON 以 name 隔离，互不覆盖。
    eng.add_strategy(CtpDemo("CTP_Demo_py", live, fast, slow, vol, vt))
    # 把整份 cfg 交给网关，含账号/密码/经纪商/行情交易前置地址等 CTP 字段
    eng.connect_all(cfg)
    eng.start()
    if secs <= 0:
        print(f"[info] 无限运行模式(run_seconds<=0)，按 Ctrl-C 停止... live_trading={live}")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("[info] 收到中断，正在停止...")
    else:
        print(f"[info] CTP 实盘链路运行中({secs}秒) live_trading={live}")
        time.sleep(secs)
    eng.stop()
    print("[info] 已停止")
