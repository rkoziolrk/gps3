/*
 * debug.c — implementacja logu diagnostycznego na USART0.
 */
#include "debug.h"
#include "uart.h"
#include "clock.h"

void dbg_putlong(long v)
{
    char buf[12];
    uint8_t i = 0;
    unsigned long u;

    if (v < 0) {
        uart0_putc('-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    do {
        buf[i++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u && i < sizeof(buf));
    while (i)
        uart0_putc(buf[--i]);
}

void dbg_puthex8(uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    uart0_putc(hx[(v >> 4) & 0xF]);
    uart0_putc(hx[v & 0xF]);
}

static void stamp(void)
{
    uart0_putc('[');
    dbg_putlong((long)clock_uptime());
    uart0_putc(']');
    uart0_putc(' ');
}

void dbg_line(const char *tag, const char *msg)
{
#if DBG_LEVEL >= 1
    stamp();
    uart0_puts(tag);
    uart0_puts(": ");
    uart0_puts(msg);
    uart0_puts("\r\n");
#else
    (void)tag; (void)msg;
#endif
}

void dbg_kv(const char *tag, const char *key, long val)
{
#if DBG_LEVEL >= 1
    stamp();
    uart0_puts(tag);
    uart0_puts(": ");
    uart0_puts(key);
    uart0_putc('=');
    dbg_putlong(val);
    uart0_puts("\r\n");
#else
    (void)tag; (void)key; (void)val;
#endif
}

void dbg_raw(const char *src, const char *line)
{
#if DBG_LEVEL >= 2
    stamp();
    uart0_puts("RAW ");
    uart0_puts(src);
    uart0_puts(" <= ");
    uart0_puts(line);
    uart0_puts("\r\n");
#else
    (void)src; (void)line;
#endif
}

void dbg_init(void)
{
    uart0_puts("\r\n=== tracker fw " FW_VERSION " / debug port ===\r\n");
}
