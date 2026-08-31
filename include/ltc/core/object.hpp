// object.hpp - vnpy 风格核心数据对象与枚举 (C++17, 无外部依赖)
//
// 职责：定义框架内跨模块共享的"领域模型"——交易所/方向/开平/委托类型/委托状态等
//       枚举，以及 TickData/BarData/OrderData/TradeData/PositionData/AccountData/
//       ContractData 等纯数据载体（POD 风格结构体），外加字符串 <-> 枚举互转、
//       vt_symbol（symbol.exchange 复合键）等工具函数。
//
// 关键设计：
//   - 所有数据对象为值类型、字段平铺、成员默认初始化，可在网关/引擎/策略间安全拷贝；
//   - vt_symbol 作为全局唯一标的标识，贯穿订单路由、持仓缓存与事件分发，是寻址主键。
//
// 与框架其它模块关系：
//   - event.hpp 的 EventPayload 直接内联存储此处各 Data 结构，实现零拷贝事件投递；
//   - strategy.hpp 的 buy/sell/short/cover 构造 OrderRequest 并最终落到 OrderData；
//   - gateway 层负责填充这些对象并 put 进 EventEngine。
#pragma once
#include <string>
#include <cstdint>
#include <iostream>

namespace ltc {
// ltc 命名空间：LTC 框架核心领域模型、枚举与工具函数的集合。

// ---------- 枚举 ----------
// 以下枚举为框架内统一的"小整数常量"，替代裸字符串以节省内存并便于比较/分支。
enum class Exchange {  // 交易所：标的上架的交易场所，决定合约规则与行情源
    NONE,
    OKX,
    BINANCE,      // 币安现货
    BINANCE_USDT, // 币安 U 本位合约
    SHFE, CFFEX, DCE, CZCE, INE, SSE, SZSE // 国内期货/股票
};

// 交易方向：做多/做空，是持仓与盈亏计算的基础维度。
enum class Direction {  // 方向
    NONE,
    LONG,   // 多
    SHORT   // 空
};

// 开平标志：区分建仓(OPEN)与平仓(CLOSE)，以及平今/平昨（国内期货手续费/保证金规则不同）。
enum class Offset {  // 开平
    NONE,
    OPEN,   // 开仓
    CLOSE,  // 平仓
    CLOSETODAY,
    CLOSEYESTERDAY
};

// 委托类型：限价单按指定价撮合，市价单即时成交，停止单触发后转市价/限价。
enum class OrderType {  // 委托类型
    NONE,
    LIMIT,   // 限价
    MARKET,  // 市价
    STOP     // 停止
};

// 委托状态机：从 SUBMITTING 提交中推进到终态（ALLTRADED 全成 / CANCELLED 撤销 / REJECTED 拒单）的完整生命周期。
enum class Status {  // 委托状态
    NONE,
    SUBMITTING,  // 提交中
    SUBMITTED,   // 已提交(等待成交)
    PARTTRADED,  // 部分成交
    ALLTRADED,   // 全部成交
    CANCELLED,   // 已撤销
    CANCELLING,  // 撤销中
    REJECTED     // 拒单
};

// K 线周期：聚合行情的时间粒度，从分钟到日线。
enum class Interval {  // K线周期
    NONE,
    MINUTE,
    MINUTE3,
    MINUTE5,
    MINUTE15,
    HOUR,
    HOUR4,
    DAILY
};

// 合约产品类型（参考 vnpy Product）：区分标的品类，决定下单/风控规则。
enum class Product {  // 产品类型
    NONE,
    EQUITY,    // 股票
    FUTURES,   // 期货
    OPTION,    // 期权
    INDEX,     // 指数
    FOREX,     // 外汇
    SPREAD,    // 价差
    FUND,      // 基金
    BOND,      // 债券
    ETF,       // ETF
    WARRANT,   // 权证
    COMBO,     // 组合
    SPOT       // 现货
};

// 期权类型：看涨/看跌。
enum class OptionType {  // 期权类型
    NONE,
    CALL,   // 看涨
    PUT     // 看跌
};

inline std::string exchange_to_str(Exchange e) {
    switch (e) {
        case Exchange::BINANCE_USDT: return "BINANCE_USDT";
        case Exchange::BINANCE: return "BINANCE";
        case Exchange::OKX: return "OKX";
        case Exchange::SHFE: return "SHFE";
        case Exchange::CFFEX: return "CFFEX";
        case Exchange::DCE: return "DCE";
        case Exchange::CZCE: return "CZCE";
        case Exchange::INE: return "INE";
        case Exchange::SSE: return "SSE";
        case Exchange::SZSE: return "SZSE";
        default: return "NONE";
    }
}
// 字符串 -> 交易所枚举（用于从 CSV 读取 exchange 列）。与 exchange_to_str 互为逆操作，
// 新增加密所/国内场所时两函数需同步修改。匹配失败返回 Exchange::NONE。
inline Exchange exchange_from_str(const std::string& s) {
    if (s == "BINANCE_USDT") return Exchange::BINANCE_USDT;
    if (s == "BINANCE") return Exchange::BINANCE;
    if (s == "OKX") return Exchange::OKX;
    if (s == "SHFE") return Exchange::SHFE;
    if (s == "CFFEX") return Exchange::CFFEX;
    if (s == "DCE") return Exchange::DCE;
    if (s == "CZCE") return Exchange::CZCE;
    if (s == "INE") return Exchange::INE;
    if (s == "SSE") return Exchange::SSE;
    if (s == "SZSE") return Exchange::SZSE;
    return Exchange::NONE;
}
inline std::string direction_to_str(Direction d) {
    return d == Direction::LONG ? "LONG" : d == Direction::SHORT ? "SHORT" : "NONE";
}
inline std::string offset_to_str(Offset o) {
    switch (o) {
        case Offset::OPEN: return "OPEN";
        case Offset::CLOSE: return "CLOSE";
        case Offset::CLOSETODAY: return "CLOSETODAY";
        case Offset::CLOSEYESTERDAY: return "CLOSEYESTERDAY";
        default: return "NONE";
    }
}
inline std::string ordertype_to_str(OrderType t) {
    return t == OrderType::LIMIT ? "LIMIT" : t == OrderType::MARKET ? "MARKET" : t == OrderType::STOP ? "STOP" : "NONE";
}
inline std::string status_to_str(Status s) {
    switch (s) {
        case Status::SUBMITTING: return "SUBMITTING";
        case Status::SUBMITTED: return "SUBMITTED";
        case Status::PARTTRADED: return "PARTTRADED";
        case Status::ALLTRADED: return "ALLTRADED";
        case Status::CANCELLED: return "CANCELLED";
        case Status::CANCELLING: return "CANCELLING";
        case Status::REJECTED: return "REJECTED";
        default: return "NONE";
    }
}

// ---------- 工具函数 ----------
// vt_symbol = "symbol.exchange"，例如 "BTCUSDT.BINANCE_USDT"：框架内标的唯一标识，
// 订单路由、持仓缓存、事件分发均以它作为寻址主键。
inline std::string make_vt_symbol(const std::string& symbol, Exchange e) {
    return symbol + "." + exchange_to_str(e);
}
// vt_orderid = "gateway.oid"：网关名 + 交易所原始订单号，拼接成全局唯一委托标识，
// 用于撤单/查询时回溯到具体网关与原始订单号。
inline std::string make_vt_orderid(const std::string& gateway, const std::string& oid) {
    return gateway + "." + oid;
}

// ---------- 数据对象 ----------
// 实时行情快照（Tick）：某一时刻某标的的最优买卖盘与最新成交。行情网关解析后填充并投递 TICK 事件。
struct TickData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    int64_t datetime = 0;     // 毫秒时间戳
    double last_price = 0.0;
    double last_volume = 0.0;
    double bid_price_1 = 0.0, bid_volume_1 = 0.0;
    double ask_price_1 = 0.0, ask_volume_1 = 0.0;
    double open_interest = 0.0;
    double volume = 0.0;
    double limit_up = 0.0, limit_down = 0.0;
    // 取复合标识：vt_symbol 已填则直接复用，否则现场拼装（symbol.exchange），避免脏数据传播
    std::string to_vt_symbol() const { return vt_symbol.empty() ? make_vt_symbol(symbol, exchange) : vt_symbol; }
};

