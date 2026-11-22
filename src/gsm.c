/*
 * gsm.c — sterownik SIM900: sekwencja inicjalizacji, GPRS i TCP.
 *
 * Sekwencja AT odtworzona z docs/PROTOCOL.md. Każdy krok wysyła komendę i czeka
 * na oczekiwaną odpowiedź w limicie czasu; po timeoutcie ponawia, a po serii
 * błędów restartuje sekwencję. UWAGA: wymaga strojenia na sprzęcie (czasy
 * PWRKEY, dokładne odpowiedzi operatora) — patrz docs/REWRITE.md.
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
static uint8_t      state;
static uint8_t      step;
static uint32_t     step_t0;
static uint8_t      retries;
static char         imei[16];
static char         pending[160];       /* ramka czekająca na wysłanie */
static uint8_t      has_pending;

/* Flagi ustawiane przez gsm_feed(), czytane przez gsm_tick(). */
static volatile uint8_t f_ok, f_connect, f_prompt, f_sendok, f_closed;

#define STEP_TIMEOUT   10        /* s na odpowiedź kroku           */
#define MAX_RETRIES     3

const char *gsm_imei(void)  { return imei; }
uint8_t     gsm_online(void){ return state == G_ONLINE || state == G_SEND_PROMPT || state == G_SEND_DATA; }

void gsm_init(const cfg_t *c)
{
    cfg = *c;
    imei[0] = '\0';
    has_pending = 0;

    /* Piny zasilania/enable modułów. */
    PIN_OUT(GSM_PWR_DDR, GSM_PWR_BIT);
    PIN_OUT(GPS_PWR_DDR, GPS_PWR_BIT);
    PIN_OUT(HWRST_DDR, HWRST_BIT);
    PIN_OUT(LED_DDR, LED_BIT);
    PIN_OUT(PWRLATCH_DDR, PWRLATCH_A3);
    PIN_OUT(PWRLATCH_DDR, PWRLATCH_A4);
    PIN_HIGH(PWRLATCH_DDR, PWRLATCH_A3);

    /* Włącz zasilanie GPS i GSM (poziomy do potwierdzenia na sprzęcie). */
    PIN_HIGH(GPS_PWR_PORT, GPS_PWR_BIT);
    PIN_HIGH(GSM_PWR_PORT, GSM_PWR_BIT);

    state = G_SEQ;
    step = 0;
    retries = 0;
    step_t0 = 0;
}

/* Zbuduj komendę dla danego kroku do bufora out. Zwraca oczekiwany token
 * (ustawiany jako wskaźnik flagi) przez parametr wyjściowy *want (0=OK). */
static const char *step_cmd(uint8_t s, char *out, uint8_t osz)
{
    switch (s) {
    case 0:  return "ATE0";
    case 1:  return "AT+CMGF=1";
    case 2:  return "AT+GSN";                 /* IMEI */
    case 3:  return "AT+CREG=2";
    case 4:  return roam_cops_cmd(cfg.roam_mode, "");
    case 5:  return "AT+CGATT=1";
    case 6:  snprintf(out, osz, "AT+CSTT=\"%s\",\"\",\"\"", APN_DEFAULT); return out;
    case 7:  return "AT+CIICR";
    case 8:  return "AT+CIFSR";
    case 9:  return "AT+CIPHEAD=1";
    case 10: snprintf(out, osz, "AT+CIPSTART=\"TCP\",\"%s\",%u", cfg.host, cfg.port); return out;
    default: return NULL;
    }
}
#define STEP_LAST 10

void gsm_feed(const char *line)
{
    /* Przechwyć IMEI: linia złożona z 15 cyfr. */
    if (imei[0] == '\0') {
        uint8_t i = 0, dig = 1;
        while (line[i]) { if (line[i] < '0' || line[i] > '9') { dig = 0; break; } i++; }
        if (dig && i == 15) { memcpy(imei, line, 15); imei[15] = '\0'; }
    }

    if (!strncmp(line, "OK", 2))            f_ok = 1;
    if (!strncmp(line, "CONNECT OK", 10))   f_connect = 1;
    if (!strncmp(line, "ALREADY CONNECT", 15)) f_connect = 1;
    if (!strncmp(line, "SEND OK", 7))       f_sendok = 1;
    if (!strncmp(line, "> ", 2) || line[0] == '>') f_prompt = 1;
    if (!strncmp(line, "CLOSED", 6) || !strncmp(line, "CONNECT FAIL", 12)) f_closed = 1;
}

static void send_step(uint32_t now)
{
    char tmp[96];
    const char *cmd = step_cmd(step, tmp, sizeof(tmp));
    f_ok = f_connect = 0;
    if (cmd)
        uart1_send_line(cmd);
    step_t0 = now;
}

void gsm_tick(uint32_t now)
{
    switch (state) {
    case G_SEQ:
        if (step_t0 == 0) { send_step(now); return; }

        /* Krok z IMEI (2) i CIFSR (8) kończymy na OK. CIPSTART (10) czeka na
         * CONNECT OK (OK oznacza tylko przyjęcie komendy). */
        if (step == STEP_LAST) {
            if (f_connect) { state = G_ONLINE; retries = 0; dbg_line("GSM", "online (TCP)"); return; }
        } else {
            if (f_ok) {
                step++;
                step_t0 = 0;
                retries = 0;
                if (step > STEP_LAST) step = STEP_LAST;
                return;
            }
        }
        if ((now - step_t0) >= STEP_TIMEOUT) {
            if (++retries > MAX_RETRIES) {    /* restart całej sekwencji */
                dbg_line("GSM", "sekwencja timeout -> restart");
                step = 0; retries = 0; step_t0 = 0; f_closed = 0;
            } else {
                send_step(now);               /* ponów krok */
            }
        }
        break;

    case G_ONLINE:
        if (f_closed) { f_closed = 0; dbg_line("GSM", "TCP CLOSED -> reconnect"); state = G_SEQ; step = 10; step_t0 = 0; return; }
        if (has_pending) {                    /* zacznij wysyłkę */
            f_prompt = 0;
            uart1_send_line("AT+CIPSEND");
            step_t0 = now;
            state = G_SEND_PROMPT;
        }
        break;

    case G_SEND_PROMPT:
        if (f_prompt) {
            uart1_puts(pending);
            uart1_putc(0x1A);                 /* Ctrl-Z kończy dane */
            f_sendok = 0;
            step_t0 = now;
            state = G_SEND_DATA;
        } else if ((now - step_t0) >= STEP_TIMEOUT) {
            state = G_ONLINE;                 /* brak promptu — porzuć wysyłkę */
        }
        break;

    case G_SEND_DATA:
        if (f_sendok) { has_pending = 0; state = G_ONLINE; }
        else if (f_closed) { f_closed = 0; has_pending = 0; state = G_SEQ; step = 10; step_t0 = 0; }
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
