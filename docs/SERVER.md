# Serwer — co musi robić

Wymagania odtworzone z firmware. Gotowa implementacja: `server/tracker_server.py`.

## Blokada adresu w firmware

Oryginalny firmware przy każdym starcie (`@0x7b4c`, przed połączeniem)
zastępuje host spoza `lgpsNN.monitoring-gps.net` (dokładnie 25 znaków) oraz
port spoza `35xx` adresem fabrycznym `lgps01.monitoring-gps.net:3506`.
Własnego serwera nie da się ustawić samym SMS-em ani zapisem EEPROM — trzeba
zmienić firmware. Robi to `tools/patch_firmware.py`:

- domyślny host pod `0xbbe0` (jedyna kopia, najwyżej 25 znaków),
- `0x7ba8`: `brne .+4` → `rjmp 0x7cb6` — host nie jest porównywany z domeną
  producenta,
- `0x7d62`: `brne .+4` → `rjmp .+4` — port nie musi zaczynać się od `35`
  (kontrola długości = 4 cyfry zostaje, bo zakłada ją m.in. odpowiedź HSK).

Skrypt sprawdza przed zmianą, czy w pliku są oryginalne bajty. Firmware nie
liczy sumy kontrolnej flasha (jedyne `lpm` są w procedurach bibliotecznych).

```bash
# tworzy flash_patched.bin i flash_patched.hex obok pliku wejściowego:
python3 tools/patch_firmware.py flash.bin --host example.entrydns.org
```

Po wgraniu: jeśli EEPROM został skasowany razem z flashem, urządzenie użyje
nowego adresu domyślnego. Jeśli EEPROM został zachowany (bit EESAVE), jest tam
wciąż adres producenta — wtedy wyślij SMS `NEW*ADR=<host>$<port>$`, który po
zdjęciu blokady nie jest już cofany.

## Transport

- Jedna sesja TCP na raz, kilka ramek w jednej sesji (najpierw `HSK-02`,
  potem `CONFIG`, potem dane). Nie zamykaj połączenia po pierwszej ramce.
- Każda ramka od urządzenia kończy się `\r\n` (@0x3f8e dokleja CR, LF i
  Ctrl-Z; Ctrl-Z modem zjada). Strumień TCP trzeba dzielić na linie — jeden
  `recv()` może zawierać część ramki albo kilka ramek.
- **Odpowiedź musi być w klamrach: `{...}`.** Firmware (przerwanie USART1,
  tryb `0x1F`) ignoruje wszystko przed `{`, zapisuje znaki do `}` (najwyżej
  198) i na tym kończy. Bez `}` czeka 30 s (150 × 200 ms) i przyjmuje `ER`.

## Ramki i odpowiedzi

| ramka od urządzenia | odpowiedź serwera | gdy odpowiedź zła / brak |
|---|---|---|
| `HSK-02;<IMEI>;2.65;ORANGE;...` | `{HSK=<port>;<host>*<xor>}` (38 znaków w klamrach) | `-HSK-ERROR` / `HSK PROBLEM`, restart |
| `CONFIG;<IMEI>;05` | 81 znaków w klamrach, suma XOR na poz. 79–81 | `-CONFIG-ERROR`, restart (kod 14) |
| `PNG-01;<IMEI>;CAN;<n>;FORM;Er=...` | `{OK}` | kody restartu nie są zerowane |
| `LOK-02;<IMEI>$<rekord>$...` | `{OK}` | rekordy zostają; brak odpowiedzi → restart (kod 9) |
| `LOGUJ-;<IMEI>;<tekst>@`, `AUTO--;...@` | dowolna (`{OK}`) | firmware nie sprawdza |

### Odpowiedź na HSK

```
poz.  1-3   HSK
poz.  4     '=' (w oryginale; firmware nie sprawdza)
poz.  5-8   port (4 cyfry)
poz.  9     ';' (w oryginale; firmware nie sprawdza)
poz. 10-34  host; krótszy niż 25 znaków kończy się '$' (reszta dowolna)
poz. 35     *
poz. 36-38  XOR znaków 1-34, 3 cyfry
```

Jeśli host i port są **takie same jak w EEPROM** urządzenia → `- ADRESY OK -`
i praca dalej. Jeśli **inne** → urządzenie zapisuje nowy adres
(`- NOWE ADRESY -`, `Zapisano w pamieci`) i się restartuje — tak serwer może
przekierować urządzenie. Host dłuższy niż 25 znaków nie mieści się w odpowiedzi.

### Odpowiedź na CONFIG

