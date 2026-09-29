#include "ed25519.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha512.h"

static const char *TAG = "ed25519";

/* ───────────────────────── 曲线常量（小端 32 字节） ───────────────────────── */

/* p = 2^255 - 19 */
static const uint8_t kP[32] = {
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};

/* L = 2^252 + 27742317777372353535851937790883648493（点群阶） */
static const uint8_t kL[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

/* d = -121665 / 121666 mod p（Edwards 曲线常数） */
static const uint8_t kD[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75, 0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c, 0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
};

/* sqrt(-1) mod p（解压缩点算候选 x 时用来二次校正） */
static const uint8_t kSqrtM1[32] = {
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4, 0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b, 0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
};

/* 基点 B */
static const uint8_t kBaseX[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9, 0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0, 0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21,
};
static const uint8_t kBaseY[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

/* (p - 5) / 8 = 2^252 - 3，解压缩点时求平方根用的指数 */
static const uint8_t kExpSqrt[32] = {
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f,
};

/* ───────────────────────── 基础工具 ───────────────────────── */

static esp_err_t mpi_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == MBEDTLS_ERR_MPI_ALLOC_FAILED) ? ESP_ERR_NO_MEM : ESP_FAIL;
}

static esp_err_t fe_load(mbedtls_mpi *r, const uint8_t *le, size_t len)
{
    return mpi_result(mbedtls_mpi_read_binary_le(r, le, len));
}

static esp_err_t fe_store(const mbedtls_mpi *a, uint8_t out[32])
{
    if (mbedtls_mpi_cmp_int(a, 0) < 0) {
        return ESP_FAIL;
    }
    return mpi_result(mbedtls_mpi_write_binary_le(a, out, 32));
}

/*
 * 域（mod p）四则。
 * 一律「算到独立临时量再拷回 r」：省掉逐个核对 mbedtls 的别名规则这件事，
 * 这里多几次 mpi_copy 完全无所谓（一次签名就是终点）。
 */
static esp_err_t fe_add(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b)
{
    mbedtls_mpi p, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_add_mpi(&t, a, b));
    }
    if (err == ESP_OK && mbedtls_mpi_cmp_mpi(&t, &p) >= 0) {
        err = mpi_result(mbedtls_mpi_sub_mpi(&t, &t, &p));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&t);
    return err;
}

static esp_err_t fe_sub(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b)
{
    mbedtls_mpi p, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_sub_mpi(&t, a, b));
    }
    if (err == ESP_OK && mbedtls_mpi_cmp_int(&t, 0) < 0) {
        err = mpi_result(mbedtls_mpi_add_mpi(&t, &t, &p));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&t);
    return err;
}

static esp_err_t fe_mul(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b)
{
    mbedtls_mpi p, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_mul_mpi(&t, a, b));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_mod_mpi(&t, &t, &p));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&t);
    return err;
}

static esp_err_t fe_sqr(mbedtls_mpi *r, const mbedtls_mpi *a)
{
    return fe_mul(r, a, a);
}

/** r = 2a mod p；别名安全（a、r 可以是同一个） */
static esp_err_t fe_dbl(mbedtls_mpi *r, const mbedtls_mpi *a)
{
    mbedtls_mpi t;
    mbedtls_mpi_init(&t);
    esp_err_t err = fe_add(&t, a, a);
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }
    mbedtls_mpi_free(&t);
    return err;
}

/** r = -a mod p */
static esp_err_t fe_neg(mbedtls_mpi *r, const mbedtls_mpi *a)
{
    mbedtls_mpi p, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        if (mbedtls_mpi_cmp_int(a, 0) == 0) {
            err = mpi_result(mbedtls_mpi_lset(&t, 0));
        } else {
            err = mpi_result(mbedtls_mpi_sub_mpi(&t, &p, a));
        }
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&t);
    return err;
}

/** r = 1/a mod p */
static esp_err_t fe_inv(mbedtls_mpi *r, const mbedtls_mpi *a)
{
    mbedtls_mpi p, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_inv_mod(&t, a, &p));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&t);
    return err;
}

