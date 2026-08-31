// gateway.hpp - 交易接口基类 (vnpy BaseGateway 风格)
//
// 职责：定义接入行情与交易的统一接口。每个交易所/API（或回测数据源）实现一个子类，
//       负责建立连接、订阅行情、发出委托，并把通道回传的 Tick/Bar/委托/成交/持仓/账户/
//       合约等数据封装后广播给策略。
//
// 关键设计：
//   - 数据回推统一走 protected 的 on_xxx(T d) 系列：按值接收、用 std::move 装入 Event，
//     调用处用 std::move 传局部对象时整条链路仅一次移动、零拷贝。
//   - on_log 暴露为 public，方便 SPI 回调等外部类（非 BaseGateway 派生）写日志。
//   - gateway_name_ 既作日志前缀，也作为 vt_orderid 的「网关名.原始单号」前缀，
//     撤单时据此把请求路由回正确的接口实例（见 MainEngine::cancel_order）。
//
// 与其他模块关系：
//   - 被 MainEngine 持有并驱动（connect/subscribe/send_order/cancel_order）。
//   - on_xxx 推送的 Event 由 EventEngine 分发到 BaseStrategy 的虚函数回调。
//   - 下单回路由 OrderRouter 抽象：实盘为 MainEngine，回测为 BacktestEngine。
#pragma once
#include <string>
#include <map>
#include <memory>
#include <functional>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// 策略下单路由接口（抽象基类）。策略不直接依赖具体引擎，只通过该接口下单/撤单，
// 从而实盘与回测可共用同一套策略代码：实盘由 MainEngine 实现，回测由 BacktestEngine 实现。
struct OrderRouter {
    virtual std::string send_order(const OrderRequest& req) = 0;
    virtual void cancel_order(const CancelRequest& req) = 0;
    // 订阅行情：实盘引擎转发到对应接口；回测数据由引擎直接喂入，无需订阅，留空实现。
    virtual void subscribe(const std::vector<std::string>& vt_symbols) {}
    virtual ~OrderRouter() = default;
};

// 交易接口基类：所有行情/交易通道（交易所 API、仿真、回测数据源）的公共基类。
// 子类只需实现 connect/close/send_order/cancel_order/subscribe，并通过 on_xxx 回推数据。
class BaseGateway {
public:
    // 构造：ee 为事件引擎（on_xxx 回推目标），name 为本接口名（日志前缀 + 撤单路由名）。
    BaseGateway(EventEngine* ee, const std::string& name)
        : event_engine_(ee), gateway_name_(name) {}
    virtual ~BaseGateway() = default;

    // 建立并登录通道；settings 为配置段（如 API key、行情/交易地址），由各接口自行解析。
    virtual void connect(const std::map<std::string, std::string>& settings) = 0;
    // 断开通道并释放底层连接资源。
    virtual void close() = 0;
    // 发单：返回交易所原始 orderid（为空表示失败）。req 含 symbol/方向/开平/类型/价格/数量。
    virtual std::string send_order(const OrderRequest& req) = 0;
    // 撤单：按 CancelRequest 中的 vt_orderid 定位并撤销对应委托。
    virtual void cancel_order(const CancelRequest& req) = 0;
    // 订阅行情：vt_symbols 为 "symbol.exchange" 列表。
    virtual void subscribe(const std::vector<std::string>& vt_symbols) = 0;

    const std::string& name() const { return gateway_name_; }

    // 日志（public：供 SPI 回调等外部类调用）
    void on_log(const std::string& msg) {
        Logger::log(Logger::Level::INFO, "[" + gateway_name_ + "] " + msg);
    }

protected:
    // 由接口向事件引擎推送数据。按值接收并 move 进 Event：
    // 调用处用 std::move 传局部对象时整条链路零拷贝（仅一次 move）。
    // 说明：实盘下 tick 来自真实行情、成交由交易所回报；回测引擎约定“tick 级撮合”——
    //   委托在【下一笔】tick 才成交（无未来函数），限价单按 last_price 穿越成交，
    //   市价单按 last_price + 滑点成交，使回测与实盘语义一致、可被策略直接复用。
    void on_tick(TickData d) { event_engine_->put(Event{EventType::TICK, std::move(d)}); }
    void on_bar(BarData d)   { event_engine_->put(Event{EventType::BAR, std::move(d)}); }
    void on_order(OrderData d){ event_engine_->put(Event{EventType::ORDER, std::move(d)}); }
    void on_trade(TradeData d){ event_engine_->put(Event{EventType::TRADE, std::move(d)}); }
    void on_position(PositionData d){ event_engine_->put(Event{EventType::POSITION, std::move(d)}); }
    void on_account(AccountData d){ event_engine_->put(Event{EventType::ACCOUNT, std::move(d)}); }
    void on_contract(ContractData d){ event_engine_->put(Event{EventType::CONTRACT, std::move(d)}); }

    EventEngine* event_engine_;   // 回推目标：on_xxx 把数据 put 进它
    std::string gateway_name_;    // 接口名：日志前缀 + vt_orderid 的路由前缀
};

} // namespace ltc
