# Serwer lokalizatora na OpenWRT

`server/tracker_server.py` jest napisany **wyłącznie na bibliotece standardowej
Pythona** — żadnego `pip`, `aiohttp`, `flask`. Dzięki temu chodzi na routerze z
OpenWRT, obsługuje połączenia urządzenia po TCP **i** serwuje interfejs WWW
(mapa + log na żywo) z tego samego procesu, na jednym porcie HTTP.

```
   SIM900 (GPRS)                    router OpenWRT                 przeglądarka (LAN)
  ────────────────┐        ┌───────────────────────────┐        ┌──────────────────┐
   urządzenie  ───┼──TCP──►│ :3506  protokół lokalizatora│        │                  │
                  │        │ :8080  WWW (mapa, CAN, log) │◄──HTTP─┤  http://router:8080│
                  │        │        jeden proces python3 │        │                  │
  ────────────────┘        └───────────────────────────┘        └──────────────────┘
```

---

## 1. Wymagane pakiety

OpenWRT dzieli Pythona na kawałki. Minimalny zestaw:

```bash
opkg update
opkg install python3-light python3-asyncio
```

- `python3-light` — rdzeń interpretera (zawiera m.in. `json`, `collections`, `zlib`).
- `python3-asyncio` — pętla zdarzeń (serwer jej używa).

Jeśli masz dużo miejsca, prościej wrzucić pełnego Pythona: `opkg install python3`.

Sprawdzenie miejsca i wersji:

```bash
df -h /overlay
python3 --version   # wystarczy 3.7+
```

> Router ma mało pamięci flash. Jeśli `/overlay` jest ciasny, zainstaluj Pythona
> na pendrive (extroot) albo trzymaj sam skrypt na USB — patrz sekcja 6.

---

## 2. Wgranie serwera

Skopiuj **jeden plik** na router (np. do `/root`):

```bash
scp server/tracker_server.py root@192.168.1.1:/root/
```

Jeśli chcesz używać OTA, skopiuj też obraz firmware:

```bash
scp build/tracker.bin root@192.168.1.1:/root/
```

---

## 3. Uruchomienie ręczne (test)

Zaloguj się na router i odpal:

```bash
python3 /root/tracker_server.py \
  --device-host example.entrydns.org \
  --device-port 3506 \
  --port 3506 \
  --web-port 8080 \
  --web-listen 0.0.0.0 \
  --log-file none
```

- `--device-host` / `--device-port` **muszą** być takie, jakie urządzenie ma
  zapisane w EEPROM (to adres, który odsyłamy w handshake HSK — jeśli się różni,
  stary firmware 2.65 zapisuje nowy i się restartuje).
- `--port 3506` — port TCP, na który łączy się urządzenie.
- `--web-port 8080` — interfejs WWW (`0` wyłącza WWW).
- `--log-file none` — bez pliku logu (oszczędza flash; domyślnie serwer tworzy
  plik `tracker_server_<data>.log` w katalogu roboczym).

Wejdź przeglądarką na **`http://192.168.1.1:8080`** — zobaczysz listę urządzeń,
mapę z trasą na żywo, odczyt CAN i surowy log.

Zatrzymanie: `Ctrl+C`.

---

## 4. Firewall — bardzo ważne

Urządzenie łączy się **z internetu** (przez GPRS), więc port lokalizatora musi
być osiągalny z WAN. Interfejs WWW trzymaj **tylko w LAN**.

### 4a. Port lokalizatora dostępny z WAN

Router musi mieć publiczny adres (lub DynDNS — patrz sekcja 7) kierujący na
`example.entrydns.org`. Jeśli serwer chodzi **na samym routerze**, wystarczy
wpuścić ruch na input WAN:

```bash
uci add firewall rule
uci set firewall.@rule[-1].name='Tracker-TCP'
uci set firewall.@rule[-1].src='wan'
uci set firewall.@rule[-1].proto='tcp'
uci set firewall.@rule[-1].dest_port='3506'
uci set firewall.@rule[-1].target='ACCEPT'
uci commit firewall
/etc/init.d/firewall restart
```

(Jeśli serwer chodzi na innym komputerze w LAN, zamiast reguły input zrób
`redirect`/port forward z WAN na ten host.)

### 4b. WWW tylko z LAN

Najpewniej: nasłuchuj WWW wyłącznie na adresie LAN routera, zamiast `0.0.0.0`:

```bash
--web-listen 192.168.1.1
```

Nie dodawaj dla portu 8080 żadnej reguły WAN. Domyślnie OpenWRT i tak odrzuca
input z WAN, więc WWW zostaje widoczne tylko od strony LAN.

---

## 5. Usługa autostartu (procd / init.d)

Żeby serwer wstawał po restarcie routera, utwórz `/etc/init.d/tracker`:

