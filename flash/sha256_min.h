/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 diskOS contributors */
/* sha256_min.h - freestanding SHA-256 (FIPS 180-4) for the DRAM NAND tools: no libc, no heap, streaming.
 * Used to hash NAND partitions on the device (fed one 2 KiB page at a time). Checked against Python's
 * hashlib by tests/release (host build of this same header). */
#ifndef SHA256_MIN_H
#define SHA256_MIN_H
typedef struct { unsigned int h[8]; unsigned int nbuf; unsigned long long nbytes; unsigned char buf[64]; } sha256_ctx;

static const unsigned int SHA256_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };

#define SHA_ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(sha256_ctx *c, const unsigned char *p){
    unsigned int w[64], a, b, d, e, f, g, h, cc, t1, t2; int i;
    for(i = 0; i < 16; i++) w[i] = ((unsigned int)p[4*i] << 24) | ((unsigned int)p[4*i+1] << 16) | ((unsigned int)p[4*i+2] << 8) | p[4*i+3];
    for(i = 16; i < 64; i++){
        unsigned int s0 = SHA_ROR(w[i-15], 7) ^ SHA_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = SHA_ROR(w[i-2], 17) ^ SHA_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for(i = 0; i < 64; i++){
        t1 = h + (SHA_ROR(e, 6) ^ SHA_ROR(e, 11) ^ SHA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i];
        t2 = (SHA_ROR(a, 2) ^ SHA_ROR(a, 13) ^ SHA_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha256_init(sha256_ctx *c){
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu; c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->nbuf = 0; c->nbytes = 0;
}
static void sha256_update(sha256_ctx *c, const unsigned char *p, unsigned int n){
    c->nbytes += n;
    while(n){
        if(c->nbuf == 0 && n >= 64){ sha256_block(c, p); p += 64; n -= 64; continue; }
        c->buf[c->nbuf++] = *p++; n--;
        if(c->nbuf == 64){ sha256_block(c, c->buf); c->nbuf = 0; }
    }
}
/* out: the digest as 8 big-endian words (word i = bytes 4i..4i+3 of the standard digest) */
static void sha256_final(sha256_ctx *c, unsigned int out[8]){
    unsigned long long bits = c->nbytes * 8u; int i;
    unsigned char pad = 0x80, z = 0;
    sha256_update(c, &pad, 1);
    while(c->nbuf != 56) sha256_update(c, &z, 1);
    for(i = 7; i >= 0; i--){ unsigned char b = (unsigned char)(bits >> (8 * i)); sha256_update(c, &b, 1); }
    for(i = 0; i < 8; i++) out[i] = c->h[i];
}
#endif
