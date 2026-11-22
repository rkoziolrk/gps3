/*
 * cfg_store.c — trwała konfiguracja w EEPROM, z sumą kontrolną.
 */
#include "cfg_store.h"
#include <avr/eeprom.h>
#include <string.h>
#include <stdlib.h>

#define CFG_MAGIC 0xA5

/* Obraz w EEPROM: magic + struktura + XOR. */
static uint8_t  EEMEM ee_magic;
static cfg_t    EEMEM ee_cfg;
static uint8_t  EEMEM ee_xor;

static uint8_t calc_xor(const cfg_t *c)
{
    const uint8_t *p = (const uint8_t *)c;
    uint8_t x = CFG_MAGIC;
    for (uint16_t i = 0; i < sizeof(cfg_t); i++)
        x ^= p[i];
    return x;
}

static void set_defaults(cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strncpy(c->host, SERVER_HOST_DEFAULT, HOST_MAXLEN - 1);
    c->port       = SERVER_PORT_DEFAULT;
    c->roam_mode  = 3;
    c->course_deg = TRK_COURSE_DEG;
    c->max_secs   = TRK_MAX_SECS;
    c->min_dist_m = TRK_MIN_DIST_M;
    c->move_spd   = TRK_MOVE_SPD;
    c->stop_secs  = TRK_STOP_SECS;
}

void cfg_save(const cfg_t *c)
{
    eeprom_update_block(c, &ee_cfg, sizeof(cfg_t));
    eeprom_update_byte(&ee_magic, CFG_MAGIC);
    eeprom_update_byte(&ee_xor, calc_xor(c));
}

void cfg_load(cfg_t *c)
{
    uint8_t magic = eeprom_read_byte(&ee_magic);
    eeprom_read_block(c, &ee_cfg, sizeof(cfg_t));
    uint8_t xr = eeprom_read_byte(&ee_xor);

    if (magic != CFG_MAGIC || xr != calc_xor(c)) {
        set_defaults(c);
        cfg_save(c);
    }
}

uint8_t cfg_set_server(cfg_t *c, const char *host, const char *port_str)
{
    size_t hl = strlen(host);
    long   p;

    if (hl == 0 || hl >= HOST_MAXLEN)        /* TWARDY limit — brak przepełnień */
        return 0;
    if (strlen(port_str) == 0 || strlen(port_str) >= PORT_MAXLEN)
        return 0;
    p = atol(port_str);
    if (p < 1 || p > 65535)
        return 0;

    memset(c->host, 0, HOST_MAXLEN);
    memcpy(c->host, host, hl);
    c->port = (uint16_t)p;
    return 1;
}
