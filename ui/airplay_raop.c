/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent RAOP envelope and packet cipher. Public RSA modulus, a protocol
 * constant, corroborated by PipeWire module-raop-sink.c (MIT), commit
 * 6c3fd1f07fae809fa52f640cf8e0bd643eb0838d. No upstream implementation copied. */
#include "airplay_raop.h"
#include "airplay_crypto.h"
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/rsa.h>
#include <string.h>
static const char modulus[] =
    "59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
    "5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
    "KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
    "OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
    "Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
    "imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
static int random_bytes(void *ctx, unsigned char *out, size_t n){ (void)ctx; return aps_random(out, n); }
static int rsa_key(mbedtls_rsa_context *c){
    uint8_t n[256], exponent[3] = { 1, 0, 1 }; size_t len = 0;
    mbedtls_rsa_init(c);
    if(mbedtls_base64_decode(n, sizeof n, &len, (const uint8_t *)modulus, sizeof modulus - 1) || len != 256 ||
       mbedtls_rsa_import_raw(c, n, sizeof n, NULL, 0, NULL, 0, NULL, 0, exponent, sizeof exponent) ||
       mbedtls_rsa_complete(c) || mbedtls_rsa_check_pubkey(c)) return -1;
    return mbedtls_rsa_set_padding(c, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1) ? -1 : 0;
}
static int base64(const uint8_t *p, size_t n, char *out, size_t cap){
    size_t used = 0; if(mbedtls_base64_encode((uint8_t *)out, cap, &used, p, n)) return -1;
    while(used && out[used - 1] == '=') --used;
    out[used] = 0; return 0;
}
int aps_raop_create(aps_raop_key *k, char wrapped[345], char iv[25], char challenge[25]){
    mbedtls_rsa_context c; uint8_t encrypted[256]; int rc = -1;
    if(rsa_key(&c)) goto end;
    if(aps_random(k, sizeof *k) || mbedtls_rsa_rsaes_oaep_encrypt(&c, random_bytes, NULL, NULL, 0, 16, k->key, encrypted) ||
       base64(encrypted, sizeof encrypted, wrapped, 345) || base64(k->iv, 16, iv, 25) || base64(k->challenge, 16, challenge, 25)) goto end;
    rc = 0;
end:
    mbedtls_rsa_free(&c); aps_wipe(encrypted, sizeof encrypted);
    if(rc){ aps_wipe(k, sizeof *k); wrapped[0] = iv[0] = challenge[0] = 0; } return rc;
}
int aps_raop_check(const aps_raop_key *k, const char *response, const uint8_t ipv4[4]){
    mbedtls_rsa_context c; uint8_t signature[256], encoded[256]; char padded[345];
    size_t len = response ? strnlen(response, sizeof padded) : 0, got = 0; int rc = -1;
    mbedtls_rsa_init(&c);
    if(!len || len > 344) goto end;
    memcpy(padded, response, len);
    while(len % 4) padded[len++] = '=';
    if(mbedtls_base64_decode(signature, sizeof signature, &got, (uint8_t *)padded, len) || got != 256 ||
       rsa_key(&c) || mbedtls_rsa_public(&c, signature, encoded)) goto end;
    if(encoded[0] != 0 || encoded[1] != 1) goto end;
    size_t at = 2; while(at < sizeof encoded && encoded[at] == 255) at++;
    if(at < 10 || at >= sizeof encoded || encoded[at++] != 0 || sizeof encoded - at < 20) goto end;
    if(!aps_equal(encoded + at, k->challenge, 16) || !aps_equal(encoded + at + 16, ipv4, 4)) goto end;
    rc = 0;
end:
    mbedtls_rsa_free(&c); aps_wipe(signature, sizeof signature); aps_wipe(encoded, sizeof encoded); return rc;
}
int aps_raop_encrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *p, size_t n, uint8_t *out){
    mbedtls_aes_context c; uint8_t copy[16]; size_t blocks = n & ~(size_t)15;
    mbedtls_aes_init(&c); memcpy(copy, iv, 16);
    int rc = mbedtls_aes_setkey_enc(&c, key, 128);
    if(!rc && blocks) rc = mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, blocks, copy, p, out);
    if(!rc && n > blocks) memcpy(out + blocks, p + blocks, n - blocks);
    mbedtls_aes_free(&c); aps_wipe(copy, sizeof copy);
    if(rc) aps_wipe(out, n);
    return rc ? -1 : 0;
}