/** r = a^exp mod p */
static esp_err_t fe_pow(mbedtls_mpi *r, const mbedtls_mpi *a, const uint8_t exp[32])
{
    mbedtls_mpi p, e, t;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&t);

    esp_err_t err = fe_load(&p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = fe_load(&e, exp, 32);
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_exp_mod(&t, a, &e, &p, NULL));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(r, &t));
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&t);
    return err;
}

/* ───────────────────────── 扩射坐标点 ───────────────────────── */

/* 扩射坐标 (X : Y : Z : T)，满足 x = X/Z、y = Y/Z、T = XY/Z */
typedef struct {
    mbedtls_mpi X;
    mbedtls_mpi Y;
    mbedtls_mpi Z;
    mbedtls_mpi T;
} ge_t;

static void ge_init(ge_t *p)
{
    mbedtls_mpi_init(&p->X);
    mbedtls_mpi_init(&p->Y);
    mbedtls_mpi_init(&p->Z);
    mbedtls_mpi_init(&p->T);
}

static void ge_free(ge_t *p)
{
    mbedtls_mpi_free(&p->X);
    mbedtls_mpi_free(&p->Y);
    mbedtls_mpi_free(&p->Z);
    mbedtls_mpi_free(&p->T);
}

/* 点运算动辄十来个中间量，用一个固定大小的池子统一 init/free，避免逐个漏释放 */
#define FE_POOL_SIZE 14
typedef struct {
    mbedtls_mpi v[FE_POOL_SIZE];
} fe_pool_t;

static void pool_init(fe_pool_t *pool)
{
    for (int i = 0; i < FE_POOL_SIZE; i++) {
        mbedtls_mpi_init(&pool->v[i]);
    }
}

static void pool_free(fe_pool_t *pool)
{
    for (int i = 0; i < FE_POOL_SIZE; i++) {
        mbedtls_mpi_free(&pool->v[i]);
    }
}

static esp_err_t ge_identity(ge_t *p)
{
    esp_err_t err = mpi_result(mbedtls_mpi_lset(&p->X, 0));
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(&p->Y, 1));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(&p->Z, 1));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(&p->T, 0));
    }
    return err;
}

/* 从仿射坐标 (x, y) 建点：Z = 1，T = x*y */
static esp_err_t ge_from_affine(ge_t *r, const uint8_t x[32], const uint8_t y[32])
{
    esp_err_t err = fe_load(&r->X, x, 32);
    if (err == ESP_OK) {
        err = fe_load(&r->Y, y, 32);
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(&r->Z, 1));
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->T, &r->X, &r->Y);
    }
    return err;
}

/* 点加（RFC 8032 §5.1.4 的 add-2008-hwcd-3），a = -1 的化简形式 */
static esp_err_t ge_add(ge_t *r, const ge_t *p, const ge_t *q)
{
    fe_pool_t pool;
    pool_init(&pool);

    mbedtls_mpi *A = &pool.v[0], *B = &pool.v[1], *C = &pool.v[2], *D = &pool.v[3];
    mbedtls_mpi *E = &pool.v[4], *F = &pool.v[5], *G = &pool.v[6], *H = &pool.v[7];
    mbedtls_mpi *t0 = &pool.v[8], *t1 = &pool.v[9], *d2 = &pool.v[10], *d = &pool.v[11];

    esp_err_t err = fe_load(d, kD, sizeof(kD));
    if (err == ESP_OK) {
        err = fe_dbl(d2, d);
    }
    if (err == ESP_OK) {
        err = fe_sub(t0, &p->Y, &p->X);
    }
    if (err == ESP_OK) {
        err = fe_sub(t1, &q->Y, &q->X);
    }
    if (err == ESP_OK) {
        err = fe_mul(A, t0, t1);
    }
    if (err == ESP_OK) {
        err = fe_add(t0, &p->Y, &p->X);
    }
    if (err == ESP_OK) {
        err = fe_add(t1, &q->Y, &q->X);
    }
    if (err == ESP_OK) {
        err = fe_mul(B, t0, t1);
    }
    if (err == ESP_OK) {
        err = fe_mul(C, &p->T, &q->T);
    }
    if (err == ESP_OK) {
        err = fe_mul(C, C, d2);
    }
    if (err == ESP_OK) {
        err = fe_mul(D, &p->Z, &q->Z);
    }
    if (err == ESP_OK) {
        err = fe_dbl(D, D);
    }
    if (err == ESP_OK) {
        err = fe_sub(E, B, A);
    }
    if (err == ESP_OK) {
        err = fe_sub(F, D, C);
    }
    if (err == ESP_OK) {
        err = fe_add(G, D, C);
    }
    if (err == ESP_OK) {
        err = fe_add(H, B, A);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->X, E, F);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->Y, G, H);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->T, E, H);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->Z, F, G);
    }

    pool_free(&pool);
    return err;
}

