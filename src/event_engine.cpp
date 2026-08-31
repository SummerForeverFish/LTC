// event_engine.cpp - EventEngine 方法实现
//
// 模块职责：EventEngine 的实盘事件分发核心。gateway 把行情/委托/成交打包成 Event
//           经无锁 MPMC 队列送入，本 TU 的消费者线程出队后零拷贝分发给策略虚函数。
// 关键设计（见 event.hpp 总述）：
//   - 载荷用 std::variant 内联存储，dispatch 用 std::get<T> 取 const& -> 零拷贝读取。
//   - 传输用有界无锁 MPMC 环形队列，热路径零堆分配、无 mutex/condvar。
//   - 分派用「按事件类型分桶的扁平分组」，策略走 register_strategy 热路径直接调虚函数
//     (无 std::function 间接、无 map 查找)，故延迟低、可支撑高频实盘。
// 与框架关系：这是「实盘路径」。回测引擎(BacktestEngine / TickBacktestEngine)走直连
//           回调、不经事件队列；EventEngine 仅用于实盘(MainEngine 驱动)。
//
// 放在独立 TU：方法体需要 BaseStrategy 完整类型（调用其虚函数 on_tick 等），
// 而 event.hpp 仅前向声明 BaseStrategy 以避免与 strategy.hpp 形成循环包含。
#include "ltc/core/event.hpp"
#include "ltc/core/strategy.hpp"   // 使 BaseStrategy 完整，方可调用虚函数

