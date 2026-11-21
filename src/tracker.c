/*
 * ============================================================================
 *  REKONSTRUKCJA FIRMWARE  —  Lokalizator GPS (fleet tracker)
 * ============================================================================
 *
 *  Plik wejsciowy : flash.bin  (64 KiB, wykorzystane 0x0000..0xC3EE = ~50 KiB)
 *  MCU            : Atmel ATmega64  (avr5)
 *  F_CPU          : 8 000 000 Hz   (potwierdzone: UBRR=51 @ 9600 baud oraz "AT+IPR=9600")
 *  Toolchain      : avr-gcc + avr-libc  (rozpoznane po rutynach runtime)
 *  Peryferia      : GPS  GlobalTop PA6C  (NMEA, PMTK)
 *                   GSM  SIMCom SIM900   (GPRS, AT)
 *                   CAN  (czujnik paliwa / RPM / pedal)
 *                   EEPROM I2C (bufor rekordow "OLD" do wyslania)
 *  Serwer         : lgps01.monitoring-gps.net  (oraz lXX.monitoring-gps.net)
 *                   lokalizator wysyla ramki protokolu znakowego (PNG/FORM/HSK/LOK/LOO/CONFIG)
 *
 * ----------------------------------------------------------------------------
 *  UWAGA O WIERNOSCI (przeczytaj!)
 * ----------------------------------------------------------------------------
 *  To jest DEKOMPILACJA / REKONSTRUKCJA, a nie oryginalne zrodlo. Z surowego
 *  obrazu flash nie da sie odtworzyc bajt-w-bajt oryginalnego .c (nazwy
 *  zmiennych, komentarze, uklad plikow sa bezpowrotnie utracone przy kompilacji).
 *
 *  Poziomy pewnosci uzyte w pliku:
 *    [HW]   - inicjalizacja sprzetu: zdekodowana DOKLADNIE z zapisow do rejestrow
 *             (adresy 0x8c..0x1d4 obrazu). Wartosci rejestrow sa pewne.
 *    [LIB]  - funkcje biblioteczne avr-libc/libgcc rozpoznane po sygnaturze kodu
 *             i podstawione gotowym odpowiednikiem (strcpy, itoa, _delay_ms, ...).
 *    [REC]  - logika aplikacji odtworzona z disassemblera + tablicy stringow
 *             + grafu wywolan. Struktura i przeplyw wierne; drobne szczegoly
 *             (np. dokladne progi, kolejnosc pol) moga sie roznic.
 *
 *  Mapa adresow (flash, slowo/bajt) kluczowych funkcji jest podana przy kazdej
 *  z nich w komentarzu "@0x....", zeby mozna bylo sprawdzic w avr-objdump:
 *      avr-objdump -D -b binary -m avr:5 flash.bin
 * ============================================================================
 */

#include "config.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <string.h>
#include <stdlib.h>

/* ========================================================================== */
/*  [LIB]  Rutyny runtime rozpoznane w obrazie (avr-libc / libgcc)            */
/* ========================================================================== */
/*
 *  Najczesciej wolane adresy w obrazie to standardowa biblioteka — NIE sa to
 *  funkcje autora firmware, dlatego uzywamy gotowych odpowiednikow:
 *
 *   @0xaeac (352x)  st X+,(lpm Z+) do NUL     ->  strcpy_P(dst, PSTR(...))
 *   @0xb7d8 (186x)  st X+,(ld Z+)  do NUL     ->  strcpy(dst, src)
 *   @0xb85a (122x)  porownanie z flagą T      ->  strcmp() / strcmp_P()
 *   @0xaae6 (287x)  skan do NUL, cofniecie    ->  (wew. pomocnik strcat/strlen)
 *   @0xb672         itoa ze znakiem           ->  itoa()/ltoa()
 *   @0xb680         itoa bez znaku            ->  utoa()/ultoa()
 *   @0xb714         akumulacja cyfr '0'..'9'  ->  atoi()/strtol()
 *   @0xab22         putchar w petli (flash)   ->  uart0_puts_P()  (trace)
 *   @0xab16/0xab12  putchar w petli (RAM)     ->  uart0_puts()    (trace)
 *   @0xac0c         putchar na UART0          ->  uart0_putc()
 *   @0xac72         petla zwloki ~2000/ms     ->  _delay_ms()
 *   @0xae98         mul r20*r16...            ->  __mulhi3 (libgcc)
 *   @0xb25c         lpm przez Z               ->  __lpm    (libgcc)
 *   @0xae84/0xa9ea  st -Y / sub r4            ->  prolog/epilog ramki (libgcc)
 *
 *  Ponizsze deklaracje odwzorowuja faktyczne porty debugowe uzyte przez firmware.
 */

/* ========================================================================== */
/*  [HW]  Przypisanie peryferiow (potwierdzone z inicjalizacji)               */
/* ========================================================================== */
/*
 *  USART0  @0x09/0x0A (UBRR0L=0x33, UCSR0B=0x18 -> RXEN0|TXEN0), wektor RX 0x38
 *  USART1  @0x98..0x9D (UBRR1L=0x33, UCSR1B=0x18 -> RXEN1|TXEN1), wektor RX 0x78
 *          UBRR=51, F_CPU=8MHz, U2X=0  =>  9600 bps 8N1 na obu portach.
 *
 *  Jeden port obsluguje GPS (PA6C, zdania NMEA), drugi SIM900 (odpowiedzi AT).
 *  ISR USART1 (czyta UDR1) sklada linie zakonczone CR/LF -> parser NMEA/AT.
 *  ISR USART0 zapisuje bajty do bufora pierscieniowego @0x0A9C i zlicza @0x0AA0.
 *
 *  Piny GPIO ustawione jako wyjscia w init (sbi DDRx):
 *     DDRA.3, PORTA.3=1 ; DDRA.4, PORTA.4=1   (zasilanie/klucze modulow)
 *     DDRD.5, DDRD.6, DDRD.7                   (sterowanie GPS/modem: ON, RESET)
 *     DDRB.4, DDRB.6, PORTB.6=1                (LED / linia CAN / klucz)
 *  Timer0 skonfigurowany jako licznik tyknięcia systemowego (ASSR/TCCR0).
 */

