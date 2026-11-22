/*
 * track.h — polityka zapisu pozycji (logowanie adaptacyjne).
 *
 * Zastępuje czysto dystansową maszynę oryginału, która ścinała zakręty
 * (docs/REWRITE.md). Rekord powstaje, gdy w ruchu spełniony jest KTÓRYKOLWIEK
 * warunek: zmiana kursu, maks. czas, min. dystans, zmiana stanu ruch/postój;
 * na postoju — rzadko.
 */
#ifndef TRACK_H
#define TRACK_H

#include <stdint.h>
#include "gps.h"
#include "cfg_store.h"

void track_init(const cfg_t *c);

/* Wywoływane na każdy świeży, ważny fiks (1 Hz). Zwraca 1, jeśli należy teraz
 * zapisać/wysłać rekord POS. move_flag: 'R' ruch, 'P' postój. */
uint8_t track_update(const gps_fix_t *fix, uint32_t uptime, char *move_flag);

#endif /* TRACK_H */
