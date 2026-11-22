/*
 * ============================================================================
 *  REKONSTRUKCJA FIRMWARE  —  Lokalizator GPS (fleet tracker)
 * ============================================================================
 *
 *  Plik wejsciowy : flash.bin  (64 KiB, wykorzystane 0x0000..0xC3EE = ~50 KiB)
 *  MCU            : Atmel ATmega64  (avr5)
 *  F_CPU          : 8 000 000 Hz   (potwierdzone: UBRR=51 @ 9600 baud oraz "AT+IPR=9600")
 *  Toolchain      : avr-gcc + avr-libc (oryginal byl skompilowany innym kompilatorem)
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
 *    [LIB]  - procedury biblioteczne z obrazu rozpoznane po dzialaniu i zastapione
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
#include <ctype.h>
#include <stdlib.h>

/* ========================================================================== */
/*  [LIB]  Procedury biblioteczne w obrazie i ich odpowiedniki w C            */
/* ========================================================================== */
/*
 *   adres     dzialanie                          odpowiednik w C
 *   --------  ---------------------------------  ------------------------------
 *   0xaeac    kopia napisu z flash do RAM        strcpy_P()
 *   0xb7d8    kopia napisu w RAM                 strcpy()
 *   0xaae6    koniec napisu (doklejanie)         strcat()
 *   0xb85a/e  porownanie napisow (flash / RAM)   strcmp_P() / strcmp()
 *   0xb84e    dlugosc napisu                     strlen()
 *   0xb7ee    wycinek napisu (pozycja od 1)      get_field()
 *   0xb882    szukanie w napisie                 strstr()/strchr()
 *   0xb714    tekst -> liczba (wynik w R16..R19) atoi()/strtol()
 *   0xb672/0  liczba -> tekst (ze znakiem/bez)   itoa()/utoa()
 *   0xb4a4    dopelnianie liczby do szerokosci   formatowanie z zerami
 *   0xaa14    zwiekszenie liczby 16-bit          x++
 *   0xac72    zwloka w ms                        _delay_ms()
 *   0xab6a    zwloka w sekundach                 _delay_ms(1000) w petli
 *   0xab5a    LCD: wyczysc + poczatek (0x01,0x80) lcd_clear_home()
 *   0xab40    LCD: druga linia (0xC0)            lcd_cmd(0xC0)
 *   0xab22    LCD: napis z flash                 lcd_puts_P()
 *   0xab16    LCD: napis z RAM                   lcd_puts()
 *   0xac0c    LCD: bajt (4-bit)                  lcd_putc()
 *   0xab7c    LCD: inicjalizacja                 lcd_init
 *   0xb9c6/e4 I2C: start / stop                  (programowe I2C PD0/PD1)
 *   0xb9f6    I2C: zapis bajtu                   (j.w.)
 *   0xba30    I2C: odczyt bajtu                  (j.w.)
 *   0xb81e    zapis EEPROM (gdy r23 = 0x7B)      eeprom_update_byte()
 *   0xb840    odczyt EEPROM                      eeprom_read_byte()
 *   0xae98    mnozenie 16-bit                    operator *
 *   0xb25c    odczyt bajtu z flash (lpm)         pgm_read_byte()
 *   0xae84/0xae90/0xa9ea  ramka R4:R5            (zmienne tymczasowe; w C — stos)
 */

/* ========================================================================== */
/*  [HW]  Przypisanie peryferiow (potwierdzone z inicjalizacji)               */
/* ========================================================================== */
/*
 *  USART0  @0x09/0x0A (UBRR0L=0x33, UCSR0B=0x18 -> RXEN0|TXEN0), bez przerwania RX
 *  USART1  @0x98..0x9D (UBRR1L=0x33, UCSR1B=0x18 -> RXEN1|TXEN1), wektor RX 0x78
 *          UBRR=51, F_CPU=8MHz, U2X=0  =>  9600 bps 8N1 na obu portach.
 *
 *  Jeden port obsluguje GPS (PA6C, zdania NMEA), drugi SIM900 (odpowiedzi AT).
 *  ISR USART1 (czyta UDR1) sklada linie zakonczone CR/LF -> parser NMEA/AT.
 *  Wektor 0x38 to TIMER1_OVF (zegar 1 s), nie odbior USART0.
 *  W logu NMEA i komendy AT ida tym samym kanalem — GPS i modem dziela USART1
 *  (przerwanie ma osobne tryby odbioru: NMEA, odpowiedzi AT, dane TCP).
 *
 *  Piny GPIO ustawione jako wyjscia w init (sbi DDRx):
 *     DDRA.3, PORTA.3=1 ; DDRA.4, PORTA.4=1   (zasilanie/klucze modulow)
 *     DDRD.5, DDRD.6, DDRD.7                   (sterowanie GPS/modem: ON, RESET)
 *     DDRB.4, DDRB.6, PORTB.6=1                (LED / linia CAN / klucz)
 *  Timer1: zegar 1 s (przerwanie TIMER1_OVF @0xa33a). ADC wlaczony (ADCSRA/ADMUX).
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
 *   @0x0A9C       czas pracy [s] (32-bit), @0x0AA0 licznik sekund (zegar 1 s)
 *   @0x0B38       stan odbioru linii USART1 (==0x22 aktywny)
 *   @0x0B39       biezacy bajt / bufor linii NMEA/AT
 *   @0x0C46       bufor roboczy do skladania i wypisywania napisow (trace)
 */
