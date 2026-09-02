// twap_demo_strategy.hpp - TWAP 算法拆单演示策略（最简写法）
//
// 职责：演示算法交易已内置到 BaseStrategy 基类后，写策略只需一行调用：
//   on_start 里 send_target_pos_twap(vt_symbol, target_pos, ...) 即可。
// 拆单/追单/撤单/多轮由框架自动完成：
//   - MainEngine 定时器线程投递 TIMER -> handle_timer -> 自动驱动算法 -> 再转调用户 on_timer；
//   - 持仓账本由框架自动维护（handle_trade），算法读 get_strategy_pos 判断是否继续下单；
//   - 停止时 handle_stop 自动停算法并撤活跃委托。
//
// 用法（config 参数）：
//   type=TwapDemo  params=vt_symbol=rb2610.SHFE,target_pos=5,price=0,slip_point=0,
//                        chase_time=5,n_intervals=4,epochs=6,live=1
//   live=0 时只打印拆单流程日志不下单（dry-run）；live=1 才真实报单。
#pragma once
#include <string>

#include "ltc/core/object.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/util.hpp"
#include "ltc/algo/algo_base.hpp"   // 让 exe 内联函数有定义（算法上下文在此实现）

namespace ltc {

// TWAP 算法演示策略：启动即把持仓调至 target_pos，其余全交给基类算法模块。
class TwapDemoStrategy : public BaseStrategy {
public:
    TwapDemoStrategy(const std::string& name, const std::string& vt_symbol,
                     double target_pos, double price, int slip_point, double chase_time,
                     int n_intervals, int epochs, bool live)
        : BaseStrategy(name), vt_symbol_(vt_symbol), target_pos_(target_pos),
          price_(price), slip_point_(slip_point), chase_time_(chase_time),
          n_intervals_(n_intervals), epochs_(epochs), live_(live) {}

    void on_init() override {
        subscribe(vt_symbol_);
        write_log("[TwapDemo] 初始化 " + vt_symbol_ + " 目标持仓=" + std::to_string(target_pos_) +
                  " live=" + std::to_string(live_));
    }

    void on_start() override {
        write_log("[TwapDemo] 启动 TWAP 拆单 (live=" + std::to_string(live_) + ")");
        if (live_) {
            // 唯一一行调用：拆单/追单/撤单/多轮全部由基类自动完成
            send_target_pos_twap(vt_symbol_, target_pos_, price_, slip_point_,
                                 chase_time_, n_intervals_, epochs_);
        } else {
            write_log("[TwapDemo/DRY] 不真实报单，仅演示算法流程日志");
        }
    }

    void on_order(const OrderData& o) override {
        if (o.status == Status::REJECTED)
            write_log("[TwapDemo] 委托被拒: " + o.vt_orderid);
    }

private:
    std::string vt_symbol_;
    double target_pos_;
    double price_;
    int slip_point_;
    double chase_time_;
    int n_intervals_;
    int epochs_;
    bool live_;
};

} // namespace ltc
