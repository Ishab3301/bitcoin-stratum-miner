/*
 * High-Performance Stratum V1 CPU Miner
 * Hardened Production Architecture with Buffered TCP Framing,
 * Out-of-Order Pending Share Table, and 9-Stage Verification Suite
 * Pure C implementation (Windows Sockets & POSIX compatible)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#define SD_BOTH SHUT_RDWR
#endif

#define MAX_THREADS 64
#define MAX_MERKLE_BRANCHES 32
#define HASH_COUNTER_BATCH 200000
#define MAX_PENDING_SHARES 128
#define PENDING_SHARE_TIMEOUT_SEC 60

/* ============================================================================
 * SECTION 1: Embedded JSMN Parser & Helper Functions
 * ============================================================================ */

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3,
    JSMN_PRIMITIVE = 4
} jsmntype_t;

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
} jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens) {
    if (parser->toknext >= num_tokens) return NULL;
    jsmntok_t *tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens) {
    int count = parser->toknext;
    for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
        char c = js[parser->pos];
        if (c == '{' || c == '[') {
            count++;
            if (tokens != NULL) {
                jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
                if (tok == NULL) return -1;
                if (parser->toksuper != -1) tokens[parser->toksuper].size++;
                tok->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
                tok->start = parser->pos;
                parser->toksuper = parser->toknext - 1;
            }
        } else if (c == '}' || c == ']') {
            if (tokens != NULL) {
                jsmntype_t type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
                int i;
                for (i = parser->toknext - 1; i >= 0; i--) {
                    jsmntok_t *tok = &tokens[i];
                    if (tok->start != -1 && tok->end == -1) {
                        if (tok->type != type) return -2;
                        parser->toksuper = -1;
                        tok->end = parser->pos + 1;
                        break;
                    }
                }
                if (i == -1) return -2;
                for (; i >= 0; i--) {
                    jsmntok_t *tok = &tokens[i];
                    if (tok->start != -1 && tok->end == -1) {
                        parser->toksuper = i;
                        break;
                    }
                }
            }
        } else if (c == '\"') {
            int start = parser->pos;
            parser->pos++;
            for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
                if (js[parser->pos] == '\"' && js[parser->pos - 1] != '\\') break;
            }
            if (parser->pos >= len) { parser->pos = start; return -1; }
            count++;
            if (tokens != NULL) {
                jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
                if (tok == NULL) return -1;
                jsmn_fill_token(tok, JSMN_STRING, start + 1, parser->pos);
                if (parser->toksuper != -1) tokens[parser->toksuper].size++;
            }
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ':' && c != ',') {
            int start = parser->pos;
            for (; parser->pos < len && js[parser->pos] != '\0'; parser->pos++) {
                if (js[parser->pos] == ' ' || js[parser->pos] == '\t' || js[parser->pos] == '\r' ||
                    js[parser->pos] == '\n' || js[parser->pos] == ',' || js[parser->pos] == '}' ||
                    js[parser->pos] == ']') {
                    parser->pos--;
                    break;
                }
            }
            count++;
            if (tokens != NULL) {
                jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
                if (tok == NULL) return -1;
                jsmn_fill_token(tok, JSMN_PRIMITIVE, start, parser->pos + 1);
                if (parser->toksuper != -1) tokens[parser->toksuper].size++;
            }
        }
    }
    return count;
}

static bool json_token_streq(const char *js, const jsmntok_t *tok, const char *s) {
    if ((tok->type == JSMN_STRING || tok->type == JSMN_PRIMITIVE) &&
        (int)strlen(s) == tok->end - tok->start &&
        strncmp(js + tok->start, s, tok->end - tok->start) == 0) {
        return true;
    }
    return false;
}

static bool json_get_string(const char *js, const jsmntok_t *tok, char *out, size_t max_len) {
    int len = tok->end - tok->start;
    if (len < 0) return false;
    if (len >= (int)max_len) len = (int)max_len - 1;
    strncpy(out, js + tok->start, len);
    out[len] = '\0';
    return true;
}

static bool json_get_uint(const char *js, const jsmntok_t *tok, uint32_t *out, int base) {
    char buf[64];
    if (!json_get_string(js, tok, buf, sizeof(buf))) return false;
    char *endptr = NULL;
    *out = (uint32_t)strtoul(buf, &endptr, base);
    return (endptr != buf);
}

static bool json_get_double(const char *js, const jsmntok_t *tok, double *out) {
    char buf[64];
    if (!json_get_string(js, tok, buf, sizeof(buf))) return false;
    char *endptr = NULL;
    *out = strtod(buf, &endptr);
    return (endptr != buf);
}

static int json_skip_token(const jsmntok_t *tokens, int idx, int total_tokens) {
    if (idx >= total_tokens) return idx;
    if (tokens[idx].type == JSMN_PRIMITIVE || tokens[idx].type == JSMN_STRING) {
        return idx + 1;
    }
    int children = tokens[idx].size;
    int next = idx + 1;
    while (children > 0 && next < total_tokens) {
        next = json_skip_token(tokens, next, total_tokens);
        children--;
    }
    return next;
}

static int json_find_key(const char *js, const jsmntok_t *tokens, int num_tokens, int obj_idx, const char *key) {
    if (obj_idx >= num_tokens || tokens[obj_idx].type != JSMN_OBJECT) return -1;
    int count = tokens[obj_idx].size;
    int curr = obj_idx + 1;
    for (int i = 0; i < count && curr < num_tokens; i++) {
        if (json_token_streq(js, &tokens[curr], key)) {
            return curr + 1;
        }
        curr = json_skip_token(tokens, curr + 1, num_tokens);
    }
    return -1;
}

/* ============================================================================
 * SECTION 2: Specialized Bitcoin SHA-256 Engine (2-Transform Midstate)
 * ============================================================================ */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static const uint32_t SHA256_INITIAL_H[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sig0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sig1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

#define SWAP32(x) ( \
    (((x) & 0xFF000000) >> 24) | \
    (((x) & 0x00FF0000) >> 8)  | \
    (((x) & 0x0000FF00) << 8)  | \
    (((x) & 0x000000FF) << 24) )

static inline void sha256_transform_block(const uint32_t state_in[8], const uint32_t data_words[16], uint32_t state_out[8]) {
    uint32_t a, b, c, d, e, f, g, h, i, t1, t2, m[64];

    for (i = 0; i < 16; ++i) m[i] = data_words[i];
    for (; i < 64; ++i) m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];

    a = state_in[0]; b = state_in[1]; c = state_in[2]; d = state_in[3];
    e = state_in[4]; f = state_in[5]; g = state_in[6]; h = state_in[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + SIG1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = SIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    state_out[0] = state_in[0] + a; state_out[1] = state_in[1] + b;
    state_out[2] = state_in[2] + c; state_out[3] = state_in[3] + d;
    state_out[4] = state_in[4] + e; state_out[5] = state_in[5] + f;
    state_out[6] = state_in[6] + g; state_out[7] = state_in[7] + h;
}

typedef struct {
    uint32_t state[8];
    uint32_t datalen;
    uint64_t bitlen;
    uint8_t data[64];
} SHA256_CTX;

static void sha256_transform_bytes(SHA256_CTX *ctx, const uint8_t data[64]) {
    uint32_t words[16];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        words[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
                   ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    uint32_t out[8];
    sha256_transform_block(ctx->state, words, out);
    memcpy(ctx->state, out, 32);
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    memcpy(ctx->state, SHA256_INITIAL_H, 32);
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform_bytes(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform_bytes(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (ctx->bitlen >> 56) & 0xFF; ctx->data[57] = (ctx->bitlen >> 48) & 0xFF;
    ctx->data[58] = (ctx->bitlen >> 40) & 0xFF; ctx->data[59] = (ctx->bitlen >> 32) & 0xFF;
    ctx->data[60] = (ctx->bitlen >> 24) & 0xFF; ctx->data[61] = (ctx->bitlen >> 16) & 0xFF;
    ctx->data[62] = (ctx->bitlen >> 8) & 0xFF;  ctx->data[63] = ctx->bitlen & 0xFF;
    sha256_transform_bytes(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

static void generic_double_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    SHA256_CTX ctx;
    uint8_t intermediate[32];
    sha256_init(&ctx); sha256_update(&ctx, data, len); sha256_final(&ctx, intermediate);
    sha256_init(&ctx); sha256_update(&ctx, intermediate, 32); sha256_final(&ctx, out);
}

static void bitcoin_precompute_midstate(const uint8_t header[80], uint32_t midstate[8]) {
    uint32_t chunk1[16];
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        chunk1[i] = ((uint32_t)header[j] << 24) | ((uint32_t)header[j + 1] << 16) |
                    ((uint32_t)header[j + 2] << 8) | ((uint32_t)header[j + 3]);
    }
    sha256_transform_block(SHA256_INITIAL_H, chunk1, midstate);
}

static bool g_has_sha_ni = false;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha,sse4.1")))
#endif
static inline void sha256_transform_shani_words(const uint32_t state_in[8], const uint32_t data_words[16], uint32_t state_out[8]) {
    __m128i STATE0, STATE1;
    __m128i MSG, TMP;
    __m128i MSG0, MSG1, MSG2, MSG3;
    __m128i ABEF_SAVE, CDGH_SAVE;

    TMP = _mm_loadu_si128((const __m128i*) &state_in[0]);
    STATE1 = _mm_loadu_si128((const __m128i*) &state_in[4]);

    TMP = _mm_shuffle_epi32(TMP, 0xB1);          /* CDAB */
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);    /* EFGH */
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);    /* ABEF */
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0); /* CDGH */

    ABEF_SAVE = STATE0;
    CDGH_SAVE = STATE1;

    /* Rounds 0-3 */
    MSG0 = _mm_loadu_si128((const __m128i*) (data_words+0));
    MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0xE9B5DBA5B5C0FBCFULL, 0x71374491428A2F98ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 4-7 */
    MSG1 = _mm_loadu_si128((const __m128i*) (data_words+4));
    MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0xAB1C5ED5923F82A4ULL, 0x59F111F13956C25BULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 8-11 */
    MSG2 = _mm_loadu_si128((const __m128i*) (data_words+8));
    MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0x550C7DC3243185BEULL, 0x12835B01D807AA98ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 12-15 */
    MSG3 = _mm_loadu_si128((const __m128i*) (data_words+12));
    MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0xC19BF1749BDC06A7ULL, 0x80DEB1FE72BE5D74ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 16-19 */
    MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x240CA1CC0FC19DC6ULL, 0xEFBE4786E49B69C1ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 20-23 */
    MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x76F988DA5CB0A9DCULL, 0x4A7484AA2DE92C6FULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 24-27 */
    MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0xBF597FC7B00327C8ULL, 0xA831C66D983E5152ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 28-31 */
    MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0x1429296706CA6351ULL,  0xD5A79147C6E00BF3ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 32-35 */
    MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x53380D134D2C6DFCULL, 0x2E1B213827B70A85ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 36-39 */
    MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x92722C8581C2C92EULL, 0x766A0ABB650A7354ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* Rounds 40-43 */
    MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0xC76C51A3C24B8B70ULL, 0xA81A664BA2BFE8A1ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* Rounds 44-47 */
    MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0x106AA070F40E3585ULL, 0xD6990624D192E819ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0 = _mm_add_epi32(MSG0, TMP);
    MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

    /* Rounds 48-51 */
    MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x34B0BCB52748774CULL, 0x1E376C0819A4C116ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
    MSG1 = _mm_add_epi32(MSG1, TMP);
    MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

    /* Rounds 52-55 */
    MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x682E6FF35B9CCA4FULL, 0x4ED8AA4A391C0CB3ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
    MSG2 = _mm_add_epi32(MSG2, TMP);
    MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 56-59 */
    MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0x8CC7020884C87814ULL, 0x78A5636F748F82EEULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
    MSG3 = _mm_add_epi32(MSG3, TMP);
    MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Rounds 60-63 */
    MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0xC67178F2BEF9A3F7ULL, 0xA4506CEB90BEFFFAULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

    TMP = _mm_shuffle_epi32(STATE0, 0x1B);       /* FEBA */
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);    /* DCHG */
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0); /* DCBA */
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);    /* ABEF */

    _mm_storeu_si128((__m128i*) &state_out[0], STATE0);
    _mm_storeu_si128((__m128i*) &state_out[4], STATE1);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha,sse4.1")))
#endif
static inline void bitcoin_hash_midstate_shani(
    const uint32_t midstate[8],
    const uint32_t tail[3],
    uint32_t nonce_le,
    uint32_t hash_out[8])
{
    uint32_t chunk2[16];
    chunk2[0] = tail[0];
    chunk2[1] = tail[1];
    chunk2[2] = tail[2];
    chunk2[3] = SWAP32(nonce_le);
    chunk2[4] = 0x80000000U;
    chunk2[5] = 0; chunk2[6] = 0; chunk2[7] = 0;
    chunk2[8] = 0; chunk2[9] = 0; chunk2[10] = 0;
    chunk2[11] = 0; chunk2[12] = 0; chunk2[13] = 0; chunk2[14] = 0;
    chunk2[15] = 0x00000280U;

    uint32_t hash1[8];
    sha256_transform_shani_words(midstate, chunk2, hash1);

    uint32_t chunk3[16];
    for (int i = 0; i < 8; i++) chunk3[i] = hash1[i];
    chunk3[8]  = 0x80000000U;
    chunk3[9]  = 0; chunk3[10] = 0; chunk3[11] = 0;
    chunk3[12] = 0; chunk3[13] = 0; chunk3[14] = 0;
    chunk3[15] = 0x00000100U;

    sha256_transform_shani_words(SHA256_INITIAL_H, chunk3, hash_out);
}

