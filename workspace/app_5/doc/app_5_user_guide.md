# app_5 — il trampolino XSPI2 della NUCLEO-H7S3L8: guida utente

La STM32H7S3 ha solo **64 KB di flash interna**: nessuna immagine caOS ci sta. Le immagini
girano dalla flash Octo-SPI esterna **MX25UW25645G** (32 MB) collegata a XSPI2, in
esecuzione diretta (XIP) all'indirizzo `0x70000000`.

`app_5` è il **trampolino**: il piccolo programma che sta nella flash interna, mappa la
XSPI2 e salta all'immagine. Ha anche un **monitor seriale** per la messa in servizio e il
recupero. È autonomo: niente librerie caOS, niente kernel, solo registri.

| | |
|---|---|
| Board | NUCLEO-H7S3L8 (MB1737), `MCU_ID 773` |
| Sorgente | `workspace/app_5/trampoline_h773.c`, linker `trampoline_h773.ld` |
| Dove sta | flash interna, `0x08000000`, settore 0 (circa 7,6 KB su 8) |
| Versione | 1.3 |

Su tutte le altre board `app_5` non fa nulla: stampa che è solo per la H773.

---

## 1. Mappa della memoria

| Area | Indirizzo | Contenuto |
|---|---|---|
| flash interna | `0x08000000` | trampolino (settore 0, 8 KB per settore) |
| XSPI2 (XIP) | `0x70000000` | immagine caOS: tabella dei vettori in testa |
| DTCM | `0x20000000` | stack dei task e stack MSP dell'immagine |
| AXI SRAM | `0x24000000` | dati, heap, pool USB e rete |

Il driver flash di caOS **rifiuta il settore 0**: un `flash erase` o `flash test` lanciato
da un'immagine non può cancellare il trampolino. Gli altri sette settori della flash
interna sono liberi per i dati.

---

## 2. Cosa fa all'accensione

1. Parte sull'HSI a 64 MHz, senza PLL: l'immagine trova i clock come dopo un reset.
2. Scrive in **un solo colpo** `PWR_CSR2` con alimentazione LDO ed `EN_XSPIM2`. Il byte
   basso di quel registro si può scrivere **una sola volta** dopo il power-on, e il suo
   valore di reset è una combinazione illegale (RM0477, Table 44): una scrittura parziale
   lo bloccherebbe, e l'immagine non potrebbe più alzare la tensione del core.
3. Configura XSPIM in modo diretto (porta N, AF9); il kernel clock di XSPI2 è hclk5.
4. Resetta la flash in tutti e tre i modi (8D, 8S, 1S), perché un reset della sola MCU
   può lasciarla in modo ottale; poi legge il JEDEC ID, che deve essere `C2 81 39`.
5. Mappa la flash in memoria a linea singola, poi prova l'**ottale DTR** (8D-8D-8D con
   DQS). Lo tiene solo se le prime 64 parole lette in DTR sono uguali a quelle lette in
   SPI; altrimenti resta in SPI.
6. Controlla la tabella dei vettori dell'immagine (vedi §4).
7. Se B2 non è premuto, rimette la console in stato di reset e salta all'immagine.

Uscita tipica sulla console (ST-LINK VCP, 115200 baud):

```
caOS trampoline H773 1.3 (XSPI2 -> 0x70000000)
XSPI2 flash ID 0x3981C2 (MX25UW25645G)
XSPI2: HSLV, octal DTR checked at 64 MHz, run clock hclk5/4
image SP=0x2000FF00 PC=0x70010125
-> jump
```

L'immagine **non riprogramma mai XSPI2**: velocità e modo restano quelli scelti qui.

---

## 3. Velocità della flash di codice

| Option byte `XSPI2_HSLV` | Modo | Clock durante l'esecuzione |
|---|---|---|
| 1 (impostato su questa board) | ottale DTR, controllato a 64 MHz | hclk5 / 4 = **75 MHz** |
| 0 (valore di fabbrica) | ottale DTR | hclk5 / 8 = 37,5 MHz |

hclk5 è il bus AXI/AHB: 300 MHz con la CPU a 600 MHz.

`XSPI2_HSLV` attiva gli I/O ad alta velocità a bassa tensione della porta 2. È ammesso
perché su questa board VDDXSPI2 è a **1,8 V** (SB34, schema MB1737 p.4). È un'impostazione
persistente della board, reversibile con STM32CubeProgrammer (`-ob XSPI2_HSLV=0`).
**Mai `XSPI1_HSLV`**: la porta 1 è alimentata a 3,3 V, e l'HSLV con 3,3 V danneggia gli I/O.

### 3.1 Misure oltre i 75 MHz

Misurate il 02/11/2026 su questa board, a temperatura ambiente, con il comando
`e92 xspeed` di ex92. La prova solo legge la flash (non scrive mai), gira da RAM a
interrupt spenti e confronta il CRC di 512 KB dell'immagine letti tre volte:

| Divisore | Clock | Con i clock già alti | Come default del trampolino |
|---|---|---|---|
| /4 | 75 MHz | riferimento | ✅ si avvia |
| /3 | 100 MHz | ✅ 6 giri su 6 | ❌ l'immagine si blocca subito dopo il salto |
| /2 | 150 MHz | ✅ 6 giri su 6 | ❌ come sopra |
| /1 | 300 MHz | mai provato: la MX25UW25645G si ferma a 200 MHz in DTR | — |

Il motivo: l'immagine passa SYSCLK al PLL **eseguendo dalla stessa XSPI2**, e in quel
momento il kernel clock di XSPI2 sale di colpo da 32 a 300 MHz mentre il controller sta
leggendo istruzioni. Con /4 il salto va da 8 a 75 MHz e regge; verso 100 o 150 MHz no.

