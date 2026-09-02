# 性能与低延迟

事件驱动内核已为高频/低延迟实盘做了零拷贝、无锁化改造。

## 改造点

| 改造点 | 旧实现 | 新实现 | 收益 |
| --- | --- | --- | --- |
| 事件载荷 | `std::any`（类型擦除 + 堆分配 + RTTI） | `std::variant`（内联存储，读时返回 `const&`） | 零拷贝读取、无堆分配 |
| 事件传输 | `std::queue<Event>` + `std::mutex` + `condvar` | 有界无锁 **MPMC 环形队列**（Vyukov，预分配、无 ABA、缓存行隔离） | 多生产者无锁入队、消费者零分配出队 |
| 分派 | `unordered_map<EventType, vector<function>>` 查找 | 按类型**扁平分桶** + 热路径 `register_strategy` 直调虚函数 | 无 map 查找、无 `std::function` 间接 |
| 网关推送 | `on_tick(const T&)` 拷贝进 `any` | `on_tick(T)` 按值 + `std::move` 进 variant | 局部对象零拷贝入队 |
| 线程模型 | 条件变量阻塞唤醒 | 忙轮询 + 空闲 `yield` 退让 | 低延迟、空闲不占满核 |

队列满时 `put` 自旋退让（背压）**不丢事件**。`EventEngine` 暴露 `queue_capacity()` / `approx_queue_size()` / `dropped_events()` 用于运行期监控水位。

## 吞吐基准

`ltc bench`（1 生产者 / 1 消费者，预分配 65536 槽）：约 **8.6 M events/sec**。`run_bench()` 用 `d.datetime` 写入发射纳秒戳、消费时求差，附带**平均/最大端到端延迟**。

```text
build/ltc.exe bench
[bench] 零拷贝事件队列吞吐: 8.6 M events/sec
        队列容量=65536  剩余=...  丢弃=0
        平均时延=... ns  最大时延=... ns
```

## 仍为待办的高频项

- **异步下单**：当前币安网关为 Stub（同步），真实接入需 WebSocket 而非阻塞 REST；CTP 为异步（`ReqOrderInsert`）。
- **批处理**：网关推送 / 撮合可批处理以降低每事件开销。
- **策略热路径限留 C++**：插件化已支持纯 C++ 策略，关键路径避免 Python 解释器开销（Python 回调经 trampoline 调用，会按 CPython 规则串行于 GIL，见 [FAQ](FAQ.md)）。
- **SPI 回调线程**：CTP 的 `OnRtn*` 目前在 CTP 线程直接回调网关（未强制切回事件线程），高频实盘前建议加一层投递到 `EventEngine`。
