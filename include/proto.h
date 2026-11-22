/*
 * proto.h — nowy, czysty protokół serwera (zastępuje stałopozycyjny format
 * oryginału z naleciałościami typu "SESJA -").
 *
 * Ramka tekstowa, jedna linia:
 *     #<TYP>,<pola CSV>*<XOR2hex>\r\n
 * Suma kontrolna: XOR wszystkich znaków między '#' a '*' (bez nich), 2 cyfry
 * hex. Styl jak w NMEA — prosty do weryfikacji po obu stronach.
 *
 * Typy device->serwer:
 *   HELLO  — handshake:   proto, imei, fw, gps, gsm
 *   PING   — keepalive:   imei, uptime_s
 *   POS    — pozycja:     utc, lat(1e7), lon(1e7), alt, sat, spd, course, flags
 *            opcjonalnie z surową ramką CAN jako ostatnie pole: CAN:<raw>
 * Współrzędne: stopnie * 1e7 ze znakiem (dodatnie = N/E).
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include "gps.h"

/* Dopisuje sumę XOR i CRLF do gotowej treści zaczynającej się od '#'.
 * Zwraca długość całości (bez '\0'). Bufor musi mieć zapas na "*HH\r\n\0". */
uint8_t proto_finish(char *buf);

uint8_t proto_hello(char *buf, uint8_t size, const char *imei);
uint8_t proto_ping(char *buf, uint8_t size, const char *imei, uint32_t uptime);

/* Buduje ramkę POS. can_raw może być NULL (brak danych CAN). */
uint8_t proto_pos(char *buf, uint8_t size, const gps_fix_t *fix,
                  char move_flag, const char *can_raw);

/* Ramka diagnostyczna: surowa linia ze źródła src (np. "CAN") na serwer.
 * Pozwala zbierać dane zdalnie, gdy debug USART0 jest niedostępny (w aucie). */
uint8_t proto_raw(char *buf, uint8_t size, const char *src, const char *payload);

#endif /* PROTO_H */
