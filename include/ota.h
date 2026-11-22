/*
 * ota.h — kontrakt aplikacja<->bootloader dla zdalnej aktualizacji.
 *
 * Flaga żądania OTA leży pod STAŁYM adresem EEPROM (znanym obu stronom), z dala
 * od konfiguracji alokowanej przez linker. Patrz docs/OTA.md.
 */
#ifndef OTA_H
#define OTA_H

#include <stdint.h>

#define OTA_FLAG_ADDR  0x07FF     /* ostatni bajt EEPROM 2 KB ATmega64 */
#define OTA_MAGIC      0xB0       /* wartość = "żądanie aktualizacji"  */

/* Ustaw flagę OTA w EEPROM i zrestartuj urządzenie (watchdog) do bootloadera.
 * Nie wraca. */
void ota_request(void);

#endif /* OTA_H */
