// main.cpp - 入口：配置驱动 + 策略插件化
//
// 设计要点：本文件不再 #include 任何 strategies/*.hpp。策略通过 StrategyRegistry 按
// 「类型名 + 参数」在运行时创建，类型来源有二：
//   1) 内置策略：由 strategies/register_builtin.cpp 自注册（编译期带入 exe）
//   2) 外部插件：由 [plugins] 配置的 DLL 在运行时 LoadLibrary 后注册
// 因此新增/替换策略既不用改主程序，也不必重编主程序（走插件时）。
//
// 用法：
//   ltc run [cfg.ini]   # 推荐：模式/插件/策略/参数全部由配置决定（默认 config/run.ini）
//   ltc list            # 列出当前已注册的全部策略类型（含插件提供）
//   ltc <mode>          # 兼容旧用法：模式 + 该模式的默认策略类型
//        mode: backtest | live_csv | live_binance | live_ctp [cfg.ini] | tick_csv | tick_backtest
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <windows.h>   // SetConsoleOutputCP：启动时把控制台切到 UTF-8，根治中文乱码

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/strategy.hpp"
#include "ltc/core/engine.hpp"
#include "ltc/core/util.hpp"
#include "ltc/core/config.hpp"
#include "ltc/core/strategy_registry.hpp"
#include "ltc/core/plugin_loader.hpp"
#include "ltc/backtest/backtest.hpp"
#include "ltc/backtest/tick_backtest.hpp"
#include "ltc/gateway/csv_gateway.hpp"
#include "ltc/gateway/binance_gateway.hpp"
#include "ltc/gateway/ctp_gateway.hpp"
#include "ltc/gateway/tick_csv_gateway.hpp"

using namespace ltc;

// ---------------------------------------------------------------- 运行配置

struct StrategySpec {
    std::string type;                 // 注册表中的类型名（内置或插件提供）
    std::string name;                 // 实例名（日志/委托 reference 用）
    StrategyParams params;            // 策略参数
};

struct RunConfig {
    std::string mode = "tick_backtest";
    std::string data_file;            // 为空时按 mode 取默认
    std::string vt_symbol = "BTCUSDT.BINANCE_USDT";
    double capital = 1'000'000.0;
    double commission = 0.0004;
    double slippage = 0.0;
    int speed_ms = 0;
    int run_seconds = 15;
    std::vector<std::string> plugins;
    std::vector<StrategySpec> strategies;
    SectionMap ctp;                   // CTP 网关参数（[ctp] 段）
};

// ---------------------------------------------------------------- 策略创建

// 按类型名创建策略；类型未注册时打印可用列表并返回 nullptr
static std::shared_ptr<BaseStrategy> make_strategy(const StrategySpec& sp) {
    auto& reg = StrategyRegistry::instance();
    auto st = reg.create(sp.type, sp.name, sp.params);
    if (!st) {
        Logger::log(Logger::Level::WARNING,
                    "策略类型未注册: " + sp.type + "\n当前可用策略:\n" + reg.dump());
    }
    return st;
}

// 把配置中的全部策略加入引擎（回测引擎与主引擎都支持 add_strategy）
template <class Engine>
static bool add_strategies(Engine& engine, const RunConfig& cfg) {
    for (const auto& sp : cfg.strategies) {
        auto st = make_strategy(sp);
        if (!st) return false;
        Logger::log(Logger::Level::INFO,
                    "载入策略: " + sp.name + " (类型=" + sp.type + ")");
        engine.add_strategy(st);
    }
    return true;
}

