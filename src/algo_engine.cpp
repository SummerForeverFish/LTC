// algo_engine.cpp - 算法交易模块的独立编译单元
//
// 集中定义 BaseStrategy 与算法模块（ltc/algo/algo_base.hpp）之间的转发函数，
// 声明见 ltc/core/strategy.hpp 顶部（前向声明，避免策略基类与算法模块循环包含）。
// 放在独立 TU（非 inline）可保证：事件引擎/主程序/绑定各 TU 只依赖声明，
// 定义唯一，链接无歧义。
#include "ltc/algo/algo_base.hpp"

namespace ltc {
namespace algo {

std::shared_ptr<AlgoContext> make_algo_context(BaseStrategy* st) {
    return std::make_shared<AlgoContext>(st);
}

void drive_algos(AlgoContext& ctx, int64_t t) {
    ctx.twap.on_timer(t);
    ctx.vp.on_timer(t);
    ctx.iceberg.on_timer(t);
    ctx.midpeg.on_timer(t);
}

bool algo_start_twap(AlgoContext& ctx, const std::string& vt, double target, double price,
                     int slip, double chase, int n, int e) {
    return ctx.twap.start(vt, target, price, slip, chase, n, e);
}

bool algo_start_vp(AlgoContext& ctx, const std::string& vt, double target, double price,
                   int slip, double chase, int n, int e, const std::vector<double>& profile) {
    return ctx.vp.start(vt, target, price, slip, chase, n, e, profile);
}

bool algo_start_iceberg(AlgoContext& ctx, const std::string& vt, double target, double price,
                        int slip, double chase, int n, int e) {
    return ctx.iceberg.start(vt, target, price, slip, chase, n, e);
}

bool algo_start_midpeg(AlgoContext& ctx, const std::string& vt, double target, double price,
                       int slip, double chase, int n, int e) {
    return ctx.midpeg.start(vt, target, price, slip, chase, n, e);
}

void stop_algo(AlgoContext& ctx, const std::string& vt) {
    ctx.twap.stop(vt);
    ctx.vp.stop(vt);
    ctx.iceberg.stop(vt);
    ctx.midpeg.stop(vt);
}

void stop_all_algos(AlgoContext& ctx) {
    ctx.twap.stop_all();
    ctx.vp.stop_all();
    ctx.iceberg.stop_all();
    ctx.midpeg.stop_all();
}

} // namespace algo
} // namespace ltc
