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

/* --- Linie sterujące modułami (porty/piny z init oryginału) -------------- */
/* Zasilanie / enable. Poziomy czynne potwierdzimy przy bring-upie na sprzęcie. */
#define GSM_PWR_PORT PORTD
#define GSM_PWR_DDR  DDRD
#define GSM_PWR_BIT  PD5           /* PWRKEY / zasilanie SIM900                */

#define GPS_PWR_PORT PORTD
#define GPS_PWR_DDR  DDRD
#define GPS_PWR_BIT  PD6           /* zasilanie GPS PA6C                       */

#define HWRST_PORT   PORTD
#define HWRST_DDR    DDRD
#define HWRST_BIT    PD7           /* linia resetu modułu                     */

#define PWRLATCH_DDR DDRA          /* PA3/PA4 ustawiane na 1 w init (klucz)   */
#define PWRLATCH_A3  PA3
#define PWRLATCH_A4  PA4

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
