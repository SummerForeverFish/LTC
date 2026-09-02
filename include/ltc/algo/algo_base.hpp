// algo_base.hpp - 算法交易下单模块（C++ 实现）
//
// 参考 D:\files\app\LS_rpc_api\LS\TradeAgent\Algo 的 Python 算法框架移植：
//   - AlgoBase 基类：事件驱动、非阻塞，由策略的 on_timer 统一驱动；
//     内部按「目标持仓 - 当前持仓」拆成多笼/多轮，逐步下单、追单、撤单。
//   - 派生算法：
//       TWAP    时间加权平均价拆单（激进追价，保证成交）
//       VP      VWAP 简化版（按成交量占比拆单）
//       Iceberg 冰山算法（大单拆小单逐笼下，不追价）
//       MidPeg  中间价算法（买卖中间价委托，不追价）
//
// 用法（在策略内）：
//   ltc::algo::TWAPAlgo algo(this);          // 绑定当前策略
//   ... 在 on_start/信号处: algo.start(vt_symbol, target_pos, price, slip, chase, slices, epochs);
//   ... 在 on_timer 回调中: algo.on_timer(t);   // 驱动算法执行
//   ... 需要时: algo.stop(vt_symbol) / algo.stop_all()
//
// 依赖策略提供的接口（均在 BaseStrategy 上）：buy/sell 下单、get_strategy_pos 查持仓、
// get_contract 查合约、get_tick 查最新行情、has_active_orders/cancel_symbol 管理活跃委托、
// subscribe 订阅、write_log 打日志。算法不直接触碰网关/引擎，天然兼容实盘与回测。
#pragma once
#include <string>
#include <map>
#include <vector>
#include <cmath>
#include <chrono>
#include <algorithm>

#include "ltc/core/strategy.hpp"
#include "ltc/core/object.hpp"
#include "ltc/core/util.hpp"

namespace ltc {
namespace algo {

// 算法任务状态（某合约一个进行中的算法任务）
struct AlgoTask {
    std::string vt_symbol;
    double target_pos = 0.0;        // 目标持仓（净持仓，正负表多空）
    double price_hint = 0.0;        // 指定委托价（0 = 按行情对手价）
    int slip_point = 0;             // 滑点（pricetick 倍数）
    double chase_time_ms = 30000.0; // 笼间隔 / 追单间隔（毫秒）
    int n_intervals = 3;            // 拆单笼数
    int max_epochs = 8;             // 最大轮数
    int current_epoch = 0;          // 已执行轮数
    int current_slice = 0;          // 当前笼序号
    double total_volume = 0.0;      // 本轮总调整量
    double remaining_volume = 0.0;  // 本轮剩余量
    bool direction_buy = true;      // 本轮方向：true=买入, false=卖出
    bool active = false;            // 任务是否运行中
    std::vector<double> volume_profile;  // VWAP 用量占比（各笼权重，自动归一化）
    ContractData contract;          // 合约快照（pricetick / min_volume / 涨跌停等）
};

// 算法交易基类：任务调度 + 通用拆单流程。子类覆写 _slice_volume/_slice_price 定制行为。
class AlgoBase {
public:
    explicit AlgoBase(BaseStrategy* strategy) : strategy_(strategy) {}
    virtual ~AlgoBase() = default;

    // 由策略的 on_timer 调用，驱动所有到期任务执行一步（非阻塞）。
    void on_timer(int64_t now_ms) {
        last_now_ms_ = now_ms;
        for (auto it = next_exec_ms_.begin(); it != next_exec_ms_.end();) {
            if (now_ms >= it->second) {
                std::string vt = it->first;
                it = next_exec_ms_.erase(it);
                if (is_active(vt)) _on_execute(vt);
            } else {
                ++it;
            }
        }
    }

    bool is_active(const std::string& vt_symbol) const {
        auto it = tasks_.find(vt_symbol);
        return it != tasks_.end() && it->second.active;
    }

    // 停止某合约算法：停任务并撤销该合约全部活跃委托。
    void stop(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it != tasks_.end()) {
            it->second.active = false;
            _cancel_active_orders(vt_symbol);
        }
        next_exec_ms_.erase(vt_symbol);
    }