/* 点倍（dbl-2008-hwcd） */
static esp_err_t ge_double(ge_t *r, const ge_t *p)
{
    fe_pool_t pool;
    pool_init(&pool);

    mbedtls_mpi *A = &pool.v[0], *B = &pool.v[1], *C = &pool.v[2], *D = &pool.v[3];
    mbedtls_mpi *E = &pool.v[4], *F = &pool.v[5], *G = &pool.v[6], *H = &pool.v[7];
    mbedtls_mpi *t0 = &pool.v[8];

    esp_err_t err = fe_sqr(A, &p->X);
    if (err == ESP_OK) {
        err = fe_sqr(B, &p->Y);
    }
    if (err == ESP_OK) {
        err = fe_sqr(C, &p->Z);
    }
    if (err == ESP_OK) {
        err = fe_dbl(C, C);
    }
    if (err == ESP_OK) {
        err = fe_neg(D, A);
    }
    if (err == ESP_OK) {
        err = fe_add(t0, &p->X, &p->Y);
    }
    if (err == ESP_OK) {
        err = fe_sqr(E, t0);
    }
    if (err == ESP_OK) {
        err = fe_sub(E, E, A);
    }
    if (err == ESP_OK) {
        err = fe_sub(E, E, B);
    }
    if (err == ESP_OK) {
        err = fe_add(G, D, B);
    }
    if (err == ESP_OK) {
        err = fe_sub(F, G, C);
    }
    if (err == ESP_OK) {
        err = fe_sub(H, D, B);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->X, E, F);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->Y, G, H);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->T, E, H);
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->Z, F, G);
    }

    pool_free(&pool);
    return err;
}

/* r = -p（扩射坐标取负：只动 X 与 T） */
static esp_err_t ge_neg(ge_t *r, const ge_t *p)
{
    esp_err_t err = fe_neg(&r->X, &p->X);
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->Y, &p->Y));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->Z, &p->Z));
    }
    if (err == ESP_OK) {
        err = fe_neg(&r->T, &p->T);
    }
    return err;
}

/*
 * r = [scalar] * p。r 需要调用方先 ge_init。
 * 朴素 double-and-add：不做常数时间、不做预计算表 —— 这里一次签名为终点，够用。
 */
static esp_err_t ge_scalar_mul(ge_t *r, const ge_t *p, const uint8_t scalar[32])
{
    ge_t acc;
    ge_init(&acc);

    esp_err_t err = ge_identity(&acc);
    for (int bit = 255; bit >= 0 && err == ESP_OK; bit--) {
        err = ge_double(&acc, &acc);
        if (err == ESP_OK && ((scalar[bit >> 3] >> (bit & 7)) & 1) != 0) {
            err = ge_add(&acc, &acc, p);
        }
    }

    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->X, &acc.X));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->Y, &acc.Y));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->Z, &acc.Z));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->T, &acc.T));
    }

    ge_free(&acc);
    return err;
}

