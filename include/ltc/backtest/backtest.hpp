// backtest.hpp - 回测引擎 + 撮合 (vnpy BacktestingEngine 风格, 事件驱动, 无 UI)
//
// 模块职责：K线(bar)级回测引擎。载入历史 CSV，逐根 K 线驱动策略 on_bar，
//           并在每根 K 线开盘时撮合上一根产生的挂单，估算资金曲线与绩效指标。
// 关键设计：
//   - 回测走「直连回调」路径：引擎直接调用 strategy->on_bar / on_order / on_trade，
//     不经 EventEngine 事件队列，因此确定性强、无并发、便于复现。
//   - 撮合采用 next-bar 成交模型：本根 K 线只撮合上一根留下的挂单，
//     用当前 K 线(open/low/high)判断成交，天然避免「未来函数」。
//   - 资金与持仓在引擎内自行维护（cash_ / long_vol_ / short_vol_ 等），
//     手续费按成交金额比例、滑点按市价不利方向施加。
// 与框架关系：
//   - 实现 OrderRouter 接口，被策略通过 set_order_router 注入，接管 buy/sell 下单。
//   - 与 TickBacktestEngine 平行（后者驱动 on_tick）；二者都只是回测，区别于
//     实盘的 EventEngine（事件队列 + 独立消费者线程）路径。
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

namespace ltc {

// 回测引擎：实现 OrderRouter，驱动策略并模拟撮合成交
class BacktestEngine : public OrderRouter {
public:
    BacktestEngine() = default;

    // 载入 CSV 历史 K 线。
    // 参数：path 数据路径；vt_symbol 合约标识(symbol.exchange)；interval K线周期；
    //       has_header 首行是否为列名。
    // 实现：逐行 split_csv；首行(若有表头)按「去空格+小写」建立列名->下标映射；
    //       之后每行填充 BarData：缺失列用默认值，datetime 缺失则按上一根+60000ms 递推；
    //       用 parse_vt_symbol 解析 symbol/exchange；open<=0 的行丢弃(过滤脏数据)。
    // 返回：是否成功载入至少一根 K 线。
    bool load_csv(const std::string& path, const std::string& vt_symbol,
                  Interval interval = Interval::MINUTE, bool has_header = true) {
        std::ifstream f(path);
        if (!f.is_open()) { Logger::log(Logger::Level::ERR, "无法打开数据文件: " + path); return false; }
        std::string line;
        std::map<std::string, int> col;
        bool first = true;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto parts = split_csv(line);
            if (first && has_header) {
                for (int i = 0; i < (int)parts.size(); ++i) {
                    std::string h = parts[i];
                    // 去空格小写
                    h.erase(0, h.find_first_not_of(" \t"));
                    h.erase(h.find_last_not_of(" \t") + 1);
                    for (char& c : h) c = (char)std::tolower((unsigned char)c);
                    col[h] = i;
                }
                first = false;
                continue;
            }
            first = false;
            BarData bar;
            bar.vt_symbol = vt_symbol;
            // 解析 vt_symbol
            BaseStrategy::parse_vt_symbol(vt_symbol, bar.symbol, bar.exchange);
            bar.interval = interval;
            auto get = [&](const std::string& name, double def) -> double {
                auto it = col.find(name);
                if (it == col.end() || it->second >= (int)parts.size()) return def;
                return to_double(parts[it->second]);
            };
            auto get_dt = [&](const std::string& name, int64_t def) -> int64_t {
                auto it = col.find(name);
                if (it == col.end() || it->second >= (int)parts.size()) return def;
                return parse_datetime(parts[it->second]);
            };
            bar.datetime = get_dt("datetime", bars_.empty() ? 0 : bars_.back().datetime + 60000);
            bar.open = get("open", 0); bar.high = get("high", 0);
            bar.low = get("low", 0); bar.close = get("close", 0);
            bar.volume = get("volume", 0); bar.open_interest = get("open_interest", 0);
            if (bar.open > 0) bars_.push_back(bar);
        }
        Logger::log(Logger::Level::INFO, "载入 " + std::to_string(bars_.size()) + " 根K线: " + vt_symbol);
        return !bars_.empty();
    }

