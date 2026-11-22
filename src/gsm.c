/*
 * gsm.c — sterownik SIM900: sekwencja inicjalizacji, odczyt SMS, GPRS i TCP.
 *
 * Sekwencja AT odtworzona z docs/PROTOCOL.md. Każdy krok wysyła komendę i czeka
 * na oczekiwaną odpowiedź w limicie czasu; po timeoutcie ponawia, a po serii
 * błędów restartuje sekwencję. SMS z pozycji 1 jest czytany w trybie komend
 * (przed zestawieniem TCP), kasowany, a następnie zgłaszany przez callback.
 * UWAGA: wymaga strojenia na sprzęcie (czasy PWRKEY, odpowiedzi operatora) —
 * patrz docs/REWRITE.md.
 */
#include "board.h"
#include "gsm.h"
#include "uart.h"
#include "debug.h"
#include "roam.h"
#include <avr/io.h>
#include <util/delay.h>
#include <string.h>
#include <stdio.h>

enum { G_OFF, G_SEQ, G_ONLINE, G_SEND_PROMPT, G_SEND_DATA };

static cfg_t        cfg;
static void       (*sms_handler)(const char *);
static uint8_t      state;
static uint8_t      step;
static uint32_t     step_t0;
static uint8_t      retries;
static char         imei[16];
static char         pending[160];
static uint8_t      has_pending;

/* Odczyt SMS. */
static uint8_t      in_cmgr;         /* trwa AT+CMGR=1 — przechwytuj treść    */
static uint8_t      in_cifsr;        /* trwa AT+CIFSR — odpowiedź to IP, nie OK */
static uint8_t      sms_marker;      /* widziano nagłówek +CMGR:              */
static uint8_t      sms_have;        /* złapano treść SMS                     */
static char         sms_body[80];

/* Flagi ustawiane przez gsm_feed(), czytane przez gsm_tick(). */
static volatile uint8_t f_ok, f_err, f_connect, f_prompt, f_sendok, f_closed;

#define STEP_TIMEOUT   10
#define MAX_RETRIES     3
#define STEP_CMGR       3
#define STEP_CMGD       4
#define STEP_COPS       6
#define STEP_CIFSR     10
#define STEP_LAST      12

const char *gsm_imei(void)   { return imei; }
uint8_t     gsm_online(void) { return state == G_ONLINE || state == G_SEND_PROMPT || state == G_SEND_DATA; }

/* Przełącznik źródła współdzielonej linii UART (PA3). W oryginale konteksty
 * modemu ustawiają PA3=0, a GPS jest słyszalny przy PA3=1 (PA4 pozostaje 1).
 *   modem=1 -> PA3=0 (wycisz GPS, słuchaj modemu)
 *   modem=0 -> PA3=1 (słuchaj GPS)
 * Patrz docs/MEMORY_MAP.md. */
static void gsm_route(uint8_t modem)
{
    if (modem) PIN_LOW(PWRLATCH_PORT, PWRLATCH_A3);
    else       PIN_HIGH(PWRLATCH_PORT, PWRLATCH_A3);
}

