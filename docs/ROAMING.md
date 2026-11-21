# Roaming

Logika z funkcji `0x673a` (wołanej w każdym obiegu pętli, `@0x40d2`).

## Skąd firmware zna kraj

Z `AT+CENG?` (tryb inżynierski SIM900), które zwraca dane obsługującej
komórki: `+CENG: <cell>,"<arfcn>,<rxl>,<rxq>,<mcc>,<mnc>,..."`. MCC `260` =
Polska (`KRAJ`), każdy inny = `ZAGRANICA`.

## Tryb roamingu (EEPROM `0x64`)

Wartości `0..7` to ustawienie samego firmware (z serwera przez `CONFIG`,
`RoamingPam=`), a nie parametr SIM900. Domyślnie `3`.

| tryb | dozwolone kraje | wybór operatora |
|------|-----------------|-----------------|
| 0 | żadne — `ZAKAZ ROAMINGU`, restart z kodem 23 | — |
| 1 | UE/EOG/UK + Szwajcaria | rotacja, `COPS=1` po kolei, na końcu `COPS=4` |
| 2 | jak 1 + pozostała Europa; Rosja bez wymuszania operatora | j.w. |
| 3 | UE/EOG/UK | j.w. (domyślny) |
| 4 | UE/EOG/UK | zawsze od 1. operatora, `COPS=1` po kolei, na końcu `COPS=4` |
| 5 | UE/EOG/UK | tylko 1. operator: `COPS=1`, potem `COPS=4` |
| 6 | UE/EOG/UK | od razu `COPS=4` z 1. operatorem |
| 7 | UE/EOG/UK | bez `AT+COPS` — sieć wybiera modem |

Komendy według dokumentacji SIM900 (3GPP 27.007):
- `AT+COPS=1,2,"MCCMNC"` — wybór ręczny wskazanej sieci (format 2 = kod numeryczny),
- `AT+COPS=4,2,"MCCMNC"` — wybór ręczny, a gdy się nie uda, modem przechodzi na automatyczny.

„Rotacja”: numer operatora, od którego zaczyna się następna próba, jest
zapisywany w EEPROM `0x3F` i zwiększany po każdej próbie, więc kolejne próby
zaczynają się od kolejnych sieci z listy.

**Kraj niedozwolony** (brak na liście dla danego trybu): na LCD
`ZAKAZ - <MCC>-n/10-`, co 60 s sprawdzanie SMS, po 11 próbach (~11 min)
restart z kodem 25.

## Tabela krajów (`@0x68ce..0x717c`)

### UE/EOG/UK — dozwolone w trybach 1–7

29 krajów: cała UE poza Polską, Norwegia, Islandia, Wielka Brytania
(brak Liechtensteinu).

| kraj | MCC | preferowani operatorzy (MNC, w kolejności) |
|---|---|---|
| Austria | 232 | 05, 03, 01 |
| Belgia | 206 | 20, 01, 10 |
| Bułgaria | 284 | 03, 05, 01 |
| Chorwacja | 219 | 01, 02, 10 |
| Cypr | 280 | 01 |
| Czechy | 230 | 02, 01, 03 |
| Dania | 238 | 06, 01, 02 |
| Estonia | 248 | 01, 02, 03 |
| Finlandia | 244 | 14, 12, 05, 91 |
| Francja | 208 | 01, 20, 15 |
| Grecja | 202 | 01, 05, 10 |
| Hiszpania | 214 | 03, 05, 07, 01, 04 |
| Holandia | 204 | 08, 20, 16, 04 |
| Irlandia | 272 | 05, 03, 02 |
| Islandia | 274 | 11, 02, 01 |
| Litwa | 246 | 02, 01, 03 |
| Luksemburg | 270 | 99, 01, 77 |
| Malta | 278 | 21, 01 |
| Niemcy | 262 | 03, 07, 02 |
| Norwegia | 242 | 02, 05, 01 |
| Portugalia | 268 | 01, 03, 06 |
| Rumunia | 226 | 10, 01, 03 |
| Szwecja | 240 | 02, 07, 01 |
| Słowacja | 231 | 01, 00, 02 |
| Słowenia | 293 | 41, 40 |
| Wielka Brytania | 234 | 33, 10, 30, 15 |
| Węgry | 216 | 30, 01, 70 |
| Włochy | 222 | 99, 01, 10, 88 |
| Łotwa | 247 | 05, 01, 02 |

Słowacja: w oryginale kod `02` stoi w trzecim slocie, za pustym `00`, a
lista kończy się na pierwszym `00` — więc firmware próbuje tylko `01`.

### Szwajcaria — tryby 1 i 2

| kraj | MCC | preferowani operatorzy (MNC, w kolejności) |
|---|---|---|
| Szwajcaria | 228 | 07, 03, 02, 01 |

### Pozostała Europa — tylko tryb 2

| kraj | MCC | preferowani operatorzy (MNC, w kolejności) |
|---|---|---|
| Albania | 276 | 01, 04, 02 |
| Białoruś | 257 | 04, 01, 02 |
| Bośnia i Hercegowina | 218 | 03, 90, 05 |
| Macedonia Płn. | 294 | 02, 01, 03 |
| Mołdawia | 259 | 02, 01, 05 |
| Serbia | 220 | 05, 03, 01 |
| Turcja | 286 | 03, 04, 01, 02 |
| Ukraina | 255 | 06, 05, 03, 01, 07 |

Rosja (MCC 250) w trybie 2 jest dozwolona bez listy operatorów — firmware
nie wysyła `AT+COPS`.

