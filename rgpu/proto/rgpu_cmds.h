/* rgpu command argument layouts + a minimal batch writer/reader.
 *
 * Args for the subset of opcodes the Phase-1/2 loopback implements end-to-end.
 * Little-endian, packed. The frontend (serializing device) writes these; the
 * renderer (rgpu-renderd) reads them and executes on a real device. */
#ifndef RGPU_CMDS_H
#define RGPU_CMDS_H
#include "rgpu_protocol.h"
#include <string.h>

#pragma pack(push, 1)
typedef struct { uint32_t width, height, format; } rgpu_args_create_texture2d; /* handle in record */
typedef struct { float rgba[4]; } rgpu_args_clear_rtv;                          /* handle = target */
typedef struct { uint32_t _reserved; } rgpu_args_present;                       /* handle = frame src */
#pragma pack(pop)

/* Tiny append-only batch writer over a caller buffer. */
typedef struct { unsigned char *buf; uint32_t cap, len; uint32_t count; } rgpu_batch_writer;

static inline void rgpu_bw_init(rgpu_batch_writer *w, unsigned char *buf, uint32_t cap) {
    w->buf = buf; w->cap = cap; w->len = 0; w->count = 0;
}
static inline int rgpu_bw_cmd(rgpu_batch_writer *w, uint32_t op, uint32_t handle,
                              const void *args, uint32_t arg_len) {
    uint32_t need = (uint32_t)sizeof(rgpu_cmd_record) + arg_len;
    if (w->len + need > w->cap) return 0;
    rgpu_cmd_record rec; rec.op = op; rec.handle = handle; rec.arg_len = arg_len;
    memcpy(w->buf + w->len, &rec, sizeof(rec)); w->len += (uint32_t)sizeof(rec);
    if (arg_len) { memcpy(w->buf + w->len, args, arg_len); w->len += arg_len; }
    w->count++; return 1;
}

/* Reader cursor over a batch payload. */
typedef struct { const unsigned char *buf; uint32_t len, pos; } rgpu_batch_reader;
static inline void rgpu_br_init(rgpu_batch_reader *r, const unsigned char *buf, uint32_t len) {
    r->buf = buf; r->len = len; r->pos = 0;
}
static inline const rgpu_cmd_record *rgpu_br_next(rgpu_batch_reader *r, const unsigned char **args) {
    if (r->pos + sizeof(rgpu_cmd_record) > r->len) return 0;
    const rgpu_cmd_record *rec = (const rgpu_cmd_record *)(r->buf + r->pos);
    uint32_t adv = (uint32_t)sizeof(rgpu_cmd_record) + rec->arg_len;
    if (r->pos + adv > r->len) return 0;
    *args = r->buf + r->pos + sizeof(rgpu_cmd_record);
    r->pos += adv;
    return rec;
}
#endif /* RGPU_CMDS_H */