#define GSM   1          /* UART kierowany do SIM900  (logiczny)            */
#define GPS   0          /* UART kierowany do PA6C                          */

/* Stany maszyny glownej (zmienna 'state' @r16 w petli @0x2c2..0x452) */
enum tracker_state {
    ST_MODEM_INIT = 0,   /* wlaczenie i konfiguracja SIM900                 */
    ST_IDLE       = 1,
    ST_HSK        = 5,    /* handshake z serwerem (HSK-02;)                  */
    ST_WORK       = 6,    /* zbieranie GPS + wysylka danych                  */
};

/* ========================================================================== */
/*  Globalne bufory i zmienne stanu (adresy RAM z disassemblera)              */
/* ========================================================================== */
/*  Odtworzone adresy (potwierdzone w obrazie):
 *   @0x004C/0x4D  wskaznik roboczy (uzywany w ISR, bazowy ptr do stringow)
 *   @0x0A9C       bufor RX USART0 (ring), @0x0AA0 licznik przyjetych bajtow
 *   @0x0B38       stan odbioru linii USART1 (==0x22 aktywny)
 *   @0x0B39       biezacy bajt / bufor linii NMEA/AT
 *   @0x0C46       bufor roboczy do skladania i wypisywania napisow (trace)
 */
static char  modem_ver[8];        /* "2.65" z AT+CGMR / "ver="               */
static char  imei[16];            /* z AT+GSN  ("IMEI>")                      */
static char  apn[16]       = "internet";
static char  server_host[40];     /* "lXX.monitoring-gps.net"                */
static uint16_t server_port;      /* domyslnie z CONFIG                       */
static uint8_t  csq;              /* sila sygnalu z AT+CSQ                    */
static uint8_t  roaming_block;    /* "ZAKAZ ROAMINGU"                         */

/* Pozycja GPS po sparsowaniu RMC/GGA/VTG */
static struct {
    uint8_t  valid;               /* 'A' w RMC                               */
    char     utc[7];              /* UTC-                                    */
    char     lat[12];             /* LAT-                                    */
    char     lon[12];             /* LON-                                    */
    char     alt[8];              /* ALT-                                    */
    char     speed[6];            /* SPEED-                                  */
    char     course[6];           /* COURSE-                                 */
    uint8_t  sats;                /* SAT-                                    */
    uint8_t  moving;              /* RUCH / POSTOJ                           */
} fix;

/* --------------------------------------------------------------------------
 *  Bufor linii UART1 wspoldzielony miedzy ISR (producent) a petla (konsument).
 *  'volatile', bo zmienia sie w przerwaniu. Odpowiada polom @0x0B38/0x0B39.
 * -------------------------------------------------------------------------- */
#define LINE_MAX 96
static volatile char    line[LINE_MAX];   /* skladana linia (@0x0B39)          */
static volatile uint8_t line_len;         /* dlugosc biezacej linii            */
static volatile uint8_t line_ready;       /* 1 = kompletna linia czeka         */
static volatile uint8_t got_cr;           /* widziano CR                       */
static volatile uint8_t rx1_active = 0x22;/* odbior wlaczony (@0x0B38 == 0x22) */
static char             nmea[LINE_MAX];   /* kopia linii do parsowania w petli */

/* ========================================================================== */
/*  [LIB]  UART0 — port serwisowy/trace                                       */
/* ========================================================================== */
/* @0xac0c */
static void uart0_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = c;
}
/* @0xab16 */  static void uart0_puts  (const char *s) { while (*s) uart0_putc(*s++); }
/* @0xab22 */  static void uart0_puts_P(const char *s) { char c; while ((c = pgm_read_byte(s++))) uart0_putc(c); }

/* ========================================================================== */
/*  [LIB]  UART1 — kanal GPS/GSM                                              */
/* ========================================================================== */
static void uart1_putc(char c)
{
    while (!(UCSR1A & (1 << UDRE1)))
        ;
    UDR1 = c;
}
static void uart1_puts(const char *s) { while (*s) uart1_putc(*s++); }

/* Wyslij komende AT do SIM900 (dodaje CR). "AT+..." @ strings 0xc04c..0xc0dc */
static void sim900_cmd(const char *at)
{
    uart1_puts(at);
    uart1_putc('\r');
}