```sh
#!/bin/sh /etc/rc.common
# Serwer lokalizatora GPS (procd)
USE_PROCD=1
START=95
STOP=10

start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/python3 /root/tracker_server.py \
        --device-host example.entrydns.org \
        --device-port 3506 \
        --port 3506 \
        --web-port 8080 \
        --web-listen 192.168.1.1 \
        --log-file none
    procd_set_param respawn 3600 5 0   # restart po krótkiej śmierci
    procd_set_param stdout 1           # logi do logread
    procd_set_param stderr 1
    procd_close_instance
}
```

Nadaj prawa i włącz:

```bash
chmod +x /etc/init.d/tracker
/etc/init.d/tracker enable      # autostart po boocie
/etc/init.d/tracker start
```

Podgląd logów (serwer pisze na stdout, procd kieruje do systemowego logu):

```bash
logread -f | grep -i tracker
```

Sterowanie:

```bash
/etc/init.d/tracker stop
/etc/init.d/tracker restart
/etc/init.d/tracker disable
```

> OTA: jeśli chcesz aktualizować zdalnie, dodaj do `command` argument
> `--fw /root/tracker.bin`. Z `--fw` serwer startuje z **uzbrojonym** OTA; możesz
> go rozbroić/uzbroić przyciskiem w interfejsie WWW (patrz sekcja 8).

---

## 6. Mało miejsca na flashu? (USB / extroot)

Jeśli brak miejsca na pełnego Pythona:

1. Podłącz pendrive, zamontuj (np. `/mnt/usb`).
2. Trzymaj `tracker_server.py`, obraz `tracker.bin` i ewentualny plik logu na USB.
3. W init.d wskaż ścieżki na `/mnt/usb/...`.
4. Rozważ **extroot** (przeniesienie `/overlay` na USB) — wtedy `opkg install
   python3` zmieści się bez problemu. Instrukcja: wiki OpenWRT „Extroot".

Serwer domyślnie trzyma w RAM tylko ograniczoną historię (ostatnie
`--max-track 500` punktów trasy na urządzenie i `--max-raw 300` linii logu).
Na bardzo słabym routerze możesz je zmniejszyć, np. `--max-track 200 --max-raw 100`.

---

## 7. DynDNS (publiczny adres dla urządzenia)

Urządzenie łączy się na `example.entrydns.org`. Jeśli masz zmienny adres z WAN,
zainstaluj klienta DDNS na routerze:

```bash
opkg install ddns-scripts luci-app-ddns
```

i skonfiguruj usługę (LuCI → Services → Dynamic DNS) tak, by `example.entrydns.org`
wskazywał na adres WAN routera. To ten sam adres, który wpisujesz w `--device-host`.

---

## 8. Interfejs WWW — co pokazuje

- **Lista urządzeń** (po IMEI): status online/offline, wersja firmware, ostatni
  kontakt, prędkość. Kliknięcie wybiera urządzenie.
- **Mapa** (OpenStreetMap, Leaflet): trasa jako linia + znacznik ostatniej
  pozycji, odświeżane na żywo przez Server-Sent Events.
- **Panel szczegółów + CAN**: pozycja, czas, satelity, oraz zdekodowane pola CAN
  (prędkość, pedał, przebieg; pola `?A`/`?B` są jeszcze niepotwierdzone).
- **Surowy log**: strumień ramek na żywo (to samo co na konsoli).
- **OTA**: status („UZBROJONE / wyłączone"), nazwa obrazu i dane ostatniej
  wysyłki; przycisk **uzbrój/rozbrój**. Gdy rozbrojone, serwer odmawia `FWREQ`
  (urządzenie nie dostanie nowego firmware, nawet jeśli bootloader o niego
  poprosi). Uzbrój dopiero gdy faktycznie chcesz zaktualizować.

> Mapa wymaga internetu w **przeglądarce** (kafelki pobiera z openstreetmap.org).
> Bez internetu i tak działa lista, odczyt CAN i surowy log; każdy punkt ma też
> link do OSM.

Interfejs nie ma logowania — dlatego trzymaj go w LAN (sekcja 4b). Jeśli
potrzebujesz dostępu z zewnątrz, zrób to przez VPN do routera (WireGuard), a nie
przez otwarcie portu 8080 na WAN.

---

## 9. Najczęstsze problemy

| Objaw | Przyczyna / rozwiązanie |
|---|---|
| `python3: not found` | brak pakietu — `opkg install python3-light python3-asyncio` |
| `ModuleNotFoundError: asyncio` | brak `python3-asyncio` |
| Urządzenie łączy i od razu się rozłącza / restartuje | `--device-host`/`--device-port` inne niż w EEPROM urządzenia |
| WWW nie otwiera się z LAN | zły `--web-listen`, albo WWW wyłączone (`--web-port 0`) |
| Urządzenie nie dociera do routera | brak reguły WAN (4a), brak port-forwardu, albo DynDNS nie wskazuje na WAN |
| OTA nie startuje | brak `--fw`, albo OTA rozbrojone w WWW; sprawdź panel OTA |
| „no space left on device" przy logu | `--log-file none` albo log na USB |