    void add_strategy(std::shared_ptr<BaseStrategy> st) { strategy_ = st; }
    void set_capital(double c) { capital_ = c; }
    void set_commission(double rate) { commission_rate_ = rate; }
    void set_slippage(double s) { slippage_ = s; }         // 每笔市价滑点(价格单位)
    void set_size(double s) { size_ = s; }                 // 合约乘数
    void set_annualization(int n) { annualization_ = n; }  // 年化因子(日线=252)

    const std::vector<std::pair<int64_t, double>>& equity_curve() const { return equity_curve_; }

    // 运行回测主循环（确定性、直连回调、不经事件队列）。
    // 步骤：注入 OrderRouter -> 调用策略 on_init/on_start 初始化；
    //       初始化现金=capital_、清空持仓与权益曲线；
    //       逐根 K 线：① 先用本根行情撮合上一根遗留的挂单(match_pending)，
    //                  ② 推送给策略 on_bar，③ 以 close 估算权益并记入曲线、计算收益率。
    //       结束后调用 on_stop 并打印绩效。next-bar 撮合模型天然避免未来函数。
    void run() {
        if (!strategy_ || bars_.empty()) return;
        strategy_->set_order_router(this);
        // 回测直接调用回调（确定性、无独立事件线程）
        strategy_->on_init();
        strategy_->on_start();

        cash_ = capital_;
        long_vol_ = short_vol_ = 0.0;
        long_price_ = short_price_ = 0.0;
        order_seq_ = 0;
        trade_count_ = 0;
        equity_curve_.clear();

        double prev_equity = capital_;
        for (size_t i = 0; i < bars_.size(); ++i) {
            const BarData& bar = bars_[i];
            // 1) 用本根K线撮合上一根产生的挂单（next-bar 成交，避免未来函数）
            match_pending(bar);
            // 2) 推送给策略
            strategy_->on_bar(bar);
            // 3) 记录权益
            double eq = equity(bar.close);
            equity_curve_.push_back({bar.datetime, eq});
            double ret = (eq - prev_equity) / (prev_equity > 0 ? prev_equity : 1.0);
            returns_.push_back(ret);
            prev_equity = eq;
        }
        strategy_->on_stop();
        print_stats();
    }

    // ---- OrderRouter 实现 ----

