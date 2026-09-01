// bar_generator.hpp - K 线合成器 (vnpy BarGenerator 风格, C++17, header-only)
//
// 职责：把更细粒度的行情聚合成 K 线并按需推给策略，对应 vnpy 的 BarGenerator：
//   1) update_tick(tick)  : 逐笔 tick 合成 1 分钟 K 线（tick 行情源的必经之路），
//                           每跨入新的一分钟时收口上一根并触发 on_bar 回调；
//   2) update_bar(bar)    : 把 1 分钟 K 线继续聚合成 N 分钟 / N 小时 / 日线
//                           （window_bar），到窗口边界时收口并触发 on_window_bar。
//
// 典型用法（与 vnpy 完全一致的三段式）：
//   class MyStrat : BaseStrategy {
//     BarGenerator bg_;                       // 成员持有
//     MyStrat(...) : BaseStrategy("x"),
//       bg_([this](const BarData& b){ on_bar1m(b); },          // 1 分钟 K 回调
//           5, [this](const BarData& b){ on_bar5m(b); },        // 5 分钟 K 回调
//           Interval::MINUTE) { }                               // 聚合单位：分钟
//     void on_tick(const TickData& t) override { bg_.update_tick(t); }  // 喂 tick
//     void on_bar1m(const BarData& b)            { bg_.update_bar(b);  }  // K 线接力
//   };
//
// 关键设计（对照 vnpy/trader/generator.py）：
//   - 1 分钟 bar.datetime 取 tick 时间向下取整到分钟（bar 按起始时刻标注）；
//   - tick 价格：bid1/ask1 均有效时取中间价（贴近真实成交带），否则用 last_price；
//   - tick 成交量：优先按累计量差分（tick.volume - 上一笔累计量），
//     差分非法（非累计数据源）时回退用本笔 last_volume，保证两类数据源都能正确聚合；
//   - 窗口聚合采用 vnpy 经典「先并入再判断边界」：窗口 K 的 datetime = 窗口内
//     第一根源 K 的取整时间，收口时机为源 K 落在边界上（分钟取模 / 小时整点 / 跨日），
//     每个窗口恰好包含 window 根源 K，无未来函数；
//   - 全部回调为 std::function<void(const BarData&)>，C++ 策略用 lambda 捕获 this，
//     Python 侧直接传可调用对象（nanobind 自动转换）。
//
// 与框架其它模块关系：
//   - 产出/消费 BarData/TickData（object.hpp），纯本地计算、不依赖事件引擎；
//   - 实盘：策略在 on_tick 中喂 BarGenerator（vnpy 用法）；
//   - 回测：TickBacktestEngine::set_bar_interval 内部也用它把 tick 聚合成 K 线。
#pragma once
#include <string>
#include <memory>
#include <functional>
#include <cstdint>

#include "ltc/core/object.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// K 线合成器：tick -> 1分钟K线 -> N分钟/N小时/日K线，逐级收口回调。
class BarGenerator {
public:
    // K 线收口回调：收口时收到一根完整 BarData（const 引用，零拷贝）。
    using BarCallback = std::function<void(const BarData&)>;

    // 把目标周期拆解为 (聚合数量 window, 聚合单位 unit)：
    //   MINUTE->1 分钟, MINUTE3->3 分钟, MINUTE5->5 分钟, MINUTE15->15 分钟,
    //   HOUR->1 小时, HOUR4->4 小时, DAILY->1 天。
    // 便于用 Interval 枚举一步构造合成器（见便捷构造函数）。
    static void decompose_interval(Interval itv, int& window, Interval& unit) {
        switch (itv) {
            case Interval::MINUTE3:  window = 3;  unit = Interval::MINUTE; break;
            case Interval::MINUTE5:  window = 5;  unit = Interval::MINUTE; break;
            case Interval::MINUTE15: window = 15; unit = Interval::MINUTE; break;
            case Interval::HOUR:     window = 1;  unit = Interval::HOUR;   break;
            case Interval::HOUR4:    window = 4;  unit = Interval::HOUR;   break;
            case Interval::DAILY:    window = 1;  unit = Interval::DAILY;  break;
            default:                 window = 1;  unit = Interval::MINUTE; break; // MINUTE/NONE 兜底
        }
    }

    // ---- 构造 ----
    // 默认构造：仅作成员占位，回调可后续用 set_on_bar/set_window 配置。
    BarGenerator() = default;

    // 基础构造（vnpy 同参语义）：on_bar 为 1 分钟 K 回调；window>0 且 on_window_bar
    // 非空时，再用 update_bar 把 1 分钟 K 聚合成 window 根一收口的窗口 K；
    // interval 为聚合单位（MINUTE / HOUR / DAILY）。
    BarGenerator(BarCallback on_bar, int window = 0,
                 BarCallback on_window_bar = nullptr,
                 Interval interval = Interval::MINUTE)
        : on_bar_(std::move(on_bar)),
          window_(window > 0 ? window : 0),
          on_window_bar_(std::move(on_window_bar)),
          interval_(interval) {}

