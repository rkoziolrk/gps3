/*
 * roam.c — strategia wyboru operatora wg trybu roamingu.
 *
 * Uproszczone na start (patrz docs/ROAMING.md — pełna tabela 39 krajów z
 * oryginału do doportowania):
 *   tryb 0      — operator zablokowany na krajowym (bez roamingu),
 *   tryb 3      — automatyczny wybór (domyślny, działa też w roamingu),
 *   pozostałe   — automatyczny (placeholder do rozbudowy per MCC).
 */
#include "roam.h"

const char *roam_cops_cmd(uint8_t mode, const char *mcc)
{
    (void)mcc;
    switch (mode) {
    case 0:
        return "AT+COPS=0";          /* automatyczny; blokadę krajową dodamy z tabelą */
    case 3:
    default:
        return "AT+COPS=0";          /* automatyczny wybór operatora */
    }
}
