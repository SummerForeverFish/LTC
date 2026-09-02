// bollinger_strategy.hpp - 布林带均值回归示例策略 (可在回测与实盘复用)
//
// 职责：基于 Bar 收盘价维护最近 window 根 K 线的滑窗，计算布林带中轨(mid=SMA)、
//       上轨(upper=mid+k·σ)、下轨(lower=mid−k·σ)，并在价格触及轨道时做均值回归。
//
// 信号逻辑（单仓位、不做金字塔加仓）：
//   - 空仓时，收盘价下穿下轨 -> 开多（赌反弹）；收盘价上穿上轨 -> 开空（赌回落）；
//   - 持多时，收盘价回到中轨上方 -> 平多；
//   - 持空时，收盘价回到中轨下方 -> 平空。
// 适用市场：震荡/均值回归行情（分钟线、日线）；强趋势行情会反复被打脸，需配合过滤。
// 已知限制：
//   1) 仅单笔固定手数 fixed_volume_，无止损/加仓，触轨即反手需先平后开（本策略不反手）；
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

// 布林带 Bar 策略：在 on_bar 中维护收盘价滑窗，计算中/上/下轨并据此开/平仓位
class BollingerStrategy : public BaseStrategy {
public:
    // 构造参数：name 策略名；window 布林窗口周期；k 标准差倍数；fixed_volume 每笔固定手数
    BollingerStrategy(const std::string& name, int window = 20, double k = 2.0,
                      double fixed_volume = 1.0)
        : BaseStrategy(name), window_(window), k_(k), fixed_volume_(fixed_volume) {}

    // 初始化回调：打印配置参数，便于核对 window/k 是否符合预期
    void on_init() override {
        Logger::log(Logger::Level::INFO, name() + " 初始化, window=" + std::to_string(window_) +
                    " k=" + std::to_string(k_) + " vol=" + std::to_string(fixed_volume_));
    }

    // K线回调（策略核心）：维护收盘价滑窗，待窗口填满后计算布林带三轨，
    // 触下轨开多 / 触上轨开空 / 回中轨平仓，成交价取 bar.close 以市价单发出。
    void on_bar(const BarData& bar) override {
        closes_.push_back(bar.close);
        if ((int)closes_.size() > window_ + 5) closes_.pop_front();

        if ((int)closes_.size() < window_) return;

        double mid = sma(window_);
        double sigma = stddev(window_, mid);
        double upper = mid + k_ * sigma;
        double lower = mid - k_ * sigma;

        // 当前策略持仓（持久化自 JSON，重启也能恢复；非账户持仓）
        double pos = get_strategy_pos(bar.vt_symbol).volume;
        pos_ = pos;   // 同步到内存，便于日志

        // 空仓：触下轨开多，触上轨开空
        if (pos == 0) {
            if (bar.close < lower)
                buy(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
            else if (bar.close > upper)
                short_(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
        }
        // 持多：回到中轨上方平多
        else if (pos > 0 && bar.close > mid) {
            sell(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
        }
        // 持空：回到中轨下方平空
        else if (pos < 0 && bar.close < mid) {
            cover(bar.vt_symbol, bar.close, fixed_volume_, OrderType::MARKET);
        }
    }

    // 成交回调：依据成交方向/开平更新自维护持仓 pos_，并打日志便于核对
    void on_trade(const TradeData& td) override {
        double old = pos_;
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN) pos_ += td.volume;
        else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN) pos_ -= td.volume;
        else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE) pos_ += td.volume;   // 平空：买回，净持仓回正
        else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE) pos_ -= td.volume;  // 平多：卖出，净持仓回零
        // 开仓均价：新开仓时记录成交价；平仓归零时清空（本策略不金字塔加仓，加权可省略）
        if (old == 0.0 && pos_ != 0.0) avg_price_ = td.price;
        else if (pos_ == 0.0)            avg_price_ = 0.0;
        // 持久化到 JSON（按策略名隔离，多策略互不覆盖）
        save_position(td.vt_symbol, pos_, avg_price_);
        Logger::log(Logger::Level::INFO, name() + " 成交 " + direction_to_str(td.direction) +
                    " @" + std::to_string(td.price) + " vol=" + std::to_string(td.volume) +
                    " pos=" + std::to_string(pos_) +
                    " avg=" + std::to_string(avg_price_));
    }

    // 委托回调：处理拒单等异常，避免无声失败（此处仅记录，不下单重试）
    void on_order(const OrderData& o) override {
        if (o.status == Status::REJECTED)
            Logger::log(Logger::Level::WARNING, name() + " 委托被拒: " + o.vt_orderid);
    }

private:
    // 内部计算：取收盘价窗口末尾 n 个点的简单移动平均（SMA）
    double sma(int n) const {
        int cnt = std::min(n, (int)closes_.size());
        double sum = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) sum += *it;
        return sum / cnt;
    }

    // 内部计算：取收盘价窗口末尾 n 个点、以给定均值 mu 计算的总体标准差 σ
    double stddev(int n, double mu) const {
        int cnt = std::min(n, (int)closes_.size());
        if (cnt <= 1) return 0.0;
        double acc = 0;
        auto it = closes_.rbegin();
        for (int i = 0; i < cnt; ++i, ++it) {
            double d = *it - mu;
            acc += d * d;
        }
        return std::sqrt(acc / cnt);
    }

    int window_;
    double k_;
    double fixed_volume_;
    std::deque<double> closes_;
    double pos_ = 0.0;
    double avg_price_ = 0.0;   // 开仓均价（持久化到 JSON）
};

} // namespace ltc
