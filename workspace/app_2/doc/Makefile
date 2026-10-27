# Makefile for the app_2 documentation (same scheme as workspace/app_50/doc)
#
#   make / make all   -> app_2_user_guide.pdf (Italian) and app_2_user_guide_en.pdf
#   make view         -> builds and opens the Italian PDF
#   make clean        -> removes PDFs and intermediate files
#
# The markdown goes through md_clean.py first: pdflatex stops on symbols
# outside Latin-1 (and inside listings even on accented letters).
#
# Dependencies: pandoc, texlive (pdflatex).
#   sudo apt install pandoc texlive-latex-recommended texlive-lang-italian

PANDOC    = pandoc
MKDIR     = mkdir -p
RM        = rm -f
BUILD_DIR = build

# Italian hyphenation only when babel has it (texlive-lang-italian): without
# it pdflatex stops, and a PDF without Italian hyphenation is still a PDF
LANG_IT := $(if $(shell kpsewhich italian.ldf 2>/dev/null),-V lang=it,)

PANDOC_OPTS = \
	--from=markdown+pipe_tables+backtick_code_blocks+yaml_metadata_block \
	--pdf-engine=pdflatex \
	--listings \
	--toc --toc-depth=3 \
	-V documentclass=article \
	-V papersize=a4 \
	-V geometry:margin=2.5cm \
	-V fontsize=10pt \
	-V colorlinks=true \
	-V linkcolor=black \
	-V urlcolor=blue \
	-V toccolor=black \
	-H header.tex

.PHONY: all view clean

all: app_2_user_guide.pdf app_2_user_guide_en.pdf

app_2_user_guide.pdf: app_2_user_guide.md md_clean.py header.tex
	$(MKDIR) $(BUILD_DIR)
	python3 md_clean.py $< $(BUILD_DIR)/$<
	$(PANDOC) $(BUILD_DIR)/$< $(PANDOC_OPTS) $(LANG_IT) --resource-path=.:$(BUILD_DIR) -o $@
	@echo "--> $@"

app_2_user_guide_en.pdf: app_2_user_guide_en.md md_clean.py header.tex
	$(MKDIR) $(BUILD_DIR)
	python3 md_clean.py $< $(BUILD_DIR)/$<
	$(PANDOC) $(BUILD_DIR)/$< $(PANDOC_OPTS) -V lang=en --resource-path=.:$(BUILD_DIR) -o $@
	@echo "--> $@"

view: app_2_user_guide.pdf
	@xdg-open app_2_user_guide.pdf 2>/dev/null || echo "open by hand: app_2_user_guide.pdf"

clean:
	$(RM) -r $(BUILD_DIR)
	$(RM) app_2_user_guide.pdf app_2_user_guide_en.pdf
