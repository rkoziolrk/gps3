# Układ EEPROM (zweryfikowany ze zrzutem i z kodem)

Źródło: `reverse/eeprom.bin` — zrzut 2 KiB wewnętrznego EEPROM ATmega64.
Znaczenie każdego pola ustalone na podstawie funkcji, które je czytają i
zapisują (klaster `0x1532..0x1f66` w obrazie flash).

## Dostęp

| adres   | funkcja                                                        |
|---------|----------------------------------------------------------------|
| 0xb80c  | czekaj na EEWE, ustaw EEAR                                     |
| 0xb81e  | zapis `r25` bajtów (tylko gdy `r23 == 0x7B` — blokada zapisu)  |
| 0xb840  | odczyt `r25` bajtów                                            |
| 0x1f66  | odczyt napisu: bajt po bajcie, aż do `0xFF`                    |
| 0x2036  | zapis napisu: `strlen+1` bajtów (razem z NUL)                  |
| 0xb498  | XOR wszystkich bajtów napisu (suma kontrolna)                  |

## Pola

| offset | zrzut                        | znaczenie                                   | zapis / odczyt |
|--------|------------------------------|---------------------------------------------|----------------|
| 0x01   | `3506`                       | port TCP serwera                            | 0x1532 / 0x15e0 |
| 0x07   | `0x00`                       | XOR(port) — suma kontrolna                  | j.w.           |
| 0x0A   | `lgps03.monitoring-gps.net`  | host serwera                                | 0x1698 / 0x1746 |
| 0x2F   | `0x31` (`'1'`)               | XOR(host) — suma kontrolna                  | j.w.           |
| 0x3C   | `R` (+`n<`)                  | ostatni stan ruchu: `R` ruch / `P` postój   | 0x17fe / 0x186c |
| 0x3F   | pusto → `1`                  | indeks preferowanego operatora roamingu     | 0x193a / 0x198c |
| 0x42   | `021`                        | kod przyczyny ostatniego restartu (sieć 260)| 0x19ee / 0x1aaa |
| 0x5A   | `000001`                     | licznik restartów bez udanej łączności      | 0x1b72 / 0x1c5e |
| 0x64   | `3`                          | tryb roamingu `0..7` („RoamingPam")         | 0x1dac / 0x1dfe |
| 0x67   | `020`                        | kod przyczyny ostatniego restartu (sieć ≠260)| 0x19ee / 0x1aaa |

Sumy kontrolne potwierdzone na zrzucie: XOR(`3506`) = `0x00` = bajt `0x07`,
XOR(`lgps03.monitoring-gps.net`) = `0x31` = bajt `0x2F`.

## Szczegóły

**Port i host (0x01, 0x0A).** Przy odczycie: pusty napis albo niezgodna suma
XOR → wartość domyślna z flasha (`3506`, `lgps01.monitoring-gps.net`) i zapis
z powrotem. Urządzenie ze zrzutu łączy się z `lgps03.monitoring-gps.net:3506`.
Osobna funkcja `@0x7c80` dodatkowo sprawdza długość portu (= 4) i wartość
(`atoi ≠ 0`), wypisując `PORT MEM LEN ERROR` / `PORT MEM VAL ERROR`;
prawdopodobnie waliduje nowy adres (np. z SMS `NEW*ADR=`).

**Stan ruchu (0x3C).** Zapisywany przy przejściu w postój (`P`) i przed każdym
restartem (bieżący stan z RAM `0x0266`), domyślnie `R`. Kopia trafia też do
zewnętrznej pamięci I2C (bank `0x7E`, adres `0x08`). Firmware zapisuje tu
tylko `R` albo `P` — bajtów `n<` za `R` w zrzucie nie wyjaśnia żaden znaleziony
zapis (prawdopodobnie pozostałość).

**Indeks operatora (0x3F).** Firmware ma tabelę preferowanych sieci dla krajów
(MCC → lista MNC, `@0x6f00..0x7180`: m.in. 262, 278, 242, 268, 226, 231, 293,
240, 216, 234, 222) i wybiera operatora przez `AT+COPS=1,2,<MCCMNC>`; przy
braku `OK` — `AT+COPS=4,2,...`. Indeks rotuje po porażkach, po 4 próbach
wraca do 1. Pusto w zrzucie (bajt `0x3F` to NUL kończący pole `0x3C`) →
używane `1`.

**Kody przyczyny restartu (0x42 / 0x67).** Procedura restartu (`0x2a28` →
`0x7e00`) zapisuje kod pod `0x42`, gdy MCC bieżącej sieci = `260`, w przeciwnym
razie (za granicą albo sieć jeszcze nieznana) pod `0x67`; potem
`RESET ZA 7 sek.` i reset watchdogiem. Odpowiedź `OK` serwera zeruje kod.
Oba kody są pokazywane na ekranie startowym (`er=021/020`) i wysyłane w ramce
`FORM` po `Er=`. Wybrane kody z obrazu:

| kod | miejsce | sytuacja                         |
|-----|---------|----------------------------------|
| 10  | 0x2e04  | `AT+CGMR`                        |
| 12–13 | 0x33c4 / 0x341a | problem z HSK         |
| 14  | 0x38ba  | błąd `CONFIG`                    |
| 16  | 0x4036  | 7 nieudanych prób                |
| 20  | 0x5cca  | modem nie odpowiada (`AT+CSQ`)   |
| 21  | 0x61d4  | błąd `AT+CIICR` (GPRS)           |
| 22  | 0x6334  | błąd `AT+CIPHEAD=1`              |
| 23  | 0x671e  | zakaz roamingu                   |
| 27  | 0x864e  | komenda `RESET` z SMS            |
| 29  | 0x3072  | `-HSK-ERROR`                     |
| 36  | 0x5e74  | IMEI / PIN                       |

W zrzucie: `021` (ostatni restart w Polsce — błąd `AT+CIICR`), `020` (poza
siecią 260 — modem nie odpowiadał).

**Licznik restartów (0x5A).** RAM `0x0B6B`, zwiększany przy każdym restarcie
z kodem, zapisywany jako 6 cyfr, zerowany po odpowiedzi `OK`. Powyżej 15
urządzenie wydłuża oczekiwanie (`NET PROBLEM`, czas rośnie z licznikiem,
ograniczenie przy 50) i w tym czasie czyta SMS. Kopia także w pamięci I2C.
`000001` = jeden restart od ostatniej udanej łączności.

**Tryb roamingu (0x64).** Jedna cyfra `0..7`, inaczej domyślne `3`. Ustawiany
z odpowiedzi `CONFIG` (`RoamingPam=<n>`, `@0x37e4`), raportowany w ramce
`FORM` po `R=3/`. Za granicą (MCC ≠ 260) z trybem `0` → `ZAKAZ ROAMINGU` i
restart z kodem 23. Znaczenie wartości 1–7 nie zostało ustalone.

## Zewnętrzna pamięć I2C

Oprócz wewnętrznego EEPROM jest pamięć zewnętrzna na **programowym I2C**:
PD0 = SCL, PD1 = SDA, open-drain przez DDRD (`0xb9c6` START, `0xb9e4` STOP,
`0xb9f6` zapis bajtu, `0xba30` odczyt). Funkcje `0x14c4` / `0x14fa` to zapis
i odczyt w stylu 24Cxx (adres urządzenia + 2 bajty adresu), wyżej `0xe42` /
`0xfa6` (bank + adres; komunikaty `ZAPIS DO PAMIECI`, `ODCZYT Z PAMIECI`,
`Bank=`, ` Adr=`). Zrzut tej pamięci nie jest dostępny.

## Bajty niezidentyfikowane

Firmware ustawia adres EEPROM tylko w jednym miejscu (`0xb80c`), wołanym przez
procedury odczytu i zapisu, a te mają 6 wywołań sięgających najdalej `0x6A`.
Poniższe dane **nie są czytane ani zapisywane przez ten firmware** —
najpewniej pozostały po innym programie lub wcześniejszej wersji.

| adres          | zawartość                 | uwagi                               |
|----------------|---------------------------|-------------------------------------|
| 0x3D–0x3E      | `6E 3C` (`n<`)            | za `R` w polu stanu ruchu           |
| 0x3F           | `00`                      | NUL za `Rn<`; pole indeksu operatora puste → używane `1` |
| 0x66           | `00`                      | między polami `0x64` i `0x67`       |
| 0x202          | `02`                      | poza obszarem konfiguracji          |
| 0x500–0x53D    | 62 × `14`                 | j.w.                                |
| 0x666–0x672    | 13 × `14`                 | j.w.                                |
| 0x673          | `FF`                      | przerwa w bloku                     |
| 0x674–0x693    | 32 × `14`                 | j.w.                                |
| 0x766          | `14`                      | j.w.                                |

Bajt `0x66` może być NUL-em po wcześniejszym, dwuznakowym zapisie pola `0x64`,
które firmware potem zastąpił wartością domyślną (np. po błędnej wartości z
`CONFIG`) — to przypuszczenie, nie zostało potwierdzone.
