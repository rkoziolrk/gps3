/*
 * can.h — odbiór danych z zewnętrznego modułu CAN (tekst na wspólnej linii).
 *
 * ATmega64 nie dekoduje magistrali CAN auta — moduł podaje gotowe wartości
 * (docs/CAN.md). Dopóki nie mamy surowej ramki modułu, ten moduł przechowuje
 * ostatnią „kandydującą" linię i udostępnia ją do doklejenia w ramce POS oraz
 * do zrzutu na port debug. Format pól (paliwo, ?A/?B) doprecyzujemy po
 * zebraniu surowych ramek.
 */
#ifndef CAN_H
#define CAN_H

#include <stdint.h>

void        can_init(void);        /* włącza enable modułu, zeruje stan        */

/* Podaj nie-GPS linię; jeśli wygląda na ramkę modułu CAN (nie odpowiedź AT),
 * zapamiętuje ją i zwraca 1. */
uint8_t     can_feed(const char *line);

const char *can_last_raw(void);    /* "" gdy brak świeżych danych              */
void        can_clear(void);       /* skasuj po zużyciu (np. po wysłaniu POS)  */

#endif /* CAN_H */
