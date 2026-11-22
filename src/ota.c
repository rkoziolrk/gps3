/*
 * ota.c — strona aplikacyjna OTA: żądanie aktualizacji.
 */
#include "ota.h"
#include "cfg.h"
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>

void ota_request(const cfg_t *c)
{
    cli();
    /* Przekaż bootloaderowi adres serwera (bootloader nie zna układu cfg). */
    eeprom_update_block(c->host, (void *)OTA_HOST_ADDR, HOST_MAXLEN);
    eeprom_update_word((uint16_t *)OTA_PORT_ADDR, c->port);
    eeprom_update_byte((uint8_t *)OTA_FLAG_ADDR, OTA_MAGIC);
    /* Reset przez watchdog -> start trafia do bootloadera (BOOTRST). */
    wdt_enable(WDTO_15MS);
    for (;;)
        ;
}
