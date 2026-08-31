#!/usr/bin/env python3
# gen_tick_data.py - 生成一份合成 tick 数据，供 tick 直喂/回测演示使用
# 用法: python examples/gen_tick_data.py [输出csv] [tick数]
#
# 职责：生成模拟的逐笔 tick 行情 CSV（默认 6000 笔），供
#       run_tick_replay.py / run_tick_backtest.py 等示例回放或回测使用。
# 前置：纯 Python 标准库，无需先编译，直接运行即可。
# 输出：默认 <项目根>/data/BTCUSDT_tick.csv，列含 datetime/symbol/exchange/
#       last_price/last_volume/买卖一档价量/open_interest/volume。
import sys, os, math, random, datetime

# 输出路径 OUT 与生成笔数 N：均可通过命令行参数覆盖（参数1=路径，参数2=笔数）。
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "data", "BTCUSDT_tick.csv")
N = int(sys.argv[2]) if len(sys.argv) > 2 else 6000

# 固定随机种子保证结果可复现；以 2024-01-01 00:00:00 为起点，起始价 30000。
random.seed(20240828)
dt = datetime.datetime(2024, 1, 1, 0, 0, 0)
price = 30000.0
rows = []
# 逐笔生成：每笔间隔 500ms，价格做"随机游走 + 轻微均值回复"，再据价差派生买卖一档。
for i in range(N):
    # 每笔 500ms
    dt = dt + datetime.timedelta(milliseconds=500)
    # 随机游走 + 轻微均值回复
    drift = (30000.0 - price) * 0.00002
    price += drift + random.gauss(0, 1.2)
    spread = max(0.1, price * 0.00005)
    bid = price - spread / 2.0
    ask = price + spread / 2.0
    last = random.choice([bid, ask, price])
    last_v = round(random.uniform(0.001, 2.0), 4)
    bid_v = round(random.uniform(0.5, 5.0), 3)
    ask_v = round(random.uniform(0.5, 5.0), 3)
    ts = dt.strftime("%Y-%m-%d %H:%M:%S.") + f"{dt.microsecond // 1000:03d}"
    rows.append((ts, "BTCUSDT", "BINANCE_USDT",
                 f"{last:.2f}", f"{last_v:.4f}",
                 f"{bid:.2f}", f"{bid_v:.3f}",
                 f"{ask:.2f}", f"{ask_v:.3f}",
                 "0.0", f"{random.uniform(100,200):.2f}"))

# 写出 CSV：先确保目标目录存在，再写入表头与各 tick 行，最后打印统计。
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    f.write("datetime,symbol,exchange,last_price,last_volume,"
            "bid_price_1,bid_volume_1,ask_price_1,ask_volume_1,open_interest,volume\n")
    for r in rows:
        f.write(",".join(r) + "\n")
print(f"已生成 {len(rows)} 笔 tick -> {os.path.abspath(OUT)}")
