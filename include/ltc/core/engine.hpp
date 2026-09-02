// engine.hpp - 实盘主引擎 (vnpy MainEngine / CtaEngine 风格，无 UI)
//
// 职责：实盘总控。持有 EventEngine、若干 BaseGateway 与若干 BaseStrategy；把策略的下单
//       请求路由到正确接口，并把网关回推的行情/成交事件分发给策略。
//
// 关键设计：
//   - 自身继承 OrderRouter：策略调 buy/sell/... 最终落到 MainEngine::send_order，由
//     resolve_gateway 按交易所（或默认接口）选接口，从而策略代码与回测完全一致。
//   - start() 顺序很关键：先给每个策略注入依赖并注册事件回调，再启动事件引擎线程，
//     最后调 on_init/on_start——确保引擎线程跑起来前订阅已就绪，不漏事件。
//   - 策略的事件回调走 EventEngine::register_strategy 的零拷贝热路径（直接调虚函数，
//     无 std::function 间接、无 map 查找）。
//
// 与其他模块关系：
//   - 持有/驱动 BaseGateway（connect/subscribe/send_order）。
//   - 通过 EventEngine 把事件分发给 BaseStrategy 的 on_xxx。
//   - 策略类型经 StrategyRegistry 创建后再 add_strategy 接入。
#pragma once
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// 实盘主引擎：驱动事件循环、管理接口与策略、转发下单；同时作为 OrderRouter 实现。
class MainEngine : public OrderRouter {
public:
    MainEngine() : event_engine_(std::make_shared<EventEngine>()) {}

    // 注册一个接口；首个注册者自动成为默认接口（send_order 路由兜底用）。
    void add_gateway(std::shared_ptr<BaseGateway> gw) {
        gateways_[gw->name()] = gw;
        if (default_gateway_.empty()) default_gateway_ = gw->name();
    }
    void set_default_gateway(const std::string& name) { default_gateway_ = name; }

    void add_strategy(std::shared_ptr<BaseStrategy> st) {
        strategies_[st->name()] = st;
    }

    void connect_all(const std::map<std::string, std::string>& settings) {
        for (auto& kv : gateways_) kv.second->connect(settings);
    }

    // 订阅行情：转发到默认接口（单接口实盘场景足够；多接口可按交易所细分）。
    void subscribe(const std::vector<std::string>& vt_symbols) override {
        auto it = gateways_.find(default_gateway_);
        if (it != gateways_.end()) it->second->subscribe(vt_symbols);
        else Logger::log(Logger::Level::ERR, "无可用接口订阅行情");
    }

    // 启动：先注册策略回调并注入依赖，再起事件引擎线程，最后 on_init/on_start。
    void start() {
        // 先把策略处理器注册进事件引擎（必须在启动引擎线程前完成，避免漏事件）
        for (auto& kv : strategies_) {
            auto st = kv.second;
            st->set_event_engine(event_engine_.get());
            st->set_order_router(this);
            register_strategy_events(st);
        }
        event_engine_->start();
        start_timer();   // 周期投递 TIMER 事件，驱动策略 on_timer / 算法交易
        for (auto& kv : strategies_) {
            auto st = kv.second;
            st->on_init();
            st->on_start();
            Logger::log(Logger::Level::INFO, "策略已启动: " + st->name());
        }
        running_ = true;
    }

    // 停止：先 handle_stop 收尾（停算法撤单 + 用户 on_stop），再关接口，最后停定时器与事件引擎线程。
    void stop() {
        running_ = false;
        for (auto& kv : strategies_) kv.second->handle_stop();
        for (auto& kv : gateways_) kv.second->close();
        stop_timer();
        event_engine_->stop();
    }

    bool is_running() const { return running_; }
    std::shared_ptr<EventEngine> event_engine() { return event_engine_; }

    // 定时器节拍（毫秒）：决定 on_timer 被调用的频率，默认 500ms。
    void set_timer_interval(int ms) { timer_interval_ms_ = ms > 0 ? ms : 500; }
    int timer_interval() const { return timer_interval_ms_; }

    // ---- OrderRouter 实现 ----
    // OrderRouter 实现：把委托路由到对应接口（resolve_gateway）；无可用接口返回空串。
    std::string send_order(const OrderRequest& req) override {
        BaseGateway* gw = resolve_gateway(req.exchange);
        if (!gw) {
            Logger::log(Logger::Level::ERR, "无可用接口处理交易所: " + exchange_to_str(req.exchange));
            return "";
        }
        return gw->send_order(req);
    }
    // OrderRouter 实现：从 vt_orderid 解析「网关名.」前缀定位接口；无前缀则用默认接口。
    void cancel_order(const CancelRequest& req) override {
        // 目前仅支持通过 vt_orderid 中的 gateway 名路由
        std::string gname = default_gateway_;
        auto p = req.vt_orderid.find('.');
        if (p != std::string::npos) gname = req.vt_orderid.substr(0, p);
        auto it = gateways_.find(gname);
        if (it != gateways_.end()) it->second->cancel_order(req);
    }

private:
    // 定时器线程：周期向事件引擎投递 TIMER 事件（payload 为毫秒时间戳）。
    void start_timer() {
        if (timer_running_.exchange(true)) return;
        timer_thread_ = std::thread([this]() {
            while (timer_running_.load()) {
                int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                event_engine_->put(Event{EventType::TIMER, now});
                std::this_thread::sleep_for(std::chrono::milliseconds(timer_interval_ms_));
            }
        });
    }
    void stop_timer() {
        if (!timer_running_.exchange(false)) return;
        if (timer_thread_.joinable()) timer_thread_.join();
    }

    // 选路：当前实现所有接口由同一默认网关处理（按交易所细分见各 gateway）；
    // 找不到时返回 nullptr，由 send_order 报错。保留 ex 参数以便后续按交易所分流。
    BaseGateway* resolve_gateway(Exchange ex) {
        // 优先按交易所匹配，否则用默认接口
        for (auto& kv : gateways_) {
            // 简化：默认接口处理其支持的所有交易所（具体见各 gateway 实现）
        }
        auto it = gateways_.find(default_gateway_);
        return it != gateways_.end() ? it->second.get() : nullptr;
    }

    // 把策略注册进事件引擎的零拷贝热路径：直接保存策略指针，分发时调其虚函数，
    // 无 std::function 间接、无 map 查找（见 EventEngine::register_strategy）。
    void register_strategy_events(std::shared_ptr<BaseStrategy> st) {
        // 走零拷贝热路径：事件引擎按类型直接调策略虚函数，处理器拿到 const 引用（无拷贝）
        event_engine_->register_strategy(st.get());
    }

    std::shared_ptr<EventEngine> event_engine_;
    std::map<std::string, std::shared_ptr<BaseGateway>> gateways_;
    std::map<std::string, std::shared_ptr<BaseStrategy>> strategies_;
    std::string default_gateway_;
    bool running_ = false;

    std::thread timer_thread_;
    std::atomic<bool> timer_running_{false};
    int timer_interval_ms_ = 500;
};

} // namespace ltc
