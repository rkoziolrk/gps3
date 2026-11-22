/*
 * uart.c — obsługa USART0 (debug) i USART1 (linia GPS+GSM).
 *
 * USART1 odbiera znakowo w ISR i składa linie zakończone CR (0x0D); LF jest
 * ignorowany. Gotowa linia trafia do bufora wymiany, skąd pętla główna
 * pobiera ją przez uart1_get_line(). Podwójny bufor: ISR pisze do rx[], po CR
 * przepisuje do ready[] i ustawia flagę — nadpisanie gubi najstarszą linię
 * (akceptowalne: pętla odpytuje często).
 */
#include "board.h"
#include "uart.h"
#include <avr/interrupt.h>

/* Rejestry USART1 na ATmega64 leżą w rozszerzonym I/O (dostęp przez pamięć),
 * dlatego używamy nazw z <avr/io.h>: UCSR1A/B/C, UBRR1H/L, UDR1. */

/* ---------------- USART0: debug (TX) ------------------------------------- */
void uart0_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = (uint8_t)c;
}

void uart0_puts(const char *s)
{
    while (*s)
        uart0_putc(*s++);
}

/* ---------------- USART1: linia GPS/GSM ---------------------------------- */
void uart1_putc(char c)
{
    while (!(UCSR1A & (1 << UDRE1)))
        ;
    UDR1 = (uint8_t)c;
}

void uart1_puts(const char *s)
{
    while (*s)
        uart1_putc(*s++);
}

void uart1_send_line(const char *s)
{
    uart1_puts(s);
    uart1_putc('\r');
}

/* Odbiór liniowy. */
static volatile char    rx_buf[UART_LINE_MAX];
static volatile uint8_t rx_len;
static volatile char    ready_buf[UART_LINE_MAX];
static volatile uint8_t ready_len;
static volatile uint8_t ready_flag;
static volatile uint8_t prompt_flag;      /* '>' od AT+CIPSEND (bez CR) */

ISR(USART1_RX_vect)
{
    uint8_t s = UCSR1A;
    char    c = (char)UDR1;

    /* Odrzuć znak z błędem ramki/przepełnienia (FE1/DOR1). */
    if (s & ((1 << FE1) | (1 << DOR1)))
        return;

    /* Znak zachęty '>' przychodzi bez CR — wykryj natychmiast. */
    if (c == '>')
        prompt_flag = 1;

    if (c == '\r') {
        /* Koniec linii: oddaj do bufora wymiany (pomiń puste). */
        if (rx_len > 0) {
            uint8_t i;
            for (i = 0; i < rx_len; i++)
                ready_buf[i] = rx_buf[i];
            ready_buf[rx_len] = '\0';
            ready_len  = rx_len;
            ready_flag = 1;
            rx_len = 0;
        }
    } else if (c == '\n') {
        /* LF ignorowany. */
    } else {
        if (rx_len < (UART_LINE_MAX - 1))
            rx_buf[rx_len++] = c;
        /* przepełnienie: nadmiar ucinany, linia dokończy się na CR */
    }
}

uint8_t uart1_got_prompt(void)
{
    uint8_t p;
    cli();
    p = prompt_flag;
    prompt_flag = 0;
    sei();
    return p;
}

int uart1_get_line(char *dst, uint8_t dstsize)
{
    uint8_t len;

    if (!ready_flag)
        return -1;

    /* Sekcja krytyczna: kopiowanie z bufora współdzielonego z ISR. */
    cli();
    len = ready_len;
    if (len > (dstsize - 1))
        len = dstsize - 1;
    for (uint8_t i = 0; i < len; i++)
        dst[i] = ready_buf[i];
    dst[len]   = '\0';
    ready_flag = 0;
    sei();

    return (int)len;
}

/* ---------------- Inicjalizacja ----------------------------------------- */
void uart_init(void)
{
    /* USART0 — debug, TX (RX też włączony, ale nieużywany). */
    UBRR0H = (uint8_t)(UBRR_VAL(UART0_BAUD) >> 8);
    UBRR0L = (uint8_t)(UBRR_VAL(UART0_BAUD));
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);   /* 8N1 */

    /* USART1 — linia GPS/GSM, RX z przerwaniem + TX. */
    UBRR1H = (uint8_t)(UBRR_VAL(UART1_BAUD) >> 8);
    UBRR1L = (uint8_t)(UBRR_VAL(UART1_BAUD));
    UCSR1B = (1 << RXEN1) | (1 << TXEN1) | (1 << RXCIE1);
    UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);   /* 8N1 */

    rx_len = ready_len = ready_flag = 0;
}
