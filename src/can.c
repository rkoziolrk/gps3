/*
 * can.c — odbiór ramek modułu CAN z linii szeregowej.
 */
#include "board.h"
#include "can.h"
#include "cfg.h"
#include <string.h>

static char last_raw[64];

void can_init(void)
{
    /* Włącz zasilanie/enable modułu CAN jak oryginał (DDRB.4 wyjście).
     * Poziom czynny do potwierdzenia na sprzęcie — zakładamy stan wysoki. */
    PIN_OUT(CANEN_DDR, CANEN_BIT);
    PIN_HIGH(CANEN_PORT, CANEN_BIT);
    last_raw[0] = '\0';
}

/* Heurystyka: odpowiedzi modemu SIM900 pomijamy; resztę traktujemy jako
 * kandydata na ramkę CAN. Do zawężenia, gdy poznamy realny format. */
static uint8_t looks_like_modem(const char *s)
{
    static const char *const pref[] = {
        "AT", "OK", "ERROR", "+", ">", "CONNECT", "CLOSED", "SEND", "RING",
        "NO CARRIER", "CME", "CMS", NULL
    };
    for (uint8_t i = 0; pref[i]; i++)
        if (!strncmp(s, pref[i], strlen(pref[i])))
            return 1;
    return 0;
}

uint8_t can_feed(const char *line)
{
    if (!line[0] || looks_like_modem(line))
        return 0;
    strncpy(last_raw, line, sizeof(last_raw) - 1);
    last_raw[sizeof(last_raw) - 1] = '\0';
    return 1;
}

const char *can_last_raw(void)
{
    return last_raw;
}

void can_clear(void)
{
    last_raw[0] = '\0';
}
