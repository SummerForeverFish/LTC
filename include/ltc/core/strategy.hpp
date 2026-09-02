// strategy.hpp - 策略基类 (vnpy StrategyTemplate 风格)
//
// 职责：用户策略的基类。策略通过重写生命周期与事件回调（on_init/on_bar/on_tick/...）
//       响应行情与成交，并用 buy/sell/short_/cover/cancel 发单/撤单；自身不关心下单落到
//       实盘还是回测——由注入的 OrderRouter 决定。
//
// 关键设计：
//   - 策略参数不在本类解析：注册时由 StrategyRegistry 的 creator lambda 通过 parse_params
//     把 "k=v" 串解析成 StrategyParams，于构造期传入（保持本类零依赖配置格式，也对应
//     规范中的 get_param 辅助读取）。
//   - 引擎在 start() 中通过 set_event_engine/set_order_router 注入依赖，再注册事件回调。
//   - 四个交易辅助函数统一归口到 protected send()，只负责装配 OrderRequest 各字段。
//
// 与其他模块关系：
//   - 被 MainEngine 持有、由 EventEngine 分发 on_xxx 虚函数（零拷贝热路径）。
//   - buy/sell/... -> send -> OrderRouter（MainEngine 实盘 / BacktestEngine 回测）。
//   - 参数经 StrategyRegistry::create 解析后传入构造函数。
#pragma once
#include <string>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <cmath>
#include <set>
#include <vector>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/position_store.hpp"

namespace ltc {

// ---- 算法交易（前向声明）：完整定义在 ltc/algo/algo_base.hpp，避免策略基类与其循环包含 ----
// 策略基类惰性持有 AlgoContext（四算法各一实例），统一方法经这些自由函数转发。
namespace algo {
struct AlgoContext;
std::shared_ptr<AlgoContext> make_algo_context(BaseStrategy* st);
void drive_algos(AlgoContext& ctx, int64_t t);
bool algo_start_twap(AlgoContext& ctx, const std::string& vt_symbol, double target_pos,
                     double price, int slip_point, double chase_time, int n_intervals, int epochs);
bool algo_start_vp(AlgoContext& ctx, const std::string& vt_symbol, double target_pos,
                   double price, int slip_point, double chase_time, int n_intervals, int epochs,
                   const std::vector<double>& volume_profile);
bool algo_start_iceberg(AlgoContext& ctx, const std::string& vt_symbol, double target_pos,
                        double price, int slip_point, double chase_time, int n_intervals, int epochs);
bool algo_start_midpeg(AlgoContext& ctx, const std::string& vt_symbol, double target_pos,
                       double price, int slip_point, double chase_time, int n_intervals, int epochs);
void stop_algo(AlgoContext& ctx, const std::string& vt_symbol);
void stop_all_algos(AlgoContext& ctx);
} // namespace algo

// 用户策略基类：继承后重写回调响应行情/成交，并用 buy/sell/short_/cover 下单。
// 引擎通过 set_event_engine/set_order_router 注入事件引擎与下单路由器。
class BaseStrategy {
public:
    BaseStrategy(const std::string& name) : strategy_name_(name), position_store_(name) {}

    virtual ~BaseStrategy() = default;

    // ---- 生命周期回调（均由引擎在固定时机调用，子类按需重写）----
    // on_init：预热/加载历史，仅一次；on_start：订阅行情、启动计时；on_stop：收尾/平仓。
    virtual void on_init() {}
    virtual void on_start() {}
    virtual void on_stop() {}
    // ---- 行情/成交/计时回调（事件引擎零拷贝分派，子类按需重写）----
    // on_tick/on_bar：逐 tick/bar 驱动；on_order/on_trade：委托与成交回报；on_timer：周期触发。
    virtual void on_tick(const TickData&) {}
    virtual void on_bar(const BarData&) {}
    virtual void on_order(const OrderData&) {}
    virtual void on_trade(const TradeData&) {}
    virtual void on_contract(const ContractData&) {}
    virtual void on_timer(int64_t) {}

    // ---- 由引擎注入 ----
    void set_event_engine(EventEngine* ee) { event_engine_ = ee; }
    void set_order_router(OrderRouter* router) { router_ = router; }
    EventEngine* event_engine() { return event_engine_; }

