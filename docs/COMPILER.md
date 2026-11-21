# Kompilator oryginalnego firmware

**Wniosek:** firmware skompilowano w **BASCOM-AVR** (BASIC, MCS Electronics),
a nie w avr-gcc ani CodeVisionAVR. Oryginalne źródło było więc w BASIC-u;
`src/tracker.c` to przepisanie logiki na C.

W obrazie nie ma żadnego napisu z nazwą kompilatora. Przypisanie opiera się na
cechach kodu wymienionych niżej.

## Dlaczego nie avr-gcc

| cecha avr-gcc                                     | w obrazie                         |
|---------------------------------------------------|-----------------------------------|
| `eor r1,r1` w kodzie startowym (rejestr zerowy)   | brak (0 wystąpień `eor/clr r1`)   |
| `r1` stale równy 0, `clr r1` po każdym `mul`      | mnożenie `0xae98` zostawia `r1`   |
| argumenty w `r24/r25, r22/r23 …`                  | argumenty jako adresy na stosie Y |
| nieużywane wektory → `jmp __bad_interrupt`        | nieużywane wektory → `reti`       |
| `__do_copy_data` / `__do_clear_bss`               | brak                              |

## Dlaczego nie CodeVisionAVR

Kod startowy CodeVision zaczyna się od `cli`, zeruje `EECR`, czyści rejestry
R2–R14 i umieszcza stos danych na początku SRAM. Tu nic z tego nie występuje,
a CodeVision nie używa pary R4:R5 jako wskaźnika ramki.

## Cechy zgodne z BASCOM-AVR

**Kod startowy (`0x8c..0xe6`):**

```
SP  = 0x10FF            ; stos sprzętowy na końcu RAM
Y   = 0x1038            ; stos programowy = 0x1100 − 200  ($hwstack = 200)
R4:R5 = 0x0C5E          ; wskaźnik ramki (zmienne tymczasowe, $framesize)
wdr
R0  = MCUCSR            ; zachowana przyczyna resetu
MCUCSR &= ~WDRF ; WDTCR = 0x18, 0x00   ; watchdog wyłączony
wyzerowanie RAM 0x0100..0x10FD
UBRR0/UCSR0B, UBRR1/UCSR1B = 9600      ; $baud / $baud1 ustawiane w starcie
call 0xab7c             ; inicjalizacja LCD jeszcze przed kodem użytkownika
clr r6                  ; rejestr flag biblioteki
```

**Wywołania funkcji:** argumenty są kopiowane do ramki R4:R5 (`0xae84`,
`0xae90`, zwalnianie `0xa9ea`), a na stos Y trafiają ich **adresy**
(`st -Y,r5 / st -Y,r4`) — przekazywanie przez referencję, jak w BASCOM.

**Przerwania:** obsługa RX zapisuje stały zestaw rejestrów R0–R5, R7, R10, R11,
R16–R31 i SREG, a pomija R6, R8, R9, R12–R15.

**Rejestr R6 jako flagi:** biblioteka zapisuje bit 2 rejestru R6
(`bld r6, 2`), np. stan ACK w odczycie I2C.

**Biblioteka:** procedury działają jak instrukcje BASIC-a — `Mid()` z pozycją
od 1, `Val()` zwracające Long w R16–R19, `Str()`, `Format()` z obsługą znaku
`+`, `Len()`, `Instr()`, `Waitms`, `Wait`, `Cls`, `Lowerline`, `Lcd`,
programowe `I2cstart/I2cwbyte/I2crbyte/I2cstop` z open-drain przez DDR.
Pełna lista: nagłówek `src/tracker.c`, sekcja `[LIB]`.

## Ograniczenia

- Nazwy instrukcji BASCOM są przypisane po działaniu procedur, a nie odczytane
  z obrazu.
- Rozpoznanie opiera się na znajomości wzorców kodu BASCOM-AVR; nie zostało
  sprawdzone przez skompilowanie programu wzorcowego, bo BASCOM nie jest tu
  dostępny.
- Dokładnego podziału obszaru 0x0C5E–0x1037 (986 B) na `$swstack` i
  `$framesize` nie da się odczytać z samego kodu startowego.

## Konsekwencja dla rekonstrukcji

Funkcja `get_field()` (odpowiednik `Mid`) liczy pozycje od 1, tak jak oryginał.
Pozycje pól odpowiedzi `CONFIG` w `docs/PROTOCOL.md` są podane od 1.