static char  modem_ver[8];        /* "2.65" z AT+CGMR / "ver="               */
static char  imei[16];            /* z AT+GSN  ("IMEI>"), RAM 0x0B42          */
static uint16_t can_counter;      /* RAM 0x0BE9, pole po ";CAN;" w ramce PNG  */
static char  apn[16]       = "internet";
static char  server_host[40];     /* "lXX.monitoring-gps.net"                */
static char     server_port_str[5]; /* port TCP serwera, np. "3506" (EEPROM 0x01) */
static uint16_t server_port;      /* ta sama wartosc liczbowo (po atoi)       */
static uint8_t  csq;              /* sila sygnalu z AT+CSQ                    */
static char     net_mcc[4];       /* 0x0B6D  MCC biezacej sieci, "260" = PL   */
static uint8_t  roaming_mode = 3; /* tryb roamingu z EEPROM 0x64 (0..7)       */
static uint8_t  roam_op_idx = 1;  /* 0x0C3B  indeks operatora (EEPROM 0x3F)   */
static char     last_reply[80];   /* ostatnia linia odpowiedzi (nie-NMEA)     */

/* Prototypy funkcji zdefiniowanych nizej. */
static void restart_with_reason(uint8_t code);
static void config_save_roam_op(uint8_t idx);
static uint8_t config_load_roam_op(void);
static void sms_poll(void);
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

    /* @0x150..0x166  Timer1 jako zegar 1 s:
     *   TCCR1A = 0; TCCR1B = 0x04 (preskaler 256); TIMSK |= TOIE1;
     *   TCNT1 = 0x85EE -> przepelnienie po 31250 taktach = 1 s przy 8 MHz     */
    TCCR1A = 0x00;
    TCCR1B = 0x04;
    TIMSK |= (1 << TOIE1);
    TCNT1  = 0x85EE;

    /* @0x172  UCSR1B &= ~RXCIE1 (przerwanie RX wlaczane pozniej)              */
    UCSR1B &= ~(1 << RXCIE1);

    /* @0x17c..0x184  przetwornik ADC: ADCSRA = 0x86 (wlaczony, preskaler 64),
     *   ADMUX = 0xC0 (napiecie odniesienia wewnetrzne 2,56 V)                 */
    ADCSRA = 0x86;
    ADMUX  = 0xC0;

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
/*  [HW]  ISR TIMER1_OVF  @0xa33a  (wektor 14, adres 0x38) — zegar 1 s          */
/* ========================================================================== */
/*  KOREKTA: wczesniej opisane jako odbior USART0. Wektor 14 ATmega64 to
 *  TIMER1_OVF; procedura przeladowuje TCNT1 i co sekunde:
 *    uptime (0x0A9C, 32-bit) +1, licznik 8-bit (0x0AA0) +1,
 *    minutniki 0x023E i 0x0242 (32-bit) -1 — ustawiane z odpowiedzi CONFIG
 *    (pozycje 12 i 6): czas do nastepnego pobrania CONFIG i do nastepnej sesji. */
static volatile uint32_t uptime_s;         /* 0x0A9C */
static volatile uint8_t  tick_s;           /* 0x0AA0 */
static volatile int32_t  t_config;         /* 0x023E  ("GET KONFIG SRV")    */
static volatile int32_t  t_session;        /* 0x0242  ("SESJA-", "CZEKAMY") */

ISR(TIMER1_OVF_vect)
{
    TCNT1 += 0x85EE;                       /* @0xa370 */
    uptime_s++;                            /* @0xa390 (0xa9fa) */
    tick_s++;                              /* @0xa398 */
    t_config--;                            /* @0xa3a2 (0xaa22) */
    t_session--;                           /* @0xa3aa (0xaa22) */
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
    char cmd[48];
    sim900_cmd("AT+CIPMUX=0");             /* 0xc07a pojedyncze polaczenie      */
    sim900_cmd("AT+CGATT=1");              /* 0xc086 podlaczenie do GPRS        */
    /* APN, uzytkownik i haslo — w logu: AT+CSTT="internet","internet","internet"
     * (@0x6006..0x60a6: trzy razy "internet")                                   */
    strcpy_P(cmd, PSTR("AT+CSTT=\""));
    strcat(cmd, apn); strcat_P(cmd, PSTR("\",\""));
    strcat(cmd, apn); strcat_P(cmd, PSTR("\",\""));
    strcat(cmd, apn); strcat_P(cmd, PSTR("\""));
    sim900_cmd(cmd);
    sim900_cmd("AT+CIICR");                /* 0xc0a6 podniesienie laczna        */
    sim900_cmd("AT+CIFSR");                /* 0xc0b0 lokalny IP                 */
    sim900_cmd("AT+CIPHEAD=1");            /* 0xc0ba naglowek w odbieranych     */
    return 1;
}

