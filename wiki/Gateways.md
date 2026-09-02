# 交易接口（Gateway）

每个接口继承 `BaseGateway`（`include/ltc/core/gateway.hpp`），负责建连、订阅、发单，并把通道回传的数据经 `protected on_xxx(T d)` 推送进 `EventEngine`（内部 `std::move` 进 `Event`，零拷贝）。`gateway_name_` 既是日志前缀，也是 `vt_orderid` 的路由前缀（撤单据此定位接口）。

基类需子类实现：`connect(settings)` / `close()` / `send_order(req)` / `cancel_order(req)` / `subscribe(vt_symbols)`。

| 接口 | 文件 | 用途 | 行情回推 |
| --- | --- | --- | --- |
| `CsvReplayGateway` | `gateway/csv_gateway.hpp` | CSV 分钟线**模拟盘**（回放 bar，推 `on_bar`） | `on_bar` |
| `BinanceGateway` | `gateway/binance_gateway.hpp` | 币安 U 本位（默认 Stub 传输，零依赖可跑，演示链路） | `on_tick` / `on_bar` |
| `CtpGateway` | `gateway/ctp_gateway.hpp` | 上期技术 CTP 期货实盘（MdApi + TraderApi） | `on_tick` / `on_order` / `on_trade` / `on_account` / `on_contract` |
| `TickCsvGateway` | `gateway/tick_csv_gateway.hpp` | tick CSV **直喂回放**（推 `on_tick`，不经分钟聚合） | `on_tick`（内置 tick 级撮合） |

> 各网关下单语义一致：限价单按 `last_price` 穿越成交（回测）/ 交易所撮合（实盘）；市价单按 `last_price` + 不利方向滑点成交（回测）。

## CtpGateway（实盘）

- 依赖：CTP_V6.7.11 头 + `win64/thostmduserapi_se.lib` / `thosttraderapi_se.lib`。
- 登录链：`MdApi/TdApi Init()` → `OnFrontConnected` →（看穿式认证）→ `ReqUserLogin` → `ReqSettlementInfoConfirm` → `ReqQryInstrument` / `ReqQryTradingAccount`。
- `connect(settings)` 读取的 `[ctp]` 段键：`md_front` / `td_front` / `broker_id` / `user_id` / `password` / `app_id`（默认 `simnow_client_test`）/ `auth_code` / `instruments`（逗号分隔，可选预订阅）。
- 行情订阅只接受合约代码（`rb2609`），接口内部自动去掉 `.SHFE` 后缀。
- `send_order` 返回 `vt_orderid = gateway.OrderRef`，撤单据此还原合约信息（`ref_info_`）。

已知限制（见 `ctp_gateway.hpp` 注释）：SPI 回调在 CTP 线程触发，目前**直接回调网关**（未强制切回事件线程）；查询未做分页/节流；登录链未做断线重连。

## 写自己的网关

```cpp
class MyGateway : public ltc::BaseGateway {
public:
    MyGateway(ltc::EventEngine* ee, const std::string& name) : BaseGateway(ee, name) {}
    void connect(const std::map<std::string,std::string>& s) override { /* 建连/登录 */ }
    void close() override {}
    std::string send_order(const ltc::OrderRequest& req) override { /* 发单，返回 orderid */ }
    void cancel_order(const ltc::CancelRequest& req) override {}
    void subscribe(const std::vector<std::string>& vt) override {}
protected:
    // 收到数据后推送：
    //   on_tick(TickData{...}); on_bar(BarData{...}); on_order(...); on_trade(...);
    //   on_account(...); on_contract(...);
};
```

随后把它 `add_gateway` 到 `MainEngine` 即可，与内置网关无差别。