// K 线（Bar）：在某一周期(interval)内聚合出的开高低收与成交量，由 BarGenerator 生成后投递 BAR 事件。
struct BarData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    int64_t datetime = 0;
    Interval interval = Interval::NONE;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
    double volume = 0.0;
    double open_interest = 0.0;
    // 取复合标识：vt_symbol 已填则直接复用，否则现场拼装（symbol.exchange），避免脏数据传播
    std::string to_vt_symbol() const { return vt_symbol.empty() ? make_vt_symbol(symbol, exchange) : vt_symbol; }
};

// 委托（Order）：一笔报单的当前状态镜像。gateway 每次收到成交/状态推送都更新并投递 ORDER 事件。
struct OrderData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    std::string orderid;
    std::string vt_orderid;
    Direction direction = Direction::NONE;
    Offset offset = Offset::NONE;
    OrderType type = OrderType::NONE;
    Status status = Status::NONE;
    double price = 0.0;
    double volume = 0.0;
    double traded = 0.0;
    int64_t datetime = 0;
    std::string gateway_name;
};

// 成交（Trade）：一笔委托的部分或全部成交回报，含成交价/量；驱动持仓与盈亏更新，投递 TRADE 事件。
struct TradeData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    std::string orderid;
    std::string tradeid;
    std::string vt_orderid;
    std::string vt_tradeid;
    Direction direction = Direction::NONE;
    Offset offset = Offset::NONE;
    double price = 0.0;
    double volume = 0.0;
    int64_t datetime = 0;
    std::string gateway_name;
};

