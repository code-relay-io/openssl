/*
 * Copyright 2019 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "cipher_aes.h"
#include "internal/providercommonerr.h"
#include "internal/provider_algs.h"

/* AES wrap with padding has IV length of 4, without padding 8 */
#define AES_WRAP_PAD_IVLEN   4
#define AES_WRAP_NOPAD_IVLEN 8

/* TODO(3.0) Figure out what flags need to be passed */
#define WRAP_FLAGS (EVP_CIPH_WRAP_MODE \
                   | EVP_CIPH_CUSTOM_IV | EVP_CIPH_FLAG_CUSTOM_CIPHER \
                   | EVP_CIPH_ALWAYS_CALL_INIT)

typedef size_t (*aeswrap_fn)(void *key, const unsigned char *iv,
                             unsigned char *out, const unsigned char *in,
                             size_t inlen, block128_f block);

static OSSL_OP_cipher_encrypt_init_fn aes_wrap_einit;
static OSSL_OP_cipher_decrypt_init_fn aes_wrap_dinit;
static OSSL_OP_cipher_update_fn aes_wrap_cipher;
static OSSL_OP_cipher_final_fn aes_wrap_final;
static OSSL_OP_cipher_freectx_fn aes_wrap_freectx;

typedef struct prov_aes_wrap_ctx_st {
    PROV_CIPHER_CTX base;
    union {
        OSSL_UNION_ALIGN;
        AES_KEY ks;
    } ks;
    unsigned int iv_set : 1;
    aeswrap_fn wrapfn;

} PROV_AES_WRAP_CTX;


static void *aes_wrap_newctx(size_t kbits, size_t blkbits,
                             size_t ivbits, unsigned int mode, uint64_t flags)
{
    PROV_AES_WRAP_CTX *wctx = OPENSSL_zalloc(sizeof(*wctx));
    PROV_CIPHER_CTX *ctx = (PROV_CIPHER_CTX *)wctx;

    if (ctx != NULL) {
        cipher_generic_initkey(ctx, kbits, blkbits, ivbits, mode, flags,
                               NULL, NULL);
        ctx->pad = (ctx->ivlen == AES_WRAP_PAD_IVLEN);
    }
    return wctx;
}

static void aes_wrap_freectx(void *vctx)
{
    PROV_AES_WRAP_CTX *wctx = (PROV_AES_WRAP_CTX *)vctx;
    PROV_CIPHER_CTX *ctx = (PROV_CIPHER_CTX *)vctx;

    OPENSSL_cleanse(ctx->iv, sizeof(ctx->iv));
    OPENSSL_clear_free(wctx,  sizeof(*wctx));
}

static int aes_wrap_init(void *vctx, const unsigned char *key,
                         size_t keylen, const unsigned char *iv,
                         size_t ivlen, int enc)
{
    PROV_CIPHER_CTX *ctx = (PROV_CIPHER_CTX *)vctx;
    PROV_AES_WRAP_CTX *wctx = (PROV_AES_WRAP_CTX *)vctx;

    ctx->enc = enc;
    ctx->block = enc ? (block128_f)AES_encrypt : (block128_f)AES_decrypt;
    if (ctx->pad)
        wctx->wrapfn = enc ? CRYPTO_128_wrap_pad : CRYPTO_128_unwrap_pad;
    else
        wctx->wrapfn = enc ? CRYPTO_128_wrap : CRYPTO_128_unwrap;

    if (iv != NULL) {
        ctx->ivlen = ivlen;
        memcpy(ctx->iv, iv, ivlen);
        wctx->iv_set = 1;
    }
    if (key != NULL) {
        if (keylen != ctx->keylen) {
           ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY_LENGTH);
           return 0;
        }
        if (ctx->enc)
            AES_set_encrypt_key(key, keylen * 8, &wctx->ks.ks);
        else
            AES_set_decrypt_key(key, keylen * 8, &wctx->ks.ks);
    }
    return 1;
}

static int aes_wrap_einit(void *ctx, const unsigned char *key, size_t keylen,
                          const unsigned char *iv, size_t ivlen)
{
    return aes_wrap_init(ctx, key, keylen, iv, ivlen, 1);
}

static int aes_wrap_dinit(void *ctx, const unsigned char *key, size_t keylen,
                          const unsigned char *iv, size_t ivlen)
{
    return aes_wrap_init(ctx, key, keylen, iv, ivlen, 0);
}

static int aes_wrap_cipher_internal(void *vctx, unsigned char *out,
                                    const unsigned char *in, size_t inlen)
{
    PROV_CIPHER_CTX *ctx = (PROV_CIPHER_CTX *)vctx;
    PROV_AES_WRAP_CTX *wctx = (PROV_AES_WRAP_CTX *)vctx;
    size_t rv;
    int pad = ctx->pad;

    /* No final operation so always return zero length */
    if (in == NULL)
        return 0;

    /* Input length must always be non-zero */
    if (inlen == 0)
        return -1;

    /* If decrypting need at least 16 bytes and multiple of 8 */
    if (!ctx->enc && (inlen < 16 || inlen & 0x7))
        return -1;

    /* If not padding input must be multiple of 8 */
    if (!pad && inlen & 0x7)
        return -1;

    if (out == NULL) {
        if (ctx->enc) {
            /* If padding round up to multiple of 8 */
            if (pad)
                inlen = (inlen + 7) / 8 * 8;
            /* 8 byte prefix */
            return inlen + 8;
        } else {
            /*
             * If not padding output will be exactly 8 bytes smaller than
             * input. If padding it will be at least 8 bytes smaller but we
             * don't know how much.
             */
            return inlen - 8;
        }
    }

    rv = wctx->wrapfn(&wctx->ks.ks, wctx->iv_set ? ctx->iv : NULL, out, in,
                      inlen, ctx->block);
    return rv ? (int)rv : -1;
}

