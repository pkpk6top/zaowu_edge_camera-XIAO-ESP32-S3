/*
 * Ed25519（PureEdDSA，RFC 8032）
 *
 * ⚠ 为什么要自带一份：ESP-IDF v5.5 内置的 mbedtls 3.6.6 只声明了 PSA_ALG_PURE_EDDSA
 *   这个算法号，库层面**没有实现**（library 下既无 eddsa 源文件，也无 PURE_EDDSA 实现），
 *   公开 API 里也拿不到 Ed25519 签名。所以这里只做「一层薄实现」：
 *   曲线运算用 mbedtls 的大数（已验证过的域算术/模幂/模逆），哈希用 mbedtls 的 SHA-512，
 *   RFC 8032 的公式与常量（p / L / d / sqrt(-1) / 基点）在下面逐字写出。
 *
 * 用途单一：设备身份自证（注册时签一次）。所以不做常数时间处理、不做预计算表 ——
 * 一次签名的开销（约百毫秒级）在这里完全可以接受。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ED25519_SEED_LEN    32
#define ED25519_PUBKEY_LEN  32
#define ED25519_SIGN_LEN    64

/**
 * @brief 由 32 字节种子（私钥）推导 32 字节公钥。
 */
esp_err_t ed25519_public_key(const uint8_t seed[ED25519_SEED_LEN], uint8_t pub[ED25519_PUBKEY_LEN]);

/**
 * @brief 用种子对消息签名，输出 64 字节签名（R || S）。
 */
esp_err_t ed25519_sign(const uint8_t seed[ED25519_SEED_LEN], const uint8_t *msg, size_t msg_len,
                       uint8_t sig[ED25519_SIGN_LEN]);

/**
 * @brief 验签。设备运行链路用不到，供自检与对端联调。
 */
esp_err_t ed25519_verify(const uint8_t pub[ED25519_PUBKEY_LEN], const uint8_t *msg, size_t msg_len,
                         const uint8_t sig[ED25519_SIGN_LEN]);

/**
 * @brief 用 RFC 8032 §7.1 的两组测试向量自检（约 0.5s）。
 *
 * 放在 app_main 里跑一次，可以在现场就确认密码学实现没被改坏。
 *
 * @return ESP_OK 两组向量都对得上
 */
esp_err_t ed25519_selftest(void);

#ifdef __cplusplus
}
#endif
