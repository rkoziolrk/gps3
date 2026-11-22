/*
 * board.h — definicje sprzętowe płytki lokalizatora (ATmega64 @ 8 MHz).
 *
 * Fakty potwierdzone z inicjalizacji oryginalnego firmware oraz z analizy
 * deasemblera — patrz docs/MEMORY_MAP.md i docs/CAN.md.
 *
 * Kluczowe różnice wobec oryginału (patrz docs/REWRITE.md):
 *   - brak LCD (nie montowany) — PORTC wolny,
 *   - USART1  = jedyna linia komunikacji: GPS (PA6C) + GSM (SIM900),
 *               demultipleksowane po treści linii,
 *   - USART0  = port DEBUG/log (w oryginale nieużywany).
 */
#ifndef BOARD_H
#define BOARD_H

#include <avr/io.h>

/* --- Zegar --------------------------------------------------------------- */
#ifndef F_CPU
#define F_CPU 8000000UL
#endif

/* --- Prędkości transmisji ------------------------------------------------ */
#define UART1_BAUD 9600UL          /* linia GPS+GSM (jak oryginał, AT+IPR=9600) */
#define UART0_BAUD 38400UL         /* debug — czysty dzielnik @8 MHz (UBRR=12, bł. 0,16%).
                                      UWAGA: 115200 @8 MHz ma ~8,5% błędu => krzaki. */

#define UBRR_VAL(baud) ((F_CPU / (16UL * (baud))) - 1)

/* --- Linie sterujące modułami (z analizy init+stringów oryginału) --------- *
 * WAŻNE: w oryginale te piny startują NISKO i są IMPULSOWANE (nie trzymane
 * wysoko). Znaczenia z komunikatów w obrazie:
 *   PD6 "WLACZAM MODEM"  — impuls HIGH ~1,5 s włącza modem (PWRKEY),
 *   PD5 "RESET ZA 7 sek" — impuls HIGH ~6 s = reset modemu (recovery),
 *   PD7 "RESET GPS"      — impuls HIGH ~10 s = reset GPS (recovery).
 * Idle = LOW. Patrz docs/MEMORY_MAP.md. */
#define MODEM_PORT   PORTD
#define MODEM_DDR    DDRD
#define MODEM_PWR_BIT PD6          /* włączenie modemu (impuls HIGH)          */
#define MODEM_RST_BIT PD5          /* reset modemu (impuls HIGH)              */
#define GPS_RST_BIT   PD7          /* reset GPS (impuls HIGH)                 */

#define PWRLATCH_PORT PORTA        /* PA3/PA4 ustawiane na 1 w init (klucz)   */
#define PWRLATCH_DDR  DDRA
#define PWRLATCH_A3   PA3
#define PWRLATCH_A4   PA4

#define LED_PORT     PORTB
#define LED_DDR      DDRB
#define LED_BIT      PB6           /* dioda statusu (PORTB.6=1 w init)        */

#define CANEN_PORT   PORTB
#define CANEN_DDR    DDRB
#define CANEN_BIT    PB4           /* enable modułu CAN (DDRB.4 wyjście)      */

/* --- Zewnętrzna pamięć na programowym I2C (opcjonalnie) ------------------ */
#define I2C_PORT     PORTD
#define I2C_DDR      DDRD
#define I2C_PIN      PIND
#define I2C_SCL_BIT  PD0
#define I2C_SDA_BIT  PD1

/* --- Pomocnicze makra pinów --------------------------------------------- */
#define PIN_HIGH(port, bit) ((port) |=  (1 << (bit)))
#define PIN_LOW(port, bit)  ((port) &= ~(1 << (bit)))
#define PIN_OUT(ddr, bit)   ((ddr)  |=  (1 << (bit)))

#endif /* BOARD_H */
