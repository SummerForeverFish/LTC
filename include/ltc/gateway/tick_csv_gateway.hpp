// tick_csv_gateway.hpp - Tick 直喂/回放网关（零依赖，可直接运行）
//
// 职责：读取本地 tick CSV，按设定速度回放并「直接」推送 TickData（不经 K线聚合）；
//       内置撮合引擎在 tick 级别以最新成交价撮合挂单。
// 适用：加密货币/期货的 tick 级回放、做市/高频策略离线验证。
// 与 BaseGateway：继承 BaseGateway，复用 on_tick/on_order/on_trade/on_account 推送。
// 与 backtest 回放区别：backtest 由 BacktestEngine 统一回放并撮合、按事件时钟驱动；
//       本网关是独立 Gateway，自带回放线程与撮合，更接近「模拟实盘」接口语义。
// 已知限制：撮合以 tick.last_price 为基准、滑点仅简单加减、无手续费/保证金模型；
//           不推送 on_contract（无合约元数据）。
#pragma once
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <cctype>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// Tick 直喂网关：加载 CSV -> 后台线程逐笔推 Tick + tick 级撮合挂单
class TickCsvGateway : public BaseGateway {
public:
    TickCsvGateway(EventEngine* ee, const std::string& name = "TICK")
        : BaseGateway(ee, name) {}

    // 连接：解析 settings(file/vt_symbol/speed_ms)，加载 CSV，启动回放线程，推送账户
    void connect(const std::map<std::string, std::string>& settings) override {
        file_ = settings.count("file") ? settings.at("file") : "data/BTCUSDT_tick.csv";
        vt_symbol_ = settings.count("vt_symbol") ? settings.at("vt_symbol") : "";
        speed_ms_ = settings.count("speed_ms") ? std::stoi(settings.at("speed_ms")) : 0;
        if (!load_csv(file_)) { on_log("tick 数据加载失败: " + file_); return; }
        on_log("Tick 直喂接口已连接, 共 " + std::to_string(ticks_.size()) + " 笔 tick" +
               (speed_ms_ > 0 ? (", 速度 " + std::to_string(speed_ms_) + "ms/笔") : ", 全速回放"));
        running_ = true;
        thread_ = std::thread([this]() { replay_loop(); });
        emit_account();
    }

    // 停止回放线程并等待结束
    void close() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        on_log("Tick 直喂接口已关闭");
    }

    // 下单：生成本地 orderid/vt_orderid，登记 SUBMITTED 挂单后推送 on_order
    std::string send_order(const OrderRequest& req) override {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string oid = std::to_string(++order_seq_);
        std::string vt_oid = make_vt_orderid(gateway_name_, oid);
        OrderData o = req.to_order(gateway_name_, oid);
        o.vt_orderid = vt_oid; o.status = Status::SUBMITTED; o.datetime = now_ms();
        pending_[vt_oid] = o;
        on_order(std::move(o));
        return vt_oid;
    }

    // 撤单：查 pending_，命中则置 CANCELLED、推送 on_order 后移除
    void cancel_order(const CancelRequest& req) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = pending_.find(req.vt_orderid);
        if (it != pending_.end()) {
            it->second.status = Status::CANCELLED;
            on_order(it->second);
            pending_.erase(it);
        }
    }

    // 订阅（回放无需真实订阅，仅记录日志）
    void subscribe(const std::vector<std::string>&) override {
        on_log("tick 订阅成功: " + (vt_symbol_.empty() ? "(全部)" : vt_symbol_));
    }

private:
    // 去除字符串两端空白字符
    static std::string trim_cs(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    // 加载 tick CSV：首行表头(小写列名映射)，后续行构造 TickData；
    //   symbol/exchange 缺失时按 BINANCE_USDT 兜底，last_price<=0 的行丢弃
    bool load_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string line; bool first = true; std::map<std::string, int> col;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto parts = split_csv(line);
            if (first) {
                for (int i = 0; i < (int)parts.size(); ++i) {
                    std::string h = trim_cs(parts[i]);
                    for (char& c : h) c = (char)std::tolower((unsigned char)c);
                    col[h] = i;
                }
                first = false; continue;
            }
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
            tk.datetime = getdt("datetime", now_ms());
            tk.symbol = gets("symbol", "");
            std::string exs = gets("exchange", "BINANCE_USDT");
            tk.exchange = exchange_from_str(exs);
            tk.vt_symbol = vt_symbol_.empty()
                ? (tk.symbol.empty() ? "" : tk.symbol + "." + exs)
                : vt_symbol_;
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
        return !ticks_.empty();
    }

    // 回放主循环：先用本笔 tick 撮合前序挂单(无未来函数)，再直推 on_tick，发账户快照
    void replay_loop() {
        for (size_t i = 0; i < ticks_.size() && running_; ++i) {
            // 撮合前序挂单（用本笔 tick 价格，下一笔产生的挂单从下笔起撮合 -> 无未来函数）
            match_pending(ticks_[i]);
            // 直喂 tick 给策略
            on_tick(ticks_[i]);
            emit_account();
            if (speed_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(speed_ms_));
        }
        on_log("tick 回放结束, 共 " + std::to_string(ticks_.size()) + " 笔");
    }

    // tick 级撮合：以 tick.last_price 为基准价
    //   市价单立即成交(多+滑点/空-滑点)；限价多/空分别在 last<=price / last>=price 成交
    //   成交后生成 TradeData、置 ALLTRADED、推送 on_order/on_trade 并移除
    void match_pending(const TickData& tk) {
        std::lock_guard<std::mutex> lk(mtx_);
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
            td.vt_tradeid = make_vt_orderid(gateway_name_, td.tradeid);
            td.direction = o.direction; td.offset = o.offset;
            td.price = fp; td.volume = o.volume;
            td.datetime = tk.datetime; td.gateway_name = gateway_name_;
            o.status = Status::ALLTRADED; o.traded = o.volume; o.price = fp;
            on_order(o);
            on_trade(std::move(td));
            pending_.erase(it);
        }
    }

    // 推送模拟账户快照（固定 PAPER 账户与初始资金）
    void emit_account() {
        AccountData a; a.accountid = "PAPER"; a.balance = capital_; a.gateway_name = gateway_name_;
        on_account(std::move(a));
    }

    std::string file_;             // tick CSV 路径
    std::string vt_symbol_;        // 统一标的(为空则用每行 symbol.exchange)
    int speed_ms_ = 0;             // 每笔回放间隔(ms)，0=全速
    std::vector<TickData> ticks_;  // 已加载的 tick 序列
    std::map<std::string, OrderData> pending_; // vt_orderid -> 挂单(撮合状态机)
    std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int order_seq_ = 0, trade_seq_ = 0;
    double capital_ = 1'000'000.0; // 模拟初始资金
    double slippage_ = 0.0;        // tick 级市价单滑点
};

} // namespace ltc
