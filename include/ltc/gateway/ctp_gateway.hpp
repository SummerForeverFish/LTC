// ctp_gateway.hpp - 上期技术 CTP 期货实盘网关（vnpy CtpGateway 风格）
//
// 职责：对接上期技术 CTP 行情(MdApi)与交易(TraderApi)，登录后推送行情/账户/合约，
//       并通过 ReqOrderInsert/ReqOrderAction 报单与撤单。
// 适用：国内期货（SHFE/CFFEX/DCE/CZCE/INE）。
// 与 BaseGateway：继承 BaseGateway，所有 SPI 回调统一经 on_* 方法再经
//       on_tick/on_order/on_trade/on_account/on_contract 推送事件。
// 依赖：CTP_V6.7.11 头文件及 win64 的 thostmduserapi_se.lib / thosttraderapi_se.lib。
// 已知限制：SPI 回调在 CTP 线程触发，目前直接回调网关（未强制切回事件线程）；
//           查询全部合约/账户后未做分页/节流；登录链未做断线重连。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cmath>
#include <chrono>
#include <ctime>

#include "ThostFtdcMdApi.h"
#include "ThostFtdcTraderApi.h"
#include "ltc/core/object.hpp"
#include "ltc/core/event.hpp"
#include "ltc/core/gateway.hpp"
#include "ltc/core/util.hpp"

namespace ltc {

// CTP 交易所代码("SHFE"等) -> ltc Exchange 枚举
inline Exchange ctp_exchange(const char* ex) {
    std::string s(ex ? ex : "");
    if (s == "SHFE")  return Exchange::SHFE;
    if (s == "CFFEX") return Exchange::CFFEX;
    if (s == "DCE")   return Exchange::DCE;
    if (s == "CZCE")  return Exchange::CZCE;
    if (s == "INE")   return Exchange::INE;
    return Exchange::NONE;
}

// CTP 产品类型(ProductClass) -> ltc Product（参考 vnpy）
inline Product ctp_product(char pc) {
    switch (pc) {
        case THOST_FTDC_PC_Futures:     return Product::FUTURES;
        case THOST_FTDC_PC_Options:     return Product::OPTION;
        case THOST_FTDC_PC_SpotOption:  return Product::OPTION;
        case THOST_FTDC_PC_Combination: return Product::COMBO;
        case THOST_FTDC_PC_Spot:        return Product::SPOT;
        default:                        return Product::FUTURES;
    }
}

// 安全写入 CTP 定长 char 数组：先清零再 strncpy（避免越界/残留）
inline void set_field(char* dst, size_t n, const std::string& src) {
    std::memset(dst, 0, n);
    if (!src.empty()) std::strncpy(dst, src.c_str(), n - 1);
}
// CTP 返回的中文字段（错误信息/合约名等）为 GBK(代码页936) 编码，
// 统一转成 UTF-8 再送入日志/数据，避免控制台/文件出现乱码。
inline std::string gbk_to_utf8(const char* src) {
    if (!src || !*src) return std::string();
    int wlen = ::MultiByteToWideChar(936, 0, src, -1, nullptr, 0);
    if (wlen <= 0) return std::string(src);             // 转换失败则原样返回
    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(936, 0, src, -1, &wbuf[0], wlen);
    int blen = ::WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (blen <= 0) return std::string(src);
    std::string out(static_cast<size_t>(blen) - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, &out[0], blen, nullptr, nullptr);
    return out;
}
// 安全转换 C 字符串（NULL -> 空串）；CTP 字段均为 GBK，统一转 UTF-8
inline std::string cstr(const char* p) { return p ? gbk_to_utf8(p) : std::string(); }

// 从 vt_symbol("rb2609.SHFE") 提取 CTP 订阅所需的合约代码("rb2609")
// CTP MdApi::SubscribeMarketData 只接受交易所合约代码，不含交易所后缀(".SHFE")
inline std::string ctp_inst_id(const std::string& vt) {
    auto p = vt.find('.');
    return p == std::string::npos ? vt : vt.substr(0, p);
}

// CTP 交易日(YYYYMMDD)+时间(HH:MM:SS)+毫秒 -> 毫秒时间戳（用 mktime 折算）
inline int64_t ctp_time_to_ms(const char* tradingday, const char* updatetime, int millisec) {
    std::tm t{};
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (tradingday && std::strlen(tradingday) >= 8)
        sscanf(tradingday, "%4d%2d%2d", &y, &m, &d);
    if (updatetime && std::strlen(updatetime) >= 8)
        sscanf(updatetime, "%2d:%2d:%2d", &hh, &mm, &ss);
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss; t.tm_isdst = 0;
    auto secs = (int64_t)std::mktime(&t);
    if (secs < 0) return now_ms();
    return secs * 1000 + (millisec > 0 ? millisec : 0);
}

// CTP 报单状态枚举 -> ltc Status
inline Status ctp_order_status(char s) {
    switch (s) {
        case THOST_FTDC_OST_AllTraded:          return Status::ALLTRADED;
        case THOST_FTDC_OST_PartTradedQueueing:
        case THOST_FTDC_OST_PartTradedNotQueueing: return Status::PARTTRADED;
        case THOST_FTDC_OST_NoTradeQueueing:
        case THOST_FTDC_OST_NoTradeNotQueueing: return Status::SUBMITTED;
        case THOST_FTDC_OST_Canceled:           return Status::CANCELLED;
        default:                                 return Status::SUBMITTED;
    }
}

class CtpGateway;  // forward

// ---------------- 行情 SPI ----------------
// 行情 SPI：薄封装，转发 CTP 行情回调到网关（线程安全由网关侧保证）
class CtpMdSpi : public CThostFtdcMdSpi {
public:
    explicit CtpMdSpi(CtpGateway* gw) : gw_(gw) {}
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) override;
private:
    CtpGateway* gw_;
};

