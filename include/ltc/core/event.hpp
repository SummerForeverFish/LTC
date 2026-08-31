// event.hpp - 事件引擎 (vnpy EventEngine 风格，零拷贝 / 无锁改造版)
//
// 相比旧版的关键变化（为真正高频实盘打基础）：
//   1) 事件载荷用 std::variant 取代 std::any
//      - 内联存储，无类型擦除堆分配、无 RTTI 查找
//      - 处理器用 std::get<T>(e.data) 拿到 const 引用 -> 零拷贝读取
//   2) 事件传输用有界无锁 MPMC 环形队列 (ring_queue.hpp) 取代
//      std::queue<Event> + std::mutex + std::condition_variable
//      - 预分配、热路径零堆分配、无锁、无 ABA、缓存行隔离
//   3) 分派用「按事件类型分桶的扁平分组」取代 unordered_map 查找；
//      热路径策略走 register_strategy 直接调虚函数（无 std::function 间接、无 map 查找）
//
// 典型链路：gateway.on_tick(TickData) -> put(Event) -> 无锁入队
//           -> 引擎线程出队 -> dispatch -> strategy.on_tick(const TickData&)（零拷贝）
#pragma once
#include <variant>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <map>
#include <mutex>
#include <optional>

#include "ltc/core/object.hpp"
#include "ltc/core/ring_queue.hpp"

namespace ltc {

// 事件类型：决定事件如何被分桶与分派。
//   TICK     行情快照事件
//   BAR      K 线事件
//   ORDER    委托状态更新事件
//   TRADE    成交流水事件
//   POSITION 持仓更新事件
//   ACCOUNT  账户资金事件
//   CONTRACT 合约元信息事件
//   LOG      日志事件（payload 为 std::string）
//   TIMER    定时器事件（payload 为 int64_t 时间戳，由引擎周期性产生）
enum class EventType {
    TICK,
    BAR,
    ORDER,
    TRADE,
    POSITION,
    ACCOUNT,
    CONTRACT,
    LOG,
    TIMER
};
// 事件类型总数（用于扁平分派数组大小；新增枚举值时同步 +1）
constexpr size_t kEventTypeCount = static_cast<size_t>(EventType::TIMER) + 1;

// 事件载荷：variant 取代 std::any。各备选项内联存储，读取返回引用即零拷贝。
// 注意：备选项顺序必须与 EventType 枚举值一一对应（TICK->TickData, BAR->BarData, ...），dispatch 据此 std::get。
using EventPayload = std::variant<
    TickData,         // TICK
    BarData,          // BAR
    OrderData,        // ORDER
    TradeData,        // TRADE
    PositionData,     // POSITION
    AccountData,      // ACCOUNT
    ContractData,     // CONTRACT
    std::string,      // LOG
    int64_t           // TIMER
>;

// Event：事件包（类型 + 载荷）。轻量值类型，通过队列在生产者/消费者线程间移动。
struct Event {
    EventType type = EventType::TICK;
    EventPayload data;

    Event() = default;
    Event(EventType t, EventPayload p) : type(t), data(std::move(p)) {}

    // 零拷贝读取：返回 payload 的 const 引用（含类型校验，类型不符抛 bad_variant_access）
    template <class T>
    const T& as() const { return std::get<T>(data); }
    template <class T>
    bool holds() const { return std::holds_alternative<T>(data); }
};

// 工厂：构造时把载荷 move 进 Event（零拷贝）
template <class T>
Event make_event(EventType t, T&& payload) {
    return Event(t, EventPayload(std::forward<T>(payload)));
}

class BaseStrategy;  // 前向声明；EventEngine 的方法体在 EventEngine 完整定义后实现

// EventEngine：事件引擎。后台单线程从 ring_queue 无锁出队并分派：
//   热路径策略走 register_strategy 直调虚函数（零拷贝、无 map 查找）；
//   通用处理器走 register_handler 按类型分桶回调（日志/监控等）。
// 典型链路：gateway.put(Event) -> 引擎线程 dispatch -> strategy.on_*(const T&)。
class EventEngine {
public:
    using Handler = std::function<void(const Event&)>;
    static constexpr size_t kQueueCapacity = 1u << 16;  // 65536 个预分配事件槽

    EventEngine() : active_(false) {}
    ~EventEngine() { stop(); }

    void start();                          // 启动后台分派线程（幂等：已运行则直接返回）
    void stop();                           // 停止线程并排空队列，确保已入队事件都被处理
    void put(Event ev);                    // 入队一个事件（队列满时自旋退让，绝不丢事件）
    void register_handler(EventType type, Handler h);  // 注册通用处理器（日志/监控等），按类型分桶
    void register_strategy(BaseStrategy* st);          // 注册策略：按事件类型分桶，热路径直调虚函数
    bool is_active() const { return active_.load(); }  // 引擎是否在运行

    // 监控接口（HFT 下观测队列水位 / 背压）
    size_t queue_capacity() const { return queue_.capacity(); }
    size_t approx_queue_size() const { return queue_.approx_size(); }
    size_t dropped_events() const { return queue_.dropped(); }

    // 合约元信息仓库：CONTRACT 事件到达时按 vt_symbol 存入（见 dispatch），供策略/引擎查询。
    // get_contract 找不到返回 std::nullopt（Python 端为 None）；all_contracts 返回全部已加载合约。
    std::optional<ContractData> get_contract(const std::string& vt_symbol) const;
    std::vector<ContractData> all_contracts() const;

private:
    void run();
    void dispatch(Event& ev);

    MpmcQueue<Event, kQueueCapacity> queue_;                // 无锁有界队列：网关写入、引擎线程消费
    std::vector<Handler> generic_handlers_[kEventTypeCount]; // 按事件类型分桶的通用处理器（下标 = EventType）
    // 热路径策略分桶：每种事件类型一个向量，dispatch 时直接遍历调虚函数，无 map 查找、无 std::function 间接
    std::vector<BaseStrategy*> tick_strategies_;
    std::vector<BaseStrategy*> bar_strategies_;
    std::vector<BaseStrategy*> order_strategies_;
    std::vector<BaseStrategy*> trade_strategies_;
    std::vector<BaseStrategy*> contract_strategies_;
    std::vector<BaseStrategy*> timer_strategies_;
    std::thread thread_;
    std::atomic<bool> active_;

    // 中央合约表：CONTRACT 事件按 vt_symbol 写入，Python 线程经 get_contract 读取，
    // 故用互斥锁保护（引擎线程写、查询线程读，低竞争）。
    std::map<std::string, ContractData> contracts_;
    mutable std::mutex contract_mutex_;
};

} // namespace ltc
