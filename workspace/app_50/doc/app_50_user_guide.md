# app_50 — guida utente

Guida all'uso del sensore di luce ambientale **VD6283TX** sotto caOS, e
all'applicazione `app_50` che lo pilota.

**Il documento è in due parti, con destinatari diversi.**

Il **capitolo 1 riguarda il sensore**, non questa applicazione: mappa dei
registri, sequenza di accensione, significato dei canali, trappole del
datasheet. Vale per qualunque impiego del VD6283TX — soglia di luce, misura
fotometrica, colorimetria, rilevazione di sfarfallio — ed è la parte da leggere
prima di scrivere qualsiasi altro codice per questa parte. Nessuna delle scelte
descritte lì dipende da cosa si vuole misurare.

Il **capitolo 2 riguarda `app_50`**, che è un banco di lettura a due sensori, uno
per bus I2C, usato per esercitare il driver e confrontare due esemplari sotto la
stessa luce. È un'applicazione di riferimento e di collaudo, non un prodotto.

Se il dark vada sottratto, mediato, ignorato o inseguito è una decisione
**dell'applicazione**, e questo documento non la prende: si limita a dire che
cosa la parte misura davvero, e quali operazioni su quei numeri hanno una base
fisica e quali no.

---

## 1. Il sensore VD6283TX

### 1.1 Che cos'è

Sensore di luce ambientale multispettrale di STMicroelectronics, in package
ottico da 1,83 × 1,0 × 0,55 mm. Interfaccia I2C, ADC a 24 bit, sei canali di
colore letti **in parallelo** — non in sequenza, quindi i sei valori di un
campione appartengono tutti allo stesso istante.

Documentazione di riferimento nel repository:

| documento | dove | alias `lookup_ref.py` |
|---|---|---|
| Datasheet DS13735 rev 3 | `doc/reference_manuals/vd6283tx.pdf` | `vd6283` |
| Manuale API STALS (UM2878) | `doc/reference_manuals/um2878-...pdf` | `stals`, `um-vd6283` |
| Breakout SATEL | `doc/reference_manuals/vd6283tx_satel-2400566.pdf` | — |

⚠️ Il datasheet **non documenta tutti i registri**. Il §4.1 dice esplicitamente
che vanno usati tramite il driver ST. Diversi registri essenziali (`SEL_PD`,
OTP, i clamp) esistono solo nel sorgente del driver, ed è da lì che questa
applicazione li ha ripresi.

### 1.2 La matrice di fotodiodi

Il cuore del sensore è una matrice **5 × 5 = 25 fotodiodi** (§8.1, Figura 17,
pagina 41 del datasheet), ciascuno con il proprio filtro ottico:

![Matrice 5x5 dei fotodiodi](diagrams/photodiode_matrix.dot)

Legenda: `R` rosso, `G` verde, `B` blu, `IR` infrarosso, `VIS` visibile
(filtrato IR), `C` clear (nessun filtro), `D` **dark** (coperto).

⚠️ **Nella figura del datasheet le due `D` non si vedono**: sono stampate nere
su fondo nero. La legenda le elenca, i quadrati appaiono vuoti. Questo ha
portato a supporre che il canale dark non esistesse su questa parte — è invece
raggiungibile, come descritto in §1.5.

Conteggio dei diodi per colore, che tornerà utile:

| colore | C | G | IR | VIS | R | B | D |
|---|---|---|---|---|---|---|---|
| numero | 5 | 5 | 4 | 3 | 3 | 3 | 2 |

Totale 25. ✅

### 1.3 I sei canali

L'assegnazione canale → colore è fissa nel silicio (Tabella 2 del datasheet):

| canale | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| colore | RED | **VIS** | BLUE | GREEN | IR | CLEAR |
| registro dati | 0x06 | 0x0A | 0x0E | 0x12 | 0x16 | 0x1A |

Ogni canale occupa un gruppo di **quattro** registri, `0x05 + 4·c .. 0x08 + 4·c`.
Il primo è **riempitivo e legge zero**; il valore a 24 bit sta nei tre
successivi, esattamente come dice il §4.3.6 del datasheet:
`DATA_H` = bit 23:16, `DATA_M` = 15:8, `DATA_L` = 7:0. Quindi base **0x06**,
passo 4.

⚠️ **Attenzione a una lettura naturale ma sbagliata della mappa del driver ST.**
Quella mappa definisce `CHANNELx(c) = 0x05 + 4c` con `_MM/_LM/_LL` a
0x06/0x07/0x08, e fa pensare che il valore *cominci* a 0x05 e che il datasheet
ne abbia omesso il byte alto. Non è così. `STALS_GetAlsValues` prende i primi
tre byte del gruppo — riempitivo, H, M — e li rende su una scala a 24 bit
spostandoli in alto di otto: **il driver ST legge deliberatamente il formato a
16 bit**, e infatti `STALS_LIB_normalize_als_for_xyz` divide `CountValue` per
256 per tornare ai conteggi.

Leggere da 0x05 non guadagna quindi un byte: ne **perde** uno, il byte basso, e
quantizza ogni conteggio a multipli di 256 sulla scala a 24 bit. Verificato
sull'hardware con `lux bytes`, 72 letture su due esemplari: riempitivo a zero
tutte le volte, byte basso diverso da zero tutte le volte. Su sei frame con la
luce che saliva lentamente, blu e IR restavano **congelati su un singolo
conteggio a 16 bit** mentre a 24 bit seguivano la stessa fluttuazione degli
altri canali.

Il §3.3.3 lo dice chiaramente: il byte basso *"is useful for improving sensing
resolution"* a bassa luce. Questa applicazione legge tutti e tre i byte.

`clear` e `vis` non sono ridondanti: **clear non ha filtro** (vede visibile +
infrarosso), **vis è filtrato IR**. La loro differenza è il contenuto
infrarosso della luce. Nella tabella colori del driver ST il canale 2 è infatti
`STALS_COLOR_CLEAR_IR_CUT`.

### 1.4 SEL_PD — quali diodi alimentano un canale

Registro `0x6B + c`, uno per canale, **non documentato nel datasheet**.
Cinque bit significativi (0x00..0x1F); scrivendo 0x20 o più il read-back
restituisce 0x00, il che conferma la larghezza.

Ogni bit seleziona uno dei fotodiodi **del colore proprio di quel canale**. La
prova è che il numero di bit accesi nella configurazione di ST coincide, per
tutti e sei i canali, con il numero di diodi di quel colore nella matrice:

| canale | SEL_PD (ST) | bit accesi | diodi di quel colore |
|---|---|---|---|
| RED | 0x07 | 3 | 3 ✅ |
| VIS | 0x07 | 3 | 3 ✅ |
| BLUE | 0x07 | 3 | 3 ✅ |
| GREEN | 0x1F | 5 | 5 ✅ |
| IR | 0x0F | 4 | 4 ✅ |
| CLEAR | 0x1F | 5 | 5 ✅ |

Questi valori vanno programmati all'avvio: al reset i registri **non**
contengono la configurazione utile. Se si saltano, il canale 2 legge zero per
sempre, perché nessun fotodiodo gli è collegato.

### 1.5 Il canale dark

I due fotodiodi `D` sono coperti da costruzione. Il loro conteggio è quindi
quello che il canale legge **in assenza di luce**: corrente di buio della
giunzione più l'offset della catena analogica, che a una sola temperatura non
sono distinguibili fra loro.

Il dark **non ha un canale proprio**. Si ottiene riprogrammando `SEL_PD` del
canale 2 (registro `0x6C`):

| valore | canale 2 legge |
|---|---|
| `0x07` = `00111` | i 3 fotodiodi **VIS** |
| `0x18` = `11000` | i 2 fotodiodi **D** (coperti) |

⚠️ **VIS e DARK sono lo stesso canale**: mai leggibili insieme. È il vincolo
principale che il dark impone a qualunque applicazione — o si rinuncia a VIS, o
si alternano i due (§2.7), o si ricostruisce VIS dagli altri canali (§2.6).

![Percorso del canale 2](diagrams/channel_path.dot)

Misura sull'hardware con il comando `lux probe` (guadagno 1x, 398 ms), che
percorre i valori di `SEL_PD` uno alla volta. ⚠️ I conteggi assoluti della
tabella sono su scala a **16 bit**: per confrontarli con una lettura a 24 bit
vanno moltiplicati per 256. I **rapporti**, che sono ciò per cui la tabella
esiste, non dipendono dalla scala.

| SEL_PD | al buio | luce piena | rapporto |
|---|---|---|---|
| 0x01 (bit 0) | 4 | 52295 | ×13000 |
| 0x02 (bit 1) | 4 | 41866 | ×10000 |
| 0x04 (bit 2) | 4 | 45580 | ×11000 |
| **0x08 (bit 3)** | **1** | **166** | **×166** |
| **0x10 (bit 4)** | **1** | **91** | **×91** |
| **0x18 (bit 3+4)** | **1** | **120** | **×120** |

I bit 0-2 variano di quattro ordini di grandezza con la luce; i bit 3 e 4 stanno
allo **0,2-0,3%** di quel valore in piena luce e a **1 conteggio** al buio. Sono
i due diodi coperti.

Il residuo di 91-166 conteggi non è sensibilità diretta ma luce che attraversa
la schermatura: è quantificato in §1.6, e ha conseguenze pratiche su come il
dark va letto.

Il datasheet specifica il *dark count noise* a **0..1,484 conteggi** (Tabella
15, misurato a guadagno 66x ed esposizione 100 ms): coerente con il conteggio
di 1 misurato al buio.

### 1.6 Quanto il dark segue la luce

I diodi coperti non sono perfettamente ciechi: un po' di luce trafila attraverso
la schermatura. La domanda pratica è **quanto**, perché da lì dipende se il dark
sia utilizzabile come riferimento di zero in presenza di luce.

**La misura.** Guadagno ed esposizione **fissi** (2,50x, ~355 ms), autogain
spento, sensore prima scoperto e poi coperto, su entrambi gli esemplari. Il
riferimento di luce è `CLEAR`, che è misurato — non `VIS`, che in dark mode è
ricostruito (§2.6) e a bassa luce non è affidabile.

| | luce (CLEAR normalizzato) | dark (NDARK) |
|---|---|---|
| **A** | 167.000 → 5.416.000 (**×32**) | 527 → 664 (**×1,26**) |
| **B** | 307.000 → 5.990.000 (**×19,5**) | 297 → 626 (**×2,1**) |

**Il dark non segue la luce.** Moltiplicando la luce per 32 il conteggio del
dark si muove di 1,26. Risolvendo `D = f·L + d` sui due punti, la parte
proporzionale vale **4,4 conteggi su 527** per A e **17,8 su 297** per B: al
livello di luce basso il dark è light-independent al **99%** su A e al **94%**
su B, e il fondo costante vale circa **171** conteggi normalizzati su A e
**153** su B (a 1,00x e 398 ms, scala a 24 bit).

