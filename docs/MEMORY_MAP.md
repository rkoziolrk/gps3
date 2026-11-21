# Mapa pamięci i rejestrów

Zdekodowane z inicjalizacji firmware (`reverse/flash.bin`, @0x8c..0x1d4)
i z procedur obsługi przerwań.

## Obraz flash

| Zakres            | Zawartość                                   |
|-------------------|---------------------------------------------|
| 0x0000..0x008B    | tablica wektorów przerwań                   |
| 0x008C            | reset / start (crt0 + `main`)               |
| ~0x0100..0x7800   | kod aplikacji                               |
| ~0xA300..0xC3EE   | runtime avr-libc/libgcc + tablica stringów  |
| 0xC3EE..0xFFFF    | puste (0xFF)                                |

Wykorzystane: 0x0000..0xC3EE (~50 KiB z 64 KiB).

## Wektory przerwań (bajtowe adresy)

| Adres | Wektor            | Handler  | Rola                          |
|-------|-------------------|----------|-------------------------------|
| 0x00  | RESET             | 0x008C   | start                         |
| 0x38  | USART0 RX Complete| 0xA33A   | bufor pierścieniowy RX (GPS)  |
| 0x78  | USART1 RX Complete| 0xA3E6   | składanie linii CR/LF (GSM)   |

Pozostałe wektory → `reti` (nieużywane).

## Rejestry ustawione w init (pewne)

| Rejestr        | Wartość | Znaczenie                                      |
|----------------|---------|------------------------------------------------|
| SPH:SPL        | 0x10FF  | stos na szczycie 4 KiB SRAM                    |
| MCUCSR         | &=~WDRF | kasowanie flagi resetu WDT                     |
| WDTCR          | 0x18→0  | wyłączenie watchdoga                            |
| UBRR0L / UBRR1L| 51      | 9600 baud @ 8 MHz                              |
| UCSR0B / UCSR1B| 0x18    | RXEN \| TXEN                                    |
| TCCR0          | 0x86    | Timer0 — tick systemowy                        |
| OCR0           | 0xC0    | wartość porównania                             |
| DDRA           | .3,.4   | wyjścia (PORTA.3/.4 = 1)                        |
| DDRD           | .5,.6,.7| GSM_PWR / GPS_PWR / RESET                       |
| DDRB           | .4,.6   | CAN / LED (PORTB.6 = 1)                         |

## Kluczowe adresy RAM (z deasemblera)

| Adres   | Zmienna (odtworzona)                                 |
|---------|------------------------------------------------------|
| 0x004C/4D | wskaźnik roboczy (używany w ISR, baza do stringów) |
| 0x0A9C  | bufor RX USART0 (ring)                               |
| 0x0AA0  | licznik przyjętych bajtów USART0                     |
| 0x0B38  | stan odbioru linii USART1 (==0x22 aktywny)           |
| 0x0B39  | bufor bieżącej linii NMEA/AT                          |
| 0x0C46  | bufor roboczy do składania/wypisywania napisów       |

## Rozpoznane rutyny biblioteczne (avr-libc / libgcc)

| Adres   | Wywołań | Odpowiednik          |
|---------|---------|----------------------|
| 0xAEAC  | 352     | `strcpy_P`           |
| 0xB7D8  | 186     | `strcpy`             |
| 0xB85A  | 122     | `strcmp` / `strcmp_P`|
| 0xB672  | —       | `itoa` / `ltoa`      |
| 0xB680  | —       | `utoa` / `ultoa`     |
| 0xB714  | —       | `atoi` / `strtol`    |
| 0xAB22  | 140     | `uart0_puts_P`       |
| 0xAB16  | —       | `uart0_puts`         |
| 0xAC0C  | —       | `uart0_putc`         |
| 0xAC72  | 71      | `_delay_ms`          |
| 0xAE98  | 32      | `__mulhi3` (libgcc)  |
| 0xB25C  | —       | `__lpm` (libgcc)     |