    // 接收策略发来的委托：生成本地委托号、登记为 pending(等待下一根 K 线撮合)，
    // 并立即以 SUBMITTED 状态回调策略 on_order。回测中不走事件队列，故同步直调。
    std::string send_order(const OrderRequest& req) override {
        std::string oid = std::to_string(++order_seq_);
        std::string vt_oid = make_vt_orderid("BACKTEST", oid);
        OrderData o = req.to_order("BACKTEST", oid);
        o.vt_orderid = vt_oid;
        o.status = Status::SUBMITTED;
        o.datetime = bars_.empty() ? now_ms() : bars_.back().datetime;
        pending_[vt_oid] = o;
        // 回测中不通过事件线程，直接回调
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
    // 实时权益：现金 + 净持仓(多-空) * 最新价 * 合约乘数。
    double equity(double last_price) const {
        return cash_ + (long_vol_ - short_vol_) * last_price * size_;
    }

    // 撮合上一根 K 线遗留的挂单（next-bar 成交模型）。
    // 限价单：多头看 bar.low 是否 <= 限价(被击穿即成交)，空头看 bar.high >= 限价，
    //         成交价取委托限价(无滑点)；市价单：以 bar.open 成交并叠加不利方向滑点。
    // 注意用「上一根」的挂单 + 「本根」的行情撮合，天然避免未来函数。
    void match_pending(const BarData& bar) {
        std::vector<std::string> keys;
        for (auto& kv : pending_) keys.push_back(kv.first);
        for (auto& key : keys) {
            auto it = pending_.find(key);
            if (it == pending_.end()) continue;
            OrderData& o = it->second;
            bool filled = false;
            double fill_price = o.price;
            if (o.type == OrderType::MARKET) {
                filled = true;
                fill_price = bar.open;
                // 市价滑点（不利方向）
                if (o.direction == Direction::LONG) fill_price += slippage_;
                else fill_price -= slippage_;
            } else if (o.type == OrderType::LIMIT) {
                if (o.direction == Direction::LONG && bar.low <= o.price) { filled = true; fill_price = o.price; }
                else if (o.direction == Direction::SHORT && bar.high >= o.price) { filled = true; fill_price = o.price; }
            }
            if (!filled) continue;

            // 成交
            TradeData td;
            td.vt_symbol = o.vt_symbol;
            td.symbol = o.symbol; td.exchange = o.exchange;
            td.orderid = o.orderid; td.vt_orderid = o.vt_orderid;
            td.tradeid = std::to_string(++trade_seq_);
            td.vt_tradeid = make_vt_orderid("BACKTEST", td.tradeid);
            td.direction = o.direction; td.offset = o.offset;
            td.price = fill_price; td.volume = o.volume;
            td.datetime = bar.datetime; td.gateway_name = "BACKTEST";

            apply_trade(td);
            o.status = Status::ALLTRADED; o.traded = o.volume; o.price = fill_price;
            strategy_->on_order(o);
            strategy_->on_trade(td);
            pending_.erase(it);
            ++trade_count_;
        }
    }

    // 将一笔成交落实到账户：扣手续费；开仓更新持仓量与持仓均价(加权平均)，
    // 买入扣资金、卖空收资金；平仓按持仓均价计算已实现盈亏并入现金。
    // 用 min(volume,持仓) 防止超平，浮点接近 0 时清零避免残差。
    void apply_trade(const TradeData& td) {
        double notional = td.price * td.volume * size_;
        double commission = notional * commission_rate_;
        cash_ -= commission;
        if (td.direction == Direction::LONG && td.offset == Offset::OPEN) {
            double newvol = long_vol_ + td.volume;
            long_price_ = newvol > 0 ? (long_price_ * long_vol_ + td.price * td.volume) / newvol : 0;
            long_vol_ = newvol;
            cash_ -= notional; // 买入占用资金
        } else if (td.direction == Direction::SHORT && td.offset == Offset::OPEN) {
            double newvol = short_vol_ + td.volume;
            short_price_ = newvol > 0 ? (short_price_ * short_vol_ + td.price * td.volume) / newvol : 0;
            short_vol_ = newvol;
            cash_ += notional; // 卖空收到资金
        } else if (td.direction == Direction::LONG && td.offset == Offset::CLOSE) {
            // 平空
            double v = std::min(td.volume, short_vol_);
            cash_ -= td.price * v * size_;
            double realized = (short_price_ - td.price) * v * size_;
            cash_ += realized;
            short_vol_ -= v;
            if (short_vol_ <= 1e-9) short_vol_ = 0;
        } else if (td.direction == Direction::SHORT && td.offset == Offset::CLOSE) {
            // 平多
            double v = std::min(td.volume, long_vol_);
            double realized = (td.price - long_price_) * v * size_;
            cash_ += realized;
            long_vol_ -= v;
            if (long_vol_ <= 1e-9) long_vol_ = 0;
        }
    }

    // 回测结束打印绩效：初始/最终权益、总收益率、成交笔数、年化 Sharpe、期末持仓。
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
        oss << "\n========== 回测结果 ==========\n";
        oss << "初始资金    : " << std::fixed << std::setprecision(2) << capital_ << "\n";
        oss << "最终权益    : " << final_eq << "\n";
        oss << "总收益率    : " << total_ret << " %\n";
        oss << "成交笔数    : " << trade_count_ << "\n";
        oss << "年化 Sharpe : " << std::setprecision(3) << sharpe << "\n";
        oss << "期末持仓    : 多=" << long_vol_ << " 空=" << short_vol_ << "\n";
        oss << "===============================\n";
        Logger::log(Logger::Level::INFO, oss.str());
    }

    std::vector<BarData> bars_;
    std::shared_ptr<BaseStrategy> strategy_;
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
