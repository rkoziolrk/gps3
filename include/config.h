/*
 * config.h — konfiguracja sprzetowa lokalizatora GPS (ATmega64 + SIM900 + PA6C)
 *
 * Wartosci zdekodowane z inicjalizacji firmware (flash.bin, @0x8c..0x1d4).
 * Patrz docs/MEMORY_MAP.md oraz reverse/flash_disasm.txt.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* --- Zegar --------------------------------------------------------------- */
#ifndef F_CPU
#define F_CPU 8000000UL            /* 8 MHz (UBRR=51 @ 9600; "AT+IPR=9600")   */
#endif

/* --- USART --------------------------------------------------------------- */
#define UART_BAUD      9600UL
#define UART_UBRR      ((F_CPU / (16UL * UART_BAUD)) - 1)   /* = 51 */

#define UART_GPS       0           /* USART0 -> GPS PA6C                       */
#define UART_GSM       1           /* USART1 -> SIM900 (ISR czyta UDR1)        */

/* --- Piny sterujace modulami (DDRx/PORTx z init) ------------------------- */
#define PWR_A3_PORT    PORTA       /* DDRA.3, PORTA.3=1  klucz zasilania       */
#define PWR_A3_BIT     PA3
#define PWR_A4_BIT     PA4         /* DDRA.4, PORTA.4=1                        */

#define GSM_PWR_BIT    PD5         /* DDRD.5  PWRKEY / zasilanie SIM900        */
#define GPS_PWR_BIT    PD6         /* DDRD.6  zasilanie GPS  (GPS-ON/GPS-OF)   */
#define RST_BIT        PD7         /* DDRD.7  linia resetu modulu              */

#define LED_BIT        PB6         /* DDRB.6, PORTB.6=1                        */
#define CAN_BIT        PB4         /* DDRB.4                                   */

/* --- Serwer -------------------------------------------------------------- */
#define SERVER_SUFFIX  ".MONITORING-GPS.NET"
#define SERVER_DEFAULT "lgps01.monitoring-gps.net"
#define APN_DEFAULT    "internet"

/* --- Wersja (string z obrazu: "ver=2.65") -------------------------------- */
#define FW_VERSION     "2.65"
#define GPS_MODULE     "PA6C"
#define GSM_MODULE     "SIM900"

#endif /* CONFIG_H */
