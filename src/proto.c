/*
 * proto.c — budowanie ramek nowego protokołu.
 */
#include "proto.h"
#include "cfg.h"
#include <stdio.h>
#include <string.h>

static const char HX[] = "0123456789ABCDEF";

uint8_t proto_finish(char *buf)
{
    uint8_t cs = 0;
    uint8_t i = 1;                 /* XOR od znaku po '#' */

    while (buf[i])
        cs ^= (uint8_t)buf[i++];
    buf[i++] = '*';
    buf[i++] = HX[(cs >> 4) & 0xF];
    buf[i++] = HX[cs & 0xF];
    buf[i++] = '\r';
    buf[i++] = '\n';
    buf[i]   = '\0';
    return i;
}

uint8_t proto_hello(char *buf, uint8_t size, const char *imei)
{
    snprintf(buf, size, "#HELLO,%d,%s,%s,%s,%s",
             PROTO_VERSION, imei, FW_VERSION, GPS_MODULE, GSM_MODULE);
    return proto_finish(buf);
}

uint8_t proto_ping(char *buf, uint8_t size, const char *imei, uint32_t uptime)
{
    snprintf(buf, size, "#PING,%s,%lu", imei, (unsigned long)uptime);
    return proto_finish(buf);
}

uint8_t proto_raw(char *buf, uint8_t size, const char *src, const char *payload)
{
    snprintf(buf, size, "#RAW,%s,%s", src, payload);
    return proto_finish(buf);
}

uint8_t proto_pos(char *buf, uint8_t size, const gps_fix_t *fix,
                  char move_flag, const char *can_raw)
{
    int n = snprintf(buf, size,
                     "#POS,%s,%ld,%ld,%d,%u,%u,%u,%c",
                     fix->utc,
                     (long)fix->lat, (long)fix->lon,
                     (int)fix->alt_m, (unsigned)fix->sats,
                     (unsigned)fix->spd_kmh, (unsigned)fix->course,
                     move_flag);
    if (can_raw && can_raw[0] && n > 0 && (uint8_t)n < size) {
        /* Surowa ramka CAN doklejana w całości — do analizy pól po stronie
         * serwera (paliwo, pola nieustalone). Patrz docs/CAN.md. */
        strncat(buf, ",CAN:", size - strlen(buf) - 1);
        strncat(buf, can_raw, size - strlen(buf) - 1);
    }
    return proto_finish(buf);
}