/* Blokada/zezwolenie roamingu (@0xc0dc "ZAKAZ ROAMINGU", 0xc210/0xc21e COPS) */
/* ==========================================================================
 *  [REC]  Roaming — funkcja @0x673a, wolana w kazdym obiegu petli (@0x40d2)
 * ==========================================================================
 *  MCC kraju pochodzi z AT+CENG? (tryb inzynierski SIM900, @0x634c):
 *    +CENG: <cell>,"<arfcn>,<rxl>,<rxq>,<mcc>,<mnc>,<bsic>,<cellid>,..."
 *  Wybor sieci przez AT+COPS (SIM900 / 3GPP 27.007):
 *    AT+COPS=1,2,"MCCMNC"  wybor reczny (format 2 = numeryczny)
 *    AT+COPS=4,2,"MCCMNC"  wybor reczny, przy porazce modem wraca do automatu
 *
 *  Tryb roamingu (EEPROM 0x64, "RoamingPam") — wartosci firmware, nie SIM900:
 *    0    roaming zabroniony: "ZAKAZ ROAMINGU", restart z kodem 23
 *    1    kraje G_EU + Szwajcaria
 *    2    kraje G_EU + Szwajcaria + G_EXT; Rosja bez wymuszania operatora
 *    3-7  tylko kraje G_EU (3 = domyslny)
 *  Wybor operatora z listy preferowanych dla kraju:
 *    <4   start od operatora zapamietanego w EEPROM 0x3F (rotacja)
 *    >=4  zawsze od pierwszego operatora
 *    >=5  probowany tylko pierwszy operator
 *    <6   po kolei AT+COPS=1,2 az do OK; gdy zaden — AT+COPS=4,2 (ostatni)
 *    6    od razu AT+COPS=4,2 z pierwszym operatorem
 *    7    bez AT+COPS — modem wybiera siec sam
 *  Kraj spoza dozwolonych: "ZAKAZ - <MCC>-n/10-", co 60 s sprawdzanie SMS,
 *  po 11 probach (~11 min) restart z kodem 25.
 *
 *  Tabela (@0x68ce..0x717c) przepisana 1:1 z obrazu. Lista MNC konczy sie na
 *  pierwszym "00" (petla @0x7250); dla Slowacji (231) kod "02" stoi za "00",
 *  wiec firmware nigdy go nie probuje — tak jest w oryginale.               */

enum { G_EU, G_CH, G_EXT, G_AUTO };
typedef struct { char mcc[4]; uint8_t grp; char mnc[18]; } roam_t;
static const roam_t roam_tab[] PROGMEM = {
    { "228", G_CH,  "07,03,02,01" },  /* Szwajcaria */
    { "257", G_EXT, "04,01,02" },  /* Bialorus */
    { "250", G_AUTO, "" },  /* Rosja */
    { "255", G_EXT, "06,05,03,01,07" },  /* Ukraina */
    { "259", G_EXT, "02,01,05" },  /* Moldawia */
    { "220", G_EXT, "05,03,01" },  /* Serbia */
    { "218", G_EXT, "03,90,05" },  /* Bosnia i Hercegowina */
    { "276", G_EXT, "01,04,02" },  /* Albania */
    { "294", G_EXT, "02,01,03" },  /* Macedonia Pln. */
    { "286", G_EXT, "03,04,01,02" },  /* Turcja */
    { "232", G_EU,  "05,03,01" },  /* Austria */
    { "206", G_EU,  "20,01,10" },  /* Belgia */
    { "284", G_EU,  "03,05,01" },  /* Bulgaria */
    { "280", G_EU,  "01" },  /* Cypr */
    { "230", G_EU,  "02,01,03" },  /* Czechy */
    { "219", G_EU,  "01,02,10" },  /* Chorwacja */
    { "238", G_EU,  "06,01,02" },  /* Dania */
    { "248", G_EU,  "01,02,03" },  /* Estonia */
    { "244", G_EU,  "14,12,05,91" },  /* Finlandia */
    { "208", G_EU,  "01,20,15" },  /* Francja */
    { "202", G_EU,  "01,05,10" },  /* Grecja */
    { "214", G_EU,  "03,05,07,01,04" },  /* Hiszpania */
    { "204", G_EU,  "08,20,16,04" },  /* Holandia */
    { "272", G_EU,  "05,03,02" },  /* Irlandia */
    { "274", G_EU,  "11,02,01" },  /* Islandia */
    { "246", G_EU,  "02,01,03" },  /* Litwa */
    { "270", G_EU,  "99,01,77" },  /* Luksemburg */
    { "247", G_EU,  "05,01,02" },  /* Lotwa */
    { "278", G_EU,  "21,01" },  /* Malta */
    { "262", G_EU,  "03,07,02" },  /* Niemcy */
    { "242", G_EU,  "02,05,01" },  /* Norwegia */
    { "268", G_EU,  "01,03,06" },  /* Portugalia */
    { "226", G_EU,  "10,01,03" },  /* Rumunia */
    { "231", G_EU,  "01,00,02" },  /* Slowacja */
    { "293", G_EU,  "41,40" },  /* Slowenia */
    { "240", G_EU,  "02,07,01" },  /* Szwecja */
    { "216", G_EU,  "30,01,70" },  /* Wegry */
    { "234", G_EU,  "33,10,30,15" },  /* Wielka Brytania */
    { "222", G_EU,  "99,01,10,88" },  /* Wlochy */
};