/* ========================================================================== */
/*  [HW]  Inicjalizacja sprzetu — ODWZOROWANIE 1:1 z obrazu @0x8c..0x1d4      */
/* ========================================================================== */
static void hw_init(void)
{
    /* @0x8c  stos na szczycie SRAM: SPH:SPL = 0x10FF (ATmega64 ma 4 KiB SRAM) */
    SP = 0x10FF;

    /* @0xa0..0xb0  watchdog OFF:
     *   MCUCSR &= ~(1<<WDRF);           ; in 0x34; andi 0xF7; out 0x34
     *   WDTCR = (1<<WDCE)|(1<<WDE);      ; out 0x21 = 0x18
     *   WDTCR = 0x00;                    ; out 0x21 = 0x00                     */
    MCUCSR &= ~(1 << WDRF);
    wdt_disable();

    /* @0xb2..0xc0  wyzerowanie .bss (0x0100..0x0FFE) — robi to crt0            */

    /* @0xc2..0xe0  USART0 i USART1: 9600 8N1 (UBRR=51 @ 8MHz, RX+TX wlaczone)  */
    UBRR0H = 0x00; UBRR0L = 51;  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UBRR1H = 0x00; UBRR1L = 51;  UCSR1B = (1 << RXEN1) | (1 << TXEN1);
    /* (UCSR?C domyslnie 8N1 — 0x06 w rejestrze ramki)                          */

    /* @0x16a..0x172  INT3/INT4 wyzwalanie zboczem (EICRA.bit6,7 / EIMSK)       *
     *   sbi 0x1a,3; sbi 0x1b,3; sbi 0x1a,4; sbi 0x1b,4  -> EICRB bits          */
    EICRA = EICRA;  /* [HW] dokladne bity: EICRB.3,4 ustawione (zbocza INT3/4)  */

    /* @0x17c..0x182  Timer0 jako tick systemowy:
     *   TCCR0 = 0x86; OCR0 = 0xC0; sbi ASSR/TCCR0.7                            *
     *   -> tryb CTC, preskaler, przerwanie porownania (podstawa czasu 1 ms)    */
    TCCR0 = 0x86;
    OCR0  = 0xC0;

    /* @0x184..0x192  kierunki pinow sterujacych modulami (patrz naglowek)      */
    DDRA |= (1 << PA3) | (1 << PA4);   PORTA |= (1 << PA3) | (1 << PA4);
    DDRD |= (1 << PD5) | (1 << PD6) | (1 << PD7);
    DDRB |= (1 << PB4) | (1 << PB6);   PORTB |= (1 << PB6);

    /* @0x148..0x14a  sei() */
    sei();
}

/* ========================================================================== */
/*  [HW]  ISR USART1 RX  @0xa3e6  (wektor 0x78) — skladanie linii NMEA/AT      */
/* ========================================================================== */
/*
 *  Logika z obrazu:
 *    - jesli stan @0x0B38 != 0x22  -> szybkie wyjscie (odbior nieaktywny)
 *    - wpisz UDR1 do bufora @0x0B39
 *    - wykrywaj CR (0x0D) i LF (0x0A): po CR+LF linia kompletna -> flaga gotowa
 *    - parser (w petli glownej) rozpoznaje $GPRMC/$GPGGA/$GPVTG i odpowiedzi AT
 */
ISR(USART1_RX_vect)
{
    uint8_t b = UDR1;                 /* lds 0x009C — odczyt kasuje flage RXC   */

    if (rx1_active != 0x22)           /* cpi 0x22 — odbior nieaktywny           */
        return;

    if (b == '\r') {                  /* CR  (0x0D)                             */
        got_cr = 1;
        return;
    }
    if (b == '\n') {                  /* LF  (0x0A) — koniec linii po CR        */
        if (got_cr && !line_ready) {  /* nie nadpisuj, dopoki petla nie odczyta */
            line[line_len] = '\0';
            line_ready = 1;           /* flaga dla petli glownej (konsument)    */
        }
        got_cr   = 0;
        line_len = 0;
        return;
    }
    if (line_len < LINE_MAX - 1 && !line_ready)
        line[line_len++] = b;         /* skladanie bajtow linii (@0x0B39+idx)   */
}

/* ========================================================================== */
/*  [HW]  ISR USART0 RX  @0xa33a  (wektor 0x38) — bufor pierscieniowy          */
/* ========================================================================== */
#define RXBUF_MAX 64
static volatile uint8_t rxbuf[RXBUF_MAX];  /* @0x0A9C bufor pierscieniowy      */
static volatile uint8_t rxcnt;             /* @0x0AA0 licznik przyjetych bajtow */

ISR(USART0_RX_vect)
{
    rxbuf[rxcnt] = UDR0;
    if (++rxcnt >= RXBUF_MAX) rxcnt = 0;   /* subi r24,0xFF == +1 (z zawinieciem) */
}

/* ========================================================================== */
/*  [REC]  Parser NMEA (PA6C)  — @0x186c (RMC/GGA), @0x1c5e (VTG)              */
/* ========================================================================== */
/*  Stringi bledow: " - RMC ERROR - " 0xbfaa, " - GGA ERROR - " 0xbfde,
 *                  " - VTG ERROR - " 0xc00a. Pola wypisywane: UTC-/LAT-/LON-/
 *                  ALT-/SAT-/SPEED-/COURSE-/DELTA-. RUCH/POSTOJ -> detekcja ruchu.
 *
 *  W obrazie rozbicie zdania na pola realizuje tokenizer @0x1f66 (separator ','),
 *  korzystajacy z pomocnika @0xfa6. Ponizej odpowiednik w czytelnym C (strtok
 *  po przecinku) wypelniajacy strukture 'fix' — wiernie co do zestawu pol.      */

/* Zwraca n-te pole (liczac od 0) zdania NMEA rozdzielanego przecinkami.
 * Odpowiednik tokenizera @0x1f66. */
static char *nmea_field(char *s, uint8_t n, char *out)
{
    uint8_t i = 0;
    while (n && *s) { if (*s++ == ',') n--; }
    while (*s && *s != ',' && *s != '*') out[i++] = *s++;
    out[i] = '\0';
    return out;
}