Per andare oltre i 75 MHz bisognerebbe fare quel cambio di clock da RAM con XSPI2 ferma,
e poi rifare la verifica. Oggi non serve: si resta a 75 MHz.

---

## 4. Quando si ferma nel monitor

Il trampolino entra nel monitor invece di saltare all'immagine quando:

| Caso | Messaggio | LED rosso LD3 |
|---|---|---|
| la flash non risponde (ID diverso da `C2 81 39`) | `flash does not answer` | lampeggia |
| la mappatura in memoria non riesce | `XSPI2: memory-mapped mode failed` | lampeggia |
| non c'è un'immagine valida | `image: <motivo>` | lampeggia |
| **B2 tenuto premuto al reset** | `B2 held: monitor` | spento |

Controlli sull'immagine (i primi due word a `0x70000000`):

- `erased (no image)`: entrambi `0xFFFFFFFF`;
- `stack pointer not in DTCM/AXI SRAM`;
- `reset vector without the Thumb bit`;
- `reset vector outside XSPI2`.

### 4.1 B2 e il tamper

B2 è su **PC13**, che è anche l'ingresso **TAMP_IN1**. La board non ha pull-up esterno su
B2: il trampolino usa il pull-up interno, premuto = 0.

Se un'immagine ha lasciato **armato il tamper 1** (resta armato anche dopo un reset di
sistema), PC13 è comandato dal TAMP qualunque sia la configurazione GPIO, e legge basso:
con la versione 1.2 ogni reset si fermava nel monitor come se B2 fosse premuto.
Dalla versione **1.3**, se `TAMP_CR1.TAMP1E` è attivo B2 viene ignorato:

```
B2 ignored: PC13 is armed as TAMP_IN1
-> jump
```

Il trampolino non disarma il tamper: decide l'immagine. Un'immagine non valida porta
comunque al monitor.

---

## 5. Il monitor

Sulla console ST-LINK VCP (USART3, 115200 8N1), con la stessa sintassi della CLI caOS:

| Comando | Cosa fa |
|---|---|
| `mem AAAAAAAA [VVVVVVVV]` | legge, o scrive e rilegge, una parola a 32 bit |
| `hexdump AAAAAAAA [N]` | N righe da 16 byte (predefinito 4, massimo 32) |
| `id` | JEDEC ID della flash XSPI2 |
| `go [AAAAAAAA]` | salta all'immagine (predefinito `0x70000000`) |
| `reset` | reset di sistema |
| `help` | l'elenco dei comandi |

Un accesso a un indirizzo non valido con `mem` o `hexdump` stampa CFSR e BFAR del fault e
torna al prompt: il monitor non si blocca.

Esempio: guardare la testa dell'immagine e poi avviarla.

```
> hexdump 70000000 2
> go
```

Per leggere i registri del dominio di backup (TAMP, backup register) serve prima
`PWR_CR1.DBP = 1` e il clock `RCC_APB4ENR.RTCAPBEN`: senza DBP quei registri si leggono 0.
Il trampolino 1.3 li accende già quando controlla B2; altrimenti va acceso prima il bit 16
di `RCC_APB4ENR` (`0x58024554`), rileggendo il registro e senza toccare gli altri bit.
Esempio, disarmare il tamper 1 a mano:

```
> mem 58024800 00000101     (PWR_CR1: DBP)
> mem 58004400 00000000     (TAMP_CR1: TAMP1E = 0)
> reset
```

---

## 6. Compilare e programmare

Il trampolino si compila con il workspace, sulla H773:

```bash
./build_caos.sh H773 -wp --app 5          # -> elfs/H773/h773_app_5.elf
```

È compilato da `workspace/h7rsxx/h773/CMakeLists.txt` da solo, con il suo linker script e
senza le librerie caOS.

Programmazione con la ST-LINK della board:

```bash
CAOS_MCU=H773 ./prog.sh -s -a 5           # trampolino nella flash interna
CAOS_MCU=H773 ./prog.sh -s -e 92          # un esempio nella flash XSPI2
```

`prog.sh` guarda l'entry point dell'ELF: se inizia con `0x7`, aggiunge da solo l'external
loader di ST per questa board (`MX25UW25645G_NUCLEO-H7S3L8.stldr`, nella cartella di
STM32CubeProgrammer). Il trampolino invece va nella flash interna senza loader.

Il trampolino si riprogramma raramente: le immagini si aggiornano senza toccarlo.

---

## 7. Recupero

| Situazione | Cosa fare |
|---|---|
| l'immagine non parte, il trampolino stampa `image: ...` | riprogrammare l'immagine con `prog.sh -e` |
| l'immagine si blocca dopo `-> jump` | tenere premuto B2 e premere reset: si entra nel monitor; poi riprogrammare l'immagine |
| ogni reset si ferma nel monitor senza B2 premuto | tamper armato con un trampolino 1.2: aggiornare il trampolino, o disarmare il tamper dal monitor (§5) |
| il trampolino stesso è danneggiato | la ST-LINK lo riprogramma sempre: `prog.sh -s -a 5` |

Un trampolino o un'immagine sbagliati **non rendono la board irrecuperabile**: la flash
interna e quella XSPI2 si riprogrammano dalla ST-LINK in ogni caso.

---

## 8. Storia delle versioni

| Versione | Data | Cosa cambia |
|---|---|---|
| 1.0 | 01/11/2026 | prima versione: XSPI2 in SPI a linea singola, monitor |
| 1.1 | 01/11/2026 | ottale DTR con DQS, tenuto solo se verificato |
| 1.2 | 01/11/2026 | HSLV e cella di compensazione: 75 MHz durante l'esecuzione |
| 1.3 | 02/11/2026 | B2 ignorato quando PC13 è armato come TAMP_IN1 |