// ---------------- 交易 SPI ----------------
// 交易 SPI：薄封装，转发 CTP 交易回调到网关
class CtpTradeSpi : public CThostFtdcTraderSpi {
public:
    explicit CtpTradeSpi(CtpGateway* gw) : gw_(gw) {}
    void OnFrontConnected() override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRtnOrder(CThostFtdcOrderField* p) override;
    void OnRtnTrade(CThostFtdcTradeField* p) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField*) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField*, CThostFtdcRspInfoField*) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField*, CThostFtdcRspInfoField*, int, bool) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField*, CThostFtdcRspInfoField*, int, bool) override;
private:
    CtpGateway* gw_;
};

// ---------------- CTP 实盘网关 ----------------
// CTP 实盘网关：聚合 MdApi/TraderApi，封装登录链与下单撤单
class CtpGateway : public BaseGateway {
public:
    explicit CtpGateway(EventEngine* ee, const std::string& name = "CTP")
        : BaseGateway(ee, name), inited_(false) {}

    ~CtpGateway() override { close(); }

    // 连接并初始化行情/交易 API（参数来源 settings）
    void connect(const std::map<std::string, std::string>& settings) override;
    void close() override;
    // 报单：组装 CThostFtdcInputOrderField 并 ReqOrderInsert（异步，回报走 OnRtnOrder）
    std::string send_order(const OrderRequest& req) override;
    // 撤单：按 vt_orderid 还原 symbol/exchange（ref_info_），组装 InputOrderActionField 并 ReqOrderAction
    void cancel_order(const CancelRequest& req) override;
    // 订阅行情：增量维护订阅列表并向 MdApi 发起订阅
    void subscribe(const std::vector<std::string>& vt_symbols) override;

    // ---- 供 SPI 回调 ----
    void on_md_connected();
    void on_td_connected();
    void on_td_authenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField*);
    void on_md_login(CThostFtdcRspInfoField*);
    void md_login();
    void td_login();
    void on_td_login(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField*);
    void on_td_settlement_confirm(CThostFtdcRspInfoField*);
    void on_rtn_depth_market_data(CThostFtdcDepthMarketDataField*);
    void on_rtn_order(CThostFtdcOrderField*);
    void on_rtn_trade(CThostFtdcTradeField*);
    void on_rsp_order_insert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField*);
    void on_err_rtn_order_insert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField*);
    void on_rsp_order_action(CThostFtdcRspInfoField*);
    void on_err_rtn_order_action(CThostFtdcRspInfoField*);
    void on_rsp_qry_instrument(CThostFtdcInstrumentField*, CThostFtdcRspInfoField*, bool last);
    void on_rsp_qry_trading_account(CThostFtdcTradingAccountField*, CThostFtdcRspInfoField*);