/** 压缩编码：32 字节小端 y，最高位放 x 的最低位 */
static esp_err_t ge_encode(const ge_t *p, uint8_t out[32])
{
    mbedtls_mpi zi, x, y;
    mbedtls_mpi_init(&zi);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&y);

    esp_err_t err = fe_inv(&zi, &p->Z);
    if (err == ESP_OK) {
        err = fe_mul(&x, &p->X, &zi);
    }
    if (err == ESP_OK) {
        err = fe_mul(&y, &p->Y, &zi);
    }
    if (err == ESP_OK) {
        err = fe_store(&y, out);
    }
    if (err == ESP_OK) {
        out[31] |= (uint8_t)((mbedtls_mpi_get_bit(&x, 0) & 1U) << 7);
    }

    mbedtls_mpi_free(&zi);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&y);
    return err;
}

/* 解压缩（RFC 8032 §5.1.3）：r 需要调用方先 ge_init */
static esp_err_t ge_decode(ge_t *r, const uint8_t in[32])
{
    fe_pool_t pool;
    pool_init(&pool);

    mbedtls_mpi *y = &pool.v[0], *y2 = &pool.v[1], *u = &pool.v[2], *v = &pool.v[3];
    mbedtls_mpi *v3 = &pool.v[4], *v7 = &pool.v[5], *t = &pool.v[6], *x = &pool.v[7];
    mbedtls_mpi *w = &pool.v[8], *p = &pool.v[9], *d = &pool.v[10], *one = &pool.v[11];
    mbedtls_mpi *sqrtm1 = &pool.v[12];

    uint8_t ybuf[32];
    memcpy(ybuf, in, sizeof(ybuf));
    int sign = (ybuf[31] >> 7) & 1;
    ybuf[31] &= 0x7f;

    esp_err_t err = fe_load(p, kP, sizeof(kP));
    if (err == ESP_OK) {
        err = fe_load(d, kD, sizeof(kD));
    }
    if (err == ESP_OK) {
        err = fe_load(sqrtm1, kSqrtM1, sizeof(kSqrtM1));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(one, 1));
    }
    if (err == ESP_OK) {
        err = fe_load(y, ybuf, sizeof(ybuf));
    }
    if (err == ESP_OK && mbedtls_mpi_cmp_mpi(y, p) >= 0) {
        err = ESP_FAIL; /* y 必须 < p */
    }

    /* u = y^2 - 1，v = d*y^2 + 1 */
    if (err == ESP_OK) {
        err = fe_sqr(y2, y);
    }
    if (err == ESP_OK) {
        err = fe_sub(u, y2, one);
    }
    if (err == ESP_OK) {
        err = fe_mul(v, d, y2);
    }
    if (err == ESP_OK) {
        err = fe_add(v, v, one);
    }

    /* x = u*v^3*(u*v^7)^((p-5)/8) */
    if (err == ESP_OK) {
        err = fe_sqr(v3, v);
    }
    if (err == ESP_OK) {
        err = fe_mul(v3, v3, v);
    }
    if (err == ESP_OK) {
        err = fe_sqr(v7, v3);
    }
    if (err == ESP_OK) {
        err = fe_mul(v7, v7, v);
    }
    if (err == ESP_OK) {
        err = fe_mul(t, u, v7);
    }
    if (err == ESP_OK) {
        err = fe_pow(t, t, kExpSqrt);
    }
    if (err == ESP_OK) {
        err = fe_mul(x, u, v3);
    }
    if (err == ESP_OK) {
        err = fe_mul(x, x, t);
    }

    /* 校验 v*x^2 == u；不等就乘一次 sqrt(-1) 再校验 */
    if (err == ESP_OK) {
        err = fe_sqr(w, x);
    }
    if (err == ESP_OK) {
        err = fe_mul(w, w, v);
    }
    if (err == ESP_OK && mbedtls_mpi_cmp_mpi(w, u) != 0) {
        if ((err = fe_mul(x, x, sqrtm1)) == ESP_OK) {
            err = fe_sqr(w, x);
        }
        if (err == ESP_OK) {
            err = fe_mul(w, w, v);
        }
        if (err == ESP_OK && mbedtls_mpi_cmp_mpi(w, u) != 0) {
            err = ESP_FAIL; /* u/v 不是二次剩余，这个编码不是合法点 */
        }
    }

    /* RFC 8032：x == 0 且 sign == 1 属于非法编码 */
    if (err == ESP_OK && mbedtls_mpi_cmp_int(x, 0) == 0 && sign != 0) {
        err = ESP_FAIL;
    }
    /* 按 sign 位取 x 或 p-x */
    if (err == ESP_OK && (mbedtls_mpi_get_bit(x, 0) & 1) != sign) {
        err = fe_neg(x, x);
    }

    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->X, x));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_copy(&r->Y, y));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_lset(&r->Z, 1));
    }
    if (err == ESP_OK) {
        err = fe_mul(&r->T, &r->X, &r->Y);
    }

    pool_free(&pool);
    return err;
}

