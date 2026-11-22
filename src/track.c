/*
 * track.c — logowanie adaptacyjne, arytmetyka całkowitoliczbowa.
 *
 * Dystans liczony przybliżeniem równokątnym (equirectangular) z tablicą cos(lat)
 * — bez float/trygonometrii. Porównania dystansu na kwadratach (bez sqrt).
 */
#include "track.h"
#include <stdlib.h>

static cfg_t      cfg;
static gps_fix_t  last;        /* ostatnio zapisany fiks          */
static uint32_t   last_t;
static uint8_t    have_last;
static uint8_t    was_moving;

/* cos(|lat|)*1024 dla 0,10,...,80° */
static const uint16_t COS1024[9] = {1024,1008,962,887,785,658,512,350,178};

void track_init(const cfg_t *c)
{
    cfg = *c;
    have_last = 0;
    was_moving = 0;
    last_t = 0;
}

static uint16_t cos1024(int32_t lat_1e7)
{
    int32_t d = lat_1e7 / 10000000L;
    if (d < 0) d = -d;
    uint8_t i = (uint8_t)(d / 10);
    if (i > 8) i = 8;
    return COS1024[i];
}

/* Kwadrat dystansu [m^2] między a i b. */
static int32_t dist2_m(const gps_fix_t *a, const gps_fix_t *b)
{
    int32_t dlat = a->lat - b->lat;        /* w jednostkach 1e-7 deg */
    int32_t dlon = a->lon - b->lon;
    int32_t ml = (dlat * 11132L) / 1000000L;               /* metry N/S */
    int32_t mo = (dlon * 11132L) / 1000000L;
    mo = (mo * (int32_t)cos1024(a->lat)) / 1024;           /* korekta E/W */
    return ml * ml + mo * mo;
}

static uint16_t course_delta(uint16_t a, uint16_t b)
{
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    if (d > 180) d = 360 - d;
    return (uint16_t)d;
}

uint8_t track_update(const gps_fix_t *fix, uint32_t uptime, char *move_flag)
{
    uint8_t moving = (fix->spd_kmh >= cfg.move_spd);
    uint8_t emit = 0;

    *move_flag = moving ? 'R' : 'P';

    if (!have_last) {
        emit = 1;                          /* pierwszy fiks zawsze */
    } else if (moving != was_moving) {
        emit = 1;                          /* zmiana ruch<->postój */
    } else if (moving) {
        uint32_t dt   = uptime - last_t;
        int32_t  d2   = dist2_m(fix, &last);
        uint16_t dc   = course_delta(fix->course, last.course);
        int32_t  mind = (int32_t)cfg.min_dist_m * (int32_t)cfg.min_dist_m;
        if (dc >= cfg.course_deg || dt >= cfg.max_secs || d2 >= mind)
            emit = 1;
    } else {
        if ((uptime - last_t) >= cfg.stop_secs)
            emit = 1;                      /* rzadki rekord na postoju */
    }

    if (emit) {
        last = *fix;
        last_t = uptime;
        have_last = 1;
        was_moving = moving;
    }
    return emit;
}
