/*
 * main.c — pętla główna lokalizatora (ATmega64).
 *
 * Architektura (docs/REWRITE.md, docs/CAN.md):
 *   - jedna linia RX (USART1): '$...' => GPS, reszta => modem + kandydat CAN,
 *   - USART0 => log/debug i zbieranie surowych ramek (paliwo, pola CAN),
 *   - logowanie adaptacyjne pozycji,
 *   - POS niesie surową ramkę CAN w całości (analiza po stronie serwera).
 *
 * To pierwszy, kompletny wsad do bring-upu na sprzęcie — nie był testowany na
 * urządzeniu. Miejsca wymagające strojenia oznaczono TODO.
 */
#include "board.h"
#include "cfg.h"
#include "cfg_store.h"
#include "uart.h"
#include "clock.h"
#include "debug.h"
#include "gps.h"
#include "gsm.h"
#include "can.h"
#include "track.h"
#include "proto.h"
#include "sms.h"
#include "ota.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <string.h>

#define PING_PERIOD 30        /* s między PING w stanie bezczynności */

static cfg_t cfg;

static void reboot(void)
{
    cli();
    wdt_enable(WDTO_15MS);
    for (;;)
        ;
}

/* Callback odebranego SMS-a (po skasowaniu go z karty). Zmiana konfiguracji
 * wymaga restartu, żeby zadziałała od nowej inicjalizacji. */
static void on_sms(const char *body)
{
    char pin[5] = {0};
    const char *im = gsm_imei();
    uint8_t n = (uint8_t)strlen(im);
    if (n >= 4) { memcpy(pin, im + n - 4, 4); pin[4] = '\0'; }

    dbg_line("SMS tresc", body);         /* pokaż dokładnie co przyszło */
    dbg_line("SMS pin", pin);            /* 4 ostatnie cyfry IMEI (do FWUPD) */

    switch (sms_parse(body, &cfg, pin)) {
    case SMS_SETADDR:
        cfg_save(&cfg);
        dbg_line("SMS", "nowy adres zapisany -> restart");
        reboot();
        break;
    case SMS_RESET:
        cfg_reset(&cfg);
        dbg_line("SMS", "reset do fabrycznych -> restart");
        reboot();
        break;
    case SMS_UPDATE:
        dbg_line("SMS", "zadanie OTA -> bootloader");
        ota_request(&cfg);         /* zapis hosta+flagi, restart do bootloadera */
        break;
    default:
        dbg_line("SMS", "brak akcji (zwykly tekst / zly format / zly PIN)");
        break;
    }
}

static void hw_init(void)
{
    MCUCSR &= ~(1 << WDRF);
    wdt_disable();

    uart_init();
    clock_init();
    dbg_init();

    cfg_load(&cfg);
    gps_init();
    can_init();
    gsm_init(&cfg, on_sms);
    track_init(&cfg);

    sei();
    wdt_enable(WDTO_2S);
}

int main(void)
{
    char    line[UART_LINE_MAX];
    char    frame[180];
    uint8_t greeted = 0;
    uint32_t last_ping = 0, last_led = 0, last_can_diag = 0;

    hw_init();
    dbg_kv("CFG", "port", cfg.port);
    dbg_line("CFG", cfg.host);

    for (;;) {
        uint32_t now = clock_uptime();

        /* 1) Odbiór i klasyfikacja linii. */
        if (uart1_get_line(line, sizeof(line)) >= 0) {
            if (line[0] == '$') {
                dbg_raw("GPS", line);
                if (gps_feed(line)) {               /* świeży, ważny fiks (RMC) */
                    const gps_fix_t *fx = gps_current();
                    char mv;
                    if (track_update(fx, now, &mv)) {
                        const char *craw = can_last_raw();
                        proto_pos(frame, sizeof(frame), fx, mv,
                                  craw[0] ? craw : NULL);
                        if (gsm_send_frame(frame))
                            can_clear();
                        dbg_raw("POS", frame);
                    }
                }
            } else {
#if DBG_RAW_LINE
                dbg_raw("LINE", line);
#endif
                gsm_feed(line);
                if (can_feed(line)) {
#if DBG_RAW_CAN
                    dbg_raw("CAN", line);           /* zbieranie danych do analizy */
#endif
                }
            }
        }

        /* 2) Napęd modemu. */
        gsm_tick(now);

        /* 3) Handshake po zestawieniu TCP. */
        if (gsm_online() && !greeted && gsm_imei()[0]) {
            proto_hello(frame, sizeof(frame), gsm_imei());
            if (gsm_send_frame(frame)) {
                greeted = 1;
                last_ping = now;
                dbg_raw("HELLO", frame);
            }
        }
        if (!gsm_online())
            greeted = 0;

        /* 4) Keepalive. */
        if (gsm_online() && greeted && (now - last_ping) >= PING_PERIOD) {
            proto_ping(frame, sizeof(frame), gsm_imei(), now);
            if (gsm_send_frame(frame))
                last_ping = now;
        }

        /* 4b) Diagnostyka CAN po TCP — zdalne zbieranie surowych ramek, gdy
         *     debug USART0 jest niedostępny (jazda). POS też niesie CAN, ale
         *     ten raport działa też na postoju z pracującym silnikiem. */
#if DIAG_CAN_TCP
        if (gsm_online() && greeted && (now - last_can_diag) >= DIAG_CAN_PERIOD) {
            const char *craw = can_last_raw();
            if (craw[0]) {
                proto_raw(frame, sizeof(frame), "CAN", craw);
                if (gsm_send_frame(frame)) {
                    last_can_diag = now;
                    can_clear();
                }
            }
        }
#endif

        /* 5) Dioda statusu: miganie (online = szybciej). TODO: strojenie. */
        if ((now - last_led) >= (gsm_online() ? 1u : 2u)) {
            LED_PORT ^= (1 << LED_BIT);
            last_led = now;
        }

        /* SMS (zmiana hosta / reset / OTA) obsługiwany w gsm_tick() -> on_sms(). */

        wdt_reset();
    }
}
