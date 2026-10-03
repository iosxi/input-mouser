/* ==================================================================
 * crypto.c - 鍵の生成と暗号化(Windows の CNG = bcrypt.dll)
 *
 *  パスワード → PBKDF2-HMAC-SHA256(20 万回)で 32 バイトの鍵にする。
 *  ini にはこの鍵だけを書き、パスワードそのものは残さない。
 *
 *  接続ごとに双方が乱数(nonce)を出し合い、
 *      確認   HMAC(鍵, "S" | 乱数M | 乱数S)  など
 *      通信鍵 HMAC(鍵, "K" | 乱数M | 乱数S)
 *  とする。通信は AES-256-GCM。GCM の nonce は送る向き 1 バイトと
 *  通し番号 8 バイトで作り、送らない(双方が数えている)。
 *  番号が食い違えば復号に失敗するので、差し込みや再送は通らない。
 * ================================================================== */

#include "mouser.h"
#include <bcrypt.h>

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

static BCRYPT_ALG_HANDLE g_aes, g_hmac;

static const BYTE k_salt[] = "input-mouser/key/v1";
#define PBKDF2_ROUNDS 200000

BOOL crypto_init(void)
{
    if (g_aes) return TRUE;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g_hmac, BCRYPT_SHA256_ALGORITHM, NULL,
                                                    BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g_aes, BCRYPT_AES_ALGORITHM, NULL, 0)))
        return FALSE;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(g_aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
        BCryptCloseAlgorithmProvider(g_aes, 0);
        g_aes = NULL;
        return FALSE;
    }
    return TRUE;
}

void crypto_random(void *buf, ULONG len)
{
    BCryptGenRandom(NULL, (PUCHAR)buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

BOOL crypto_derive(const WCHAR *password, BYTE out[32])
{
    char pw[1024];
    int  n = WideCharToMultiByte(CP_UTF8, 0, password, -1, pw, sizeof(pw), NULL, NULL);
    BOOL ok;

    if (n <= 0) return FALSE;
    ok = BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(g_hmac, (PUCHAR)pw, (ULONG)(n - 1),
                                             (PUCHAR)k_salt, sizeof(k_salt) - 1,
                                             PBKDF2_ROUNDS, out, 32, 0));
    SecureZeroMemory(pw, sizeof(pw));
    return ok;
}

void crypto_hmac(const BYTE key[32], const void *a, int alen, const void *b, int blen,
                 const void *c, int clen, BYTE out[32])
{
    BCRYPT_HASH_HANDLE h = NULL;

    ZeroMemory(out, 32);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(g_hmac, &h, NULL, 0, (PUCHAR)key, 32, 0))) return;
    if (alen) BCryptHashData(h, (PUCHAR)a, (ULONG)alen, 0);
    if (blen) BCryptHashData(h, (PUCHAR)b, (ULONG)blen, 0);
    if (clen) BCryptHashData(h, (PUCHAR)c, (ULONG)clen, 0);
    BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h);
}

/* ------------------------------------------------------------------ */
/*  AES-256-GCM                                                         */
/* ------------------------------------------------------------------ */

BOOL gcm_init(Gcm *g, const BYTE key[32], BYTE dir)
{
    BCRYPT_KEY_HANDLE k = NULL;

    g->key = NULL;
    g->dir = dir;
    g->ctr = 0;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(g_aes, &k, NULL, 0, (PUCHAR)key, 32, 0)))
        return FALSE;
    g->key = k;
    return TRUE;
}

void gcm_free(Gcm *g)
{
    if (g->key) BCryptDestroyKey((BCRYPT_KEY_HANDLE)g->key);
    g->key = NULL;
}

static void make_nonce(BYTE nonce[12], BYTE dir, UINT64 ctr)
{
    int i;
    nonce[0] = dir;
    nonce[1] = nonce[2] = nonce[3] = 0;
    for (i = 0; i < 8; i++) nonce[4 + i] = (BYTE)(ctr >> (8 * i));
}

BOOL gcm_seal(Gcm *g, const BYTE *in, ULONG len, BYTE *out)
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BYTE  nonce[12];
    ULONG got = 0;

    if (!g->key) return FALSE;
    make_nonce(nonce, g->dir, g->ctr++);
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = nonce;
    ai.cbNonce = sizeof(nonce);
    ai.pbTag   = out + len;
    ai.cbTag   = 16;
    return BCRYPT_SUCCESS(BCryptEncrypt((BCRYPT_KEY_HANDLE)g->key, (PUCHAR)in, len, &ai, NULL, 0,
                                        out, len, &got, 0)) && got == len;
}

/* g->ctr は受けた数を数える。dir は相手の送る向き */
BOOL gcm_open(Gcm *g, BYTE dir, const BYTE *in, ULONG len, BYTE *out)
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BYTE  nonce[12];
    ULONG got = 0, n;

    if (!g->key || len < 16) return FALSE;
    n = len - 16;
    make_nonce(nonce, dir, g->ctr++);
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = nonce;
    ai.cbNonce = sizeof(nonce);
    ai.pbTag   = (PUCHAR)in + n;
    ai.cbTag   = 16;
    return BCRYPT_SUCCESS(BCryptDecrypt((BCRYPT_KEY_HANDLE)g->key, (PUCHAR)in, n, &ai, NULL, 0,
                                        out, n, &got, 0)) && got == n;
}
