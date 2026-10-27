# Makefile per la documentazione di app_50
#
#   make / make all   -> app_50_user_guide.pdf
#   make diagrams     -> solo i .pdf dei diagrammi Graphviz
#   make view         -> compila e apre il PDF
#   make clean        -> rimuove PDF e file intermedi
#   make help         -> questo elenco
#
# Impostato come doc/Makefile del progetto: i .dot diventano .pdf con dot, il
# testo passa per pdflatex. La differenza e' che qui la sorgente e' markdown e
# non LaTeX, quindi ci si appoggia a pandoc.
#
# Dipendenze: pandoc, graphviz (dot), texlive (pdflatex).
#   sudo apt install pandoc graphviz texlive-latex-recommended texlive-lang-italian

PANDOC   = pandoc
DOT      = dot
MKDIR    = mkdir -p
RM       = rm -f

DOC       = app_50_user_guide
BUILD_DIR = build

# I diagrammi: sorgente versionato, .pdf generato. Due generatori, perche' due
# cose diverse: graphviz per gli schemi (chi parla con chi), matplotlib per i
# grafici quantitativi (una curva contro un asse).
DOT_SRCS = $(wildcard diagrams/*.dot)
DOT_PDFS = $(DOT_SRCS:.dot=.pdf)
PY_SRCS  = $(wildcard diagrams/*.py)
PY_PDFS  = $(PY_SRCS:.py=.pdf)

# Opzioni pandoc. --listings usa il pacchetto listings per i blocchi di codice
# (gli stessi che usa il manuale del progetto); il resto e' impaginazione.
PANDOC_OPTS = \
	--from=markdown+pipe_tables+backtick_code_blocks+yaml_metadata_block \
	--pdf-engine=pdflatex \
	--listings \
	--toc --toc-depth=3 \
	-V documentclass=article \
	-V papersize=a4 \
	-V geometry:margin=2.5cm \
	-V fontsize=10pt \
	-V lang=it \
	-V colorlinks=true \
	-V linkcolor=black \
	-V urlcolor=blue \
	-V toccolor=black \
	-H header.tex

.PHONY: all diagrams view clean help

all: $(DOC).pdf

diagrams: $(DOT_PDFS) $(PY_PDFS)

diagrams/%.pdf: diagrams/%.dot
	$(DOT) -Tpdf $< -o $@

diagrams/%.pdf: diagrams/%.py
	python3 $< $@

# Il markdown passa per md_clean.py prima di pandoc: pdflatex si ferma con un
# errore secco sui simboli fuori dalla Latin-1, e il piu' insidioso e' il
# selettore di variazione emoji (U+FE0F) che segue i simboli e non si vede nel
# sorgente. Il filtro li traduce in equivalenti testuali e rimappa i
# riferimenti ai diagrammi da .dot a .pdf.
LOG = logs/session_u575_2026-08-31.txt

$(DOC).pdf: $(DOC).md $(DOT_PDFS) $(PY_PDFS) md_clean.py header.tex $(LOG)
	$(MKDIR) $(BUILD_DIR)
	python3 md_clean.py $(DOC).md $(BUILD_DIR)/$(DOC).md
	# --resource-path: il markdown intermedio sta in build/, ma i diagrammi
	# restano in diagrams/ - senza questo LaTeX non li trova e impagina un
	# riquadro vuoto invece di fermarsi, che e' peggio di un errore.
	$(PANDOC) $(BUILD_DIR)/$(DOC).md $(PANDOC_OPTS) --resource-path=.:$(BUILD_DIR) -o $@
	@echo "--> $@"

view: $(DOC).pdf
	@xdg-open $(DOC).pdf 2>/dev/null || echo "apri a mano: $(DOC).pdf"

clean:
	$(RM) -r $(BUILD_DIR)
	$(RM) $(DOC).pdf
	$(RM) $(DOT_PDFS) $(PY_PDFS)

help:
	@echo "Target disponibili:"
	@echo "  all        - genera $(DOC).pdf (predefinito)"
	@echo "  diagrams   - genera solo i PDF dei diagrammi Graphviz"
	@echo "  view       - genera il PDF e lo apre"
	@echo "  clean      - rimuove PDF e file intermedi"
	@echo ""
	@echo "Diagrammi presenti:"
	@for f in $(DOT_SRCS) $(PY_SRCS); do echo "    $$f"; done
	@echo ""
	@echo "Dipendenze: pandoc, graphviz, matplotlib, texlive-latex-recommended"
