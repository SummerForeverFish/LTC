// tick_backtest.hpp - tick 级回测引擎 (直喂 on_tick, tick 级撮合)
//
// 模块职责：tick 级回测引擎。载入逐笔 tick，驱动策略 on_tick，并在每笔 tick 以
//           last_price 为基准撮合上一笔 tick 产生的挂单，估算资金曲线与绩效指标。
// 关键设计：
//   - 与 BacktestEngine 平行：此处驱动 on_tick 而非 on_bar，撮合基准为 tick.last_price，
//     订单在「下一笔 tick」撮合(避免未来函数)，适合验证 tick 级/高频策略。
//   - 同样走直连回调、确定性、无并发，不经 EventEngine 事件队列。
//   - 限价按 last_price 穿越限价成交(取委托价)，市价按 last_price+不利方向滑点成交。
// 与框架关系：实现 OrderRouter，被策略 set_order_router 注入；与 BacktestEngine 同属
//           回测路径，区别于实盘 EventEngine(事件队列+独立消费者线程)路径。
// 与 BacktestEngine 平行：这里驱动策略 on_tick 而非 on_bar，撮合基准为 tick.last_price，
// 订单在「下一笔 tick」撮合（避免未来函数），适合验证 tick 级/高频策略。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <cctype>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"
#include "ltc/core/bar_generator.hpp"

namespace ltc {

class TickBacktestEngine : public OrderRouter {
public:
    TickBacktestEngine() = default;

    // 载入 CSV tick 数据。
    // 参数：path 数据路径；vt_symbol 合约标识(为空时每行按 symbol.exchange 自动生成)；
    //       has_header 首行是否为列名。
    // 实现：首行建「小写列名->下标」映射；之后每行填充 TickData，datetime 缺失则按
    //       上一笔+500ms 递推，exchange 用 exchange_from_str 解析；last_price<=0 的行丢弃。
    // 支持列：datetime,symbol,exchange,last_price,last_volume,bid/ask 一二档,open_interest,
    //         volume,limit_up,limit_down。
    // 返回：是否成功载入至少一笔 tick。
    bool load_tick_csv(const std::string& path, const std::string& vt_symbol = "",
                       bool has_header = true) {
        std::ifstream f(path);
        if (!f.is_open()) { Logger::log(Logger::Level::ERR, "无法打开 tick 数据文件: " + path); return false; }
        std::string line; std::map<std::string, int> col; bool first = true;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto parts = split_csv(line);
            if (first && has_header) {
                for (int i = 0; i < (int)parts.size(); ++i) {
                    std::string h = parts[i];
                    h.erase(0, h.find_first_not_of(" \t"));
                    h.erase(h.find_last_not_of(" \t") + 1);
                    for (char& c : h) c = (char)std::tolower((unsigned char)c);
                    col[h] = i;
                }
                first = false; continue;
            }
            first = false;
            TickData tk;
            auto get = [&](const std::string& n, double d) {
                auto it = col.find(n); return (it == col.end() || it->second >= (int)parts.size()) ? d : to_double(parts[it->second]);
            };
            auto gets = [&](const std::string& n, const std::string& d) {
                auto it = col.find(n); return (it == col.end() || it->second >= (int)parts.size()) ? d : trim_cs(parts[it->second]);
            };
            auto getdt = [&](const std::string& n, int64_t d) {
                auto it = col.find(n); return (it == col.end() || it->second >= (int)parts.size()) ? d : parse_datetime(parts[it->second]);
            };
            tk.datetime = getdt("datetime", ticks_.empty() ? 0 : ticks_.back().datetime + 500);
            tk.symbol = gets("symbol", "");
            std::string exs = gets("exchange", "BINANCE_USDT");
            tk.exchange = exchange_from_str(exs);
            tk.vt_symbol = vt_symbol.empty()
                ? (tk.symbol.empty() ? "" : tk.symbol + "." + exs)
                : vt_symbol;
            tk.last_price = get("last_price", 0);
            tk.last_volume = get("last_volume", 0);
            tk.bid_price_1 = get("bid_price_1", 0);
            tk.bid_volume_1 = get("bid_volume_1", 0);
            tk.ask_price_1 = get("ask_price_1", 0);
            tk.ask_volume_1 = get("ask_volume_1", 0);
            tk.open_interest = get("open_interest", 0);
            tk.volume = get("volume", 0);
            tk.limit_up = get("limit_up", 0);
            tk.limit_down = get("limit_down", 0);
            if (tk.last_price > 0) ticks_.push_back(tk);
        }
        Logger::log(Logger::Level::INFO, "载入 " + std::to_string(ticks_.size()) + " 笔 tick: " +
                    (vt_symbol.empty() ? "(按行)" : vt_symbol));
        return !ticks_.empty();
    }

    void add_strategy(std::shared_ptr<BaseStrategy> st) { strategy_ = st; }