Il fondo è **stabile allo 0,8% RMS**: su 22 campioni consecutivi la colonna
`DARK` è rimasta fra 431 e 445. ⚠️ Letti a 16 bit quegli stessi conteggi
varrebbero **1,7**, con il rumore interamente sotto il gradino di
quantizzazione: né il fondo né la sua stabilità sarebbero misurabili. È la
ragione pratica per leggere tutti e tre i byte del canale (§1.3).

**Cautele su questi numeri.**

- Il coefficiente di trafilamento `f` **non è affidabile**: le due condizioni
  avevano spettri molto diversi (il sensore era coperto con un dito, che è un
  filtro passa-rosso — trasmissione misurata 0,14% nel blu, 0,37% nel verde,
  2,2% nel rosso, 5,6% nell'IR, la finestra ottica del tessuto biologico) e il
  percorso di trafilamento ha una sua risposta spettrale. Serve una schermatura
  opaca e neutra per misurarlo davvero.
- Il **fondo** invece regge, perché il termine proporzionale valeva pochi
  conteggi su 176: anche sbagliando `f` di tre volte il fondo si sposta di una
  decina di conteggi. È un limite superiore — con luce residua può solo essere
  più basso.
- A **una sola temperatura** corrente di buio e offset fisso della catena sono
  indistinguibili. Separarli richiede di variare la temperatura (§1.7).

⚠️ Il rapporto `dark/luce` **non è costante**: su B passa da 7 a 0,28 parti per
mille mentre la luce sale. Il trafilamento non è proporzionale, e un modello che
lo tratti come una frazione fissa della luce è sbagliato.

⚠️ Corollario diagnostico invece valido: un dark che segue la luce
**proporzionalmente**, con rapporto vicino a 1, significa che il canale 2 sta
leggendo i diodi visibili e che la commutazione di `SEL_PD` non ha avuto
effetto.

Il datasheet specifica il *dark count noise* a **0..1,484 conteggi**
(**Tabella 16**, p. 39-40), misurato a guadagno **66x** ed esposizione 100 ms.
⚠️ Non è confrontabile direttamente con una lettura a 1x: riportato a quel
guadagno, un solo conteggio a 1x varrebbe ~66 conteggi a 66x. Significa
semplicemente che **a guadagno unitario la corrente di buio sta sotto il gradino
di quantizzazione** e non è misurabile — non che la parte sia fuori specifica.

### 1.7 Compensare la deriva termica con il canale dark

Nel silicio la corrente di buio **raddoppia grosso modo ogni 8-10 °C**. Su un
intervallo industriale, diciamo -20..+80 °C, sono un centinaio di gradi, cioè
**tre o quattro ordini di grandezza**. Per una misura di luce debole quello è
l'errore dominante, e il canale dark è ciò che la parte offre per rimuoverlo.

**L'idea centrale: non serve conoscere la temperatura.** Il dark *è* la
grandezza che si vorrebbe conoscere — l'offset che il canale ha in quel momento,
a quella temperatura. Si sottrae quello, e la temperatura non entra mai nel
calcolo. Un termometro sarebbe un secondo strumento da tarare, con un accoppiamento
termico proprio; il dark è già sullo stesso die del canale da correggere.

#### La parte non toglie il proprio buio

⚠️ Il VD6283TX **non sottrae da sé il proprio buio**, e non c'è nulla nella
catena che lo faccia: l'unica operazione applicata ai conteggi, nel driver ST
come in questa applicazione, è la **moltiplicazione** per il coefficiente OTP
(§1.11). Nessun termine additivo. I due *clamp* (`DC_CLAMP_EN` 0x30 e
`AC_CLAMP_EN` 0x2F) vanno scritti al bring-up perché ST li scrive, ma la loro
funzione non è documentata da nessuna parte — né nel datasheet né in un commento
del driver — e attribuire loro una sottrazione di buio è congettura.

Quindi: **se il buio va rimosso, lo rimuove l'applicazione.**

#### Sottrarre, non dividere

La deriva del buio è un errore **additivo**: si somma alla luce, non la scala.
Un errore additivo si toglie con una **sottrazione**. La divisione — prendere il
rapporto fra il canale di luce e il dark, nella speranza che la temperatura si
semplifichi — è l'operazione giusta per un disturbo *moltiplicativo* (una deriva
di guadagno), e su un offset **non solo non funziona: cancella la misura**.

Il canale clear vede la luce **più** il buio, sulla stessa catena:

```
CLEAR(T) = L + D(T)
```

Dividendo per il dark, il termine di temperatura non se ne va — resta, e diventa
il termine dominante:

```
CLEAR(T) / D(T) = L / D(T) + 1
```

Il `+ 1` non porta alcuna informazione sulla luce. Tutta l'informazione sta in
`L / D(T)`, cioè in un termine che **si rimpicciolisce esattamente quanto il
buio cresce**. Su un intervallo -20..+80 °C, prendendo il caso *più favorevole*
al rapporto — un raddoppio ogni ~11 °C, cioè un fattore **512** su cento gradi:

| | `D(T)` | `L / D(T)` | rapporto `CLEAR/D` | differenza `CLEAR − D` |
|---|---|---|---|---|
| **-20 °C** | `D0` | 1 | **2,000** | `L` |
| **+80 °C** | `512·D0` | 1/512 | **1,002** | `L` |

![Perché il rapporto CLEAR/dark non compensa la temperatura](diagrams/dark_ratio.py)

Il pannello **(a)** è la tabella disegnata: con la **luce ferma**, il rapporto
scivola sul proprio asintoto. L'area colorata — lo scarto fra la curva e la
retta a 1 — è *tutta* l'informazione sulla luce contenuta nella lettura, e a
+80 °C non c'è quasi più. Il pannello **(b)** dice la stessa cosa dal punto di
vista di chi deve decidere: quanto si muove la lettura se la luce sale del 10%.
La differenza risponde **10% a qualunque temperatura**; il rapporto risponde 5%
a freddo e **0,02%** a caldo.

A luce **immutata**, il rapporto passa da 2,000 a 1,002: la grandezza da
misurare è scesa dal 50% della lettura allo **0,2%**, ed è quasi interamente
sostituita dalla costante 1. Una soglia tarata a freddo è insensata a caldo, e a
caldo distinguere due livelli di luce che differiscono del 10% richiederebbe di
risolvere **due parti su diecimila** sul rapporto di due grandezze entrambe
rumorose. La misura è azzerata.

La sensibilità del rapporto ha una forma chiusa che vale la pena tenere a mente,
perché evita di attribuirle il fattore sbagliato:

```
risposta del rapporto = risposta della differenza / (1 + D/L)
```

Il rapporto perde un fattore **256** fra i due estremi — *non* 512, perché
**parte già dimezzato**: a freddo il `+ 1` vale quanto il segnale, quindi metà
della sensibilità è persa prima ancora che la temperatura salga. Il 512 del buio
ricompare come distanza **verticale** fra le due curve a +80 °C, cioè `1 + D/L`
= **513 volte**: è di tanto che la differenza è più sensibile del rapporto nel
punto peggiore.

⚠️ Qui `D` sta per il buio **del canale che si sta correggendo**, mentre il
riferimento misurato è il canale dark, che è un'altra catena (§1.7, tabella
sotto). La forma onesta della correzione è `a + b·DARK`, non `DARK` nudo — ma
l'argomento non cambia: sostituendo, il rapporto diventa `L/DARK + a/DARK + b` e
collassa allo stesso modo sulla costante `b`, mentre la differenza continua a
restituire `L` esatto. È la **forma** dell'operazione a essere sbagliata, non la
taratura.

La colonna a destra mostra il perché: nella **differenza** il termine `D(T)`
sparisce del tutto e resta `L`, a piena ampiezza e con lo stesso valore alle due
temperature. È il motivo per cui questa applicazione calcola
`VIS − Kdark·dark` (§2.7) e non un rapporto.

⚠️ La stessa obiezione vale per qualunque altra combinazione che **divida** per
il dark, incluso il `D/V` stampato nella tabella: quella colonna è un
**indicatore diagnostico** — dice quanto pesa il buio rispetto al segnale, ed è
comoda per accorgersi che il canale 2 non ha commutato — non un valore
compensato in temperatura.

#### Quale coppia canale/riferimento scegliere

Non tutte le combinazioni hanno la stessa base fisica.

| accoppiamento | percorso | commento |
|---|---|---|
| **VIS − DARK** | entrambi canale 2, DC | stesso ADC, stesso clamp, stesso guadagno, stesso coefficiente OTP: è una vera rimozione di offset. Ma **si alternano**, perché condividono il canale |
| **CLEAR − DARK** | AC contro DC | catene analogiche **diverse**, con clamp distinti e coefficienti OTP distinti. Non c'è motivo per cui i due offset coincidano: serve una relazione calibrata — è `Kdark`, misurata da `lux kdark cal` (§2.7) |
| **k·(R+G+B) − DARK** | tutti DC | simultanei, ma la somma porta **tre** correnti di buio contro un riferimento singolo, e `k` è tarato sulla *luce*, non sull'offset |

La forma onesta della correzione, quando i due non condividono il canale, non è
una sottrazione 1:1 ma una **relazione misurata**:

```
offset_canale(T) = a + b · DARK(T)
```

dove `b` assorbe la differenza di area, filtro e catena. `a` e `b` si ricavano
una volta, al buio, facendo variare la temperatura e registrando insieme il
canale da correggere e il dark. Da lì in poi si sottrae `a + b·DARK` e la
temperatura resta fuori dal calcolo.

#### Condizioni perché funzioni

- **Il dark deve essere risolto.** A 1,00x vale ~170 conteggi (§1.6), che letti
  a 16 bit diventano 1,7: un riferimento quantizzato a due livelli non corregge
  nulla. Servono la lettura a 24 bit (§1.3) e un guadagno adeguato — il
  datasheet specifica il rumore di buio a 66x.
- **Guadagno ed esposizione fissi, autogain spento.** Un riferimento preso con
  impostazioni diverse da quelle della misura va riportato alla stessa scala, e
  ogni passaggio in più è un errore in più.
- ⚠️ **Mai a cavallo di 33,33x.** È la soglia dove il coefficiente OTP cambia
  nibble (§1.11): le colonne assolute fanno un gradino fino al **18%** senza che
  la luce sia cambiata. Un rapporto fra due canali che commutano insieme è
  immune; un valore assoluto no.
- **Il dark si può leggere di rado.** Insegue la temperatura, che si muove in
  minuti: un frame di riferimento ogni decine di secondi basta, e nel frattempo
  si tiene l'ultimo valore. Nei frame in cui il canale 2 porta il dark, gli
  altri cinque canali continuano a misurare normalmente — non si resta ciechi.
