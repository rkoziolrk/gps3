/*
 * gsm.h — sterownik SIM900 (GPRS + TCP) jako maszyna stanów.
 */
#ifndef GSM_H
#define GSM_H

#include <stdint.h>
#include "cfg_store.h"

/* sms_cb: wywoływane z treścią odebranego SMS-a (po skasowaniu go z karty),
 * gdy na pozycji 1 był SMS. Może nie wrócić (reboot przy zmianie konfiguracji
 * / OTA). NULL = nie czytaj SMS. */
void        gsm_init(const cfg_t *c, void (*sms_cb)(const char *body));
void        gsm_tick(uint32_t uptime);        /* napędza sekwencję/timeouty     */
void        gsm_feed(const char *line);       /* konsumuje odpowiedzi modemu     */

uint8_t     gsm_online(void);                 /* 1 = TCP zestawiony              */
uint8_t     gsm_send_frame(const char *frame);/* wyślij ramkę (gdy online)       */
const char *gsm_imei(void);                   /* IMEI z AT+GSN ("" dopóki brak)  */

#endif /* GSM_H */