private:
    // 生成自增报单引用号（OrderRef），用于关联本地订单与 CTP 回报
    std::string gen_order_ref() {
        return std::to_string(++order_ref_);
    }

    CThostFtdcMdApi*     md_api_ = nullptr;
    CThostFtdcTraderApi* td_api_ = nullptr;
    CtpMdSpi*  md_spi_ = nullptr;
    CtpTradeSpi* td_spi_ = nullptr;

    std::string broker_id_, user_id_, password_;
    std::string app_id_, auth_code_;
    std::string md_front_, td_front_;
    int  front_id_ = 0, session_id_ = 0;
    long order_ref_ = 0;
    int  req_id_ = 0;

    std::vector<std::string> subscribed_;
    // symbol -> 最小变动价位(price tick)，合约查询后缓存，供下单限价取整
    std::map<std::string, double> price_tick_;
    std::atomic<bool> inited_;
    std::atomic<bool> md_logged_in_{false};
    std::mutex mtx_;
    // orderref -> (symbol, exchange) 供撤单时还原合约信息
    std::map<std::string, std::pair<std::string, Exchange>> ref_info_;
};

// ================== 实现 ==================

inline void CtpMdSpi::OnFrontConnected() { gw_->on_md_connected(); }

inline void CtpMdSpi::OnFrontDisconnected(int nReason) {
    gw_->on_log("行情断开, reason=" + std::to_string(nReason));
}

inline void CtpMdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField* info, int, bool) {
    gw_->on_md_login(info);
}

inline void CtpMdSpi::OnRspSubMarketData(CThostFtdcSpecificInstrumentField*, CThostFtdcRspInfoField* info, int, bool) {
    if (info && info->ErrorID != 0)
        gw_->on_log("订阅回报错误: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
}

inline void CtpMdSpi::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* p) {
    if (!p) return;
    gw_->on_rtn_depth_market_data(p);
}

inline void CtpTradeSpi::OnFrontConnected() { gw_->on_td_connected(); }

inline void CtpTradeSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info, int, bool) {
    gw_->on_td_login(login, info);
}

inline void CtpTradeSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField* a, CThostFtdcRspInfoField* info, int, bool) {
    gw_->on_td_authenticate(a, info);
}

inline void CtpTradeSpi::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField* info, int, bool) {
    gw_->on_td_settlement_confirm(info);
}

inline void CtpTradeSpi::OnRtnOrder(CThostFtdcOrderField* p) { if (p) gw_->on_rtn_order(p); }
inline void CtpTradeSpi::OnRtnTrade(CThostFtdcTradeField* p) { if (p) gw_->on_rtn_trade(p); }
inline void CtpTradeSpi::OnRspOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* i, int, bool) { gw_->on_rsp_order_insert(p, i); }
inline void CtpTradeSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* i) { gw_->on_err_rtn_order_insert(p, i); }
inline void CtpTradeSpi::OnRspOrderAction(CThostFtdcInputOrderActionField*, CThostFtdcRspInfoField* i, int, bool) { gw_->on_rsp_order_action(i); }
inline void CtpTradeSpi::OnErrRtnOrderAction(CThostFtdcOrderActionField*, CThostFtdcRspInfoField* i) { gw_->on_err_rtn_order_action(i); }
inline void CtpTradeSpi::OnRspQryInstrument(CThostFtdcInstrumentField* p, CThostFtdcRspInfoField* i, int, bool last) { gw_->on_rsp_qry_instrument(p, i, last); }
inline void CtpTradeSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* i, int, bool) { gw_->on_rsp_qry_trading_account(p, i); }

