/*
 * ota.c — strona aplikacyjna OTA: żądanie aktualizacji.
 */
#include "ota.h"
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>

void ota_request(void)
{
    cli();
    eeprom_update_byte((uint8_t *)OTA_FLAG_ADDR, OTA_MAGIC);
    /* Reset przez watchdog -> start trafia do bootloadera (BOOTRST). */
    wdt_enable(WDTO_15MS);
    for (;;)
        ;
}
