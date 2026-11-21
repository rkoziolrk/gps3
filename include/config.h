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

/* --- Wyswietlacz LCD HD44780 (4-bit) na PORTC (DDRC.0..5 = wyjscia) ------- */
#define LCD_PORT       PORTC
#define LCD_D4_D7      0x0F        /* PC0..PC3 = D4..D7                        */
#define LCD_RS_BIT     PC4
#define LCD_E_BIT      PC5

/* --- Pamiec: wewnetrzny EEPROM ATmega64 (konfiguracja, docs/EEPROM.md) ---- *
 * oraz zewnetrzna pamiec na programowym I2C: PD0 = SCL, PD1 = SDA.          */
#define I2C_SCL_BIT    PD0
#define I2C_SDA_BIT    PD1

/* --- CAN: dane z zewnetrznego modulu (brama CAN->szeregowa), parsowane ---- *
 * w RAM; ATmega64 nie ma kontrolera CAN.                                     */

/* --- Serwer -------------------------------------------------------------- */
#define SERVER_SUFFIX  ".MONITORING-GPS.NET"
#define SERVER_DEFAULT "lgps01.monitoring-gps.net"
#define SERVER_PORT_DEFAULT "3506"  /* domyslny port TCP (flash 0xbbda)         */
#define APN_DEFAULT    "internet"

/* --- Wersja (string z obrazu: "ver=2.65") -------------------------------- */
#define FW_VERSION     "2.65"
#define GPS_MODULE     "PA6C"
#define GSM_MODULE     "SIM900"

#endif /* CONFIG_H */