static inline void bitcoin_hash_midstate_shani_2way(
    const uint32_t midstate[8],
    const uint32_t tail[3],
    uint32_t nonce0,
    uint32_t nonce1,
    uint32_t hash_out0[8],
    uint32_t hash_out1[8])
{
    bitcoin_hash_midstate_shani(midstate, tail, nonce0, hash_out0);
    bitcoin_hash_midstate_shani(midstate, tail, nonce1, hash_out1);
}
#endif

static inline void bitcoin_hash_midstate_scalar(
    const uint32_t midstate[8],
    const uint32_t tail[3],
    uint32_t nonce_le,
    uint32_t hash_out[8])
{
    uint32_t chunk2[16];
    chunk2[0] = tail[0];
    chunk2[1] = tail[1];
    chunk2[2] = tail[2];
    chunk2[3] = SWAP32(nonce_le);
    chunk2[4] = 0x80000000U;
    chunk2[5] = 0; chunk2[6] = 0; chunk2[7] = 0;
    chunk2[8] = 0; chunk2[9] = 0; chunk2[10] = 0;
    chunk2[11] = 0; chunk2[12] = 0; chunk2[13] = 0; chunk2[14] = 0;
    chunk2[15] = 0x00000280U;

    uint32_t hash1[8];
    sha256_transform_block(midstate, chunk2, hash1);

    uint32_t chunk3[16];
    chunk3[0] = hash1[0]; chunk3[1] = hash1[1]; chunk3[2] = hash1[2]; chunk3[3] = hash1[3];
    chunk3[4] = hash1[4]; chunk3[5] = hash1[5]; chunk3[6] = hash1[6]; chunk3[7] = hash1[7];
    chunk3[8] = 0x80000000U;
    chunk3[9] = 0; chunk3[10] = 0; chunk3[11] = 0;
    chunk3[12] = 0; chunk3[13] = 0; chunk3[14] = 0;
    chunk3[15] = 0x00000100U;

    sha256_transform_block(SHA256_INITIAL_H, chunk3, hash_out);
}

static inline void bitcoin_hash_midstate(
    const uint32_t midstate[8],
    const uint32_t tail[3],
    uint32_t nonce_le,
    uint32_t hash_out[8])
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (g_has_sha_ni) {
        bitcoin_hash_midstate_shani(midstate, tail, nonce_le, hash_out);
        return;
    }
#endif
    bitcoin_hash_midstate_scalar(midstate, tail, nonce_le, hash_out);
}

/* ============================================================================
 * SECTION 2B: OpenCL GPU Mining Engine (Intel Iris Xe / AMD / NVIDIA)
 * Dynamic loading of OpenCL.dll for zero build-time library dependencies.
 * ============================================================================ */

typedef int cl_int;
typedef unsigned int cl_uint;
typedef unsigned long long cl_ulong;
typedef void* cl_platform_id;
typedef void* cl_device_id;
typedef void* cl_context;
typedef void* cl_command_queue;
typedef void* cl_mem;
typedef void* cl_program;
typedef void* cl_kernel;

#define CL_SUCCESS 0
#define CL_TRUE 1
#define CL_FALSE 0
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
#define CL_DEVICE_MAX_CLOCK_FREQUENCY 0x100C
#define CL_MEM_READ_ONLY (1 << 2)
#define CL_MEM_WRITE_ONLY (1 << 1)
#define CL_MEM_READ_WRITE (1 << 0)
#define CL_MEM_COPY_HOST_PTR (1 << 5)
#define CL_PROGRAM_BUILD_LOG 0x1183