    // 启用 K 线合成：把逐笔 tick 用 BarGenerator 聚合为指定周期 K 线，
    // 收口后回调策略 on_bar（策略同时仍会收到每笔 on_tick）。
    //   - Interval::MINUTE   ：策略收到 1 分钟 K（tick 直接合成）；
    //   - MINUTE3/5/15、HOUR、HOUR4、DAILY：策略收到对应目标周期的合成 K；
    //   - Interval::NONE     ：关闭合成（默认），策略只收 tick。
    // 便于在 tick 回测上验证 K 线策略，不必先离线把 tick 转成 K 线 CSV。
    void set_bar_interval(Interval itv) {
        if (itv == Interval::NONE) { bg_.reset(); return; }
        bar_interval_ = itv;
        bg_.reset(new BarGenerator(
            // 1 分钟 K 收口：目标即 1 分钟则直接给策略，否则接力进窗口聚合
            [this](const BarData& b) {
                if (bar_interval_ == Interval::MINUTE) strategy_->on_bar(b);
                else bg_->update_bar(b);
            },
            itv,
            // 目标周期 K 收口：推给策略 on_bar
            [this](const BarData& b) { strategy_->on_bar(b); }));
    }

    void set_capital(double c) { capital_ = c; }
    void set_commission(double rate) { commission_rate_ = rate; }
    void set_slippage(double s) { slippage_ = s; }
    void set_size(double s) { size_ = s; }
    void set_annualization(int n) { annualization_ = n; }

    const std::vector<std::pair<int64_t, double>>& equity_curve() const { return equity_curve_; }

    // 运行 tick 回测：直连回调驱动策略 on_tick(不经事件队列)，每笔 tick 先撮合上一笔
    // 遗留挂单、再喂 tick、再记录权益。确定性、无并发、可复现。
    void run() {
        if (!strategy_ || ticks_.empty()) return;
        strategy_->set_order_router(this);
        strategy_->on_init();
        strategy_->on_start();

        cash_ = capital_; long_vol_ = short_vol_ = 0.0;
        long_price_ = short_price_ = 0.0;
        order_seq_ = 0; trade_count_ = 0;
        equity_curve_.clear(); returns_.clear();

        for (size_t i = 0; i < ticks_.size(); ++i) {
            const TickData& tk = ticks_[i];
            // 1) 撮合上一笔 tick 产生的挂单（下一笔成交，无未来函数）
            match_pending(tk);
            // 2) 直喂 tick 给策略
            strategy_->on_tick(tk);
            // 3) K 线合成（若启用 set_bar_interval）：跨分钟收口时同步回调 on_bar
            if (bg_) bg_->update_tick(tk);
            // 4) 记录权益
            double eq = equity(tk.last_price);
            equity_curve_.push_back({tk.datetime, eq});
            if (i > 0) {
                double prev = equity_curve_[i - 1].second;
                double ret = (eq - prev) / (prev > 0 ? prev : 1.0);
                returns_.push_back(ret);
            }
        }
        // 收口仍在合成中的最后一根 K 线，再结束策略
        if (bg_) bg_->finish();
        strategy_->on_stop();
        print_stats();
    }

    // ---- OrderRouter 实现 ----

