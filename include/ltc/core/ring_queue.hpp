// ring_queue.hpp - 无锁有界 MPMC 环形队列 (Vyukov 算法)
//
// 设计目标（为高频/低延迟场景服务）：
//   - 预分配：所有 cell 在构造时一次性分配，热路径（enqueue/dequeue）零堆分配
//   - 无锁：生产者/消费者均用 CAS + 内存序，无 mutex / condvar
//   - 无 ABA：每个 cell 携带单调递增的 seq 序号，取代「指针 + CAS」的 ABA 隐患
//   - 缓存友好：enqueue_pos_ / dequeue_pos_ 各自独占一个缓存行，cell 内 seq 与
//     data 拉开间距，避免 false sharing
//   - 多生产者多消费者安全：可同时被多个网关线程写入、被单一引擎线程消费
//
// 模板参数 Capacity 必须是 2 的幂，便于用位与代替取模。
#pragma once
#include <atomic>
#include <memory>
#include <cstdint>

namespace ltc {

template <class T, size_t Capacity>
class MpmcQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "MpmcQueue Capacity 必须是 2 的幂");

    // Cell：队列槽位。seq 是「序号」而非指针——它同时充当状态位与无 ABA 保护：
    //   seq == pos       表示该槽可写（enqueue 占用）
    //   seq == pos+1     表示该槽可读（dequeue 占用）
    //   seq == pos+Cap   表示该槽已读、回到下一轮可写态
    // 为什么无 ABA：传统「指针+CAS」在指针复用后可能误判；这里用单调递增的 seq 代替指针，
    // 即使槽位轮转复用，seq 也已变化，CAS 必然失败重试，杜绝 ABA。
    struct Cell {
        std::atomic<size_t> seq;
        T data;
    };

    static constexpr size_t kMask = Capacity - 1;

    std::unique_ptr<Cell[]> buffer_;                  // 预分配槽位数组（构造时一次分配，热路径零堆分配）
    alignas(64) std::atomic<size_t> enqueue_pos_{0};  // 生产者游标，独占一个缓存行，避免与 dequeue_pos_ 产生 false sharing
    alignas(64) std::atomic<size_t> dequeue_pos_{0};  // 消费者游标，独占一个缓存行
    alignas(64) std::atomic<size_t> dropped_{0};      // 仅监控用，非并发关键路径（被填满时由调用方统计丢弃）

public:
    // 初始化：第 i 个槽序号置为 i，使首轮 enqueue 在 pos==i 时 seq==pos 命中；之后每轮 +Capacity 循环复用。
    MpmcQueue() : buffer_(std::make_unique<Cell[]>(Capacity)) {
        for (size_t i = 0; i < Capacity; ++i)
            buffer_[i].seq.store(i, std::memory_order_relaxed);
    }

    // 生产者入队：成功返回 true；队列满返回 false（不阻塞、不分配，是否丢事件由调用方决定）。
    // 内存序：读 enqueue_pos_ 用 relaxed（仅作探测起点，真正占坑靠 CAS）；
    //         读 seq 用 acquire，保证读到该槽旧数据已对其他线程可见；
    //         发布 seq 用 release，使本次写入的 data 对随后 dequeue 的线程可见。
    bool enqueue(T&& item) {
        Cell* cell;
        // relaxed 读游标：仅作探测起点，真正占坑靠下面的 CAS
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                // 槽空闲且轮次匹配：尝试占用（CAS 失败说明被别的生产者抢先，pos 被刷新后重试）
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // 满：该槽尚未被消费（seq < pos），队列已满
            } else {
                // 落后了：重新加载游标到最新位置再试
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);                          // 写入载荷（此刻只有本生产者持有该槽）
        cell->seq.store(pos + 1, std::memory_order_release);   // 标记可读，release 让 data 对消费者可见
        return true;
    }

    // 消费者出队：成功返回 true 并把 item 移出；空返回 false（非阻塞）。
    // 内存序与 enqueue 对称：读 seq 用 acquire（读到生产者 release 的 data），发布 seq 用 release
    //         （标记该槽已空、可被下一轮生产者覆盖，对生产者可见）。
    bool dequeue(T& item) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                // 槽已写满且轮次匹配：尝试占用（CAS 失败说明被别的消费者抢先，重试）
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // 空：该槽尚未被生产（seq < pos+1）
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        item = std::move(cell->data);                          // 移出载荷
        cell->seq.store(pos + Capacity, std::memory_order_release);  // 标回可写：+Capacity 回到下一轮空闲态
        return true;
    }

    static constexpr size_t capacity() { return Capacity; }

    // 近似长度（并发下非精确，仅供监控）
    size_t approx_size() const {
        intptr_t d = static_cast<intptr_t>(enqueue_pos_.load(std::memory_order_relaxed))
                  - static_cast<intptr_t>(dequeue_pos_.load(std::memory_order_relaxed));
        return d < 0 ? 0 : static_cast<size_t>(d);
    }

    size_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void inc_dropped() { dropped_.fetch_add(1, std::memory_order_relaxed); }
};

} // namespace ltc
