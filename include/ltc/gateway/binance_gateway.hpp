// binance_gateway.hpp - 币安 U 本位合约网关（vnpy Gateway 风格，零依赖可编译）
//
// 职责：对接币安 fapi REST（下单/撤单/查账户）与 WebSocket（行情）。
// 适用：币安 U 本位永续/合约（Exchange::BINANCE_USDT）。
// 与 BaseGateway：继承 BaseGateway，复用 on_bar/on_order/on_trade/on_account 推送。
// 设计：将网络传输抽象为 HttpClient / WsClient，默认提供 Stub 便于零依赖运行；
//       实盘把 Stub 替换为 libcurl + ixwebsocket 实现即可，业务逻辑不变。
// 已知限制：
//   1) 行情 WebSocket 当前为 Stub（随机游走模拟 K线），非真实行情；
//   2) 下单/撤单为同步阻塞 HTTP 调用（http_->post 内阻塞），高频下是吞吐瓶颈；
//   3) Stub 下单不做风控/持仓校验，仅回显参数。
#pragma once
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <functional>
#include <sstream>
#include <cstdlib>

#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/util.hpp"
#include "ltc/gateway/crypto.hpp"

namespace ltc {

// ---------- 传输层抽象 ----------
// HTTP 响应：状态码 + 响应体
struct HttpResponse { int code = 0; std::string body; };

// HTTP 传输层抽象（GET/POST/DELETE）。实盘由 libcurl 实现，默认 Stub
class HttpClient {
public:
    virtual HttpResponse get(const std::string& url, const std::string& query,
                             const std::map<std::string,std::string>& headers) = 0;
    virtual HttpResponse post(const std::string& url, const std::string& query,
                              const std::map<std::string,std::string>& headers) = 0;
    virtual HttpResponse del(const std::string& url, const std::string& query,
                             const std::map<std::string,std::string>& headers) = 0;
    virtual ~HttpClient() = default;
};

// WebSocket 传输层抽象（连接/发送/关闭）。实盘由 ixwebsocket 实现，默认 Stub
class WsClient {
public:
    using MsgHandler = std::function<void(const std::string&)>;
    virtual void connect(const std::string& url, MsgHandler handler) = 0;
    virtual void send(const std::string& text) = 0;
    virtual void close() = 0;
    virtual ~WsClient() = default;
};

// ---------- Stub 传输层（无网络即可运行，模拟交易所）----------
// 无网络 Stub HTTP 实现：按 URL/query 回显模拟响应，便于本地编译运行
class StubHttpClient : public HttpClient {
public:
    // 模拟 GET：/fapi/v1/account 返回余额，/exchangeInfo 返回时间戳
    HttpResponse get(const std::string& url, const std::string&,
                     const std::map<std::string,std::string>&) override {
        HttpResponse r; r.code = 200;
        if (url.find("/fapi/v1/account") != std::string::npos)
            r.body = R"({"availableBalance":"100000.0","totalWalletBalance":"100000.0"})";
        else if (url.find("/fapi/v1/exchangeInfo") != std::string::npos)
            r.body = R"({"serverTime":0})";
        return r;
    }
    // 模拟 POST 下单：解析 query 中 symbol/side/price/qty，回显为 order ack JSON
    HttpResponse post(const std::string& url, const std::string& query,
                      const std::map<std::string,std::string>&) override {
        HttpResponse r; r.code = 200;
        // 解析 query 中的 symbol/side/price/qty 回显为下单 ack
        std::string symbol = field(query, "symbol");
        std::string side = field(query, "side");
        std::string price = field(query, "price");
        std::string qty = field(query, "quantity");
        std::string oid = field(query, "newClientOrderId");
        if (oid.empty()) oid = "stub-" + std::to_string(++seq_);
        r.body = R"({"orderId":)" + std::to_string(++srv_) +
                 R"(,"symbol":")" + symbol + R"(","side":")" + side +
                 R"(","price":")" + price + R"(","origQty":")" + qty +
                 R"(","status":"NEW","clientOrderId":")" + oid + R"("})";
        return r;
    }
    // 模拟 DELETE 撤单：回显 CANCELED 与 clientOrderId
    HttpResponse del(const std::string& url, const std::string& query,
                     const std::map<std::string,std::string>&) override {
        HttpResponse r; r.code = 200;
        r.body = R"({"status":"CANCELED","clientOrderId":")" + field(query,"origClientOrderId") + R"("})";
        return r;
    }