    // 接收策略委托：生成 TICKBACK 本地委托号、登记 pending、立即以 SUBMITTED 回调 on_order。
    // 回测中不走事件队列，同步直调。
    std::string send_order(const OrderRequest& req) override {
        std::string oid = std::to_string(++order_seq_);
        std::string vt_oid = make_vt_orderid("TICKBACK", oid);
        OrderData o = req.to_order("TICKBACK", oid);
        o.vt_orderid = vt_oid;
        o.status = Status::SUBMITTED;
        o.datetime = ticks_.empty() ? now_ms() : ticks_.back().datetime;
        pending_[vt_oid] = o;
        strategy_->on_order(o);
        return vt_oid;
    }
    void cancel_order(const CancelRequest& req) override {
        auto it = pending_.find(req.vt_orderid);
        if (it != pending_.end()) {
            it->second.status = Status::CANCELLED;
            strategy_->on_order(it->second);
            pending_.erase(it);
        }
    }

private:
    static std::string trim_cs(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    // 实时权益：现金 + 净持仓(多-空) * 最新价 * 合约乘数。
    double equity(double last_price) const {
        return cash_ + (long_vol_ - short_vol_) * last_price * size_;
    }

    // 撮合上一笔 tick 遗留的挂单（next-tick 成交，无未来函数）。
    // 基准价为 tk.last_price：
    //   - 限价单：多头看 last_price <= 限价(价格回落穿透)、空头看 last_price >= 限价，
    //     成交价取委托限价；
    //   - 市价单：直接以 last_price 成交，并按方向叠加不利方向滑点。
    void match_pending(const TickData& tk) {
        std::vector<std::string> keys;
        for (auto& kv : pending_) keys.push_back(kv.first);
        for (auto& key : keys) {
            auto it = pending_.find(key);
            if (it == pending_.end()) continue;
            OrderData& o = it->second;
            bool filled = false; double fp = o.price;
            double ref = tk.last_price;
            if (o.type == OrderType::MARKET) {
                filled = true; fp = ref;
                if (o.direction == Direction::LONG) fp += slippage_;
                else fp -= slippage_;
            } else if (o.type == OrderType::LIMIT) {
                if (o.direction == Direction::LONG && ref <= o.price) { filled = true; fp = o.price; }
                else if (o.direction == Direction::SHORT && ref >= o.price) { filled = true; fp = o.price; }
            }
            if (!filled) continue;

            TradeData td;
            td.vt_symbol = o.vt_symbol; td.symbol = o.symbol; td.exchange = o.exchange;
            td.orderid = o.orderid; td.vt_orderid = o.vt_orderid;
            td.tradeid = std::to_string(++trade_seq_);
            td.vt_tradeid = make_vt_orderid("TICKBACK", td.tradeid);
            td.direction = o.direction; td.offset = o.offset;
            td.price = fp; td.volume = o.volume;
            td.datetime = tk.datetime; td.gateway_name = "TICKBACK";

            apply_trade(td);
            o.status = Status::ALLTRADED; o.traded = o.volume; o.price = fp;
            strategy_->on_order(o);
            strategy_->on_trade(td);
            pending_.erase(it);
            ++trade_count_;
        }
    }

    // 将一笔成交落实到账户(与 BacktestEngine::apply_trade 逻辑一致)：扣手续费、开仓更新
    // 持仓量与持仓均价、平仓按均价计算已实现盈亏并入现金；min(volume,持仓) 防超平，残值清零。
    void apply_trade(const TradeData& td) {
        double notional = td.price * td.volume * size_;
        double commission = notional * commission_rate_;
        cash_ -= commission;
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN) {
            double newvol = long_vol_ + td.volume;
            long_price_ = newvol > 0 ? (long_price_ * long_vol_ + td.price * td.volume) / newvol : 0;
            long_vol_ = newvol;
            cash_ -= notional;
        } else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN) {
            double newvol = short_vol_ + td.volume;
            short_price_ = newvol > 0 ? (short_price_ * short_vol_ + td.price * td.volume) / newvol : 0;
            short_vol_ = newvol;
            cash_ += notional;
        } else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE) {
            double v = std::min(td.volume, short_vol_);
            cash_ -= td.price * v * size_;
            double realized = (short_price_ - td.price) * v * size_;
            cash_ += realized;
            short_vol_ -= v;
            if (short_vol_ <= 1e-9) short_vol_ = 0;
        } else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE) {
            double v = std::min(td.volume, long_vol_);
            double realized = (td.price - long_price_) * v * size_;
            cash_ += realized;
            long_vol_ -= v;
            if (long_vol_ <= 1e-9) long_vol_ = 0;
        }
    }

    // 回测结束打印绩效：初始/最终权益、总收益率、成交笔数、tick 总数、年化 Sharpe、期末持仓。
    void print_stats() {
        double final_eq = equity_curve_.empty() ? capital_ : equity_curve_.back().second;
        double total_ret = (final_eq - capital_) / capital_ * 100.0;
        double mean = 0, var = 0;
        for (double r : returns_) mean += r;
        mean /= returns_.size() > 0 ? returns_.size() : 1;
        for (double r : returns_) var += (r - mean) * (r - mean);
        double std = std::sqrt(var / (returns_.size() > 0 ? returns_.size() : 1));
        double sharpe = (std > 0) ? (mean / std) * std::sqrt((double)annualization_) : 0.0;

        std::ostringstream oss;
        oss << "\n========== Tick 回测结果 ==========\n";
        oss << "初始资金    : " << std::fixed << std::setprecision(2) << capital_ << "\n";
        oss << "最终权益    : " << final_eq << "\n";
        oss << "总收益率    : " << total_ret << " %\n";
        oss << "成交笔数    : " << trade_count_ << "\n";
        oss << "tick 总数   : " << ticks_.size() << "\n";
        oss << "年化 Sharpe : " << std::setprecision(3) << sharpe << "\n";
        oss << "期末持仓    : 多=" << long_vol_ << " 空=" << short_vol_ << "\n";
        oss << "===================================\n";
        Logger::log(Logger::Level::INFO, oss.str());
    }

    std::vector<TickData> ticks_;
    std::shared_ptr<BaseStrategy> strategy_;
    std::unique_ptr<BarGenerator> bg_;   // K 线合成器（set_bar_interval 启用）
    Interval bar_interval_ = Interval::NONE; // 当前合成目标周期
    std::map<std::string, OrderData> pending_;
    int order_seq_ = 0, trade_seq_ = 0;
    double capital_ = 1'000'000.0;
    double commission_rate_ = 0.0004;
    double slippage_ = 0.0;
    double size_ = 1.0;
    int annualization_ = 252;
    double cash_ = 0, long_vol_ = 0, short_vol_ = 0, long_price_ = 0, short_price_ = 0;
    int trade_count_ = 0;
    std::vector<std::pair<int64_t, double>> equity_curve_;
    std::vector<double> returns_;
};

} // namespace ltc