    void stop_all() {
        std::vector<std::string> keys;
        for (auto& kv : tasks_) keys.push_back(kv.first);
        for (auto& k : keys) stop(k);
    }

protected:
    // 定时器触发时执行一步（基类提供通用流程，子类可按需覆写）。
    virtual void _on_execute(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it == tasks_.end() || !it->second.active) return;
        if (it->second.remaining_volume > 0) {
            _execute_slice(vt_symbol);
            return;
        }
        // 本轮所有笼已下完：先撤活跃单（无论是否为空），确认无单后进入下一轮
        _cancel_active_orders(vt_symbol);
        if (_has_active_orders(vt_symbol)) {
            _log("[" + tag_ + "] " + vt_symbol + " 撤单中，" + std::to_string(cancel_confirm_ms_ / 1000) + "s后确认");
            _schedule_next(vt_symbol, cancel_confirm_ms_);
        } else {
            _schedule_next(vt_symbol, cancel_confirm_ms_);
            _on_round_done(vt_symbol);
        }
    }

    // 当前笼应下数量（默认均分；VWAP 覆写为按占比）。
    virtual double _slice_volume(const AlgoTask& task) const {
        int n = std::max(1, task.n_intervals);
        double min_vol = _get_min_volume(task.vt_symbol);
        double base = _round_volume(task.total_volume / n, task.vt_symbol);
        if (task.current_slice == n - 1) return task.remaining_volume;
        double v = std::min(base, task.remaining_volume);
        if (v < min_vol) v = task.remaining_volume;
        return v;
    }

    // 当前笼委托价（默认：买=卖一价+滑点，卖=买一价-滑点；TWAP/MidPeg 覆写）。
    virtual double _slice_price(const AlgoTask& task, const TickData& tick) const {
        double pt = task.contract.pricetick > 0 ? task.contract.pricetick : 0.01;
        double base = task.direction_buy ? tick.ask_price_1 : tick.bid_price_1;
        if (base <= 0) base = tick.last_price;
        if (base <= 0) return 0.0;
        double slip = task.slip_point * pt;
        double p = task.direction_buy ? base + slip : base - slip;
        return std::round(p / pt) * pt;
    }

    // ---- 通用工具（子类可复用） ----

    // 初始化任务（子类 start 调用）：校验合约、停旧任务、建新任务。
    bool _init_task(const std::string& vt_symbol, double target_pos, double price,
                    int slip_point, double chase_time, int n_intervals, int epochs,
                    const std::vector<double>& volume_profile) {
        auto contract = strategy_->get_contract(vt_symbol);
        if (!contract) {
            _log("[" + tag_ + "] 未获取到合约 " + vt_symbol + "，使用默认规则(pricetick=0.01,min_volume=1)");
        }
        if (is_active(vt_symbol)) stop(vt_symbol);
        AlgoTask t;
        t.vt_symbol = vt_symbol;
        t.target_pos = target_pos;
        t.price_hint = price;
        t.slip_point = slip_point;
        t.chase_time_ms = chase_time * 1000.0;
        t.n_intervals = std::max(1, n_intervals);
        t.max_epochs = std::max(1, epochs);
        t.volume_profile = volume_profile;
        t.contract = contract ? *contract : ContractData{};
        t.active = true;
        tasks_[vt_symbol] = std::move(t);
        return true;
    }

    // 开始一轮：检查当前持仓与目标，决定方向与总量，然后下第一笼。
    void _start_round(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it == tasks_.end() || !it->second.active) return;
        AlgoTask& task = it->second;

