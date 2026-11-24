/*
 * boot.c — bootloader OTA dla ATmega64 (sekcja BOOT @0xF000, 2048 słów).
 *
 * Zadanie: na życzenie (flaga EEPROM ustawiana przez aplikację) pobrać obraz
 * firmware po TCP (SIM900) i zapisać sekcję aplikacji przez SPM, po czym oddać
 * jej sterowanie. Bootloader NIGDY nie nadpisuje siebie — urządzenie jest
 * odzyskiwalne zdalnie. Patrz docs/OTA.md.
 *
 * Rdzeń (SPM, CRC32, skok, UART) jest kompletny. Sekwencja modemu i pętla
 * pobierania DZIAŁAJĄ na założeniach z docs/PROTOCOL.md i WYMAGAJĄ dostrojenia
 * na stole (czasy PWRKEY, odpowiedzi operatora) — oznaczone TODO.
 */
#include <avr/io.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <stdlib.h>

#include "../include/board.h"
#include "../include/ota.h"
#include "../include/cfg.h"

#define APP_END   0xF000UL        /* koniec sekcji aplikacji (początek BOOT)   */

/* Meta obrazu w EEPROM (tuż przed flagą): rozmiar(4) + CRC32(4). */
#define META_SIZE_ADDR 0x07F4
#define META_CRC_ADDR  0x07F8

/* ---------------- UART1 (linia modemu) — odbiór w PRZERWANIU --------------- *
 * SPM (boot_page_erase/write) blokuje CPU na ~9 ms. Przy 9600 bps w tym czasie
 * wpada ~9 bajtów, a sprzętowy bufor USART ma tylko 2 bajty -> bez ISR byłby
 * overrun i utrata danych obrazu (CRC FAIL / timeout -> wieczne ponawianie).
 * ISR leży w sekcji BOOT (NRWW), więc jest wykonywalna także w trakcie
 * programowania sekcji aplikacji (RWW) — bajty trafiają do pierścienia i nic
 * nie ginie. Pierścień 256 (uint8_t głowa/ogon zawijają się same). */
#define RBUF_SZ 256
static volatile uint8_t rbuf[RBUF_SZ];
static volatile uint8_t rb_head, rb_tail;

ISR(USART1_RX_vect)
{
    uint8_t s = UCSR1A;
    uint8_t c = UDR1;
    if (s & ((1 << FE1) | (1 << DOR1)))     /* odrzuć bajt z błędem ramki/overrun */
        return;
    uint8_t nh = (uint8_t)(rb_head + 1);
    if (nh != rb_tail) { rbuf[rb_head] = c; rb_head = nh; }
}

static void u1_init(void)
{
    UBRR1H = (uint8_t)(UBRR_VAL(UART1_BAUD) >> 8);
    UBRR1L = (uint8_t)(UBRR_VAL(UART1_BAUD));
    UCSR1B = (1 << RXEN1) | (1 << TXEN1) | (1 << RXCIE1);
    UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);
    rb_head = rb_tail = 0;
    /* Wektory przerwań do sekcji BOOT (IVSEL=1), potem globalne zezwolenie.
     * jump_to_app() przywraca IVSEL=0 i wyłącza USART przed oddaniem app. */
    MCUCR = (1 << IVCE);
    MCUCR = (1 << IVSEL);
    sei();
}
static void u1_putc(char c) { while (!(UCSR1A & (1 << UDRE1))); UDR1 = (uint8_t)c; }
static void u1_puts(const char *s) { while (*s) u1_putc(*s++); }
static void u1_line(const char *s) { u1_puts(s); u1_putc('\r'); }

/* Debug na USART0 (38400, jak aplikacja) — żeby widzieć przebieg OTA. */
static void u0_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VAL(UART0_BAUD) >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL(UART0_BAUD));
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}
static void u0_puts(const char *s) { while (*s) { while (!(UCSR0A & (1 << UDRE0))); UDR0 = (uint8_t)*s++; } }

/* Odbiór 1 bajtu z pierścienia (wypełnianego w ISR) z grubym timeoutem.
 * Zwraca -1 przy timeoutcie. */
static int u1_getc(uint16_t ms)
{
    while (ms--) {
        for (uint16_t i = 0; i < 800; i++) {     /* ~1 ms @8MHz (zgrubnie) */
            if (rb_head != rb_tail)
                return (uint8_t)rbuf[rb_tail++];
        }
    }
    return -1;
}