void gsm_init(const cfg_t *c, void (*sms_cb)(const char *))
{
    cfg = *c;
    sms_handler = sms_cb;
    imei[0] = '\0';
    has_pending = 0;
    sms_have = sms_marker = in_cmgr = 0;

    /* Piny modułów: wyjścia, stan NISKI (idle) — jak w oryginale. */
    PIN_OUT(MODEM_DDR, MODEM_PWR_BIT);
    PIN_OUT(MODEM_DDR, MODEM_RST_BIT);
    PIN_OUT(MODEM_DDR, GPS_RST_BIT);
    PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
    PIN_LOW(MODEM_PORT, MODEM_RST_BIT);
    PIN_LOW(MODEM_PORT, GPS_RST_BIT);
    PIN_OUT(LED_DDR, LED_BIT);
    PIN_OUT(PWRLATCH_DDR, PWRLATCH_A3);
    PIN_OUT(PWRLATCH_DDR, PWRLATCH_A4);
    PIN_HIGH(PWRLATCH_PORT, PWRLATCH_A3);
    PIN_HIGH(PWRLATCH_PORT, PWRLATCH_A4);

    /* Włączenie modemu jak oryginał: impuls HIGH na PD6 ("WLACZAM MODEM"),
     * potem czas na boot. Bez tego SIM900 nie startuje → brak odpowiedzi AT.
     * (komunikaty bez znacznika czasu — przerwania jeszcze wyłączone) */
    uart0_puts("MODEM: wlaczam (PD6 impuls 1.5s)...\r\n");
    PIN_HIGH(MODEM_PORT, MODEM_PWR_BIT);
    _delay_ms(1500);
    PIN_LOW(MODEM_PORT, MODEM_PWR_BIT);
    uart0_puts("MODEM: boot ~5s...\r\n");
    _delay_ms(5000);                         /* oryginał czeka 15 s; skrócone */

    gsm_route(1);                            /* słuchaj modemu (wycisz GPS)   */

    /* Rozgrzewka autobaud SIM900: kilka "AT" pozwala wykryć 9600. */
    for (uint8_t i = 0; i < 5; i++) {
        uart1_send_line("AT");
        _delay_ms(300);
    }

    state = G_SEQ;
    step = 0;
    retries = 0;
    step_t0 = 0;
}

static const char *step_cmd(uint8_t s, char *out, uint8_t osz)
{
    switch (s) {
    case 0:  return "ATE0";
    case 1:  return "AT+GSN";                 /* IMEI (przed odczytem SMS)     */
    case 2:  return "AT+CMGF=1";              /* SMS w trybie tekstowym        */
    case 3:  return "AT+CMGR=1";              /* czytaj SMS z pozycji 1        */
    case 4:  return "AT+CMGD=1";              /* skasuj pozycję 1              */
    case 5:  return "AT+CREG=2";
    case 6:  return roam_cops_cmd(cfg.roam_mode, "");
    case 7:  return "AT+CGATT=1";
    case 8:  snprintf(out, osz, "AT+CSTT=\"%s\",\"\",\"\"", APN_DEFAULT); return out;
    case 9:  return "AT+CIICR";
    case 10: return "AT+CIFSR";
    case 11: return "AT+CIPHEAD=1";
    case 12: snprintf(out, osz, "AT+CIPSTART=\"TCP\",\"%s\",%u", cfg.host, cfg.port); return out;
    default: return NULL;
    }
}

void gsm_feed(const char *line)
{
    if (imei[0] == '\0') {
        uint8_t i = 0, dig = 1;
        while (line[i]) { if (line[i] < '0' || line[i] > '9') { dig = 0; break; } i++; }
        if (dig && i == 15) { memcpy(imei, line, 15); imei[15] = '\0'; }
    }

    /* Przechwytywanie treści SMS podczas AT+CMGR=1. */
    if (in_cmgr) {
        if (!strncmp(line, "+CMGR:", 6)) {
            sms_marker = 1;
        } else if (sms_marker && line[0] && strncmp(line, "OK", 2)) {
            strncpy(sms_body, line, sizeof(sms_body) - 1);
            sms_body[sizeof(sms_body) - 1] = '\0';
            sms_have = 1;
            sms_marker = 0;
        }
    }

    /* AT+CIFSR zwraca sam adres IP (linia zaczynająca się cyfrą), bez "OK". */
    if (in_cifsr && line[0] >= '0' && line[0] <= '9') f_ok = 1;

    if (!strncmp(line, "OK", 2))              f_ok = 1;
    if (!strncmp(line, "ERROR", 5))           f_err = 1;
    if (!strncmp(line, "CONNECT OK", 10))     f_connect = 1;
    if (!strncmp(line, "ALREADY CONNECT", 15))f_connect = 1;
    if (!strncmp(line, "SEND OK", 7))         f_sendok = 1;
    if (!strncmp(line, "> ", 2) || line[0] == '>') f_prompt = 1;
    if (!strncmp(line, "CLOSED", 6) || !strncmp(line, "CONNECT FAIL", 12)) f_closed = 1;
}

