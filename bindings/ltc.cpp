// ltc.cpp - nanobind 绑定：把 ltc 引擎接口暴露给 Python
// 构建见 build_py.ps1。Python 侧可直接回测与实盘：
//   import ltc as v
//   eng = v.BacktestEngine(); eng.load_csv(...); eng.add_strategy(MyStrat("x")); eng.run()
//
// 职责：本文件是 C++ 框架与 Python 之间的唯一桥接层，用 NB_MODULE 把核心类
//       （事件引擎/回测引擎/接口/策略基类/注册表/插件加载器/ini 配置）逐个
//       包装成可被 Python import 的类型与函数。
// 关键设计：
//   - 用 nanobind 的 trampoline（NB_TRAMPOLINE + NB_OVERRIDE）让 Python 能继承
//     Strategy 并覆盖 on_tick/on_bar 等回调；C++ 侧事件仍以 const 引用零拷贝传入。
//   - 数据对象（BarData/TickData/...）按字段 def_rw 暴露，保持与 C++ 内存布局一致。
//   - 本次新增暴露 EventEngine 的 queue_capacity/approx_queue_size/dropped_events，
//     供 Python 侧观测零拷贝无锁队列的水位与背压。
// 与框架其它模块关系：仅依赖 include/ltc 下头文件，不直接包含任何具体策略；
//   策略通过 StrategyRegistry/PluginLoader 在运行时按类型名创建，因此 Python 端
//   既能用内置策略，也能 load 外部 DLL 插件。
// 构建注意：静态链接 nanobind 时需传入 /DNB_STATIC（Windows 下把 NB 符号改为非 DLL 导入）。
#include <memory>
#include <windows.h>  // SetConsoleOutputCP：import ltc 时把控制台切到 UTF-8，根治中文乱码
#include <nanobind/nanobind.h>
#include <nanobind/trampoline.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/function.h>     // Python 可调用对象 <-> std::function（BarGenerator 回调）

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/bar_generator.hpp"
#include "ltc/core/engine.hpp"
#include "ltc/backtest/backtest.hpp"
#include "ltc/backtest/tick_backtest.hpp"
#include "ltc/gateway/csv_gateway.hpp"
#include "ltc/gateway/binance_gateway.hpp"
#include "ltc/gateway/ctp_gateway.hpp"
#include "ltc/gateway/tick_csv_gateway.hpp"
#include "ltc/core/config.hpp"
#include "ltc/core/strategy_registry.hpp"
#include "ltc/core/plugin_loader.hpp"

namespace nb = nanobind;
using namespace ltc;

// ---- Python 可继承的策略基类（trampoline）----
// trampoline 是 nanobind 的关键机制：NB_TRAMPOLINE 声明本类承接 BaseStrategy 的虚函数，
// 每个 override 用 NB_OVERRIDE 把调用转发给 Python 子类真正实现的方法。
// 这样 Python 端 class MyStrat(Strategy) 会被包成 PyStrategy，C++ 事件引擎以
// const TickData& 等零拷贝引用回调时，能正确落到 Python 重载。
struct PyStrategy : BaseStrategy {
    NB_TRAMPOLINE(BaseStrategy);
    void on_init()   override { NB_OVERRIDE(on_init); }
    void on_start()  override { NB_OVERRIDE(on_start); }
    void on_stop()   override { NB_OVERRIDE(on_stop); }
    void on_tick(const TickData& d)    override { NB_OVERRIDE(on_tick, d); }
    void on_bar(const BarData& d)      override { NB_OVERRIDE(on_bar, d); }
    void on_order(const OrderData& d)  override { NB_OVERRIDE(on_order, d); }
    void on_trade(const TradeData& d)  override { NB_OVERRIDE(on_trade, d); }
    void on_timer(int64_t t)           override { NB_OVERRIDE(on_timer, t); }
    void on_contract(const ContractData& d) override { NB_OVERRIDE(on_contract, d); }
};