/* Czytaj linię zakończoną '\n' do bufora; zwraca długość lub -1 (timeout). */
static int u1_getline(char *dst, uint8_t sz, uint16_t ms)
{
    uint8_t n = 0;
    for (;;) {
        int c = u1_getc(ms);
        if (c < 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') { dst[n] = '\0'; return n; }
        if (n < sz - 1) dst[n++] = (char)c;
    }
}

/* Wyślij AT i czekaj na podciąg "OK" w odpowiedzi (kilka linii). */
static uint8_t at_ok(const char *cmd, uint16_t ms)
{
    char ln[40];
    u1_line(cmd);
    for (uint8_t k = 0; k < 8; k++) {
        if (u1_getline(ln, sizeof(ln), ms) < 0) return 0;
        if (!strncmp(ln, "OK", 2)) return 1;
        if (!strncmp(ln, "ERROR", 5)) return 0;
    }
    return 0;
}

/* Czytaj linie aż pojawi się podciąg tok (albo ERROR/timeout). */
static uint8_t wait_token(const char *tok, uint16_t ms)
{
    char ln[48];
    for (uint8_t k = 0; k < 12; k++) {
        if (u1_getline(ln, sizeof(ln), ms) < 0) return 0;
        if (strstr(ln, tok)) return 1;
        if (!strncmp(ln, "ERROR", 5)) return 0;
    }
    return 0;
}

/* ---------------- SPM: zapis strony aplikacji --------------------------- */
static void program_page(uint32_t page, uint8_t *buf)
{
    uint16_t i;
    eeprom_busy_wait();
    boot_page_erase(page);
    boot_spm_busy_wait();
    for (i = 0; i < SPM_PAGESIZE; i += 2) {
        uint16_t w = buf[i] | ((uint16_t)buf[i + 1] << 8);
        boot_page_fill(page + i, w);
    }
    boot_page_write(page);
    boot_spm_busy_wait();
    boot_rww_enable();
}

/* ---------------- CRC32 (jak zlib) -------------------------------------- */
static uint32_t crc32_up(uint32_t crc, uint8_t d)
{
    crc ^= d;
    for (uint8_t k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
    return crc;
}

/* ---------------- Skok do aplikacji ------------------------------------- */
static void jump_to_app(void)
{
    UCSR1B = 0;
    cli();
    /* wektory przerwań -> sekcja aplikacji */
    MCUCR = (1 << IVCE);
    MCUCR = 0;
    __asm__ __volatile__("jmp 0x0000");
}

/* ---------------- Pobranie i zapis obrazu ------------------------------- *
 * Serwer po "FWREQ <imei>" odsyła:  "FW <rozmiar> <crc32hex>\n" + bajty.    *
 * Tryb surowy (CIPHEAD=0), więc czytamy strumień wprost.                   */
static uint8_t do_ota(void)
{
    char    ln[48];
    uint8_t page_buf[SPM_PAGESIZE];

    /* --- mux: słuchaj modemu (PA3=0, PA4=1) — bez tego GPS zalewa linię --- */
    DDRA  |= (1 << PA3) | (1 << PA4);
    PORTA &= ~(1 << PA3);
    PORTA |=  (1 << PA4);

    PIN_OUT(MODEM_DDR, MODEM_PWR_BIT);
    PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);     /* idle LOW (jak oryginał) */

    /* Sonda RAZ: AT -> OK? Jeśli nie ma odpowiedzi, zakładamy że wyłączony.
     * (Impuls na włączonym SIM900 by go wyłączył.) */
    u0_puts("BOOT: modem?\r\n");
    if (!at_ok("AT", 1500)) {
        /* Wyłączony -> włącz: PD6 HIGH 1200 ms (tranzystor ściąga PWRKEY w dół). */
        u0_puts("BOOT: wlaczam (PD6 HIGH 1200ms)\r\n");
        PIN_HIGH(MODEM_PORT, MODEM_PWR_BIT);
        _delay_ms(1200);
        PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
        if (wait_token("RDY", 5000)) u0_puts("BOOT: RDY\r\n");
        else                         u0_puts("BOOT: brak RDY (ide dalej)\r\n");
        u0_puts("BOOT: rozgrzewka (3x AT)...\r\n");
        for (uint8_t i = 0; i < 3; i++) { u1_line("AT"); _delay_ms(300); }
    }
    if (!at_ok("ATE0", 1000)) { u0_puts("BOOT: brak modemu\r\n"); return 0; }

    /* CIPSHUT — zamknij ewentualną starą sesję z aplikacji (zwraca "SHUT OK"). */
    u0_puts("BOOT: CIPSHUT\r\n");
    u1_line("AT+CIPSHUT");
    wait_token("SHUT OK", 8000);        /* toleruj — i tak idziemy dalej */

    /* Rejestracja w sieci (AT+CREG? -> ",1" dom. lub ",5" roaming). */
    {
        uint8_t reg = 0;
        for (uint8_t k = 0; k < 20 && !reg; k++) {
            u1_line("AT+CREG?");
            for (uint8_t j = 0; j < 4; j++) {
                if (u1_getline(ln, sizeof(ln), 1000) < 0) break;
                if (strstr(ln, "+CREG:") && (strstr(ln, ",1") || strstr(ln, ",5"))) { reg = 1; break; }
            }
            if (!reg) _delay_ms(1000);
        }
        if (!reg) { u0_puts("BOOT: brak rejestracji\r\n"); return 0; }
    }
    u0_puts("BOOT: zarejestrowany\r\n");

    u0_puts("BOOT: CGATT\r\n");
    at_ok("AT+CGATT=1", 10000);         /* GPRS attach (toleruj) */

    char cstt[64];
    strcpy(cstt, "AT+CSTT=\"" APN_DEFAULT "\",\"\",\"\"");
    u0_puts("BOOT: CSTT\r\n");
    if (!at_ok(cstt, 3000)) { u0_puts("BOOT: CSTT FAIL\r\n"); return 0; }

    u0_puts("BOOT: CIICR\r\n");
    if (!at_ok("AT+CIICR", 10000)) { u0_puts("BOOT: CIICR FAIL\r\n"); return 0; }

    /* CIFSR — potwierdź IP (odpowiedź to adres, nie "OK"); bramka na GPRS. */
    u0_puts("BOOT: CIFSR\r\n");
    {
        uint8_t ip = 0;
        u1_line("AT+CIFSR");
        for (uint8_t k = 0; k < 6; k++) {
            if (u1_getline(ln, sizeof(ln), 3000) < 0) break;
            if (ln[0] >= '0' && ln[0] <= '9') { ip = 1; break; }
            if (!strncmp(ln, "ERROR", 5)) break;
        }
        if (!ip) { u0_puts("BOOT: brak IP (GPRS down)\r\n"); return 0; }
    }

    if (!at_ok("AT+CIPHEAD=0", 1000)) { u0_puts("BOOT: CIPHEAD FAIL\r\n"); return 0; }

    /* CIPSTART: host+port z bloku OTA w EEPROM (zapisane przez aplikację). */
    char host[HOST_MAXLEN];
    eeprom_read_block(host, (const void *)OTA_HOST_ADDR, HOST_MAXLEN);
    host[HOST_MAXLEN - 1] = '\0';
    uint16_t port = eeprom_read_word((const uint16_t *)OTA_PORT_ADDR);
    if ((uint8_t)host[0] == 0xFF || host[0] == '\0') {   /* brak -> domyślny */
        strcpy(host, SERVER_HOST_DEFAULT);
        port = SERVER_PORT_DEFAULT;
    }
    u0_puts("BOOT: host=");
    u0_puts(host);
    u0_puts("\r\n");
    char cmd[96];
    strcpy(cmd, "AT+CIPSTART=\"TCP\",\"");
    strcat(cmd, host);
    strcat(cmd, "\",");
    { char ps[6]; utoa(port, ps, 10); strcat(cmd, ps); }
    u0_puts("BOOT: CIPSTART\r\n");
    u1_line(cmd);
    /* czekaj CONNECT OK */
    uint8_t conn = 0;
    for (uint8_t k = 0; k < 12; k++) {
        if (u1_getline(ln, sizeof(ln), 2000) < 0) break;
        if (!strncmp(ln, "CONNECT OK", 10) || !strncmp(ln, "ALREADY", 7)) { conn = 1; break; }
        if (!strncmp(ln, "CONNECT FAIL", 12)) break;
    }
    if (!conn) { u0_puts("BOOT: brak CONNECT\r\n"); return 0; }
    u0_puts("BOOT: CONNECT OK, pobieram\r\n");

    /* --- żądanie obrazu --- */
    u1_line("AT+CIPSEND");
    {   /* czekaj na znak zachęty '>' (bez CR) */
        uint8_t got_p = 0;
        for (uint8_t k = 0; k < 30 && !got_p; k++)
            if (u1_getc(100) == '>') got_p = 1;
        if (!got_p) return 0;
    }
    u1_puts("FWREQ\n");
    u1_putc(0x1A);

    /* --- nagłówek "FW <size> <crc>" (pomiń "SEND OK" i inne linie modemu) --- */
    {
        uint8_t hdr = 0;
        for (uint8_t k = 0; k < 6 && !hdr; k++) {
            if (u1_getline(ln, sizeof(ln), 8000) < 0) return 0;
            if (!strncmp(ln, "FW ", 3)) hdr = 1;
        }
        if (!hdr) return 0;
    }
    uint32_t size = 0, want_crc = 0;
    {
        char *p = ln + 3;
        while (*p >= '0' && *p <= '9') size = size * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        for (; *p; p++) {
            uint8_t h = (*p <= '9') ? *p - '0' : (*p | 0x20) - 'a' + 10;
            want_crc = (want_crc << 4) | h;
        }
    }
    if (size == 0 || size > APP_END) { u0_puts("BOOT: zly naglowek\r\n"); return 0; }
    u0_puts("BOOT: naglowek OK, flashuje...\r\n");

    /* --- strumień bajtów -> strony flash + CRC --- *
     * dirty=1 gdy zaczęliśmy zapisywać flash: wtedy błąd zwraca 2 (aplikacja
     * może być częściowo nadpisana — nie wolno do niej skoczyć). */
    uint32_t got = 0, crc = 0xFFFFFFFFUL, addr = 0;
    uint16_t fill = 0;
    uint8_t  dirty = 0;
    memset(page_buf, 0xFF, sizeof(page_buf));
    while (got < size) {
        int c = u1_getc(8000);
        if (c < 0) return dirty ? 2 : 0;
        page_buf[fill++] = (uint8_t)c;
        crc = crc32_up(crc, (uint8_t)c);
        got++;
        if (fill == SPM_PAGESIZE) {
            program_page(addr, page_buf);
            dirty = 1;
            addr += SPM_PAGESIZE;
            fill = 0;
            memset(page_buf, 0xFF, sizeof(page_buf));
        }
    }
    if (fill) {                             /* ostatnia, niepełna strona */
        program_page(addr, page_buf);
        dirty = 1;
    }
    crc ^= 0xFFFFFFFFUL;

    if (crc != want_crc) {
        u1_puts("FWERR\n");
        u0_puts("BOOT: CRC FAIL\r\n");
        return dirty ? 2 : 0;               /* zła suma -> ponów przy starcie */
    }

    u0_puts("BOOT: CRC OK -> aplikacja\r\n");
    /* sukces: zapisz meta, skasuj flagę OTA */
    eeprom_update_dword((uint32_t *)META_SIZE_ADDR, size);
    eeprom_update_dword((uint32_t *)META_CRC_ADDR, want_crc);
    eeprom_update_byte((uint8_t *)OTA_FLAG_ADDR, 0xFF);
    u1_puts("FWOK\n");
    return 1;
}

int main(void)
{
    uint8_t flag = eeprom_read_byte((uint8_t *)OTA_FLAG_ADDR);

    if (flag == OTA_MAGIC) {
        u0_init();
        u0_puts("\r\nBOOT: OTA start\r\n");
        u1_init();
        uint8_t safe_fail = 0;
        uint8_t ever_dirty = 0;              /* czy flash aplikacji był tknięty */
        for (;;) {
            uint8_t r = do_ota();
            if (r == 1)                      /* sukces (flaga skasowana w do_ota) */
                break;
            if (r == 2)                      /* flash naruszony -> aplikacja uszkodzona */
                ever_dirty = 1;
            /* Oddajemy sterowanie starej aplikacji TYLKO gdy flash NIGDY nie był
             * tknięty (czyste niepowodzenia połączenia). Jeśli cokolwiek już
             * zapisaliśmy -> aplikacja uszkodzona, NIE skaczemy, ponawiamy aż do
             * udanego OTA (odzysk ostatecznie przez VVDI/ISP). */
            if (r == 0 && !ever_dirty) {
                if (++safe_fail >= 5) {
                    eeprom_update_byte((uint8_t *)OTA_FLAG_ADDR, 0xFF);
                    break;
                }
            }
            _delay_ms(8000);
        }
    }
    jump_to_app();
    for (;;)
        ;
}
