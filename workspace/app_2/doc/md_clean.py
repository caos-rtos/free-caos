#!/usr/bin/env python3
"""Prepara un markdown per pdflatex (copia da workspace/app_50/doc, senza il log in appendice).

Due cose che pdflatex non digerisce e che questo filtro risolve:

1. I simboli Unicode problematici. Non solo quelli fuori dalla Latin-1: il
   punto centrale "·" e' Latin-1 valido, ma dentro \lstinline (modo verbatim)
   fa fallire pdflatex lo stesso. `inputenc` si ferma con un errore
   secco su ogni carattere non dichiarato, e il selettore di variazione emoji
   (U+FE0F, invisibile, che segue i simboli come ⚠️) e' il piu' insidioso
   perche' non si vede nel sorgente. Vengono sostituiti con equivalenti
   testuali, non rimossi: "⚠️" diventa "**!**", che nel PDF resta un
   avvertimento visibile.

2. I riferimenti ai diagrammi. Nel markdown i diagrammi sono citati come .dot
   o .py (il sorgente, che si legge e si versiona); qui diventano .pdf, che e'
   cio' che dot o matplotlib hanno prodotto e che LaTeX sa includere.

Usato dal Makefile, non a mano. L'alternativa sarebbe lualatex, che i simboli
li accetta ma poi non li trova nei font e li stampa come nulla - un
avvertimento che sparisce e' peggio di uno tradotto.
"""

import os
import re
import sys

# Sostituzioni testuali. L'ordine conta: la sequenza con il selettore di
# variazione va prima del simbolo nudo, o resterebbe il selettore orfano.
REPLACEMENTS = [
    ("⚠️", "**!**"),   # ⚠️ con variation selector
    ("⚠", "**!**"),         # ⚠ nudo
    ("✅", "**OK**"),        # ✅
    ("✓", "OK"),            # ✓
    ("✗", "NO"),            # ✗
    ("✔", "OK"),            # U+2714 - variante "heavy" di ✓
    ("✘", "NO"),            # U+2718 - variante "heavy" di ✗
    ("️", ""),              # variation selector orfano
    ("→", "->"),            # →
    ("←", "<-"),            # ←
    ("⇒", "=>"),            # ⇒
    ("×", "x"),             # ×
    ("≤", "<="),            # ≤
    ("≥", ">="),            # ≥
    ("−", "-"),             # U+2212 meno tipografico, non il trattino ASCII
    ("√", "radice di "),    # U+221A
    ("≠", "!="),            # ≠
    ("≈", "~"),             # ≈
    ("µ", "u"),             # µ (micro)
    ("·", "*"),             # · - listings non lo accetta in verbatim
    ("§", "par."),          # § - idem, e nei blocchi di codice e' frequente
    ("°C", " gradi C"),     # °C - prima del grado nudo, o resta "gradiC"
    ("°", " gradi"),        # °
    ("…", "..."),           # …
    ("—", "--"),            # — em dash
    ("–", "-"),             # – en dash
    ("“", '"'), ("”", '"'),
    ("‘", "'"), ("’", "'"),
    (" ", " "),             # spazio unificatore
]


# Dentro un blocco di codice `listings` lavora in verbatim e rifiuta anche gli
# accentati, che nel testo normale passano senza problemi. Translitterarli solo
# lì evita di imbruttire la prosa italiana.
ACCENTS = str.maketrans({
    "à": "a'", "è": "e'", "é": "e'", "ì": "i'", "ò": "o'", "ù": "u'",
    "À": "A'", "È": "E'", "É": "E'", "Ì": "I'", "Ò": "O'", "Ù": "U'",
})


def strip_accents_in_code(text):
    """Translittera gli accentati dentro i blocchi ``` e i code span."""
    out = []
    in_block = False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            in_block = not in_block
            out.append(line)
            continue
        if in_block:
            out.append(line.translate(ACCENTS))
        else:
            # code span inline: solo il contenuto fra backtick
            out.append(re.sub(r"`([^`\n]*)`",
                              lambda m: "`" + m.group(1).translate(ACCENTS) + "`",
                              line))
    return "\n".join(out)


def clean(text):
    for src, dst in REPLACEMENTS:
        text = text.replace(src, dst)

    text = strip_accents_in_code(text)

    # I diagrammi: nel markdown si cita il SORGENTE (.dot per graphviz, .py per
    # i grafici matplotlib), nel PDF serve il .pdf che il Makefile ne ricava.
    text = re.sub(r'(!\[[^\]]*\]\([^)]*?)\.(?:dot|py)(\))', r'\1.pdf\2', text)

    # Quello che resta fuori dalla Latin-1 fermerebbe comunque pdflatex. Gli
    # accentati (Latin-1) nel testo normale vanno bene e non sono segnalati.
    # Segnalarlo e' meglio che lasciarlo esplodere in mezzo a mille righe di
    # log LaTeX: qui si sa subito quale carattere e dove.
    leftover = {}
    for n, line in enumerate(text.split("\n"), 1):
        for ch in line:
            if ord(ch) > 0xFF:
                leftover.setdefault(ch, n)
    if leftover:
        for ch, line in sorted(leftover.items()):
            print("md_clean: U+%04X (%s) alla riga %d non ha sostituzione"
                  % (ord(ch), ch, line), file=sys.stderr)

    return text


def main():
    if len(sys.argv) != 3:
        print("uso: md_clean.py <input.md> <output.md>", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(clean(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