- ⚠️ **Il rapporto è per esemplare.** Misurato su due parti dello stesso lotto:
  `b` (cioè `Kdark`) vale **0,91** su una e **1,21** sull'altra — il 30% di
  differenza. Un valore condiviso è sbagliato per entrambe. E non si deduce dal
  numero di diodi: il conteggio è già normalizzato su quello (§1.8).
- ⚠️ **Che `b` sia costante in temperatura è un'ipotesi, non un risultato.**
  `Kdark` è un rapporto fra due correnti di buio: se i diodi coperti e quelli da
  correggere hanno lo stesso coefficiente termico il rapporto è invariante, ed è
  ciò che rende legittima tutta la costruzione. Se differiscono, `b` deriva con
  la temperatura e la correzione, anziché togliere la temperatura dal segnale,
  ce la infila. Su questo banco `Kdark` si ripete allo 0,8% (una parte) e al
  2,8% (l'altra) **a temperatura ambiente**; a temperature diverse non è ancora
  stato misurato, ed è la prima cosa che una prova termica deve stabilire.

#### Che cosa questo documento non dice

A **una sola temperatura**, corrente di buio e offset fisso della catena danno
lo stesso numero e non sono separabili. Solo una variazione di temperatura li
distingue: la prima raddoppia ogni ~9 °C, il secondo no. Verificarlo, e ricavare
`a` e `b`, è una campagna di misura che esula da questa guida — qui si descrive
solo che cosa il sensore mette a disposizione per farla.

### 1.8 Normalizzazione sul numero di diodi

Il registro `SPARE_1` scritto a `0x3F` — commento del driver ST: *"set count to
be independent of pd numbers"* — fa sì che il conteggio sia una **media** sui
diodi selezionati, non una somma.

Conseguenza che sorprende: selezionando *più* diodi il conteggio può
**scendere**. Dalla sweep sopra, in piena luce:

| SEL_PD | conteggio |
|---|---|
| 0x07 (3 diodi visibili) | 48558 |
| 0x0F (+ 1 diodo scuro) | 36832 |
| 0x1F (+ 2 diodi scuri) | 29096 |

Aggiungendo diodi coperti a diodi illuminati la media cala. Non è un difetto: è
ciò che rende confrontabili fra loro canali con numero di diodi diverso, ed è
il presupposto del confronto `clear` vs `vis`.

### 1.9 Esposizione e guadagno

**Esposizione** (registri `0x1D`/`0x1E`, `EXTIME` a 10 bit):

```
esposizione = EXTIME × 1,6 ms      da 1,6 ms a 1636,8 ms
```

Il passo è di 1,6 ms perché EXTIME conta cicli dell'oscillatore interno da
10,24 MHz. Il driver ST scrive **prima il byte basso (0x1E), poi l'alto
(0x1D)**, mascherando quest'ultimo con `0x03`.

⚠️ **Il §4.3.12 stampa la formula con un `+1`, ed è sbagliata.** Lo stesso
datasheet si contraddice in tre punti, e tutti e tre danno ragione alla forma
senza offset:

| dove | dice | senza `+1` | con `+1` |
|---|---|---|---|
| Tabella 16 | `0x001` = 1,6 ms | 1 × 1,6 = **1,6** ✔ | 3,2 ✘ |
| Tabella 16 | `0x047` = 113,6 ms | 71 × 1,6 = **113,6** ✔ | 115,2 ✘ |
| Tabella 16 | `0x048` = 115,2 ms | 72 × 1,6 = **115,2** ✔ | 116,8 ✘ |
| §4.3.12 | reset `0x0032` = 80 ms | 50 × 1,6 = **80,0** ✔ | 81,6 ✘ |

Anche il driver ST concorda: `set_exposure()` scrive `round(µs / 1600)`,
ricalcola `passi × 1600` e limita i passi a `[1, 0x3FF]` — cioè tratta lo 0
come non valido, il che ha senso solo senza il `+1`. E
`STALS_LIB_normalize_als_for_xyz()` normalizza a 100800 µs, che su questa
convenzione sono **63 passi esatti** (e 62,0 sull'altra: non un numero tondo,
il che è già un indizio).

⚠️ **Questa applicazione ha seguito il §4.3.12 fino al 2026-09-02**, poi è
passata alla forma corretta. Conseguenze: la colonna `EXP` legge ora 398 ms dove
leggeva 400, e la normalizzazione ha perso il `+1` da entrambi i lati del suo
rapporto. Quel `+1` **non si semplificava**, contrariamente a quanto questo
documento affermava: l'errore vale `1/(passi+1)`, cioè 0,4% all'esposizione di
default ma **un fattore 2 a EXTIME = 1** e 1,5x a EXTIME = 2. Le catture prese a
esposizione corta e normalizzate ne sono affette.

La convenzione è isolata nella macro `EXTIME_PLUS_ONE`: rimetterla a `1u`
riporta tutto alla lettura precedente in una riga. L'esperimento che chiude la
questione in laboratorio è EXTIME = 1 contro EXTIME = 3 a luce ferma — conteggi
in rapporto 1:3 significano nessun `+1`, 1:2 significano `+1`.

**Guadagno analogico** (registri `0x25 + c`, uno per canale):

⚠️ **Il registro non contiene il guadagno, ma l'indice + 1** di una tabella
interna. Scrivendovi un valore di guadagno si ottiene silenziosamente un
guadagno sbagliato — la parte accetta qualunque byte.

| indice | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | **12** | 13 | 14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| guadagno | 66,67x | 50x | 33,33x | 25x | 16,67x | 10x | 7,14x | 5x | 3,33x | 2,5x | 1,67x | 1,25x | **1,00x** | 0,83x | 0,71x |

La tabella è **discendente**: alzare il guadagno significa *diminuire*
l'indice. Il guadagno unitario è l'indice 12, che si scrive nel registro come
`0x0D`.

⚠️ Tre fonti danno tre insiemi di valori, e hanno ragione su cose diverse: il
§4.3.13 elenca i codici nominali (66,6x, 16x, 7,1x), la Tabella 16 dà i valori
**tipici misurati** (66,78, 16,76, 7,17), il driver ST usa quelli in tabella qui
sopra (66,67, 16,67, 7,14). Vanno tenuti quelli del driver: sono ciò contro cui
`set_channel_gain()` arrotonda e in cui è espressa la soglia cal10/cal50
dell'OTP (§1.11), quindi una tabella diversa sceglierebbe un coefficiente
diverso per lo stesso codice di registro.

### 1.10 Saturazione

Il livello a cui un canale si ferma dipende dall'esposizione, e la **Tabella 16**
lo dà su **entrambe** le scale di lettura:

| EXTIME | tetto a 24 bit | tetto a 16 bit |
|---|---|---|
| `0x001`..`0x047` | 233.045 per passo | 910 per passo |
| `0x048`..`0x3FF` | **16.777.215** | 65.535 |

Le due colonne sono lo stesso tetto fisico letto a due risoluzioni, in rapporto
1:256 — lo stesso rapporto del minimo riportabile (256 contro 1). Questa
applicazione legge i 24 bit pieni (§1.3), quindi vale la colonna di sinistra.

⚠️ **La soglia è sul numero di passi, non sui millisecondi.** La Tabella 16
divide a `0x047`/`0x048`, e il confronto va fatto lì: `EXTIME` contro `0x47`.
Convertire prima in millisecondi introduce il troncamento a intero della
conversione — e, finché questa applicazione portava il `+1` di §1.9, spostava
addirittura il passo 71 dalla parte sbagliata. Lavorare sui passi è immune a
entrambi i problemi, ed è il motivo per cui `als_sat_level()` è scritta così.

⚠️ La nota del datasheet *"based on a 16-bit ADC format"* si riferisce alla
**colonna di destra**, non a un tetto valido in ogni caso: chi legge i 24 bit
pieni e confronta con 65535 vede un canale saturo **256 volte** prima che lo sia
davvero.

Il §3.3.4 aggiunge una prescrizione importante: il controllo di saturazione va
fatto **sui dati grezzi**, prima della correzione di fabbrica. La correzione
scala un valore già bloccato e lo porta *sopra* il tetto — testandone il
risultato non si rileverebbe alcun clipping. Il sintomo, su una lettura a 16
bit: un `CLEAR` bloccato che riporta 79870 contro un tetto di 65535, cioè
esattamente `65535 × il proprio coefficiente`.

Restare **sopra** `0x047` passi porta il tetto a fondo scala invece che a un
valore proporzionale ai passi: è il motivo per cui l'applicazione usa 398 ms
(EXTIME 249) di default.

#### Il pavimento: sotto 256 conteggi la parte riporta zero

Il tetto ha un gemello in basso, ed è quello che in pratica sorprende di più.
Il §3.3.3 lo enuncia così: *"the minimum reported irradiance level (guaranteed
by design) is 0x000100"*, e la Tabella 16 lo ripete come **256 conteggi** sulla
lettura a 24 bit (1 su quella a 16). Sotto quel livello il registro non
restituisce un numero piccolo: restituisce **zero**.

⚠️ Non è la soglia software di ST discussa in §1.11 — quella è una scelta del
driver su un valore già corretto, questa è la parte. Non riprodurre la prima
non fa comparire i valori che la seconda ha già azzerato.

La conseguenza operativa è netta: **in una scatola buia a 1,00x e 398 ms ogni
colonna legge 0**, e sembra un guasto. Non lo è. Scalando le misure di questo
banco da 66,67x:

| grandezza | a 66,67x | previsto a 1,00x / 398 ms |
|---|---|---|
| dark (A) | 14.753 grezzi a 400 ms | **222** conteggi |
| clear (A) | 58.078 grezzi a 1636 ms | **212** conteggi |

Entrambi sotto 256, quindi entrambi zero — e con loro `NDARK`, `NCLR`, `mLUX` e
tutto ciò che ne discende. Il rimedio è guadagno, esposizione o autogain. Dal
2026-09-02 `lux meas` e `lux meas_cl` **avvisano all'armamento** se l'autogain è
spento e il guadagno è sotto 5x, citando questa soglia.

### 1.11 La memoria OTP — calibrazione di fabbrica

Ogni esemplare porta in memoria OTP (*One-Time Programmable*) i dati della
propria calibrazione di fabbrica. **Nessuno di questi registri è documentato
nel datasheet.**

| registro | contenuto |
|---|---|
| `0x58` | OTP_CTRL1 — scrittura di 0 per avviare la lettura |
| `0x5A` | OTP_STATUS — bit 1 e 3 a 1 quando entrambi i banchi sono pronti |
| `0x5B` | banco 0, 8 byte |
| `0x63` | banco 1, 8 byte |