private:
    // 从 query 串中按 key= 取值（截取到下一个 & 或结尾）
    static std::string field(const std::string& q, const std::string& key) {
        size_t p = q.find(key + "="); if (p == std::string::npos) return "";
        size_t s = p + key.size() + 1;
        size_t e = q.find('&', s);
        return q.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }
    int seq_ = 0, srv_ = 1000;
};

// Stub WebSocket：连接后周期性推送模拟 K线，演示实时行情链路
// Stub WebSocket：连接后每秒推送一根随机游走模拟 K线，演示行情链路
class StubWsClient : public WsClient {
public:
    // 后台线程周期性生成随机游走 K线并回调 handler（每 1s 一根）
    void connect(const std::string& url, MsgHandler handler) override {
        url_ = url; handler_ = handler; running_ = true;
        thread_ = std::thread([this]() {
            double p = 30000.0;
            while (running_) {
                // 生成一根随机游走 K线
                double o = p;
                double c = o * (1.0 + ((rand() % 200) - 100) / 10000.0);
                double h = std::max(o, c) * 1.001;
                double l = std::min(o, c) * 0.999;
                p = c;
                std::ostringstream oss;
                oss << R"({"e":"kline","k":{"t":)" << (int64_t)(now_ms())
                    << R"(,"o":")" << o << R"(","h":")" << h << R"(","l":")" << l
                    << R"(","c":")" << c << R"(","v":"1.0","x":true}})";
                if (handler_) handler_(oss.str());
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
    }
    void send(const std::string&) override {}
    void close() override { running_ = false; if (thread_.joinable()) thread_.join(); }
private:
    std::string url_; MsgHandler handler_; std::thread thread_; std::atomic<bool> running_{false};
};

// ---------- 币安合约接口 ----------
// 币安合约网关：注入传输层，处理登录/下单/撤单/行情回调
class BinanceGateway : public BaseGateway {
public:
    // 注入真实传输层（默认 Stub）。实盘时由外部传入 libcurl/ixwebsocket 实现。
    BinanceGateway(EventEngine* ee, std::shared_ptr<HttpClient> http = nullptr,
                   std::shared_ptr<WsClient> ws = nullptr, const std::string& name = "BINANCE")
        : BaseGateway(ee, name) {
        http_ = http ? http : std::make_shared<StubHttpClient>();
        ws_ = ws ? ws : std::make_shared<StubWsClient>();
    }

    // 连接：读取 api_key/secret，查询账户并推送余额；订阅 BTCUSDT 1m K线行情(Stub)
    void connect(const std::map<std::string, std::string>& settings) override {
        api_key_ = settings.count("api_key") ? settings.at("api_key") : "";
        api_secret_ = settings.count("api_secret") ? settings.at("api_secret") : "";
        // 1) 查询账户
        auto acc = http_->get(base_url_ + "/fapi/v1/account", "", auth_headers());
        if (acc.code == 200) {
            AccountData a; a.accountid = "BINANCE"; a.balance = 100000.0;
            a.gateway_name = gateway_name_; on_account(std::move(a));
        }
        // 2) 订阅行情（此处订阅 BTCUSDT 1m K线）
        std::string stream_url = ws_base_ + "/ws/btcusdt@kline_1m";
        ws_->connect(stream_url, [this](const std::string& msg) { on_ws_message(msg); });
        on_log("币安接口已连接(Stub传输)，订阅行情: btcusdt@kline_1m");
    }

    void close() override { ws_->close(); on_log("币安接口已关闭"); }