/* Odczyt MCC obslugujacej komorki z AT+CENG? do net_mcc. */
static uint8_t sim900_read_mcc(void)
{
    uint16_t t;
    sim900_cmd("AT+CENG=1");
    sim900_cmd("AT+CENG?");
    for (t = 0; t < 200; t++) {                       /* do ~2 s              */
        while (line_ready) {
            nmea_poll();
            if (strncmp_P(last_reply, PSTR("+CENG: 0,"), 9) == 0) {
                char *q = strchr(last_reply, '"');    /* "arfcn,rxl,rxq,mcc,.. */
                uint8_t f;
                for (f = 0; q && f < 3; f++) q = strchr(q + 1, ',');
                if (q) { strncpy(net_mcc, q + 1, 3); net_mcc[3] = '\0'; return 1; }
            }
        }
        _delay_ms(10);
    }
    return 0;
}

/* Czekaj na odpowiedz "OK" od modemu. */
static uint8_t sim900_wait_ok(uint16_t ms)
{
    while (ms--) {
        while (line_ready) {
            nmea_poll();
            if (strcmp_P(last_reply, PSTR("OK")) == 0) return 1;
        }
        _delay_ms(1);
    }
    return 0;
}

/* AT+COPS=<tryb>,2,"<MCC><MNC>" */
static uint8_t sim900_select_operator(char mode, const char *mnc)
{
    char cmd[28];
    strcpy_P(cmd, PSTR("AT+COPS=0,2,\""));
    cmd[8] = mode;
    strcat(cmd, net_mcc);
    strcat(cmd, mnc);
    strcat_P(cmd, PSTR("\""));
    sim900_cmd(cmd);
    return sim900_wait_ok(3000);
}

static void sim900_roaming(void)
{
    roam_t e;
    char mnc[6][3];
    uint8_t i, n, count, idx, k, tries, found = 0;

    if (!sim900_read_mcc()) return;
    if (strcmp_P(net_mcc, PSTR("260")) == 0) {        /* Polska              */
        lcd_puts_P(PSTR("KRAJ"));
        return;
    }
    lcd_puts_P(PSTR("ZAGRANICA"));
    if (roaming_mode == 0) {
        lcd_puts_P(PSTR("ZAKAZ ROAMINGU "));
        restart_with_reason(23);
    }

    for (i = 0; i < sizeof(roam_tab) / sizeof(roam_tab[0]); i++) {
        memcpy_P(&e, &roam_tab[i], sizeof(e));
        if (strcmp(e.mcc, net_mcc) != 0) continue;
        if (e.grp == G_EU) found = 1;
        else if (e.grp == G_CH) found = (roaming_mode == 1 || roaming_mode == 2);
        else found = (roaming_mode == 2);
        break;
    }
    if (found && e.grp == G_AUTO) return;             /* Rosja w trybie 2    */

    if (!found) {                                     /* kraj niedozwolony   */
        for (tries = 0; tries <= 10; tries++) {
            lcd_puts_P(PSTR("ZAKAZ - "));
            lcd_puts(net_mcc);
            _delay_ms(60000);                         /* Wait 60 @0x71de     */
            sms_poll();
        }
        restart_with_reason(25);
    }

    /* lista MNC do pierwszego "00" (petla @0x7250) */
    for (count = 0, n = 0; count < 6 && e.mnc[n]; count++, n += 3) {
        mnc[count][0] = e.mnc[n]; mnc[count][1] = e.mnc[n + 1]; mnc[count][2] = 0;
        if (mnc[count][0] == '0' && mnc[count][1] == '0') break;
        if (!e.mnc[n + 2]) { count++; break; }
    }
    if (count == 0) return;

    idx = config_load_roam_op();                      /* EEPROM 0x3F         */
    if (idx > count) { idx = 1; config_save_roam_op(1); }
    else config_save_roam_op((uint8_t)(idx + 1));     /* nastepnym razem dalej*/
    if (roaming_mode >= 4) { idx = 1; config_save_roam_op(1); }
    if (roaming_mode >= 5) count = 1;

    k = 1;
    if (roaming_mode < 6) {
        for (i = 1; i <= count; i++) {
            k = (uint8_t)(i + idx - 1);
            if (k > count) k -= count;
            if (sim900_select_operator('1', mnc[k - 1])) return;
        }
    }
    if (roaming_mode <= 6)
        sim900_select_operator('4', mnc[k - 1]);
}

