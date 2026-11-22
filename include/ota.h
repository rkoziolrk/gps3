/*
 * ota.h — kontrakt aplikacja<->bootloader dla zdalnej aktualizacji.
 *
 * Flaga żądania OTA leży pod STAŁYM adresem EEPROM (znanym obu stronom), z dala
 * od konfiguracji alokowanej przez linker. Patrz docs/OTA.md.
 */
#ifndef OTA_H
#define OTA_H

#include <stdint.h>
#include "cfg_store.h"

/* Blok OTA pod STAŁYMI adresami EEPROM (wspólny kontrakt app<->bootloader),
 * wysoko, z dala od konfiguracji alokowanej przez linker. */
#define OTA_HOST_ADDR  0x07C0     /* host serwera OTA (HOST_MAXLEN bajtów)     */
#define OTA_PORT_ADDR  0x07F0     /* port (uint16)                            */
#define OTA_SIZE_ADDR  0x07F4     /* rozmiar wgranego obrazu (uint32)         */
#define OTA_CRC_ADDR   0x07F8     /* CRC32 wgranego obrazu (uint32)           */
#define OTA_FLAG_ADDR  0x07FF     /* flaga żądania OTA                        */
#define OTA_MAGIC      0xB0       /* wartość = "żądanie aktualizacji"         */

/* Zapisz host+port do bloku OTA, ustaw flagę i zrestartuj (watchdog) do
 * bootloadera. Nie wraca. */
void ota_request(const cfg_t *c);

#endif /* OTA_H */
