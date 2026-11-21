/*
 * ============================================================================
 *  REKONSTRUKCJA FIRMWARE  —  Lokalizator GPS (fleet tracker)
 * ============================================================================
 *
 *  Plik wejsciowy : flash.bin  (64 KiB, wykorzystane 0x0000..0xC3EE = ~50 KiB)
 *  MCU            : Atmel ATmega64  (avr5)
 *  F_CPU          : 8 000 000 Hz   (potwierdzone: UBRR=51 @ 9600 baud oraz "AT+IPR=9600")
 *  Oryginal       : BASCOM-AVR (BASIC, MCS Electronics) — rozpoznany po kodzie
 *                   startowym, ramce R4:R5, rejestrze flag R6 i bibliotece
 *                   (patrz docs/COMPILER.md). Ta rekonstrukcja: C dla avr-gcc.
 *  Peryferia      : GPS  GlobalTop PA6C  (NMEA, PMTK)
 *                   GSM  SIMCom SIM900   (GPRS, AT)
 *                   CAN  (czujnik paliwa / RPM / pedal)
 *                   pamiec zewnetrzna na programowym I2C (PD0/PD1),
 *                   najpewniej bufor rekordow "OLD" (Bank=/Adr=)
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
 *    [LIB]  - procedury biblioteki BASCOM-AVR rozpoznane po dzialaniu i zastapione
 *             odpowiednikami z avr-libc (strcpy, itoa, _delay_ms, ...).
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
#include <avr/eeprom.h>
#include <stddef.h>
#include <util/delay.h>
#include <string.h>
#include <stdlib.h>

/* ========================================================================== */
/*  [LIB]  Biblioteka BASCOM-AVR w obrazie i jej odpowiedniki w C             */
/* ========================================================================== */
/*
 *  Najczesciej wolane adresy to procedury biblioteki BASCOM-AVR. W rekonstrukcji
 *  zastepujemy je odpowiednikami z avr-libc:
 *
 *   adres    BASCOM (z dzialania)          odpowiednik w C
 *   -------  ----------------------------  ------------------------------------
 *   0xaeac   s = "stala"  (kopia z flash)  strcpy_P(dst, PSTR(...))
 *   0xb7d8   s = t        (kopia w RAM)    strcpy(dst, src)
 *   0xaae6   s = s + ...  (koniec napisu)  strcat (pomocnik)
 *   0xb85a/e If s = "..." / If s = t       strcmp_P() / strcmp()
 *   0xb84e   Len(s)                        strlen()
 *   0xb7ee   Mid(s, poz, dl)  (poz od 1!)  get_field()
 *   0xb882   Instr(...)                    strstr()/strchr()
 *   0xb714   Val(s)  -> Long w R16..R19    atoi()/strtol()
 *   0xb672/0 Str(x)  (ze znakiem/bez)      itoa()/utoa()
 *   0xb4a4   Format(s, "000")              dopelnianie zerami
 *   0xaa14   Incr x  (Word)                x++
 *   0xac72   Waitms n                      _delay_ms()
 *   0xab6a   Wait n  (sekundy)             _delay_ms(1000) w petli
 *   0xab5a   Cls     (LCD 0x01, 0x80)      lcd_clear_home()
 *   0xab40   Lowerline (LCD 0xC0)          lcd_cmd(0xC0)
 *   0xab22   Lcd "stala"                   lcd_puts_P()
 *   0xab16   Lcd s                         lcd_puts()
 *   0xac0c   (bajt na LCD, 4-bit)          lcd_putc()
 *   0xab7c   inicjalizacja LCD (w starcie) lcd_init
 *   0xb9c6/e4 I2cstart / I2cstop           (programowe I2C PD0/PD1)
 *   0xb9f6   I2cwbyte                      (j.w.)
 *   0xba30   I2crbyte                      (j.w.)
 *   0xb81e   zapis EEPROM (gdy r23=0x7B)   eeprom_update_byte()
 *   0xb840   odczyt EEPROM                 eeprom_read_byte()
 *   0xae98   mnozenie 16-bit               operator *
 *   0xb25c   odczyt bajtu z flash (lpm)    pgm_read_byte()
 *   0xae84/0xae90/0xa9ea  ramka R4:R5      (zmienne tymczasowe; w C — stos)
 *
 *  Nazwy BASCOM sa przypisane po dzialaniu procedur; obraz nie zawiera nazw.
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
static char     server_port_str[5]; /* port TCP serwera, np. "3506" (EEPROM 0x01) */
static uint16_t server_port;      /* ta sama wartosc liczbowo (po atoi)       */
static uint8_t  csq;              /* sila sygnalu z AT+CSQ                    */
static char     net_mcc[4];       /* 0x0B6D  MCC biezacej sieci, "260" = PL   */
static uint8_t  roaming_mode = 3; /* tryb roamingu z EEPROM 0x64 (0..7)       */
static uint8_t  roam_op_idx = 1;  /* 0x0C3B  indeks operatora (EEPROM 0x3F)   */
static char     last_reply[16];   /* ostatnia linia odpowiedzi (nie-NMEA)     */