I 120 bit **non sono in ordine naturale**: ogni byte è a bit invertiti, e il
banco 1 è sfasato di 4 bit perché il nibble basso del suo primo byte appartiene
al banco 0. Il disimpacchettamento va riprodotto alla lettera dal driver ST;
sbagliandolo si ottengono coefficienti plausibili ma falsi.

Campi utilizzati:

| bit | ampiezza | campo |
|---|---|---|
| `c·8` (c = 0..5) | 8 | **due** coefficienti del canale c, uno per nibble |
| 51 | 9 | `hf_trim` — trim dell'oscillatore a 10,24 MHz |
| 60..112 | — | identificativo univoco del pezzo (vedi sotto) |
| 113 | 3 | versione OTP (sommata a 0x10) |
| 116 | 4 | `lf_trim` — trim dell'oscillatore a 48,7 kHz |

⚠️ **Ogni byte contiene DUE coefficienti, non uno.** Il nibble alto è la
calibrazione per guadagni sotto 33,33x, quello basso per 33,33x e oltre
(`calfactor1050()` nel driver ST, soglia `0x2154`). Quale dei due sia in vigore
dipende dal guadagno del momento, quindi con l'autogain acceso può cambiare in
corsa. `lux otp` li mostra entrambi, con `<-` su quello selezionato dal guadagno
corrente.

I due possono essere molto distanti: su un esemplare il canale `clear` ha byte
`0x9F`, cioè ×1,031 sotto soglia e **×1,218** sopra — e i canali non si spostano
tutti nello stesso verso (su un altro pezzo il blu *scende*, da ×1,031 a
×1,000). Attraversare i 33,33x quindi non riscala i canali uniformemente: ne
cambia il **bilanciamento**.

**Identificativo del pezzo.** Dai bit 60..112 il driver ST ricava un UID di 14
caratteri (`otp_generate_uid`), riprodotto qui in `otp_uid()` e stampato da
`lux otp`. ⚠️ Serve perché **A e B sono posizioni di bus, non pezzi**: basta
riconnettere i due sensori a rovescio perché ogni coefficiente si sposti
silenziosamente sull'altra etichetta, senza che nulla nella cattura lo segnali.
Qualunque dato tarato per esemplare va riferito all'UID.

**Perché conta.** Senza OTP ogni parte usa il valore nominale (`0x80`, fattore
×1,000). Il coefficiente reale spazia da **×0,781 a ×1,219**, cioè fino al
**56% di scarto fra due esemplari**: le letture restano ripetibili sul singolo
sensore ma **non sono confrontabili fra sensori diversi** — che è esattamente
la domanda a cui questa applicazione deve rispondere.

`hf_trim` aggrava il problema: corregge l'oscillatore che *conta* l'esposizione,
quindi senza di esso la finestra da 398 ms è nominale e non reale, e i due
errori si moltiplicano.

Misura sui due esemplari in uso, **identificati per UID** e non per posizione di
bus. ⚠️ I coefficienti riportati sono i **soli nibble cal10** (guadagno sotto
33,33x); sopra soglia valgono gli altri, che possono muoversi in verso opposto.

| | `ade869b895218f` | `a87bd2032e9a34` | default |
|---|---|---|---|
| versione OTP | 0x15 | 0x15 | — |
| `hf_trim` | 0x0F6 | 0x0EE | 0x0E3 |
| `lf_trim` | 0x6 | 0x6 | 0x7 |
| coeff. RED | ×1,093 | ×1,000 | ×1,000 |
| coeff. VIS | ×1,062 | ×1,031 | ×1,000 |
| coeff. BLUE | ×1,000 | ×1,031 | ×1,000 |
| coeff. GREEN | ×1,093 | ×1,062 | ×1,000 |
| coeff. IR | ×1,062 | ×1,031 | ×1,000 |
| coeff. CLEAR | ×1,031 | ×1,031 | ×1,000 |

Su RED c'è il **9,3%** di errore sistematico fra i due, e gli `hf_trim`
differiscono di 8 passi — nessuno dei due è il valore nominale.

**Applicazione della correzione** (da `apply_calfactor` del driver ST):

```
coefficiente = f(nibble del byte OTP)        128 = ×1,000
conteggio corretto = (coefficiente × grezzo) >> 7
```

Quale dei due nibble usare dipende dal **valore** del guadagno in uso (soglia
0x2154 = 33,33x), non dal codice del registro. Con l'autogain il guadagno
cambia in corsa, quindi va deciso a ogni campione.

⚠️ **Una soglia di ST non è riprodotta**: il driver azzera qualunque valore
corretto sotto 256 conteggi. Su un banco di misura cancella dati veri — visto
due volte sull'hardware: un dark di 118 conteggi azzerato mentre l'altro
sensore ne riportava 318 solo perché a guadagno più alto, e `RED`/`BLUE` a zero
in luce debole mentre `GREEN` a 256 sopravviveva. Uno zero che significa
"piccolo" è indistinguibile da uno che significa "guasto", e il secondo è ciò
che si deve poter vedere.

### 1.12 Bring-up: cosa serve oltre al datasheet

La parte risponde su I2C e produce numeri plausibili anche senza la sequenza
di inizializzazione completa. I passi seguenti non sono nel datasheet ma sono
necessari perché i conteggi siano corretti (da `dev_configuration` di ST):

L'ordine è quello di `dev_configuration()`, verificato sul sorgente:

1. **Lettura OTP** — `otp_reset`, poi il caricamento dei due banchi e la
   decodifica dei campi (§1.11). Va per prima perché fornisce i trim al passo 2.
2. **Trim degli oscillatori** — `0x3D` = 1, poi `hf_trim` su `0x3E`/`0x3F` e
   `lf_trim` su `0x40`. Determina quanto dura realmente un'esposizione.
3. **Clamp** — `AC_CLAMP_EN` (0x2F) = 1, `DC_CLAMP_EN` (0x30) = 0x1F. ⚠️ Che
   cosa facciano esattamente **non è documentato**: né il datasheet li elenca,
   né il driver ST li commenta. Vanno scritti perché ST li scrive; attribuire
   loro una funzione precisa è congettura (§1.7). `SPARE_1` (0x72) = 0x3F per
   la normalizzazione sul numero di diodi, e questa invece è documentata da un
   commento di ST.
4. **Selezione fotodiodi** — `SEL_PD` per tutti e sei i canali (§1.4).
5. **Guadagno** su tutti i canali (ST parte da 0x0100, cioè 1x).
6. **Pedestal** (0x32) = 3, percorso flicker.
7. **Dithering** — registri 0x76..0x79, allarga lo spettro del clock.
8. **GPIO2 come ingresso** — `SPARE_0` (0x71) = 1, stato di riposo.

L'**esposizione** non fa parte di `dev_configuration`: ST la imposta a parte,
con `STALS_SetExposureTime`, e così fa questa applicazione.

Restano poi, al momento dell'avvio della misura:

9. **Abilitazione canali** — due registri con ruoli diversi:
   - `DC_EN` (0x2E) è la **maschera dei canali** sul percorso DC, cioè i
     canali 1..5;
   - `AC_EN` (0x2D) abilita il percorso **AC**, che serve il canale 6/clear
     perché quel canale fa anche da canale flicker.

   Il datasheet li chiama "ALS channel enable" e "Channel 6 enable"; i nomi del
   driver (`DC_EN`/`AC_EN`) dicono meglio di cosa si tratti. Servono
   **entrambe** le scritture per avere tutti e sei i canali.
10. **Flicker off** (`0x31` = 0), altrimenti sottrae il canale clear all'ALS.
11. **Avvio** — `ALS_CTRL` (0x03). Il "modo 3" è la combinazione
    `ALS_START | ALS_CONTINUOUS | ALS_CONTINUOUS_SLAVED` = bit 0, 1 e 2: è
    l'aggettivo *slaved* a significare che ogni frame attende il riscontro
    dell'host (§1.13).

### 1.13 Il handshake di frame (modo 3)

Nel modo continuo 3 la parte completa una misura e poi **si ferma**, in attesa
di un riscontro dal microcontrollore. Il §3.3 lo dice esplicitamente: i dati
I2C non vengono aggiornati fino al riconoscimento successivo.

Senza il riscontro ogni lettura restituisce lo stesso frame congelato: i numeri
smettono semplicemente di rispondere alla luce.

Il riscontro si dà su `INTERRUPT_CTRL` (0x02) scrivendo 1 e poi 0 sul bit
`CLR_INTR`. Il bit `INTR_ST` (bit 1) indica lo stato: **1 = misura ancora in
corso**, 0 = frame completo.

⚠️ **Attendere un tempo fisso al posto del polling non funziona.** Il confine
del frame è scandito dall'oscillatore del sensore, non dal clock del
microcontrollore: i due derivano, e l'attesa a volte cade appena prima del
confine restituendo un valore che contiene ancora parte del frame precedente.
Con i fotodiodi commutati sul dark il difetto si manifestava come picchi
sporadici di alcune centinaia di conteggi — circa il 5% della lettura visibile,
cioè la coda di un frame visibile che filtrava in uno scuro.

### 1.14 Indirizzo I2C

L'indirizzo è **0x20 a 7 bit**, che il datasheet cita anche come 0x40 — il byte
di scrittura a 8 bit (indirizzo << 1). Sono lo stesso indirizzo scritto in due
modi; l'API caOS vuole la forma a 7 bit.

L'indirizzo **è fisso per ogni VD6283TX**: cambiarlo richiede di tenere GPIO1
basso durante l'accensione, e va ripetuto a ogni accensione (§3.4.2). È il
motivo per cui i due sensori di questa applicazione stanno su **due bus
separati**: sullo stesso bus risponderebbero entrambi.

Velocità supportate fino a 1 MHz; l'applicazione usa 100 kHz, adeguati a
cablaggi volanti con pull-up discreti. La cadenza di campionamento è comunque
determinata dall'esposizione, non dal bus.

---

## 2. L'applicazione

### 2.1 Che cosa fa e perché è fatta così

L'applicazione legge **due VD6283TX** e mette le loro letture una accanto
all'altra, in forma tabellare, per rispondere a una domanda sola: *i due
esemplari misurano la stessa cosa sotto la stessa luce?*

Da quella domanda discendono le scelte non ovvie:

- **un bus I2C per sensore** — l'indirizzo è fisso e non cambiabile a caldo
  (§1.14), quindi sullo stesso bus risponderebbero entrambi;
- **la configurazione è comune ai due** — se un sensore misurasse a guadagno
  diverso dall'altro, il confronto sarebbe fra *configurazioni*, non fra
  sensori, e la differenza sembrerebbe un guasto;
