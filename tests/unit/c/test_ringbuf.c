/* C4 单元测试：ringbuf（host 端编译，不依赖 ESP-IDF）。
 *
 * 覆盖：空态 / 基本读写 / 满态拒绝 / 跨页回绕 / 空间不足部分写入 / 强制覆盖（丢旧留新）。
 * 这套用例是"计算机基础"的实证：环形缓冲的下标镜像、满/空判定、边界语义都在这儿。
 */
#include <stdio.h>
#include <string.h>

#include "ringbuf.h"

static int g_fail;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static uint8_t pool[16];
static uint8_t out[32];

static void case_empty(void)
{
    ringbuf_t rb;
    ringbuffer_init(&rb, pool, 16);
    CHECK(rb.buffer_size == 16);
    CHECK(ringbuffer_data_len(&rb) == 0);
    CHECK(ringbuffer_get(&rb, out, 4) == 0);
}

static void case_basic(void)
{
    static const uint8_t a[5] = {1, 2, 3, 4, 5};
    ringbuf_t rb;
    ringbuffer_init(&rb, pool, 16);
    CHECK(ringbuffer_put(&rb, a, 5) == 5);
    CHECK(ringbuffer_data_len(&rb) == 5);
    CHECK(ringbuffer_get(&rb, out, 5) == 5);
    CHECK(memcmp(out, a, 5) == 0);
    CHECK(ringbuffer_data_len(&rb) == 0);
}

static void case_full_rejects(void)
{
    uint8_t b[16];
    ringbuf_t rb;
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)(i + 10);
    ringbuffer_init(&rb, pool, 16);
    CHECK(ringbuffer_put(&rb, b, 16) == 16);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_put(&rb, b, 1) == 0);       /* 满态：不再接受写入 */
    CHECK(ringbuffer_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, b, 16) == 0);              /* 满态读出的顺序必须正确 */
}

static void case_wraparound(void)
{
    uint8_t b[12];
    ringbuf_t rb;
    for (int i = 0; i < 12; i++) b[i] = (uint8_t)(i + 20);
    ringbuffer_init(&rb, pool, 16);
    CHECK(ringbuffer_put(&rb, b, 10) == 10);
    CHECK(ringbuffer_get(&rb, out, 10) == 10);
    /* 此时写指针贴着尾部，再写 12 字节必然跨页（回绕） */
    CHECK(ringbuffer_put(&rb, b, 12) == 12);
    CHECK(ringbuffer_data_len(&rb) == 12);
    CHECK(ringbuffer_get(&rb, out, 12) == 12);
    CHECK(memcmp(out, b, 12) == 0);
    CHECK(ringbuffer_data_len(&rb) == 0);
}

static void case_partial_put(void)
{
    uint8_t b[20];
    ringbuf_t rb;
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(i + 30);
    ringbuffer_init(&rb, pool, 16);
    /* 空间只有 16：只写入能容纳的部分并返回实际字节数（不静默丢数据） */
    CHECK(ringbuffer_put(&rb, b, 20) == 16);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, b, 16) == 0);
}

static void case_put_force_keeps_newest(void)
{
    uint8_t b[20];
    ringbuf_t rb;
    for (int i = 0; i < 20; i++) b[i] = (uint8_t)(i + 40);
    ringbuffer_init(&rb, pool, 16);
    /* 强制写入 20 字节到 16 字节缓冲：语义是"丢最旧、留最新 16 字节" */
    CHECK(ringbuffer_put_force(&rb, b, 20) == 16);
    CHECK(ringbuffer_data_len(&rb) == 16);
    CHECK(ringbuffer_get(&rb, out, 16) == 16);
    CHECK(memcmp(out, &b[4], 16) == 0);
}

int main(void)
{
    case_empty();
    case_basic();
    case_full_rejects();
    case_wraparound();
    case_partial_put();
    case_put_force_keeps_newest();

    if (g_fail) {
        printf("FAILED: %d assertion(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASS: ringbuf host unit (6 cases)\n");
    return 0;
}
