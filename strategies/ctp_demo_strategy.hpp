// ctp_demo_strategy.hpp - CTP 实盘演示策略 (tick 驱动, 双均线, 默认 dry-run)
//
// 职责：演示 CTP 实盘接入方式——CTP 行情网关只推送 on_tick，本策略用
//       BarGenerator（ltc/core/bar_generator.hpp，vnpy 风格 K 线合成器）把
//       tick 聚合成 1 分钟 K 线，再在聚合出的 Bar 上跑双均线，是 CTP 用户的最小可运行样例。
// 信号逻辑：tick 经 BarGenerator 合成为标准 1 分钟 Bar（含开高低收/量）；
//           Bar 数攒够慢线窗口后计算快慢均线，金叉(pos_==0) -> 开多，死叉(pos_>0) -> 平多。
// 适用市场：国内期货（上期/中金/大商/郑商等 CTP 穿透式网关覆盖的品种）。
// 已知限制 / dry-run 闸门：
//   1) dry-run 闸门由构造参数 live 控制（默认 false）：live=false 时绝不调用 buy/sell，
//      只打印 [DRY] 金叉/死叉信号，便于不上实盘验证逻辑；live=true 才真正以限价单报单。
//   2) 仅做多、固定手数、无止损；pos_ 由 on_trade 自维护，未与柜台对账。
#pragma once
#include <deque>
#include <cmath>
#include <algorithm>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"
#include "ltc/core/bar_generator.hpp"

namespace ltc {

// CTP tick 聚合分钟线双均线策略：BarGenerator 合成 1 分钟 Bar，再跑金叉/死叉
class CtpDemoStrategy : public BaseStrategy {
public:
    // 构造参数：live 是否真实报单(默认 false=dry-run 只打印)；fast/slow 均线周期；
    //          fixed_volume 固定手数
    CtpDemoStrategy(const std::string& name, bool live = false,
                    int fast = 10, int slow = 30, double fixed_volume = 1.0)
        : BaseStrategy(name), live_(live), fast_(fast), slow_(slow), fixed_volume_(fixed_volume),
          // K 线合成器：tick -> 1 分钟 Bar，收口后转发给本策略 on_bar（信号核心）
          bg_(std::bind(&CtpDemoStrategy::on_bar, this, std::placeholders::_1)) {}

    // 初始化回调：打印 live/fast/slow 配置，核对是否处于 dry-run
    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " 初始化 live=" + std::to_string(live_) +
                    " fast=" + std::to_string(fast_) + " slow=" + std::to_string(slow_));
    }

    // tick 回调：逐笔喂给 BarGenerator，跨分钟时其内部收口 1 分钟 Bar 并同步回调 on_bar。
    // 合成口径见 bar_generator.hpp：开高低收/成交量(累计差分)/持仓量，无需策略自己维护。
    void on_tick(const TickData& tk) override {
        bg_.update_tick(tk);
    }

    // K线回调（信号核心）：维护收盘价滑窗，慢线窗口填满后计算快慢均线，
    // 金叉开多/死叉平多；live=true 走真实限价单，否则 [DRY] 仅打印信号。
    void on_bar(const BarData& bar) override {
        closes_.push_back(bar.close);
        if ((int)closes_.size() > slow_ + 5) closes_.pop_front();
        if ((int)closes_.size() < slow_) return;

        double fast_ma = ma(fast_);
        double slow_ma = ma(slow_);

        if (pos_ == 0 && fast_ma > slow_ma) {
            if (live_) buy(bar.vt_symbol, bar.close, fixed_volume_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [DRY] 金叉 BUY " + bar.vt_symbol +
                             " close=" + std::to_string(bar.close));
        } else if (pos_ > 0 && fast_ma < slow_ma) {
            if (live_) sell(bar.vt_symbol, bar.close, fixed_volume_, OrderType::LIMIT);
            else Logger::log(Logger::Level::INFO, name() + " [DRY] 死叉 SELL " + bar.vt_symbol +
                             " close=" + std::to_string(bar.close));
        }
    }

    // 成交回调：更新自维护持仓 pos_ 并打印成交明细（与 double_ma 一致）
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
        int cnt = std::min(n, (int)closes_.size());
        double sum = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return cnt ? sum / cnt : 0.0;
    }

    bool live_;
    int fast_, slow_;
    double fixed_volume_;
    std::deque<double> closes_;
    double pos_ = 0.0;
    BarGenerator bg_;   // vnpy 风格 K 线合成器：tick -> 1 分钟 Bar
};

} // namespace ltc
