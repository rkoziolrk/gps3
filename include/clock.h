/*
 * clock.h — zegar sekundowy na Timer1 (jak w oryginale: preskaler 256).
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

void     clock_init(void);
uint32_t clock_uptime(void);      /* sekundy od startu */

#endif /* CLOCK_H */
