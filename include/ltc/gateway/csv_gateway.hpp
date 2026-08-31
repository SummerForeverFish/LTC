// csv_gateway.hpp - CSV K线回放 / 模拟撮合网关（零依赖，可直接运行）
//
// 职责：读取本地 K线 CSV，按设定速度逐根回放并推送 Bar 事件；内置撮合引擎在每根
//       K线到达时对挂单做本地撮合（下一根 K线成交），并推送 Order/Trade。
// 适用：加密货币/期货的分钟级回放、paper trading、策略离线验证。
// 与 BaseGateway：继承 BaseGateway，复用 on_bar/on_order/on_trade/on_account 推送。
// 已知限制：撮合每根 K线仅触发一次、滑点/手续费未建模；行情为离线回放无实时性；
//           不查询合约表，on_contract 不推送（无 symbol 元数据）。
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

// CSV 回放网关：加载 CSV -> 后台线程逐根推 Bar + 撮合挂单
class CsvReplayGateway : public BaseGateway {
public:
    CsvReplayGateway(EventEngine* ee, const std::string& name = "CSV")
        : BaseGateway(ee, name) {}

    // 连接：解析 settings(file/vt_symbol/speed_ms)，加载 CSV，启动回放线程，推送账户
    void connect(const std::map<std::string, std::string>& settings) override {
        file_ = settings.count("file") ? settings.at("file") : "data/BTCUSDT.csv";
        vt_symbol_ = settings.count("vt_symbol") ? settings.at("vt_symbol") : "BTCUSDT.BINANCE_USDT";
        speed_ms_ = settings.count("speed_ms") ? std::stoi(settings.at("speed_ms")) : 200;
        interval_ = Interval::MINUTE;
        if (!load_csv(file_)) { on_log("数据加载失败"); return; }
        on_log("CSV 回放接口已连接, 共 " + std::to_string(bars_.size()) + " 根K线, 速度 " +
               std::to_string(speed_ms_) + "ms/根");
        running_ = true;
        thread_ = std::thread([this]() { replay_loop(); });
        emit_account();
    }

    // 停止回放线程并等待结束
    void close() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        on_log("CSV 回放接口已关闭");
    }

    // 下单：生成本地 orderid/vt_orderid，登记为 SUBMITTED 挂单，推送 on_order 返回 vt_oid
    std::string send_order(const OrderRequest& req) override {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string oid = std::to_string(++order_seq_);
        std::string vt_oid = make_vt_orderid(gateway_name_, oid);
        OrderData o = req.to_order(gateway_name_, oid);
        o.vt_orderid = vt_oid; o.status = Status::SUBMITTED;
        o.datetime = now_ms();
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
        on_log("订阅成功: " + vt_symbol_);
    }

private:
    // 加载 CSV：首行解析表头(小写列名映射)，之后行构造 BarData；
    //   缺列回退默认值，open<=0 的行丢弃。日期缺失时按上一根 +60s 推算。
    bool load_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string line; bool first = true; std::map<std::string,int> col;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto parts = split_csv(line);
            if (first) {
                for (int i = 0; i < (int)parts.size(); ++i) {
                    std::string h = parts[i];
                    h.erase(0, h.find_first_not_of(" \t"));
                    h.erase(h.find_last_not_of(" \t") + 1);
                    for (char& c : h) c = (char)std::tolower((unsigned char)c);
                    col[h] = i;
                }
                first = false; continue;
            }
            BarData b; b.vt_symbol = vt_symbol_;
            BaseStrategy::parse_vt_symbol(vt_symbol_, b.symbol, b.exchange);
            b.interval = interval_;
            auto get = [&](const std::string& n, double d) {
                auto it = col.find(n); return (it==col.end()||it->second>=(int)parts.size())?d:to_double(parts[it->second]);
            };
            auto getdt = [&](const std::string& n, int64_t d) {
                auto it = col.find(n); return (it==col.end()||it->second>=(int)parts.size())?d:parse_datetime(parts[it->second]);
            };
            b.datetime = getdt("datetime", bars_.empty()?0:bars_.back().datetime+60000);
            b.open=get("open",0); b.high=get("high",0); b.low=get("low",0); b.close=get("close",0);
            b.volume=get("volume",0); b.open_interest=get("open_interest",0);
            if (b.open>0) bars_.push_back(b);
        }
        return !bars_.empty();
    }

    // 回放主循环：先撮合上根遗留挂单 -> 推本根 Bar -> 发账户快照 -> 按 speed_ms 节流
    void replay_loop() {
        for (size_t i = 0; i < bars_.size() && running_; ++i) {
            // 撮合上一根遗留挂单
            match_pending(bars_[i]);
            // 推送本根K线
            on_bar(bars_[i]);
            emit_account();
            std::this_thread::sleep_for(std::chrono::milliseconds(speed_ms_));
        }
        on_log("回放结束");
    }

    // 撮合挂单状态机：对每笔 pending 订单按本根 Bar 的 OHLC 判定是否成交
    //   市价单立即以 open 成交；限价多/空分别在 bar.low<=price / bar.high>=price 成交
    //   成交后生成 TradeData、置 ALLTRADED、推送 on_order/on_trade 并移除
    void match_pending(const BarData& bar) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::string> keys;
        for (auto& kv : pending_) keys.push_back(kv.first);
        for (auto& key : keys) {
            auto it = pending_.find(key);
            if (it == pending_.end()) continue;
            OrderData& o = it->second;
            bool filled = false; double fp = o.price;
            if (o.type == OrderType::MARKET) { filled = true; fp = bar.open; }
            else if (o.type == OrderType::LIMIT) {
                if (o.direction==Direction::LONG && bar.low<=o.price) { filled=true; fp=o.price; }
                else if (o.direction==Direction::SHORT && bar.high>=o.price) { filled=true; fp=o.price; }
            }
            if (!filled) continue;
            TradeData td;
            td.vt_symbol=o.vt_symbol; td.symbol=o.symbol; td.exchange=o.exchange;
            td.orderid=o.orderid; td.vt_orderid=o.vt_orderid;
            td.tradeid=std::to_string(++trade_seq_); td.vt_tradeid=make_vt_orderid(gateway_name_,td.tradeid);
            td.direction=o.direction; td.offset=o.offset; td.price=fp; td.volume=o.volume;
            td.datetime=bar.datetime; td.gateway_name=gateway_name_;
            o.status=Status::ALLTRADED; o.traded=o.volume; o.price=fp;
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

    std::string file_;            // CSV 文件路径
    std::string vt_symbol_;       // 回放标的
    Interval interval_ = Interval::MINUTE;
    int speed_ms_ = 200;          // 每根 K线回放间隔(ms)
    std::vector<BarData> bars_;   // 已加载的 K线序列
    std::map<std::string, OrderData> pending_; // vt_orderid -> 挂单(撮合状态机)
    std::mutex mtx_;              // 保护 pending_ 的并发访问
    std::atomic<bool> running_{false};
    std::thread thread_;
    int order_seq_ = 0, trade_seq_ = 0;
    double capital_ = 1'000'000.0; // 模拟初始资金
};

} // namespace ltc
