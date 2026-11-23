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

/* Akceptujemy tylko linie wyglądające jak ramka modułu CAN:
 *   - zawierają '!' (separator sekcji CAN1!CAN2), albo
 *   - zaczynają się "+9<cyfra>" (sekcja CAN2, np. "+9902900000").
 * To odrzuca IP (10.x), urwane fragmenty GPS (0.00,K,N*32) i URC-i modemu
 * (+CREG, +CMGR, +IPD...), które zaczynają się "+<litera>". */
static uint8_t looks_like_can(const char *s)
{
    if (s[0] == '+' && s[1] == '9')
        return 1;
    for (const char *p = s; *p; p++)
        if (*p == '!')
            return 1;
    return 0;
}

uint8_t can_feed(const char *line)
{
    if (!looks_like_can(line))
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