// -------- 连接 / 登录流程 --------
// 连接流程：创建 Md/Td Api，注册 SPI 与前置地址，Init() 触发 OnFrontConnected
inline void CtpGateway::connect(const std::map<std::string, std::string>& settings) {
    md_front_   = settings.count("md_front")   ? settings.at("md_front")   : "";
    td_front_   = settings.count("td_front")   ? settings.at("td_front")   : "";
    broker_id_  = settings.count("broker_id")  ? settings.at("broker_id")  : "";
    user_id_    = settings.count("user_id")    ? settings.at("user_id")    : "";
    password_   = settings.count("password")   ? settings.at("password")   : "";
    // 看穿式终端认证（SimNow 默认 AppID=simnow_client_test, AuthCode=0000000000000000）
    // 配置 auth_code 时先 ReqAuthenticate 再登录；留空则跳过认证（部分柜台可不开终端认证）。
    app_id_     = settings.count("app_id")     ? settings.at("app_id")     : "simnow_client_test";
    auth_code_  = settings.count("auth_code")  ? settings.at("auth_code")  : "";
    if (settings.count("instruments"))
        for (auto& s : split_csv(settings.at("instruments")))
            if (!s.empty()) subscribed_.push_back(s);

    if (md_front_.empty() || td_front_.empty() || user_id_.empty()) {
        on_log("CTP 连接参数缺失(md_front/td_front/user_id)");
        return;
    }

    // 行情
    md_api_ = CThostFtdcMdApi::CreateFtdcMdApi("ctp_md_flow");
    md_spi_ = new CtpMdSpi(this);
    md_api_->RegisterSpi(md_spi_);
    md_api_->RegisterFront((char*)md_front_.c_str());
    md_api_->Init();

    // 交易
    td_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi("ctp_td_flow");
    td_spi_ = new CtpTradeSpi(this);
    td_api_->RegisterSpi(td_spi_);
    td_api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    td_api_->SubscribePublicTopic(THOST_TERT_QUICK);
    td_api_->RegisterFront((char*)td_front_.c_str());
    td_api_->Init();

    on_log("CTP 接口初始化完成, 等待前置连接...");
    inited_ = true;
}

// 行情前置已连：行情 API 不支持看穿式认证，直接登录后订阅
inline void CtpGateway::on_md_connected() {
    on_log("行情前置已连接, 发起登录");
    md_logged_in_ = false;
    md_login();
}

// 行情登录：填充登录字段并发起 ReqUserLogin，成功后订阅
inline void CtpGateway::md_login() {
    CThostFtdcReqUserLoginField req{};
    set_field(req.BrokerID, sizeof(req.BrokerID), broker_id_);
    set_field(req.UserID,   sizeof(req.UserID),   user_id_);
    set_field(req.Password, sizeof(req.Password), password_);
    set_field(req.UserProductInfo, sizeof(req.UserProductInfo), "ltc");
    md_api_->ReqUserLogin(&req, ++req_id_);
}

// 行情登录回调：成功后按订阅列表发起 SubscribeMarketData
inline void CtpGateway::on_md_login(CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        on_log("行情登录失败: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
        return;
    }
    on_log("行情已登录");
    md_logged_in_ = true;
    if (subscribed_.empty()) return;
    // CTP 行情订阅只接受合约代码("rb2609")，需去掉 vt_symbol 的交易所后缀(".SHFE")
    std::vector<std::string> insts;
    for (auto& s : subscribed_) insts.push_back(ctp_inst_id(s));
    std::vector<char*> ptrs;
    for (auto& s : insts) ptrs.push_back((char*)s.c_str());
    md_api_->SubscribeMarketData(ptrs.data(), (int)ptrs.size());
    on_log("已订阅行情: " + std::to_string(subscribed_.size()) + " 个合约");
}

// 交易前置已连：先看穿式认证(若配置)，成功后再登录
inline void CtpGateway::on_td_connected() {
    on_log("交易前置已连接");
    if (auth_code_.empty()) {
        td_login();
    } else {
        CThostFtdcReqAuthenticateField req{};
        set_field(req.BrokerID,  sizeof(req.BrokerID),  broker_id_);
        set_field(req.UserID,    sizeof(req.UserID),    user_id_);
        set_field(req.AuthCode,  sizeof(req.AuthCode),  auth_code_);
        set_field(req.AppID,     sizeof(req.AppID),     app_id_);
        td_api_->ReqAuthenticate(&req, ++req_id_);
        on_log("交易发起看穿式认证 AppID=" + app_id_);
    }
}

// 交易认证回调：成功后登录
inline void CtpGateway::on_td_authenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        on_log("交易认证失败: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
        return;
    }
    on_log("交易认证成功, 发起登录");
    td_login();
}

// 交易登录：填充登录字段并发起 ReqUserLogin
inline void CtpGateway::td_login() {
    CThostFtdcReqUserLoginField req{};
    set_field(req.BrokerID, sizeof(req.BrokerID), broker_id_);
    set_field(req.UserID,   sizeof(req.UserID),   user_id_);
    set_field(req.Password, sizeof(req.Password), password_);
    set_field(req.UserProductInfo, sizeof(req.UserProductInfo), "ltc");
    td_api_->ReqUserLogin(&req, ++req_id_);
}

