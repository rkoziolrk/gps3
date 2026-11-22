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

/* ---------------- UART1 (linia modemu, surowo) --------------------------- */
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

    /* --- bringup modemu: impuls PD6 ("WLACZAM MODEM") jak w aplikacji --- */
    PIN_OUT(MODEM_DDR, MODEM_PWR_BIT);
    PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
    PIN_HIGH(MODEM_PORT, MODEM_PWR_BIT);
    _delay_ms(1500);
    PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
    _delay_ms(5000);
    if (!at_ok("ATE0", 1000)) return 0;
    if (!at_ok("AT+CIPSHUT", 2000)) { /* ignoruj */ }
    char cstt[64];
    strcpy(cstt, "AT+CSTT=\"" APN_DEFAULT "\",\"\",\"\"");
    if (!at_ok(cstt, 3000)) return 0;
    if (!at_ok("AT+CIICR", 6000)) return 0;
    if (!at_ok("AT+CIPHEAD=0", 1000)) return 0;

    /* CIPSTART: host:port z cfg domyślnego (TODO: wczytać z EEPROM cfg). */
    char cmd[96];
    strcpy(cmd, "AT+CIPSTART=\"TCP\",\"" SERVER_HOST_DEFAULT "\",");
    { char ps[6]; utoa(SERVER_PORT_DEFAULT, ps, 10); strcat(cmd, ps); }
    u1_line(cmd);
    /* czekaj CONNECT OK */
    uint8_t conn = 0;
    for (uint8_t k = 0; k < 12; k++) {
        if (u1_getline(ln, sizeof(ln), 2000) < 0) break;
        if (!strncmp(ln, "CONNECT OK", 10) || !strncmp(ln, "ALREADY", 7)) { conn = 1; break; }
        if (!strncmp(ln, "CONNECT FAIL", 12)) break;
    }
    if (!conn) return 0;

    /* --- żądanie obrazu --- */
    if (!at_ok("AT+CIPSEND", 2000)) { /* prompt '>' zamiast OK — uproszczenie */ }
    u1_puts("FWREQ\n");
    u1_putc(0x1A);

    /* --- nagłówek "FW <size> <crc>" --- */
    if (u1_getline(ln, sizeof(ln), 8000) < 0) return 0;
    if (strncmp(ln, "FW ", 3)) return 0;
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
    if (size == 0 || size > APP_END) return 0;

    /* --- strumień bajtów -> strony flash + CRC --- */
    uint32_t got = 0, crc = 0xFFFFFFFFUL, addr = 0;
    uint16_t fill = 0;
    memset(page_buf, 0xFF, sizeof(page_buf));
    while (got < size) {
        int c = u1_getc(8000);
        if (c < 0) return 0;
        page_buf[fill++] = (uint8_t)c;
        crc = crc32_up(crc, (uint8_t)c);
        got++;
        if (fill == SPM_PAGESIZE) {
            program_page(addr, page_buf);
            addr += SPM_PAGESIZE;
            fill = 0;
            memset(page_buf, 0xFF, sizeof(page_buf));
        }
    }
    if (fill)                               /* ostatnia, niepełna strona */
        program_page(addr, page_buf);
    crc ^= 0xFFFFFFFFUL;

    if (crc != want_crc) {
        u1_puts("FWERR\n");
        return 0;                           /* zła suma -> ponów przy starcie */
    }

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
        u1_init();
        /* ponawiaj aż do sukcesu — bootloader zostaje, więc to bezpieczne */
        while (!do_ota())
            _delay_ms(5000);
    }
    jump_to_app();
    for (;;)
        ;
}
