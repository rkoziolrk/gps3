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

/* ---------------- UART1 (linia modemu, surowo, polling) ------------------- *
 * Odbiór pollingiem WYSTARCZA, bo obraz pobieramy w trybie ręcznym modemu
 * (AT+CIPRXGET): podczas SPM nie przychodzą żadne dane (modem sam nic nie
 * sypie), więc nie ma overrunu 2-bajtowego FIFO. Dzięki temu nie potrzeba ISR
 * ani tablicy wektorów w sekcji BOOT (oszczędność miejsca). */
static void u1_init(void)
{
    UBRR1H = (uint8_t)(UBRR_VAL(UART1_BAUD) >> 8);
    UBRR1L = (uint8_t)(UBRR_VAL(UART1_BAUD));
    UCSR1B = (1 << RXEN1) | (1 << TXEN1);
    UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);
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

/* Odbiór 1 bajtu z grubym timeoutem (~ms*pętle). Zwraca -1 przy timeoutcie. */
static int u1_getc(uint16_t ms)
{
    while (ms--) {
        for (uint16_t i = 0; i < 800; i++) {     /* ~1 ms @8MHz (zgrubnie) */
            if (UCSR1A & (1 << RXC1))
                return (uint8_t)UDR1;
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

/* ---------------- Pobieranie danych TCP przez CIPRXGET (pull) ------------ *
 * W trybie ręcznym (AT+CIPRXGET=1) host sam decyduje, kiedy pobrać dane:
 *   AT+CIPRXGET=2,<want>  ->  "+CIPRXGET: 2,<len>,<rest>" + <len> bajtów + OK
 * <len> bywa < want (tyle, ile jest w buforze); 0 = chwilowo pusto; przy
 * zamknięciu pojawi się "CLOSED". Dane są surowe (mogą zawierać CR/LF/0x1A),
 * więc czytamy DOKŁADNIE <len> bajtów po liczniku, nie liniowo. */
static uint8_t  rx_stage[256];
static uint16_t rx_n, rx_i;

/* Jedno pobranie do rx_stage. Zwraca liczbę bajtów (>=0) lub -1 (błąd/CLOSED). */
static int rx_pull(uint16_t want)
{
    char ln[48];
    u1_puts("AT+CIPRXGET=2,");
    { char ps[6]; utoa(want, ps, 10); u1_puts(ps); }
    u1_putc('\r');

    int len = -1;
    for (uint8_t k = 0; k < 8; k++) {
        if (u1_getline(ln, sizeof(ln), 3000) < 0) return -1;
        if (!strncmp(ln, "+CIPRXGET: 2", 12)) {
            char *p = ln + 12;              /* "...2,<len>,<rest>" */
            if (*p == ',') p++;
            len = 0;
            while (*p >= '0' && *p <= '9') len = len * 10 + (*p++ - '0');
            break;
        }
        if (strstr(ln, "CLOSED"))      return -1;   /* serwer zamknął */
        if (!strncmp(ln, "ERROR", 5))  return -1;
    }
    if (len <= 0) return len < 0 ? -1 : 0;

    for (int i = 0; i < len; i++) {
        int c = u1_getc(3000);
        if (c < 0) return -1;
        rx_stage[i] = (uint8_t)c;
    }
    /* po danych: "\r\nOK\r\n" — skonsumuj do OK */
    for (uint8_t k = 0; k < 4; k++) {
        if (u1_getline(ln, sizeof(ln), 1000) < 0) break;
        if (!strncmp(ln, "OK", 2)) break;
    }
    return len;
}

/* Następny bajt strumienia (dociąga przez CIPRXGET gdy staging pusty).
 * Zwraca -1 przy braku danych w limicie czasu / zamknięciu. */
static int rx_getc(void)
{
    if (rx_i < rx_n) return rx_stage[rx_i++];
    rx_n = rx_i = 0;
    for (uint8_t tries = 0; tries < 200; tries++) {   /* ~ do ~8 s łącznie */
        int got = rx_pull(sizeof(rx_stage));
        if (got < 0)  return -1;
        if (got > 0) { rx_n = (uint16_t)got; rx_i = 1; return rx_stage[0]; }
        _delay_ms(40);                                /* bufor pusty -> poczekaj */
    }
    return -1;
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
        u0_puts("BOOT: pwr on\r\n");
        PIN_HIGH(MODEM_PORT, MODEM_PWR_BIT);
        _delay_ms(1200);
        PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
        if (wait_token("RDY", 5000)) u0_puts("BOOT: RDY\r\n");
        else                         u0_puts("BOOT: noRDY\r\n");
        u0_puts("BOOT: warmup\r\n");
        for (uint8_t i = 0; i < 3; i++) { u1_line("AT"); _delay_ms(300); }
    }
    if (!at_ok("ATE0", 1000)) { u0_puts("BOOT: no mdm\r\n"); return 0; }

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
        if (!reg) { u0_puts("BOOT: noreg\r\n"); return 0; }
    }
    u0_puts("BOOT: reg\r\n");

    u0_puts("BOOT: CGATT\r\n");
    at_ok("AT+CGATT=1", 10000);         /* GPRS attach (toleruj) */

    char cstt[64];
    strcpy(cstt, "AT+CSTT=\"" APN_DEFAULT "\",\"\",\"\"");
    u0_puts("BOOT: CSTT\r\n");
    if (!at_ok(cstt, 3000)) { u0_puts("BOOT: cstt!\r\n"); return 0; }

    u0_puts("BOOT: CIICR\r\n");
    if (!at_ok("AT+CIICR", 10000)) { u0_puts("BOOT: ciicr!\r\n"); return 0; }

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
        if (!ip) { u0_puts("BOOT: noIP\r\n"); return 0; }
    }

    /* Tryb RĘCZNEGO odbioru: modem NIE sypie danych sam (+IPD), tylko trzyma je
     * w swoim buforze; host pobiera na żądanie przez AT+CIPRXGET=2,<len>. To daje
     * kontrolę przepływu (host↔modem, lokalnie po UART) — podczas SPM dane czekają
     * w modemie, a nadmiar dławi TCP. Koniec gubienia bajtów. */
    if (!at_ok("AT+CIPRXGET=1", 1000)) { u0_puts("BOOT: rxget!\r\n"); return 0; }

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
    if (!conn) { u0_puts("BOOT: noconn\r\n"); return 0; }
    u0_puts("BOOT: conn, dl\r\n");

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

    /* Potwierdzenie wysłania żądania — to URC modemu (nie dane TCP). */
    {
        uint8_t ok = 0;
        for (uint8_t k = 0; k < 6 && !ok; k++) {
            if (u1_getline(ln, sizeof(ln), 8000) < 0) break;
            if (!strncmp(ln, "SEND OK", 7)) ok = 1;
            if (!strncmp(ln, "ERROR", 5))   return 0;
        }
        /* brak SEND OK nie jest krytyczny — dane i tak pociągniemy przez CIPRXGET */
    }

    /* Nagłówek "FW <size> <crc>\n" — to już DANE TCP: pobieramy je (i resztę
     * obrazu) przez CIPRXGET, bajt po bajcie z rx_getc(). */
    {
        uint8_t n = 0;
        for (;;) {
            int c = rx_getc();
            if (c < 0) return 0;
            if (c == '\n') { ln[n] = '\0'; break; }
            if (c != '\r' && n < sizeof(ln) - 1) ln[n++] = (char)c;
        }
        if (strncmp(ln, "FW ", 3)) { u0_puts("BOOT: noFW\r\n"); return 0; }
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
    if (size == 0 || size > APP_END) { u0_puts("BOOT: badhdr\r\n"); return 0; }
    u0_puts("BOOT: hdr ok, flash\r\n");

    /* --- strumień bajtów -> strony flash + CRC --- *
     * dirty=1 gdy zaczęliśmy zapisywać flash: wtedy błąd zwraca 2 (aplikacja
     * może być częściowo nadpisana — nie wolno do niej skoczyć). */
    uint32_t got = 0, crc = 0xFFFFFFFFUL, addr = 0;
    uint16_t fill = 0;
    uint8_t  dirty = 0;
    memset(page_buf, 0xFF, sizeof(page_buf));
    while (got < size) {
        int c = rx_getc();                  /* pull przez CIPRXGET (host steruje) */
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

    u0_puts("BOOT: CRC ok\r\n");
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