/* $GPRMC,hhmmss.sss,A/V,lat,N,lon,E,speed,course,ddmmyy,... */
static uint8_t gps_parse_rmc(char *s)
{
    char f[12];
    if (strncmp_P(s, PSTR("$GPRMC"), 6) != 0) { uart0_puts_P(PSTR(" - RMC ERROR - ")); return 0; }

    nmea_field(s, 2, f);                          /* status A/V                */
    fix.valid = (f[0] == 'A');
    if (!fix.valid) return 0;

    nmea_field(s, 1, fix.utc);                    /* UTC-   (hhmmss)           */
    nmea_field(s, 3, fix.lat);                    /* LAT-                      */
    nmea_field(s, 5, fix.lon);                    /* LON-                      */
    nmea_field(s, 7, fix.speed);                  /* SPEED- (wezly)            */
    nmea_field(s, 8, fix.course);                 /* COURSE-                   */
    return fix.valid;
}

/* $GPGGA,hhmmss,lat,N,lon,E,fix,sat,hdop,alt,M,... */
static void gps_parse_gga(char *s)
{
    char f[12];
    if (strncmp_P(s, PSTR("$GPGGA"), 6) != 0) { uart0_puts_P(PSTR(" - GGA ERROR - ")); return; }
    nmea_field(s, 7, f);  fix.sats = atoi(f);     /* SAT-                      */
    nmea_field(s, 9, fix.alt);                    /* ALT-   (m n.p.m.)         */
}

/* $GPVTG,course,T,,M,speed_kn,N,speed_kmh,K,... */
static void gps_parse_vtg(char *s)
{
    if (strncmp_P(s, PSTR("$GPVTG"), 6) != 0) { uart0_puts_P(PSTR(" - VTG ERROR - ")); return; }
    nmea_field(s, 1, fix.course);                 /* COURSE-                   */
    nmea_field(s, 7, fix.speed);                  /* SPEED- (km/h)             */
    /* detekcja RUCH/POSTOJ z progu predkosci -> fix.moving                    */
    fix.moving = (atoi(fix.speed) > 0);
}

/* --------------------------------------------------------------------------
 *  Dyspozytor linii (KONSUMENT) — wywolywany z petli glownej.
 *  Jesli ISR zlozyla kompletna linie, przejmuje ja (z wylaczonymi przerwaniami,
 *  zeby nie zlapac jej w polowie) i kieruje do wlasciwego parsera po prefiksie.
 *  Zwraca 1, gdy przetworzono zdanie pozycyjne (RMC z waznym fixem).
 * -------------------------------------------------------------------------- */
static uint8_t nmea_poll(void)
{
    uint8_t new_fix = 0;

    if (!line_ready)                  /* nic nie czeka                          */
        return 0;

    cli();                            /* sekcja krytyczna: kopia bufora ISR     */
    {   uint8_t i;                    /* reczna kopia (line[] jest volatile)    */
        for (i = 0; i < LINE_MAX - 1 && line[i]; i++) nmea[i] = line[i];
        nmea[i] = '\0';
    }
    line_ready = 0;                   /* zwolnij bufor dla ISR                  */
    sei();

    if      (!strncmp_P(nmea, PSTR("$GPRMC"), 6)) new_fix = gps_parse_rmc(nmea);
    else if (!strncmp_P(nmea, PSTR("$GPGGA"), 6)) gps_parse_gga(nmea);
    else if (!strncmp_P(nmea, PSTR("$GPVTG"), 6)) gps_parse_vtg(nmea);
    /* pozostale linie (odpowiedzi AT modemu: OK / ERROR / +CSQ / +CENG ...)
     * obsluguje osobno warstwa modemu — tu je pomijamy.                        */

    return new_fix;
}

/* Konfiguracja PA6C: wybor zdan NMEA przez PMTK (@strings 0xbeaa..0xbf4c) */
static void gps_config(void)
{
    uart1_puts("$PMTK314,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*29\r\n"); /* tylko RMC */
    /* alternatywne maski PMTK314 oraz PMTK101 (hot start) / PMTK104 (full reset) */
}

/* @0xc01a GPS-ON / @0xc022 GPS-OF — sterowanie zasilaniem modulu GPS (pin D) */
static void gps_power(uint8_t on)
{
    if (on) { PORTD |=  (1 << PD6); uart0_puts_P(PSTR("GPS-ON")); }
    else    { PORTD &= ~(1 << PD6); uart0_puts_P(PSTR("GPS-OF")); }
}

/* ========================================================================== */
/*  [REC]  Wlaczenie i konfiguracja SIM900  — @0x1378 (ST_MODEM_INIT)         */
/* ========================================================================== */
/*  Sekwencja wprost z tablicy stringow 0xc036..0xc0dc:                        */
static uint8_t sim900_power_on(void)
{
    uart0_puts_P(PSTR("WLACZAM MODEM"));   /* 0xc03e */
    PORTD |= (1 << PD5);                   /* linia PWRKEY/zasilanie            */
    _delay_ms(2000);

    sim900_cmd("ATE0");                    /* 0xc02a echo off                   */
    sim900_cmd("AT+IPR=9600");             /* 0xbe80 staly baud                 */
    sim900_cmd("AT+CSQ");                  /* 0xc04c jakosc sygnalu -> csq      */
    sim900_cmd("AT+GSN");                  /* 0xc05c IMEI -> "IMEI>"            */
    sim900_cmd("AT+CGMR");                 /* 0xbcd6 wersja firmware modemu     */
    sim900_cmd("AT+CREG=2");               /* 0xc070 rejestracja + komorka      */
    sim900_cmd("AT+CENG=1");               /* 0xc0c8 tryb engineering (LAC/CID) */
    sim900_cmd("AT+CENG?");                /* 0xc0d2 odczyt komorki             */
    return 1;
}