- **i task partono fermi** — si avviano con `lux start`. Con entrambi liberi
  dal reset non ci sarebbe modo di capire a quale bus appartiene un guasto;
- **colonne normalizzate** — l'autogain è invece per-sensore (§2.5), quindi i
  due possono finire su guadagni diversi: le colonne `NVIS`/`NDARK` riportano
  tutto a un riferimento comune e restano confrontabili comunque.

### 2.2 Struttura dei file

| file | contenuto |
|---|---|
| `vd6283_ctrl.h` | mappa registri, tipi, costanti, mappatura per-MCU |
| `vd6283_ctrl.c` | calcoli puri: OTP, autogain, saturazione, normalizzazione |
| `vd6283_i2c.h/.c` | ogni accesso al bus: registri, bring-up, frame, sweep |
| `ca_os_main_app_50.c` | task, CLI, menu, stampa |
| `os_config_app_50.h` | configurazione (generata, **mai** da editare a mano) |

La separazione fra `_ctrl` e `_i2c` non è estetica: in `_ctrl` non c'è una sola
scrittura sul bus, quindi quelle funzioni si possono verificare senza un
sensore collegato — ed è così che l'impacchettamento OTP è stato validato
contro il driver ST su 4000 casi generati.

### 2.3 I task

Tre task, creati in `ca_os_main`:

| task | livello | ruolo |
|---|---|---|
| `lux_a` | LEVEL3 | misura il sensore A |
| `lux_b` | LEVEL3 | misura il sensore B |
| `lux_pr` | LEVEL4 | preleva dalla coda e stampa |

![Architettura dei task](diagrams/task_flow.dot)

I due task di misura **possiedono il proprio bus**: sono gli unici a scrivere
sul sensore mentre gira. La CLI non tocca mai il bus di un sensore in marcia —
imposta una richiesta (`dark_req`, `probe_req`) che il task applica al confine
di un frame. È il motivo per cui `lux setup` pretende che entrambi siano fermi.

I task si fermano su un **flag interrogato periodicamente**, non su un semaforo:
un semaforo richiederebbe che il task abbia già raggiunto l'attesa prima del
segnale, e sull'hardware questo si è tradotto in avvii persi.

Fra misura e stampa c'è una **coda di 8 elementi**. Se la stampa non sta al
passo, il task di misura non si blocca: conta la perdita nella colonna `DROP`.
Un task di misura fermo su una coda piena smetterebbe silenziosamente di
leggere il sensore.

### 2.4 Il flusso di una misura

1. `lux start` alza il flag; il task se ne accorge entro 50 ms.
2. `vd_configure()` identifica la parte, **legge l'OTP** e programma tutta la
   sequenza di bring-up (§1.12).
3. Se `k` non è mai stato misurato, i primi 16 campioni servono a calibrarlo
   (§2.6) e sono marcati `CAL`.
4. Da lì il ciclo è: attesa del frame su `INTR_ST` → lettura dei sei canali →
   correzione OTP → autogain → invio in coda.
5. `lux stop` abbassa il flag: il task finisce il frame in corso, disabilita il
   motore ALS e torna in attesa. Non viene mai sospeso dall'esterno, quindi non
   può restare bloccato a metà di una transazione I2C.

### 2.5 L'autogain

Mantiene il canale **clear** fra il 40% e l'85% del fondo scala, con obiettivo
al 70%. Clear è il canale più sensibile (5 fotodiodi, nessun filtro): tenendo
lui in campo, gli altri ci stanno di conseguenza.

**Non procede per passi.** Il conteggio è proporzionale a guadagno × esposizione,
quindi il fattore necessario è semplicemente `obiettivo / attuale`: si calcola
la coppia (guadagno, esposizione) e ci si salta in una mossa. Procedendo per
indici, il recupero dal buio richiedeva fino a 14 campioni — quasi sei secondi
di letture visibilmente sbagliate. Ora ne bastano **1-3**.

Due accorgimenti non ovvi:

- **Quando clear è saturo non dice quanto è oltre**: ogni sovraesposizione
  legge lo stesso tetto. In quel caso la media di R, G, B — ancora in campo perché
  filtrati e con meno diodi — stima quanto clear *avrebbe* letto. Il rapporto
  fra le due scale è **misurato a runtime**, perché dipende dallo spettro della
  luce: sull'hardware varia fra 8,4 e 24,6.
- **L'esposizione si muove solo se il guadagno non basta**, perché cambia la
  cadenza dei campioni mentre il guadagno no.

⚠️ L'autogain è **per sensore**: reagisce alla luce che quel sensore riceve
davvero. Dopo un suo intervento i due possono avere guadagni diversi — è
legittimo, ed è la ragione per cui esistono le colonne normalizzate.

### 2.6 La ricostruzione di VIS

VIS e DARK sono lo stesso canale (§1.5), quindi non si leggono insieme. La prima
delle due vie d'uscita è **calcolare VIS invece di misurarlo**, dagli altri
canali:

```
VIS = k * (RED + GREEN + BLUE)
```

Il vantaggio è la **simultaneità**: dark e visibile si riferiscono allo stesso
frame, e il canale 2 può restare sul dark per tutta la cattura. Il prezzo è che
VIS non è più misurato. L'altra via — alternare davvero i due — è descritta in
§2.7.

Misurato sull'hardware, `k` sta fra **0,99 e 1,10** ed è stabile allo
**0,02-0,33%** a luce costante. Non è però una costante universale: fra due
condizioni di illuminazione è passato da 1,099 a 1,072, seguendo lo spettro
esattamente come il rapporto clear/RGB dell'autogain. Va quindi **misurato sotto
la luce che si userà**.

Lo fa `lux cal`, su 16 campioni con il canale 2 ancora sul visibile. Dopo, `k`
resta **congelato**: un coefficiente che si muove renderebbe le righe non
confrontabili fra loro. Il valore in vigore è riportato nell'intestazione `#`
della cattura, per sensore.

⚠️ **Solo `lux cal` misura `k`** — `lux start` non lo fa più (§2.8). E richiede
luce vera: al buio il rapporto degenera a 1/3, perché VIS, R, G e B portano
tutti soltanto corrente di buio. Misurato 0,374 in scatola chiusa contro ~1,07
sotto lampada. Un `k` così non descrive nessuno spettro, e le colonne `CVIS` e
`D/V` costruite su di esso non significano nulla: se l'intestazione riporta un
`k` intorno a 0,33, la calibrazione è stata fatta al buio e va rifatta.

Accuratezza misurata: errore tipico **0,3-0,5 per mille**. Va confrontato con il
`D/V` che si sta osservando, che dipende dalla luce: a sensore coperto sono
risultate circa 20 parti per mille (rapporto 40:1, comodo), ma in luce ordinaria
`D/V` scende a **0,3 parti per mille** — cioè allo stesso ordine dell'errore di
ricostruzione. È la ragione per cui, a luce piena, un `VIS` misurato è
preferibile a uno ricostruito.

Con il dark **spento** la colonna mostra `CVIS` accanto al VIS misurato: serve a
verificare il metodo prima di fidarsene. Con il dark **acceso** diventa `DARK`,
e VIS porta il valore ricostruito — a quel punto dark e visibile si riferiscono
allo **stesso istante**, che è il vantaggio per cui la ricostruzione esiste.

⚠️ **Due limiti da conoscere prima di adottarla.** La somma di tre canali somma
anche i loro tre rumori: il segnale cresce del 18% rispetto a VIS da solo
(58,7 contro 49,7 conteggi/(µW/cm²) a parità di guadagno, Tab. 19-22) mentre il
rumore cresce di 1,73 (radice di 3), quindi il rapporto segnale/rumore **peggiora di circa il
30%**. E a bassa luce la ricostruzione smette di seguire: fittando il
trafilamento (§1.6) su `NVIS` invece che su `CLEAR` le due pendenze differivano
di sette volte, perché vicino al fondo R, G e B sono dominati dai propri offset.
Per una misura vicino alla soglia di sensibilità, quindi, un VIS misurato è
preferibile a uno ricostruito.

### 2.7 L'acquisizione mista VIS/DARK

La ricostruzione compra la simultaneità al prezzo di non misurare più VIS.
`lux meas` fa il **compromesso opposto**: il canale 2 resta sui fotodiodi
visibili, quindi **VIS è misurato**, e il dark viene campionato una volta ogni
`div` campioni e **tenuto** fra un riferimento e l'altro. La lettura corretta è

```
corr = VIS - (Kdark * dark_tenuto) / 1000
```

`Kdark` è espresso ×1000, quindi 1000 = 1,000.

⚠️ **Non va dedotto dal numero di diodi.** Il conteggio è già una **media sui
diodi selezionati** (§1.8) — è ciò che fa `SPARE_1 = 0x3F` — quindi il rapporto
5 a 2 fra i diodi clear e quelli coperti **è già stato diviso via dalla parte**,
e moltiplicare per 2,5 introdurrebbe un errore di 2,5x invece di toglierne uno.
Lo sweep di §1.5 lo dimostra: i due diodi coperti letti insieme danno 120, non
la somma 166 + 91 = 257, ma la loro media.

Ciò che Kdark deve assorbire è tutto il resto: i coefficienti OTP dei due canali
(diversi), le due catene di clamp (`DC_CLAMP_EN` per CH1..CH5, `AC_CLAMP_EN` per
CH6), l'imprecisione della media stessa (120 misurati contro 128,5 attesi, −7%) e
un'eventuale differenza vera di densità di corrente di buio. Nessuno di questi si
deriva a tavolino: **va misurato**, con `lux kdark cal`.

#### Kdark è per esemplare, non per configurazione

⚠️ È l'unica grandezza di questa applicazione che **non** è comune ai due
sensori. Tutto il resto è deliberatamente globale — i due pezzi si confrontano
solo se girano nella stessa configurazione — ma Kdark non è una configurazione:
è una calibrazione di parte, della stessa famiglia di `k`. Misurato su questo
banco:

```
A: 0,918  0,931  0,905      (tre misure, dispersione 2,8%)
B: 1,202  1,208  1,212      (tre misure, dispersione 0,8%)
```

Un valore condiviso non può essere giusto per entrambi. Con `Kdark = 1,200` per
tutti e due — quasi esatto per B, del 30% troppo alto per A — A bloccava a zero
**ogni** riga mentre B lasciava un residuo positivo di 5-10. Per questo
l'argomento `Kdark` è stato **tolto** da `lux meas`: un argomento posizionale in
mezzo a quel comando poteva solo imporre lo stesso valore a entrambi, e stava
dove il lettore si aspetta il `discard`. Un `lux meas_cl 1 0 3` digitato per
provare il discard ha azzerato in silenzio due calibrazioni misurate.

#### Misurarlo: `lux kdark cal`

