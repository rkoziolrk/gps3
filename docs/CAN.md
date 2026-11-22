# CAN — skąd pochodzą dane (analiza deasemblera)

Prześledzone w `reverse/flash_disasm.txt`. Pytanie wyjściowe: skąd w rekordzie
biorą się pola CAN i czym jest pole „?B" (kandydat na paliwo).

## Wniosek najważniejszy

**ATmega64 nie dekoduje magistrali CAN auta.** Układ nie ma kontrolera CAN i w
kodzie nie ma obsługi ramek CAN z samochodu. Dane przychodzą **już zdekodowane,
tekstowo, z zewnętrznego modułu CAN** — firmware tylko je parsuje, wygładza i
przepisuje do rekordu.

To jest kluczowe dla „?B ≈ 0287 przy prawie pełnym baku": liczba pochodzi z
**własnej skali modułu CAN** (zaszytej w firmware modułu, którego nie mamy), a
nie z przeliczenia w tym firmware. Dlatego nie odpowiada litrom ani procentom.

## Pola, które firmware zna po nazwie

W funkcji obróbki CAN (rejon `0x8a00..0xa2a0`) są etykiety ładowane do
wyświetlacza/logu debug (`call 0xab22` = `lcd_puts_P`, Z = adres stringu flash):

| string (flash) | adres | znaczenie |
|---|---|---|
| `C=` | 0xc3a6 | pole CAN (nieustalone — licznik?) |
| `V=` | 0xc3aa | prędkość |
| ` Pedal=` | 0xc3ae | pedał gazu |
| `Paliwo=` | 0xc3b6 | **paliwo** (jawne pole od modułu) |
| ` RPM=` | 0xc3be | obroty (tylko na LCD, nie w rekordzie) |
| `Li=` | 0xc3c4 | nieustalone |
| ` B=` | 0xc3c8 | nieustalone |
| `SONDA1=` | 0xc35c | sonda (osobny czujnik) |

Czyli moduł dostarcza **jawną wartość „Paliwo"** — pole paliwa istnieje i jest
realne. `?B` w rekordzie (sekcja CAN2) jest jego **silnym kandydatem**, ale
dokładnego odwzorowania bajt-w-bajt token→`?B` nie potwierdzono na 100 %.

## Wygładzanie paliwa

Przy `[0x0B5E]==6` firmware liczy uśrednianie (rejon `0xa018..0xa0b0`), stringi:
`WYGLADZ= J=` (0xc364), ` sum_pal=` (0xc370), ` X33=`, ` X22=`, ` W_can=`
(0xc3cc, „wygładzony CAN"), ` W_sonda1=`. To potwierdza, że wartość paliwa jest
**filtrowana/uśredniana** zanim trafi do rekordu — stąd jej stabilność na
krótkim przejeździe.

## Obsługa ramki i błędy

- Ramka dzielona na tokeny funkcją `0xb672` (Mid/split) po separatorze; tokeny
  są z powrotem sklejane (`0xb7d8` strcpy / `0xaae6` strcat) do pól rekordu.
- `BRAK CAN ` (0xc3d4, `@0xa1dc`) — brak danych z modułu: pola CAN są zerowane.
- `RAMKA CAN ERROR` (0xc3de, `@0xa26a`) — ramka nie przeszła walidacji
  (sprawdzane bajty nagłówka w `0x0AA1`/`0x0AA2`).

## Transport (jak dane wchodzą do ATmegi)

| | USART1 | USART0 |
|---|---|---|
| RX (odbiór) | **tak** — jedyny ISR RX `@0xA3E6` (wektor 0x78), czyta `UDR1` (mem 0x009C), składa linię do bufora `0x0B39` do znaku CR | **nie** — brak odczytu `UDR0`, brak pollingu RXC0 |
| TX (nadawanie) | tak (`sts 0x009C`) | tak (`out 0x0c`, `@0xadcc`) |

**Cały odbiór idzie jedną linią (USART1)** — GPS (`$…`), modem (AT) i ramki
modułu CAN są demultipleksowane po treści linii. **USART0 jest wyłącznie
nadawczy** (log/debug lub odpytywanie modułu). W firmware nie ma odbioru na
USART0, więc dane CAN docierają jako tekst na współdzielonej linii USART1, a nie
osobnym portem. Jest to zgodne z obserwacją, że cała komunikacja ATmegi z
GPS/GSM jest na jednym UART.

## Co z tego wynika dla identyfikacji paliwa

- Pole paliwa **istnieje** i jest podawane przez moduł — nie trzeba go
  „odgadywać" z surowego CAN.
- Jego **skala jest własnością modułu** (nie tego firmware), więc wartości ~287
  nie przeliczymy na litry bez punktu odniesienia.
- Do kalibracji nadal potrzebny: drugi log przy innym poziomie baku albo liczba
  z oryginalnej platformy (`docs/SERVER.md`, `docs/REWRITE.md`).