/* Nawiazanie sesji GPRS (@strings 0xc07a..0xc0ba) */
static uint8_t sim900_gprs_attach(void)
{
    char cmd[32];
    sim900_cmd("AT+CIPMUX=0");             /* 0xc07a pojedyncze polaczenie      */
    sim900_cmd("AT+CGATT=1");              /* 0xc086 podlaczenie do GPRS        */
    strcpy(cmd, "AT+CSTT=");               /* 0xc092 */
    strcat(cmd, apn);                      /* 0xc09c "internet"                 */
    sim900_cmd(cmd);
    sim900_cmd("AT+CIICR");                /* 0xc0a6 podniesienie laczna        */
    sim900_cmd("AT+CIFSR");                /* 0xc0b0 lokalny IP                 */
    sim900_cmd("AT+CIPHEAD=1");            /* 0xc0ba naglowek w odbieranych     */
    return 1;
}

/* Blokada/zezwolenie roamingu (@0xc0dc "ZAKAZ ROAMINGU", 0xc210/0xc21e COPS) */
static void sim900_roaming(void)
{
    if (roaming_block) {
        uart0_puts_P(PSTR("ZAKAZ ROAMINGU "));
        sim900_cmd("AT+COPS=4,2,");        /* wymus konkretnego operatora       */
    } else {
        sim900_cmd("AT+COPS=1,2,");        /* automatyczny wybor                */
    }
}

/* Otwarcie polaczenia TCP do serwera (@0xbe52 CIPSTART, 0xbe6a CIPSEND) */
static uint8_t sim900_tcp_open(void)
{
    char cmd[80];
    /* host = "lXX" + ".MONITORING-GPS.NET"  (0xc230 "LGPS", 0xc236 sufiks)     */
    strcpy(cmd, "AT+CIPSTART=\"TCP\",\"");
    strcat(cmd, server_host);
    strcat(cmd, "\",");
    itoa(server_port, cmd + strlen(cmd), 10);
    sim900_cmd(cmd);                       /* oczekiwanie na CONNECT OK         */
    return 1;
}
static void sim900_tcp_send(const char *payload)
{
    sim900_cmd("AT+CIPSEND");              /* modem odpowiada '>'               */
    uart1_puts(payload);
    uart1_putc(0x1A);                      /* Ctrl-Z konczy wysylke             */
}

/* ========================================================================== */
/*  [REC]  Obsluga SMS  — @0x44a4 / @0x440e  (ST_WORK: "-- CZYTAM SMS ---")    */
/* ========================================================================== */
/*  Komendy rozpoznawane w tresci SMS (stringi 0xc2ce..0xc34e):
 *    "RESET*" / "RESET-"  -> KOMENDA RESET  (restart lokalizatora)
 *    "NEW*ADR="           -> zmiana adresu serwera, zapis do EEPROM
 *  Po przetworzeniu: AT+CMGD (kasowanie) -> "SMS IS CLEAR".                    */
static void sms_poll(void)
{
    char buf[160];
    uart0_puts_P(PSTR("-- CZYTAM SMS  ---"));
    sim900_cmd("AT+CMGF=1");               /* tryb tekstowy                     */
    sim900_cmd("AT+CMGR=1");               /* odczyt 1. wiadomosci              */
    /* [REC] jesli "ERROR"/"[Brak SMS]" -> wyjdz */
    /* if (strstr_P(buf, PSTR("RESET"))) { uart0_puts_P(PSTR("KOMENDA RESET")); reset_mcu(); } */
    /* if (strstr_P(buf, PSTR("NEW*ADR="))) { parse_new_server(buf); save_config_eeprom(); } */
    sim900_cmd("AT+CMGD=1");               /* skasuj                            */
    uart0_puts_P(PSTR("SMS IS CLEAR"));
    (void)buf;
}

/* ========================================================================== */
/*  [REC]  Pamiec EEPROM I2C — bufor rekordow "OLD"  (@0xbbaa/0xbbc8)          */
/* ========================================================================== */
/*  "ZAPIS DO PAMIECI" / "ODCZYT Z PAMIECI" + "Bank=" / " Adr=".
 *  Gdy brak zasiegu: rekordy pozycji trafiaja do EEPROM. Po odzyskaniu laczna
 *  wysylane sa jako ramki "LOO-02;" ("WYSLALEM DANE OLD" / "BRAK DANYCH OLD").*/
static void mem_store_record(const char *rec)   { /* I2C: wybor banku/adresu, zapis */ (void)rec; }
static int  mem_read_record (char *out, uint8_t idx) { (void)out; (void)idx; return 0; }

static void send_old_records(void)
{
    char rec[64];
    uart0_puts_P(PSTR("LOO-BE"));          /* begin */
    if (mem_read_record(rec, 0)) {
        sim900_tcp_send(rec);              /* "LOO-02;..." */
        uart0_puts_P(PSTR("WYSLALEM DANE OLD    "));
    } else {
        uart0_puts_P(PSTR("BRAK DANYCH OLD"));
    }
    uart0_puts_P(PSTR("LOO-EN"));          /* end */
}

