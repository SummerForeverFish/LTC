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

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"

namespace ltc {

// 用户策略基类：继承后重写回调响应行情/成交，并用 buy/sell/short_/cover 下单。
// 引擎通过 set_event_engine/set_order_router 注入事件引擎与下单路由器。
class BaseStrategy {
public:
    BaseStrategy(const std::string& name) : strategy_name_(name) {}

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
};

} // namespace ltc