        double current = _get_current_pos(vt_symbol);
        double target = task.target_pos;
        if (std::fabs(current - target) < 1e-8) {
            _finish_task(vt_symbol, "完成: 目标=" + std::to_string(target) + " 当前=" + std::to_string(current));
            return;
        }
        // 已越过目标（超买/超卖）→ 直接完成，不反转方向
        if (task.current_epoch > 0) {
            if (task.direction_buy && current > target) {
                _finish_task(vt_symbol, "完成(超买): 目标=" + std::to_string(target) + " 当前=" + std::to_string(current));
                return;
            }
            if (!task.direction_buy && current < target) {
                _finish_task(vt_symbol, "完成(超卖): 目标=" + std::to_string(target) + " 当前=" + std::to_string(current));
                return;
            }
        }
        if (task.current_epoch >= task.max_epochs) {
            _finish_task(vt_symbol, "达到最大轮数 (" + std::to_string(task.max_epochs) + ")");
            return;
        }
        double change = _round_volume(target - current, vt_symbol);
        double min_vol = _get_min_volume(vt_symbol);
        if (std::fabs(change) < min_vol) {
            _finish_task(vt_symbol, "剩余调整量不足最小单位");
            return;
        }
        task.direction_buy = change > 0;
        task.total_volume = std::fabs(change);
        task.remaining_volume = task.total_volume;
        task.current_slice = 0;
        _log("[" + tag_ + "] 轮 " + std::to_string(task.current_epoch + 1) + "/" +
             std::to_string(task.max_epochs) + " " + vt_symbol + " " +
             (task.direction_buy ? "BUY" : "SELL") + " 总量=" + std::to_string(task.total_volume) +
             " 持仓=" + std::to_string(current));
        _execute_slice(vt_symbol);
    }

    // 下当前笼：算量算价，经策略 buy/sell 报单，然后安排下一笼或收尾。
    void _execute_slice(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it == tasks_.end() || !it->second.active) return;
        AlgoTask& task = it->second;
        double min_vol = _get_min_volume(vt_symbol);
        if (task.remaining_volume < min_vol || task.current_slice >= task.n_intervals) {
            _all_done(vt_symbol);
            return;
        }
        // 持仓已达目标 → 停止下单
        double current = _get_current_pos(vt_symbol);
        if ((task.direction_buy && current >= task.target_pos) ||
            (!task.direction_buy && current <= task.target_pos)) {
            _all_done(vt_symbol);
            return;
        }
        double this_vol = _slice_volume(task);
        if (this_vol < min_vol) { _all_done(vt_symbol); return; }

        auto tick = _get_tick(vt_symbol);
        if (!tick) {
            _log("[" + tag_ + "] " + vt_symbol + " 无行情，已订阅重试");
            _schedule_next(vt_symbol, 3000.0);
            return;
        }
        double price = _slice_price(task, *tick);
        if (price <= 0) {
            _log("[" + tag_ + "] " + vt_symbol + " 价格异常，已订阅重试");
            _schedule_next(vt_symbol, 3000.0);
            return;
        }
        if (task.direction_buy) strategy_->buy(vt_symbol, price, this_vol);
        else strategy_->sell(vt_symbol, price, this_vol);

        current = _get_current_pos(vt_symbol);
        _log("[" + tag_ + "] " + vt_symbol + " 第" + std::to_string(task.current_slice + 1) +
             "/" + std::to_string(task.n_intervals) + "笼 量=" + std::to_string(this_vol) +
             " 价=" + std::to_string(price) + " 持仓=" + std::to_string(current));

        task.remaining_volume -= this_vol;
        task.current_slice += 1;
        if (task.remaining_volume > 0 && task.current_slice < task.n_intervals) {
            _schedule_next(vt_symbol, task.chase_time_ms);
        } else {
            _all_done(vt_symbol);
        }
    }

    // 本轮所有笼已下完：等 chase_time 后由 _on_execute 撤单并进入下一轮。
    void _all_done(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it != tasks_.end()) _schedule_next(vt_symbol, it->second.chase_time_ms);
    }

    // 所有笼已下且无活跃委托 → 进入下一轮。
    void _on_round_done(const std::string& vt_symbol) {
        auto it = tasks_.find(vt_symbol);
        if (it != tasks_.end()) {
            it->second.current_epoch += 1;
            _start_round(vt_symbol);
        }
    }

    std::optional<ContractData> _get_contract(const std::string& vt_symbol) {
        return strategy_->get_contract(vt_symbol);
    }
    double _get_current_pos(const std::string& vt_symbol) {
        return strategy_->get_strategy_pos(vt_symbol).volume;
    }
    double _get_min_volume(const std::string& vt_symbol) const {
        auto c = strategy_->get_contract(vt_symbol);
        return (c && c->min_volume > 0) ? c->min_volume : 1.0;
    }
    double _round_volume(double v, const std::string& vt_symbol) const {
        double step = _get_min_volume(vt_symbol);
        if (step <= 0) return v;
        return std::round(v / step) * step;
    }
    bool _has_active_orders(const std::string& vt_symbol) const {
        return strategy_->has_active_orders(vt_symbol);
    }
    void _cancel_active_orders(const std::string& vt_symbol) {
        strategy_->cancel_symbol(vt_symbol);
    }
    void _schedule_next(const std::string& vt_symbol, double delay_ms) {
        next_exec_ms_[vt_symbol] = last_now_ms_ + (int64_t)delay_ms;
    }
    void _finish_task(const std::string& vt_symbol, const std::string& reason = "") {
        auto it = tasks_.find(vt_symbol);
        if (it != tasks_.end()) it->second.active = false;
        next_exec_ms_.erase(vt_symbol);
        if (!reason.empty()) _log("[" + tag_ + "] " + vt_symbol + " " + reason);
    }
    std::optional<TickData> _get_tick(const std::string& vt_symbol) {
        auto t = strategy_->get_tick(vt_symbol);
        if (!t) strategy_->subscribe(vt_symbol);
        return t;
    }
    void _log(const std::string& msg) { strategy_->write_log(msg); }

    BaseStrategy* strategy_;
    std::map<std::string, int64_t> next_exec_ms_;   // vt_symbol -> 下次执行时间(ms)
    std::map<std::string, AlgoTask> tasks_;         // vt_symbol -> 任务状态
    int64_t last_now_ms_ = 0;                       // 最近一次 on_timer 时间戳
    std::string tag_ = "Algo";                      // 日志前缀
    double cancel_confirm_ms_ = 1000.0;             // 撤单后确认等待（TWAP 用 2000）
};