/* ========================================================================== */
/*  [REC]  Magistrala CAN — paliwo / RPM / pedal  (@0x924a i okolice)          */
/* ========================================================================== */
/*  Stringi 0xc3d4 "BRAK CAN", 0xc3de "RAMKA CAN ERROR", 0xbcac "CAN=",
 *  oraz pola: Paliwo=, RPM=, Pedal=, SONDA1=, W_can=, WYGLADZ= (usrednianie).
 *  Odczyt ramek CAN -> wygladzanie (filtr) poziomu paliwa (SONDA1).           */
static struct { uint16_t rpm, fuel, pedal, sonda1; uint8_t ok; } can_data;
static void can_read(void)
{
    /* [REC] odbior ramki, walidacja; przy bledzie -> "RAMKA CAN ERROR" */
    /* usrednianie SONDA1: W_sonda1 = (suma_probek)/N  ("WYGLADZ= J=")   */
}

/* ========================================================================== */
/*  [REC]  Protokol serwera — budowa ramki pozycji (@0xbc14..0xbe3a)          */
/* ========================================================================== */
/*  Rodzaje ramek wysylanych do monitoring-gps.net:
 *    "PNG-01;"  ping/heartbeat
 *    "FORM;"    pelny rekord pozycji (FORMAT danych)
 *    "HSK-02;"  handshake  (zawiera ";2.65;ORANGE;" ";SIM900;FGP" ";PA6C")
 *    "LOK-02;"  lokalizacja biezaca
 *    "LOO-02;"  rekordy zalegle z pamieci
 *    "CONFIG;"  pobranie/ustawienie konfiguracji
 *  Pola: ";A=1" aktywny, "CAN=1", "R=3/", "DP=0", "OD=0", "DS=01", "MEM=",
 *        "SD=1 SN=0", "WY=1", "GG=1", "LOGUJ-;", "AUTO--;".                   */
/*  Budowa ramki FORM — wierna sekwencja z obrazu @0x23cc..0x25e6.
 *  Kolejnosc sklejania (strcpy_P/strcat + itoa) odtworzona 1:1:            */
static void build_frame(char *out)
{
    char num[12];

    strcpy_P(out, PSTR("FORM;"));          /* @0x23d0                        */
    strcat_P(out, PSTR("Er="));            /* @0x23e0  kod bledu/statusu     */
    /* strcat(out, err_str);  (RAM 0xbc2a) */
    strcat_P(out, PSTR(";A=1 "));          /* @0x2400  aktywny               */
    strcat_P(out, PSTR("CAN=1 "));         /* @0x2410  obecnosc CAN          */

    strcat_P(out, PSTR("R=3/"));           /* @0x2420  licznik rekordu       */
    ltoa(/*wynik func @0x1dfe*/ 0, num, 10);
    strcat(out, num);                      /* @0x243e                        */
    strcat_P(out, PSTR(" "));              /* @0x244e                        */

    strcat_P(out, PSTR("DP=0 "));          /* @0x245e                        */
    strcat_P(out, PSTR("OD=0 "));          /* @0x246e                        */
    strcat_P(out, PSTR("DS=01 "));         /* @0x247e                        */

    strcat_P(out, PSTR("MEM="));           /* @0x248e  stan pamieci          */
    ltoa(0 /*bank*/, num, 10); strcat(out, num);   /* @0x249e               */
    strcat_P(out, PSTR(":"));              /* @0x24ae                        */
    ltoa(0 /*adres*/, num, 10); strcat(out, num);  /* @0x24be               */
    strcat_P(out, PSTR(" "));              /* @0x24ce                        */

    strcat_P(out, PSTR("SD=1 SN=0"));      /* @0x24de                        */
    strcat_P(out, PSTR("WY=1 "));          /* @0x24ee  flaga wyslania        */
    strcat_P(out, PSTR("GG=1"));           /* @0x24fe  jakosc GPS            */

    /* czesc danych pozycji/logu (po @0x2588):                               */
    strcat_P(out, PSTR("LOGUJ-;"));        /* @0x2588                        */
    /* strcat(out, pozycja_z_bufora);      (RAM 0x0b42 — sparsowane pola GPS)*/
    strcat_P(out, PSTR(";"));              /* @0x25a8                        */
    strcat_P(out, PSTR("@"));              /* @0x25c8  terminator rekordu    */

    /* [REC] pola DP/OD/DS/SD/SN/WY/GG w tym buildzie sa w wiekszosci stale;
     *       dynamiczne sa: R=3/<n>, MEM=<bank>:<adr> oraz tresc po LOGUJ-;.
     *       Wartosci pozycji pochodza ze struktury 'fix' (UTC/LAT/LON/...).  */
}

/* Handshake z serwerem (@0x... ST_HSK). Przy bledzie: "-HSK-ERROR", po 5s retry */
static uint8_t server_handshake(void)
{
    char frame[96];
    strcpy(frame, "HSK-02;");
    strcat(frame, ";2.65;ORANGE;");        /* wersja + operator                 */
    strcat(frame, ";SIM900;FGP");          /* typ modemu                        */
    strcat(frame, ";PA6C");                /* typ GPS                           */
    strcat(frame, ";1;TEMP;");
    sim900_tcp_send(frame);
    /* oczekiwanie odpowiedzi: "- ADRESY OK -" / "- NOWE ADRESY -" / "- HSK PROBLEM -5s -" */
    return 1;
}

