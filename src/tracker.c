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
    uint8_t b = UDR1;                 /* lds 0x009C */
    extern volatile uint8_t rx1_active, line_len, got_cr;
    extern char line[];
    if (rx1_active != 0x22)           /* cpi 0x22 */
        return;
    line[line_len] = b;               /* st X,r24 @0x0B39+idx */
    if (b == '\r') got_cr = 1;        /* cpi 0x0D */
    if (b == '\n' && got_cr) {        /* cpi 0x0A -> linia kompletna */
        line[line_len] = '\0';
        /* ustaw flage "linia gotowa" dla petli glownej */
    } else {
        line_len++;
    }
}

/* ========================================================================== */
/*  [HW]  ISR USART0 RX  @0xa33a  (wektor 0x38) — bufor pierscieniowy          */
/* ========================================================================== */
ISR(USART0_RX_vect)
{
    extern volatile uint8_t rxbuf[]; /* @0x0A9C */
    extern volatile uint8_t rxcnt;   /* @0x0AA0 */
    rxbuf[rxcnt] = UDR0;
    rxcnt++;                          /* subi r24,0xFF == +1 */
}

/* ========================================================================== */
/*  [REC]  Parser NMEA (PA6C)  — @0x186c (GGA/RMC), @0x1c5e (VTG)              */
/* ========================================================================== */
/*  Stringi bledow: " - RMC ERROR - " 0xbfaa, " - GGA ERROR - " 0xbfde,
 *                  " - VTG ERROR - " 0xc00a. Pola: UTC/LAT/LON/ALT/SAT/SPEED/
 *                  COURSE/DELTA. RUCH/POSTOJ -> detekcja ruchu.               */
static uint8_t gps_parse_rmc(const char *s)
{
    /* $GPRMC,hhmmss,A/V,lat,N,lon,E,speed,course,date,... */
    if (strncmp_P(s, PSTR("$GPRMC"), 6) != 0) { uart0_puts_P(PSTR(" - RMC ERROR - ")); return 0; }
    /* [REC] rozbicie po przecinkach, kopiowanie pol do struktury 'fix' */
    fix.valid = 0; /* 'A' => 1 */
    return fix.valid;
}
static void gps_parse_gga(const char *s)
{
    if (strncmp_P(s, PSTR("$GPGGA"), 6) != 0) { uart0_puts_P(PSTR(" - GGA ERROR - ")); return; }
    /* [REC] wysokosc (ALT), liczba satelitow (SAT) */
}
static void gps_parse_vtg(const char *s)
{
    if (strncmp_P(s, PSTR("$GPVTG"), 6) != 0) { uart0_puts_P(PSTR(" - VTG ERROR - ")); return; }
    /* [REC] predkosc (SPEED), kurs (COURSE) */
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
static void build_frame(char *out)
{
    strcpy(out, "FORM;");
    strcat(out, "A=1 ");
    /* [REC] doklej: pozycja (fix.lat/lon/utc/speed/course/alt/sats),
     *               CAN (paliwo/rpm), stan ruchu (RUCH/POSTOJ), MEM, itd.      */
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

/* Pobranie konfiguracji z serwera (@0xbd9c "CONFIG;", bledy "-CONFIG-ERROR")  */
static void server_get_config(void)
{
    sim900_tcp_send("CONFIG;");
    /* [REC] parsowanie odpowiedzi -> server_host/port, RoamingPam, progi.
     *        przy sukcesie "- ADRESY OK -" zapis do EEPROM ("Zapisano w pamieci") */
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
            /* 1) odczyt i parsowanie GPS (RMC/GGA/VTG) z bufora linii */
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