    // 查询合约元信息：经注入的 event_engine_ 查中央合约表；引擎未注入或合约尚未加载返回 nullopt。
    std::optional<ContractData> get_contract(const std::string& vt_symbol) const {
        if (!event_engine_) return std::nullopt;
        return event_engine_->get_contract(vt_symbol);
    }

    const std::string& name() const { return strategy_name_; }

    // 查询本策略持仓（持久化自 JSON 文件，重启也能恢复；非账户持仓）。
    // 返回 {volume: 净持仓(正负表多/空), avg_price: 开仓均价}。
    PositionInfo get_strategy_pos(const std::string& vt_symbol) const {
        return position_store_.get(vt_symbol);
    }
    // 落盘本策略持仓（通常无需手动调用，见下方 handle_trade 自动维护）。
    void save_position(const std::string& vt_symbol, double volume, double avg_price) {
        position_store_.set(vt_symbol, volume, avg_price);
    }
    // 控制持仓账本是否落盘到 JSON（回测引擎置 false，只维护内存账本）。
    void set_persist(bool b) { position_store_.set_persist(b); }

    // 成交入口：由事件引擎在分发 TRADE 事件时调用。先按 TradeData 自动维护本策略
    // 持仓账本（净持仓 + 加权开仓均价），再转调用户重写的 on_trade 回调。
    // C++/Python 策略都可直接用 get_strategy_pos 读取，无需各自维护一份。
    void handle_trade(const TradeData& td) {
        update_position_store(td);
        on_trade(td);
    }

    // 委托状态入口：由事件引擎在分发 ORDER 事件时调用。先维护本策略的活跃委托集合
    // （SUBMITTING/SUBMITTED/PARTTRADED 视为活跃，终态自动移除），再转调用户重写的 on_order。
    void handle_order(const OrderData& o) {
        update_active_orders(o);
        on_order(o);
    }

    // ---- 算法交易支持：活跃委托查询 / 批量撤单 / 最新行情 / 日志 ----
    // 某合约是否存在未终态委托（供算法判断撤单/补单时机）。
    bool has_active_orders(const std::string& vt_symbol) const {
        auto it = active_orderids_.find(vt_symbol);
        return it != active_orderids_.end() && !it->second.empty();
    }
    // 撤销某合约全部活跃委托（算法拆单停用 / 追单前清理用）。
    void cancel_symbol(const std::string& vt_symbol) {
        auto it = active_orderids_.find(vt_symbol);
        if (it == active_orderids_.end()) return;
        for (const auto& oid : it->second) {
            CancelRequest req;
            req.vt_orderid = oid;
            if (router_) router_->cancel_order(req);
        }
        active_orderids_[vt_symbol].clear();
    }
    // 查询最新行情（经 event_engine_ 的缓存）；无行情返回 std::nullopt（Python 端为 None）。
    std::optional<TickData> get_tick(const std::string& vt_symbol) const {
        return event_engine_ ? event_engine_->get_tick(vt_symbol) : std::nullopt;
    }
    // 写策略日志（自动带策略名前缀），供算法与策略共用。
    void write_log(const std::string& msg) {
        Logger::log(Logger::Level::INFO, name() + " " + msg);
    }

    // ---- 算法交易（策略基类内置，写策略最简路径）----
    // 由事件引擎在分发 TIMER 事件时调用：先自动驱动本策略的算法（若启动过），再转调
    // 用户重写的 on_timer。因此策略只需调 send_target_pos_*，无需自己写 on_timer 拆单逻辑。
    void handle_timer(int64_t t) {
        if (algo_ctx_) algo::drive_algos(*algo_ctx_, t);
        on_timer(t);
    }
    // 停止钩子：先停掉全部算法（自动撤销活跃委托），再转调用户重写的 on_stop。
    void handle_stop() {
        if (algo_ctx_) algo::stop_all_algos(*algo_ctx_);
        on_stop();
    }