    // 同步 REST 下单（高频瓶颈点）：
    //   组装 query(symbol/side/type/quantity/price/newClientOrderId/timestamp/recvWindow)，
    //   用 HMAC-SHA256 对 query 签名并追加 &signature，POST /fapi/v1/order。
    //   成功则构造 SUBMITTED OrderData 推送 on_order，并记录 vt_orderid->symbol 供撤单还原。
    //   注意：市价单不带 price 字段。
    std::string send_order(const OrderRequest& req) override {
        std::string symbol = req.symbol;
        std::string side = req.direction == Direction::LONG ? "BUY" : "SELL";
        std::string orderid = std::to_string(++srv_seq_);
        std::string vt_oid = make_vt_orderid(gateway_name_, orderid);
        // 币安市价单不需要 price
        std::ostringstream q;
        q << "symbol=" << symbol << "&side=" << side;
        q << "&type=" << (req.type == OrderType::MARKET ? "MARKET" : "LIMIT");
        q << "&quantity=" << req.volume;
        if (req.type == OrderType::LIMIT) q << "&price=" << req.price;
        q << "&newClientOrderId=" << vt_oid;
        q << "&timestamp=" << now_ms() << "&recvWindow=5000";
        std::string sig = hmac_sha256_hex(api_secret_, q.str());
        q << "&signature=" << sig;
        auto resp = http_->post(base_url_ + "/fapi/v1/order", q.str(), auth_headers());
        if (resp.code == 200) {
            OrderData o = req.to_order(gateway_name_, orderid);
            o.status = Status::SUBMITTED; o.datetime = now_ms();
            o.vt_orderid = vt_oid;
            order_symbol_[vt_oid] = symbol;
            on_order(std::move(o));
            on_log("下单成功: " + resp.body);
            return vt_oid;
        }
        on_log("下单失败(" + std::to_string(resp.code) + "): " + resp.body);
        return "";
    }

    // 同步 REST 撤单：用 vt_orderid 查出 symbol，组装 origClientOrderId+timestamp，
    //   HMAC 签名后 DELETE /fapi/v1/order；结束后清除 symbol 映射
    void cancel_order(const CancelRequest& req) override {
        std::string symbol = req.vt_orderid; // 兜底
        auto it = order_symbol_.find(req.vt_orderid);
        if (it != order_symbol_.end()) symbol = it->second;
        std::ostringstream q;
        q << "symbol=" << symbol << "&origClientOrderId=" << req.vt_orderid;
        q << "&timestamp=" << now_ms();
        std::string sig = hmac_sha256_hex(api_secret_, q.str());
        q << "&signature=" << sig;
        auto resp = http_->del(base_url_ + "/fapi/v1/order", q.str(), auth_headers());
        on_log("撤单返回(" + std::to_string(resp.code) + "): " + resp.body);
        order_symbol_.erase(req.vt_orderid);
    }

    // 订阅（Stub 阶段仅记录日志，真实行情在 connect 内已订阅 BTCUSDT 1m）
    void subscribe(const std::vector<std::string>&) override { on_log("订阅请求已提交"); }

private:
    // 构造鉴权头：仅携带 X-MBX-APIKEY（签名在 query 的 signature 字段）
    std::map<std::string, std::string> auth_headers() {
        return { { "X-MBX-APIKEY", api_key_ } };
    }

    // WebSocket 行情回调：极简解析 kline JSON 字段(t/o/h/l/c/v)构造 BarData 并推送 on_bar
    void on_ws_message(const std::string& msg) {
        // 简化解析：提取 kline 字段构造 BarData 并推送
        auto field = [&](const std::string& key) -> std::string {
            std::string k = "\"" + key + "\":";
            size_t p = msg.find(k); if (p == std::string::npos) return "";
            size_t s = msg.find('"', p + k.size());
            if (s == std::string::npos) { // 数字字段
                size_t e = msg.find_first_of(",}", p + k.size());
                return msg.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - (p + k.size()));
            }
            size_t e = msg.find('"', s + 1);
            return msg.substr(s + 1, e - s - 1);
        };
        if (msg.find("\"kline\"") == std::string::npos) return;
        BarData b;
        b.symbol = "BTCUSDT"; b.exchange = Exchange::BINANCE_USDT;
        b.vt_symbol = make_vt_symbol(b.symbol, b.exchange);
        b.interval = Interval::MINUTE;
        try {
            b.datetime = std::stoll(field("t"));
            b.open = std::stod(field("o")); b.high = std::stod(field("h"));
            b.low = std::stod(field("l")); b.close = std::stod(field("c"));
            b.volume = std::stod(field("v"));
        } catch (...) { return; }
        on_bar(std::move(b));
    }

    std::shared_ptr<HttpClient> http_;
    std::shared_ptr<WsClient> ws_;
    std::string api_key_, api_secret_;
    std::string base_url_ = "https://fapi.binance.com";      // 实盘域名
    std::string ws_base_  = "wss://fapi.binance.com";         // 实盘 ws 域名
    std::map<std::string, std::string> order_symbol_; // vt_orderid -> symbol（撤单还原用）
    int order_seq_ = 0, srv_seq_ = 2000;
};

} // namespace ltc