    // 便捷构造：直接按目标周期构造，如 Interval::MINUTE5 / HOUR4 / DAILY，
    // 内部自动拆解为 (window, unit)。on_bar 收 1 分钟 K，on_window_bar 收目标周期 K。
    BarGenerator(BarCallback on_bar, Interval target, BarCallback on_window_bar)
        : BarGenerator(std::move(on_bar), 0, std::move(on_window_bar), Interval::MINUTE) {
        decompose_interval(target, window_, interval_);
    }

    // ---- 配置 ----
    void set_on_bar(BarCallback cb) { on_bar_ = std::move(cb); }
    void set_window(int window, BarCallback cb, Interval interval = Interval::MINUTE) {
        window_ = window > 0 ? window : 0;
        on_window_bar_ = std::move(cb);
        interval_ = interval;
    }

    // 未收口的 1 分钟 K（可能有：跨分钟时才收口）；窗口 K 同理。
    const BarData* current_bar() const { return bar_ ? &*bar_ : nullptr; }
    const BarData* current_window_bar() const { return window_bar_ ? &*window_bar_ : nullptr; }

    // ------------------------------------------------------------------
    // 1) tick -> 1 分钟 K 线（实盘喂实时 tick；tick 回测引擎也走这里）
    // ------------------------------------------------------------------
    // 逐笔更新：同一分钟内只累加 high/low/close/volume；跨入新分钟时收口上一根
    // （触发 on_bar），并以上一笔 tick 开出新 bar。bar.datetime 为该分钟结束时刻
    // （起始+60s，即收口时刻），贴近真实 K 线“以收盘价收口”的语义。
    void update_tick(const TickData& tick) {
        double price = tick.last_price;
        // 买卖一档均有效时取中间价（vnpy 口径，抹去单边报价噪声）；否则用最新价
        if (tick.bid_price_1 > 0 && tick.ask_price_1 > 0) {
            price = (tick.bid_price_1 + tick.ask_price_1) * 0.5;
        }
        if (price <= 0) return;                       // 无有效价格，丢弃脏 tick

        int64_t minute = floor_minute(tick.datetime); // 本笔 tick 所属分钟（起始毫秒）
        if (!bar_) {
            // 首笔 tick：开出新一分钟的 bar
            bar_.reset(new BarData());
            bar_->symbol = tick.symbol;
            bar_->exchange = tick.exchange;
            bar_->vt_symbol = tick.to_vt_symbol();
            bar_->interval = Interval::MINUTE;
            bar_->datetime = minute;
            bar_->open = bar_->high = bar_->low = price;
            bar_->close = price;
            bar_->volume = 0;
            bar_->open_interest = tick.open_interest;
            last_cum_volume_ = tick.volume;           // 记录累计量基准
        } else if (minute != bar_->datetime) {
            // 跨入新分钟：收口上一根。datetime 由“起始分钟”改为“结束分钟”(起始+60s)，
            // 即该根 1 分钟 K 收口时刻（close 已是最后一笔价格），再触发 on_bar
            bar_->datetime = bar_->datetime + 60000;
            bar_->vt_symbol = bar_->to_vt_symbol();
            if (on_bar_) on_bar_(*bar_);
            bar_.reset();
            update_tick(tick);                        // 递归开出新一分钟的 bar
            return;
        } else {
            // 同一分钟：滚动更新
            if (price > bar_->high) bar_->high = price;
            if (price < bar_->low)  bar_->low  = price;
            bar_->close = price;
            bar_->volume += tick_volume(tick);        // 累计本分钟成交量
            bar_->open_interest = tick.open_interest; // 持仓量取最新
            last_cum_volume_ = tick.volume;
        }
        last_tick_ms_ = tick.datetime;
    }