// ---------------------------------------------------------------- 无限运行 / 定时停止
// 所有 live/replay 模式在 connect+start 之后用本函数阻塞主线程。
//   secs<=0 -> 无限运行，仅当用户按 Ctrl-C(SIGINT) 时退出（真实长连接场景）
//   secs> 0 -> 睡满指定秒数后自动退出（demo / 联调场景，行为同改动前）
static std::atomic<bool> g_stop_requested{false};
static void install_stop_handler() {
    std::signal(SIGINT, [](int) { g_stop_requested = true; });
#ifdef SIGTERM
    std::signal(SIGTERM, [](int) { g_stop_requested = true; });
#endif
}
static void run_until_stop(int secs) {
    install_stop_handler();
    if (secs <= 0) {
        Logger::log(Logger::Level::INFO,
                    "无限运行模式(run_seconds<=0)，按 Ctrl-C 停止...");
        while (!g_stop_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
    }
}

// ---------------------------------------------------------------- 各运行模式

// 旧模式：bar 级回测。用 BarData 分钟线驱动，撮合在 K 线收盘。
static int run_backtest(const RunConfig& cfg) {
    BacktestEngine engine;
    engine.set_capital(cfg.capital);
    engine.set_commission(cfg.commission);
    engine.set_slippage(cfg.slippage);
    engine.load_csv(cfg.data_file, cfg.vt_symbol, Interval::MINUTE, true);
    if (!add_strategies(engine, cfg)) return 1;
    engine.run();
    return 0;
}

// 旧模式：CSV 重放模拟盘。CsvReplayGateway 按 speed_ms 节奏回放 bar 数据，
// 经事件引擎推给策略，但成交由模拟撮合，不接入真实交易所。
static int run_live_csv(const RunConfig& cfg) {
    MainEngine engine;
    auto gw = std::make_shared<CsvReplayGateway>(engine.event_engine().get(), "CSV");
    engine.add_gateway(gw);
    engine.set_default_gateway("CSV");
    if (!add_strategies(engine, cfg)) return 1;

    std::map<std::string, std::string> settings;
    settings["file"] = cfg.data_file;
    settings["vt_symbol"] = cfg.vt_symbol;
    settings["speed_ms"] = std::to_string(cfg.speed_ms);
    engine.connect_all(settings);
    engine.start();

    Logger::log(Logger::Level::INFO,
                "模拟盘运行中(" + std::to_string(cfg.run_seconds) + "秒)...");
    run_until_stop(cfg.run_seconds);
    engine.stop();
    return 0;
}

// 旧模式：币安(Stub)实盘链路。BinanceGateway 此处为桩实现，仅演示事件/委托链路，
// api_key/secret 为占位符，真正下单需接入完整 REST/WebSocket 实现。
static int run_live_binance(const RunConfig& cfg) {
    MainEngine engine;
    auto gw = std::make_shared<BinanceGateway>(engine.event_engine().get());
    engine.add_gateway(gw);
    engine.set_default_gateway("BINANCE");
    if (!add_strategies(engine, cfg)) return 1;

    std::map<std::string, std::string> settings;
    settings["api_key"] = "YOUR_API_KEY";
    settings["api_secret"] = "YOUR_API_SECRET";
    engine.connect_all(settings);
    engine.start();

    Logger::log(Logger::Level::INFO,
                "币安(Stub)实盘链路运行中(" + std::to_string(cfg.run_seconds) + "秒)...");
    run_until_stop(cfg.run_seconds);
    engine.stop();
    return 0;
}

// 旧模式：CTP 期货实盘链路。CtpGateway 接入期货柜台；[ctp] 段参数经 cfg.ctp 透传，
// run_seconds / live_trading 亦从 ctp 段读取，用于本地不真正下单地联调。
static int run_live_ctp(const RunConfig& cfg) {
    MainEngine engine;
    auto gw = std::make_shared<CtpGateway>(engine.event_engine().get(), "CTP");
    engine.add_gateway(gw);
    engine.set_default_gateway("CTP");
    if (!add_strategies(engine, cfg)) return 1;

    auto settings = cfg.ctp;
    if (settings.count("md_front") == 0 || settings.count("td_front") == 0)
        Logger::log(Logger::Level::WARNING, "未配置 md_front/td_front，CTP 无法连接");

    engine.connect_all(settings);
    engine.start();

    int secs = param_int(cfg.ctp, "run_seconds", cfg.run_seconds);
    Logger::log(Logger::Level::INFO, "CTP 实盘链路运行中(" + std::to_string(secs) +
                "秒) live_trading=" + std::to_string(param_bool(cfg.ctp, "live_trading", false)));
    run_until_stop(secs);
    engine.stop();
    return 0;
}

// 旧模式：tick 直喂回放。TickCsvGateway 直接推送 on_tick（不经分钟聚合），
// 策略逐笔实时响应；speed_ms=0 表示全速回放。
static int run_tick_csv(const RunConfig& cfg) {
    // tick 直喂回放：TickCsvGateway 直接推送 on_tick，策略逐笔响应
    MainEngine engine;
    auto gw = std::make_shared<TickCsvGateway>(engine.event_engine().get(), "TICK");
    engine.add_gateway(gw);
    engine.set_default_gateway("TICK");
    if (!add_strategies(engine, cfg)) return 1;

    std::map<std::string, std::string> settings;
    settings["file"] = cfg.data_file;
    settings["speed_ms"] = std::to_string(cfg.speed_ms);   // 0 = 全速回放
    engine.connect_all(settings);
    engine.start();

    Logger::log(Logger::Level::INFO,
                "tick 直喂回放中(" + std::to_string(cfg.run_seconds) + "秒)...");
    run_until_stop(cfg.run_seconds);
    engine.stop();
    return 0;
}

// 旧模式：tick 级回测。TickBacktestEngine 直喂 on_tick 并以 tick 级撮合
// （下一笔 tick 成交，无未来函数），用于精细验证 tick 策略。
static int run_tick_backtest(const RunConfig& cfg) {
    // tick 级回测：直喂 on_tick，tick 级撮合（下一笔成交，无未来函数）
    TickBacktestEngine engine;
    engine.set_capital(cfg.capital);
    engine.set_commission(cfg.commission);
    engine.set_slippage(cfg.slippage);
    engine.load_tick_csv(cfg.data_file, "");
    if (!add_strategies(engine, cfg)) return 1;
    engine.run();
    return 0;
}

// ---------------------------------------------------------------- 零拷贝队列基准
// 测量 EventEngine 真实热路径（生产者线程入队 + 引擎线程出队分发）的吞吐与延迟。
// 这是「零拷贝事件队列」改造后的端到端指标，可与改造前的 std::any+mutex 版本对照。
static int run_bench() {
    const uint64_t N = 5'000'000;

    // 基准专用策略：只做计数与延迟统计，不下单、不持仓，从而排除交易逻辑的开销，
    // 单纯压测事件队列本身的热路径吞吐。
    struct BenchStrat : BaseStrategy {
        using BaseStrategy::BaseStrategy;   // 继承 BaseStrategy(const std::string&)
        std::atomic<uint64_t> n{0};
        std::atomic<uint64_t> sum_ns{0};
        std::atomic<uint64_t> max_ns{0};
        void on_tick(const TickData& d) override {
            ++n;
            // 用生产者写入的发射时间戳与消费时刻求差，得到端到端入队-分发延迟
            int64_t lat = now_ns() - d.datetime;   // d.datetime 被生产者写入发射时刻(ns)
            if (lat < 0) lat = 0;
            sum_ns.fetch_add((uint64_t)lat, std::memory_order_relaxed);
            // 无锁更新最大延迟：CAS 自旋直到成功写入更大值
            auto m = max_ns.load(std::memory_order_relaxed);
            while (lat > m && !max_ns.compare_exchange_weak(m, (uint64_t)lat)) {}
        }
        void on_bar(const BarData&) override {}
        void on_order(const OrderData&) override {}
        void on_trade(const TradeData&) override {}
    };

    EventEngine ee;
    auto st = std::make_shared<BenchStrat>("bench");
    ee.register_strategy(st.get());   // 走零拷贝热路径：引擎按 TICK 类型直接调虚函数
    ee.start();

    TickData tk; tk.last_price = 1.0;
    auto t0 = now_ns();
    for (uint64_t i = 0; i < N; ++i) {
        tk.datetime = now_ns();                      // 生产者打纳秒时间戳
        ee.put(Event{EventType::TICK, tk});          // 入队（variant 内联存储，无堆分配）
    }
    while (st->n.load() < N) std::this_thread::yield();   // 等待消费完成
    auto t1 = now_ns();
    ee.stop();

    double sec = (t1 - t0) / 1e9;
    uint64_t cnt = st->n.load();
    std::cout << "[bench] 零拷贝事件队列吞吐: " << (N / sec / 1e6) << " M events/sec\n"
              << "        (总事件=" << N << "  耗时=" << sec << " s)\n"
              << "        队列容量=" << ee.queue_capacity()
              << "  剩余=" << ee.approx_queue_size()
              << "  丢弃=" << ee.dropped_events() << "\n";
    if (cnt > 0) {
        std::cout << "        平均时延=" << (st->sum_ns.load() / (double)cnt) << " ns"
                  << "  最大时延=" << st->max_ns.load() << " ns\n";
    }
    return 0;
}

static int dispatch(const RunConfig& rc) {
    if (rc.mode == "backtest")      return run_backtest(rc);
    if (rc.mode == "live_csv")      return run_live_csv(rc);
    if (rc.mode == "live_binance")  return run_live_binance(rc);
    if (rc.mode == "live_ctp")      return run_live_ctp(rc);
    if (rc.mode == "tick_csv")      return run_tick_csv(rc);
    if (rc.mode == "tick_backtest") return run_tick_backtest(rc);
    Logger::log(Logger::Level::WARNING, "未知模式: " + rc.mode +
                " (可选: backtest|live_csv|live_binance|live_ctp|tick_csv|tick_backtest)");
    return 1;
}

// ---------------------------------------------------------------- 配置驱动

// 配置驱动入口：解析 cfg.ini，按「先插件→再运行参数→再策略」的顺序装配并运行。
// 这是推荐用法——模式/插件/策略/参数全部由文件决定，主程序代码无需改动。
static int run_config(const std::string& path) {
    IniConfig cfg;
    if (!cfg.load(path)) {
        Logger::log(Logger::Level::WARNING, cfg.last_error());
        return 1;
    }

    // 1) 先加载外部插件：插件导出的策略类型会注册进策略表
    for (const auto& kv : cfg.section("plugins")) {
        if (kv.second.empty()) continue;
        std::string err;
        if (!PluginLoader::instance().load(kv.second, &err))
            Logger::log(Logger::Level::WARNING, err);
    }

    // 2) 读取运行参数
    RunConfig rc;
    rc.mode       = cfg.get("run", "mode", "tick_backtest");
    rc.data_file  = cfg.get("run", "data_file", "");
    rc.vt_symbol  = cfg.get("run", "vt_symbol", "BTCUSDT.BINANCE_USDT");
    rc.capital    = cfg.get_double("run", "capital", 1'000'000.0);
    rc.commission = cfg.get_double("run", "commission", 0.0004);
    rc.slippage   = cfg.get_double("run", "slippage", 0.0);
    rc.speed_ms   = cfg.get_int("run", "speed_ms", 0);
    rc.run_seconds= cfg.get_int("run", "run_seconds", 15);
    rc.ctp        = cfg.section("ctp");

    if (rc.data_file.empty()) {   // 按模式选默认数据
        rc.data_file = (rc.mode == "tick_csv" || rc.mode == "tick_backtest")
                           ? "data/BTCUSDT_tick.csv" : "data/BTCUSDT.csv";
    }

    // 3) 收集策略：[strategy] 为首个，[strategy.2] [strategy.3]... 为追加（可选）
    auto collect = [&](const std::string& sec) {
        if (!cfg.has_section(sec)) return;
        StrategySpec sp;
        sp.type = cfg.get(sec, "type", "");
        if (sp.type.empty()) return;
        sp.name   = cfg.get(sec, "name", sp.type);
        sp.params = parse_params(cfg.get(sec, "params", ""));
        rc.strategies.push_back(sp);
    };
    collect("strategy");
    for (int i = 2; i <= 16; ++i) collect("strategy." + std::to_string(i));

    if (rc.strategies.empty()) {
        Logger::log(Logger::Level::WARNING,
                    "配置中未指定策略，请在 [strategy] 段写 type=<已注册类型>");
        return 1;
    }

    return dispatch(rc);
}

// ---------------------------------------------------------------- main

static void print_usage() {
    std::cout << "用法:\n"
              << "  ltc run [cfg.ini]   配置驱动（推荐），默认 config/run.ini\n"
              << "  ltc list            列出已注册策略（含插件提供）\n"
              << "  ltc bench           零拷贝事件队列吞吐/延迟微基准\n"
              << "  ltc <mode>          兼容旧用法，策略取该模式默认类型\n"
              << "     mode = backtest | live_csv | live_binance | live_ctp [cfg.ini]\n"
              << "          | tick_csv | tick_backtest\n";
}

int main(int argc, char** argv) {
    // 中文日志是 UTF-8 字节，而 Windows 控制台默认代码页是 GBK(936)，会导致乱码。
    // 启动时把控制台输出/输入代码页切到 UTF-8(65001)，与 Python 绑定(ltc.cpp)做法一致。
    ::SetConsoleOutputCP(65001);
    ::SetConsoleCP(65001);

    // 解析参数：argv[1] 为子命令（bench/list/run）或旧式 mode 名
    std::string a1 = argc > 1 ? argv[1] : "";

    // 零拷贝事件队列微基准
    if (a1 == "bench") return run_bench();

    // 列出可用策略（此时尚未加载插件，仅显示内置）
    if (a1 == "list") {
        auto& reg = StrategyRegistry::instance();
        std::cout << "已注册策略:\n" << reg.dump();
        // 同时打印已通过 PluginLoader 加载的插件路径
        auto plugins = PluginLoader::instance().loaded();
        std::cout << "已加载插件: ";
        if (plugins.empty()) std::cout << "(无)";
        else for (const auto& p : plugins) std::cout << p << " ";
        std::cout << "\n";
        return 0;
    }

    // 配置驱动（推荐）：读 argv[2] 指定的 ini，缺省 config/run.ini
    if (a1 == "run") return run_config(argc > 2 ? argv[2] : "config/run.ini");

    // 兼容旧的模式参数用法：argv[1] 即 mode，策略用该模式的默认内置类型与参数
    if (!a1.empty()) {
        RunConfig rc;
        rc.mode = a1;
        // 按模式选默认数据文件（tick 模式用 tick 数据，其余用分钟线）
        rc.data_file = (a1 == "tick_csv" || a1 == "tick_backtest")
                           ? "data/BTCUSDT_tick.csv" : "data/BTCUSDT.csv";

        if (a1 == "backtest" || a1 == "live_csv" || a1 == "live_binance") {
            rc.strategies.push_back({"DoubleMA", "DoubleMA",
                                     parse_params("fast=10,slow=30,vol=1.0")});
        } else if (a1 == "tick_csv") {
            rc.strategies.push_back({"TickDemo", "TICK_Demo",
                                     parse_params("live=0,fast=50,slow=200,vol=1.0")});
        } else if (a1 == "tick_backtest") {
            rc.strategies.push_back({"TickDemo", "TickMA",
                                     parse_params("live=1,fast=50,slow=200,vol=1.0")});
        } else if (a1 == "live_ctp") {
            // 旧用法沿用 ctp_settings.ini（扁平 key=value）
            IniConfig c;
            c.load(argc > 2 ? argv[2] : "config/ctp_settings.ini");
            rc.ctp = c.section("");
            StrategyParams p;
            p["live"] = param_bool(rc.ctp, "live_trading", false) ? "1" : "0";
            p["fast"] = std::to_string(param_int(rc.ctp, "fast", 10));
            p["slow"] = std::to_string(param_int(rc.ctp, "slow", 30));
            p["vol"]  = std::to_string(param_double(rc.ctp, "fixed_volume", 1.0));
            rc.strategies.push_back({"CtpDemo", "CTP_Demo", p});
            rc.run_seconds = param_int(rc.ctp, "run_seconds", 30);
        }

        return dispatch(rc);
    }

    print_usage();
    return 1;
}