// ---------- 具体算法 ----------

// TWAP：时间加权平均价拆单。激进追价（买=min(卖一*1.01,涨停价)，卖=max(买一*0.99,跌停价)），
//       尽量保证成交；其余流程（多轮、撤单、补单）与基类一致。
class TWAPAlgo : public AlgoBase {
public:
    using AlgoBase::AlgoBase;
    explicit TWAPAlgo(BaseStrategy* s) : AlgoBase(s) { tag_ = "TWAP"; cancel_confirm_ms_ = 2000.0; }

    bool start(const std::string& vt_symbol, double target_pos, double price = 0.0,
               int slip_point = 0, double chase_time = 30.0, int n_intervals = 3, int epochs = 8) {
        if (!_init_task(vt_symbol, target_pos, price, slip_point, chase_time, n_intervals, epochs, {}))
            return false;
        _log("[" + tag_ + "] 启动 " + vt_symbol + " 目标=" + std::to_string(target_pos) +
             " 笼数=" + std::to_string(n_intervals) + " 轮数=" + std::to_string(epochs));
        _start_round(vt_symbol);
        return true;
    }

protected:
    double _slice_price(const AlgoTask& task, const TickData& tick) const override {
        double pt = task.contract.pricetick > 0 ? task.contract.pricetick : 0.01;
        if (task.direction_buy) {
            double p = tick.ask_price_1 > 0 ? tick.ask_price_1 : tick.last_price;
            if (p <= 0) return 0.0;
            double cap = tick.limit_up > 0 ? tick.limit_up : p * 1.0;   // 涨停价封顶
            return std::ceil(std::min(p * 1.01, cap) / pt) * pt;       // 向上取整到最小价位
        } else {
            double p = tick.bid_price_1 > 0 ? tick.bid_price_1 : tick.last_price;
            if (p <= 0) return 0.0;
            double floor_ = tick.limit_down > 0 ? tick.limit_down : p * 1.0;
            return std::floor(std::max(p * 0.99, floor_) / pt) * pt;
        }
    }
};

// VP：VWAP 简化版，按成交量占比拆单（默认均分），价用对手价±滑点，不追价。
class VPAlgo : public AlgoBase {
public:
    using AlgoBase::AlgoBase;
    explicit VPAlgo(BaseStrategy* s) : AlgoBase(s) { tag_ = "VP"; }

