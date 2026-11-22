# Firmware (nowy) — budowa, moduły, bring-up

Czysty firmware w C (avr-gcc) wg decyzji z `docs/REWRITE.md`. Buduje się do
`build/tracker.hex`. **Status: pierwszy kompletny wsad, nietestowany na
sprzęcie** — miejsca do strojenia oznaczono `TODO` w kodzie.

## Budowanie

```bash
make            # -> build/tracker.hex (+ rozmiar)
make flash      # avrdude; ustaw PROGRAMMER/PORT
make clean
```

Zużycie: ~9 KB flash (14%), ~1,3 KB RAM.

## Moduły (`src/`, `include/`)

| plik | rola |
|---|---|
| `main.c` | pętla: klasyfikacja linii, logowanie, wysyłka POS/PING, watchdog |
| `board.h` | piny i zegar (z init oryginału) |
| `cfg.h` / `cfg_store.c` | ustawienia domyślne + trwała konfiguracja w EEPROM (z sumą i limitami) |
| `uart.c` | USART1 (linia GPS+GSM, odbiór liniowy w ISR) + USART0 (debug) |
| `clock.c` | zegar 1 s (Timer1) |
| `debug.c` | log i **zbieranie surowych ramek** na USART0 |
| `gps.c` | parser NMEA RMC/GGA/VTG (arytmetyka całkowita) |
| `gsm.c` | SIM900: sekwencja AT, GPRS, TCP (maszyna stanów) |
| `can.c` | odbiór tekstowej ramki modułu CAN (heurystyka + zrzut) |
| `track.c` | **logowanie adaptacyjne** (kurs/czas/dystans/zdarzenia) |
| `proto.c` | nowy protokół ramek (`#TYP,...*XOR`) |
| `sms.c` | komendy SMS (z kontrolą długości — bez pętli restartów) |
| `roam.c` | wybór operatora wg trybu roamingu (tabela krajów: TODO) |

## Zbieranie danych do nierozwiązanych pytań

Na porcie **USART0 (38400 8N1)** leci log z sygnaturą czasu. (38400 bo dzieli się
czysto przy 8 MHz; 115200 @8 MHz ma ~8,5% błędu → krzaki.) Istotne wpisy:

- `RAW GPS <= $G...` — surowe zdania NMEA,
- `RAW LINE <= ...` — wszystkie nie-GPS linie (odpowiedzi modemu **oraz** ramki
  modułu CAN — to jest materiał do rozpracowania pola paliwa i pól `?A/?B`),
- `RAW CAN <= ...` — linie uznane heurystycznie za ramkę modułu CAN,
- `POS <= #POS,...,CAN:<surowa ramka>` — co trafia na serwer; **POS niesie
  surową ramkę CAN w całości**, więc serwer też ją zarchiwizuje.

Procedura: podłącz konwerter USB-UART do USART0, nagraj log podczas jazdy przy
podłączonym module CAN. Mając surową ramkę modułu, doprecyzujemy w `can.c`
rozbicie na pola i w końcu kalibrację paliwa (potrzebny log przy innym poziomie
baku — `docs/CAN.md`).

## Konfiguracja (`include/cfg.h`)

- `SERVER_HOST_DEFAULT` / `SERVER_PORT_DEFAULT` — serwer domyślny (bez blokady
  vendora),
- `TRK_*` — progi logowania adaptacyjnego (też w EEPROM, zmienne z serwera),
- `DBG_*` — poziom logu i zrzuty surowych ramek.

## Do zrobienia przed/po bring-upie (TODO w kodzie)

- poziomy czynne pinów PWRKEY/enable (GSM/GPS/CAN) i czasy sekwencji zasilania,
- pełna tabela roamingu (`docs/ROAMING.md`) w `roam.c`,
- bufor rekordów offline (gdy brak zasięgu) — pamięć zewn. I2C,
- dostrojenie progów logowania na realnej jeździe,
- aktualizacja serwera testowego pod nowy protokół (`#TYP,...*XOR`).
