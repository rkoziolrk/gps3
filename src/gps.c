/*
 * gps.c — parser NMEA dla PA6C (RMC + GGA + VTG), arytmetyka całkowitoliczbowa.
 *
 * Współrzędne liczone bez float: "ddmm.mmmmm" -> stopnie * 1e7.
 *   minutes_scaled5 = mm*100000 + ułamek(5 cyfr)
 *   frac_1e7 = minutes_scaled5 * 5 / 3     (bo *1e7/60 = *5/3 przy skali 1e5)
 *   deg_1e7  = dd*10000000 + frac_1e7
 */
#include "gps.h"
#include <string.h>
#include <stdlib.h>

static gps_fix_t fix;

void gps_init(void)
{
    memset(&fix, 0, sizeof(fix));
}

const gps_fix_t *gps_current(void)
{
    return &fix;
}

/* Walidacja sumy NMEA: XOR znaków między '$' a '*' == hex po '*'. */
static uint8_t nmea_checksum_ok(const char *s)
{
    uint8_t cs = 0;
    const char *p = s;

    if (*p != '$')
        return 0;
    p++;
    while (*p && *p != '*')
        cs ^= (uint8_t)*p++;
    if (*p != '*')
        return 1;               /* brak sumy — akceptuj (część modułów pomija) */
    p++;
    uint8_t want = (uint8_t)strtol(p, NULL, 16);
    return cs == want;
}

/* Rozbija kopię linii na pola CSV (max n). Zwraca liczbę pól. */
static uint8_t split(char *s, char **f, uint8_t n)
{
    uint8_t c = 0;
    f[c++] = s;
    while (*s && c < n) {
        if (*s == ',' || *s == '*') {
            *s = '\0';
            f[c++] = s + 1;
        }
        s++;
    }
    return c;
}

/* "ddmm.mmmmm" -> stopnie*1e7; degdig = 2 (lat) lub 3 (lon). */
static int32_t nmea_coord(const char *s, uint8_t degdig)
{
    char deg[4] = {0};
    uint8_t i;
    int32_t dd, m5 = 0;

    if (!s || !*s)
        return 0;
    for (i = 0; i < degdig && s[i]; i++)
        deg[i] = s[i];
    dd = atol(deg);

    /* część minutowa: "mm.mmmmm" */
    const char *mp = s + degdig;
    int32_t mint = 0, scale = 100000, fracv = 0;
    while (*mp && *mp != '.')
        mint = mint * 10 + (*mp++ - '0');
    if (*mp == '.') {
        mp++;
        while (*mp && scale > 1) {
            scale /= 10;
            fracv += (int32_t)(*mp++ - '0') * scale;
        }
    }
    m5 = mint * 100000 + fracv;          /* minuty * 1e5 */
    return dd * 10000000L + (m5 * 5) / 3;
}

uint8_t gps_feed(const char *line)
{
    char buf[80];
    char *f[20];
    uint8_t nf;

    if (strncmp(line, "$G", 2) != 0)
        return 0;
    if (!nmea_checksum_ok(line))
        return 0;

    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    nf = split(buf, f, 20);

    const char *typ = buf + 3;            /* po "$GP"/"$GN" -> "RMC"/"GGA"/"VTG" */

    if (!strncmp(typ, "RMC", 3) && nf >= 10) {
        /* f1=utc f2=status f3=lat f4=N/S f5=lon f6=E/W f7=knots f8=course f9=date */
        strncpy(fix.utc, f[1], 6);  fix.utc[6] = '\0';
        strncpy(fix.date, f[9], 6); fix.date[6] = '\0';
        fix.valid = (f[2][0] == 'A');
        if (fix.valid) {
            fix.lat = nmea_coord(f[3], 2);
            if (f[4][0] == 'S') fix.lat = -fix.lat;
            fix.lon = nmea_coord(f[5], 3);
            if (f[6][0] == 'W') fix.lon = -fix.lon;
            fix.course = (uint16_t)atoi(f[8]);
        }
        return fix.valid;
    } else if (!strncmp(typ, "GGA", 3) && nf >= 10) {
        /* f6=fixq f7=sat f9=alt */
        fix.sats  = (uint8_t)atoi(f[7]);
        fix.alt_m = (int16_t)atoi(f[9]);
    } else if (!strncmp(typ, "VTG", 3) && nf >= 8) {
        /* f1=course(T) f7=speed km/h */
        if (f[1][0])
            fix.course = (uint16_t)atoi(f[1]);
        fix.spd_kmh = (uint16_t)atoi(f[7]);
    }
    return 0;
}
