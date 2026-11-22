/*
 * sms.c — parser komend SMS. Kluczowa różnica wobec oryginału: pełna kontrola
 * długości i wymagane terminatory '$', więc źle sformatowany SMS jest
 * odrzucany, a nie przepełnia bufor i nie powoduje pętli restartów.
 */
#include "sms.h"
#include <string.h>

sms_action_t sms_parse(const char *text, cfg_t *c, const char *imei_pin)
{
    if (!strncmp(text, "NEW*ADR=", 8)) {
        const char *p = text + 8;
        char host[HOST_MAXLEN];
        char port[PORT_MAXLEN];
        uint8_t i;

        /* host do '$' */
        for (i = 0; p[i] && p[i] != '$'; i++) {
            if (i >= HOST_MAXLEN - 1)
                return SMS_BADFMT;         /* za długi — odrzuć */
            host[i] = p[i];
        }
        if (p[i] != '$')                   /* brak terminatora — odrzuć */
            return SMS_BADFMT;
        host[i] = '\0';
        p += i + 1;

        /* port do '$' */
        for (i = 0; p[i] && p[i] != '$'; i++) {
            if (i >= PORT_MAXLEN - 1)
                return SMS_BADFMT;
            port[i] = p[i];
        }
        if (p[i] != '$')
            return SMS_BADFMT;
        port[i] = '\0';

        if (!cfg_set_server(c, host, port))
            return SMS_BADFMT;
        return SMS_SETADDR;
    }

    if (!strncmp(text, "RESET*", 6) || !strncmp(text, "RESET-", 6)) {
        const char *pin = text + 6;
        if (imei_pin && !strncmp(pin, imei_pin, 4))
            return SMS_RESET;
        return SMS_BADFMT;
    }

    return SMS_NONE;
}