Al buio CLEAR non raccoglie altro che la propria corrente di buio, quindi la
definizione è il rapporto, e i due termini vengono **dallo stesso frame**: in
dark mode il canale 2 porta il dark mentre il canale 6 continua a integrare
indisturbato. Nessuna commutazione, nessun valore tenuto, nessun frame da
scartare — è ciò che rende questa misura automatizzabile dove `k` richiede il
giro di modi di `lux cal`.

⚠️ **Sui conteggi grezzi.** `meas_correct()` sottrae grezzo da grezzo, quindi
Kdark deve essere un rapporto di grezzi. Ricavarlo a mano dalle colonne `CLEAR`
e `DARK` stampate dà un numero sbagliato del rapporto fra i coefficienti OTP dei
due canali — su queste parti l'11-14%.

Il comando si prende da sé **guadagno ed esposizione massimi** e li restituisce
alla fine: a 1,00x il dark legge 0 (§1.10) e il rapporto non ha né numeratore né
denominatore. Massimi e non "quanto basta", perché non c'è nulla da cercare —
il segnale utile *è* la corrente di buio. In più regala un controllo: a piena
sensibilità la luce ambiente porta il rapporto verso 50, e sopra 5,000 il
comando **rifiuta** senza memorizzare nulla.

⚠️ Nessun controllo automatico distingue il buio dalla **luce fioca**: a un
rapporto di 3 il comando accetta un valore che è per lo più illuminazione. La
garanzia è la scatola chiusa.

**La sottrazione è fatta sui conteggi grezzi, prima della normalizzazione.** È
lecito solo perché un rinfresco del dark è **forzato** ogni volta che guadagno o
esposizione si muovono: il dark tenuto e il VIS che corregge vengono quindi
sempre dalle stesse impostazioni. Senza quel vincolo si sottrarrebbero conteggi
presi a un guadagno da conteggi presi a un altro.

Il rinfresco avviene **in cima al ciclo**, prima della lettura: il dark che
corregge una riga non è mai posteriore alla riga stessa. Se la lettura del dark
fallisce sul bus, l'ultimo valore buono resta in vigore — un dark vecchio è una
risposta peggiore di uno fresco, ma molto migliore di nessuna.

⚠️ **Un dark che legge zero viene tenuto, non scartato.** A 1,00x il fondo di
queste parti sta sotto il conteggio (§1.6), quindi zero è la lettura corretta al
buio; filtrarlo sostituirebbe di nascosto un valore preso in un momento
illuminato.

**Il costo in tempo.** Un rinfresco non è una finestra di esposizione ma
`(1 + discard)` per ogni scrittura di `SEL_PD`, e le scritture sono due — verso
i diodi coperti e ritorno. Quattro finestre con il valore predefinito, cioè
~1,6 s a 398 ms: a `div = 10` la cattura è circa il **40% più lenta**.

⚠️ **`discard = 0` non funziona**, benché il §3.3 del datasheet lasci pensare di
sì (la parte si ferma dopo ogni misura in attesa del riscontro, quindi `SEL_PD`
si scrive mentre nulla sta integrando). Provato: il visibile torna **esattamente
zero** su righe alterne e il dark risale 545, 866, 7076, 11639, 21248 — frame
parziali, non luce. Un conteggio di zero esatto è la firma: i registri non erano
stati rinfrescati quando sono stati letti. Il valore predefinito è 1, ed è il
punto esatto in cui un dark "che segue la luce" va cercato prima di attribuirlo
all'ottica.

#### La variante su CLEAR: `lux meas_cl`

Stesso modo, stesso dark, stesso `div`, stessa riga — cambia **solo il canale
corretto**: `lux meas` corregge VIS (canale 2), `lux meas_cl` corregge CLEAR
(canale 6).

CLEAR è il rivelatore più largo che la parte abbia: la Figura 17 conta **cinque**
fotodiodi clear contro tre visibili, e la legenda dell'§8.1 dice che clear non ha
filtro, quindi raccoglie il visibile **e** l'infrarosso dove VIS è filtrato IR.
Più diodi e più banda spingono nella stessa direzione: a parità di luce il
conteggio è maggiore e il pavimento di un allarme di soglia sta proporzionalmente
più lontano dal rumore. È anche il canale che l'autogain già governa, quindi il
canale corretto e quello governato diventano lo stesso.

⚠️ **La sottrazione attraversa due canali, e Kdark non è più lo stesso numero.**
In modo VIS segnale e dark condividono il canale 2, quindi il coefficiente OTP è
comune ai due termini e si semplifica nella differenza. Su CLEAR no: si applica
il coefficiente del **canale 6**, non quello del canale 2, e il resto lo assorbe
Kdark — che va rimisurato, e viene più grande.

⚠️ **`lux clamp` non agisce più simmetricamente.** CH1..CH5 stanno sotto
`DC_CLAMP_EN`, CH6 sotto `AC_CLAMP_EN`: in modo CLEAR il comando muove il **dark**
e lascia intatto il **clear**, dove in modo VIS muoveva i due termini insieme.

Nota positiva: `SEL_PD` muove solo il canale 2, quindi in modo CLEAR il canale 6
**integra indisturbato per tutto il rinfresco del dark**. Un frame a cavallo
della commutazione può sporcare il dark, mai il clear.

Sintassi:

```
lux meas                        stampa i parametri in vigore
lux meas <div> [discard]        arma il modo su ENTRAMBI i sensori
lux meas_cl <div> [discard]     la stessa cosa, correggendo CLEAR
```

Non esiste un `lux meas off`: il modo appartiene a una **corsa**, non alla
configurazione, e si spegne con `lux stop`. `div`, `discard` e Kdark restano
memorizzati.

In questo modo la tabella cambia forma (§2.9): non avrebbe senso stampare sei
canali quando la riga esiste per un numero solo. Il marcatore **`D`** segnala le
righe il cui dark è stato appena rimisurato — l'unica informazione che non si
ricava dai numeri. Un `NDARK` che resta identico per dieci righe e poi cambia
sulla riga marcata è il comportamento atteso, non un sensore bloccato.

### 2.8 I comandi

```
lux start [1|2]      avvia (senza numero: entrambi)
lux stop  [1|2]      ferma
lux cal              misura k = VIS/(R+G+B) - RICHIEDE BANCO ILLUMINATO
lux kdark            mostra Kdark dei due sensori
lux kdark <1|2> <v>  imposta Kdark di UN sensore, x1000
lux kdark cal        MISURA Kdark su entrambi - RICHIEDE SCATOLA BUIA
lux dark <on|off>    canale 2 sui fotodiodi coperti (entrambi)
lux meas <div> [discard]     acquisizione mista, corregge VIS  (§2.7)
lux meas_cl <div> [discard]  la stessa cosa su CLEAR           (§2.7)
lux lx               lux e temperatura colore da R,G,B, lettura immediata
lux period <ms>      pausa fra una misura e l'altra (0 = back-to-back)
lux clamp <on|off>   clamp DC sul canale 2
lux setup            menu interattivo, agisce su ENTRAMBI
lux otp <1|2>        calibrazione di fabbrica e UID letti dalla parte
lux probe <1|2> [ch] sweep dei bit di SEL_PD su un canale
lux bytes <1|2>      i 4 byte grezzi di ogni gruppo di canale (diagnostico)
lux head             ristampa l'intestazione delle colonne
map                  mappa di cablaggio sensore-connettore
scan                 sonda ogni indirizzo I2C sui due bus
reg <addr> [val]     accesso grezzo ai registri, hex (sensore B)
show                 stato, configurazione e contatori
raw                  una lettura immediata da entrambi
wire                 prova di continuità sui pin I2C (sensori fermi)
```

Il numero del sensore serve dove la risposta è **per parte** (`otp`, `probe`,
`kdark`) o dove si vuole isolare un bus (`start`, `stop`). Dove la
configurazione è comune (`setup`, `dark`, `cal`, `period`, `clamp`) il numero è
accettato ma non seleziona nulla.

⚠️ **`lux start` non calibra più** (fino al 2026-09-02 lo faceva alla prima
esecuzione della sessione). Al buio `R+G+B` è zero, la condizione di accumulo
non si verifica mai, `cal_req` non scende e la calibrazione **non finisce**:
finché è in corso l'autogain resta bloccato, quindi il sensore non può nemmeno
raggiungere le impostazioni che gli darebbero qualcosa da calibrare. Visto
sull'hardware come sedici e più righe di zeri a 1,00x, tutte marcate `CAL`, con
il guadagno fermo e nessun messaggio a dirlo. `k` si misura solo con `lux cal`,
che ora si arrende dopo un budget di campioni inutilizzabili invece di
appendersi.

⚠️ **Le due calibrazioni chiedono condizioni opposte**, ed è facile confonderle:

| comando | misura | serve |
|---|---|---|
| `lux cal` | `k = VIS/(R+G+B)` | **banco illuminato** e stabile |
| `lux kdark cal` | `Kdark = CLEAR/DARK` | **scatola chiusa** |

Al buio `k` degenera a 1/3 — misurato 0,374 in scatola contro ~1,07 sotto luce
vera — perché con VIS, R, G e B che portano tutti solo corrente di buio il
rapporto diventa "un canale su tre" e non descrive più alcuno spettro.

Il menu di `lux setup`:

```
 1) exposure       1..1636 ms
 2) gain           indice 0..14 (12 = 1,00x)
 3) otp correction on/off - per il confronto A/B
 4) autogain       on/off
 5) dark mode      canale 2 sui diodi coperti
 6) reset to defaults
 7) show otp details
 8) quit
```

`h` mostra l'aiuto, riga vuota annulla la domanda in corso, `q` esce sempre.

### 2.9 Il formato tabellare

Una riga per campione, colonne a larghezza fissa, pensate per essere incollate
in un foglio di calcolo:

```
S   SEQ      RED      VIS    GREEN     BLUE       IR    CLEAR     DARK    D/V     NVIS    NDARK %FS SAT GAINx  EXP DROP ADJ
A   779  1581825  4675231  2326019   788996   410411 12898928     1373     29  2089488      613  74  --  2.50  358    0  --
```