static int aes_wrap_final(void *vctx, unsigned char *out, size_t *outl,
                          size_t outsize)
{
    *outl = 0;
    return 1;
}

static int aes_wrap_cipher(void *vctx,
                           unsigned char *out, size_t *outl, size_t outsize,
                           const unsigned char *in, size_t inl)
{
    PROV_AES_WRAP_CTX *ctx = (PROV_AES_WRAP_CTX *)vctx;
    size_t len;

    if (outsize < inl) {
        ERR_raise(ERR_LIB_PROV, PROV_R_OUTPUT_BUFFER_TOO_SMALL);
        return -1;
    }

    len = aes_wrap_cipher_internal(ctx, out, in, inl);
    if (len == 0)
        return -1;

    *outl = len;
    return 1;
}

static int aes_wrap_set_ctx_params(void *vctx, const OSSL_PARAM params[])
{
    PROV_CIPHER_CTX *ctx = (PROV_CIPHER_CTX *)vctx;
    const OSSL_PARAM *p;
    size_t keylen = 0;

    p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_KEYLEN);
    if (p != NULL) {
        if (!OSSL_PARAM_get_size_t(p, &keylen)) {
            ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
            return 0;
        }
        if (ctx->keylen != keylen) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY_LENGTH);
            return 0;
        }
    }
    return 1;
}

#define IMPLEMENT_cipher(mode, fname, UCMODE, flags, kbits, blkbits, ivbits)   \
    static OSSL_OP_cipher_get_params_fn aes_##kbits##_##fname##_get_params;    \
    static int aes_##kbits##_##fname##_get_params(OSSL_PARAM params[])         \
    {                                                                          \
        return cipher_generic_get_params(params, EVP_CIPH_##UCMODE##_MODE,     \
                                         flags, kbits, blkbits, ivbits);       \
    }                                                                          \
    static OSSL_OP_cipher_newctx_fn aes_##kbits##fname##_newctx;               \
    static void *aes_##kbits##fname##_newctx(void *provctx)                    \
    {                                                                          \
        return aes_##mode##_newctx(kbits, blkbits, ivbits,                     \
                                   EVP_CIPH_##UCMODE##_MODE, flags);           \
    }                                                                          \
    const OSSL_DISPATCH aes##kbits##fname##_functions[] = {                    \
        { OSSL_FUNC_CIPHER_NEWCTX,                                             \
            (void (*)(void))aes_##kbits##fname##_newctx },                     \
        { OSSL_FUNC_CIPHER_ENCRYPT_INIT, (void (*)(void))aes_##mode##_einit }, \
        { OSSL_FUNC_CIPHER_DECRYPT_INIT, (void (*)(void))aes_##mode##_dinit }, \
        { OSSL_FUNC_CIPHER_UPDATE, (void (*)(void))aes_##mode##_cipher },      \
        { OSSL_FUNC_CIPHER_FINAL, (void (*)(void))aes_##mode##_final },        \
        { OSSL_FUNC_CIPHER_FREECTX, (void (*)(void))aes_##mode##_freectx },    \
        { OSSL_FUNC_CIPHER_GET_PARAMS,                                         \
            (void (*)(void))aes_##kbits##_##fname##_get_params },              \
        { OSSL_FUNC_CIPHER_GETTABLE_PARAMS,                                    \
            (void (*)(void))cipher_generic_gettable_params },                  \
        { OSSL_FUNC_CIPHER_GET_CTX_PARAMS,                                     \
            (void (*)(void))cipher_generic_get_ctx_params },                   \
        { OSSL_FUNC_CIPHER_SET_CTX_PARAMS,                                     \
            (void (*)(void))aes_wrap_set_ctx_params },                         \
        { OSSL_FUNC_CIPHER_GETTABLE_CTX_PARAMS,                                \
            (void (*)(void))cipher_generic_gettable_ctx_params },              \
        { OSSL_FUNC_CIPHER_SETTABLE_CTX_PARAMS,                                \
            (void (*)(void))cipher_generic_settable_ctx_params },              \
        { 0, NULL }                                                            \
    }

IMPLEMENT_cipher(wrap, wrap, WRAP, WRAP_FLAGS, 256, 64, AES_WRAP_NOPAD_IVLEN * 8);
IMPLEMENT_cipher(wrap, wrap, WRAP, WRAP_FLAGS, 192, 64, AES_WRAP_NOPAD_IVLEN * 8);
IMPLEMENT_cipher(wrap, wrap, WRAP, WRAP_FLAGS, 128, 64, AES_WRAP_NOPAD_IVLEN * 8);
IMPLEMENT_cipher(wrap, wrappad, WRAP, WRAP_FLAGS, 256, 64, AES_WRAP_PAD_IVLEN * 8);
IMPLEMENT_cipher(wrap, wrappad, WRAP, WRAP_FLAGS, 192, 64, AES_WRAP_PAD_IVLEN * 8);
IMPLEMENT_cipher(wrap, wrappad, WRAP, WRAP_FLAGS, 128, 64, AES_WRAP_PAD_IVLEN * 8);