/* ───────────────────────── SHA-512 ───────────────────────── */

static esp_err_t sha512_parts(const uint8_t *p0, size_t n0, const uint8_t *p1, size_t n1, const uint8_t *p2,
                              size_t n2, uint8_t out[64])
{
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);

    esp_err_t err = mpi_result(mbedtls_sha512_starts(&ctx, 0));
    if (err == ESP_OK && p0 != NULL && n0 > 0) {
        err = mpi_result(mbedtls_sha512_update(&ctx, p0, n0));
    }
    if (err == ESP_OK && p1 != NULL && n1 > 0) {
        err = mpi_result(mbedtls_sha512_update(&ctx, p1, n1));
    }
    if (err == ESP_OK && p2 != NULL && n2 > 0) {
        err = mpi_result(mbedtls_sha512_update(&ctx, p2, n2));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_sha512_finish(&ctx, out));
    }

    mbedtls_sha512_free(&ctx);
    return err;
}

/* ───────────────────────── 标量运算（mod L） ───────────────────────── */

static esp_err_t sc_reduce64(const uint8_t in[64], uint8_t out[32])
{
    mbedtls_mpi a, l, r;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&l);
    mbedtls_mpi_init(&r);

    esp_err_t err = fe_load(&a, in, 64);
    if (err == ESP_OK) {
        err = fe_load(&l, kL, sizeof(kL));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_mod_mpi(&r, &a, &l));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_write_binary_le(&r, out, 32));
    }

    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&l);
    mbedtls_mpi_free(&r);
    return err;
}

/** out = (r + k*a) mod L */
static esp_err_t sc_muladd(uint8_t out[32], const uint8_t k[32], const uint8_t a[32], const uint8_t r[32])
{
    mbedtls_mpi mk, ma, mr, l, t, u;
    mbedtls_mpi_init(&mk);
    mbedtls_mpi_init(&ma);
    mbedtls_mpi_init(&mr);
    mbedtls_mpi_init(&l);
    mbedtls_mpi_init(&t);
    mbedtls_mpi_init(&u);

    esp_err_t err = fe_load(&mk, k, 32);
    if (err == ESP_OK) {
        err = fe_load(&ma, a, 32);
    }
    if (err == ESP_OK) {
        err = fe_load(&mr, r, 32);
    }
    if (err == ESP_OK) {
        err = fe_load(&l, kL, sizeof(kL));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_mul_mpi(&t, &mk, &ma));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_add_mpi(&u, &t, &mr));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_mod_mpi(&t, &u, &l));
    }
    if (err == ESP_OK) {
        err = mpi_result(mbedtls_mpi_write_binary_le(&t, out, 32));
    }

    mbedtls_mpi_free(&mk);
    mbedtls_mpi_free(&ma);
    mbedtls_mpi_free(&mr);
    mbedtls_mpi_free(&l);
    mbedtls_mpi_free(&t);
    mbedtls_mpi_free(&u);
    return err;
}

/** S 必须 < L（否则签名不合法） */
static bool sc_is_canonical(const uint8_t s[32])
{
    mbedtls_mpi ms, l;
    mbedtls_mpi_init(&ms);
    mbedtls_mpi_init(&l);

    bool ok = false;
    if (fe_load(&ms, s, 32) == ESP_OK && fe_load(&l, kL, sizeof(kL)) == ESP_OK) {
        ok = mbedtls_mpi_cmp_mpi(&ms, &l) < 0;
    }

    mbedtls_mpi_free(&ms);
    mbedtls_mpi_free(&l);
    return ok;
}