    // ------------------------------------------------------------------
    // 2) 1 分钟 K -> N 分钟 / N 小时 / 日 K 线（在 on_bar 回调里接力喂入）
    // ------------------------------------------------------------------
    // vnpy 经典「先并入、再判断边界」：源 bar 先并入窗口，随后若源 bar 落在
    // 窗口边界上则收口并触发 on_window_bar。窗口 K 的 datetime = 窗口内首根源 K
    // 的取整时间；window=0 或未设置 on_window_bar 时本函数为空操作。
    void update_bar(const BarData& bar) {
        if (window_ <= 0 || !on_window_bar_) return;

        // 先算边界（日线收口时若用新一天 bar 的 datetime 会污染窗口标签，需在此之前判定）
        bool finished = false;
        if (interval_ == Interval::MINUTE) {
            finished = (minute_of_hour(bar.datetime) % window_) == 0;
        } else if (interval_ == Interval::HOUR) {
            if (minute_of_hour(bar.datetime) == 0)
                finished = (window_ <= 1) || ((hour_of_day(bar.datetime) % window_) == 0);
        } else if (interval_ == Interval::DAILY) {
            finished = window_bar_ && day_index(bar.datetime) != day_index(window_bar_->datetime);
        }

        if (!window_bar_) {
            // 开新窗口：拷贝源 bar 元信息，datetime 先对齐到窗口起点（随后会被推进到收口时刻）
            window_bar_.reset(new BarData(bar));
            window_bar_->interval = interval_;
            window_bar_->datetime = aligned_start(bar.datetime);
        } else {
            // 并入当前窗口：滚动 high/low/close，量与持仓累加/取新；
            // 并把窗口 datetime 推进到“收口那根”的结束时间（日线跨日收口时除外，保留上一交易日标签）
            if (bar.high > window_bar_->high) window_bar_->high = bar.high;
            if (bar.low  < window_bar_->low)  window_bar_->low  = bar.low;
            window_bar_->close = bar.close;
            window_bar_->volume += bar.volume;
            window_bar_->open_interest = bar.open_interest;
            if (!(interval_ == Interval::DAILY && finished))
                window_bar_->datetime = bar.datetime;
        }

        if (finished) {
            window_bar_->vt_symbol = window_bar_->to_vt_symbol();
            on_window_bar_(*window_bar_);
            window_bar_.reset();
        }
    }

    // 收口未完成的 K 线：数据流结束后调用（回测收尾 / 停机时），
    // 把仍在合成中的 1 分钟 K 与窗口 K 强制推给回调，避免丢掉最后半根。
    void finish() {
        if (window_bar_ && on_window_bar_) {
            window_bar_->vt_symbol = window_bar_->to_vt_symbol();
            on_window_bar_(*window_bar_);
            window_bar_.reset();
        }
        if (bar_ && on_bar_) {
            bar_->vt_symbol = bar_->to_vt_symbol();
            on_bar_(*bar_);
            bar_.reset();
        }
    }

private:
    // ---- 时间工具（毫秒时间戳 -> 分钟/小时/日）----
    // 向下取整到分钟（bar 按起始时刻标注，与 vnpy replace(second=0) 一致）
    static int64_t floor_minute(int64_t ms) { return ms - (ms % 60000); }
    // 分钟位（0-59），用于分钟窗口边界与小时整点判断
    static int minute_of_hour(int64_t ms) { return int((ms / 60000) % 60); }
    // 小时位（0-23），用于 N 小时窗口边界判断
    static int hour_of_day(int64_t ms) { return int((ms / 3600000) % 24); }
    // 天序号（自纪元起的天数），用于跨日判断（不受时区影响的稳定日界）
    static int64_t day_index(int64_t ms) { return ms / 86400000; }

    // 窗口 K 的起始时间对齐：分钟窗口向下取整到 window 分钟；小时/日窗口
    // 对齐到整小时/整日。让窗口 K 的 datetime 落在规整边界上，便于指标对齐。
    int64_t aligned_start(int64_t ms) const {
        if (interval_ == Interval::MINUTE) {
            int64_t m = ms / 60000;
            return (m - m % window_) * 60000;
        }
        if (interval_ == Interval::HOUR) {
            int64_t h = ms / 3600000;
            return (h - h % window_) * 3600000;
        }
        return day_index(ms) * 86400000;   // DAILY：对齐到当日 0 点（UTC）
    }

    // 计算本笔 tick 相对上一笔的成交量：
    //   - 累计量源（CTP/币安 WS 等，volume 单调增）：用差分；
    //   - 非累计源（部分 CSV，volume 即本笔量）：差分非法时回退 last_volume。
    double tick_volume(const TickData& tick) const {
        if (tick.volume > last_cum_volume_ && last_cum_volume_ > 0)
            return tick.volume - last_cum_volume_;        // 累计量差分
        return tick.last_volume > 0 ? tick.last_volume : 0; // 本笔量回退
    }

    BarCallback on_bar_;            // 1 分钟 K 收口回调（tick 聚合路径）
    int window_ = 0;                // 窗口聚合根数（0 = 不做窗口聚合）
    BarCallback on_window_bar_;     // 窗口 K 收口回调（N 分钟/小时/日）
    Interval interval_ = Interval::MINUTE; // 窗口聚合单位

    std::unique_ptr<BarData> bar_;              // 合成中的 1 分钟 K
    std::unique_ptr<BarData> window_bar_;       // 合成中的窗口 K
    double last_cum_volume_ = 0;                // 上一笔 tick 的累计成交量（差分基准）
    int64_t last_tick_ms_ = 0;                  // 上一笔 tick 时间戳
};

} // namespace ltc
