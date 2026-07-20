# Spec — Dashboard web locale per Air Sensor (ESP32 + SEN66)

Data: 2026-07-20
Stato: approvata dall'utente (design generale); notifiche: **nessuna** (scelta esplicita).

## Obiettivo

Eliminare completamente Blynk (limiti del piano free) e servire l'interfaccia
direttamente dall'ESP32 via web server locale. L'interfaccia implementa
fedelmente il design Claude Design **"Air Dashboard v2.dc.html"** (progetto
`e7167602-ca9e-459a-bb16-5a86cfe13c75`), più un pannello `/admin` completo per
calibrazione e manutenzione del SEN66, nello stesso stile visivo.

## Non-obiettivi (esclusioni esplicite)

- **Nessuna notifica push** di alcun tipo (PWA push impossibile su HTTP;
  servizi esterni scartati per scelta). La logica `checkAlert`/isteresi e le
  soglie di allerta vengono rimosse: i colori delle bande nella UI sono fissi,
  come nel design.
- Il backup/ripristino del learning VOC è **automatico** (nessuna azione
  manuale di salvataggio/ripristino); in `/admin` sono esposti solo un toggle
  on/off della persistenza e un reset dell'apprendimento.
- Nessun HTTPS, nessuna autenticazione (rete domestica fidata).
- Nessuno storico persistente: il buffer 24h vive in RAM e si perde al riavvio.

## Architettura

File nello sketch Arduino (stessa cartella, si compilano insieme):

| File | Contenuto |
|---|---|
| `airsensor.ino` | Logica sensore (invariata dove possibile), web server, API JSON, persistenza settaggi, storico |
| `dashboard_html.h` | Pagina `/` completa (HTML+CSS+JS) come stringa PROGMEM |
| `admin_html.h` | Pagina `/admin` completa come stringa PROGMEM |

Librerie: `WebServer.h` (core ESP32, sincrono), `ESPmDNS.h`, `Preferences.h`,
`SensirionI2cSen66`. **Rimosse**: `BlynkSimpleEsp32.h` e i `#define BLYNK_*`;
il timer Blynk è sostituito da scheduling con `millis()`.

### Endpoint HTTP

| Endpoint | Metodo | Funzione |
|---|---|---|
| `/` | GET | Dashboard |
| `/admin` | GET | Pannello manutenzione/calibrazione |
| `/api/data` | GET | Valori correnti + stato sensore + uptime misura |
| `/api/history` | GET | Le 9 serie 24h complete (48 punti ciascuna) |
| `/api/settings` | GET/POST | Lettura/scrittura settaggi persistiti |
| `/api/cmd` | POST | Azioni: `fan`, `heater`, `frc`, `reset_learning`, `co2_factory_reset`, `clear_status` |
| `/api/device` | GET | Seriale, firmware, product name, flag di stato SEN66, RSSI, heap, uptime, data ultima pulizia ventola / ciclo SHT |
| `/manifest.json` | GET | Manifest PWA (standalone, tema scuro) |

mDNS: il dispositivo risponde a `http://airsensor.local`.

## Dashboard `/`

Implementazione fedele del mockup v2 (dark `#0d1117`, card `#141b24`,
IBM Plex Sans/Mono da Google Fonts con fallback system-ui):

- Header: nome stanza (configurabile da `/admin`, default "Living room"),
  "Updated X s ago", pill di stato.
- Stati pill: `online` (READY), `warmup` (WARMUP), `calibrating`
  (GAS_CALIBRATION), `fault` (FAULT); `offline` è deciso dal client quando
  il fetch fallisce o i dati sono stantii.
- Banner guasto (visibile in FAULT) con flag errore dal device status.
- Gauge CO₂ ad arco 220°, 400–2400 ppm, bande colore e chip label come nel
  mockup (`co2Band`).
- Card PM2.5 / VOC / NOx con barre percentuali e label banda.
- Card Temperature / Humidity.
- Lista "Particulate detail": PM1, PM4, PM10.
- Ogni metrica cliccabile → bottom-sheet con grafico area SVG 24h (48 punti)
  e min/avg/max calcolati client-side dalla serie.
- Polling: `/api/data` ogni 5 s; `/api/history` all'apertura del dettaglio.
- Desktop (>700px): stesso stile, layout a due colonne (gauge a sinistra,
  card a destra); il bottom-sheet diventa modale centrato.
- PWA: manifest + apple-touch-icon inline (SVG/PNG generato), installabile
  sulla home iOS come app standalone (senza push).

## Pannello `/admin`

Stesso linguaggio visivo della dashboard. Sezioni:

1. **Calibrazione CO₂** — FRC con target ppm configurabile (default 420,
   guardia: misura attiva da ≥3 min, il firmware rifiuta altrimenti);
   ASC on/off; factory reset CO₂ (con conferma).