    // TWAP 拆单：把持仓调到 target_pos，拆 n_intervals 笼、最多 epochs 轮、笼间隔 chase_time 秒。
    bool send_target_pos_twap(const std::string& vt_symbol, double target_pos,
                              double price = 0.0, int slip_point = 0,
                              double chase_time = 30.0, int n_intervals = 3, int epochs = 8) {
        if (!algo_ctx_) algo_ctx_ = algo::make_algo_context(this);
        return algo::algo_start_twap(*algo_ctx_, vt_symbol, target_pos, price, slip_point,
                                     chase_time, n_intervals, epochs);
    }
    // VWAP 按占比拆单：volume_profile 为空时均分。
    bool send_target_pos_vp(const std::string& vt_symbol, double target_pos,
                            double price = 0.0, int slip_point = 0,
                            double chase_time = 30.0, int n_intervals = 3, int epochs = 8,
                            const std::vector<double>& volume_profile = {}) {
        if (!algo_ctx_) algo_ctx_ = algo::make_algo_context(this);
        return algo::algo_start_vp(*algo_ctx_, vt_symbol, target_pos, price, slip_point,
                                   chase_time, n_intervals, epochs, volume_profile);
    }
    // Iceberg 冰山算法：大单拆小单逐笼下，不追价。
    bool send_target_pos_iceberg(const std::string& vt_symbol, double target_pos,
                                 double price = 0.0, int slip_point = 0,
                                 double chase_time = 10.0, int n_intervals = 5, int epochs = 8) {
        if (!algo_ctx_) algo_ctx_ = algo::make_algo_context(this);
        return algo::algo_start_iceberg(*algo_ctx_, vt_symbol, target_pos, price, slip_point,
                                        chase_time, n_intervals, epochs);
    }
    // MidPeg 中间价算法：买卖中间价委托，不追价。
    bool send_target_pos_midpeg(const std::string& vt_symbol, double target_pos,
                                double price = 0.0, int slip_point = 0,
                                double chase_time = 10.0, int n_intervals = 3, int epochs = 8) {
        if (!algo_ctx_) algo_ctx_ = algo::make_algo_context(this);
        return algo::algo_start_midpeg(*algo_ctx_, vt_symbol, target_pos, price, slip_point,
                                       chase_time, n_intervals, epochs);
    }
    // 停止某合约的算法（停任务并撤活跃委托）；停止全部算法。
    void stop_algo(const std::string& vt_symbol) {
        if (algo_ctx_) algo::stop_algo(*algo_ctx_, vt_symbol);
    }
    void stop_all_algos() {
        if (algo_ctx_) algo::stop_all_algos(*algo_ctx_);
    }

  private:
    // 根据委托状态维护活跃委托集合：未终态加入，终态(全成/撤销/拒单)移除。
    void update_active_orders(const OrderData& o) {
        if (o.vt_symbol.empty() || o.vt_orderid.empty()) return;
        auto& s = active_orderids_[o.vt_symbol];
        switch (o.status) {
            case Status::SUBMITTING:
            case Status::SUBMITTED:
            case Status::PARTTRADED:
                s.insert(o.vt_orderid);
                break;
            case Status::ALLTRADED:
            case Status::CANCELLED:
            case Status::REJECTED:
                s.erase(o.vt_orderid);
                break;
            default:
                break;
        }
    }
    // 根据单笔成交更新本策略持仓账本：开仓按成交量加权均价，平仓只减仓、均价不变；
    // 反手（方向相反的开仓）先平旧仓再开新仓。逻辑与 vnpy 持仓维护一致。
    void update_position_store(const TradeData& td) {
        PositionInfo cur = position_store_.get(td.vt_symbol);
        double signed_vol = td.volume * (td.direction == Direction::LONG ? 1.0 : -1.0);
        bool is_open = (td.offset == Offset::OPEN);
        double new_vol = cur.volume;
        double new_avg = cur.avg_price;
        if (is_open) {
            if (cur.volume == 0.0 || (cur.volume > 0) == (signed_vol > 0)) {
                new_vol = cur.volume + signed_vol;
                new_avg = (cur.volume == 0.0) ? td.price
                    : (cur.avg_price * std::fabs(cur.volume) + td.price * td.volume)
                      / (std::fabs(cur.volume) + td.volume);
            } else {
                double remain = cur.volume + signed_vol;
                if (std::fabs(remain) < 1e-9) {
                    new_vol = 0.0; new_avg = 0.0;
                } else if ((remain > 0) == (signed_vol > 0)) {
                    new_vol = remain; new_avg = td.price;
                } else {
                    new_vol = remain; new_avg = cur.avg_price;
                }
            }
        } else {
            new_vol = cur.volume - signed_vol;
            if (std::fabs(new_vol) < 1e-9) { new_vol = 0.0; new_avg = 0.0; }
            else new_avg = cur.avg_price;
        }
        position_store_.set(td.vt_symbol, new_vol, new_avg);
    }