/* Prototypy funkcji zdefiniowanych nizej. */
static void restart_with_reason(uint8_t code);
static void config_save_roam_op(uint8_t idx);
static void config_save_host(const char *host);

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
/*  [HW]  Wyswietlacz LCD HD44780 w trybie 4-bit na PORTC                      */
/* ========================================================================== */
/*  KOREKTA wobec wczesniejszej wersji: to, co wygladalo na "trace UART0",
 *  jest w rzeczywistosci zapisem na LCD. Dowody z obrazu:
 *    @0xac0c  swap r24 + 2x wyslij nibble + impuls E  -> zapis bajtu 4-bit
 *    @0xac18  4x (asr r24; sbi/cbi PORTC.{3,2,1,0})    -> wyslanie nibble
 *    @0xac6c  krotka zwloka (impuls E / busy)
 *    @0xab5a  wysyla komendy 0x01 (Clear) i 0x80 (Home) -> typowe HD44780
 *  Przypisanie pinow (DDRC.0..5 = wyjscia, z init @0xab84):
 *    PC0..PC3 = D4..D7 (szyna danych 4-bit), PC4 = RS, PC5 = E
 *  UART0 (UDR0) sluzy do GPS (RX); napisy statusowe ida na LCD.                */

#define LCD_DATA_PORT  PORTC   /* PC0..PC3 = D4..D7 */
#define LCD_RS         PC4
#define LCD_E          PC5

/* @0xac18 — wyslij 4 bity (nibble) na PC3..PC0 */
static void lcd_nibble(uint8_t v)
{
    if (v & 1) PORTC |=  (1 << PC3); else PORTC &= ~(1 << PC3);
    if (v & 2) PORTC |=  (1 << PC2); else PORTC &= ~(1 << PC2);
    if (v & 4) PORTC |=  (1 << PC1); else PORTC &= ~(1 << PC1);
    if (v & 8) PORTC |=  (1 << PC0); else PORTC &= ~(1 << PC0);
    PORTC |=  (1 << LCD_E);          /* impuls E */
    _delay_us(1);
    PORTC &= ~(1 << LCD_E);
}

/* @0xac0c — wyslij pelny bajt (dwa nibble: starszy, potem mlodszy) */
static void lcd_byte(uint8_t b)
{
    lcd_nibble(b >> 4);
    lcd_nibble(b & 0x0F);
    _delay_us(50);                   /* @0xac6c  czas wykonania              */
}

static void lcd_cmd (uint8_t c) { PORTC &= ~(1 << LCD_RS); lcd_byte(c); }
static void lcd_putc(char c)    { PORTC |=  (1 << LCD_RS); lcd_byte((uint8_t)c); }

/* @0xab5a — Clear (0x01) + Home (0x80) */
static void lcd_clear_home(void) { lcd_cmd(0x01); _delay_ms(2); lcd_cmd(0x80); }

/* @0xab16 */  static void lcd_puts  (const char *s) { while (*s) lcd_putc(*s++); }
/* @0xab22 */  static void lcd_puts_P(const char *s) { char c; while ((c = pgm_read_byte(s++))) lcd_putc(c); }

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
/*  [REC]  Parser NMEA (PA6C)  — funkcja @0x4b20 (RMC ~0x4ca0, GGA ~0x5380,     */
/*         VTG ~0x56a4). UWAGA: 0x186c i 0x1c5e to funkcje EEPROM, nie NMEA.  */
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
    if (strncmp_P(s, PSTR("$GPRMC"), 6) != 0) { lcd_puts_P(PSTR(" - RMC ERROR - ")); return 0; }

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
    if (strncmp_P(s, PSTR("$GPGGA"), 6) != 0) { lcd_puts_P(PSTR(" - GGA ERROR - ")); return; }
    nmea_field(s, 7, f);  fix.sats = atoi(f);     /* SAT-                      */
    nmea_field(s, 9, fix.alt);                    /* ALT-   (m n.p.m.)         */
}