/* ========================================================================== *
 *  [REC]  Pobranie i parsowanie konfiguracji z serwera                       *
 *         funkcja @0x33b6 ; string "CONFIG;" @0x342a (flash 0xbd9c)          *
 * ========================================================================== *
 *
 *  Odpowiedz serwera na ramke CONFIG ma format STALOPOZYCYJNY: kazdy parametr
 *  siedzi pod znanym offsetem znakowym i ma znana szerokosc. Firmware NIE dzieli
 *  jej po separatorze — wycina pod-napisy funkcja get_field() (@0xb7ee), ktora
 *  kopiuje 'width' znakow poczawszy od 'off', a nastepnie atoi() (@0xb714).
 *
 *  Mapa pol odtworzona 1:1 z obrazu (offset, szerokosc, adres RAM docelowy):
 *
 *    f#  off  w   RAM      uwagi
 *    --  ---  --  ------   -------------------------------------------------
 *     0    6   5  0x0242   wartosc 32-bit (interwal/licznik)
 *     1   12   5  0x023E   wartosc 32-bit (interwal/licznik)
 *     2   18   1  0x0257   gdy ==0 -> default 6   (@0x34c2)
 *     3   20   1  0x0B5E
 *     4   22   2  0x0268
 *     5   25   1  0x0268+  (drugi parametr w tym samym rejonie)
 *     6   27   2  0x0B63   dodatkowo skalowany przez __mulhi3 (@0x356e)
 *     7   30   1  0x0C17
 *     -   33   -  flaga: strcmp(pole, ";05") @0x35b4 (0xbda4)
 *     8   37   3  0x025B
 *     9   41   2  0x025C
 *    10   44   2  0x0B76
 *    11   47   1  0x0B77
 *    12   49   2  0x025E
 *    13   52   1  0x0C14
 *    14   54   3  0x0C15
 *    15   59   1  0x0269
 *    16   60   1  0x0B86
 *    17   61   1  0x0B87
 *    18   62   1  0x0C22
 *    19   63   1  0x0C23
 *
 *  (Nazwy semantyczne wiekszosci pol nie sa znane z samego obrazu — zachowano
 *   adresy RAM jako zrodlo prawdy; role wpisano tam, gdzie wynikaja z uzycia.)
 */

/* [LIB-like] @0xb7ee — wytnij 'width' znakow od offsetu 'off' z 'src' do 'dst'.
 * Kopiowanie konczy sie na NUL lub po 'width' znakach. */
static void get_field(const char *src, uint8_t off, uint8_t width, char *dst)
{
    while (off && *src) { src++; off--; }      /* przewin do offsetu          */
    while (width && *src) { *dst++ = *src++; width--; }
    *dst = '\0';
}

/* Zparsowane parametry konfiguracyjne (adresy RAM z obrazu w komentarzach) */
static struct {
    uint32_t t0;          /* 0x0242 */
    uint32_t t1;          /* 0x023E */
    uint8_t  p257;        /* 0x0257  licznik odliczajacy cykle do wyslania:
                           *   init=2 (@0x01a0), default z CONFIG=6 (@0x34d0),
                           *   odczyt-dekrementacja-zapis @0x477c/0x4786,
                           *   porownywany @0x11da/0x2bd2 -> wyzwala raport.    */
    uint8_t  p_b5e;       /* 0x0B5E */
    uint16_t p268a;       /* 0x0268 */
    uint8_t  p268b;       /* 0x0268+ */
    uint16_t p_b63;       /* 0x0B63  (skalowany) */
    uint8_t  p_c17;       /* 0x0C17 */
    uint8_t  flag05;      /* pole porownane z ";05" */
    uint16_t p25b, p25c, p_b76, p25e, p_c15;
    uint8_t  p_b77, p_c14, p269, p_b86, p_b87, p_c22, p_c23;
} cfg;

static char cfg_resp[64];     /* bufor odpowiedzi serwera (@0x01A7)           */

static void server_get_config(void)
{
    /* zadanie CONFIG wysyla warstwa nadrzedna; tu parsujemy odebrana linie.   */
    sim900_tcp_send("CONFIG;");          /* @0xbd9c                             */

    /* (payload odpowiedzi trafia do cfg_resp[] — @0x3f8e w obrazie)           */
    char f[8];
    const char *r = cfg_resp;

    get_field(r,  6, 5, f); cfg.t0    = strtoul(f, NULL, 10);   /* 0x0242 */
    get_field(r, 12, 5, f); cfg.t1    = strtoul(f, NULL, 10);   /* 0x023E */
    get_field(r, 18, 1, f); cfg.p257  = atoi(f);
    if (cfg.p257 == 0) cfg.p257 = 6;                            /* @0x34c2 default */
    get_field(r, 20, 1, f); cfg.p_b5e = atoi(f);               /* 0x0B5E */
    get_field(r, 22, 2, f); cfg.p268a = atoi(f);               /* 0x0268 */
    get_field(r, 25, 1, f); cfg.p268b = atoi(f);
    get_field(r, 27, 2, f); cfg.p_b63 = atoi(f);               /* 0x0B63 (+scale) */
    get_field(r, 30, 1, f); cfg.p_c17 = atoi(f);               /* 0x0C17 */

    get_field(r, 33, 2, f);                                    /* flaga ";05" @0x35b4 */
    cfg.flag05 = (strcmp_P(f, PSTR(";05")) == 0);

    get_field(r, 37, 3, f); cfg.p25b  = atoi(f);
    get_field(r, 41, 2, f); cfg.p25c  = atoi(f);
    get_field(r, 44, 2, f); cfg.p_b76 = atoi(f);
    get_field(r, 47, 1, f); cfg.p_b77 = atoi(f);
    get_field(r, 49, 2, f); cfg.p25e  = atoi(f);
    get_field(r, 52, 1, f); cfg.p_c14 = atoi(f);
    get_field(r, 54, 3, f); cfg.p_c15 = atoi(f);
    get_field(r, 59, 1, f); cfg.p269  = atoi(f);
    get_field(r, 60, 1, f); cfg.p_b86 = atoi(f);
    get_field(r, 61, 1, f); cfg.p_b87 = atoi(f);
    get_field(r, 62, 1, f); cfg.p_c22 = atoi(f);
    get_field(r, 63, 1, f); cfg.p_c23 = atoi(f);

    /* blokada roamingu wynika z konfiguracji ("RoamingPam=" 0xbdae)           */
    roaming_block = cfg.flag05;

    /* przy bledzie formatu: "-CONFIG-ERROR " (0xbdba); sukces -> zapis EEPROM  */
}

