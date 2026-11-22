/*
 * roam.h — obsługa roamingu (praca za granicą).
 *
 * Tryb (0..7) pochodzi z konfiguracji. Pełna tabela krajów/operatorów z
 * oryginału (docs/ROAMING.md) do doportowania; na start: mapowanie trybu na
 * strategię wyboru operatora (AT+COPS).
 */
#ifndef ROAM_H
#define ROAM_H

#include <stdint.h>

/* Zwraca komendę AT wyboru operatora dla danego trybu i MCC bieżącej sieci,
 * albo NULL gdy nie trzeba nic wysyłać (zostaw automatyczny wybór). */
const char *roam_cops_cmd(uint8_t mode, const char *mcc);

#endif /* ROAM_H */
