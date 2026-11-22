/*
 * debug.h — log diagnostyczny i zbieranie danych na USART0.
 *
 * Lekki, bez pełnego printf (oszczędność flash). Każda linia logu jest
 * poprzedzona znacznikiem czasu [uptime_s]. Kategorie zrzutu surowego służą do
 * analizy nierozwiązanych pytań (ramki CAN, pola paliwa) — patrz docs/CAN.md.
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include "cfg.h"

void dbg_init(void);                       /* nagłówek startowy na porcie debug */

void dbg_line(const char *tag, const char *msg);   /* [t] tag: msg              */
void dbg_kv(const char *tag, const char *key, long val);

/* Zrzut surowej, sklasyfikowanej linii z USART1 (źródło: "GPS"/"GSM"/"CAN"). */
void dbg_raw(const char *src, const char *line);

/* Pomocnicze wypisywanie liczb na port debug (bez printf). */
void dbg_putlong(long v);
void dbg_puthex8(uint8_t v);

#endif /* DEBUG_H */