namespace ltc {

// 注册策略：把同一策略指针同时塞进 5 个类型分桶(tick/bar/order/trade/timer)。
// 分派时直接遍历对应桶调虚函数，省去 std::function 包装与 unordered_map 查找，
// 是热路径低延迟的关键。
void EventEngine::register_strategy(BaseStrategy* st) {
    // 按事件类型分桶：分发时直接遍历这些指针调虚函数，无 std::function、无 map 查找。
    tick_strategies_.push_back(st);
    bar_strategies_.push_back(st);
    order_strategies_.push_back(st);
    trade_strategies_.push_back(st);
    contract_strategies_.push_back(st);
    timer_strategies_.push_back(st);
}

// 注册通用处理器(日志/监控等)：按事件类型存入扁平分桶数组的对应槽位。
// 用 move 接收 std::function，避免拷贝；分桶数组下标直取，免去 map 查找。
void EventEngine::register_handler(EventType type, Handler h) {
    // 通用处理器（日志 / 监控等）：按类型存入扁平分桶，避免 unordered_map 查找。
    generic_handlers_[static_cast<size_t>(type)].push_back(std::move(h));
}

// 向事件队列投递一个 Event。按值接收后 std::move 入队，整条链路仅一次移动、零拷贝。
// 队列满时 enqueue 返回 false，这里自旋 + yield 退让形成「背压」：生产者线程让出 CPU
// 等待消费者腾出空位，绝不丢弃事件(故 dropped_events() 监控计数恒为 0)。
// 这正是与「回测直连回调」不同的实盘路径特征——事件经队列缓冲、异步消费。
void EventEngine::put(Event ev) {
    // 无锁入队；队列满时自旋退让（背压，绝不丢事件）。
    while (!queue_.enqueue(std::move(ev))) {
        std::this_thread::yield();
    }
}

// 启动消费者线程。active_ 用 exchange(true) 保证只启动一次(重复调用直接返回)。
// 新线程执行 run()：从 MPMC 队列取 Event 并分派，与生产者(gateway)并发运行。
void EventEngine::start() {
    if (active_.exchange(true)) return;
    thread_ = std::thread([this]() { run(); });
}

// 停止引擎：active_ 置 false 让 run() 退出循环；exchange(false) 仅首次生效。
// join() 等待消费者线程结束，并借 run() 末尾的排空逻辑保证已入队事件均被处理。
void EventEngine::stop() {
    if (!active_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

// 消费者线程主循环。acquire 读取 active_：队列非空则出队并 dispatch；为空则 yield
// 让出 CPU(忙轮询+空闲退让，避免空转占满一个核，同时保证低延迟唤醒)。
// 收到停止信号后，再排空队列里剩余事件并分派，确保不漏处理已入队的事件。
void EventEngine::run() {
    Event ev;
    while (active_.load(std::memory_order_acquire)) {
        if (queue_.dequeue(ev)) { dispatch(ev); continue; }
        // 空闲退让，避免空转占满一个核
        std::this_thread::yield();
    }
    // 退出前排空队列，保证已入队事件都被处理
    while (queue_.dequeue(ev)) dispatch(ev);
}

// 分派单个 Event：两步。
//   1) 通用处理器：先按类型查扁平分桶 generic_handlers_，非空则逐个调用(捕获异常避免
//      单个 handler 崩掉整个引擎)；用于日志/监控等非策略消费。
//   2) 热路径：按 EventType switch，用 std::get<T> 取 variant 内联载荷的 const 引用，
//      直接遍历对应策略分桶调虚函数(on_tick/on_bar/...)。引用直传、零拷贝、无堆分配，
//      是低延迟关键。POSITION/ACCOUNT/LOG 等仅走通用处理器(default 分支)；CONTRACT 已走热路径。
void EventEngine::dispatch(Event& ev) {
    // 1) 通用处理器（若有）
    const size_t idx = static_cast<size_t>(ev.type);
    const auto& gh = generic_handlers_[idx];
    if (!gh.empty()) {
        for (auto& h : gh) {
            try { h(ev); }
            catch (const std::exception& e) {
                std::cerr << "[EventEngine] handler exception: " << e.what() << std::endl;
            }
        }
    }
    // 2) 热路径：直接调策略虚函数，处理器拿到 const 引用 -> 零拷贝
    switch (ev.type) {
        case EventType::TICK: {
            const auto& d = std::get<TickData>(ev.data);
            for (auto* s : tick_strategies_) s->on_tick(d);
            break;
        }
        case EventType::BAR: {
            const auto& d = std::get<BarData>(ev.data);
            for (auto* s : bar_strategies_) s->on_bar(d);
            break;
        }
        case EventType::ORDER: {
            const auto& d = std::get<OrderData>(ev.data);
            for (auto* s : order_strategies_) s->on_order(d);
            break;
        }
        case EventType::TRADE: {
            const auto& d = std::get<TradeData>(ev.data);
            for (auto* s : trade_strategies_) s->on_trade(d);
            break;
        }
        case EventType::TIMER: {
            const auto& d = std::get<int64_t>(ev.data);
            for (auto* s : timer_strategies_) s->on_timer(d);
            break;
        }
        case EventType::CONTRACT: {
            const auto& d = std::get<ContractData>(ev.data);
            {
                std::lock_guard<std::mutex> lk(contract_mutex_);
                contracts_[d.vt_symbol] = d;
            }
            for (auto* s : contract_strategies_) s->on_contract(d);
            break;
        }
        default:
            // POSITION / ACCOUNT / LOG 仅走通用处理器
            break;
    }
}

// 按 vt_symbol 查询中央合约表；未加载/未找到返回 std::nullopt（Python 端为 None）。
std::optional<ContractData> EventEngine::get_contract(const std::string& vt_symbol) const {
    std::lock_guard<std::mutex> lk(contract_mutex_);
    auto it = contracts_.find(vt_symbol);
    if (it != contracts_.end()) return it->second;
    // 兜底：查询串不含交易所后缀（无 '.'）时，按 symbol 字段模糊匹配，
    // 如 "rb2609" 也能命中已存的 "rb2609.SHFE"；同 symbol 多交易所时取首个匹配。
    if (vt_symbol.find('.') == std::string::npos) {
        for (const auto& kv : contracts_) {
            if (kv.second.symbol == vt_symbol) return kv.second;
        }
    }
    return std::nullopt;
}

// 返回全部已加载合约（快照），便于 Python 端枚举全市场合约元信息。
std::vector<ContractData> EventEngine::all_contracts() const {
    std::lock_guard<std::mutex> lk(contract_mutex_);
    std::vector<ContractData> out;
    out.reserve(contracts_.size());
    for (const auto& kv : contracts_) out.push_back(kv.second);
    return out;
}

} // namespace ltc