| colonna | significato |
|---|---|
| `S` | A o B — la **posizione di bus**, non il pezzo (l'UID è in `lux otp`) |
| `SEQ` | numero progressivo della misura |
| `RED`..`CLEAR` | i canali, corretti OTP |
| `VIS` | misurato, oppure ricostruito in dark mode |
| `DARK` / `CVIS` | il canale 2 nell'altro ruolo (§2.6) |
| `D/V` | dark / visibile, in **parti per 100000** |
| `NVIS` `NDARK` | gli stessi, normalizzati a 1,00x e all'esposizione di riferimento |
| `%FS` | percentuale del livello di saturazione, sul canale più vicino al tetto |
| `SAT` | 1 se un canale è bloccato al tetto |
| `GAINx` | guadagno **reale** (non l'indice) |
| `EXP` | esposizione in ms |
| `DROP` | campioni persi dalla coda |
| `ADJ` | `1` se l'autogain è intervenuto dopo questa riga, `S` se questa riga **attraversa** un intervento, `CAL` durante la calibrazione |

⚠️ **`D/V` è in parti per 100000.** Il per mille non ha risoluzione dove serve:
in luce ordinaria il rapporto vale 0,29 per mille e si stamperebbe come `0`.
Resta un indicatore diagnostico, non una grandezza compensata (§1.7).

⚠️ **`S` nella colonna `ADJ`** marca la riga che ha attraversato un cambio di
guadagno o esposizione. `ADJ=1` sta sulla riga *precedente* — quella dopo la
quale l'autogain è intervenuto — quindi la riga contaminata, che ha integrato
metà con le vecchie impostazioni, è la **successiva**. Si riconosce anche a
occhio: un `NDARK` di 878 fra vicini che leggono 647-664. **Va esclusa da
qualunque fit.**

`k` e la nota `OTP-OFF` non sono colonne ma stanno nell'intestazione `#`, per
sensore: sono costanti per tutta la cattura, e la riga ha un tetto di **127
caratteri** che le otto cifre per canale riempiono quasi del tutto.

#### La tabella dell'acquisizione mista

In acquisizione mista (§2.7) le colonne sono altre, e molte meno: quel modo
esiste per produrre **un** numero, e le sei colonne per canale lo
seppellirebbero.

```
S   SEQ      mLUX     NCLR %FS    NDARK  KDARK GAINx  EXP MRK
A    38        85        5   0      232    905 66.67 1636
A    19        85       -6   0      243    905 66.67 1636   D
```

| colonna | significato |
|---|---|
| `mLUX` | lux × 1000 da R,G,B (§2.12). Indipendente da Kdark e dal dark |
| `NVIS` / `NCLR` | il residuo normalizzato: `segnale − Kdark × dark`. Il nome dice quale canale è stato corretto — `NCLR` per `lux meas_cl` |
| `%FS` | percentuale del fondo scala normalizzato |
| `NDARK` | il dark usato per questa riga, normalizzato |
| `KDARK` | il coefficiente in forza, ×1000 — **per sensore** |
| `MRK` | `!` `X` `S` `D`, in quest'ordine di priorità |

⚠️ **La colonna del residuo ha un SEGNO.** Fino al 2026-09-02 era bloccata a
zero e la riga marcata `!`. Aveva senso finché Kdark era una supposizione: un
risultato negativo voleva dire coefficiente troppo alto. Ma dopo `lux kdark cal`
la correzione è tarata per rendere quel numero **zero al buio**, quindi al
pavimento è rumore attorno allo zero e **metà è negativa per costruzione**.
Bloccarla buttava via quella metà — cioè proprio il rumore che una soglia deve
superare — e marcava ogni riga. Misurato: 22 righe su 22 di una cattura a due
sensori, contro una probabilità di 2,4e-07 se fosse rumore centrato.

⚠️ **Di conseguenza `!` ha cambiato significato**: non più "la sottrazione è
andata sotto zero", ma "il residuo sta più del **5%** sotto lo zero rispetto a
quanto è stato sottratto", cioè Kdark è davvero fuori taratura per quel pezzo.
La soglia è una frazione del termine sottratto e non un numero di conteggi,
perché lo stesso errore su Kdark produce un residuo proporzionale al dark: un
valore assoluto scatterebbe ad alto guadagno e tacerebbe a guadagno basso. Il 5%
sono circa due sigma del canale peggiore.

Su una cattura di riferimento al buio (50 righe, due sensori, `div = 1`) il
residuo ha dato media −0,48 e −1,28 con sigma 2,7 e 2,3, segni distribuiti da
entrambe le parti, e **zero marcatori `!`**.

⚠️ **Il rumore del residuo è quasi tutto del DARK, non del segnale.** La
correlazione fra `NCLR` e `NDARK` sulla stessa cattura è `r = −0,94` (A) e
`−0,90` (B), con pendenza pari a `−Kdark × coefficiente OTP del canale 6` — cioè
esattamente il coefficiente della sottrazione. Tolto il contributo del dark, la
sigma scende da 2,7 a **0,89** e da 2,3 a **0,99**: il dark spiega l'89% e l'81%
della varianza. Il CLEAR è tre volte più silenzioso della correzione che gli si
applica.

Mediare il dark ridurrebbe quel rumore di √N, ma introdurrebbe un **bias**
proporzionale alla velocità di deriva termica — e la corrente di buio raddoppia
ogni 8-10 K, quindi in un transitorio vero il bias domina. Filtrare il
**residuo** invece del dark non ha quel difetto (la cancellazione della
temperatura avviene per campione, prima del filtro) ma ritarda il segnale, che
per un allarme è il difetto peggiore. Per questo **non è filtrato nulla**: la
sigma qui sopra è il pavimento con cui una soglia deve fare i conti.

Scelte che rendono il formato importabile:

- **`--` invece di 0** dove un valore non esiste: nel foglio la cella resta
  vuota anziché valere zero;
- **`SAT` e `ADJ` sono colonne**, non note in coda: una nota dopo l'ultimo campo
  è invisibile a un import a larghezza fissa, e sono proprio le righe da
  filtrare;
- **VIS e DARK sono due colonne distinte** anche se sono lo stesso canale: una
  colonna che cambia significato a metà cattura verrebbe mediata dal foglio;
- le righe che iniziano con `#` sono **commenti** con la configurazione, e ogni
  strumento di import le scarta.

### 2.10 Prendere una misura ripetibile

Non una procedura per un esperimento particolare, ma le condizioni sotto cui una
cattura di questa applicazione significa qualcosa. Vengono tutte da catture in
cui, non rispettandole, i numeri sono risultati sbagliati.

**Fissa le impostazioni.** `lux setup` → autogain **off**, guadagno ed
esposizione scelti una volta. Con l'autogain acceso i due sensori finiscono su
impostazioni diverse (e restano lì quando lo si spegne), ogni suo intervento
sporca una riga, e attraversando i 33,33x cambia il nibble OTP con un gradino
fino al 18% sulle colonne assolute (§1.11).

**Verifica che i due sensori siano davvero uguali.** Il menu segnala con `!!` se
l'altro ha guadagno o esposizione diversi. Un confronto A/B fra configurazioni
diverse misura le configurazioni, non i sensori.

**Tieni la luce ferma, e sappi che cosa la sta illuminando.** Non basta che sia
costante in intensità: se cambia **sorgente** cambia lo spettro, e con esso `k`,
il rapporto clear/RGB dell'autogain e la frazione di trafilamento. In una
cattura reale, coprendo e scoprendo i sensori, il blu è salito di 7 volte mentre
l'IR scendeva di 3 — e da quel punto in poi `k`, tarato prima, non valeva più.

**Attendi che la lampada si stabilizzi** prima di `lux cal`: `k` ha continuato a
muoversi per un centinaio di campioni dopo l'accensione. Lo scarto riportato
dalla calibrazione è il controllo: se è largo, la luce non era ferma.

**Verifica il metodo prima di fidartene.** Con dark spento, confronta `VIS` e
`CVIS`: devono differire di pochi per mille. Su una cattura a luce stabile sono
risultati 500580 contro 500423, cioè lo 0,03%.

**Scarta le righe marcate.** `SAT=1` (un canale al tetto: dice *"almeno fondo
scala"* e nient'altro), `ADJ=S` (riga a cavallo di un cambio di impostazioni),
`ADJ=CAL` (righe della calibrazione). `DROP` che cresce dice che la stampa non
sta al passo e la cattura ha buchi che `SEQ` da solo non rivela.

**Per coprire un sensore usa qualcosa di opaco e neutro.** Un dito non è un
coperchio: trasmette il rosso e l'infrarosso (misurato 0,14% nel blu, 5,6%
nell'IR — la finestra ottica del tessuto) e per giunta è caldo, quindi perturba
proprio l'offset che si sta cercando di leggere.

**⚠️ Tieni i LED della scheda fuori dalla scatola, o spenti.** `EN_LED` è a **0**
in questa applicazione, ed è una scelta, non una dimenticanza. I task
commutavano LED1 e LED2 a ogni campione: dentro una scatola chiusa con i
sensori, quella è una sorgente di luce che si accende e si spegne **in
controfase con la misura**.

Misurato il 2026-09-02 con `lux meas_cl` a 1,00x/398 ms, 175 campioni: la
colonna del segnale alternava sulla **parità dell'indice di campione** e vi
restava agganciata per tutta la corsa.

```
             livello    delta pari-dispari
  B scoperto   6796           1161
  B in scatola 3036           1075        livello −55%, delta −7%
```

Il contributo è **additivo** — una luce costante che si aggiunge — dove un
guasto di guadagno, esposizione o handshake avrebbe scalato con la luce. Il
dettaglio che chiude il ragionamento: le righe marcate `D` leggevano **più alte**
delle altre righe pari (11304 contro ~10675), perché un rinfresco del dark fa
passare quattro frame che non accodano campioni e quindi **non commutano il
LED** — la finestra subito dopo integra con il LED fermo. Nient'altro lega
l'ampiezza dell'alternanza al numero di frame saltati.

Se serve un indicatore di vita durante una cattura, dev'essere qualcosa che il
sensore non può vedere: un contatore sulla console, non un diodo nella scatola.

**Colonne normalizzate o grezze.** `NVIS`/`NDARK` sono riportate a 1,00x e
all'esposizione di riferimento, quindi restano confrontabili fra impostazioni e
fra sensori. Le colonne grezze servono quando interessa la scala reale dell'ADC,
per esempio per giudicare la vicinanza al tetto.

### 2.11 Una sessione reale

Il file `doc/logs/session_u575_2026-08-31.txt`, riprodotto per intero
nell'appendice, riporta una cattura completa su NUCLEO-U575ZI-Q. Percorre i tre
modi nell'ordine in cui conviene usarli: VIS misurato con autogain e la luce
variata a mano, poi il dark mode con la ricostruzione, poi l'acquisizione mista.
Vale la pena leggerci sei cose.

**I due esemplari hanno calibrazioni diverse** — è la ragione d'essere della
lettura OTP:

```
uid ade869b895218f:  hf_trim 0x0F6   red x1.093  green x1.093  clear x1.031
uid a87bd2032e9a34:  hf_trim 0x0EE   red x1.000  green x1.062  clear x1.031
```

Su `red` c'è il **9,3%** di scarto fra le due parti, e i due `hf_trim`
differiscono di 8 passi — nessuno dei due è il valore nominale 0x0E3. ⚠️ In
questa cattura il primo dei due si trova sul bus **A**; in una cattura
precedente era sul bus B. È il motivo per cui il pezzo si identifica per UID
(§1.11).

**`k` non è una costante.** Qui vale 0,986 su A e 1,000 su B, con uno scarto di
appena lo 0,1% su 16 campioni; sotto altre luci gli stessi due pezzi hanno dato
1,037 e 1,061. Segue lo spettro (§2.6): va misurato sotto la luce che si userà,
e lo scarto riportato dalla calibrazione dice se la luce era ferma.

**La normalizzazione regge un cambio di scala.** Alla riga 15 il sensore A gira
a 1,00x e 398 ms con `NVIS = 2011481`; due righe dopo l'autogain lo ha portato a
2,50x e 332 ms, e `NVIS` legge 2020990. Fra le due c'è lo **0,5%**, attraverso
un cambio di guadagno di 2,5 volte e uno di esposizione: è esattamente ciò che
le colonne normalizzate devono fare, ed è il controllo da rifare quando se ne
sospetta una.

**L'autogain diverge fra i due sensori, ed è corretto così.** Nelle righe 32-38
A gira a 33,33x/411 ms e B a 25,00x/412 ms; più avanti A sale a 66,67x mentre B
resta a 33,33x. Ciascuno reagisce alla luce che riceve davvero. I marcatori si
leggono in coppia: `ADJ=1` sulla riga **dopo** la quale l'autogain è
intervenuto, `ADJ=S` sulla riga successiva, che ha integrato a cavallo delle due
impostazioni ed è quella **da scartare**.

**Il dark si vede solo finché c'è luce.** In dark mode, a 1,00x e 398 ms, il
`NDARK` sta fra 440 e 530 su entrambi i pezzi con `D/V` fra 22 e 35 parti per
100000. Quando la luce cala la colonna passa a `0`, e `NDARK` diventa `--`: a
guadagno unitario il fondo di queste parti sta **sotto un conteggio** (§1.6).
Non è un guasto ed è la ragione per cui una misura di dark seria si fa a
guadagno alto.

**I due metodi danno lo stesso dark.** Le righe finali sono in acquisizione
mista (`lux meas 10 1000`): `NDARK` vale 480 su A e 421 su B, resta fermo per
dieci righe e cambia solo su quelle marcate `D`. Sono gli stessi valori che il
dark mode leggeva poche decine di righe prima, ottenuti però **senza rinunciare
a VIS misurato** — che è il motivo per cui il modo misto esiste.

⚠️ **Qualche riga della cattura è corrotta dall'eco della tastiera.** Righe come
`lA   113 ...` o `19788t37` sono il comando digitato che si è interlacciato con
la stampa dei task. Sono un difetto noto della console, non del sensore: le
righe che ne portano traccia vanno scartate, perché un carattere finito dentro
un numero lo rende plausibile e falso.

Un principio che vale per chiunque legga questi dati: un canale bloccato al
tetto dice *"almeno fondo scala"* e nient'altro. Ogni calcolo che lo tratta come
un numero produce un risultato privo di senso, tanto più insidioso quanto più
sembra plausibile — è per questo che `SAT` è una colonna e non una nota in
coda.

---

### 2.12 Lux e temperatura colore

Fino al 2026-09-02 questa applicazione **non produceva lux**, nonostante il
prefisso dei comandi: riportava conteggi, corretti e normalizzati, ma conteggi.
La conversione fotometrica è portata da `STALS_LIB` del driver ST v2.3.0 e dal
suo esempio `als_cct_lux`, in tre passi:

```
1)  norm[c] = (100800 / esposizione_us) * conteggio[c] / guadagno
2)  [X Y Z] = M(3x3) * [R G B]
3)  lux = Y                        cct = McCamy(x, y)
```

con

```
M = |  0,205570   0,416700  -0,143816 |
    | -0,028752   0,506372  -0,120614 |
    | -0,552625   0,335866   0,494781 |
```

Tre cose meritano attenzione.

**`lux` è letteralmente `Y`.** `STALS_LIB_compute_lux()` prende X, Y, Z e
restituisce Y, castando gli altri due a `void`. Tutta la calibrazione assoluta
sta nella matrice.

**Non è una somma.** Le righe portano coefficienti **negativi** (−0,14, −0,03,
−0,12, −0,55), quindi nessun `k(R+G+B)` riproduce Y per alcun `k`: i canali
colore si sovrappongono in lunghezza d'onda e la matrice sottrae la
sovrapposizione. La terza riga è essenzialmente `−R +G +B`.

**Usa R, G e B — non VIS.** È la proprietà che la rende la strada giusta per
l'allarme: funziona invariata mentre il canale 2 è sui diodi coperti, quindi la
misura del dark e quella della luce smettono di contendersi lo stesso canale, e
la ricostruzione di §2.6 non serve.

⚠️ **La matrice è una calibrazione, non una costante della parte.** Sono i
coefficienti che ST distribuisce nel suo esempio, misurati sul proprio modulo con
il proprio vetrino. Danno l'ordine di grandezza giusto e un comportamento
**relativo** solido; il valore assoluto vale quanto le due ottiche si somigliano.
Il comando lo dichiara a ogni invocazione, e l'intestazione della cattura lo
scrive nella riga `# mLUX`.

L'implementazione è interamente **intera**: il kernel salva ormai un contesto FP,
ma una conversione fotometrica in un ciclo da 400 ms non è una buona ragione per
trascinarci dentro l'emulazione software della doppia precisione. Verificata
contro la versione `double` di ST su otto casi (luce diurna, lampada calda,
penombra, esposizione a un passo, quasi saturazione, spettro fuori gamut, tutto
zero): **lux entro lo 0,001%, CCT entro lo 0,05%** sopra 1 lx.

Due limiti trovati proprio da quel confronto, entrambi corretti:

- la scala interna era ×1024 e **azzerava la penombra**: una lettura di 12/30/9
  conteggi a 66,6x troncava a zero tutti i canali normalizzati, e la funzione
  rispondeva "nessun risultato" dove ST dà 0,0002 lx. Cioè falliva **proprio nel
  regime in cui vive l'allarme**. Ora è ×65536, con il limite di overflow della
  `i64` calcolato e imposto invece che assunto;
- ST controlla solo `X+Y+Z == 0` prima di dividere. Con una matrice a
  coefficienti negativi, al buio X, Y o la somma escono **negativi**, e dividere
  lì produce una cromaticità fuori diagramma e una CCT che sembra una misura.
  Ora è respinto.

Il lux è riportato in **millilux interi** — la stessa convenzione della colonna
`KDARK`, che porta un intero ×1000 — con una portata fino a 4,29e6 lx. Sotto
~0,001 lx la colonna legge 0 pur essendoci luce: è la risoluzione dell'unità, non
un errore.

#### Il pavimento del lux è corrente di buio

⚠️ La fotometria **non sottrae alcun dark**: sono R, G e B attraverso la matrice.
In una scatola buia a massima sensibilità quel numero è quindi la corrente di
buio dei tre canali colore. Misurato: **~0,077 lx (A) e ~0,066 lx (B)** a
66,67x/1636 ms. È il numero che una soglia deve superare per significare
qualcosa.

E si muove con la temperatura, perché corrente di buio è. Fra due catture a
distanza di minuti:

```
        NDARK               mLUX
  A   210 -> 236  +12,4%    77 -> 85  +10,4%
  B   162 -> 171   +5,6%    66 -> 68   +3,0%
```

Stessa direzione, ampiezza simile, su due pezzi con coefficienti diversi: è la
conferma che quel pavimento **è** il buio. Una soglia in lux piazzata vicino a
0,08 lx si sposterebbe del 10% per una deriva termica come questa.

### 2.13 La pausa fra le misure

`ALS_PERIOD` (§4.3.5) è il tempo di inattività che la parte inserisce fra una
misura e la successiva. È stato 0 dall'inizio — frame consecutivi, il passo
dettato dalla sola esposizione — che è giusto per un banco di misura e sbagliato
per ciò che questa applicazione deve diventare: un allarme di soglia non ha
bisogno di una lettura ogni 398 ms, e i frame che non prende sono il risparmio di
energia più a buon mercato che ci sia.

⚠️ È la **pausa**, non l'intervallo: la parte integra per `EXTIME`, poi resta
ferma per `ALS_PERIOD`, quindi il ritmo è la somma. Per questo è una manopola
separata dall'esposizione — che cambia anche il *significato* di ogni lettura
(tetto di saturazione, rumore), mentre la pausa cambia solo la frequenza.

```
lux period          mostra il valore in vigore e il ritmo che ne risulta
lux period <ms>     0..5227, passo 20,5 ms, applicato al prossimo start
```

Come `lux clamp`, si scrive in configurazione: va dato a sensori fermi.

## 3. Appendice — la sessione di riferimento

Cattura integrale su NUCLEO-U575ZI-Q del 2026-08-31, commentata in §2.11. Il
file sorgente è `doc/logs/session_u575_2026-08-31.txt`: quello che segue ne è
l'inclusione letterale, così il PDF è autosufficiente.

Le pagine seguenti sono in orizzontale: le righe della tabella sono larghe 124
caratteri e su A4 verticale sarebbero illeggibili.

<!--SESSIONLOG-->

---

## 4. Riferimenti incrociati

| argomento | file sorgente |
|---|---|
| Mappa registri, tipi, costanti | `vd6283_ctrl.h` |
| Calcoli OTP, autogain, saturazione | `vd6283_ctrl.c` |
| Ogni accesso al bus, bring-up, sweep | `vd6283_i2c.c` |
| Task, CLI, menu, stampa | `ca_os_main_app_50.c` |

Per cercare nella documentazione hardware:

```bash
python3 tools/lookup_ref.py vd6283 SATURATION      # §3.3.4 - e il pavimento, §3.3.3
python3 tools/lookup_ref.py vd6283 PHOTODIODE      # §8.1, Figura 17
python3 tools/lookup_ref.py vd6283 EXPOSURE        # §4.3.12 - la formula sbagliata
python3 tools/lookup_ref.py stals ENUM             # API STALS
```

Il codice sorgente di riferimento di ST è in `STSW-IMG302_DRV/` (driver v2.3.0);
gli esempi applicativi, da cui vengono la conversione fotometrica di §2.12 e la
convenzione di esposizione di §1.9, in `stm32_f401-V2.3.0/examples/`.

| grandezza | dove si misura | condizione |
|---|---|---|
| `k = VIS/(R+G+B)` | `lux cal` | banco illuminato e stabile |
| `Kdark = CLEAR/DARK` | `lux kdark cal` | scatola chiusa |
| coefficienti OTP | `lux otp <1\|2>` | nessuna |
| lux, CCT | `lux lx` | nessuna (assoluto non calibrato) |
