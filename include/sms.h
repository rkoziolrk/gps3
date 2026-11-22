/*
 * sms.h — zdalne sterowanie SMS-em (z kontrolą długości — bez pętli restartów
 * z oryginału, docs/PROTOCOL.md).
 */
#ifndef SMS_H
#define SMS_H

#include "cfg_store.h"

typedef enum {
    SMS_NONE = 0,     /* nic / zwykły tekst                     */
    SMS_SETADDR,      /* zmieniono host+port (cfg zaktualizowany) */
    SMS_RESET,        /* reset do ustawień fabrycznych           */
    SMS_UPDATE,       /* żądanie zdalnej aktualizacji (OTA)      */
    SMS_BADFMT        /* rozpoznana komenda, ale zły format      */
} sms_action_t;

/* Parsuje treść SMS. Przy SMS_SETADDR aktualizuje *c (z walidacją długości);
 * wywołujący decyduje o zapisie do EEPROM i restarcie. imei_pin = 4 ostatnie
 * cyfry IMEI (PIN do RESET). */
sms_action_t sms_parse(const char *text, cfg_t *c, const char *imei_pin);

#endif /* SMS_H */
