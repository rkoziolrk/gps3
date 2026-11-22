/*
 * cfg_store.h — konfiguracja trwała w wewnętrznym EEPROM ATmega64.
 *
 * W odróżnieniu od oryginału: jawna struktura z sumą kontrolną i TWARDYMI
 * limitami długości (brak przepełnień buforów przy zapisie hosta/portu z SMS).
 */
#ifndef CFG_STORE_H
#define CFG_STORE_H

#include <stdint.h>
#include "cfg.h"

typedef struct {
    char     host[HOST_MAXLEN];
    uint16_t port;
    uint8_t  roam_mode;        /* 0..7, patrz docs/ROAMING.md                 */
    uint8_t  course_deg;       /* progi logowania adaptacyjnego               */
    uint8_t  max_secs;
    uint8_t  min_dist_m;
    uint8_t  move_spd;
    uint16_t stop_secs;
} cfg_t;

/* Wczytaj z EEPROM; przy złej sumie/niezainicjowanym — ustaw domyślne i zapisz. */
void   cfg_load(cfg_t *c);
void   cfg_save(const cfg_t *c);

/* Ustaw host+port z kontrolą długości; zwraca 1 gdy OK, 0 gdy odrzucono. */
uint8_t cfg_set_server(cfg_t *c, const char *host, const char *port_str);

#endif /* CFG_STORE_H */