/* Otwarcie polaczenia TCP do serwera (@0xbe52 CIPSTART, 0xbe6a CIPSEND) */
static uint8_t sim900_tcp_open(void)
{
    char cmd[80];
    /* Skladanie jak w obrazie @0x3d34..0x3dd4:
     *   AT+CIPSTART="TCP","host","port"  (cudzyslowy potwierdzone logiem)
     * host i port pochodza z EEPROM (po walidacji w config_load).              */
    strcpy_P(cmd, PSTR("AT+CIPSTART=\""));  /* @0x3d34 */
    strcat_P(cmd, PSTR("TCP"));            /* @0x3d54 */
    strcat_P(cmd, PSTR("\",\""));          /* @0x3d74 */
    strcat(cmd, server_host);              /* @0x3d94  np. lgps03.monitoring-gps.net */
    strcat_P(cmd, PSTR("\",\""));          /* @0x3db4 */
    strcat(cmd, server_port_str);          /* @0x3dd4  np. "3506"               */
    strcat_P(cmd, PSTR("\""));
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
/*  [REC]  Obsluga SMS — odczyt @0x80ac, komendy @0x84dc, kasowanie @0x883a     */
/* ========================================================================== */
/*  - odczytywana jest TYLKO pozycja 1 na karcie SIM (AT+CMGR=1); kolejne SMS-y
 *    czekaja, dopoki pozycja 1 nie zostanie skasowana,
 *  - numer nadawcy: 9 cyfr po "+48" (RAM 0x0B78),
 *  - PIN = 4 ostatnie cyfry IMEI (Mid(IMEI,12,4), RAM 0x0B66, na LCD "PIN>"),
 *  - "RESET*<PIN>" / "RESET-<PIN>": ustawienia fabryczne (lgps01...:3506,
 *    tryb roamingu 3), skasowanie SMS, "KOMENDA RESET", restart (kod 27),
 *  - "NEW*ADR=<host>$<port>$" (bez PIN-u): zapis hosta (bufor 36 B) i portu
 *    (bufor 5 B), tryb roamingu 3, skasowanie SMS, restart (kod 28).
 *    W oryginale NIE MA kontroli dlugosci: bez '$' petle zbieraja do 255 znakow
 *    i przepelniaja 5-bajtowy bufor portu — urzadzenie resetuje sie przed
 *    skasowaniem SMS-a i wpada w petle restartow. Tu dlugosci sa sprawdzane,
 *  - inny SMS przy aktywnej sesji: tresc wysylana na serwer jako LOGUJ-;
 *    (w tej sciezce obrazu nie ma AT+CMGD).                                   */

static void config_save_port(const char *port);    /* def. nizej              */
static void config_save_roaming_mode(uint8_t m);   /* def. nizej              */

/* Zbierz linie odpowiedzi modemu (bez NMEA) przez 'ms' milisekund. */
static void sim900_collect(char *out, uint8_t size, uint16_t ms)
{
    out[0] = '\0';
    while (ms--) {
        while (line_ready) {
            nmea_poll();
            if (last_reply[0] && strlen(out) + strlen(last_reply) + 2 < size) {
                strcat(out, last_reply);
                strcat_P(out, PSTR("\n"));
            }
            last_reply[0] = '\0';
        }
        _delay_ms(1);
    }
}

static void sms_delete(void)                       /* @0x883a                 */
{
    sim900_cmd("AT+CMGD=1,3");                     /* 3 = przeczytane i wyslane */
    sim900_wait_ok(3000);
    lcd_puts_P(PSTR("SMS IS CLEAR"));
}

static void sms_poll(void)
{
    char buf[160], pin[5], cmd[12];
    char *p, *q;

    lcd_puts_P(PSTR("-- CZYTAM SMS  ---"));
    sim900_cmd("AT+CMGF=1");
    sim900_wait_ok(1000);
    sim900_cmd("AT+CMGR=1");                       /* tylko pozycja 1!        */
    sim900_collect(buf, sizeof(buf), 2000);
    if (!strstr_P(buf, PSTR("+CMGR:"))) {
        lcd_puts_P(PSTR("[Brak SMS]"));
        return;
    }

    /* PIN = 4 ostatnie cyfry IMEI */
    strncpy(pin, strlen(imei) >= 15 ? imei + 11 : "0000", 4);
    pin[4] = '\0';

    /* RESET*<PIN> / RESET-<PIN> -> ustawienia fabryczne (@0x8540..0x864e) */
    strcpy_P(cmd, PSTR("RESET*")); strcat(cmd, pin);
    p = strstr(buf, cmd);
    strcpy_P(cmd, PSTR("RESET-")); strcat(cmd, pin);
    if (p || strstr(buf, cmd)) {
        config_save_host(SERVER_DEFAULT);
        config_save_port(SERVER_PORT_DEFAULT);
        config_save_roaming_mode(3);
        sms_delete();
        lcd_puts_P(PSTR("KOMENDA RESET"));
        restart_with_reason(27);
    }

    /* NEW*ADR=<host>$<port>$ (@0x866a..0x8822) */
    p = strstr_P(buf, PSTR("NEW*ADR="));
    if (p) {
        char host[36], port[5];
        p += 8;
        q = strchr(p, '$');
        if (!q || q - p > 35 || !strchr(q + 1, '$') || strchr(q + 1, '$') - (q + 1) != 4) {
            /* oryginal tego nie sprawdza i sie zawiesza — tu tylko kasujemy SMS */
            lcd_puts_P(PSTR("ZLY FORMAT NEW*ADR"));
            sms_delete();
            return;
        }
        memcpy(host, p, q - p); host[q - p] = '\0';
        memcpy(port, q + 1, 4); port[4] = '\0';
        lcd_clear_home();
        lcd_puts(host); lcd_puts_P(PSTR(":")); lcd_puts(port);
        config_save_host(host);
        config_save_port(port);
        config_save_roaming_mode(3);
        sms_delete();
        restart_with_reason(28);
    }

    /* inny SMS: przy aktywnej sesji tresc idzie na serwer jako LOGUJ */
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
 * firmware wypisuje "ZAKAZ ROAMINGU" (@0x668e). Znaczenie pozostalych
 * wartosci: opis przy sim900_roaming() oraz docs/ROAMING.md.                  */
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
/* Blokada adresu producenta — @0x7b4c, wolana przy kazdym starcie (@0x45a)
 * PRZED polaczeniem z serwerem. Host musi miec dokladnie 25 znakow i postac
 * "lgpsNN.monitoring-gps.net" (NN > 0, wielkosc liter bez znaczenia), port —
 * 4 cyfry zaczynajace sie od "35". Inny adres (np. ustawiony SMS-em
 * NEW*ADR=) jest zastepowany fabrycznym "lgps01.monitoring-gps.net" (0xbbe0)
 * / "3506" i zapisywany do EEPROM.                                            */
static void config_vendor_lock(void)
{
    char up[26];
    uint8_t i, ok = (strlen(server_host) == 25);
    if (ok) {
        for (i = 0; i < 25; i++) up[i] = (char)toupper((unsigned char)server_host[i]);
        up[25] = '\0';
        ok = !strncmp_P(up, PSTR("LGPS"), 4)
             && atoi((char[]){ up[4], up[5], 0 }) > 0
             && !strcmp_P(up + 6, PSTR(".MONITORING-GPS.NET"));
    }
    if (strlen(server_host) == 25)
        lcd_puts_P(ok ? PSTR("ADRES MEM OK") : PSTR("ADRES MEM ERROR"));
    if (!ok) {
        strcpy_P(server_host, PSTR(SERVER_DEFAULT));
        config_save_host(server_host);
    }
    if (strlen(server_port_str) != 4) {
        lcd_puts_P(PSTR("PORT MEM LEN ERROR"));
        strcpy_P(server_port_str, PSTR(SERVER_PORT_DEFAULT));
        config_save_port(server_port_str);
    } else if (strncmp_P(server_port_str, PSTR("35"), 2) != 0) {
        lcd_puts_P(PSTR("PORT MEM VAL ERROR"));
        strcpy_P(server_port_str, PSTR(SERVER_PORT_DEFAULT));
        config_save_port(server_port_str);
    } else {
        lcd_puts_P(PSTR("PORT MEM OK"));
    }
}

static void config_load(void)
{
    config_load_host();
    config_load_port();
    config_vendor_lock();                  /* @0x45a -> 0x7b4c, przed polaczeniem */
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

    /* Naglowek @0x2372..0x23c8 — potwierdzony logiem:
     *   PNG-01;<IMEI>;CAN;<n>;FORM;Er=012/016;A=1 CAN=1 R=3/2 DP=0 OD=0
     *   DS=01 MEM=0:0 SD=1 SN=0WY=1 GG=1                                   */
    strcpy_P(out, PSTR("PNG-01;"));        /* @0x2380                        */
    strcat(out, imei);                     /* RAM 0x0B42 (z AT+GSN)          */
    strcat_P(out, PSTR(";CAN;"));          /* @0x23a0                        */
    utoa(can_counter, num, 10);            /* RAM 0x0BE9, w logu 25          */
    strcat(out, num);
    strcat_P(out, PSTR(";"));
    strcat_P(out, PSTR("FORM;"));          /* @0x23d0                        */
    strcat_P(out, PSTR("Er="));            /* @0x23e0                        */
    {   char codes[8];                     /* @0x1aaa: "012/016"             */
        config_load_reset_codes(codes);    /* kody przyczyn restartu PL/inne */
        strcat(out, codes);
    }
    strcat_P(out, PSTR(";A=1 "));          /* @0x2400                        */
    strcat_P(out, PSTR("CAN=1 "));         /* @0x2410                        */
    strcat_P(out, PSTR("R=3/"));           /* @0x2420                        */
    utoa(roaming_mode, num, 10);           /* @0x242c 0x1dfe = tryb roamingu */
    strcat(out, num);
    strcat_P(out, PSTR(" "));
    strcat_P(out, PSTR("DP=0 "));          /* @0x245e                        */
    strcat_P(out, PSTR("OD=0 "));          /* @0x246e                        */
    strcat_P(out, PSTR("DS=01 "));         /* @0x247e                        */
    strcat_P(out, PSTR("MEM="));           /* @0x248e  bank:adres pamieci I2C*/
    utoa(0, num, 10); strcat(out, num);
    strcat_P(out, PSTR(":"));
    utoa(0, num, 10); strcat(out, num);
    strcat_P(out, PSTR(" "));
    strcat_P(out, PSTR("SD=1 SN=0"));      /* @0x24de  (bez spacji, jak w logu)*/
    strcat_P(out, PSTR("WY=1 "));          /* @0x24ee                        */
    strcat_P(out, PSTR("GG=1"));           /* @0x24fe  koniec ramki          */
    /* Dalej w tej samej funkcji (@0x2588) jest osobna ramka "LOGUJ-;...@",
     * wysylana w innej galezi — nie jest czescia tej linii.                  */
}

/* Handshake z serwerem (@0x... ST_HSK). Przy bledzie: "-HSK-ERROR", po 5s retry */
static uint8_t server_handshake(void)
{
    /* Format potwierdzony logiem:
     *   HSK-02;<IMEI>;2.65;ORANGE;MT02-51083;SIM900;FGP;260;03;0;0;PA6C;1;TEMP;R26;B13
     * Stale "MT02-", "51083", ";SIM900;FGP", ";PA6C", ";1;TEMP;" sa we flashu.
     * 260;03 = MCC;MNC sieci. B13 = Mid(AT+CGMR,14,3) — wersja oprogramowania
 * modemu (potwierdzone logiem). Pola 0;0 oraz R26 — znaczenie nieustalone.   */
    char frame[112];
    strcpy_P(frame, PSTR("HSK-02;"));
    strcat(frame, imei);
    strcat_P(frame, PSTR(";2.65;ORANGE;"));
    strcat_P(frame, PSTR("MT02-51083"));
    strcat_P(frame, PSTR(";SIM900;FGP;"));
    strcat(frame, net_mcc);
    strcat_P(frame, PSTR(";03;0;0"));      /* MNC + dwa pola nieustalone      */
    strcat_P(frame, PSTR(";PA6C"));
    strcat_P(frame, PSTR(";1;TEMP;"));
    strcat_P(frame, PSTR("R26;B13"));      /* nieustalone                     */
    sim900_tcp_send(frame);
    return 1;
}

/* ========================================================================== *
 *  [REC]  Pobranie i parsowanie konfiguracji z serwera                       *
 *         funkcja @0x33b6 ; string "CONFIG;" @0x342a (flash 0xbd9c)          *
 * ========================================================================== *
 *
 *  Odpowiedz ma format STALOPOZYCYJNY: parametr pod stala pozycja (liczona
 *  OD 1) i o stalej szerokosci. Pola wycina get_field() (@0xb7ee), liczby
 *  zamienia atoi() (@0xb714). Mapa 1:1 z obrazu:
 *
 *    poz  w   cel       uwagi
 *    ---  --  --------  ----------------------------------------------------
 *      6   5  0x0242    liczba 32-bit
 *     12   5  0x023E    liczba 32-bit
 *     18   1  0x0257    licznik cykli do raportu; 0 -> 6 (@0x34c2)
 *     20   1  0x0B5E
 *     22   2  0x0268
 *     25   1  (lokalna) TRYB ROAMINGU -> EEPROM 0x64 (gdy suma kontrolna OK)
 *     27   2  0x0B63    dodatkowo przeskalowany mnozeniem (@0x356e)
 *     30   1  0x0C17
 *     32   4  0x0BEB    tekst; "0000" -> pusty napis. Uzycie nieustalone
 *     37   3  0x025B
 *     41   2  0x025C
 *     44   2  0x0B76
 *     47   1  0x0B77
 *     49   2  0x025E
 *     52   1  0x0C14
 *     54   3  0x0C15
 *     59   1  0x0269
 *     60   1  0x0B86
 *     61   1  0x0B87
 *     62   1  0x0C22
 *     63   1  0x0C23
 *     64   1  0x0C24
 *     79   3  (lokalna) SUMA KONTROLNA: XOR znakow na pozycjach 1..77
 *
 *  Sprawdzenie sumy (@0x37aa..0x37ca): Mid(odp,1,77) -> XOR (@0xb498)
 *  == liczba z pozycji 79..81 ?  tak: zapisz tryb roamingu do EEPROM i
 *  wypisz "RoamingPam=<n>" (@0x3806); nie: "-CONFIG-ERROR <xor>" i restart
 *  z kodem 14 (@0x38ba). Pozostale pola trafiaja do RAM przed sprawdzeniem.
 */

/* [LIB-like] @0xb7ee — wytnij 'width' znakow od offsetu 'off' z 'src' do 'dst'.
 * Kopiowanie konczy sie na NUL lub po 'width' znakach. */
static void get_field(const char *src, uint8_t off, uint8_t width, char *dst)
{
    /* Pozycja liczona OD 1 (@0xb7ee: czyta 'off' znakow,
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
    uint8_t  p268;        /* 0x0268 */
    uint16_t p_b63;       /* 0x0B63  (skalowany) */
    uint8_t  p_c17;       /* 0x0C17 */
    char     text_beb[5]; /* 0x0BEB  pole tekstowe z pozycji 32..35          */
    uint16_t p25b, p25c, p_b76, p25e, p_c15;
    uint8_t  p_b77, p_c14, p269, p_b86, p_b87, p_c22, p_c23, p_c24;
} cfg;

static char cfg_resp[96];     /* bufor odpowiedzi serwera (@0x01A7)           */

static void server_get_config(void)
{
    char f[8];
    const char *r = cfg_resp;
    uint8_t roam, sum;

    sim900_tcp_send("CONFIG;");          /* @0xbd9c                             */
    /* (odpowiedz trafia do cfg_resp[] — @0x3f8e w obrazie)                    */

    get_field(r,  6, 5, f); cfg.t0    = strtoul(f, NULL, 10);   /* 0x0242 */
    get_field(r, 12, 5, f); cfg.t1    = strtoul(f, NULL, 10);   /* 0x023E */
    cli(); t_session = (int32_t)cfg.t0; t_config = (int32_t)cfg.t1; sei();
    get_field(r, 18, 1, f); cfg.p257  = atoi(f);
    if (cfg.p257 == 0) cfg.p257 = 6;                            /* @0x34c2 */
    get_field(r, 20, 1, f); cfg.p_b5e = atoi(f);               /* 0x0B5E */
    get_field(r, 22, 2, f); cfg.p268  = atoi(f);               /* 0x0268 */
    get_field(r, 25, 1, f); roam      = atoi(f);               /* tryb roamingu */
    get_field(r, 27, 2, f); cfg.p_b63 = atoi(f);               /* 0x0B63 */
    get_field(r, 30, 1, f); cfg.p_c17 = atoi(f);               /* 0x0C17 */
    get_field(r, 32, 4, f);                                    /* @0x35a8 */
    if (strcmp_P(f, PSTR("0000")) == 0) cfg.text_beb[0] = '\0';
    else strcpy(cfg.text_beb, f);
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
    get_field(r, 64, 1, f); cfg.p_c24 = atoi(f);
    get_field(r, 79, 3, f); sum       = atoi(f);               /* suma kontrolna */

    {   char body[78];
        get_field(r, 1, 77, body);                             /* @0x37aa */
        if (str_xor(body) == sum) {                            /* @0x37c8 */
            config_save_roaming_mode(roam);                    /* EEPROM 0x64 */
            config_load_roaming_mode();                        /* walidacja 0..7 */
            lcd_puts_P(PSTR("RoamingPam="));
            utoa(roaming_mode, f, 10);
            lcd_puts(f);
        } else {
            lcd_puts_P(PSTR("-CONFIG-ERROR "));
            restart_with_reason(14);                           /* @0x38ba */
        }
    }
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
            sim900_roaming();              /* co obieg, jak @0x40d2 -> 0x673a */

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
