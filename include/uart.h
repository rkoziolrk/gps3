/*
 * uart.h — dwa UART-y ATmega64.
 *   USART1: jedyna linia komunikacji (GPS + GSM), odbiór liniowy przez ISR.
 *   USART0: port debug (tylko nadawanie).
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>

#define UART_LINE_MAX 96          /* maks. długość odbieranej linii (bez CRLF) */

/* --- Inicjalizacja ------------------------------------------------------- */
void uart_init(void);

/* --- USART1: linia GPS/GSM ----------------------------------------------- */
void uart1_putc(char c);
void uart1_puts(const char *s);          /* bez dodawania CR/LF               */
void uart1_send_line(const char *s);     /* dodaje CR                          */

/*
 * uart1_get_line: jeśli czeka kompletna linia, kopiuje ją (zakończoną '\0')
 * do bufora użytkownika i zwraca jej długość; inaczej zwraca -1.
 * Linia jest pozbawiona CR/LF. Bezpieczne wywoływanie w pętli głównej.
 */
int uart1_get_line(char *dst, uint8_t dstsize);

/* --- USART0: debug ------------------------------------------------------- */
void uart0_putc(char c);
void uart0_puts(const char *s);

#endif /* UART_H */