// 登录成功回调：保存 FrontID/SessionID，随后发起结算单确认
inline void CtpGateway::on_td_login(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        on_log("交易登录失败: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
        return;
    }
    if (login) {
        front_id_  = login->FrontID;
        session_id_ = login->SessionID;
        on_log("交易登录成功 front=" + std::to_string(front_id_) + " session=" + std::to_string(session_id_));
    }
    CThostFtdcSettlementInfoConfirmField cf{};
    set_field(cf.BrokerID,  sizeof(cf.BrokerID),  broker_id_);
    set_field(cf.InvestorID, sizeof(cf.InvestorID), user_id_);
    td_api_->ReqSettlementInfoConfirm(&cf, ++req_id_);
}

// 结算确认成功：发起合约查询(全部)与资金查询，完成登录链
inline void CtpGateway::on_td_settlement_confirm(CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0) {
        on_log("结算确认失败: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
        return;
    }
    on_log("结算确认成功, 查询合约与资金");
    CThostFtdcQryInstrumentField q{};
    td_api_->ReqQryInstrument(&q, ++req_id_);   // 空 -> 全部合约
    CThostFtdcQryTradingAccountField qa{};
    set_field(qa.BrokerID, sizeof(qa.BrokerID), broker_id_);
    set_field(qa.InvestorID, sizeof(qa.InvestorID), user_id_);
    td_api_->ReqQryTradingAccount(&qa, ++req_id_);
}

// --------- 行情 -> TickData ---------
// 深度行情推送：把 CTP DepthMarketData 映射为 TickData 并 on_tick 推送
inline void CtpGateway::on_rtn_depth_market_data(CThostFtdcDepthMarketDataField* p) {
    TickData tk;
    tk.symbol   = cstr(p->InstrumentID);
    tk.exchange = ctp_exchange(p->ExchangeID);
    // CTP 行情的 ExchangeID 常为空格/空串，无法据此推断交易所；用已订阅列表
    // (symbol.exchange 形式) 反查兜底，使 vt_symbol 不再变成 "rbXXXX.NONE"。
    if (tk.exchange == Exchange::NONE) {
        for (auto& vt : subscribed_) {
            if (ctp_inst_id(vt) == tk.symbol) {
                auto pos = vt.find('.');
                if (pos != std::string::npos) { tk.exchange = ctp_exchange(vt.c_str() + pos + 1); break; }
            }
        }
    }
    tk.vt_symbol = make_vt_symbol(tk.symbol, tk.exchange);
    tk.datetime = ctp_time_to_ms(p->TradingDay, p->UpdateTime, p->UpdateMillisec);
    tk.last_price = p->LastPrice;
    tk.last_volume = 0.0;
    tk.bid_price_1 = p->BidPrice1; tk.bid_volume_1 = p->BidVolume1;
    tk.ask_price_1 = p->AskPrice1; tk.ask_volume_1 = p->AskVolume1;
    tk.open_interest = p->OpenInterest;
    tk.volume = p->Volume;
    tk.limit_up = p->UpperLimitPrice; tk.limit_down = p->LowerLimitPrice;
    on_tick(std::move(tk));
}

// --------- 报单/成交 -> OrderData/TradeData ---------
// 报单回报：把 CTP OrderField 映射为 OrderData（含开平/方向/状态）并 on_order 推送
inline void CtpGateway::on_rtn_order(CThostFtdcOrderField* p) {
    OrderData o;
    o.symbol = cstr(p->InstrumentID);
    o.exchange = ctp_exchange(p->ExchangeID);
    o.vt_symbol = make_vt_symbol(o.symbol, o.exchange);
    o.orderid = cstr(p->OrderRef);
    o.vt_orderid = make_vt_orderid(gateway_name_, o.orderid);
    o.direction = (p->Direction == THOST_FTDC_D_Buy) ? Direction::LONG : Direction::SHORT;
    char off = p->CombOffsetFlag[0];
    o.offset = (off == THOST_FTDC_OF_Open) ? Offset::OPEN :
               (off == THOST_FTDC_OF_CloseToday) ? Offset::CLOSETODAY :
               (off == THOST_FTDC_OF_CloseYesterday) ? Offset::CLOSEYESTERDAY : Offset::CLOSE;
    o.type = (p->OrderPriceType == THOST_FTDC_OPT_AnyPrice) ? OrderType::MARKET : OrderType::LIMIT;
    o.status = ctp_order_status(p->OrderStatus);
    o.price = p->LimitPrice;
    o.volume = p->VolumeTotalOriginal;
    o.traded = p->VolumeTraded;
    o.datetime = now_ms();
    o.gateway_name = gateway_name_;
    on_order(std::move(o));
}

