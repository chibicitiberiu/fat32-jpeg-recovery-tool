/*
 * bitstream.cpp - MSB-first bit reader with JPEG byte stuffing
 *
 * Handles FF00 escaping: FF followed by 00 is treated as a single FF data byte.
 * FF followed by D0-D7 is a restart marker.
 *
 * Feature flag: g_lenient_ff_enabled controls whether FF 01-BF are skipped.
 * FF followed by D9 is EOI.
 * FF followed by anything else non-zero is a marker (error in entropy data).
 *
 * Uses a 64-bit accumulator for fast multi-bit reads without per-bit looping.
 * Reference: libjpeg-turbo jdhuff.h BITREAD macros, jpeg-fragments JpegByteStream.
 */
#include "sdrecov.h"
#include <cstring>

bool g_lenient_ff_enabled = true;

/* Result codes from fill_buffer */
enum FillResult { FILL_OK, FILL_EOF, FILL_MARKER };

struct BitStream {
    const uint8_t *data;
    size_t         len;
    size_t         pos;         /* current byte position (including stuffed bytes) */
    uint64_t       buf;         /* bit accumulator, MSB-aligned */
    int            avail;       /* valid bits in buf (counted from MSB) */
    bool           hit_marker;  /* set when we encounter a non-RST marker */
    uint8_t        marker_byte; /* the marker byte (Dx for RST, D9 for EOI, etc.) */
};

void bs_init(BitStream &bs, const uint8_t *data, size_t len)
{
    bs.data = data;
    bs.len  = len;
    bs.pos  = 0;
    bs.buf  = 0;
    bs.avail = 0;
    bs.hit_marker = false;
    bs.marker_byte = 0;
}

/*
 * Fill the bit buffer with at least 'need' bits.
 * Returns FILL_OK, FILL_EOF, or FILL_MARKER.
 */
static FillResult bs_fill(BitStream &bs, int need)
{
    while (bs.avail < need) {
        if (bs.pos >= bs.len)
            return FILL_EOF;

        uint8_t byte = bs.data[bs.pos++];

        if (byte == 0xFF) {
            /* Byte stuffing: peek at next byte */
            if (bs.pos >= bs.len)
                return FILL_EOF;

            uint8_t next = bs.data[bs.pos];
            if (next == 0x00) {
                /* FF00 -> FF data byte, consume the 00 */
                bs.pos++;
            } else if (next >= 0xD0 && next <= 0xD7) {
                /* Restart marker - don't consume, caller handles */
                bs.hit_marker = true;
                bs.marker_byte = next;
                bs.pos--; /* back up so caller sees the FF */
                return FILL_MARKER;
            } else if (next >= 0xC0) {
                /* Standard JPEG marker (C0-FE): SOF, SOS, EOI, etc. */
                bs.hit_marker = true;
                bs.marker_byte = next;
                bs.pos--;
                return FILL_MARKER;
            } else if (g_lenient_ff_enabled) {
                /* FF followed by 01-BF: reserved/undefined.
                 * On corrupted media, this is almost certainly a bit error
                 * in a byte-stuffed FF 00 pair. Skip it like libjpeg does. */
                bs.pos++;
            } else {
                /* Strict mode: treat as marker */
                bs.hit_marker = true;
                bs.marker_byte = next;
                bs.pos--;
                return FILL_MARKER;
            }
        }

        /* Shift byte into accumulator at MSB side */
        bs.buf |= (uint64_t)byte << (56 - bs.avail);
        bs.avail += 8;
    }
    return FILL_OK;
}

/*
 * Peek at the next 'nbits' bits without consuming them.
 * Returns >= 0 on success, -2 on EOF, -3 on marker.
 */
int bs_peek(BitStream &bs, int nbits)
{
    if (bs.avail < nbits) {
        FillResult fr = bs_fill(bs, nbits);
        if (fr == FILL_EOF)    return -2;
        if (fr == FILL_MARKER) return -3;
    }
    return (int)(bs.buf >> (64 - nbits));
}

/*
 * Consume 'nbits' bits from the stream.
 */
void bs_skip(BitStream &bs, int nbits)
{
    bs.buf <<= nbits;
    bs.avail -= nbits;
}

/*
 * Peek and consume in one call. Returns -1 on EOF/marker.
 */
int bs_read(BitStream &bs, int nbits)
{
    int val = bs_peek(bs, nbits);
    if (val >= 0)
        bs_skip(bs, nbits);
    return val;
}

/*
 * Get current byte offset in the stream (including stuffed bytes).
 */
size_t bs_byte_offset(const BitStream &bs)
{
    return bs.pos - (bs.avail / 8);
}

/*
 * Skip to next byte boundary (discard remaining bits in current byte).
 * Used at restart markers.
 */
void bs_align(BitStream &bs)
{
    int discard = bs.avail % 8;
    if (discard > 0) {
        bs.buf <<= discard;
        bs.avail -= discard;
    }
}

/*
 * Check and consume a restart marker.
 * Returns true if RST marker with expected sequence was found.
 * Resets bit buffer.
 *
 * After decoding the last MCU of a restart interval, bs.pos may not
 * be exactly at the FF byte of the RST marker. There can be a small
 * gap of un-consumed bytes (fill data that was loaded into the bit
 * buffer but whose raw bytes are still between bs.pos and the marker).
 * We scan forward up to 8 bytes to find the FF Dn marker.
 */
bool bs_consume_restart(BitStream &bs, int expected_rst)
{
    /* Discard remaining bits (fill bits from end of restart interval) */
    bs.buf = 0;
    bs.avail = 0;

    /* Scan forward looking for FF Dn, allowing a few non-FF bytes
     * (fill data between the bit reader position and the marker) */
    size_t limit = bs.pos + 8;
    if (limit > bs.len) limit = bs.len;

    while (bs.pos < limit) {
        uint8_t byte = bs.data[bs.pos];
        if (byte == 0xFF) {
            if (bs.pos + 1 < bs.len) {
                uint8_t next = bs.data[bs.pos + 1];
                if (next >= 0xD0 && next <= 0xD7) {
                    int rst_num = next - 0xD0;
                    bs.pos += 2; /* consume the FF Dn */
                    bs.hit_marker = false;
                    return rst_num == expected_rst;
                } else if (next == 0xFF) {
                    bs.pos++; /* skip padding FF */
                    continue;
                } else if (next == 0x00) {
                    return false; /* FF00 is data, not a marker */
                } else {
                    return false; /* non-RST marker (EOI, SOS, etc.) */
                }
            }
            break;
        }
        bs.pos++; /* skip non-FF byte (fill data) */
    }
    return false;
}
