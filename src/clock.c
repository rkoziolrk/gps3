/*
 * clock.c — zegar 1 s na Timer1.
 *
 * Jak w oryginale: preskaler 256, przeładowanie TCNT1 tak, by przepełnienie
 * następowało co ~1 s. Przy 8 MHz / 256 = 31250 taktów/s, więc licznik 16-bit
 * ładujemy wartością 65536 - 31250 = 34286 = 0x85EE (zgodne z init oryginału).
 */
#include "board.h"
#include "clock.h"
#include <avr/interrupt.h>

#define TCNT1_RELOAD 0x85EE

static volatile uint32_t uptime_s;

ISR(TIMER1_OVF_vect)
{
    TCNT1 = TCNT1_RELOAD;
    uptime_s++;
}

void clock_init(void)
{
    uptime_s = 0;
    TCNT1 = TCNT1_RELOAD;
    TCCR1A = 0;
    TCCR1B = (1 << CS12);          /* preskaler 256 */
    TIMSK |= (1 << TOIE1);         /* przerwanie od przepełnienia */
}

uint32_t clock_uptime(void)
{
    uint32_t v;
    cli();
    v = uptime_s;
    sei();
    return v;
}
