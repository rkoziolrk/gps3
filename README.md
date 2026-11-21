# gps3 — lokalizator GPS (ATmega64 + SIM900 + PA6C)

Rekonstrukcja firmware lokalizatora GPS floty, odtworzona z obrazu flash
(`reverse/flash.bin`, 64 KiB, ATmega64). Urządzenie raportuje pozycję przez
GPRS do serwera `*.monitoring-gps.net` i czyta dane z magistrali CAN
(paliwo / RPM / pedał).

> **Uwaga:** kod w `src/` to **dekompilacja / rekonstrukcja**, a nie oryginalne
> źródło. Z surowego obrazu flash nie da się odzyskać nazw zmiennych, komentarzy
> ani układu plików. Poziom pewności każdego fragmentu jest oznaczony w kodzie:
> `[HW]` (dokładnie z rejestrów), `[LIB]` (rozpoznana biblioteka avr-libc/libgcc),
> `[REC]` (logika odtworzona z deasemblera + stringów + grafu wywołań).

## Sprzęt

| Element   | Szczegóły                                                        |
|-----------|------------------------------------------------------------------|
| MCU       | Atmel **ATmega64**, **8 MHz**                                     |
| GPS       | GlobalTop **PA6C** (NMEA; konfiguracja PMTK314/101/104)           |
| GSM/GPRS  | SIMCom **SIM900** (komendy AT, TCP)                               |
| USART0    | GPS, **9600 8N1**                                                 |
| USART1    | SIM900, **9600 8N1** (ISR RX składa linie CR/LF)                 |
| CAN       | czujnik paliwa (SONDA1), RPM, pedał                              |
| Pamięć    | wewnętrzny EEPROM (konfiguracja) + zewnętrzna pamięć na programowym I2C (PD0/PD1) |
| Wyświetlacz| LCD **HD44780** 4-bit na PORTC (status urządzenia)              |
| CAN źródło| dane z **zewnętrznego** modułu (brama CAN→szeregowa), parsowane  |

## Struktura katalogów

```
src/      tracker.c           — rekonstrukcja firmware w C
include/  config.h            — konfiguracja sprzętowa (zegar, UART, piny)
docs/     PROTOCOL.md         — ramki protokołu serwera + komendy AT/SMS
          MEMORY_MAP.md       — mapa rejestrów i adresów RAM/flash
reverse/  flash.bin           — oryginalny obraz flash (materiał źródłowy)
          flash_disasm.txt    — pełny deasembler AVR do weryfikacji
```

## Budowanie

Wymaga toolchainu AVR (`avr-gcc`, `avr-libc`, `binutils-avr`):

```bash
make            # -> build/tracker.hex
make size       # rozmiar sekcji
make disasm     # deasembler z ELF
make flash      # wgranie (ustaw PROGRAMMER/PORT)
```

## Weryfikacja rekonstrukcji

Każda funkcja w `src/tracker.c` ma w komentarzu adres `@0x....` z oryginalnego
obrazu. Można go sprawdzić w deasemblerze:

```bash
avr-objdump -D -b binary -m avr:5 reverse/flash.bin | less
```

## Status

- [x] Inicjalizacja sprzętu — zdekodowana 1:1 z rejestrów
- [x] Mapa UART / przerwań RX
- [x] Architektura protokołu (z tablicy stringów)
- [x] Szkielet maszyny stanów + sekwencja AT
- [x] Parser NMEA — rozbicie pól (RMC/GGA/VTG) + podłączenie do ISR
- [x] Pamięć rekordów — wewnętrzny EEPROM AVR (kolejka FIFO)
- [x] CAN — parser pól (paliwo/RPM/pedał/sonda) + wygładzanie
- [x] Wyświetlacz LCD HD44780 (korekta: PORTC to LCD, nie trace UART)
