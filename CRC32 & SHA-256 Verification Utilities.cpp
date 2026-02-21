/* crypto_verify.c */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── CRC32 (IEEE 802.3 polynomial) ──────────────────────────────────── */
static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256U; i++) {
        uint32_t crc = i;
        for (uint8_t j = 0; j < 8U; j++) {
            crc = (crc & 1U) ? (0xEDB88320U ^ (crc >> 1)) : (crc >> 1);
        }
        crc32_table[i] = crc;
    }
    crc32_table_ready = true;
}

uint32_t crc32_calculate(const uint8_t *data, uint32_t len)
{
    if (!crc32_table_ready) crc32_init_table();

    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ── Minimal SHA-256 (FIPS 180-4) ───────────────────────────────────── */
/* In production use mbedTLS / wolfSSL / hardware crypto accelerator.    */

static const uint32_t K[64] = {
    0x428A2F98,0x71374491,0xB5C0FBCF,0xE9B5DBA5,
    0x3956C25B,0x59F111F1,0x923F82A4,0xAB1C5ED5,
    0xD807AA98,0x12835B01,0x243185BE,0x550C7DC3,
    0x72BE5D74,0x80DEB1FE,0x9BDC06A7,0xC19BF174,
    0xE49B69C1,0xEFBE4786,0x0FC19DC6,0x240CA1CC,
    0x2DE92C6F,0x4A7484AA,0x5CB0A9DC,0x76F988DA,
    0x983E5152,0xA831C66D,0xB00327C8,0xBF597FC7,
    0xC6E00BF3,0xD5A79147,0x06CA6351,0x14292967,
    0x27B70A85,0x2E1B2138,0x4D2C6DFC,0x53380D13,
    0x650A7354,0x766A0ABB,0x81C2C92E,0x92722C85,
    0xA2BFE8A1,0xA81A664B,0xC24B8B70,0xC76C51A3,
    0xD192E819,0xD6990624,0xF40E3585,0x106AA070,
    0x19A4C116,0x1E376C08,0x2748774C,0x34B0BCB5,
    0x391C0CB3,0x4ED8AA4A,0x5B9CCA4F,0x682E6FF3,
    0x748F82EE,0x78A5636F,0x84C87814,0x8CC70208,
    0x90BEFFFA,0xA4506CEB,0xBEF9A3F7,0xC67178F2
};

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)(((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x)(ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x)(ROTR(x,17)^ROTR(x,19)^((x)>>10))

typedef struct { uint8_t data[64]; uint32_t datalen;
                 uint64_t bitlen;  uint32_t state[8]; } SHA256_CTX;

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *data)
{
    uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64];
    uint32_t i,j;
    for (i=0,j=0;i<16;i++,j+=4)
        m[i]=((uint32_t)data[j]<<24)|((uint32_t)data[j+1]<<16)|
             ((uint32_t)data[j+2]<<8)|(uint32_t)data[j+3];
    for (;i<64;i++)
        m[i]=SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+K[i]+m[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

void sha256_compute(const uint8_t *data, uint32_t len, uint8_t *digest)
{
    SHA256_CTX ctx;
    ctx.datalen = 0; ctx.bitlen = 0;
    ctx.state[0]=0x6A09E667; ctx.state[1]=0xBB67AE85;
    ctx.state[2]=0x3C6EF372; ctx.state[3]=0xA54FF53A;
    ctx.state[4]=0x510E527F; ctx.state[5]=0x9B05688C;
    ctx.state[6]=0x1F83D9AB; ctx.state[7]=0x5BE0CD19;

    for (uint32_t i = 0; i < len; i++) {
        ctx.data[ctx.datalen++] = data[i];
        if (ctx.datalen == 64) {
            sha256_transform(&ctx, ctx.data);
            ctx.bitlen += 512; ctx.datalen = 0;
        }
    }
    /* Padding */
    uint32_t pad_idx = ctx.datalen;
    ctx.bitlen += ctx.datalen * 8ULL;
    ctx.data[pad_idx++] = 0x80;
    while (pad_idx < 56) ctx.data[pad_idx++] = 0x00;
    for (int k = 7; k >= 0; k--) ctx.data[pad_idx++]=(uint8_t)(ctx.bitlen>>(k*8));
    sha256_transform(&ctx, ctx.data);

    for (uint32_t i = 0; i < 4; i++)
        for (int k = 3; k >= 0; k--)
            for (int s = 0; s < 8; s++)
                digest[s*4+k] = (ctx.state[s] >> (k*8)) & 0xFF;
    /* Simplified: standard byte output */
    for (int s = 0; s < 8; s++) {
        digest[s*4+0] = (ctx.state[s] >> 24) & 0xFF;
        digest[s*4+1] = (ctx.state[s] >> 16) & 0xFF;
        digest[s*4+2] = (ctx.state[s] >>  8) & 0xFF;
        digest[s*4+3] = (ctx.state[s]      ) & 0xFF;
    }
}