static void send_step(uint32_t now)
{
    char tmp[96];
    const char *cmd = step_cmd(step, tmp, sizeof(tmp));
    f_ok = f_err = f_connect = 0;
    in_cmgr  = (step == STEP_CMGR);
    in_cifsr = (step == STEP_CIFSR);
    gsm_route(1);                       /* na czas komendy i odpowiedzi: modem */
    if (cmd) {
        dbg_line("GSM>", cmd);          /* pokaż, co wysyłamy do modemu */
        uart1_send_line(cmd);
    }
    step_t0 = now;
}

void gsm_tick(uint32_t now)
{
    switch (state) {
    case G_SEQ:
        if (step_t0 == 0) { send_step(now); return; }

        if (step == STEP_LAST) {
            if (f_connect) { state = G_ONLINE; retries = 0; dbg_line("GSM", "online (TCP)"); }
            else if ((now - step_t0) >= STEP_TIMEOUT) {
                if (++retries > MAX_RETRIES) { step = 0; retries = 0; step_t0 = 0; f_closed = 0; }
                else send_step(now);
            }
            return;
        }

        /* CMGD (pusta pozycja) i COPS (gdy już zarejestrowany) mogą dać ERROR
         * — traktujemy to jak sukces i idziemy dalej. */
        if (f_ok || ((step == STEP_CMGD || step == STEP_COPS) && f_err)) {
            if (step == STEP_CMGD) {
                in_cmgr = 0;
                if (sms_have && sms_handler) {
                    sms_have = 0;
                    sms_handler(sms_body);     /* może nie wrócić (reboot/OTA) */
                }
            }
            step++;
            step_t0 = 0;
            retries = 0;
            return;
        }
        if ((now - step_t0) >= STEP_TIMEOUT) {
            if (++retries > MAX_RETRIES) {
                dbg_line("GSM", "sekwencja timeout -> restart");
                step = 0; retries = 0; step_t0 = 0; f_closed = 0;
            } else {
                send_step(now);
            }
        }
        break;

    case G_ONLINE:
        if (f_closed) { f_closed = 0; dbg_line("GSM", "TCP CLOSED -> reconnect"); state = G_SEQ; step = STEP_LAST; step_t0 = 0; return; }
        if (has_pending) {
            gsm_route(1);               /* wysyłka -> słuchaj modemu */
            f_prompt = 0;
            uart1_send_line("AT+CIPSEND");
            step_t0 = now;
            state = G_SEND_PROMPT;
        } else {
            gsm_route(0);               /* bezczynny online -> słuchaj GPS */
        }
        break;

    case G_SEND_PROMPT:
        if (f_prompt) {
            uart1_puts(pending);
            uart1_putc(0x1A);
            f_sendok = 0;
            step_t0 = now;
            state = G_SEND_DATA;
        } else if ((now - step_t0) >= STEP_TIMEOUT) {
            state = G_ONLINE;
        }
        break;

    case G_SEND_DATA:
        if (f_sendok) { has_pending = 0; state = G_ONLINE; }
        else if (f_closed) { f_closed = 0; has_pending = 0; state = G_SEQ; step = STEP_LAST; step_t0 = 0; }
        else if ((now - step_t0) >= STEP_TIMEOUT) { has_pending = 0; state = G_ONLINE; }
        break;

    default:
        break;
    }
}

uint8_t gsm_send_frame(const char *frame)
{
    if (!gsm_online() || has_pending)
        return 0;
    strncpy(pending, frame, sizeof(pending) - 1);
    pending[sizeof(pending) - 1] = '\0';
    has_pending = 1;
    return 1;
}