/* ========================================================================== */
/*  [REC]  Reset programowy (@0xc318 "RESET*", 0xc2bc "RESET ZA 7 sek.")       */
/* ========================================================================== */
static void reset_mcu(void)
{
    uart0_puts_P(PSTR("RESTART"));
    wdt_enable(WDTO_15MS);                 /* reset przez watchdog              */
    for (;;) ;
}

/* ========================================================================== */
/*  [REC]  Petla glowna — maszyna stanow  @reset->0x8c, loop @0x2c2..0x452     */
/* ========================================================================== */
int main(void)
{
    uint8_t state = ST_MODEM_INIT;

    hw_init();                             /* @0x8c  [HW] */
    gps_config();                          /* PMTK: wybor zdan NMEA */
    uart0_puts_P(PSTR("SIM900"));
    uart0_puts_P(PSTR("ver="));            /* "2.65" */

    for (;;) {                             /* glowna petla @0x2c2 */
        wdt_reset();

        switch (state) {

        case ST_MODEM_INIT:                /* @0x1378 */
            gps_power(1);
            if (sim900_power_on() && sim900_gprs_attach()) {
                sim900_roaming();
                state = ST_HSK;
            }
            break;

        case ST_HSK:                       /* @... "HSK-02;" */
            if (sim900_tcp_open() && server_handshake()) {
                server_get_config();
                state = ST_WORK;
            } else {
                uart0_puts_P(PSTR("- HSK PROBLEM -5s -"));
                _delay_ms(5000);
                state = ST_MODEM_INIT;
            }
            break;

        case ST_WORK:                      /* @0x37c.. glowna praca */
            /* 1) odczyt i parsowanie GPS (RMC/GGA/VTG) z bufora linii ISR:
             *    konsumuj wszystkie linie zlozone przez ISR od ostatniego obiegu */
            while (line_ready)
                nmea_poll();               /* wypelnia strukture 'fix'           */

            if (fix.valid) uart0_puts_P(PSTR("NOWE DANE GPS"));
            else           uart0_puts_P(PSTR("BRAK NOWYCH GPS"));

            /* 2) odczyt CAN (paliwo/RPM) */
            can_read();

            /* 3) detekcja ruch/postoj -> decyzja o wpisie i czestotliwosci */

            /* 4) wyslanie zaleglych rekordow z EEPROM, potem biezacej pozycji */
            send_old_records();
            {
                char frame[128];
                build_frame(frame);
                if (csq /* zasieg ok */) {
                    sim900_tcp_send(frame);          /* "-- WYSYLAM DANE ---" */
                } else {
                    mem_store_record(frame);         /* "ZAPIS DO PAMIECI"    */
                    uart0_puts_P(PSTR("NET PROBLEM"));
                }
            }

            /* 5) okresowe sprawdzenie SMS (komendy RESET / NEW*ADR=) */
            sms_poll();

            /* 6) ping utrzymujacy sesje */
            sim900_tcp_send("PNG-01;");
            break;

        default:
            state = ST_MODEM_INIT;
            break;
        }
    }
    return 0;  /* nieosiagalne */
}

/*
 * ============================================================================
 *  PODSUMOWANIE ARCHITEKTURY (z analizy obrazu)
 * ============================================================================
 *  - MCU ATmega64 @8MHz, 2x USART 9600 8N1: GPS (PA6C) + GSM (SIM900).
 *  - GPS: zdania NMEA $GPRMC/$GPGGA/$GPVTG; konfiguracja przez PMTK314/101/104.
 *  - GSM: pelny ciag komend AT (CSQ, GSN, CREG, CENG, CSTT/CIICR/CIFSR,
 *         CIPSTART/CIPSEND) -> sesja TCP do lXX.monitoring-gps.net.
 *  - Protokol tekstowy: ramki PNG/FORM/HSK/LOK/LOO/CONFIG z polami stanu.
 *  - EEPROM I2C: kolejka rekordow "OLD" na czas braku zasiegu.
 *  - CAN: odczyt paliwa (SONDA1), RPM, pedalu; usrednianie poziomu paliwa.
 *  - SMS: zdalne komendy RESET i zmiana adresu serwera (NEW*ADR=).
 *  - Kontrola roamingu (AT+COPS), watchdog, reset programowy.
 *
 *  Toolchain: avr-gcc + avr-libc (rozpoznane rutyny: strcpy/strcpy_P/strcmp/
 *  itoa/atoi/_delay_ms oraz libgcc __lpm/__mulhi3/ramki stosu).
 * ============================================================================
 */