2. **Compensazione ambiente** — altitudine 0–3000 m; pressione ambiente
   700–1200 hPa con toggle "usa pressione invece dell'altitudine";
   offset temperatura (°C) e slope (i valori umani vengono scalati ×200 e
   ×10000 dal firmware; slot fisso 0).
3. **Tuning VOC** — i 6 parametri con range e default del datasheet
   (indexOffset 1–250 def 100, learningTimeOffset/Gain 1–1000 h def 12,
   gatingMaxDuration 0–3000 min def 180, stdInitial 10–5000 def 50,
   gainFactor 1–1000 def 230). In fondo alla sezione: toggle **"Persistenza
   apprendimento VOC"** (default on) e pulsante **"Reset apprendimento VOC"**
   (con conferma) che cancella il blob salvato, esegue `deviceReset`,
   riapplica tutti i settaggi persistiti e riavvia la misura — si perde solo
   il learning VOC/NOx, non la configurazione.
4. **Tuning NOx** — solo i parametri efficaci (indexOffset def 1,
   learningTimeOffsetHours def 12, gatingMaxDuration def 720, gainFactor
   def 230); `learningTimeGainHours=12` e `stdInitial=50` fissati dal
   firmware come richiede il datasheet.
5. **Manutenzione** — pulizia ventola, ciclo SHT heater, reset learning
   completo (deviceReset + wipe Preferences di tuning, con conferma);
   date ultima esecuzione fan/heater.
6. **Generale** — nome stanza.
7. **Device info** (sola lettura) — product name, seriale, firmware,
   flag errore (fan, RHT, gas, CO₂×2, PM, fan speed warning) con pulsante
   "clear sticky flags", RSSI, IP, heap libero, uptime.

Ogni azione mostra esito (ok/errore) restituito dal firmware.

## Dati e persistenza

### Valori correnti
`sendData()` (ogni 10 s, invariato nella sostanza) aggiorna una struct globale
`latest` con le 9 metriche + validità per-campo (warmup PM, 0x7FFF/0xFFFF,
range plausibili — regole esistenti conservate) + stato macchina.

### Storico 24h
Ring buffer: 9 metriche × 48 bucket da 30 min, float32 (~1,7 KB RAM).
Ogni bucket è la **media aritmetica** dei campioni da 10 s validi ricevuti
nel bucket (media incrementale: somma + contatore, nessun array di appoggio).
Bucket senza campioni validi = NaN, serializzato come `null` in JSON e gestito
dal grafico.

### Settaggi persistiti (`Preferences`, namespace `airsensor`)
Tutti i settaggi SEN66 sono volatili nel sensore (tranne ASC/FRC) → l'ESP32
li salva e li riapplica a ogni avvio nell'ordine: altitudine (o pressione),
offset temperatura, tuning VOC, tuning NOx, restore stato VOC, ASC,
start misura. Chiavi esistenti `lastFan`/`lastHeat` conservate.

### Stato VOC automatico
- Salvataggio: ogni 60 min in misura (`getVocAlgorithmState`, blob 8 byte)
  in Preferences — solo se la persistenza è attiva (toggle in `/admin`,
  default on; disattivandola il blob salvato viene cancellato).
- Ripristino: a ogni avvio, prima di `startContinuousMeasurement`, se la
  persistenza è attiva e il blob esiste.
- "Reset apprendimento VOC" (in `/admin`) e "reset learning" completo
  cancellano entrambi questo blob.

### Applicazione settaggi (comandi idle-only)
POST `/api/settings` → il firmware: `stopMeasurement()` → applica i comandi
idle-only → `startContinuousMeasurement()` → il warmup PM riparte (la macchina
a stati esistente lo gestisce già). I comandi any-state (pressione ambiente,
offset temperatura) vengono applicati senza fermare la misura.

## Logica conservata dal codice attuale

- Macchina a stati BOOT/WARMUP/GAS_CALIBRATION/READY/FAULT e finestre di
  warmup (30 s PM, 6 h NOx).
- Manutenzione programmata (fan settimanale, SHT heater mensile, solo con
  orologio NTP sincronizzato).
- Validazione plausibilità dei valori.
- Retry automatico del sensore in FAULT.
- NTP (`configTime`) per le date di manutenzione.

## Gestione errori

- Ogni comando I2C passa da `check()` (esistente); gli endpoint `/api/*`
  restituiscono `{"ok":false,"error":"..."}` con HTTP 500/400 in caso di
  fallimento, e la UI lo mostra.
- Il web server risponde anche in stato FAULT (la dashboard mostra il banner).
- Input POST validati contro i range del datasheet lato firmware (oltre che
  lato UI).

## Test

- Compilazione: `arduino-cli compile` (o IDE Arduino) per il target ESP32.
- Verifica UI: le pagine HTML sono testabili in browser con un mock server
  locale che serve JSON di esempio (file di sviluppo in `dev/`, non caricato
  sull'ESP32).
- UAT sul dispositivo reale: flash, verifica mDNS, dashboard, ogni azione
  admin, riavvio con riapplicazione settaggi.