// 模块入口：NB_MODULE(ltc, m) 生成一个名为 "ltc" 的 Python 扩展模块，m 为模块对象。
// 其下所有 nb::class_ / nb::enum_ / def 调用都在填充该模块的导出表。
NB_MODULE(ltc, m) {
    // 根治中文乱码：C++ 引擎直接 printf 输出 UTF-8 中文，而 Windows 控制台默认
    // 代码页是 GBK(936)。这里在模块加载时把控制台输出/输入代码页切到 UTF-8(65001)，
    // 任何 `import ltc` 的脚本都自动生效，无需每个脚本单独处理。
    ::SetConsoleOutputCP(65001);
    ::SetConsoleCP(65001);

    // 关闭退出时的引用泄漏告警（BarGenerator/Strategy 等 trampoline 对象的
    // 跨语言持有属正常现象，终端用户无需看到这些提示）。
    nb::set_leak_warnings(false);

    m.doc() = "vnpy 风格 C++17 交易框架的 Python 绑定 (回测 / 实盘)";

    // ---------- 枚举 ----------
    nb::enum_<Exchange>(m, "Exchange")
        .value("NONE", Exchange::NONE).value("OKX", Exchange::OKX)
        .value("BINANCE", Exchange::BINANCE).value("BINANCE_USDT", Exchange::BINANCE_USDT)
        .value("SHFE", Exchange::SHFE).value("CFFEX", Exchange::CFFEX)
        .value("DCE", Exchange::DCE).value("CZCE", Exchange::CZCE)
        .value("INE", Exchange::INE).value("SSE", Exchange::SSE)
        .value("SZSE", Exchange::SZSE);

    nb::enum_<Direction>(m, "Direction")
        .value("NONE", Direction::NONE).value("LONG", Direction::LONG)
        .value("SHORT", Direction::SHORT);

    nb::enum_<Offset>(m, "Offset")
        .value("NONE", Offset::NONE).value("OPEN", Offset::OPEN)
        .value("CLOSE", Offset::CLOSE).value("CLOSETODAY", Offset::CLOSETODAY)
        .value("CLOSEYESTERDAY", Offset::CLOSEYESTERDAY);

    nb::enum_<OrderType>(m, "OrderType")
        .value("NONE", OrderType::NONE).value("LIMIT", OrderType::LIMIT)
        .value("MARKET", OrderType::MARKET).value("STOP", OrderType::STOP);

    nb::enum_<Status>(m, "Status")
        .value("NONE", Status::NONE).value("SUBMITTING", Status::SUBMITTING)
        .value("SUBMITTED", Status::SUBMITTED).value("PARTTRADED", Status::PARTTRADED)
        .value("ALLTRADED", Status::ALLTRADED).value("CANCELLED", Status::CANCELLED)
        .value("CANCELLING", Status::CANCELLING).value("REJECTED", Status::REJECTED);

    nb::enum_<Interval>(m, "Interval")
        .value("NONE", Interval::NONE).value("MINUTE", Interval::MINUTE)
        .value("MINUTE3", Interval::MINUTE3).value("MINUTE5", Interval::MINUTE5)
        .value("MINUTE15", Interval::MINUTE15).value("HOUR", Interval::HOUR)
        .value("HOUR4", Interval::HOUR4).value("DAILY", Interval::DAILY);

    nb::enum_<Product>(m, "Product")
        .value("NONE", Product::NONE).value("EQUITY", Product::EQUITY)
        .value("FUTURES", Product::FUTURES).value("OPTION", Product::OPTION)
        .value("INDEX", Product::INDEX).value("FOREX", Product::FOREX)
        .value("SPREAD", Product::SPREAD).value("FUND", Product::FUND)
        .value("BOND", Product::BOND).value("ETF", Product::ETF)
        .value("WARRANT", Product::WARRANT).value("COMBO", Product::COMBO)
        .value("SPOT", Product::SPOT);

    nb::enum_<OptionType>(m, "OptionType")
        .value("NONE", OptionType::NONE).value("CALL", OptionType::CALL)
        .value("PUT", OptionType::PUT);

    // ---------- 数据对象 ----------
    nb::class_<BarData>(m, "BarData")
        .def(nb::init<>())
        .def_rw("symbol", &BarData::symbol)
        .def_rw("exchange", &BarData::exchange)
        .def_rw("vt_symbol", &BarData::vt_symbol)
        .def_rw("datetime", &BarData::datetime)
        .def_rw("interval", &BarData::interval)
        .def_rw("open", &BarData::open)
        .def_rw("high", &BarData::high)
        .def_rw("low", &BarData::low)
        .def_rw("close", &BarData::close)
        .def_rw("volume", &BarData::volume)
        .def_rw("open_interest", &BarData::open_interest);

    nb::class_<TickData>(m, "TickData")
        .def(nb::init<>())
        .def_rw("symbol", &TickData::symbol)
        .def_rw("exchange", &TickData::exchange)
        .def_rw("vt_symbol", &TickData::vt_symbol)
        .def_rw("datetime", &TickData::datetime)
        .def_rw("last_price", &TickData::last_price)
        .def_rw("last_volume", &TickData::last_volume)
        .def_rw("bid_price_1", &TickData::bid_price_1)
        .def_rw("bid_volume_1", &TickData::bid_volume_1)
        .def_rw("ask_price_1", &TickData::ask_price_1)
        .def_rw("ask_volume_1", &TickData::ask_volume_1)
        .def_rw("open_interest", &TickData::open_interest)
        .def_rw("volume", &TickData::volume)
        .def_rw("limit_up", &TickData::limit_up)
        .def_rw("limit_down", &TickData::limit_down);

    nb::class_<OrderData>(m, "OrderData")
        .def(nb::init<>())
        .def_rw("symbol", &OrderData::symbol)
        .def_rw("exchange", &OrderData::exchange)
        .def_rw("vt_symbol", &OrderData::vt_symbol)
        .def_rw("orderid", &OrderData::orderid)
        .def_rw("vt_orderid", &OrderData::vt_orderid)
        .def_rw("direction", &OrderData::direction)
        .def_rw("offset", &OrderData::offset)
        .def_rw("type", &OrderData::type)
        .def_rw("status", &OrderData::status)
        .def_rw("price", &OrderData::price)
        .def_rw("volume", &OrderData::volume)
        .def_rw("traded", &OrderData::traded)
        .def_rw("datetime", &OrderData::datetime)
        .def_rw("gateway_name", &OrderData::gateway_name);

    nb::class_<TradeData>(m, "TradeData")
        .def(nb::init<>())
        .def_rw("symbol", &TradeData::symbol)
        .def_rw("exchange", &TradeData::exchange)
        .def_rw("vt_symbol", &TradeData::vt_symbol)
        .def_rw("orderid", &TradeData::orderid)
        .def_rw("tradeid", &TradeData::tradeid)
        .def_rw("vt_orderid", &TradeData::vt_orderid)
        .def_rw("vt_tradeid", &TradeData::vt_tradeid)
        .def_rw("direction", &TradeData::direction)
        .def_rw("offset", &TradeData::offset)
        .def_rw("price", &TradeData::price)
        .def_rw("volume", &TradeData::volume)
        .def_rw("datetime", &TradeData::datetime)
        .def_rw("gateway_name", &TradeData::gateway_name);

    nb::class_<PositionData>(m, "PositionData")
        .def(nb::init<>())
        .def_rw("symbol", &PositionData::symbol)
        .def_rw("exchange", &PositionData::exchange)
        .def_rw("vt_symbol", &PositionData::vt_symbol)
        .def_rw("direction", &PositionData::direction)
        .def_rw("volume", &PositionData::volume)
        .def_rw("frozen", &PositionData::frozen)
        .def_rw("price", &PositionData::price)
        .def_rw("pnl", &PositionData::pnl)
        .def_rw("gateway_name", &PositionData::gateway_name);

    nb::class_<AccountData>(m, "AccountData")
        .def(nb::init<>())
        .def_rw("accountid", &AccountData::accountid)
        .def_rw("balance", &AccountData::balance)
        .def_rw("frozen", &AccountData::frozen)
        .def_rw("gateway_name", &AccountData::gateway_name);

    nb::class_<ContractData>(m, "ContractData")
        .def(nb::init<>())
        .def_rw("symbol", &ContractData::symbol)
        .def_rw("exchange", &ContractData::exchange)
        .def_rw("vt_symbol", &ContractData::vt_symbol)
        .def_rw("name", &ContractData::name)
        .def_rw("active", &ContractData::active)
        .def_rw("product", &ContractData::product)
        .def_rw("size", &ContractData::size)
        .def_rw("pricetick", &ContractData::pricetick)
        .def_rw("min_volume", &ContractData::min_volume)
        .def_rw("max_volume", &ContractData::max_volume)
        .def_rw("net_position", &ContractData::net_position)
        .def_rw("stop_supported", &ContractData::stop_supported)
        .def_rw("history_data", &ContractData::history_data)
        .def_rw("option_strike", &ContractData::option_strike)
        .def_rw("option_underlying", &ContractData::option_underlying)
        .def_rw("option_type", &ContractData::option_type)
        .def_rw("option_expiry", &ContractData::option_expiry)
        .def_rw("option_portfolio", &ContractData::option_portfolio)
        .def_rw("option_index", &ContractData::option_index)
        .def_rw("gateway_name", &ContractData::gateway_name);

    // ---------- 策略基类（可继承）----------
    nb::class_<BaseStrategy, PyStrategy>(m, "Strategy")
        .def(nb::init<const std::string&>(), nb::arg("name"))
        .def("on_init", &BaseStrategy::on_init)
        .def("on_start", &BaseStrategy::on_start)
        .def("on_stop", &BaseStrategy::on_stop)
        .def("on_tick", &BaseStrategy::on_tick)
        .def("on_bar", &BaseStrategy::on_bar)
        .def("on_order", &BaseStrategy::on_order)
        .def("on_trade", &BaseStrategy::on_trade)
        .def("on_timer", &BaseStrategy::on_timer)
        .def("on_contract", &BaseStrategy::on_contract)
        .def("get_contract", &BaseStrategy::get_contract, nb::arg("vt_symbol"),
             "查询合约元信息(vt_symbol)；未加载/未找到返回 None")
        .def("buy", &BaseStrategy::buy, nb::arg("vt_symbol"), nb::arg("price"),
             nb::arg("volume"), nb::arg("type") = OrderType::LIMIT)
        .def("sell", &BaseStrategy::sell, nb::arg("vt_symbol"), nb::arg("price"),
             nb::arg("volume"), nb::arg("type") = OrderType::LIMIT)
        .def("short", &BaseStrategy::short_, nb::arg("vt_symbol"), nb::arg("price"),
             nb::arg("volume"), nb::arg("type") = OrderType::LIMIT)
        .def("cover", &BaseStrategy::cover, nb::arg("vt_symbol"), nb::arg("price"),
             nb::arg("volume"), nb::arg("type") = OrderType::LIMIT)
        .def("cancel", &BaseStrategy::cancel, nb::arg("vt_orderid"))
        .def("subscribe", &BaseStrategy::subscribe, nb::arg("vt_symbol"),
             "订阅行情：vt_symbol 形如 'rb2609.SHFE'，转发到引擎默认接口")
        .def_prop_ro("name", &BaseStrategy::name);

    // ---------- K 线合成器（vnpy BarGenerator 风格）----------
    // Python 端用法（与 vnpy 三段式一致）：
    //   class MyStrat(v.Strategy):
    //       def __init__(self, name):
    //           super().__init__(name)
    //           self.bg = v.BarGenerator(self.on_bar_1m, 5, self.on_bar_5m,
    //                                    v.Interval.MINUTE)
    //       def on_tick(self, tick):  self.bg.update_tick(tick)   # 喂 tick
    //       def on_bar_1m(self, bar): self.bg.update_bar(bar)     # K 线接力
    //       def on_bar_5m(self, bar): ...                         # 5 分钟 K 策略逻辑
    //   便捷构造：v.BarGenerator(self.on_bar_1m, v.Interval.MINUTE5, self.on_bar_5m)
    using BG = BarGenerator;
    nb::class_<BG>(m, "BarGenerator")
        .def(nb::init<BG::BarCallback>(),
             nb::arg("on_bar"),
             "仅 tick 合成 1 分钟 K：on_bar 收口回调")
        .def(nb::init<BG::BarCallback, int, BG::BarCallback, Interval>(),
             nb::arg("on_bar"), nb::arg("window"),
             nb::arg("on_window_bar"), nb::arg("interval"),
             "构造：on_bar 收 1 分钟 K；window>0 且给出 on_window_bar 时"
             "按 (window, interval) 聚合出目标周期 K")
        .def(nb::init<BG::BarCallback, Interval, BG::BarCallback>(),
             nb::arg("on_bar"), nb::arg("target"), nb::arg("on_window_bar"),
             "便捷构造：target 直接给目标周期（MINUTE5/HOUR4/DAILY 等），"
             "内部自动拆解为 (window, interval)")
        .def("update_tick", &BG::update_tick, nb::arg("tick"),
             "喂入逐笔 tick：跨分钟时收口上一根 1 分钟 K 并触发 on_bar")
        .def("update_bar", &BG::update_bar, nb::arg("bar"),
             "喂入 1 分钟 K：在 on_bar 回调里接力调用，聚合出目标周期 K")
        .def("finish", &BG::finish,
             "强制收口合成中的最后一根 K（数据流结束时调用，避免丢半根）");

    // ---------- 事件引擎（零拷贝 / 无锁队列）----------
    // 本次新增暴露的三个监控接口，便于 Python 端实时观测零拷贝无锁队列状态：
    nb::class_<EventEngine>(m, "EventEngine")
        .def("queue_capacity", &EventEngine::queue_capacity,
             "预分配事件槽总数（无锁环形队列容量）")
        .def("approx_queue_size", &EventEngine::approx_queue_size,
             "队列中近似事件数（监控背压/水位）")
        .def("dropped_events", &EventEngine::dropped_events,
             "因队列满被丢弃的事件数（当前为背压不退让，正常为 0）")
        .def("get_contract", &EventEngine::get_contract, nb::arg("vt_symbol"),
             "按 vt_symbol 查合约元信息，未找到返回 None")
        .def("all_contracts", &EventEngine::all_contracts,
             "返回全部已加载合约元信息列表");

    // ---------- 回测引擎 ----------
    nb::class_<BacktestEngine>(m, "BacktestEngine")
        .def(nb::init<>())
        .def("load_csv", &BacktestEngine::load_csv, nb::arg("path"), nb::arg("vt_symbol"),
             nb::arg("interval") = Interval::MINUTE, nb::arg("has_header") = true)
        .def("add_strategy", &BacktestEngine::add_strategy)
        .def("set_capital", &BacktestEngine::set_capital, nb::arg("capital"))
        .def("set_commission", &BacktestEngine::set_commission, nb::arg("rate"))
        .def("set_slippage", &BacktestEngine::set_slippage, nb::arg("slippage"))
        .def("set_size", &BacktestEngine::set_size, nb::arg("size"))
        .def("set_annualization", &BacktestEngine::set_annualization, nb::arg("n"))
        .def("run", &BacktestEngine::run)
        .def("equity_curve", &BacktestEngine::equity_curve);

    // ---------- 接口（基类 + 两个实现）----------
    nb::class_<BaseGateway>(m, "BaseGateway");

    nb::class_<CsvReplayGateway, BaseGateway>(m, "CsvReplayGateway")
        .def(nb::init<EventEngine*, const std::string&>(), nb::arg("event_engine"),
             nb::arg("name") = "CSV");

    nb::class_<BinanceGateway, BaseGateway>(m, "BinanceGateway")
        .def(nb::init<EventEngine*, std::shared_ptr<HttpClient>,
                      std::shared_ptr<WsClient>, const std::string&>(),
             nb::arg("event_engine"), nb::arg("http") = nullptr, nb::arg("ws") = nullptr,
             nb::arg("name") = "BINANCE");

    nb::class_<CtpGateway, BaseGateway>(m, "CtpGateway")
        .def(nb::init<EventEngine*, const std::string&>(), nb::arg("event_engine"),
             nb::arg("name") = "CTP");

    nb::class_<TickCsvGateway, BaseGateway>(m, "TickCsvGateway")
        .def(nb::init<EventEngine*, const std::string&>(), nb::arg("event_engine"),
             nb::arg("name") = "TICK");

    // ---------- tick 级回测引擎 ----------
    nb::class_<TickBacktestEngine>(m, "TickBacktestEngine")
        .def(nb::init<>())
        .def("load_tick_csv", &TickBacktestEngine::load_tick_csv, nb::arg("path"),
             nb::arg("vt_symbol") = "", nb::arg("has_header") = true)
        .def("add_strategy", &TickBacktestEngine::add_strategy)
        .def("set_bar_interval", &TickBacktestEngine::set_bar_interval,
             nb::arg("interval"),
             "启用 K 线合成：tick 回测中自动聚合成指定周期 K 线并回调 on_bar"
             "（NONE 关闭；MINUTE 收 1 分钟 K，其余周期收对应合成 K）")
        .def("set_capital", &TickBacktestEngine::set_capital, nb::arg("capital"))
        .def("set_commission", &TickBacktestEngine::set_commission, nb::arg("rate"))
        .def("set_slippage", &TickBacktestEngine::set_slippage, nb::arg("slippage"))
        .def("set_size", &TickBacktestEngine::set_size, nb::arg("size"))
        .def("set_annualization", &TickBacktestEngine::set_annualization, nb::arg("n"))
        .def("run", &TickBacktestEngine::run)
        .def("equity_curve", &TickBacktestEngine::equity_curve);

    // ---------- 策略注册表（配置驱动 / 插件化的入口）----------
    // 用法：先 PluginLoader.load("xxx.dll") 注册插件类型，再 create(type, name, params)
    // 这里的静态 lambda 仅把调用转发到进程内单例 StrategyRegistry::instance()，
    // 使 Python 端可直接 v.StrategyRegistry.create(...) 按类型名+参数创建策略。
    nb::class_<StrategyRegistry>(m, "StrategyRegistry")
        .def_static("list", []() { return StrategyRegistry::instance().list(); })
        .def_static("has", [](const std::string& t) {
            return StrategyRegistry::instance().has(t); })
        .def_static("describe", [](const std::string& t) {
            return StrategyRegistry::instance().describe(t); })
        .def_static("dump", []() { return StrategyRegistry::instance().dump(); })
        .def_static("create", [](const std::string& type, const std::string& name,
                                 const StrategyParams& params) {
            return StrategyRegistry::instance().create(type, name, params);
        }, nb::arg("type"), nb::arg("name"), nb::arg("params") = StrategyParams{});

    // ---------- 策略插件加载器（运行时加载外部 DLL）----------
    nb::class_<PluginLoader>(m, "PluginLoader")
        .def_static("load", [](const std::string& path) {
            std::string err;
            bool ok = PluginLoader::instance().load(path, &err);
            return ok ? std::string() : err;   // 成功返回空串，失败返回原因
        }, nb::arg("path"))
        .def_static("loaded", []() { return PluginLoader::instance().loaded(); })
        .def_static("unload_all", []() { PluginLoader::instance().unload_all(); });

    // ---------- ini 配置解析 ----------
    nb::class_<IniConfig>(m, "IniConfig")
        .def(nb::init<>())
        .def("load", &IniConfig::load, nb::arg("path"))
        .def("has_section", &IniConfig::has_section, nb::arg("section"))
        .def("section", &IniConfig::section, nb::arg("section"))
        .def("get", &IniConfig::get, nb::arg("section"), nb::arg("key"),
             nb::arg("def") = std::string())
        .def("get_int", &IniConfig::get_int, nb::arg("section"), nb::arg("key"),
             nb::arg("def") = 0)
        .def("get_double", &IniConfig::get_double, nb::arg("section"), nb::arg("key"),
             nb::arg("def") = 0.0)
        .def("get_bool", &IniConfig::get_bool, nb::arg("section"), nb::arg("key"),
             nb::arg("def") = false)
        .def("last_error", &IniConfig::last_error);

    // ---------- 实盘主引擎 ----------
    // 暴露 add_gateway / add_strategy / connect_all / start / stop 等，使 Python 也能
    // 组装并驱动一个完整实盘引擎（事件引擎、接口、策略都在 C++ 侧运行）。
    nb::class_<MainEngine>(m, "MainEngine")
        .def(nb::init<>())
        .def("add_gateway", &MainEngine::add_gateway)
        .def("set_default_gateway", &MainEngine::set_default_gateway, nb::arg("name"))
        .def("add_strategy", &MainEngine::add_strategy)
        .def("connect_all", &MainEngine::connect_all, nb::arg("settings"))
        .def("subscribe", &MainEngine::subscribe, nb::arg("vt_symbols"),
             "订阅行情：vt_symbols 为 'symbol.exchange' 列表")
        .def("start", &MainEngine::start)
        .def("stop", &MainEngine::stop)
        .def("is_running", &MainEngine::is_running)
        .def("event_engine", &MainEngine::event_engine);
}