// 成交回报：把 CTP TradeField 映射为 TradeData 并 on_trade 推送
inline void CtpGateway::on_rtn_trade(CThostFtdcTradeField* p) {
    TradeData td;
    td.symbol = cstr(p->InstrumentID);
    td.exchange = ctp_exchange(p->ExchangeID);
    td.vt_symbol = make_vt_symbol(td.symbol, td.exchange);
    td.orderid = cstr(p->OrderRef);
    td.tradeid = cstr(p->TradeID);
    td.vt_orderid = make_vt_orderid(gateway_name_, td.orderid);
    td.vt_tradeid = make_vt_orderid(gateway_name_, td.tradeid);
    td.direction = (p->Direction == THOST_FTDC_D_Buy) ? Direction::LONG : Direction::SHORT;
    char off = p->OffsetFlag;
    td.offset = (off == THOST_FTDC_OF_Open) ? Offset::OPEN :
                (off == THOST_FTDC_OF_CloseToday) ? Offset::CLOSETODAY :
                (off == THOST_FTDC_OF_CloseYesterday) ? Offset::CLOSEYESTERDAY : Offset::CLOSE;
    td.price = p->Price;
    td.volume = p->Volume;
    td.datetime = ctp_time_to_ms(p->TradeDate, p->TradeTime, 0);
    td.gateway_name = gateway_name_;
    on_trade(std::move(td));
}