  public:
    // ---- 交易辅助函数（vnpy 风格，内部统一走 protected send）----
    // buy=开多, sell=平多, short_=开空, cover=平空；vt_symbol="symbol.exchange"，
    // price 为委托价、volume 为数量、type 默认 LIMIT（限价）。返回 vt_orderid（可撤单）。
    std::string buy(const std::string& vt_symbol, double price, double volume,
                    OrderType type = OrderType::LIMIT) {
        return send(Direction::LONG, Offset::OPEN, vt_symbol, price, volume, type);
    }
    std::string sell(const std::string& vt_symbol, double price, double volume,
                     OrderType type = OrderType::LIMIT) {
        return send(Direction::SHORT, Offset::CLOSE, vt_symbol, price, volume, type);
    }
    std::string short_(const std::string& vt_symbol, double price, double volume,
                       OrderType type = OrderType::LIMIT) {
        return send(Direction::SHORT, Offset::OPEN, vt_symbol, price, volume, type);
    }
    std::string cover(const std::string& vt_symbol, double price, double volume,
                      OrderType type = OrderType::LIMIT) {
        return send(Direction::LONG, Offset::CLOSE, vt_symbol, price, volume, type);
    }

    // 撤销委托：以 vt_orderid（"网关名.原始单号"）定位，经 router_ 路由到对应接口。
    void cancel(const std::string& vt_orderid) {
        CancelRequest req;
        req.vt_orderid = vt_orderid;
        router_->cancel_order(req);
    }

    // 订阅行情：把合约 vt_symbol（"symbol.exchange"）经 router_（实盘为 MainEngine）
    // 转发到对应接口；与 buy/sell 同机制，策略不感知实盘/回测。
    void subscribe(const std::string& vt_symbol) {
        router_->subscribe(std::vector<std::string>{vt_symbol});
    }

    // 解析 vt_symbol -> (symbol, exchange)
    static void parse_vt_symbol(const std::string& vt_symbol, std::string& symbol, Exchange& ex) {
        auto pos = vt_symbol.find('.');
        if (pos == std::string::npos) { symbol = vt_symbol; ex = Exchange::NONE; return; }
        symbol = vt_symbol.substr(0, pos);
        std::string exs = vt_symbol.substr(pos + 1);
        if (exs == "BINANCE_USDT") ex = Exchange::BINANCE_USDT;
        else if (exs == "BINANCE") ex = Exchange::BINANCE;
        else if (exs == "OKX") ex = Exchange::OKX;
        else if (exs == "SHFE") ex = Exchange::SHFE;
        else if (exs == "CFFEX") ex = Exchange::CFFEX;
        else if (exs == "DCE") ex = Exchange::DCE;
        else if (exs == "CZCE") ex = Exchange::CZCE;
        else if (exs == "INE") ex = Exchange::INE;
        else if (exs == "SSE") ex = Exchange::SSE;
        else if (exs == "SZSE") ex = Exchange::SZSE;
        else ex = Exchange::NONE;
    }

protected:
    // 统一下单入口：把 方向/开平/vt_symbol/价格/数量/类型 装配成 OrderRequest，
    // reference 记为本策略名（便于成交回报复原），再经 router_ 路由到具体接口。
    std::string send(Direction dir, Offset off, const std::string& vt_symbol,
                     double price, double volume, OrderType type) {
        std::string symbol; Exchange ex;
        parse_vt_symbol(vt_symbol, symbol, ex);
        OrderRequest req;
        req.symbol = symbol; req.exchange = ex;
        req.direction = dir; req.offset = off; req.type = type;
        req.price = price; req.volume = volume; req.reference = strategy_name_;
        return router_->send_order(req);
    }

    std::string strategy_name_;
    EventEngine* event_engine_ = nullptr;
    OrderRouter* router_ = nullptr;
    PositionStore position_store_;   // 本策略持仓持久化（JSON，按策略名命名空间隔离）
    std::map<std::string, std::set<std::string>> active_orderids_;  // vt_symbol -> 活跃委托集合
    std::shared_ptr<algo::AlgoContext> algo_ctx_;  // 算法上下文（惰性创建，见 algo_base.hpp）
};

} // namespace ltc
