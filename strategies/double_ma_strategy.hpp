// double_ma_strategy.hpp - 双均线穿越示例策略 (可在回测与实盘复用)
//
// 职责：基于 Bar 收盘价维护快慢两条均线，快线上穿慢线时开多、下穿时平多，属经典双均线趋势策略。
// 信号逻辑：当快线 ma(fast_) 上穿慢线 ma(slow_) 且当前空仓(pos_==0) -> 市价开多；
//           快线下穿慢线且当前持多(pos_>0) -> 市价平多。全程只维持 0 或多头一个固定仓位，不反手。
// 适用市场：中低频趋势行情（商品期货/数字货币的分钟线、日线），震荡市易反复触发信号。
// 已知限制：
//   1) 仅做多、单笔固定手数 fixed_volume_，无加仓/减仓与止损；
//   2) 依赖外部已聚合好的 Bar 数据（回测或分钟线网关），本身不做 K 线聚合；
//   3) 持仓 pos_ 由 on_trade 自维护，未与柜台实际持仓对账，异常成交会导致 pos 漂移。
#pragma once
#include <deque>
#include <numeric>
#include <cmath>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// 双均线 Bar 策略：在 on_bar 中维护收盘价滑窗，计算快慢均线并据此开/平多仓
class DoubleMaStrategy : public BaseStrategy {
public:
    // 构造参数：name 策略名；fast 快线周期；slow 慢线周期；fixed_volume 每笔固定手数
    DoubleMaStrategy(const std::string& name, int fast = 10, int slow = 30,
                     double fixed_volume = 1.0)
        : BaseStrategy(name), fast_(fast), slow_(slow), fixed_volume_(fixed_volume) {}

    // 初始化回调：仅打印配置参数，便于核对 fast/slow 是否符合预期
    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " 初始化, fast=" + std::to_string(fast_) +
                    " slow=" + std::to_string(slow_));
    }

    // K线回调（策略核心）：维护收盘价滑窗，待慢线窗口填满后计算快慢均线，
    // 金叉开多/死叉平多，成交价取 bar.close 以市价单发出。
    void on_bar(const BarData& bar) override {
        closes_.push_back(bar.close);
        if ((int)closes_.size() > slow_ + 5) closes_.pop_front();

        if ((int)closes_.size() < slow_) return;

        double fast_ma = ma(fast_);
        double slow_ma = ma(slow_);

        // 多头穿越：快线上穿慢线且当前无仓 -> 开多
        if (pos_ == 0 && fast_ma > slow_ma) {
            buy(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
        }
        // 多头离场：快线下穿慢线且有仓 -> 平多
        else if (pos_ > 0 && fast_ma < slow_ma) {
            sell(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
        }
    }

    // 成交回调：依据成交方向/开平更新自维护持仓 pos_，并打日志便于核对
    void on_trade(const TradeData& td) override {
        // 依据成交更新持仓（vnpy 风格由策略自行维护 pos）
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN) pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN) pos_ -= td.volume;
        else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE) pos_ -= td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE) pos_ -= td.volume;
        Logger::log(Logger::Level::INFO, name() + " 成交 " + direction_to_str(td.direction) +
                    " @" + std::to_string(td.price) + " vol=" + std::to_string(td.volume) +
                    " pos=" + std::to_string(pos_));
    }

    // 委托回调：处理拒单等异常，避免无声失败（此处仅记录，不下单重试）
    void on_order(const OrderData& o) override {
        // 可在此处理拒单/部分成交
        if (o.status == Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " 委托被拒: " + o.vt_orderid);
    }

private:
    // 内部计算：取收盘价窗口末尾 n 个点的简单移动平均（SMA），窗口不足 n 时取全部
    double ma(int n) const {
        int cnt = std::min(n, (int)closes_.size());
        double sum = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return sum / cnt;
    }

    int fast_, slow_;
    double fixed_volume_;
    std::deque<double> closes_;
    double pos_ = 0.0;
};

} // namespace ltc