/* ───────────────────────── 对外 API ───────────────────────── */

/** h = SHA-512(seed)；前半 clamp 成标量 a，后半是确定性随机数用的 prefix */
static esp_err_t expand_seed(const uint8_t seed[32], uint8_t a[32], uint8_t prefix[32])
{
    uint8_t h[64];
    esp_err_t err = sha512_parts(seed, 32, NULL, 0, NULL, 0, h);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(a, h, 32);
    memcpy(prefix, h + 32, 32);

    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;

    memset(h, 0, sizeof(h));
    return ESP_OK;
}

esp_err_t ed25519_public_key(const uint8_t seed[ED25519_SEED_LEN], uint8_t pub[ED25519_PUBKEY_LEN])
{
    if (seed == NULL || pub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t a[32], prefix[32];
    esp_err_t err = expand_seed(seed, a, prefix);

    ge_t base, A;
    ge_init(&base);
    ge_init(&A);
    if (err == ESP_OK) {
        err = ge_from_affine(&base, kBaseX, kBaseY);
    }
    if (err == ESP_OK) {
        err = ge_scalar_mul(&A, &base, a);
    }
    if (err == ESP_OK) {
        err = ge_encode(&A, pub);
    }

    ge_free(&A);
    ge_free(&base);
    memset(a, 0, sizeof(a));
    memset(prefix, 0, sizeof(prefix));
    return err;
}

esp_err_t ed25519_sign(const uint8_t seed[ED25519_SEED_LEN], const uint8_t *msg, size_t msg_len,
                       uint8_t sig[ED25519_SIGN_LEN])
{
    if (seed == NULL || sig == NULL || (msg == NULL && msg_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t a[32], prefix[32], pub[32];
    uint8_t hram[64], r[32], k[32];
    esp_err_t err = expand_seed(seed, a, prefix);

    if (err == ESP_OK) {
        err = ed25519_public_key(seed, pub);
    }

    /* r = SHA-512(prefix || M) mod L，R = [r]B */
    ge_t base, R;
    ge_init(&base);
    ge_init(&R);
    if (err == ESP_OK) {
        err = sha512_parts(prefix, 32, msg, msg_len, NULL, 0, hram);
    }
    if (err == ESP_OK) {
        err = sc_reduce64(hram, r);
    }
    if (err == ESP_OK) {
        err = ge_from_affine(&base, kBaseX, kBaseY);
    }
    if (err == ESP_OK) {
        err = ge_scalar_mul(&R, &base, r);
    }
    if (err == ESP_OK) {
        err = ge_encode(&R, sig); /* 前 32 字节 = R */
    }

    /* k = SHA-512(R || A || M) mod L，S = (r + k*a) mod L */
    if (err == ESP_OK) {
        err = sha512_parts(sig, 32, pub, 32, msg, msg_len, hram);
    }
    if (err == ESP_OK) {
        err = sc_reduce64(hram, k);
    }
    if (err == ESP_OK) {
        err = sc_muladd(sig + 32, k, a, r);
    }

    ge_free(&R);
    ge_free(&base);
    memset(a, 0, sizeof(a));
    memset(prefix, 0, sizeof(prefix));
    memset(hram, 0, sizeof(hram));
    memset(r, 0, sizeof(r));
    memset(k, 0, sizeof(k));
    return err;
}

esp_err_t ed25519_verify(const uint8_t pub[ED25519_PUBKEY_LEN], const uint8_t *msg, size_t msg_len,
                         const uint8_t sig[ED25519_SIGN_LEN])
{
    if (pub == NULL || sig == NULL || (msg == NULL && msg_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sc_is_canonical(sig + 32)) {
        return ESP_FAIL;
    }

    /*
     * 校验 [S]B == R + [k]A，等价于 encode([S]B - [k]A) == R。
     * 这里把 [k]A 就地取负，再与 [S]B 相加，省一路减法。
     */
    ge_t A, R, base, SB, kA, sum;
    ge_init(&A);
    ge_init(&R);
    ge_init(&base);
    ge_init(&SB);
    ge_init(&kA);
    ge_init(&sum);

    uint8_t hram[64], k[32], got[32];
    esp_err_t err = ge_decode(&A, pub);
    if (err == ESP_OK) {
        err = ge_decode(&R, sig); /* R 也要是合法的曲线点 */
    }
    if (err == ESP_OK) {
        err = sha512_parts(sig, 32, pub, 32, msg, msg_len, hram);
    }
    if (err == ESP_OK) {
        err = sc_reduce64(hram, k);
    }
    if (err == ESP_OK) {
        err = ge_from_affine(&base, kBaseX, kBaseY);
    }
    if (err == ESP_OK) {
        err = ge_scalar_mul(&SB, &base, sig + 32); /* [S]B */
    }
    if (err == ESP_OK) {
        err = ge_scalar_mul(&kA, &A, k); /* [k]A */
    }
    if (err == ESP_OK) {
        err = ge_neg(&kA, &kA);
    }
    if (err == ESP_OK) {
        err = ge_add(&sum, &SB, &kA);
    }
    if (err == ESP_OK) {
        err = ge_encode(&sum, got);
    }
    if (err == ESP_OK && memcmp(got, sig, 32) != 0) {
        err = ESP_FAIL;
    }

    memset(hram, 0, sizeof(hram));
    memset(k, 0, sizeof(k));

    ge_free(&A);
    ge_free(&R);
    ge_free(&base);
    ge_free(&SB);
    ge_free(&kA);
    ge_free(&sum);
    return err;
}

/* ───────────────────────── 自检（RFC 8032 §7.1） ───────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_FAIL;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

typedef struct {
    const char *seed;
    const char *pub;
    const char *msg; /* 可以为 "" （空消息） */
    const char *sig;
} rfc8032_vector_t;

static const rfc8032_vector_t s_vectors[] = {
    {
        /* TEST 1：空消息 */
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc6"
        "1e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    },
    {
        /* TEST 2：单字节消息 0x72 */
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e"
        "458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    },
};

esp_err_t ed25519_selftest(void)
{
    for (size_t i = 0; i < sizeof(s_vectors) / sizeof(s_vectors[0]); i++) {
        const rfc8032_vector_t *v = &s_vectors[i];

        uint8_t seed[32], want_pub[32], msg[1], want_sig[64];
        size_t msg_len = strlen(v->msg) / 2;

        esp_err_t err = hex_decode(v->seed, seed, sizeof(seed));
        if (err == ESP_OK) {
            err = hex_decode(v->pub, want_pub, sizeof(want_pub));
        }
        if (err == ESP_OK) {
            err = hex_decode(v->sig, want_sig, sizeof(want_sig));
        }
        if (err == ESP_OK && msg_len > 0) {
            err = hex_decode(v->msg, msg, msg_len);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "自检向量 %u 解码失败", (unsigned)i);
            return ESP_FAIL;
        }

        uint8_t pub[32];
        err = ed25519_public_key(seed, pub);
        if (err != ESP_OK || memcmp(pub, want_pub, 32) != 0) {
            ESP_LOGE(TAG, "自检向量 %u 公钥不符（实现或向量有误，请核对 RFC 8032 §7.1）", (unsigned)i);
            return ESP_FAIL;
        }

        uint8_t sig[64];
        err = ed25519_sign(seed, msg_len > 0 ? msg : NULL, msg_len, sig);
        if (err != ESP_OK || memcmp(sig, want_sig, 64) != 0) {
            ESP_LOGE(TAG, "自检向量 %u 签名不符（实现或向量有误，请核对 RFC 8032 §7.1）", (unsigned)i);
            return ESP_FAIL;
        }

        if (ed25519_verify(want_pub, msg_len > 0 ? msg : NULL, msg_len, want_sig) != ESP_OK) {
            ESP_LOGE(TAG, "自检向量 %u 验签失败", (unsigned)i);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Ed25519 自检通过（RFC 8032 §7.1 两组向量）");
    return ESP_OK;
}
