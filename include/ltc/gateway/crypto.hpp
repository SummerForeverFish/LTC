// crypto.hpp - 纯 C++17 实现的 SHA256 / HMAC-SHA256（零外部依赖）
//
// 职责：提供交易所 REST 请求签名所需的密码学原语，被 BinanceGateway
//       用于构造 HMAC-SHA256 签名（见 binance_gateway.hpp 的 send_order/cancel_order）。
//       本身不是网关，与 BaseGateway 无继承关系，是底层工具模块。
// 适用：任何需要 HMAC-SHA256 签名的 HTTP 接口（目前仅币安合约使用）。
// 已知限制：纯软件实现、非恒定时间，仅适合签名小字符串；每次调用会堆分配内部缓冲；
//           线程安全（无共享可变状态），但高频大批量数据下性能不如硬件/库实现。
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace ltc {

namespace detail {
    // 32 位循环右移（SHA-256 压缩函数 Σ/σ 使用）
    static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    // SHA-256 的 64 个轮常量 K（前 64 个素数立方根小数部分的头 32 位）
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    // 标准 SHA-256：data/len 为输入，digest 输出 32 字节。
    // 流程：消息 padding（补 0x80 + 0 填充至 56 字节边界 + 64 位大端长度），
    //       分块(512bit)做 64 轮压缩，最终 h[0..7] 大端拼接成摘要。
    static inline void sha256(const uint8_t* data, size_t len, uint8_t digest[32]) {
        uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        std::vector<uint8_t> msg(data, data + len);
        uint64_t bitlen = (uint64_t)len * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0x00);
        for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bitlen >> (i * 8)));
        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t)msg[off+4*i]<<24)|((uint32_t)msg[off+4*i+1]<<16)|
                       ((uint32_t)msg[off+4*i+2]<<8)|((uint32_t)msg[off+4*i+3]);
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
                uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
                uint32_t ch = (e&f)^((~e)&g);
                uint32_t t1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
                uint32_t maj = (a&b)^(a&c)^(b&c);
                uint32_t t2 = S0 + maj;
                hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
            }
            h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        }
        for (int i = 0; i < 8; ++i) {
            digest[4*i]   = (uint8_t)(h[i]>>24);
            digest[4*i+1] = (uint8_t)(h[i]>>16);
            digest[4*i+2] = (uint8_t)(h[i]>>8);
            digest[4*i+3] = (uint8_t)(h[i]);
        }
    }
} // detail

// 对字符串做 SHA-256 并返回小写十六进制串（长度 64）
inline std::string sha256_hex(const std::string& s) {
    uint8_t d[32];
    detail::sha256((const uint8_t*)s.data(), s.size(), d);
    static const char* hx = "0123456789abcdef";
    std::string out; out.reserve(64);
    for (int i = 0; i < 32; ++i) { out.push_back(hx[d[i]>>4]); out.push_back(hx[d[i]&0xf]); }
    return out;
}

// HMAC-SHA256：key 为 API secret，msg 为待签名 query 串；返回十六进制签名串。
// 算法：inner = SHA256((key^0x36) || msg)，outer = SHA256((key^0x5c) || inner)。
// 若 key 超 64 字节先用 SHA256 压缩到 32 字节（符合 RFC 2104）。
inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    std::vector<uint8_t> k(64, 0x36);
    if (key.size() > 64) {
        uint8_t d[32]; detail::sha256((const uint8_t*)key.data(), key.size(), d);
        for (int i = 0; i < 32; ++i) k[i] = d[i];
    } else {
        for (size_t i = 0; i < key.size(); ++i) k[i] = (uint8_t)key[i];
    }
    // 构造 inner 输入：64 字节 ipad(key^0x36) 拼接原始消息
    std::vector<uint8_t> inner(64 + msg.size());
    for (int i = 0; i < 64; ++i) inner[i] = k[i] ^ 0x36;
    std::memcpy(inner.data() + 64, msg.data(), msg.size());
    std::string inner_hash = sha256_hex(std::string((const char*)inner.data(), inner.size()));
    // inner_hash 是 hex，需要转回字节再外层
    std::vector<uint8_t> inner_bytes;
    for (size_t i = 0; i < inner_hash.size(); i += 2) {
        uint8_t b = (uint8_t)std::strtol(inner_hash.substr(i,2).c_str(), nullptr, 16);
        inner_bytes.push_back(b);
    }
    // 构造 outer 输入：64 字节 opad(key^0x5c) 拼接 inner 的字节摘要
    std::vector<uint8_t> outer(64 + inner_bytes.size());
    for (int i = 0; i < 64; ++i) outer[i] = k[i] ^ 0x5c;
    std::memcpy(outer.data() + 64, inner_bytes.data(), inner_bytes.size());
    uint8_t d[32]; detail::sha256(outer.data(), outer.size(), d);
    static const char* hx = "0123456789abcdef";
    std::string out; out.reserve(64);
    for (int i = 0; i < 32; ++i) { out.push_back(hx[d[i]>>4]); out.push_back(hx[d[i]&0xf]); }
    return out;
}

} // namespace ltc
