// tick_demo_strategy.hpp - tick 直喂演示策略 (在原始 tick 上做快/慢均线, 穿越即交易)
//
// 职责：演示「tick 直喂」模式——不聚合 K 线，直接在每一笔 tick.last_price 上维护快慢均线，
//       体现策略可逐笔（亚秒级）响应的能力，与 CtpDemoStrategy 的分钟聚合形成对照。
// 信号逻辑：在 on_tick 中维护 last_price 滑窗，攒够慢线窗口后算快慢均线；
//           金叉(pos_==0) -> 开多，死叉(pos_>0) -> 平多。
// 适用市场：tick 频率高、流动性好的品种（如数字货币、活跃期货主力），需低延迟行情。
// 已知限制 / dry-run 闸门：
//   1) dry-run 闸门由构造参数 live 控制（默认 false）：live=false 只打印 [DRY] 信号不下单。
//   2) 快/慢均线参数默认 fast=50/slow=200（tick 数量级远大于分钟线），需按 tick 频率重设。
//   3) live=true 时以对手价(ask/bid)加微小偏移限价报单，未考虑滑点与深度，仅作演示；
//   4) 仅做多、固定手数、无止损；pos_ 由 on_trade 自维护，未与柜台对账。
#pragma once
#include <deque>
#include <cmath>
#include <algorithm>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// tick 直喂快慢均线策略：直接在逐笔 tick 的 last_price 上算均线，金叉做多/死叉平多
class TickDemoStrategy : public BaseStrategy {
public:
    // 构造参数：live 是否真实报单(默认 false=dry-run)；fast/slow 均线周期(默认 50/200)；
    //          fixed_volume 固定手数
    TickDemoStrategy(const std::string& name, bool live = false,
                     int fast = 50, int slow = 200, double fixed_volume = 1.0)
        : BaseStrategy(name), live_(live), fast_(fast), slow_(slow), fixed_volume_(fixed_volume) {}

    // 初始化回调：打印 live/fast/slow/vol 配置
    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " tick 策略初始化 live=" + std::to_string(live_) +
                    " fast=" + std::to_string(fast_) + " slow=" + std::to_string(slow_) +
                    " vol=" + std::to_string(fixed_volume_));
    }

    // tick 回调（策略核心）：过滤无效价后维护 last_price 滑窗，慢线窗口填满后计算快慢均线，
    // 金叉开多/死叉平多；live=true 以对手价限价报单，否则 [DRY] 仅打印信号。
    void on_tick(const TickData& tk) override {
        if (tk.last_price <= 0.0) return;
        prices_.push_back(tk.last_price);
        if ((int)prices_.size() > slow_ + 8) prices_.pop_front();
        if ((int)prices_.size() < slow_) return;

        double fast_ma = ma(fast_);
        double slow_ma = ma(slow_);
        std::string vt = tk.vt_symbol;

        if (pos_ == 0.0 && fast_ma > slow_ma) {
            if (live_) {
                double px = tk.ask_price_1 > 0 ? tk.ask_price_1 : tk.last_price * 1.0001;
                buy(vt, px, fixed_volume_, OrderType::LIMIT);
            } else {
                Logger::log(Logger::Level::INFO, name() + " [DRY] tick 金叉 BUY @" +
                            std::to_string(tk.last_price));
            }
        } else if (pos_ > 0.0 && fast_ma < slow_ma) {
            if (live_) {
                double px = tk.bid_price_1 > 0 ? tk.bid_price_1 : tk.last_price * 0.9999;
                sell(vt, px, fixed_volume_, OrderType::LIMIT);
            } else {
                Logger::log(Logger::Level::INFO, name() + " [DRY] tick 死叉 SELL @" +
                            std::to_string(tk.last_price));
            }
        }
    }

    // 成交回调：更新自维护持仓 pos_ 并打印成交明细
    void on_trade(const TradeData& td) override {
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN)        pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN)   pos_ -= td.volume;
        else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE)   pos_ -= td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE)  pos_ -= td.volume;
        Logger::log(Logger::Level::INFO, name() + " 成交 " + direction_to_str(td.direction) +
                    " @" + std::to_string(td.price) + " vol=" + std::to_string(td.volume) +
                    " pos=" + std::to_string(pos_));
    }

    // 委托回调：记录拒单，避免无声失败
    void on_order(const OrderData& o) override {
        if (o.status == Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " 委托被拒: " + o.vt_orderid);
    }

private:
    // 内部计算：窗口末尾 n 点简单移动平均，窗口不足时取全部并规避除零
    double ma(int n) const {
        int cnt = std::min(n, (int)prices_.size());
        double sum = 0;
        auto it = prices_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return cnt ? sum / cnt : 0.0;
    }

    bool live_;
    int fast_, slow_;
    double fixed_volume_;
    std::deque<double> prices_;
    double pos_ = 0.0;
};

} // namespace ltc