// 持仓（Position）：某标的某方向的净头寸、冻结量与浮动盈亏，由成交回报累计维护。
struct PositionData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    Direction direction = Direction::NONE;
    double volume = 0.0;
    double frozen = 0.0;
    double price = 0.0;       // 持仓均价
    double pnl = 0.0;         // 浮动盈亏
    std::string gateway_name;
};

// 账户（Account）：某网关下的资金权益与冻结金额。
struct AccountData {
    std::string accountid;
    double balance = 0.0;     // 总权益
    double frozen = 0.0;
    std::string gateway_name;
};

// 合约元信息（Contract）：标的大小(size)、最小变动价位(pricetick)、最小委托量等合约规则，供风控与下单校验。
// 字段对齐 vnpy ContractData：除基础规则外，期货含 product，期权含 option_* 系列。
struct ContractData {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_symbol;
    std::string name;
    bool active = true;
    Product product = Product::NONE;  // 产品类型（未设置时=未知，避免误判为期货）
    double size = 1.0;                   // 合约乘数
    double pricetick = 0.01;             // 最小价格变动
    double min_volume = 0.0;             // 最小委托量
    double max_volume = 0.0;             // 最大委托量
    bool net_position = false;           // 是否净持仓(单向)模式
    bool stop_supported = false;         // 是否支持停止单
    bool history_data = false;           // 是否支持历史数据
    double option_strike = 0.0;          // 期权行权价
    std::string option_underlying;       // 期权标的合约
    OptionType option_type = OptionType::NONE; // 期权类型（未设置时=未知，避免误判为看涨）
    std::string option_expiry;           // 期权到期日(YYYYMMDD)
    std::string option_portfolio;        // 期权组合(标的)
    std::string option_index;            // 期权合约指数
    std::string gateway_name;
    // 取复合标识：vt_symbol 已填则直接复用，否则现场拼装（symbol.exchange），避免脏数据传播
    std::string to_vt_symbol() const { return vt_symbol.empty() ? make_vt_symbol(symbol, exchange) : vt_symbol; }
};

// ---------- 请求对象 ----------
// 下单请求（OrderRequest）：策略调用 buy/sell 时构造的"意图"，经网关转换为交易所协议并回填成 OrderData。
struct OrderRequest {
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    Direction direction = Direction::NONE;
    Offset offset = Offset::NONE;
    OrderType type = OrderType::LIMIT;
    double volume = 0.0;
    double price = 0.0;
    std::string reference; // 策略标识
    // 把请求固化为一笔 OrderData：生成 vt_symbol/vt_orderid、状态置为 SUBMITTING，供网关与引擎持有。
    OrderData to_order(const std::string& gateway, const std::string& oid) const {
        OrderData o;
        o.symbol = symbol; o.exchange = exchange;
        o.vt_symbol = make_vt_symbol(symbol, exchange);
        o.orderid = oid; o.vt_orderid = make_vt_orderid(gateway, oid);
        o.direction = direction; o.offset = offset; o.type = type;
        o.status = Status::SUBMITTING; o.price = price; o.volume = volume;
        o.gateway_name = gateway;
        return o;
    }
};

// 撤单请求（CancelRequest）：仅凭 vt_orderid 即可定位并撤销一笔在途委托。
struct CancelRequest {
    std::string orderid;
    std::string symbol;
    Exchange exchange = Exchange::NONE;
    std::string vt_orderid;
};

} // namespace ltc
