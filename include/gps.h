/*
 * gps.h — parser NMEA (RMC/GGA/VTG) modułu PA6C.
 */
#ifndef GPS_H
#define GPS_H

#include <stdint.h>

typedef struct {
    uint8_t  valid;        /* 1 = ważny fiks (RMC status A)                   */
    int32_t  lat;          /* szer. geogr. w stopniach * 1e7 (dodatnie = N)   */
    int32_t  lon;          /* dł.  geogr. w stopniach * 1e7 (dodatnie = E)    */
    int16_t  alt_m;        /* wysokość n.p.m. [m]                             */
    uint8_t  sats;         /* liczba satelitów                                */
    uint16_t spd_kmh;      /* prędkość [km/h]                                 */
    uint16_t course;       /* kurs [°] 0..359                                 */
    char     utc[7];       /* "hhmmss"                                        */
    char     date[7];      /* "ddmmrr"                                        */
} gps_fix_t;

void gps_init(void);

/* Podaj kompletną linię NMEA (zaczynającą się od '$'); aktualizuje stan.
 * Zwraca 1, jeśli to było RMC i mamy świeży, kompletny fiks. */
uint8_t gps_feed(const char *line);

const gps_fix_t *gps_current(void);

#endif /* GPS_H */