// 报单录入回报：仅 ErrorID!=0 时推送 REJECTED 订单（成功走 OnRtnOrder）
inline void CtpGateway::on_rsp_order_insert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* info) {
    if (!info || info->ErrorID == 0) return;  // 成功走 OnRtnOrder
    on_log("报单录入错误: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
    if (!p) return;
    OrderData o;
    o.symbol = cstr(p->InstrumentID); o.exchange = ctp_exchange(p->ExchangeID);
    o.vt_symbol = make_vt_symbol(o.symbol, o.exchange);
    o.orderid = cstr(p->OrderRef);
    o.vt_orderid = make_vt_orderid(gateway_name_, o.orderid);
    o.status = Status::REJECTED; o.gateway_name = gateway_name_;
    on_order(std::move(o));
}

// 报单被拒(交易所前置)：推送 REJECTED 订单
inline void CtpGateway::on_err_rtn_order_insert(CThostFtdcInputOrderField* p, CThostFtdcRspInfoField* info) {
    on_log("报单被拒(OnErrRtnOrderInsert): " + (info ? std::to_string(info->ErrorID) : std::string()) + " " + (info ? cstr(info->ErrorMsg) : ""));
    if (!p) return;
    OrderData o;
    o.symbol = cstr(p->InstrumentID); o.exchange = ctp_exchange(p->ExchangeID);
    o.vt_symbol = make_vt_symbol(o.symbol, o.exchange);
    o.orderid = cstr(p->OrderRef);
    o.vt_orderid = make_vt_orderid(gateway_name_, o.orderid);
    o.status = Status::REJECTED; o.gateway_name = gateway_name_;
    on_order(std::move(o));
}

// 撤单回报：仅错误时记录日志
inline void CtpGateway::on_rsp_order_action(CThostFtdcRspInfoField* info) {
    if (info && info->ErrorID != 0)
        on_log("撤单回报错误: " + std::to_string(info->ErrorID) + " " + cstr(info->ErrorMsg));
}
// 撤单被拒：记录日志
inline void CtpGateway::on_err_rtn_order_action(CThostFtdcRspInfoField* info) {
    on_log("撤单被拒: " + (info ? std::to_string(info->ErrorID) : std::string()) + " " + (info ? cstr(info->ErrorMsg) : ""));
}

// 合约查询回报：映射为 ContractData，active 由 IsTrading=='1' 判定；last 标记查询结束
inline void CtpGateway::on_rsp_qry_instrument(CThostFtdcInstrumentField* p, CThostFtdcRspInfoField* info, bool last) {
    if (p && info && info->ErrorID == 0) {
        ContractData c;
        c.symbol = cstr(p->InstrumentID);
        c.exchange = ctp_exchange(p->ExchangeID);
        c.vt_symbol = make_vt_symbol(c.symbol, c.exchange);
        c.name = cstr(p->InstrumentName);
        c.active = (p->IsTrading == '1');  // TThostFtdcBoolType: '1'=是
        c.product = ctp_product(p->ProductClass);
        c.size = p->VolumeMultiple;
        c.pricetick = p->PriceTick;
        price_tick_[c.symbol] = c.pricetick;  // 缓存 tick，供下单限价取整
        c.min_volume = p->MinLimitOrderVolume;
        c.max_volume = p->MaxLimitOrderVolume;
        // 净持仓(单向)模式：上期/中金/能源中心按净持仓，大商/郑商按多空双向
        c.net_position = (c.exchange == Exchange::SHFE ||
                          c.exchange == Exchange::CFFEX ||
                          c.exchange == Exchange::INE);
        // 期权合约补充标的/行权价/类型/到期等元信息
        if (c.product == Product::OPTION) {
            c.option_strike = p->StrikePrice;
            c.option_underlying = cstr(p->UnderlyingInstrID);
            c.option_type = (p->OptionsType == THOST_FTDC_CP_CallOptions)
                                ? OptionType::CALL : OptionType::PUT;
            c.option_expiry = cstr(p->ExpireDate);
            c.option_portfolio = cstr(p->UnderlyingInstrID);
            c.option_index = c.option_underlying + "." +
                             std::to_string((int)p->StrikePrice) + "." +
                             (c.option_type == OptionType::CALL ? "C" : "P");
        }
        c.gateway_name = gateway_name_;
        on_contract(std::move(c));
    }
    if (last) on_log("合约查询完成");
}

// 资金查询回报：映射为 AccountData 并 on_account 推送
inline void CtpGateway::on_rsp_qry_trading_account(CThostFtdcTradingAccountField* p, CThostFtdcRspInfoField* info) {
    if (p && info && info->ErrorID == 0) {
        AccountData a;
        a.accountid = cstr(p->AccountID);
        a.balance = p->Balance;
        a.frozen = p->FrozenCash;
        a.gateway_name = gateway_name_;
        on_account(std::move(a));
        on_log("资金查询: 权益=" + std::to_string(p->Balance));
    }
}

// --------- 下单 / 撤单 ---------
// 下单：解析 vt_symbol 得到 symbol/exchange，生成 OrderRef 并写入 ref_info_（撤单还原用）；
//   填充 InputOrderField（方向/开平/价格类型/数量/GFD 等），ReqOrderInsert 异步发送；
//   返回 vt_orderid = gateway.OrderRef
inline std::string CtpGateway::send_order(const OrderRequest& req) {
    if (!td_api_) { on_log("交易API未初始化"); return ""; }
    std::string symbol; Exchange ex;
    BaseStrategy::parse_vt_symbol(make_vt_symbol(req.symbol, req.exchange), symbol, ex);

    std::string ref = gen_order_ref();
    ref_info_[ref] = {symbol, ex};
    CThostFtdcInputOrderField o{};
    set_field(o.BrokerID,  sizeof(o.BrokerID),  broker_id_);
    set_field(o.InvestorID, sizeof(o.InvestorID), user_id_);
    set_field(o.UserID,    sizeof(o.UserID),    user_id_);
    set_field(o.InstrumentID, sizeof(o.InstrumentID), symbol);
    set_field(o.ExchangeID,   sizeof(o.ExchangeID),   exchange_to_str(ex));
    set_field(o.OrderRef,  sizeof(o.OrderRef),  ref);
    o.Direction = (req.direction == Direction::LONG) ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    // 开平标志：取第一个字符
    char offc = '0';
    if (req.offset == Offset::OPEN) offc = THOST_FTDC_OF_Open;
    else if (req.offset == Offset::CLOSETODAY) offc = THOST_FTDC_OF_CloseToday;
    else if (req.offset == Offset::CLOSEYESTERDAY) offc = THOST_FTDC_OF_CloseYesterday;
    else offc = THOST_FTDC_OF_Close;
    o.CombOffsetFlag[0] = offc; o.CombOffsetFlag[1] = '\0';
    o.CombHedgeFlag[0]  = THOST_FTDC_HF_Speculation; o.CombHedgeFlag[1] = '\0';
    o.OrderPriceType = (req.type == OrderType::MARKET) ? THOST_FTDC_OPT_AnyPrice : THOST_FTDC_OPT_LimitPrice;
    o.LimitPrice = req.price;
    // 按合约最小变动价位取整，避免「价格非最小报单倍数」被柜台拒单
    auto pit = price_tick_.find(symbol);
    if (pit != price_tick_.end() && pit->second > 0)
        o.LimitPrice = std::round(o.LimitPrice / pit->second) * pit->second;
    o.VolumeTotalOriginal = (int)req.volume;
    o.TimeCondition = THOST_FTDC_TC_GFD;
    o.VolumeCondition = THOST_FTDC_VC_AV;
    o.MinVolume = 1;
    o.ContingentCondition = THOST_FTDC_CC_Immediately;
    o.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    o.IsAutoSuspend = 0;
    o.UserForceClose = 0;

    int r = td_api_->ReqOrderInsert(&o, ++req_id_);
    if (r != 0) { on_log("下单失败 ReqOrderInsert ret=" + std::to_string(r)); return ""; }
    std::string vt_oid = make_vt_orderid(gateway_name_, ref);
    on_log("已发送报单 ref=" + ref + " " + symbol + " " +
           direction_to_str(req.direction) + " " + offset_to_str(req.offset) +
           " vol=" + std::to_string(req.volume));
    return vt_oid;
}

// 撤单：从 vt_orderid 提取 OrderRef，用 ref_info_ 还原 symbol/exchange（兜底用 req）；
//   填充 InputOrderActionField(ActionFlag=Delete) 并 ReqOrderAction
inline void CtpGateway::cancel_order(const CancelRequest& req) {
    if (!td_api_) { on_log("交易API未初始化"); return; }
    std::string ref = req.vt_orderid;
    auto p = ref.find('.');
    if (p != std::string::npos) ref = ref.substr(p + 1);
    if (ref.empty()) { on_log("撤单失败: 空 orderref"); return; }

    std::string symbol; Exchange ex;
    auto it = ref_info_.find(ref);
    if (it != ref_info_.end()) {
        symbol = it->second.first; ex = it->second.second;
    } else {
        symbol = req.symbol; ex = req.exchange;
        if (symbol.empty()) BaseStrategy::parse_vt_symbol(req.vt_orderid, symbol, ex);
    }

    CThostFtdcInputOrderActionField a{};
    set_field(a.BrokerID,  sizeof(a.BrokerID),  broker_id_);
    set_field(a.InvestorID, sizeof(a.InvestorID), user_id_);
    set_field(a.OrderRef,  sizeof(a.OrderRef),  ref);
    set_field(a.ExchangeID,   sizeof(a.ExchangeID),   exchange_to_str(ex));
    set_field(a.InstrumentID, sizeof(a.InstrumentID), symbol);
    a.FrontID = front_id_;
    a.SessionID = session_id_;
    a.ActionFlag = THOST_FTDC_AF_Delete;
    a.LimitPrice = 0;
    a.VolumeChange = 0;
    int r = td_api_->ReqOrderAction(&a, ++req_id_);
    if (r != 0) on_log("撤单失败 ReqOrderAction ret=" + std::to_string(r));
}

// 订阅：增量合并订阅列表并向行情 API 发起订阅（避免重复）
inline void CtpGateway::subscribe(const std::vector<std::string>& vt_symbols) {
    for (auto& s : vt_symbols)
        if (std::find(subscribed_.begin(), subscribed_.end(), s) == subscribed_.end())
            subscribed_.push_back(s);
    // 仅行情已登录后再向交易所发起订阅；未登录时由 on_md_login 统一订阅
    if (md_api_ && md_logged_in_ && !subscribed_.empty()) {
        // CTP 行情订阅只接受合约代码("rb2609")，需去掉 vt_symbol 的交易所后缀(".SHFE")
        std::vector<std::string> insts;
        for (auto& s : subscribed_) insts.push_back(ctp_inst_id(s));
        std::vector<char*> ptrs;
        for (auto& s : insts) ptrs.push_back((char*)s.c_str());
        md_api_->SubscribeMarketData(ptrs.data(), (int)ptrs.size());
    }
}

// 释放 Md/Td Api 与 SPI，退出接口
inline void CtpGateway::close() {
    if (td_api_)  { td_api_->Release();  td_api_->Join();  td_api_ = nullptr; }
    if (md_api_)  { md_api_->Release();  md_api_->Join();  md_api_ = nullptr; }
    delete td_spi_; td_spi_ = nullptr;
    delete md_spi_; md_spi_ = nullptr;
    inited_ = false;
    on_log("CTP 接口已关闭");
}

} // namespace ltc