/* $GPVTG,course,T,,M,speed_kn,N,speed_kmh,K,... */
static void gps_parse_vtg(char *s)
{
    if (strncmp_P(s, PSTR("$GPVTG"), 6) != 0) { lcd_puts_P(PSTR(" - VTG ERROR - ")); return; }
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
    else {  /* inne linie: odpowiedzi modemu / serwera (OK, ERROR, ...)       */
        strncpy(last_reply, nmea, sizeof(last_reply) - 1);
        last_reply[sizeof(last_reply) - 1] = '\0';
    }

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
    if (on) { PORTD |=  (1 << PD6); lcd_puts_P(PSTR("GPS-ON")); }
    else    { PORTD &= ~(1 << PD6); lcd_puts_P(PSTR("GPS-OF")); }
}

/* ========================================================================== */
/*  [REC]  Wlaczenie i konfiguracja SIM900  — @0x1378 (ST_MODEM_INIT)         */
/* ========================================================================== */
/*  Sekwencja wprost z tablicy stringow 0xc036..0xc0dc:                        */
static uint8_t sim900_power_on(void)
{
    lcd_puts_P(PSTR("WLACZAM MODEM"));   /* 0xc03e */
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
/* Roaming (@0x6480..0x7600):
 *  - siec 260 (Polska)               -> nic nie robimy ("KRAJ")
 *  - za granica i tryb (EEPROM 0x64) == 0 -> "ZAKAZ ROAMINGU", restart kod 23
 *  - za granica, tryb != 0           -> reczny wybor operatora z tabeli
 *    preferowanych sieci (MCC -> lista MNC, @0x6f00..0x7180; m.in. 262, 278,
 *    242, 268, 226, 231, 293, 240, 216, 234, 222) przez AT+COPS=1,2,<MCCMNC>;
 *    przy braku OK fallback AT+COPS=4,2,... (reczny z powrotem do automatu).
 *    Indeks probowanego operatora (EEPROM 0x3F) rotuje; po 4 probach -> 1.
 *  Tabela MNC nie jest tu przepisana — ponizej tylko logika wyboru.           */
static void sim900_roaming(void)
{
    char cmd[24];
    if (strcmp_P(net_mcc, PSTR("260")) == 0) {
        lcd_puts_P(PSTR("KRAJ"));
        return;
    }
    lcd_puts_P(PSTR("ZAGRANICA"));
    if (roaming_mode == 0) {
        lcd_puts_P(PSTR("ZAKAZ ROAMINGU "));
        restart_with_reason(23);           /* kod 23 @0x671e                    */
    }
    strcpy_P(cmd, PSTR("AT+COPS=1,2,"));   /* reczny wybor operatora            */
    strcat(cmd, net_mcc);                  /* + MNC z tabeli dla roam_op_idx    */
    sim900_cmd(cmd);
    config_save_roam_op((uint8_t)(roam_op_idx + 1));   /* nastepny przy porazce */
}

/* Otwarcie polaczenia TCP do serwera (@0xbe52 CIPSTART, 0xbe6a CIPSEND) */
static uint8_t sim900_tcp_open(void)
{
    char cmd[80];
    /* Skladanie jak w obrazie @0x3d34..0x3dd4:
     *   "AT+CIPSTART=" + "TCP" + "," + host + "," + port
     * host i port pochodza z EEPROM (po walidacji w config_load).              */
    strcpy_P(cmd, PSTR("AT+CIPSTART="));   /* @0x3d34 */
    strcat_P(cmd, PSTR("TCP"));            /* @0x3d54 */
    strcat_P(cmd, PSTR(","));              /* @0x3d74 */
    strcat(cmd, server_host);              /* @0x3d94  np. lgps03.monitoring-gps.net */
    strcat_P(cmd, PSTR(","));              /* @0x3db4 */
    strcat(cmd, server_port_str);          /* @0x3dd4  np. "3506"               */
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
    lcd_puts_P(PSTR("-- CZYTAM SMS  ---"));
    sim900_cmd("AT+CMGF=1");               /* tryb tekstowy                     */
    sim900_cmd("AT+CMGR=1");               /* odczyt 1. wiadomosci              */
    /* [REC] jesli "ERROR"/"[Brak SMS]" -> wyjdz */
    if (strstr_P(buf, PSTR("RESET"))) {    /* "RESET*"/"RESET-" -> restart       */
        lcd_puts_P(PSTR("KOMENDA RESET"));
        restart_with_reason(27);           /* kod 27 @0x864e                  */
    }
    {   char *p = strstr_P(buf, PSTR("NEW*ADR="));   /* zmiana hosta serwera      */
        if (p) { config_save_host(p + 8); }
    }
    sim900_cmd("AT+CMGD=1");               /* skasuj                            */
    lcd_puts_P(PSTR("SMS IS CLEAR"));
    (void)buf;
}

/* ========================================================================== *
 *  [HW]  Konfiguracja w wewnetrznym EEPROM ATmega64                          *
 * ========================================================================== *
 *  Dostep przez sprzetowy EEPROM AVR. Prymitywy w obrazie: @0xb80c (czekaj
 *  EEWE + ustaw EEAR), @0xb81e (zapis r25 bajtow; tylko gdy r23 == 0x7B),
 *  @0xb840 (odczyt r25 bajtow). Na nich zbudowane sa dwie procedury napisowe:
 *    @0x1f66  odczyt napisu: bajt po bajcie od adresu, az do 0xFF
 *    @0x2036  zapis napisu: strlen(s)+1 bajtow (razem z NUL)
 *  Kazde pole ma wlasna pare funkcji (klaster @0x1532..0x1f66).
 *
 *  UKLAD (zweryfikowany ze zrzutem reverse/eeprom.bin i z kodem):
 *
 *    off   zrzut                         znaczenie                 funkcje
 *    ----  ----------------------------  ------------------------  -----------
 *    0x01  "3506"                        port TCP serwera          0x1532/15e0
 *    0x07  0x00                          XOR(port) — suma kontr.   (j.w.)
 *    0x0A  "lgps03.monitoring-gps.net"   host serwera              0x1698/1746
 *    0x2F  0x31 ('1')                    XOR(host) — suma kontr.   (j.w.)
 *    0x3C  "R" (+"n<", patrz nizej)      ostatni stan ruchu R/P    0x17fe/186c
 *    0x3F  "" -> domyslnie 1             indeks operatora roaming. 0x193a/198c
 *    0x42  "021"                         kod ost. restartu (PL)    0x19ee/1aaa
 *    0x5A  "000001"                      licznik restartow bez OK  0x1b72/1c5e
 *    0x64  "3"                           tryb roamingu 0..7        0x1dac/1dfe
 *    0x67  "020"                         kod ost. restartu (≠PL)   0x19ee/1aaa
 *
 *  0x3C: firmware zapisuje tam tylko "R" albo "P"; bajty "n<" po "R" nie
 *  pochodza z zadnego zapisu, ktory znaleziono w tym obrazie (pozostalosc).
 *  0x3C i 0x5A sa dodatkowo kopiowane do zewnetrznej pamieci I2C (0xe42),
 *  patrz docs/EEPROM.md, sekcja "Zewnetrzna pamiec I2C".                      */

struct eeconfig {
    uint8_t  _r00;                 /* 0x00                                     */
    char     port[6];              /* 0x01  "3506"                             */
    uint8_t  port_xor;             /* 0x07  XOR bajtow portu                   */
    uint8_t  _r08[2];              /* 0x08                                     */
    char     host[37];             /* 0x0A  "lgps03.monitoring-gps.net"        */
    uint8_t  host_xor;             /* 0x2F  XOR bajtow hosta                   */
    uint8_t  _r30[12];             /* 0x30                                     */
    char     motion[3];            /* 0x3C  "R" = ruch, "P" = postoj           */
    char     roam_op_idx[3];       /* 0x3F  indeks operatora (tekst, np. "1")  */
    char     reset_code_home[4];   /* 0x42  "021"                              */
    uint8_t  _r46[20];             /* 0x46                                     */
    char     restart_count[7];     /* 0x5A  "000001"                           */
    uint8_t  _r61[3];              /* 0x61                                     */
    char     roaming_mode[2];      /* 0x64  "3"                                */
    uint8_t  _r66;                 /* 0x66                                     */
    char     reset_code_abroad[4]; /* 0x67  "020"                              */
};
static struct eeconfig EEMEM ee_cfg;    /* alokacja od 0x0000 EEPROM           */

/* Kontrola, ze pola struktury trafiaja dokladnie w offsety ze zrzutu: */
_Static_assert(offsetof(struct eeconfig, port)              == 0x01, "port");
_Static_assert(offsetof(struct eeconfig, port_xor)          == 0x07, "port_xor");
_Static_assert(offsetof(struct eeconfig, host)              == 0x0A, "host");
_Static_assert(offsetof(struct eeconfig, host_xor)          == 0x2F, "host_xor");
_Static_assert(offsetof(struct eeconfig, motion)            == 0x3C, "motion");
_Static_assert(offsetof(struct eeconfig, roam_op_idx)       == 0x3F, "roam_op_idx");
_Static_assert(offsetof(struct eeconfig, reset_code_home)   == 0x42, "code_home");
_Static_assert(offsetof(struct eeconfig, restart_count)     == 0x5A, "restart_count");
_Static_assert(offsetof(struct eeconfig, roaming_mode)      == 0x64, "roaming_mode");
_Static_assert(offsetof(struct eeconfig, reset_code_abroad) == 0x67, "code_abroad");

/* Zmienne RAM powiazane z polami EEPROM (adresy RAM z obrazu). */
static uint16_t restart_count;     /* 0x0B6B  restarty bez udanej lacznosci    */
static char     motion_state[4];   /* 0x0266  "R" / "P"                        */

/* @0x1f66 — odczyt napisu z EEPROM (do 0xFF albo NUL). */
static void ee_read_str(const void *ee, char *dst, uint8_t max)
{
    const uint8_t *a = (const uint8_t *)ee;
    uint8_t i = 0;
    while (i < max - 1) {
        uint8_t b = eeprom_read_byte(a++);
        if (b == 0xFF || b == 0x00) break;
        dst[i++] = (char)b;
    }
    dst[i] = '\0';
}

/* @0x2036 — zapis napisu do EEPROM razem z koncowym NUL. */
static void ee_write_str(void *ee, const char *s)
{
    uint8_t *a = (uint8_t *)ee;
    do { eeprom_update_byte(a++, (uint8_t)*s); } while (*s++);
}

/* @0xb498 — XOR wszystkich bajtow napisu (suma kontrolna portu/hosta). */
static uint8_t str_xor(const char *s)
{
    uint8_t x = 0;
    while (*s) x ^= (uint8_t)*s++;
    return x;
}

/* --- port (0x01 + XOR @0x07): zapis @0x1532, odczyt @0x15e0 ------------------ */
static void config_save_port(const char *port)
{
    ee_write_str(ee_cfg.port, port);
    eeprom_update_byte(&ee_cfg.port_xor, str_xor(port));
}
static void config_load_port(void)
{
    ee_read_str(ee_cfg.port, server_port_str, sizeof(server_port_str));
    /* pusto albo zla suma XOR -> domyslny "3506" i zapis z powrotem */
    if (server_port_str[0] == '\0' ||
        eeprom_read_byte(&ee_cfg.port_xor) != str_xor(server_port_str)) {
        strcpy_P(server_port_str, PSTR(SERVER_PORT_DEFAULT));
        config_save_port(server_port_str);
    }
}

/* --- host (0x0A + XOR @0x2F): zapis @0x1698, odczyt @0x1746 ------------------ */
static void config_save_host(const char *host)
{
    lcd_puts_P(PSTR("ZAPIS DO PAMIECI"));              /* 0xbbaa               */
    ee_write_str(ee_cfg.host, host);
    eeprom_update_byte(&ee_cfg.host_xor, str_xor(host));
    lcd_puts_P(PSTR("Zapisano w pamieci "));           /* 0xbd44               */
}
static void config_load_host(void)
{
    ee_read_str(ee_cfg.host, server_host, sizeof(server_host));
    if (server_host[0] == '\0' ||
        eeprom_read_byte(&ee_cfg.host_xor) != str_xor(server_host)) {
        strcpy_P(server_host, PSTR(SERVER_DEFAULT));   /* lgps01...            */
        config_save_host(server_host);
    }
}

/* --- stan ruchu (0x3C): zapis @0x17fe, odczyt @0x186c ------------------------ */
static void config_save_motion(const char *st)         /* "R" albo "P"         */
{
    ee_write_str(ee_cfg.motion, st);
    /* obraz kopiuje tez do pamieci I2C: 0xe42(bank 0x7E, adr 0x08)           */
}
static void config_load_motion(void)
{
    ee_read_str(ee_cfg.motion, motion_state, 2);       /* bierzemy 1. znak     */
    if (motion_state[0] != 'R' && motion_state[0] != 'P') {
        strcpy_P(motion_state, PSTR("R"));             /* domyslnie RUCH       */
        config_save_motion(motion_state);
    }
}

/* --- indeks operatora roamingowego (0x3F): zapis @0x193a, odczyt @0x198c ----- */
static void config_save_roam_op(uint8_t idx)
{
    char b[4];
    utoa(idx, b, 10);
    ee_write_str(ee_cfg.roam_op_idx, b);
}
static uint8_t config_load_roam_op(void)
{
    char b[4];
    ee_read_str(ee_cfg.roam_op_idx, b, sizeof(b));
    return b[0] ? (uint8_t)atoi(b) : 1;                /* puste -> 1           */
}

/* --- kod przyczyny ostatniego restartu (0x42 PL / 0x67 inne): @0x19ee ------- */
static void config_save_reset_code(uint8_t code)
{
    char b[4];
    b[0] = '0' + (code / 100) % 10;                    /* 3 cyfry, np. "021"   */
    b[1] = '0' + (code / 10) % 10;
    b[2] = '0' + code % 10;
    b[3] = '\0';
    if (strcmp_P(net_mcc, PSTR("260")) == 0)           /* siec w Polsce        */
        ee_write_str(ee_cfg.reset_code_home, b);
    else                                               /* za granica / nieznana*/
        ee_write_str(ee_cfg.reset_code_abroad, b);
}
/* @0x1aaa — "kodPL/kodInne" do ekranu startowego ("er=") i ramki FORM ("Er=") */
static void config_load_reset_codes(char *out)         /* np. "021/020"        */
{
    char b[4];
    ee_read_str(ee_cfg.reset_code_home, b, sizeof(b));
    strcpy(out, b[0] ? b : "000");
    strcat_P(out, PSTR("/"));
    ee_read_str(ee_cfg.reset_code_abroad, b, sizeof(b));
    strcat(out, b[0] ? b : "000");
}

/* --- licznik restartow bez udanej lacznosci (0x5A): @0x1b72 / @0x1c5e ------- */
static void config_save_restart_count(void)
{
    char b[7];
    uint16_t n = restart_count;
    for (int8_t i = 5; i >= 0; i--) { b[i] = '0' + n % 10; n /= 10; }
    b[6] = '\0';
    ee_write_str(ee_cfg.restart_count, b);             /* np. "000001"         */
    /* obraz kopiuje tez do pamieci I2C (0xe42)                                 */
}
static void config_load_restart_count(void)
{
    char b[7];
    ee_read_str(ee_cfg.restart_count, b, sizeof(b));
    if (b[0] == '\0') { restart_count = 0; config_save_restart_count(); }
    else restart_count = (uint16_t)atoi(b);
}

/* --- tryb roamingu (0x64, "RoamingPam="): zapis @0x1dac, odczyt @0x1dfe ----- *
 * Dozwolone '0'..'7', inaczej domyslne 3. Gdy siec != 260 i tryb == 0,
 * firmware wypisuje "ZAKAZ ROAMINGU" (@0x668e). Znaczenie wartosci 1..7
 * nie zostalo ustalone.                                                       */
static void config_save_roaming_mode(uint8_t m)
{
    char b[2] = { (char)('0' + (m & 7)), '\0' };
    ee_write_str(ee_cfg.roaming_mode, b);
}
static void config_load_roaming_mode(void)
{
    char b[3];
    ee_read_str(ee_cfg.roaming_mode, b, sizeof(b));
    if (b[0] >= '0' && b[0] <= '7' && b[1] == '\0') {
        roaming_mode = (uint8_t)(b[0] - '0');
    } else {
        roaming_mode = 3;                              /* @0x1e46 ldi r24,3    */
        config_save_roaming_mode(roaming_mode);
    }
}

/* Wczytanie calej konfiguracji przy starcie. */
static void config_load(void)
{
    config_load_host();
    config_load_port();
    server_port = (uint16_t)atoi(server_port_str);
    config_load_motion();
    roam_op_idx = config_load_roam_op();
    config_load_restart_count();
    config_load_roaming_mode();
}

/* Restart z kodem przyczyny — procedura @0x2a28 / @0x7e00:
 * zapisz stan ruchu i licznik restartow, zapisz kod przyczyny,
 * "RESET ZA 7 sek." (@0x7dde), potem reset przez watchdog.
 * Kody znalezione w obrazie (wybrane): 20 = modem nie odpowiada (AT+CSQ),
 * 21 = blad AT+CIICR (GPRS), 22 = blad AT+CIPHEAD, 23 = zakaz roamingu,
 * 27 = komenda RESET z SMS, 29 = blad HSK, 36 = IMEI/PIN.                     */
static void restart_with_reason(uint8_t code)
{
    config_save_motion(motion_state);
    restart_count++;                                   /* @0x2a66 (0xaa14)     */
    config_save_restart_count();
    lcd_puts_P(PSTR("RESTART"));
    config_save_reset_code(code);
    lcd_puts_P(PSTR("RESET ZA 7 sek. "));
    wdt_enable(WDTO_120MS);                            /* WDTCR=0x1B @0x7e34   */
    for (;;) ;
}

/* Odpowiedz "OK" od serwera: zeruj licznik restartow i kod przyczyny. */
static void config_server_ok(void)
{
    restart_count = 0;                                 /* @0x414c              */
    config_save_restart_count();
    config_save_reset_code(0);                         /* @0x253c 0x19ee(0)    */
}

/* --------------------------------------------------------------------------
 *  Bufor rekordow "OLD" na czas braku zasiegu.
 *  W obrazie rekordy trafiaja najpewniej do zewnetrznej pamieci I2C
 *  (0xe42/0xfa6: bank + adres, "ZAPIS DO PAMIECI" / "ODCZYT Z PAMIECI").
 *  Warstwa I2C nie jest tu odtworzona — kolejka modelowana w RAM (ulotna).
 -------------------------------------------------------------------------- */
#define OLD_SLOTS  8
#define OLD_SIZE   48
static char    old_q[OLD_SLOTS][OLD_SIZE];
static uint8_t old_head, old_tail;

static void mem_store_record(const char *rec)
{
    uint8_t nx = (uint8_t)((old_tail + 1) % OLD_SLOTS);
    if (nx == old_head) return;            /* bufor pelny — rekord przepada     */
    lcd_puts_P(PSTR("ZAPIS DO PAMIECI"));
    strncpy(old_q[old_tail], rec, OLD_SIZE - 1);
    old_q[old_tail][OLD_SIZE - 1] = '\0';
    old_tail = nx;
}

static void send_old_records(void)
{
    lcd_puts_P(PSTR("LOO-BE"));          /* begin */
    if (old_head != old_tail) {
        lcd_puts_P(PSTR("ODCZYT Z PAMIECI"));
        sim900_tcp_send(old_q[old_head]);           /* "LOO-02;..."            */
        old_head = (uint8_t)((old_head + 1) % OLD_SLOTS);  /* pop po wyslaniu  */
        lcd_puts_P(PSTR("WYSLALEM DANE OLD    "));
    } else {
        lcd_puts_P(PSTR("BRAK DANYCH OLD"));
    }
    lcd_puts_P(PSTR("LOO-EN"));          /* end */
}

/* ========================================================================== *
 *  [REC]  CAN — paliwo / RPM / pedal / sonda  (parser @0x924a)               *
 * ========================================================================== *
 *  WAZNE: ATmega64 nie ma kontrolera CAN, a w obrazie nie ma ani SPI, ani
 *  magistrali CAN. Funkcja @0x924a NIE czyta sprzetu — PARSUJE dane CAN juz
 *  odebrane do bufora RAM (~0x0B82): wyciaga pola przez atoi (@0xb714), liczy
 *  roznice miedzy kolejnymi probkami (instrukcje sub/sbc @0x92a2) i wygladza.
 *  Dane CAN dostarcza wiec ZEWNETRZNY modul (brama CAN->szeregowa / FMS),
 *  a MCU je interpretuje. Stringi: "CAN=", "BRAK CAN", "RAMKA CAN ERROR",
 *  "Paliwo=", " RPM=", " Pedal=", "SONDA1=", " W_can=", "WYGLADZ= J=".         */

static struct {
    uint16_t rpm;         /* RPM=                                              */
    uint16_t fuel;        /* Paliwo=                                           */
    uint16_t pedal;       /* Pedal=                                            */
    uint16_t sonda1;      /* SONDA1= (surowy poziom paliwa z czujnika)         */
    uint16_t sonda1_avg;  /* W_sonda1 — wygladzony (filtr usredniajacy)        */
    int16_t  fuel_delta;  /* roznica wobec poprzedniej probki (@0x92a2)        */
    uint8_t  ok;          /* 0 = "BRAK CAN" / blad ramki                       */
} can_data;

/* Bufor z danymi CAN od zewnetrznego modulu (odpowiednik RAM ~0x0B82).        */
static char can_buf[64];

/* Prosty filtr usredniajacy (wygladzanie SONDA1): wykladniczy, N=8.           */
static uint16_t smooth(uint16_t avg, uint16_t sample)
{
    return (uint16_t)(avg - (avg >> 3) + (sample >> 3));   /* avg += (x-avg)/8 */
}

static void can_read(void)
{
    char f[12];

    if (can_buf[0] == '\0') {                 /* brak danych od modulu CAN     */
        can_data.ok = 0;
        lcd_puts_P(PSTR("BRAK CAN "));         /* 0xc3d4                        */
        return;
    }
    if (strncmp_P(can_buf, PSTR("CAN="), 4) != 0) {   /* zly naglowek ramki    */
        can_data.ok = 0;
        lcd_puts_P(PSTR("RAMKA CAN ERROR"));   /* 0xc3de                        */
        return;
    }

    uint16_t prev_fuel = can_data.fuel;

    nmea_field(can_buf, 1, f); can_data.rpm    = atoi(f);   /* RPM=            */
    nmea_field(can_buf, 2, f); can_data.fuel   = atoi(f);   /* Paliwo=         */
    nmea_field(can_buf, 3, f); can_data.pedal  = atoi(f);   /* Pedal=          */
    nmea_field(can_buf, 4, f); can_data.sonda1 = atoi(f);   /* SONDA1=         */

    /* roznica zuzycia paliwa (sub/sbc @0x92a2) */
    can_data.fuel_delta = (int16_t)(can_data.fuel - prev_fuel);
    /* wygladzanie poziomu paliwa (WYGLADZ= / W_sonda1) */
    can_data.sonda1_avg = smooth(can_data.sonda1_avg, can_data.sonda1);
    can_data.ok = 1;
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
    strcat_P(out, PSTR("Er="));            /* @0x23e0                        */
    {   char codes[8];                     /* @0x2372 -> 0x1aaa: "021/020"   */
        config_load_reset_codes(codes);    /* kody przyczyn restartu PL/inne */
        strcat(out, codes);
    }
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
    /* Jak BASIC-owe Mid(): pozycja liczona OD 1 (@0xb7ee: czyta 'off' znakow,
     * potem cofa wskaznik o 1), wiec pierwszy kopiowany znak to src[off-1].   */
    if (off) off--;
    while (off && *src) { src++; off--; }      /* przewin do pozycji          */
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
    /* Tryb roamingu: obsluga CONFIG zapisuje go pod EEPROM 0x64 i wypisuje
     * "RoamingPam=<n>" (@0x37e4). Offset pola w odpowiedzi nie jest ustalony,
     * dlatego tutaj go nie przypisujemy.                                     */

    /* przy bledzie formatu: "-CONFIG-ERROR " (0xbdba); sukces -> zapis EEPROM  */
}

/* ========================================================================== */
/*  [REC]  Petla glowna — maszyna stanow  @reset->0x8c, loop @0x2c2..0x452     */
/* ========================================================================== */
int main(void)
{
    uint8_t state = ST_MODEM_INIT;

    hw_init();                             /* @0x8c  [HW] */
    config_load();                         /* host, port, stan, liczniki z EEPROM */
    gps_config();                          /* PMTK: wybor zdan NMEA */
    lcd_clear_home();                      /* ekran startowy @0x03be:           */
    lcd_puts_P(PSTR("SIM900"));            /* "SIM900 ver=2.65 er=021/020"      */
    lcd_puts_P(PSTR(" ver="));
    lcd_puts_P(PSTR(FW_VERSION));
    {   char codes[8];
        config_load_reset_codes(codes);    /* kody przyczyn ostatnich restartow */
        lcd_puts_P(PSTR(" er="));
        lcd_puts(codes);
    }

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
                lcd_puts_P(PSTR("- HSK PROBLEM -5s -"));
                _delay_ms(5000);
                state = ST_MODEM_INIT;
            }
            break;

        case ST_WORK:                      /* @0x37c.. glowna praca */
            /* 1) odczyt i parsowanie GPS (RMC/GGA/VTG) z bufora linii ISR:
             *    konsumuj wszystkie linie zlozone przez ISR od ostatniego obiegu */
            while (line_ready)
                nmea_poll();               /* wypelnia strukture 'fix'           */

            if (fix.valid) lcd_puts_P(PSTR("NOWE DANE GPS"));
            else           lcd_puts_P(PSTR("BRAK NOWYCH GPS"));

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
                    while (line_ready) nmea_poll();  /* odbierz odpowiedz      */
                    if (strcmp_P(last_reply, PSTR("OK")) == 0)
                        config_server_ok();          /* @0x2522 / @0x414c      */
                } else {
                    mem_store_record(frame);         /* "ZAPIS DO PAMIECI"    */
                    lcd_puts_P(PSTR("NET PROBLEM"));
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
 *  - EEPROM wewn.: konfiguracja (host/port + XOR, liczniki, kody restartu).
 *  - pamiec I2C (programowe I2C PD0/PD1): rekordy "OLD" na czas braku zasiegu.
 *  - CAN: odczyt paliwa (SONDA1), RPM, pedalu; usrednianie poziomu paliwa.
 *  - SMS: zdalne komendy RESET i zmiana adresu serwera (NEW*ADR=).
 *  - Kontrola roamingu (AT+COPS), watchdog, reset programowy.
 *
 *  Toolchain: avr-gcc + avr-libc (rozpoznane rutyny: strcpy/strcpy_P/strcmp/
 *  itoa/atoi/_delay_ms oraz libgcc __lpm/__mulhi3/ramki stosu).
 * ============================================================================
 */
