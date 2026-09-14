#ifndef __RING_BUF_
#define __RING_BUF_

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RINGBUFFER_EMPTY=0,
    RINGBUFFER_FULL,
    /* half full is neither full nor empty */
    RINGBUFFER_HALFFULL,
}ringbuffer_status_t;


typedef struct{
    uint8_t *buffer;        /* Buffer pointer */
    uint16_t read_mirror:1; /* page 0 or 1 */
    uint16_t read_index:14; /* Read index */
    uint16_t write_mirror:1; /* page 0 or 1 */
    uint16_t write_index:14; /* Write index */
    uint16_t buffer_size;    /* Size of the buffer */
}ringbuf_t;


void ringbuffer_init(ringbuf_t *rb, uint8_t *pool, uint16_t size);
void ringbuffer_reset(ringbuf_t *rb);
uint16_t ringbuffer_put(ringbuf_t *rb,const uint8_t *data, uint16_t length);
uint16_t ringbuffer_put_force(ringbuf_t *rb,const uint8_t *data,uint16_t length);
uint16_t ringbuffer_putbyte(ringbuf_t *rb, const uint8_t data);
uint16_t ringbuffer_putbyte_force(ringbuf_t *rb, const uint8_t data);
uint16_t ringbuffer_get(ringbuf_t *rb ,uint8_t *ptr ,uint16_t length);
uint16_t ringbuffer_peek(ringbuf_t *rb, uint8_t **ptr);
uint16_t ringbuffer_getbyte(ringbuf_t *rb, uint8_t *byte);
uint16_t ringbuffer_data_len(ringbuf_t *rb);

#define ringbuffer_space_len(rb) ((rb)->buffer_size - ringbuffer_data_len(rb))

#ifdef __cplusplus
}
#endif

#endif /* __RING_BUF_ */