    bool start(const std::string& vt_symbol, double target_pos, double price = 0.0,
               int slip_point = 0, double chase_time = 30.0, int n_intervals = 3, int epochs = 8,
               std::vector<double> volume_profile = {}) {
        std::vector<double> profile = volume_profile;
        if (profile.empty()) {
            profile.resize(std::max(1, n_intervals));
            for (auto& v : profile) v = 1.0 / profile.size();
        } else {
            double s = 0; for (double v : profile) s += v;
            if (s > 0) for (double& v : profile) v /= s;   // 归一化
        }
        if (!_init_task(vt_symbol, target_pos, price, slip_point, chase_time, n_intervals, epochs, profile))
            return false;
        _log("[" + tag_ + "] 启动 " + vt_symbol + " 目标=" + std::to_string(target_pos) +
             " 笼数=" + std::to_string(profile.size()));
        _start_round(vt_symbol);
        return true;
    }

protected:
    // 按成交量占比拆单：本笼量 = 总量 * 该笼权重（末笼补剩余）。
    double _slice_volume(const AlgoTask& task) const override {
        double min_vol = _get_min_volume(task.vt_symbol);
        int idx = std::min(task.current_slice, (int)task.volume_profile.size() - 1);
        double ratio = task.volume_profile.empty()
                           ? (1.0 / std::max(1, task.n_intervals))
                           : task.volume_profile[idx];
        if (task.current_slice == task.n_intervals - 1) return task.remaining_volume;
        double v = std::min(_round_volume(task.total_volume * ratio, task.vt_symbol),
                            task.remaining_volume);
        if (v < min_vol) v = task.remaining_volume;
        return v;
    }
};

// Iceberg：冰山算法，大单拆成小单逐笼下，不追价（对手价±滑点）。
class IcebergAlgo : public AlgoBase {
public:
    using AlgoBase::AlgoBase;
    explicit IcebergAlgo(BaseStrategy* s) : AlgoBase(s) { tag_ = "Iceberg"; }

    bool start(const std::string& vt_symbol, double target_pos, double price = 0.0,
               int slip_point = 0, double chase_time = 10.0, int n_intervals = 5, int epochs = 8) {
        if (!_init_task(vt_symbol, target_pos, price, slip_point, chase_time, n_intervals, epochs, {}))
            return false;
        _log("[" + tag_ + "] 启动 " + vt_symbol + " 目标=" + std::to_string(target_pos) +
             " 笼数=" + std::to_string(n_intervals));
        _start_round(vt_symbol);
        return true;
    }
};

// MidPeg：中间价算法，以买卖中间价委托，不追价。
class MidPegAlgo : public AlgoBase {
public:
    using AlgoBase::AlgoBase;
    explicit MidPegAlgo(BaseStrategy* s) : AlgoBase(s) { tag_ = "MidPeg"; }

    bool start(const std::string& vt_symbol, double target_pos, double price = 0.0,
               int slip_point = 0, double chase_time = 10.0, int n_intervals = 3, int epochs = 8) {
        if (!_init_task(vt_symbol, target_pos, price, slip_point, chase_time, n_intervals, epochs, {}))
            return false;
        _log("[" + tag_ + "] 启动 " + vt_symbol + " 目标=" + std::to_string(target_pos) +
             " 笼数=" + std::to_string(n_intervals));
        _start_round(vt_symbol);
        return true;
    }

protected:
    // 中间价 ± 滑点。
    double _slice_price(const AlgoTask& task, const TickData& tick) const override {
        double pt = task.contract.pricetick > 0 ? task.contract.pricetick : 0.01;
        double mid = (tick.ask_price_1 + tick.bid_price_1) / 2;
        if (mid <= 0) mid = tick.last_price;
        if (mid <= 0) return 0.0;
        double slip = task.slip_point * pt;
        double p = task.direction_buy ? mid + slip : mid - slip;
        return std::round(p / pt) * pt;
    }
};

// ---- 策略基类集成：AlgoContext 持有四算法各一实例 ----
// 声明在 ltc/core/strategy.hpp（前向），实现在这里；BaseStrategy 惰性持有
// 一个 AlgoContext，策略只需调 send_target_pos_twap() 等，由 handle_timer 自动驱动。
struct AlgoContext {
    explicit AlgoContext(BaseStrategy* st) : twap(st), vp(st), iceberg(st), midpeg(st) {}
    TWAPAlgo twap;
    VPAlgo vp;
    IcebergAlgo iceberg;
    MidPegAlgo midpeg;
};

// 转发函数（make_algo_context / drive_algos / algo_start_* / stop_*）定义在
// src/algo_engine.cpp（声明见 ltc/core/strategy.hpp 前向），避免多 TU 链接歧义。

} // namespace algo
} // namespace ltc