Mapa pól: `docs/PROTOCOL.md`. Znane znaczenia:

| poz. | szer. | znaczenie | domyślnie w serwerze |
|---|---|---|---|
| 6 | 5 | czas do następnej sesji [s] (najpewniej) | `00060` |
| 12 | 5 | czas do następnego pobrania CONFIG [s] (najpewniej) | `03600` |
| 18 | 1 | liczba rekordów w ramce `LOK` (`0` → 6) | `2` |
| 25 | 1 | tryb roamingu 0–7 | `3` |
| 32 | 4 | pole tekstowe (`0000` = puste) | `0000` |

Pozostałe pola mają nieustalone znaczenie; serwer wysyła wartości, które
firmware ustawia sam przy starcie (`22`=06, `37`=018, `41`=09, `49`=45,
`59`=4), a resztę jako zera. Pozycje 6 i 12 to minutniki zmniejszane co
sekundę przez zegar (przerwanie TIMER1_OVF).

## Ramka LOK

`LOK-02;<IMEI>$`, potem do `n` rekordów (n z poz. 18 CONFIG), każdy
zakończony `$`. Rekord składa funkcja `0x210e`, sklejając pola **bez
separatorów**, więc początek ma stałą szerokość. Układ **prawdopodobny** —
szerokości z wartości wpisywanych przy błędach parsera NMEA (`@0x4b20`),
znaczenia z etykiet LCD (`UTC-`, `ALT-`, `LON-`, `LAT-`, `SAT-`, `SPEED-`):

| pole | szer. | przykład | źródło | pewność |
|---|---|---|---|---|
| długość geogr. `dddmm.mmmm` bez kropki | 9 | `021007000` | GGA | wysoka |
| `E` / `W` | 1 | `E` | GGA | wysoka |
| szerokość geogr. `ddmm.mmmm` bez kropki | 8 | `52138000` | GGA | wysoka |
| `N` / `S` | 1 | `N` | GGA | wysoka |
| wysokość [m] | 4 | `0110` | GGA | średnia |
| liczba satelitów | 2 | `07` | GGA | średnia |
| prędkość [km/h] | 3 | `048` | VTG | średnia |
| czas UTC `hhmmss` | 6 | `120100` | GGA | wysoka |
| data `ddmmrr` | 6 | `010126` | RMC | średnia |
| licznik (ruch/postój), 2 liczby, `!<pole>!`, 3 wartości CAN | — | — | — | nieznane |

Serwer dekoduje początek rekordu (`decode_record()`), a całość zawsze zapisuje
też w postaci surowej. Układ trzeba potwierdzić na prawdziwej ramce.

## Test

`server/test_client.py` udaje urządzenie (zmyślony IMEI i trasa) i sprawdza
odpowiedzi regułami firmware, a potem robi test odwrotny — czy złe odpowiedzi
są odrzucane:

```bash
python3 tracker_server.py --port 3506 --device-host example.entrydns.org
python3 test_client.py --port 3506 --device-host example.entrydns.org
```

Serwer zapisuje w logu **każdy odebrany kawałek danych od razu**
(`odebrano N B: ...`), zanim złoży z niego ramkę — więc widać także dane w złym
formacie, obcy ruch (np. skanery portów) i połączenia, w których nic nie
przyszło. Dane bez `\r\n` leżące w buforze dłużej niż `--partial-warn` sekund
(domyślnie 3) dostają ostrzeżenie „niepełna ramka”, a przy rozłączeniu serwer
podaje liczbę odebranych bajtów i ramek. Znaki sterujące z odebranych danych są
pokazywane jako `\xNN`, żeby nie sterowały terminalem.

Oba skrypty kolorują wyjście w terminalu (typy ramek, rozkodowane pozycje,
wynik sprawdzeń). Kolory wyłącza `--no-color` albo zmienna `NO_COLOR`; przy
przekierowaniu do pliku lub potoku wyłączają się same, a plik logu serwera
jest zawsze bez kodów kolorów.

Uwaga: suma XOR jest słaba — dwa takie same przekłamania bitu się znoszą
i nie zależy od kolejności znaków (test pokazuje to, zamieniając miejscami
separatory `=` i `;`, których firmware i tak nie sprawdza).

## Odpowiedź oryginalnego serwera

Log z urządzenia połączonego z oryginalnym serwerem pokazał odpowiedź
`{HSK=;*086}` — ten sam układ (`HSK=<port>;<host>*<suma>`), ale z pustym portem
i hostem: serwer nie ma adresu dla tego urządzenia. Firmware odrzuca ją
(długość 9 zamiast 38) i restartuje się.
