# app_6 — Comet Guard, uno sparatutto giocato via VNC

Demo del doppio buffer del framebuffer (`ca_fb_present()` a fine fotogramma) e del servizio
VNC con TLS e login, comandato dalla tastiera del viewer. Gioco originale: una nave in basso
difende lo schermo da uno sciame di droni geometrici (anelli, rombi, esagoni) che ondeggia
in formazione e, ondata dopo ondata, manda sempre più droni in picchiata, a sparare.

Board: **NUCLEO-H753ZI** con Ethernet (l'unica col CRYP e RAM per due buffer 320×240). Sulle
altre l'app si dichiara non supportata.

## Per giocare, in breve

1. Board H753 col cavo Ethernet, rete del banco su (`sudo tools/bench_net.sh up`); la board
   stampa `DHCP BOUND IP=...`.
2. `vncviewer -X509CA=examples/example_139/tls_identity_cert.pem <ip>`, utente `caos`,
   password `caos-vnc-demo`.
3. Nella finestra del viewer: **Spazio** fa partire la partita, **← →** muovono la nave,
   **Spazio** spara, **↑** alza lo scudo. Tre navi; dopo il game over, Spazio riparte.

## Comandi di gioco (dal viewer)

| Tasto | Effetto |
|---|---|
| ← → | muovono la nave |
| Spazio | spara (tre colpi in volo al massimo); dal titolo o dopo il game over, nuova partita |
| ↑ | scudo: 1,5 s di invulnerabilità, poi 6 s di ricarica |

Ondate: punti 40 (anello), 20 (rombo), 10 (esagono), doppi se il drone è colpito in
picchiata. A ogni ondata le picchiate sono più frequenti, più veloci e più numerose (fino a
5 insieme), e dalla terza spara anche la formazione. Tre navi.

## Comandi da console (VCP dell'ST-LINK, o qualunque CLI)

Si gioca anche senza viewer, per esempio da `caos_shell` sul PC:

| Comando | Effetto |
|---|---|
| `left [n]`, `right [n]` | tiene premuto il tasto per n fotogrammi (default 5 = 15 pixel, massimo 90) |
| `shield` (o `up`) | scudo |
| `fire` (o `btn1`) | sparo |
| `credit` | gettone: nuova partita dal titolo o dopo il game over |

Passano da `ca_vnc_inject_key(CA_VNC_SRC_LOCAL, ...)`, lo stesso ingresso dei tasti del
viewer: il gioco ha un solo percorso per l'input, qualunque sia la sorgente. Il rilascio lo
fa il task del gioco dopo n fotogrammi. Sono parole intere: `up` non si confonde con
`uptime`.

## Collegarsi

```
vncviewer -X509CA=examples/example_139/tls_identity_cert.pem <ip della board>
```

Utente `caos`, password `caos-vnc-demo` (l'identità e le credenziali dell'esempio 139).
L'indirizzo arriva dal DHCP del banco (`sudo tools/bench_net.sh up`) e la board lo stampa:
`DHCP BOUND IP=...`.

## CLI

| Comando | Effetto |
|---|---|
| `app6 start` | nuova partita |
| `app6 pause` | pausa / ripresa |
| `app6 reset` | torna al titolo, punteggio azzerato |
| `app6 stats` | fotogrammi al secondo, ms di lavoro per fotogramma (medio, massimo), fotogrammi in ritardo, copie A→B del doppio buffer, byte/s verso il viewer, latenza tasto→fotogramma lato board, heap libero |

## Come disegna

- **Framebuffer 320×240 RGB565, due buffer**: il gioco disegna in A, `ca_fb_present()` copia
  in B le tessere cambiate a fine fotogramma, la VNC legge B. Il viewer non vede mai un
  fotogramma a metà. `FB_COPY_MS` e `VNC_POLL_MS` a 30 ms (quelli di ex141, 50 ms,
  avrebbero limitato lo schermo a 20 fotogrammi al secondo).
- **Sfondo nero, si cancella solo dove gli oggetti erano**: ogni fotogramma cancella prima
  tutte le posizioni vecchie e poi disegna tutte le nuove. La mappa sporca contiene così
  solo quello che si muove, ed è tutto ciò che la VNC trasmette.
- **Riga di stato con LVGL**: LVGL disegna il testo (Montserrat 14) su un secondo
  framebuffer di 320×16, tutto suo (`ca_lvgl_start()`, task GUI), e non tocca mai lo
  schermo del gioco. Il gioco ricopia la riga solo quando i suoi pixel cambiano.
- **30 fotogrammi al secondo** dal task `game`: la scadenza è assoluta (t0 + n × 33 ms), il
  lavoro di ogni fotogramma è misurato col DWT; un fotogramma oltre la scadenza conta in
  `late` e il ritmo riparte da lì.
- Il ritmo che si vede sul PC dipende anche dal viewer: il server manda un aggiornamento
  quando il client lo chiede.

## Il codice

Tutto in `ca_os_app_6.c`, a sezioni:

| Sezione | Cosa fa |
|---|---|
| Screen and timing | dimensioni (320×240, riga di stato alta 16), periodo del fotogramma (33 ms), colori RGB565 |
| Sprites | la grafica: ogni sprite 16×12 è un array di stringhe, un carattere per pixel (`.` nero, lettere = colori della tavolozza `s_pal`). `sprites_init()` li converte una volta in RGB565 (`s_spr[]`), pronti per `ca_fb_blit()` |
| Drawing | `drawn_t` ricorda dove un oggetto è stato disegnato; `erase()` lo cancella col nero, `put_sprite()`/`put_rect()` lo disegnano e aggiornano la posizione |
| Game state | `game_t g`: stato della partita, punti, ondata, navi, nave (`sx`), scudo, formazione (`fx`, `fdir`), 32 droni, 3 colpi, 8 proiettili nemici |
| Status line | il testo LVGL su un framebuffer suo (sotto) |
| VNC input | `on_key()`: i tasti diventano bit in `s_keys` (tenuti premuti) e uno scatto in `s_fire_req` (lo sparo) |
| Task `game` | il ciclo dei 30 fotogrammi e le statistiche |
| Network | DHCP, ARP e task `net` presi da ex141 |
| CLI | `app6 ...` e i comandi da console |

### Il ciclo di un fotogramma (`task_game`)

```
do_cmd()       comandi della CLI (start, pause, reset)
game_step()    la logica: tasti, movimenti, collisioni, punti, ondate
hold_release() rilascia i tasti premuti da console scaduti
game_draw()    cancella tutte le posizioni vecchie, poi disegna tutte le nuove
hud_update()   ricompone il testo della riga di stato, se è cambiato lo passa a LVGL
hud_copy()     copia la riga disegnata da LVGL nello schermo, se i pixel sono cambiati
ca_fb_present()  il fotogramma finito passa al buffer letto dalla VNC
```

Poi aspetta la scadenza successiva: `next += 33 ms`, scadenza assoluta, così un
fotogramma lento non sposta tutti i successivi (quelli oltre la scadenza contano in `late`).

### Stati della partita

`GS_TITLE` (formazione che ondeggia, Spazio per giocare) → `GS_PLAY` → `GS_DYING` (nave
colpita, esplosione per 1,5 s, poi una nave in meno e 2 s di invulnerabilità) → di nuovo
`GS_PLAY`, oppure `GS_OVER` all'ultima nave. Formazione distrutta: `GS_WAVE` per 2 s, poi
l'ondata successiva.

### I droni

Ogni drone è in uno di cinque stati: `EN_SLOT` (al suo posto nella formazione, che
ondeggia di un pixel ogni due fotogrammi), `EN_DIVE` (in picchiata), `EN_RETURN` (rientra
dall'alto verso il suo posto), `EN_BOOM` (esplode), `EN_DEAD`.

La picchiata (`start_dive()`) punta verso dove si trova la nave in quel momento, con una
velocità verticale costante e un'oscillazione laterale presa da una tabella di seni intera
(`s_sin32`): niente virgola mobile, il task è un task normale (`CA_JOB_STD`). Le posizioni
in picchiata sono in virgola fissa Q4 (sedicesimi di pixel). Un drone che esce dal fondo
rientra dall'alto e torna al suo posto.

### La difficoltà

Quattro funzioni dell'ondata `g.wave`, tutte con un tetto:

| Funzione | Ondata 1 | Cresce fino a |
|---|---|---|
| `dive_every()` fotogrammi fra due picchiate | 80 | 25 |
| `max_divers()` droni in picchiata insieme | 1 | 5 |
| `dive_speed()` velocità della picchiata (Q4 per fotogramma) | 32 | 64 |
| `fire_chance()` probabilità di sparo, su 256 | 4 | 20 |

Dalla terza ondata spara anche la formazione. Per cambiare il carattere del gioco basta
toccare questi numeri.

### Come arrivano i tasti

Un solo percorso, qualunque sia la sorgente: il servizio VNC chiama `on_key()` per i tasti
del viewer; i comandi da console (`left`, `right`, `shield`, `fire`) chiamano
`ca_vnc_inject_key(CA_VNC_SRC_LOCAL, ...)`, che arriva alla stessa `on_key()`. Il gioco legge
`s_keys` una volta per fotogramma; lo sparo è uno scatto (`s_fire_req`), così ogni
pressione di Spazio è un colpo anche se arriva fra due fotogrammi.

## Misure (H753, 20/11/2026, `app6 stats`)

| | |
|---|---|
| fotogrammi al secondo | 30, nessuno in ritardo |
| lavoro per fotogramma | 1,5 ms medio, 5,4 ms massimo, su 33 |
| doppio buffer | 2 buffer, una copia A→B per fotogramma |
| traffico verso il viewer (TLS, 32 bpp) | ~1,0–1,3 MB/s, 15–19 aggiornamenti/s a un client che chiede il successivo solo dopo aver ricevuto il precedente |
| latenza tasto → fotogramma, lato board | 15 ms medi, 31 massimi |
| heap libero | 59 KB (su 463: due buffer da 150 KB, riga di stato, LVGL, VNC con TLS) |

## Configurazione

`os_config_app_6.h`, creata con `caos_config.py --new 6 --from ex141 -wp`, poi
`--set FB_COPY_MS=30U --set VNC_POLL_MS=30U`. Compilazione:
`./build_caos.sh H753 -wp --app 6`.