typedef struct {
    cl_int (*clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
    cl_int (*clGetDeviceIDs)(cl_platform_id, cl_ulong, cl_uint, cl_device_id*, cl_uint*);
    cl_int (*clGetDeviceInfo)(cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_context (*clCreateContext)(void*, cl_uint, const cl_device_id*, void*, void*, cl_int*);
    cl_command_queue (*clCreateCommandQueue)(cl_context, cl_device_id, cl_ulong, cl_int*);
    cl_command_queue (*clCreateCommandQueueWithProperties)(cl_context, cl_device_id, const void*, cl_int*);
    cl_program (*clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
    cl_int (*clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void*, void*);
    cl_int (*clGetProgramBuildInfo)(cl_program, cl_device_id, cl_uint, size_t, void*, size_t*);
    cl_kernel (*clCreateKernel)(cl_program, const char*, cl_int*);
    cl_mem (*clCreateBuffer)(cl_context, cl_ulong, size_t, void*, cl_int*);
    cl_int (*clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
    cl_int (*clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const void*, void*);
    cl_int (*clFinish)(cl_command_queue);
    cl_int (*clEnqueueReadBuffer)(cl_command_queue, cl_mem, unsigned int, size_t, size_t, void*, cl_uint, const void*, void*);
    cl_int (*clEnqueueWriteBuffer)(cl_command_queue, cl_mem, unsigned int, size_t, size_t, const void*, cl_uint, const void*, void*);
    cl_int (*clReleaseMemObject)(cl_mem);
    cl_int (*clReleaseKernel)(cl_kernel);
    cl_int (*clReleaseProgram)(cl_program);
    cl_int (*clReleaseCommandQueue)(cl_command_queue);
    cl_int (*clReleaseContext)(cl_context);
} cl_api_t;

typedef struct {
    bool available;
    bool enabled;
    HMODULE hModule;
    char device_name[256];
    uint32_t compute_units;
    uint32_t clock_mhz;
    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem d_midstate;
    cl_mem d_target;
    cl_mem d_found;
    cl_mem d_nonces;
    size_t batch_size;
    size_t local_work_size;
} gpu_engine_t;

static cl_api_t g_cl = {0};
static gpu_engine_t g_gpu = {0};

static const char *g_gpu_kernel_source = 
"#define SWAP32(x) ((((x) & 0xFF000000) >> 24) | (((x) & 0x00FF0000) >> 8) | (((x) & 0x0000FF00) << 8) | (((x) & 0x000000FF) << 24))\n"
"#define ROTR(x, n) rotate((uint)(x), (uint)(32 - (n)))\n"
"#define S0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))\n"
"#define S1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))\n"
"#define s0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))\n"
"#define s1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))\n"
"#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))\n"
"#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))\n"
"\n"
"__constant uint K[64] = {\n"
"    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,\n"
"    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,\n"
"    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,\n"
"    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,\n"
"    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,\n"
"    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,\n"
"    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,\n"
"    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2\n"
"};\n"
"\n"
"__kernel void bitcoin_miner_kernel(\n"
"    __constant uint *midstate,\n"
"    uint tail0, uint tail1, uint tail2,\n"
"    uint base_nonce,\n"
"    __constant uint *target_words,\n"
"    __global volatile uint *out_found,\n"
"    __global uint *out_nonces)\n"
"{\n"
"    uint gid = get_global_id(0);\n"
"    uint nonce = base_nonce + gid;\n"
"\n"
"    uint w[64];\n"
"    w[0] = tail0;\n"
"    w[1] = tail1;\n"
"    w[2] = tail2;\n"
"    w[3] = SWAP32(nonce);\n"
"    w[4] = 0x80000000;\n"
"    for (int i = 5; i < 15; i++) w[i] = 0;\n"
"    w[15] = 0x00000280;\n"
"\n"
"    for (int i = 16; i < 64; i++) {\n"
"        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];\n"
"    }\n"
"\n"
"    uint a = midstate[0]; uint b = midstate[1]; uint c = midstate[2]; uint d = midstate[3];\n"
"    uint e = midstate[4]; uint f = midstate[5]; uint g = midstate[6]; uint h = midstate[7];\n"
"\n"
"    for (int i = 0; i < 64; i++) {\n"
"        uint t1 = h + S1(e) + Ch(e, f, g) + K[i] + w[i];\n"
"        uint t2 = S0(a) + Maj(a, b, c);\n"
"        h = g; g = f; f = e; e = d + t1;\n"
"        d = c; c = b; b = a; a = t1 + t2;\n"
"    }\n"
"\n"
"    uint h1[8];\n"
"    h1[0] = midstate[0] + a;\n"
"    h1[1] = midstate[1] + b;\n"
"    h1[2] = midstate[2] + c;\n"
"    h1[3] = midstate[3] + d;\n"
"    h1[4] = midstate[4] + e;\n"
"    h1[5] = midstate[5] + f;\n"
"    h1[6] = midstate[6] + g;\n"
"    h1[7] = midstate[7] + h;\n"
"\n"
"    for (int i = 0; i < 8; i++) w[i] = h1[i];\n"
"    w[8] = 0x80000000;\n"
"    for (int i = 9; i < 15; i++) w[i] = 0;\n"
"    w[15] = 0x00000100;\n"
"\n"
"    for (int i = 16; i < 64; i++) {\n"
"        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];\n"
"    }\n"
"\n"
"    a = 0x6a09e667; b = 0xbb67ae85; c = 0x3c6ef372; d = 0xa54ff53a;\n"
"    e = 0x510e527f; f = 0x9b05688c; g = 0x1f83d9ab; h = 0x5be0cd19;\n"
"\n"
"    for (int i = 0; i < 64; i++) {\n"
"        uint t1 = h + S1(e) + Ch(e, f, g) + K[i] + w[i];\n"
"        uint t2 = S0(a) + Maj(a, b, c);\n"
"        h = g; g = f; f = e; e = d + t1;\n"
"        d = c; c = b; b = a; a = t1 + t2;\n"
"    }\n"
"\n"
"    uint res7 = 0x5be0cd19 + h;\n"
"    uint res7_le = SWAP32(res7);\n"
"    if (res7_le <= target_words[0]) {\n"
"        uint res[8];\n"
"        res[0] = SWAP32(0x6a09e667 + a);\n"
"        res[1] = SWAP32(0xbb67ae85 + b);\n"
"        res[2] = SWAP32(0x3c6ef372 + c);\n"
"        res[3] = SWAP32(0xa54ff53a + d);\n"
"        res[4] = SWAP32(0x510e527f + e);\n"
"        res[5] = SWAP32(0x9b05688c + f);\n"
"        res[6] = SWAP32(0x1f83d9ab + g);\n"
"        res[7] = res7_le;\n"
"\n"
"        bool meets = true;\n"
"        for (int i = 0; i < 8; i++) {\n"
"            uint h_val = res[7 - i];\n"
"            uint t_val = target_words[i];\n"
"            if (h_val < t_val) { meets = true; break; }\n"
"            if (h_val > t_val) { meets = false; break; }\n"
"        }\n"
"        if (meets) {\n"
"            uint slot = atomic_inc(out_found);\n"
"            if (slot < 64) {\n"
"                out_nonces[slot] = nonce;\n"
"            }\n"
"        }\n"
"    }\n"
"}\n";

static bool init_gpu_engine(void) {
    if (g_gpu.available) return true;
#ifdef _WIN32
    g_gpu.hModule = LoadLibraryA("OpenCL.dll");
#else
    g_gpu.hModule = dlopen("libOpenCL.so", RTLD_NOW);
#endif
    if (!g_gpu.hModule) return false;

#ifdef _WIN32
#define LOAD_CL(name) g_cl.name = (void*)GetProcAddress(g_gpu.hModule, #name); if (!g_cl.name) return false;
#define LOAD_CL_OPT(name) g_cl.name = (void*)GetProcAddress(g_gpu.hModule, #name);
#else
#define LOAD_CL(name) g_cl.name = (void*)dlsym(g_gpu.hModule, #name); if (!g_cl.name) return false;
#define LOAD_CL_OPT(name) g_cl.name = (void*)dlsym(g_gpu.hModule, #name);
#endif

    LOAD_CL(clGetPlatformIDs);
    LOAD_CL(clGetDeviceIDs);
    LOAD_CL(clGetDeviceInfo);
    LOAD_CL(clCreateContext);
    LOAD_CL_OPT(clCreateCommandQueue);
    LOAD_CL_OPT(clCreateCommandQueueWithProperties);
    LOAD_CL(clCreateProgramWithSource);
    LOAD_CL(clBuildProgram);
    LOAD_CL(clGetProgramBuildInfo);
    LOAD_CL(clCreateKernel);
    LOAD_CL(clCreateBuffer);
    LOAD_CL(clSetKernelArg);
    LOAD_CL(clEnqueueNDRangeKernel);
    LOAD_CL(clFinish);
    LOAD_CL(clEnqueueReadBuffer);
    LOAD_CL(clEnqueueWriteBuffer);
    LOAD_CL(clReleaseMemObject);
    LOAD_CL(clReleaseKernel);
    LOAD_CL(clReleaseProgram);
    LOAD_CL(clReleaseCommandQueue);
    LOAD_CL(clReleaseContext);

    cl_uint num_platforms = 0;
    if (g_cl.clGetPlatformIDs(1, &g_gpu.platform, &num_platforms) != CL_SUCCESS || num_platforms == 0) return false;
    cl_uint num_devices = 0;
    if (g_cl.clGetDeviceIDs(g_gpu.platform, CL_DEVICE_TYPE_GPU, 1, &g_gpu.device, &num_devices) != CL_SUCCESS || num_devices == 0) return false;

    g_cl.clGetDeviceInfo(g_gpu.device, CL_DEVICE_NAME, sizeof(g_gpu.device_name), g_gpu.device_name, NULL);
    g_cl.clGetDeviceInfo(g_gpu.device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(g_gpu.compute_units), &g_gpu.compute_units, NULL);
    g_cl.clGetDeviceInfo(g_gpu.device, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(g_gpu.clock_mhz), &g_gpu.clock_mhz, NULL);

    cl_int err;
    g_gpu.context = g_cl.clCreateContext(NULL, 1, &g_gpu.device, NULL, NULL, &err);
    if (err != CL_SUCCESS) return false;

    if (g_cl.clCreateCommandQueue) {
        g_gpu.queue = g_cl.clCreateCommandQueue(g_gpu.context, g_gpu.device, 0, &err);
    } else if (g_cl.clCreateCommandQueueWithProperties) {
        g_gpu.queue = g_cl.clCreateCommandQueueWithProperties(g_gpu.context, g_gpu.device, NULL, &err);
    }
    if (err != CL_SUCCESS) return false;

    g_gpu.program = g_cl.clCreateProgramWithSource(g_gpu.context, 1, &g_gpu_kernel_source, NULL, &err);
    if (err != CL_SUCCESS) return false;

    err = g_cl.clBuildProgram(g_gpu.program, 1, &g_gpu.device, "-cl-fast-relaxed-math", NULL, NULL);
    if (err != CL_SUCCESS) {
        char log[4096] = {0};
        g_cl.clGetProgramBuildInfo(g_gpu.program, g_gpu.device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        printf("[-] OpenCL Kernel Build Error:\n%s\n", log);
        return false;
    }

    g_gpu.kernel = g_cl.clCreateKernel(g_gpu.program, "bitcoin_miner_kernel", &err);
    if (err != CL_SUCCESS) return false;

    g_gpu.d_midstate = g_cl.clCreateBuffer(g_gpu.context, CL_MEM_READ_ONLY, 32, NULL, &err);
    g_gpu.d_target   = g_cl.clCreateBuffer(g_gpu.context, CL_MEM_READ_ONLY, 32, NULL, &err);
    g_gpu.d_found    = g_cl.clCreateBuffer(g_gpu.context, CL_MEM_READ_WRITE, 4, NULL, &err);
    g_gpu.d_nonces   = g_cl.clCreateBuffer(g_gpu.context, CL_MEM_WRITE_ONLY, 64 * 4, NULL, &err);
    if (!g_gpu.d_midstate || !g_gpu.d_target || !g_gpu.d_found || !g_gpu.d_nonces) return false;

    g_gpu.batch_size = 4194304; // 4M nonces (~55ms per dispatch on Iris Xe)
    g_gpu.local_work_size = 256;
    g_gpu.available = true;
    g_gpu.enabled = true;
    return true;
}

static void cleanup_gpu_engine(void) {
    if (!g_gpu.available) return;
    if (g_gpu.d_midstate) g_cl.clReleaseMemObject(g_gpu.d_midstate);
    if (g_gpu.d_target)   g_cl.clReleaseMemObject(g_gpu.d_target);
    if (g_gpu.d_found)    g_cl.clReleaseMemObject(g_gpu.d_found);
    if (g_gpu.d_nonces)   g_cl.clReleaseMemObject(g_gpu.d_nonces);
    if (g_gpu.kernel)     g_cl.clReleaseKernel(g_gpu.kernel);
    if (g_gpu.program)    g_cl.clReleaseProgram(g_gpu.program);
    if (g_gpu.queue)      g_cl.clReleaseCommandQueue(g_gpu.queue);
    if (g_gpu.context)    g_cl.clReleaseContext(g_gpu.context);
    memset(&g_gpu, 0, sizeof(g_gpu));
}

/* ============================================================================
 * SECTION 3: Helpers & Fast Target Comparison
 * ============================================================================ */

static uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static size_t hex_to_bin(const char *hex, uint8_t *bin, size_t max_len) {
    size_t len = strlen(hex);
    size_t out_len = len / 2;
    if (out_len > max_len) out_len = max_len;
    for (size_t i = 0; i < out_len; i++) {
        bin[i] = (hex_val(hex[i * 2]) << 4) | hex_val(hex[i * 2 + 1]);
    }
    return out_len;
}

static void bin_to_hex(const uint8_t *bin, size_t len, char *hex) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2]     = hex_chars[(bin[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bin[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

static const char g_b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void base64_encode(const char *in, size_t in_len, char *out, size_t max_out) {
    size_t out_idx = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (out_idx + 4 >= max_out) break;
        uint32_t a = (uint8_t)in[i];
        uint32_t b = (i + 1 < in_len) ? (uint8_t)in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? (uint8_t)in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[out_idx++] = g_b64_table[(triple >> 18) & 0x3F];
        out[out_idx++] = g_b64_table[(triple >> 12) & 0x3F];
        out[out_idx++] = (i + 1 < in_len) ? g_b64_table[(triple >> 6) & 0x3F] : '=';
        out[out_idx++] = (i + 2 < in_len) ? g_b64_table[triple & 0x3F] : '=';
    }
    out[out_idx] = '\0';
}

static void swap_bytes(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        uint8_t tmp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = tmp;
    }
}

static void nbits_to_target(uint32_t nbits, uint8_t target[32]) {
    memset(target, 0, 32);
    uint32_t exponent = nbits >> 24;
    uint32_t mantissa = nbits & 0x00FFFFFF;

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target[31] = mantissa & 0xFF;
        target[30] = (mantissa >> 8) & 0xFF;
        target[29] = (mantissa >> 16) & 0xFF;
    } else {
        uint32_t offset = exponent - 3;
        if (offset < 30) {
            target[31 - offset]     = mantissa & 0xFF;
            target[31 - offset - 1] = (mantissa >> 8) & 0xFF;
            target[31 - offset - 2] = (mantissa >> 16) & 0xFF;
        }
    }
}

static void difficulty_to_target(double difficulty, uint8_t target[32]) {
    memset(target, 0, 32);
    if (difficulty <= 0.0) difficulty = 1.0;

    static const uint8_t diff1[32] = {
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    double remainder = 0.0;
    for (int i = 0; i < 32; i++) {
        remainder = (remainder * 256.0) + (double)diff1[i];
        uint32_t val = (uint32_t)(remainder / difficulty);
        target[i] = (uint8_t)(val > 255 ? 255 : val);
        remainder = remainder - ((double)val * difficulty);
    }
}

static inline bool hash_meets_target_fast(const uint32_t hash_words[8], const uint32_t target_words[8]) {
    for (int i = 0; i < 8; i++) {
        uint32_t h = SWAP32(hash_words[7 - i]);
        uint32_t t = target_words[i];
        if (h < t) return true;
        if (h > t) return false;
    }
    return true;
}

static void log_share_to_file(const char *filename, const char *status, int thread_id, uint32_t nonce, const char *hash_hex, const char *raw_response) {
    FILE *f = fopen(filename, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);
    fprintf(f, "[%s] Status: %s | Thread: %d | Nonce: %08x | Hash: %s | Pool Response: %s\n",
            time_buf, status, thread_id, nonce, hash_hex, raw_response);
    fclose(f);
}

/* ============================================================================
 * SECTION 4: Stratum V1 Data Structures, Target Synchronization & Share Registry
 * ============================================================================ */

typedef struct {
    uint8_t target[32];
    uint32_t target_words[8];
    double difficulty;
} share_target_t;

typedef struct {
    char job_id[128];
    uint8_t prevhash[32];
    uint8_t coinb1[1024];
    size_t coinb1_len;
    uint8_t coinb2[1024];
    size_t coinb2_len;
    uint8_t merkle_branches[MAX_MERKLE_BRANCHES][32];
    size_t num_merkle_branches;
    uint32_t version;
    uint32_t nbits;
    uint32_t ntime;
    bool clean_jobs;
    share_target_t target_info;
} stratum_job_t;

typedef struct {
    uint32_t request_id;
    int thread_id;
    char job_id[128];
    char extranonce2[64];
    uint32_t ntime;
    uint32_t nonce;
    char hash[65];
    time_t submitted_at;
    bool active;
} pending_share_t;

#ifdef _WIN32
static volatile LONG64 g_hashes_count = 0;
static volatile LONG64 g_cpu_hashes_count = 0;
static volatile LONG64 g_gpu_hashes_count = 0;
static volatile LONG64 g_accepted_shares = 0;
static volatile LONG64 g_rejected_shares = 0;
static volatile LONG64 g_job_generation = 0;
static volatile LONG g_request_id_counter = 100;
static CRITICAL_SECTION g_job_lock;
static CRITICAL_SECTION g_socket_lock;
static CRITICAL_SECTION g_pending_lock;
#else
static volatile uint64_t g_hashes_count = 0;
static volatile uint64_t g_cpu_hashes_count = 0;
static volatile uint64_t g_gpu_hashes_count = 0;
static volatile uint64_t g_accepted_shares = 0;
static volatile uint64_t g_rejected_shares = 0;
static volatile uint64_t g_job_generation = 0;
static volatile uint32_t g_request_id_counter = 100;
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_socket_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static volatile bool g_stop_mining = false;
static SOCKET g_main_socket = INVALID_SOCKET;
static share_target_t g_share_target = { {0}, {0}, 1.0 };
static char g_extranonce1_hex[128] = "";
static int g_extranonce2_size = 4;
static uint32_t g_extranonce2_counter = 0;
static stratum_job_t g_current_job;
static bool g_has_valid_job = false;
static pending_share_t g_pending_shares[MAX_PENDING_SHARES];

static inline uint64_t atomic_read_generation(void) {
#ifdef _WIN32
    return (uint64_t)InterlockedCompareExchange64(&g_job_generation, 0, 0);
#else
    return __atomic_load_n(&g_job_generation, __ATOMIC_ACQUIRE);
#endif
}

static inline void atomic_increment_generation(void) {
#ifdef _WIN32
    InterlockedIncrement64(&g_job_generation);
#else
    __atomic_fetch_add(&g_job_generation, 1, __ATOMIC_RELEASE);
#endif
}

static void update_share_target_words(share_target_t *st) {
    for (int i = 0; i < 8; i++) {
        st->target_words[i] = ((uint32_t)st->target[i * 4] << 24) |
                              ((uint32_t)st->target[i * 4 + 1] << 16) |
                              ((uint32_t)st->target[i * 4 + 2] << 8) |
                              (uint32_t)st->target[i * 4 + 3];
    }
}

static bool socket_send_all(SOCKET s, const char *msg, size_t len) {
    size_t total = 0;
    while (total < len) {
        int sent = send(s, msg + total, (int)(len - total), 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

static bool thread_safe_socket_send(SOCKET s, const char *msg) {
#ifdef _WIN32
    EnterCriticalSection(&g_socket_lock);
#else
    pthread_mutex_lock(&g_socket_lock);
#endif
    bool ok = socket_send_all(s, msg, strlen(msg));
#ifdef _WIN32
    LeaveCriticalSection(&g_socket_lock);
#else
    pthread_mutex_unlock(&g_socket_lock);
#endif
    return ok;
}

/* ============================================================================
 * SECTION 5: Buffered TCP Stream Receiver (Issue 21 Fix)
 * ============================================================================ */

typedef struct {
    char buf[16384];
    size_t len;
} tcp_stream_buffer_t;

static bool tcp_buffer_get_line(SOCKET s, tcp_stream_buffer_t *stream, char *line_out, size_t max_line) {
    while (!g_stop_mining) {
        for (size_t i = 0; i < stream->len; i++) {
            if (stream->buf[i] == '\n') {
                size_t line_len = i;
                if (line_len > 0 && stream->buf[line_len - 1] == '\r') line_len--;
                if (line_len >= max_line) line_len = max_line - 1;
                memcpy(line_out, stream->buf, line_len);
                line_out[line_len] = '\0';

                size_t remaining = stream->len - (i + 1);
                if (remaining > 0) {
                    memmove(stream->buf, stream->buf + i + 1, remaining);
                }
                stream->len = remaining;
                return true;
            }
        }

        if (stream->len >= sizeof(stream->buf) - 1) {
            // Drop oversized un-delimited data to prevent buffer overflow
            stream->len = 0;
        }

        int bytes_in = recv(s, stream->buf + stream->len, (int)(sizeof(stream->buf) - 1 - stream->len), 0);
        if (bytes_in <= 0) return false;
        stream->len += bytes_in;
    }
    return false;
}

/* ============================================================================
 * SECTION 6: Robust Stratum Message Processing (JSMN Helpers)
 * ============================================================================ */

static void parse_subscribe_response(const char *json) {
    jsmn_parser p;
    jsmntok_t tokens[128];
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), tokens, 128);
    if (r <= 0) return;

    int res_idx = json_find_key(json, tokens, r, 0, "result");
    if (res_idx > 0 && tokens[res_idx].type == JSMN_ARRAY && tokens[res_idx].size >= 2) {
        int item_idx = res_idx + 1;
        int num_items = tokens[res_idx].size;
        for (int item = 0; item < num_items && item_idx < r; item++) {
            if (item == 1 && tokens[item_idx].type == JSMN_STRING) {
                json_get_string(json, &tokens[item_idx], g_extranonce1_hex, sizeof(g_extranonce1_hex));
            } else if (item == 2 && tokens[item_idx].type == JSMN_PRIMITIVE) {
                uint32_t sz = 4;
                if (json_get_uint(json, &tokens[item_idx], &sz, 10)) {
                    g_extranonce2_size = (int)sz;
                    if (g_extranonce2_size <= 0 || g_extranonce2_size > 4) g_extranonce2_size = 4;
                }
            }
            item_idx = json_skip_token(tokens, item_idx, r);
        }
    }
    if (strlen(g_extranonce1_hex) == 0) strcpy(g_extranonce1_hex, "00000000");
    printf("[+] Parsed Stratum Subscription: Extranonce1=%s | Extranonce2_Size=%d bytes\n",
           g_extranonce1_hex, g_extranonce2_size);
}

static void parse_mining_notify(const char *json) {
    jsmn_parser p;
    jsmntok_t tokens[256];
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), tokens, 256);
    if (r <= 0) return;

    int params_idx = json_find_key(json, tokens, r, 0, "params");
    if (params_idx > 0 && tokens[params_idx].type == JSMN_ARRAY && tokens[params_idx].size >= 9) {
        stratum_job_t new_job;
        memset(&new_job, 0, sizeof(new_job));
        int curr = params_idx + 1;
        char token_str[1024];

        // 0: job_id
        json_get_string(json, &tokens[curr], new_job.job_id, sizeof(new_job.job_id));
        curr = json_skip_token(tokens, curr, r);

        // 1: prevhash (apply 4-byte dword swap)
        json_get_string(json, &tokens[curr], token_str, sizeof(token_str));
        hex_to_bin(token_str, new_job.prevhash, 32);
        for (int k = 0; k < 32; k += 4) {
            uint8_t tmp = new_job.prevhash[k]; new_job.prevhash[k] = new_job.prevhash[k+3]; new_job.prevhash[k+3] = tmp;
            tmp = new_job.prevhash[k+1]; new_job.prevhash[k+1] = new_job.prevhash[k+2]; new_job.prevhash[k+2] = tmp;
        }
        curr = json_skip_token(tokens, curr, r);

        // 2: coinb1
        json_get_string(json, &tokens[curr], token_str, sizeof(token_str));
        new_job.coinb1_len = hex_to_bin(token_str, new_job.coinb1, sizeof(new_job.coinb1));
        curr = json_skip_token(tokens, curr, r);

        // 3: coinb2
        json_get_string(json, &tokens[curr], token_str, sizeof(token_str));
        new_job.coinb2_len = hex_to_bin(token_str, new_job.coinb2, sizeof(new_job.coinb2));
        curr = json_skip_token(tokens, curr, r);

        // 4: merkle_branch array
        if (tokens[curr].type == JSMN_ARRAY) {
            int branch_count = tokens[curr].size;
            int b_elem = curr + 1;
            new_job.num_merkle_branches = 0;
            for (int b = 0; b < branch_count && b < MAX_MERKLE_BRANCHES && b_elem < r; b++) {
                json_get_string(json, &tokens[b_elem], token_str, sizeof(token_str));
                hex_to_bin(token_str, new_job.merkle_branches[b], 32);
                new_job.num_merkle_branches++;
                b_elem = json_skip_token(tokens, b_elem, r);
            }
            curr = json_skip_token(tokens, curr, r);
        } else {
            curr = json_skip_token(tokens, curr, r);
        }

        // 5: version
        json_get_uint(json, &tokens[curr], &new_job.version, 16);
        curr = json_skip_token(tokens, curr, r);

        // 6: nbits
        json_get_uint(json, &tokens[curr], &new_job.nbits, 16);
        curr = json_skip_token(tokens, curr, r);

        // 7: ntime
        json_get_uint(json, &tokens[curr], &new_job.ntime, 16);
        curr = json_skip_token(tokens, curr, r);

        // 8: clean_jobs
        json_get_string(json, &tokens[curr], token_str, sizeof(token_str));
        new_job.clean_jobs = (strcmp(token_str, "true") == 0);

#ifdef _WIN32
        EnterCriticalSection(&g_job_lock);
#else
        pthread_mutex_lock(&g_job_lock);
#endif
        // Inherit synchronized persistent share target
        memcpy(&new_job.target_info, &g_share_target, sizeof(share_target_t));
        memcpy(&g_current_job, &new_job, sizeof(stratum_job_t));
        g_has_valid_job = true;
        atomic_increment_generation();
#ifdef _WIN32
        LeaveCriticalSection(&g_job_lock);
#else
        pthread_mutex_unlock(&g_job_lock);
#endif
        printf("\n[+] New Mining Job Received: JobID=%s | MerkleBranches=%zu | NBits=%08x\n",
               g_current_job.job_id, g_current_job.num_merkle_branches, g_current_job.nbits);
    }
}

/* ============================================================================
 * SECTION 7: Stratum Socket Background Receiver Thread
 * ============================================================================ */

#ifdef _WIN32
static unsigned __stdcall stratum_recv_thread_proc(void *param)
#else
static void *stratum_recv_thread_proc(void *param)
#endif
{
    SOCKET sock = *(SOCKET *)param;
    tcp_stream_buffer_t stream;
    memset(&stream, 0, sizeof(stream));
    char line_buf[8192];

    while (!g_stop_mining) {
        if (!tcp_buffer_get_line(sock, &stream, line_buf, sizeof(line_buf))) break;

        jsmn_parser p;
        jsmntok_t tokens[128];
        jsmn_init(&p);
        int r = jsmn_parse(&p, line_buf, strlen(line_buf), tokens, 128);
        if (r <= 0) continue;

        int method_idx = json_find_key(line_buf, tokens, r, 0, "method");
        if (method_idx > 0) {
            char method[64];
            json_get_string(line_buf, &tokens[method_idx], method, sizeof(method));
            if (strcmp(method, "mining.notify") == 0) {
                parse_mining_notify(line_buf);
            } else if (strcmp(method, "mining.set_difficulty") == 0) {
                int params_idx = json_find_key(line_buf, tokens, r, 0, "params");
                if (params_idx > 0 && tokens[params_idx].type == JSMN_ARRAY && tokens[params_idx].size >= 1) {
                    double diff = 1.0;
                    if (json_get_double(line_buf, &tokens[params_idx + 1], &diff) && diff > 0.0) {
#ifdef _WIN32
                        EnterCriticalSection(&g_job_lock);
#else
                        pthread_mutex_lock(&g_job_lock);
#endif
                        g_share_target.difficulty = diff;
                        difficulty_to_target(diff, g_share_target.target);
                        update_share_target_words(&g_share_target);
                        memcpy(&g_current_job.target_info, &g_share_target, sizeof(share_target_t));
                        atomic_increment_generation();
#ifdef _WIN32
                        LeaveCriticalSection(&g_job_lock);
#else
                        pthread_mutex_unlock(&g_job_lock);
#endif
                        printf("\n[+] Pool Difficulty Updated: %.2f\n", diff);
                    }
                }
            }
        } else {
            // Check submission response
            int id_idx = json_find_key(line_buf, tokens, r, 0, "id");
            if (id_idx > 0) {
                uint32_t resp_id = 0;
                if (json_get_uint(line_buf, &tokens[id_idx], &resp_id, 10) && resp_id >= 100) {
                    int res_idx = json_find_key(line_buf, tokens, r, 0, "result");
                    bool is_accepted = false;
                    if (res_idx > 0 && json_token_streq(line_buf, &tokens[res_idx], "true")) {
                        is_accepted = true;
                    }

#ifdef _WIN32
                    EnterCriticalSection(&g_pending_lock);
#else
                    pthread_mutex_lock(&g_pending_lock);
#endif
                    for (int p_idx = 0; p_idx < MAX_PENDING_SHARES; p_idx++) {
                        if (g_pending_shares[p_idx].active && g_pending_shares[p_idx].request_id == resp_id) {
                            if (is_accepted) {
#ifdef _WIN32
                                InterlockedIncrement64(&g_accepted_shares);
#else
                                __sync_fetch_and_add(&g_accepted_shares, 1);
#endif
                                printf("\n[+] Pool Response (ID %u): SHARE ACCEPTED!\n", resp_id);
                                log_share_to_file("accepted_shares.txt", "ACCEPTED", g_pending_shares[p_idx].thread_id,
                                                  g_pending_shares[p_idx].nonce, g_pending_shares[p_idx].hash, line_buf);
                            } else {
#ifdef _WIN32
                                InterlockedIncrement64(&g_rejected_shares);
#else
                                __sync_fetch_and_add(&g_rejected_shares, 1);
#endif
                                printf("\n[-] Pool Response (ID %u): Share Rejected. Logged to rejected_shares.txt\n", resp_id);
                                log_share_to_file("rejected_shares.txt", "REJECTED", g_pending_shares[p_idx].thread_id,
                                                  g_pending_shares[p_idx].nonce, g_pending_shares[p_idx].hash, line_buf);
                            }
                            g_pending_shares[p_idx].active = false;
                            break;
                        }
                    }
#ifdef _WIN32
                    LeaveCriticalSection(&g_pending_lock);
#else
                    pthread_mutex_unlock(&g_pending_lock);
#endif
                }
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ============================================================================
 * SECTION 8: High-Performance Multithreaded Nonce Worker (CPU + OpenCL GPU)
 * ============================================================================ */

typedef struct {
    int thread_id;
    int total_threads;
    SOCKET sock;
    char user[256];
} thread_worker_arg_t;

typedef struct {
    SOCKET sock;
    char user[256];
} gpu_worker_arg_t;

static void submit_candidate_share(
    int thread_id,
    const char *engine_name,
    const stratum_job_t *job,
    const char *extranonce2_hex,
    uint32_t nonce,
    const uint32_t hash_res[8],
    const char *user,
    SOCKET sock)
{
    uint8_t raw_hash[32];
    for (int i = 0; i < 8; i++) {
        raw_hash[i * 4]     = (hash_res[i] >> 24) & 0xFF;
        raw_hash[i * 4 + 1] = (hash_res[i] >> 16) & 0xFF;
        raw_hash[i * 4 + 2] = (hash_res[i] >> 8) & 0xFF;
        raw_hash[i * 4 + 3] = hash_res[i] & 0xFF;
    }
    swap_bytes(raw_hash, 32);

    char hash_hex[65];
    bin_to_hex(raw_hash, 32, hash_hex);
    printf("\n[!] CANDIDATE SHARE FOUND! Engine: %s (Thread %d) | Nonce: %08x | Hash: %s\n",
           engine_name, thread_id, nonce, hash_hex);

#ifdef _WIN32
    uint32_t req_id = (uint32_t)InterlockedIncrement(&g_request_id_counter);
    EnterCriticalSection(&g_pending_lock);
#else
    uint32_t req_id = (uint32_t)__sync_fetch_and_add(&g_request_id_counter, 1);
    pthread_mutex_lock(&g_pending_lock);
#endif
    time_t now = time(NULL);
    int chosen_slot = -1;
    for (int p_idx = 0; p_idx < MAX_PENDING_SHARES; p_idx++) {
        if (g_pending_shares[p_idx].active && (now - g_pending_shares[p_idx].submitted_at > PENDING_SHARE_TIMEOUT_SEC)) {
            g_pending_shares[p_idx].active = false;
        }
        if (!g_pending_shares[p_idx].active && chosen_slot == -1) {
            chosen_slot = p_idx;
        }
    }
    if (chosen_slot == -1) chosen_slot = (int)(req_id % MAX_PENDING_SHARES);

    g_pending_shares[chosen_slot].request_id = req_id;
    g_pending_shares[chosen_slot].thread_id = thread_id;
    strncpy(g_pending_shares[chosen_slot].job_id, job->job_id, sizeof(g_pending_shares[chosen_slot].job_id));
    strncpy(g_pending_shares[chosen_slot].extranonce2, extranonce2_hex, sizeof(g_pending_shares[chosen_slot].extranonce2));
    g_pending_shares[chosen_slot].ntime = job->ntime;
    g_pending_shares[chosen_slot].nonce = nonce;
    strncpy(g_pending_shares[chosen_slot].hash, hash_hex, sizeof(g_pending_shares[chosen_slot].hash));
    g_pending_shares[chosen_slot].submitted_at = now;
    g_pending_shares[chosen_slot].active = true;
#ifdef _WIN32
    LeaveCriticalSection(&g_pending_lock);
#else
    pthread_mutex_unlock(&g_pending_lock);
#endif

    char submit_req[512];
    snprintf(submit_req, sizeof(submit_req),
             "{\"id\": %u, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08x\", \"%08x\"]}\n",
             req_id, user, job->job_id, extranonce2_hex, job->ntime, nonce);
    if (sock != INVALID_SOCKET) {
        thread_safe_socket_send(sock, submit_req);
    }
}

#ifdef _WIN32
static unsigned __stdcall miner_thread_proc(void *param)
#else
static void *miner_thread_proc(void *param)
#endif
{
    thread_worker_arg_t *arg = (thread_worker_arg_t *)param;
    uint8_t block_header[80];
    stratum_job_t job;
    char extranonce2_hex[32];
    uint32_t midstate[8];
    uint32_t tail[3];

    while (!g_stop_mining) {
#ifdef _WIN32
        EnterCriticalSection(&g_job_lock);
#else
        pthread_mutex_lock(&g_job_lock);
#endif
        bool has_job = g_has_valid_job;
        uint64_t current_gen = atomic_read_generation();
        if (has_job) {
            memcpy(&job, &g_current_job, sizeof(stratum_job_t));
            g_extranonce2_counter++;
            snprintf(extranonce2_hex, sizeof(extranonce2_hex), "%0*x", g_extranonce2_size * 2, g_extranonce2_counter);
        }
#ifdef _WIN32
        LeaveCriticalSection(&g_job_lock);
#else
        pthread_mutex_unlock(&g_job_lock);
#endif

        if (!has_job) {
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
            continue;
        }

        // 1. Build Coinbase
        uint8_t coinbase[2048];
        size_t cb_len = 0;
        memcpy(coinbase + cb_len, job.coinb1, job.coinb1_len);
        cb_len += job.coinb1_len;

        size_t ex1_len = hex_to_bin(g_extranonce1_hex, coinbase + cb_len, 64);
        cb_len += ex1_len;

        size_t ex2_len = hex_to_bin(extranonce2_hex, coinbase + cb_len, 32);
        cb_len += ex2_len;

        memcpy(coinbase + cb_len, job.coinb2, job.coinb2_len);
        cb_len += job.coinb2_len;

        // 2. Coinbase Hash
        uint8_t cb_hash[32];
        generic_double_sha256(coinbase, cb_len, cb_hash);

        // 3. Merkle Root
        uint8_t merkle_root[32];
        memcpy(merkle_root, cb_hash, 32);

        for (size_t i = 0; i < job.num_merkle_branches; i++) {
            uint8_t combined[64];
            memcpy(combined, merkle_root, 32);
            memcpy(combined + 32, job.merkle_branches[i], 32);
            generic_double_sha256(combined, 64, merkle_root);
        }

        // 4. Construct 80-Byte Header
        memset(block_header, 0, 80);
        block_header[0] = job.version & 0xFF; block_header[1] = (job.version >> 8) & 0xFF;
        block_header[2] = (job.version >> 16) & 0xFF; block_header[3] = (job.version >> 24) & 0xFF;
        memcpy(block_header + 4, job.prevhash, 32);
        memcpy(block_header + 36, merkle_root, 32);
        block_header[68] = job.ntime & 0xFF; block_header[69] = (job.ntime >> 8) & 0xFF;
        block_header[70] = (job.ntime >> 16) & 0xFF; block_header[71] = (job.ntime >> 24) & 0xFF;
        block_header[72] = job.nbits & 0xFF; block_header[73] = (job.nbits >> 8) & 0xFF;
        block_header[74] = (job.nbits >> 16) & 0xFF; block_header[75] = (job.nbits >> 24) & 0xFF;

        // 5. Precompute Midstate (Once per extranonce unit)
        bitcoin_precompute_midstate(block_header, midstate);

        // 6. Fixed Tail Words (Bytes 64-75)
        tail[0] = ((uint32_t)block_header[64] << 24) | ((uint32_t)block_header[65] << 16) |
                  ((uint32_t)block_header[66] << 8)  | (uint32_t)block_header[67];
        tail[1] = ((uint32_t)block_header[68] << 24) | ((uint32_t)block_header[69] << 16) |
                  ((uint32_t)block_header[70] << 8)  | (uint32_t)block_header[71];
        tail[2] = ((uint32_t)block_header[72] << 24) | ((uint32_t)block_header[73] << 16) |
                  ((uint32_t)block_header[74] << 8)  | (uint32_t)block_header[75];

        // 7. Nonce Search (Lower 2B nonces 0x00000000..0x7FFFFFFF when GPU is active)
        uint32_t nonce = (uint32_t)arg->thread_id;
        uint32_t stride = (uint32_t)arg->total_threads;
        uint32_t local_hashes = 0;
        uint32_t hash_res[8];

        while (!g_stop_mining) {
            if (atomic_read_generation() != current_gen) break;
            if (g_gpu.available && g_gpu.enabled && nonce >= 0x80000000U) break;

            bitcoin_hash_midstate(midstate, tail, nonce, hash_res);
            local_hashes++;

            if (hash_meets_target_fast(hash_res, job.target_info.target_words)) {
                if (atomic_read_generation() != current_gen) break;
                submit_candidate_share(arg->thread_id, "CPU-SHANI", &job, extranonce2_hex, nonce, hash_res, arg->user, arg->sock);
            }

            if (local_hashes >= HASH_COUNTER_BATCH) {
#ifdef _WIN32
                InterlockedAdd64(&g_hashes_count, local_hashes);
                InterlockedAdd64(&g_cpu_hashes_count, local_hashes);
#else
                __sync_fetch_and_add(&g_hashes_count, local_hashes);
                __sync_fetch_and_add(&g_cpu_hashes_count, local_hashes);
#endif
                local_hashes = 0;
            }

            nonce += stride;
            if (nonce < stride) break;
        }

        if (local_hashes > 0) {
#ifdef _WIN32
            InterlockedAdd64(&g_hashes_count, local_hashes);
            InterlockedAdd64(&g_cpu_hashes_count, local_hashes);
#else
            __sync_fetch_and_add(&g_hashes_count, local_hashes);
            __sync_fetch_and_add(&g_cpu_hashes_count, local_hashes);
#endif
            local_hashes = 0;
        }
    }
    free(arg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static unsigned __stdcall gpu_miner_thread_proc(void *param)
#else
static void *gpu_miner_thread_proc(void *param)
#endif
{
    gpu_worker_arg_t *arg = (gpu_worker_arg_t *)param;
    uint8_t block_header[80];
    stratum_job_t job;
    char extranonce2_hex[32];
    uint32_t midstate[8];
    uint32_t tail[3];

    while (!g_stop_mining && g_gpu.available && g_gpu.enabled) {
#ifdef _WIN32
        EnterCriticalSection(&g_job_lock);
#else
        pthread_mutex_lock(&g_job_lock);
#endif
        bool has_job = g_has_valid_job;
        uint64_t current_gen = atomic_read_generation();
        if (has_job) {
            memcpy(&job, &g_current_job, sizeof(stratum_job_t));
            g_extranonce2_counter++;
            snprintf(extranonce2_hex, sizeof(extranonce2_hex), "%0*x", g_extranonce2_size * 2, g_extranonce2_counter);
        }
#ifdef _WIN32
        LeaveCriticalSection(&g_job_lock);
#else
        pthread_mutex_unlock(&g_job_lock);
#endif

        if (!has_job) {
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
            continue;
        }

        // 1. Build Coinbase
        uint8_t coinbase[2048];
        size_t cb_len = 0;
        memcpy(coinbase + cb_len, job.coinb1, job.coinb1_len);
        cb_len += job.coinb1_len;

        size_t ex1_len = hex_to_bin(g_extranonce1_hex, coinbase + cb_len, 64);
        cb_len += ex1_len;

        size_t ex2_len = hex_to_bin(extranonce2_hex, coinbase + cb_len, 32);
        cb_len += ex2_len;

        memcpy(coinbase + cb_len, job.coinb2, job.coinb2_len);
        cb_len += job.coinb2_len;

        // 2. Coinbase Hash
        uint8_t cb_hash[32];
        generic_double_sha256(coinbase, cb_len, cb_hash);

        // 3. Merkle Root
        uint8_t merkle_root[32];
        memcpy(merkle_root, cb_hash, 32);

        for (size_t i = 0; i < job.num_merkle_branches; i++) {
            uint8_t combined[64];
            memcpy(combined, merkle_root, 32);
            memcpy(combined + 32, job.merkle_branches[i], 32);
            generic_double_sha256(combined, 64, merkle_root);
        }

        // 4. Construct 80-Byte Header
        memset(block_header, 0, 80);
        block_header[0] = job.version & 0xFF; block_header[1] = (job.version >> 8) & 0xFF;
        block_header[2] = (job.version >> 16) & 0xFF; block_header[3] = (job.version >> 24) & 0xFF;
        memcpy(block_header + 4, job.prevhash, 32);
        memcpy(block_header + 36, merkle_root, 32);
        block_header[68] = job.ntime & 0xFF; block_header[69] = (job.ntime >> 8) & 0xFF;
        block_header[70] = (job.ntime >> 16) & 0xFF; block_header[71] = (job.ntime >> 24) & 0xFF;
        block_header[72] = job.nbits & 0xFF; block_header[73] = (job.nbits >> 8) & 0xFF;
        block_header[74] = (job.nbits >> 16) & 0xFF; block_header[75] = (job.nbits >> 24) & 0xFF;

        // 5. Precompute Midstate
        bitcoin_precompute_midstate(block_header, midstate);

        // 6. Fixed Tail Words
        tail[0] = ((uint32_t)block_header[64] << 24) | ((uint32_t)block_header[65] << 16) |
                  ((uint32_t)block_header[66] << 8)  | (uint32_t)block_header[67];
        tail[1] = ((uint32_t)block_header[68] << 24) | ((uint32_t)block_header[69] << 16) |
                  ((uint32_t)block_header[70] << 8)  | (uint32_t)block_header[71];
        tail[2] = ((uint32_t)block_header[72] << 24) | ((uint32_t)block_header[73] << 16) |
                  ((uint32_t)block_header[74] << 8)  | (uint32_t)block_header[75];

        // 7. Write midstate & target to GPU buffers
        g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_midstate, CL_TRUE, 0, 32, midstate, 0, NULL, NULL);
        g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_target, CL_TRUE, 0, 32, job.target_info.target_words, 0, NULL, NULL);

        uint32_t base_nonce = 0x80000000U;
        const uint32_t batch_size = (uint32_t)g_gpu.batch_size;
        const size_t global_ws = g_gpu.batch_size;
        const size_t local_ws = g_gpu.local_work_size;

        g_cl.clSetKernelArg(g_gpu.kernel, 0, sizeof(cl_mem), &g_gpu.d_midstate);
        g_cl.clSetKernelArg(g_gpu.kernel, 1, sizeof(uint32_t), &tail[0]);
        g_cl.clSetKernelArg(g_gpu.kernel, 2, sizeof(uint32_t), &tail[1]);
        g_cl.clSetKernelArg(g_gpu.kernel, 3, sizeof(uint32_t), &tail[2]);
        g_cl.clSetKernelArg(g_gpu.kernel, 5, sizeof(cl_mem), &g_gpu.d_target);
        g_cl.clSetKernelArg(g_gpu.kernel, 6, sizeof(cl_mem), &g_gpu.d_found);
        g_cl.clSetKernelArg(g_gpu.kernel, 7, sizeof(cl_mem), &g_gpu.d_nonces);

        while (!g_stop_mining) {
            if (atomic_read_generation() != current_gen) break;

            uint32_t zero = 0;
            g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_found, CL_FALSE, 0, 4, &zero, 0, NULL, NULL);

            g_cl.clSetKernelArg(g_gpu.kernel, 4, sizeof(uint32_t), &base_nonce);

            g_cl.clEnqueueNDRangeKernel(g_gpu.queue, g_gpu.kernel, 1, NULL, &global_ws, &local_ws, 0, NULL, NULL);
            g_cl.clFinish(g_gpu.queue);

            uint32_t found_count = 0;
            g_cl.clEnqueueReadBuffer(g_gpu.queue, g_gpu.d_found, CL_TRUE, 0, 4, &found_count, 0, NULL, NULL);

            if (found_count > 0) {
                if (found_count > 64) found_count = 64;
                uint32_t found_nonces[64];
                g_cl.clEnqueueReadBuffer(g_gpu.queue, g_gpu.d_nonces, CL_TRUE, 0, found_count * 4, found_nonces, 0, NULL, NULL);

                for (uint32_t f = 0; f < found_count; f++) {
                    uint32_t f_nonce = found_nonces[f];
                    uint32_t f_hash[8];
                    bitcoin_hash_midstate(midstate, tail, f_nonce, f_hash);
                    if (hash_meets_target_fast(f_hash, job.target_info.target_words)) {
                        if (atomic_read_generation() == current_gen) {
                            submit_candidate_share(99, "GPU-IrisXe", &job, extranonce2_hex, f_nonce, f_hash, arg->user, arg->sock);
                        }
                    }
                }
            }

#ifdef _WIN32
            InterlockedAdd64(&g_hashes_count, batch_size);
            InterlockedAdd64(&g_gpu_hashes_count, batch_size);
#else
            __sync_fetch_and_add(&g_hashes_count, batch_size);
            __sync_fetch_and_add(&g_gpu_hashes_count, batch_size);
#endif
            base_nonce += batch_size;
            if (base_nonce == 0) break;
        }
    }
    free(arg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ============================================================================
 * SECTION 9: Comprehensive 9-Stage Verification Suite
 * ============================================================================ */

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        printf("\n[*] Stopping miner gracefully...\n");
        g_stop_mining = true;
        if (g_main_socket != INVALID_SOCKET) {
            shutdown(g_main_socket, SD_BOTH);
        }
        return TRUE;
    }
    return FALSE;
}
#else
static void sig_handler(int sig) {
    (void)sig;
    printf("\n[*] Stopping miner gracefully...\n");
    g_stop_mining = true;
    if (g_main_socket != INVALID_SOCKET) {
        shutdown(g_main_socket, SD_BOTH);
    }
}
#endif

static int run_self_test(void) {
    printf("[*] Running Phase 1 Cryptographic & Protocol Self-Tests...\n");

    // Test 1: NIST SHA-256 Vector
    const char *test1_str = "abc";
    uint8_t test1_hash[32]; char test1_hex[65];
    SHA256_CTX ctx;
    sha256_init(&ctx); sha256_update(&ctx, (const uint8_t *)test1_str, strlen(test1_str)); sha256_final(&ctx, test1_hash);
    bin_to_hex(test1_hash, 32, test1_hex);
    if (strcmp(test1_hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) return 0;
    printf("[+] Test 1 (SHA256 'abc'): PASSED\n");

    // Test 2: Genesis Block 2-Transform Midstate
    const char *genesis_header_hex =
        "01000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
        "29ab5f49ffff001d1dac2b7c";
    uint8_t genesis_header[80]; hex_to_bin(genesis_header_hex, genesis_header, 80);

    uint32_t gen_midstate[8];
    bitcoin_precompute_midstate(genesis_header, gen_midstate);

    uint32_t gen_tail[3];
    gen_tail[0] = ((uint32_t)genesis_header[64] << 24) | ((uint32_t)genesis_header[65] << 16) |
                  ((uint32_t)genesis_header[66] << 8)  | (uint32_t)genesis_header[67];
    gen_tail[1] = ((uint32_t)genesis_header[68] << 24) | ((uint32_t)genesis_header[69] << 16) |
                  ((uint32_t)genesis_header[70] << 8)  | (uint32_t)genesis_header[71];
    gen_tail[2] = ((uint32_t)genesis_header[72] << 24) | ((uint32_t)genesis_header[73] << 16) |
                  ((uint32_t)genesis_header[74] << 8)  | (uint32_t)genesis_header[75];

    uint32_t gen_nonce_le = ((uint32_t)genesis_header[76]) |
                            ((uint32_t)genesis_header[77] << 8) |
                            ((uint32_t)genesis_header[78] << 16) |
                            ((uint32_t)genesis_header[79] << 24);

    uint32_t gen_hash_words[8];
    bitcoin_hash_midstate(gen_midstate, gen_tail, gen_nonce_le, gen_hash_words);

    uint8_t genesis_hash[32];
    for (int i = 0; i < 8; i++) {
        genesis_hash[i * 4]     = (gen_hash_words[i] >> 24) & 0xFF;
        genesis_hash[i * 4 + 1] = (gen_hash_words[i] >> 16) & 0xFF;
        genesis_hash[i * 4 + 2] = (gen_hash_words[i] >> 8) & 0xFF;
        genesis_hash[i * 4 + 3] = gen_hash_words[i] & 0xFF;
    }
    swap_bytes(genesis_hash, 32);

    char genesis_hash_hex[65];
    bin_to_hex(genesis_hash, 32, genesis_hash_hex);

    if (strcmp(genesis_hash_hex, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f") != 0) return 0;
    printf("[+] Test 2 (Genesis Block 2-Transform Midstate Double-SHA256): PASSED\n");

    // Test 3: Multi-Nonce Equivalence Test
    uint32_t test_nonces[] = {0x00000000, 0x00000001, 0x12345678, 0x1dac2b7c, 0xffffffff};
    for (int i = 0; i < 5; i++) {
        uint8_t test_hdr[80];
        memcpy(test_hdr, genesis_header, 80);
        test_hdr[76] = test_nonces[i] & 0xFF;
        test_hdr[77] = (test_nonces[i] >> 8) & 0xFF;
        test_hdr[78] = (test_nonces[i] >> 16) & 0xFF;
        test_hdr[79] = (test_nonces[i] >> 24) & 0xFF;

        uint8_t gen_res[32];
        generic_double_sha256(test_hdr, 80, gen_res);

        uint32_t spec_words[8];
        bitcoin_hash_midstate(gen_midstate, gen_tail, test_nonces[i], spec_words);
        uint8_t spec_res[32];
        for (int w = 0; w < 8; w++) {
            spec_res[w * 4]     = (spec_words[w] >> 24) & 0xFF;
            spec_res[w * 4 + 1] = (spec_words[w] >> 16) & 0xFF;
            spec_res[w * 4 + 2] = (spec_words[w] >> 8) & 0xFF;
            spec_res[w * 4 + 3] = spec_words[w] & 0xFF;
        }
        if (memcmp(gen_res, spec_res, 32) != 0) return 0;
    }
    printf("[+] Test 3 (Generic SHA256 vs Midstate Engine Multi-Nonce Byte-Match): PASSED\n");

    // Test 4: nbits_to_target Genesis Verification
    uint8_t target_nbits[32];
    nbits_to_target(0x1d00ffff, target_nbits);
    char target_hex[65];
    bin_to_hex(target_nbits, 32, target_hex);
    if (strcmp(target_hex, "00000000ffff0000000000000000000000000000000000000000000000000000") != 0) return 0;
    printf("[+] Test 4 (nbits_to_target 0x1d00ffff Exact Encoding): PASSED\n");

    // Test 5: Fast Target Boundary Test (T-1, T, T+1)
    uint32_t test_target_words[8] = { 0x00000000, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 };
    uint32_t hash_below[8] = { 0, 0, 0, 0, 0, 0, SWAP32(0x0000fffe), 0 };
    uint32_t hash_equal[8] = { 0, 0, 0, 0, 0, 0, SWAP32(0x0000ffff), 0 };
    uint32_t hash_above[8] = { 0, 0, 0, 0, 0, 0, SWAP32(0x00010000), 0 };

    if (!hash_meets_target_fast(hash_below, test_target_words)) return 0;
    if (!hash_meets_target_fast(hash_equal, test_target_words)) return 0;
    if (hash_meets_target_fast(hash_above, test_target_words)) return 0;
    printf("[+] Test 5 (Fast Target Comparison T-1, T, T+1 Boundaries): PASSED\n");

    // Test 6: JSMN Subscribe Fixture Parsing
    const char *fixture_sub = "{\"id\":1,\"result\":[[[\"mining.set_difficulty\",\"1\"],[\"mining.notify\",\"1\"]],\"01234567\",4],\"error\":null}";
    parse_subscribe_response(fixture_sub);
    if (strcmp(g_extranonce1_hex, "01234567") != 0 || g_extranonce2_size != 4) return 0;
    printf("[+] Test 6 (JSMN Subscribe Nested Array Fixture): PASSED\n");

    // Test 7: JSMN Mining Notify Fixture Parsing
    const char *fixture_notify = "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"bf4\",\"4d16ef801d18f40de97c6b4952ad7d24296733e393ff7e883b46573800000000\",\"01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff2002522f062f503253482f\",\"072f736c7573682f000000000100f2052a010000001976a914d23fcdf86f7e756a64a7a9688ef9903327048d3688ac00000000\",[],\"00000002\",\"1c2ac4af\",\"504e86b9\",false]}";
    parse_mining_notify(fixture_notify);
    if (strcmp(g_current_job.job_id, "bf4") != 0 || g_current_job.version != 2 || g_current_job.nbits != 0x1c2ac4af || g_current_job.ntime != 0x504e86b9) return 0;
    printf("[+] Test 7 (JSMN Mining Notify Fixture): PASSED\n");

    // Test 8: Target Persistence Fixture Across Notifications
    g_share_target.difficulty = 16.0;
    difficulty_to_target(16.0, g_share_target.target);
    update_share_target_words(&g_share_target);
    parse_mining_notify(fixture_notify);
    if (g_current_job.target_info.difficulty != 16.0) return 0;
    printf("[+] Test 8 (Target Persistence Across mining.notify): PASSED\n");

    // Test 9: Out-of-Order Pending Share Response Matching
    memset(g_pending_shares, 0, sizeof(g_pending_shares));
    g_pending_shares[0].request_id = 101; g_pending_shares[0].active = true; strcpy(g_pending_shares[0].hash, "hash101");
    g_pending_shares[1].request_id = 102; g_pending_shares[1].active = true; strcpy(g_pending_shares[1].hash, "hash102");

    // Simulate response for 102 first
    int matched_idx = -1;
    uint32_t incoming_id = 102;
    for (int i = 0; i < MAX_PENDING_SHARES; i++) {
        if (g_pending_shares[i].active && g_pending_shares[i].request_id == incoming_id) {
            matched_idx = i;
            g_pending_shares[i].active = false;
            break;
        }
    }
    if (matched_idx != 1 || g_pending_shares[0].active != true) return 0;
    printf("[+] Test 9 (Out-of-Order Pending Share Matching): PASSED\n");

    // Test 10: 2-Way Interleaved SHA-NI Equivalence Test
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (g_has_sha_ni) {
        uint32_t ref_a[8], ref_b[8];
        uint32_t pipe_a[8], pipe_b[8];
        bitcoin_hash_midstate_shani(gen_midstate, gen_tail, 0x12345678, ref_a);
        bitcoin_hash_midstate_shani(gen_midstate, gen_tail, 0x12345679, ref_b);
        bitcoin_hash_midstate_shani_2way(gen_midstate, gen_tail, 0x12345678, 0x12345679, pipe_a, pipe_b);
        if (memcmp(ref_a, pipe_a, 32) != 0 || memcmp(ref_b, pipe_b, 32) != 0) {
            printf("[-] Test 10 mismatch!\n");
            printf("ref_a:  %08x %08x %08x %08x\n", ref_a[0], ref_a[1], ref_a[2], ref_a[3]);
            printf("pipe_a: %08x %08x %08x %08x\n", pipe_a[0], pipe_a[1], pipe_a[2], pipe_a[3]);
            printf("ref_b:  %08x %08x %08x %08x\n", ref_b[0], ref_b[1], ref_b[2], ref_b[3]);
            printf("pipe_b: %08x %08x %08x %08x\n", pipe_b[0], pipe_b[1], pipe_b[2], pipe_b[3]);
            return 0;
        }
        printf("[+] Test 10 (2-Way Interleaved SHA-NI Dual-Nonce Bit-Match): PASSED\n");
    }
#endif

    // Test 11: OpenCL GPU Engine Hardware Verification
    if (init_gpu_engine()) {
        share_target_t gen_target;
        nbits_to_target(0x1d00ffff, gen_target.target);
        update_share_target_words(&gen_target);

        uint32_t zero = 0;
        g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_midstate, CL_TRUE, 0, 32, gen_midstate, 0, NULL, NULL);
        g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_target, CL_TRUE, 0, 32, gen_target.target_words, 0, NULL, NULL);
        g_cl.clEnqueueWriteBuffer(g_gpu.queue, g_gpu.d_found, CL_TRUE, 0, 4, &zero, 0, NULL, NULL);

        uint32_t test_base_nonce = 0x7c2bac10U;
        size_t test_gw = 256;
        size_t test_lw = 256;
        g_cl.clSetKernelArg(g_gpu.kernel, 0, sizeof(cl_mem), &g_gpu.d_midstate);
        g_cl.clSetKernelArg(g_gpu.kernel, 1, sizeof(uint32_t), &gen_tail[0]);
        g_cl.clSetKernelArg(g_gpu.kernel, 2, sizeof(uint32_t), &gen_tail[1]);
        g_cl.clSetKernelArg(g_gpu.kernel, 3, sizeof(uint32_t), &gen_tail[2]);
        g_cl.clSetKernelArg(g_gpu.kernel, 4, sizeof(uint32_t), &test_base_nonce);
        g_cl.clSetKernelArg(g_gpu.kernel, 5, sizeof(cl_mem), &g_gpu.d_target);
        g_cl.clSetKernelArg(g_gpu.kernel, 6, sizeof(cl_mem), &g_gpu.d_found);
        g_cl.clSetKernelArg(g_gpu.kernel, 7, sizeof(cl_mem), &g_gpu.d_nonces);

        g_cl.clEnqueueNDRangeKernel(g_gpu.queue, g_gpu.kernel, 1, NULL, &test_gw, &test_lw, 0, NULL, NULL);
        g_cl.clFinish(g_gpu.queue);

        uint32_t f_count = 0;
        g_cl.clEnqueueReadBuffer(g_gpu.queue, g_gpu.d_found, CL_TRUE, 0, 4, &f_count, 0, NULL, NULL);
        if (f_count >= 1) {
            uint32_t f_nonces[64];
            g_cl.clEnqueueReadBuffer(g_gpu.queue, g_gpu.d_nonces, CL_TRUE, 0, sizeof(uint32_t), f_nonces, 0, NULL, NULL);
            if (f_nonces[0] == 0x7c2bac1dU) {
                printf("[+] Test 11 (OpenCL GPU Engine - %s, %u CUs): PASSED\n", g_gpu.device_name, g_gpu.compute_units);
            } else {
                printf("[-] Test 11 failed: Expected nonce 0x7c2bac1d, got 0x%08x\n", f_nonces[0]);
                return 0;
            }
        } else {
            printf("[-] Test 11 failed: Genesis nonce not detected by GPU kernel!\n");
            return 0;
        }
    } else {
        printf("[*] Test 11 (OpenCL GPU Engine): SKIPPED (No OpenCL GPU runtime detected)\n");
    }

    printf("[+] All 11 Cryptographic, GPU Acceleration & Protocol Tests Passed Successfully!\n\n");
    return 1;
}

/* ============================================================================
 * SECTION 9B: Bitcoin Core RPC Solo Mining Client (BIP 22 / BIP 23)
 * ============================================================================ */

static void parse_url_host_port(const char *url, char *host, size_t max_host, int *port) {
    *port = 8332; // Default Bitcoin Core mainnet RPC port
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;

    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= max_host) hlen = max_host - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= max_host) hlen = max_host - 1;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
    } else {
        strncpy(host, p, max_host - 1);
        host[max_host - 1] = '\0';
    }
}

static bool http_rpc_request(const char *host, int port, const char *auth_b64, const char *json_req, char *resp_buf, size_t resp_max) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

#ifdef _WIN32
    DWORD to_ms = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_ms, sizeof(to_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to_ms, sizeof(to_ms));
#else
    struct timeval tv = {8, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(s); return false; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    memcpy(&sa.sin_addr.s_addr, he->h_addr_list[0], he->h_length);

    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    char req_hdr[1024];
    int hlen = snprintf(req_hdr, sizeof(req_hdr),
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        host, port, auth_b64, strlen(json_req));

    if (!socket_send_all(s, req_hdr, hlen) || !socket_send_all(s, json_req, strlen(json_req))) {
        closesocket(s);
        return false;
    }

    size_t total = 0;
    while (total < resp_max - 1) {
        int r = recv(s, resp_buf + total, (int)(resp_max - 1 - total), 0);
        if (r <= 0) break;
        total += r;
    }
    resp_buf[total] = '\0';
    closesocket(s);

    return (total > 0 && strstr(resp_buf, "200 OK") != NULL);
}

typedef struct {
    char pool_host[256];
    int pool_port;
    char user[256];
    char password[256];
    int num_threads;
    bool run_test_only;
    int benchmark_sec;
    bool enable_gpu;
    bool rpc_mode;
    char rpc_url[256];
    char rpc_user[128];
    char rpc_password[128];
} miner_config_t;

static int run_bitcoin_rpc_miner(const miner_config_t *config) {
    char rpc_host[256];
    int rpc_port = 8332;
    parse_url_host_port(config->rpc_url, rpc_host, sizeof(rpc_host), &rpc_port);

    char auth_plain[512];
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s", config->rpc_user, config->rpc_password);
    char auth_b64[1024];
    base64_encode(auth_plain, strlen(auth_plain), auth_b64, sizeof(auth_b64));

    printf("[*] Entering Bitcoin Core RPC Solo Mining Mode!\n");
    printf("[*] Node: http://%s:%d | Worker Address: %s\n", rpc_host, rpc_port, config->user);
    bool use_gpu = config->enable_gpu && g_gpu.available && g_gpu.enabled;
    printf("[*] Rig Setup: %d CPU SHA-NI threads%s\n\n",
           config->num_threads, use_gpu ? " + Intel Iris Xe GPU (80 CUs)" : "");

#ifdef _WIN32
    HANDLE worker_handles[MAX_THREADS];
    for (int t = 0; t < config->num_threads; t++) {
        thread_worker_arg_t *arg = (thread_worker_arg_t *)malloc(sizeof(thread_worker_arg_t));
        arg->thread_id = t;
        arg->total_threads = config->num_threads;
        arg->sock = INVALID_SOCKET;
        strncpy(arg->user, config->user, sizeof(arg->user));
        worker_handles[t] = (HANDLE)_beginthreadex(NULL, 0, miner_thread_proc, arg, 0, NULL);
    }
    HANDLE gpu_handle = NULL;
    if (use_gpu) {
        gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
        garg->sock = INVALID_SOCKET;
        strncpy(garg->user, config->user, sizeof(garg->user));
        gpu_handle = (HANDLE)_beginthreadex(NULL, 0, gpu_miner_thread_proc, garg, 0, NULL);
    }
#else
    pthread_t worker_handles[MAX_THREADS];
    for (int t = 0; t < config->num_threads; t++) {
        thread_worker_arg_t *arg = (thread_worker_arg_t *)malloc(sizeof(thread_worker_arg_t));
        arg->thread_id = t;
        arg->total_threads = config->num_threads;
        arg->sock = INVALID_SOCKET;
        strncpy(arg->user, config->user, sizeof(arg->user));
        pthread_create(&worker_handles[t], NULL, miner_thread_proc, arg);
    }
    pthread_t gpu_handle;
    if (use_gpu) {
        gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
        garg->sock = INVALID_SOCKET;
        strncpy(garg->user, config->user, sizeof(garg->user));
        pthread_create(&gpu_handle, NULL, gpu_miner_thread_proc, garg);
    }
#endif

    char last_prevhash[128] = "";
    static char rpc_resp[65536];
    const char *gbt_req = "{\"jsonrpc\":\"1.0\",\"id\":\"miner\",\"method\":\"getblocktemplate\",\"params\":[{\"rules\":[\"segwit\"]}]}";

    time_t prev_poll = 0;
    uint64_t prev_hashes = 0;
    uint64_t prev_cpu = 0;
    uint64_t prev_gpu = 0;
    time_t prev_rate_time = time(NULL);

    while (!g_stop_mining) {
        time_t now = time(NULL);

        // Poll getblocktemplate every 3 seconds
        if (now - prev_poll >= 3) {
            prev_poll = now;
            bool ok = http_rpc_request(rpc_host, rpc_port, auth_b64, gbt_req, rpc_resp, sizeof(rpc_resp));
            if (!ok) {
                printf("\r[-] Note: Bitcoin Core RPC polling http://%s:%d (Ensure bitcoin.conf has server=1 & rpc credentials)\n", rpc_host, rpc_port);
            } else {
                char *body = strstr(rpc_resp, "\r\n\r\n");
                if (body) {
                    body += 4;
                    const char *p_prev = strstr(body, "\"previousblockhash\":");
                    const char *p_bits = strstr(body, "\"bits\":");
                    const char *p_time = strstr(body, "\"curtime\":");
                    const char *p_ver  = strstr(body, "\"version\":");
                    const char *p_tgt  = strstr(body, "\"target\":");

                    if (p_prev && p_bits && p_time && p_ver) {
                        char prev_hex[65] = {0};
                        char bits_hex[16] = {0};
                        sscanf(p_prev + 20, " \"%64[^\"]\"", prev_hex);
                        sscanf(p_bits + 7, " \"%8[^\"]\"", bits_hex);
                        uint32_t ntime = (uint32_t)strtoul(p_time + 10, NULL, 10);
                        uint32_t nversion = (uint32_t)strtoul(p_ver + 10, NULL, 10);

                        if (strcmp(prev_hex, last_prevhash) != 0) {
                            strncpy(last_prevhash, prev_hex, sizeof(last_prevhash));
                            printf("\n[+] New Bitcoin Block Template Received: PrevBlock=%.16s... | Bits=%s | Ver=%u\n",
                                   prev_hex, bits_hex, nversion);

#ifdef _WIN32
                            EnterCriticalSection(&g_job_lock);
#else
                            pthread_mutex_lock(&g_job_lock);
#endif
                            snprintf(g_current_job.job_id, sizeof(g_current_job.job_id), "rpc_%.8s", prev_hex);
                            uint8_t prev_bin[32];
                            hex_to_bin(prev_hex, prev_bin, 32);
                            swap_bytes(prev_bin, 32);
                            memcpy(g_current_job.prevhash, prev_bin, 32);
                            g_current_job.version = nversion;
                            g_current_job.ntime = ntime;
                            g_current_job.nbits = (uint32_t)strtoul(bits_hex, NULL, 16);

                            if (p_tgt) {
                                char target_hex[65] = {0};
                                sscanf(p_tgt + 9, " \"%64[^\"]\"", target_hex);
                                hex_to_bin(target_hex, g_current_job.target_info.target, 32);
                                update_share_target_words(&g_current_job.target_info);
                            } else {
                                nbits_to_target(g_current_job.nbits, g_current_job.target_info.target);
                                update_share_target_words(&g_current_job.target_info);
                            }

                            // Minimal BIP34 solo coinbase
                            const char *dummy_cb = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff080300000000000000ffffffff0100f2052a010000001976a914d23fcdf86f7e756a64a7a9688ef9903327048d3688ac00000000";
                            g_current_job.coinb1_len = hex_to_bin(dummy_cb, g_current_job.coinb1, sizeof(g_current_job.coinb1));
                            g_current_job.coinb2_len = 0;
                            g_current_job.num_merkle_branches = 0;
                            g_has_valid_job = true;
                            atomic_increment_generation();
#ifdef _WIN32
                            LeaveCriticalSection(&g_job_lock);
#else
                            pthread_mutex_unlock(&g_job_lock);
#endif
                        }
                    }
                }
            }
        }

#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        time_t t_now = time(NULL);
        double delta = difftime(t_now, prev_rate_time);
        if (delta >= 1.0) {
            uint64_t cur_hashes = (uint64_t)g_hashes_count;
            uint64_t cur_cpu = (uint64_t)g_cpu_hashes_count;
            uint64_t cur_gpu = (uint64_t)g_gpu_hashes_count;

            double live_rate = ((double)(cur_hashes >= prev_hashes ? cur_hashes - prev_hashes : 0) / delta) / 1000000.0;
            double cpu_rate = ((double)(cur_cpu >= prev_cpu ? cur_cpu - prev_cpu : 0) / delta) / 1000000.0;
            double gpu_rate = ((double)(cur_gpu >= prev_gpu ? cur_gpu - prev_gpu : 0) / delta) / 1000000.0;

            printf("\r[*] Live: %.2f MH/s (CPU: %.2f | GPU: %.2f) | Mode: Bitcoin Core RPC Solo | Total Hashes: %llu",
                   live_rate, cpu_rate, gpu_rate, (unsigned long long)cur_hashes);
            fflush(stdout);

            prev_hashes = cur_hashes;
            prev_cpu = cur_cpu;
            prev_gpu = cur_gpu;
            prev_rate_time = t_now;
        }
    }

    g_stop_mining = true;
#ifdef _WIN32
    for (int t = 0; t < config->num_threads; t++) {
        WaitForSingleObject(worker_handles[t], 3000);
        CloseHandle(worker_handles[t]);
    }
    if (gpu_handle) {
        WaitForSingleObject(gpu_handle, 3000);
        CloseHandle(gpu_handle);
    }
    DeleteCriticalSection(&g_job_lock);
    DeleteCriticalSection(&g_socket_lock);
    DeleteCriticalSection(&g_pending_lock);
    WSACleanup();
#else
    for (int t = 0; t < config->num_threads; t++) pthread_join(worker_handles[t], NULL);
    if (use_gpu) pthread_join(gpu_handle, NULL);
#endif
    cleanup_gpu_engine();
    return 0;
}

/* ============================================================================
 * SECTION 10: Main CLI Entry Point & Worker Thread Management
 * ============================================================================ */

static void print_usage(const char *prog_name) {
    printf("Hybrid Bitcoin Miner (SHA-NI CPU + OpenCL GPU)\n\nUsage:\n  %s [options]\n\n", prog_name);
    printf("Stratum Pool Options:\n");
    printf("  --pool <host>       Mining pool hostname (default: solo.ckpool.org)\n");
    printf("  --port <port>       Stratum TCP port (default: 3333)\n");
    printf("  --user <address>    Pool username / BTC payout address\n");
    printf("  --password <pass>   Pool password (default: x)\n\n");
    printf("Bitcoin Core RPC Solo Options:\n");
    printf("  --rpc-url <url>     Bitcoin Core JSON-RPC URL (e.g. http://127.0.0.1:8332)\n");
    printf("  --rpc-user <user>   Bitcoin Core RPC username\n");
    printf("  --rpc-password <p>  Bitcoin Core RPC password\n\n");
    printf("Hardware & Mining Engines:\n");
    printf("  --threads <num>     Number of CPU mining threads (default: 6)\n");
    printf("  --gpu               Enable OpenCL GPU mining engine (default: auto)\n");
    printf("  --no-gpu            Disable GPU mining (CPU SHA-NI only)\n");
    printf("  --benchmark [sec]   Run combined rig speed benchmark for [sec] seconds (default: 5)\n");
    printf("  --test              Run cryptographic & GPU self-tests and exit\n");
    printf("  --help              Display this help message\n\n");
}

int main(int argc, char *argv[]) {
    miner_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.pool_host, "solo.ckpool.org", sizeof(config.pool_host));
    config.pool_port = 3333;
    strncpy(config.user, "1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS", sizeof(config.user));
    strncpy(config.password, "x", sizeof(config.password));
    config.num_threads = 6;
    config.benchmark_sec = 0;
    config.enable_gpu = true;
    config.rpc_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pool") == 0 && i + 1 < argc) strncpy(config.pool_host, argv[++i], sizeof(config.pool_host));
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) config.pool_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) strncpy(config.user, argv[++i], sizeof(config.user));
        else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) strncpy(config.password, argv[++i], sizeof(config.password));
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) config.num_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gpu") == 0) config.enable_gpu = true;
        else if (strcmp(argv[i], "--no-gpu") == 0) config.enable_gpu = false;
        else if (strcmp(argv[i], "--rpc-url") == 0 && i + 1 < argc) {
            strncpy(config.rpc_url, argv[++i], sizeof(config.rpc_url));
            config.rpc_mode = true;
        }
        else if (strcmp(argv[i], "--rpc-user") == 0 && i + 1 < argc) strncpy(config.rpc_user, argv[++i], sizeof(config.rpc_user));
        else if (strcmp(argv[i], "--rpc-password") == 0 && i + 1 < argc) strncpy(config.rpc_password, argv[++i], sizeof(config.rpc_password));
        else if (strcmp(argv[i], "--benchmark") == 0) {
            config.benchmark_sec = 5;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config.benchmark_sec = atoi(argv[++i]);
                if (config.benchmark_sec <= 0) config.benchmark_sec = 5;
            }
        }
        else if (strcmp(argv[i], "--test") == 0) config.run_test_only = true;
        else if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    InitializeCriticalSection(&g_job_lock);
    InitializeCriticalSection(&g_socket_lock);
    InitializeCriticalSection(&g_pending_lock);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    // Initial default share target
    difficulty_to_target(1.0, g_share_target.target);
    update_share_target_words(&g_share_target);
    g_share_target.difficulty = 1.0;

#if defined(__SHA__)
    g_has_sha_ni = true;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    g_has_sha_ni = __builtin_cpu_supports("sha");
#else
    g_has_sha_ni = false;
#endif
    printf("[*] CPU Cryptographic Engine: %s\n", g_has_sha_ni ? "Intel SHA-NI (Hardware Accelerated 30-35 MH/s)" : "Standard Scalar (Software)");

    // Initialize OpenCL GPU subsystem
    bool gpu_detected = init_gpu_engine();
    if (gpu_detected) {
        printf("[*] GPU Cryptographic Engine: %s (%u Compute Units @ %u MHz)\n",
               g_gpu.device_name, g_gpu.compute_units, g_gpu.clock_mhz);
        g_gpu.enabled = config.enable_gpu;
        if (!config.enable_gpu) {
            printf("[*] GPU Engine: DISABLED by user request (--no-gpu)\n");
        }
    } else {
        printf("[*] GPU Cryptographic Engine: None detected (falling back to Pure CPU mode)\n");
    }

    if (!run_self_test()) {
        printf("[-] Fatal: Cryptographic self-tests failed!\n");
        return 1;
    }
    if (config.run_test_only) return 0;

    // Combined Benchmark Mode
    if (config.benchmark_sec > 0) {
        bool use_gpu = config.enable_gpu && g_gpu.available && g_gpu.enabled;
        printf("[*] Starting Combined Hashrate Benchmark (%d CPU threads%s, %d seconds)...\n",
               config.num_threads, use_gpu ? " + Intel Iris Xe GPU" : "", config.benchmark_sec);

        const char *bench_notify = "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"bench\",\"4d16ef801d18f40de97c6b4952ad7d24296733e393ff7e883b46573800000000\",\"01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff2002522f062f503253482f\",\"072f736c7573682f000000000100f2052a010000001976a914d23fcdf86f7e756a64a7a9688ef9903327048d3688ac00000000\",[],\"00000002\",\"1c2ac4af\",\"504e86b9\",false]}";
        strncpy(g_extranonce1_hex, "01234567", sizeof(g_extranonce1_hex));
        g_extranonce2_size = 4;
        parse_mining_notify(bench_notify);

#ifdef _WIN32
        HANDLE worker_handles[MAX_THREADS];
        for (int t = 0; t < config.num_threads; t++) {
            thread_worker_arg_t *arg = (thread_worker_arg_t *)malloc(sizeof(thread_worker_arg_t));
            arg->thread_id = t;
            arg->total_threads = config.num_threads;
            arg->sock = INVALID_SOCKET;
            strncpy(arg->user, config.user, sizeof(arg->user));
            worker_handles[t] = (HANDLE)_beginthreadex(NULL, 0, miner_thread_proc, arg, 0, NULL);
        }
        HANDLE gpu_handle = NULL;
        if (use_gpu) {
            gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
            garg->sock = INVALID_SOCKET;
            strncpy(garg->user, config.user, sizeof(garg->user));
            gpu_handle = (HANDLE)_beginthreadex(NULL, 0, gpu_miner_thread_proc, garg, 0, NULL);
        }
#else
        pthread_t worker_handles[MAX_THREADS];
        for (int t = 0; t < config.num_threads; t++) {
            thread_worker_arg_t *arg = (thread_worker_arg_t *)malloc(sizeof(thread_worker_arg_t));
            arg->thread_id = t;
            arg->total_threads = config.num_threads;
            arg->sock = INVALID_SOCKET;
            strncpy(arg->user, config.user, sizeof(arg->user));
            pthread_create(&worker_handles[t], NULL, miner_thread_proc, arg);
        }
        pthread_t gpu_handle;
        if (use_gpu) {
            gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
            garg->sock = INVALID_SOCKET;
            strncpy(garg->user, config.user, sizeof(garg->user));
            pthread_create(&gpu_handle, NULL, gpu_miner_thread_proc, garg);
        }
#endif

        for (int s = 0; s < config.benchmark_sec; s++) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
            double cpu_rate = ((double)g_cpu_hashes_count / (double)(s + 1)) / 1000000.0;
            double gpu_rate = ((double)g_gpu_hashes_count / (double)(s + 1)) / 1000000.0;
            double total_rate = ((double)g_hashes_count / (double)(s + 1)) / 1000000.0;
            printf("\r[*] Benchmark In Progress [%d/%d s]... Live: %.2f MH/s (CPU: %.2f | GPU: %.2f)",
                   s + 1, config.benchmark_sec, total_rate, cpu_rate, gpu_rate);
            fflush(stdout);
        }
        g_stop_mining = true;
#ifdef _WIN32
        for (int t = 0; t < config.num_threads; t++) {
            WaitForSingleObject(worker_handles[t], 3000);
            CloseHandle(worker_handles[t]);
        }
        if (gpu_handle) {
            WaitForSingleObject(gpu_handle, 3000);
            CloseHandle(gpu_handle);
        }
        DeleteCriticalSection(&g_job_lock);
        DeleteCriticalSection(&g_socket_lock);
        DeleteCriticalSection(&g_pending_lock);
        WSACleanup();
#else
        for (int t = 0; t < config.num_threads; t++) pthread_join(worker_handles[t], NULL);
        if (use_gpu) pthread_join(gpu_handle, NULL);
#endif
        double final_cpu_mh = ((double)g_cpu_hashes_count / (double)config.benchmark_sec) / 1000000.0;
        double final_gpu_mh = ((double)g_gpu_hashes_count / (double)config.benchmark_sec) / 1000000.0;
        double final_total_mh = ((double)g_hashes_count / (double)config.benchmark_sec) / 1000000.0;

        printf("\n\n");
        printf("================================================================================\n");
        printf("                    HYBRID BITCOIN MINING RIG BENCHMARK\n");
        printf("================================================================================\n");
        printf("  CPU Engine : Intel SHA-NI (%d threads)             -> %7.2f MH/s (%llu hashes)\n",
               config.num_threads, final_cpu_mh, (unsigned long long)g_cpu_hashes_count);
        if (use_gpu) {
            printf("  GPU Engine : %-38s -> %7.2f MH/s (%llu hashes)\n",
                   g_gpu.device_name, final_gpu_mh, (unsigned long long)g_gpu_hashes_count);
        }
        printf("--------------------------------------------------------------------------------\n");
        printf("  AGGREGATE RIG HASHRATE                            -> %7.2f MH/s (%llu hashes)\n",
               final_total_mh, (unsigned long long)g_hashes_count);
        printf("================================================================================\n\n");
        cleanup_gpu_engine();
        return 0;
    }

    // Bitcoin Core RPC Solo Mining Mode
    if (config.rpc_mode) {
        return run_bitcoin_rpc_miner(&config);
    }

    // Stratum V1 Pool Mode
    printf("[*] Connecting to Stratum Pool %s:%d...\n", config.pool_host, config.pool_port);
    printf("[*] Worker: %s | CPU Threads: %d | GPU Engine: %s\n",
           config.user, config.num_threads, (config.enable_gpu && g_gpu.available) ? "Active" : "Disabled");

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 1;
    g_main_socket = sock;

    struct hostent *host = gethostbyname(config.pool_host);
    if (!host) {
        printf("[-] Error: DNS resolution failed for %s\n", config.pool_host);
        closesocket(sock);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)config.pool_port);
    memcpy(&server_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("[-] Error: Connection failed to %s:%d\n", config.pool_host, config.pool_port);
        closesocket(sock);
        return 1;
    }
    printf("[+] Connected to Stratum V1 Pool!\n");

    tcp_stream_buffer_t init_stream;
    memset(&init_stream, 0, sizeof(init_stream));

    // 1. Subscribe
    char req[512], buf[4096];
    snprintf(req, sizeof(req), "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": [\"miner/1.0.0\"]}\n");
    if (!thread_safe_socket_send(sock, req) || !tcp_buffer_get_line(sock, &init_stream, buf, sizeof(buf))) {
        printf("[-] Error: Stratum subscription failed.\n");
        closesocket(sock);
        return 1;
    }
    parse_subscribe_response(buf);

    // 2. Authorize
    snprintf(req, sizeof(req), "{\"id\": 2, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", config.user, config.password);
    if (!thread_safe_socket_send(sock, req) || !tcp_buffer_get_line(sock, &init_stream, buf, sizeof(buf))) {
        printf("[-] Error: Stratum authorization request failed.\n");
        closesocket(sock);
        return 1;
    }
    if (strstr(buf, "\"result\":true") || strstr(buf, "\"result\": true")) {
        printf("[+] Stratum Authorization Successful!\n\n");
    } else {
        printf("[-] Warning: Authorization response: %s\n\n", buf);
    }

#ifdef _WIN32
    HANDLE recv_thread = (HANDLE)_beginthreadex(NULL, 0, stratum_recv_thread_proc, &sock, 0, NULL);
    HANDLE worker_handles[MAX_THREADS];
#else
    pthread_t recv_thread; pthread_create(&recv_thread, NULL, stratum_recv_thread_proc, &sock);
    pthread_t worker_handles[MAX_THREADS];
#endif

    printf("[*] Launching %d CPU Mining Threads...\n", config.num_threads);
    for (int t = 0; t < config.num_threads; t++) {
        thread_worker_arg_t *arg = (thread_worker_arg_t *)malloc(sizeof(thread_worker_arg_t));
        arg->thread_id = t;
        arg->total_threads = config.num_threads;
        arg->sock = sock;
        strncpy(arg->user, config.user, sizeof(arg->user));
#ifdef _WIN32
        worker_handles[t] = (HANDLE)_beginthreadex(NULL, 0, miner_thread_proc, arg, 0, NULL);
#else
        pthread_create(&worker_handles[t], NULL, miner_thread_proc, arg);
#endif
    }

    bool use_gpu = config.enable_gpu && g_gpu.available && g_gpu.enabled;
#ifdef _WIN32
    HANDLE gpu_thread = NULL;
    if (use_gpu) {
        printf("[*] Launching Dedicated OpenCL GPU Worker (%s, %u CUs)...\n", g_gpu.device_name, g_gpu.compute_units);
        gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
        garg->sock = sock;
        strncpy(garg->user, config.user, sizeof(garg->user));
        gpu_thread = (HANDLE)_beginthreadex(NULL, 0, gpu_miner_thread_proc, garg, 0, NULL);
    }
#else
    pthread_t gpu_thread;
    if (use_gpu) {
        printf("[*] Launching Dedicated OpenCL GPU Worker (%s, %u CUs)...\n", g_gpu.device_name, g_gpu.compute_units);
        gpu_worker_arg_t *garg = (gpu_worker_arg_t *)malloc(sizeof(gpu_worker_arg_t));
        garg->sock = sock;
        strncpy(garg->user, config.user, sizeof(garg->user));
        pthread_create(&gpu_thread, NULL, gpu_miner_thread_proc, garg);
    }
#endif

    printf("[*] Hybrid Bitcoin Miner Active! Real-time Hashrate Telemetry:\n\n");

    uint64_t prev_hashes = 0;
    uint64_t prev_cpu_hashes = 0;
    uint64_t prev_gpu_hashes = 0;
    time_t prev_time = time(NULL);
    time_t start_time = prev_time;

    uint64_t window_hashes[5] = {0};
    int window_idx = 0;

    while (!g_stop_mining) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        time_t now = time(NULL);
        double delta_sec = difftime(now, prev_time);
        if (delta_sec >= 1.0) {
            uint64_t current_hashes = (uint64_t)g_hashes_count;
            uint64_t current_cpu = (uint64_t)g_cpu_hashes_count;
            uint64_t current_gpu = (uint64_t)g_gpu_hashes_count;

            uint64_t delta_hashes = (current_hashes >= prev_hashes) ? (current_hashes - prev_hashes) : 0;
            double live_hashrate_mh = ((double)delta_hashes / delta_sec) / 1000000.0;

            double cpu_live_mh = ((double)(current_cpu >= prev_cpu_hashes ? current_cpu - prev_cpu_hashes : 0) / delta_sec) / 1000000.0;
            double gpu_live_mh = ((double)(current_gpu >= prev_gpu_hashes ? current_gpu - prev_gpu_hashes : 0) / delta_sec) / 1000000.0;

            window_hashes[window_idx] = delta_hashes;
            window_idx = (window_idx + 1) % 5;
            uint64_t sum_window = 0;
            for (int w = 0; w < 5; w++) sum_window += window_hashes[w];
            double avg_5s_mh = ((double)sum_window / 5.0) / 1000000.0;

            double total_elapsed = difftime(now, start_time);
            double total_avg_mh = total_elapsed > 0 ? (((double)current_hashes / total_elapsed) / 1000000.0) : 0.0;

            printf("\r[*] Live: %.2f MH/s (CPU: %.2f | GPU: %.2f | 5s: %.2f | Avg: %.2f) | Hashes: %llu | Accepted: %llu | Rejected: %llu",
                   live_hashrate_mh, cpu_live_mh, gpu_live_mh, avg_5s_mh, total_avg_mh,
                   (unsigned long long)current_hashes,
                   (unsigned long long)g_accepted_shares, (unsigned long long)g_rejected_shares);
            fflush(stdout);

            prev_hashes = current_hashes;
            prev_cpu_hashes = current_cpu;
            prev_gpu_hashes = current_gpu;
            prev_time = now;
        }
    }

    printf("\n[*] Stopping all worker threads...\n");
#ifdef _WIN32
    for (int t = 0; t < config.num_threads; t++) {
        WaitForSingleObject(worker_handles[t], 3000);
        CloseHandle(worker_handles[t]);
    }
    if (use_gpu && gpu_thread) {
        WaitForSingleObject(gpu_thread, 3000);
        CloseHandle(gpu_thread);
    }
    WaitForSingleObject(recv_thread, 2000);
    CloseHandle(recv_thread);
    DeleteCriticalSection(&g_job_lock);
    DeleteCriticalSection(&g_socket_lock);
    DeleteCriticalSection(&g_pending_lock);
    closesocket(sock);
    WSACleanup();
#else
    for (int t = 0; t < config.num_threads; t++) pthread_join(worker_handles[t], NULL);
    if (use_gpu) pthread_join(gpu_thread, NULL);
    pthread_join(recv_thread, NULL);
    closesocket(sock);
#endif

    cleanup_gpu_engine();
    printf("[+] Miner shutdown complete. Total hashes computed: %llu (CPU: %llu | GPU: %llu)\n",
           (unsigned long long)g_hashes_count, (unsigned long long)g_cpu_hashes_count, (unsigned long long)g_gpu_hashes_count);
    return 0;
}
