XSLTPROC = xsltproc
INSTALL_DIR = $(DESTDIR)/usr/share/doc/subversion
INSTALL = install

## You shouldn't normally need to edit anything below here.
SHELL = /bin/sh
SVNVERSION = svnversion

BOOK_TOP = .
BOOK_HTML_CHUNK_DIR = $(BOOK_DIR)/html-chunk
BOOK_DIR = ${BOOK_TOP}/book
BOOK_HTML_TARGET = $(BOOK_DIR)/svn-book.html
BOOK_HTML_CHUNK_TARGET = $(BOOK_HTML_CHUNK_DIR)/index.html  # index.html is created last
BOOK_PDF_TARGET = $(BOOK_DIR)/svn-book.pdf
BOOK_PS_TARGET = $(BOOK_DIR)/svn-book.ps
BOOK_FO_TARGET = $(BOOK_DIR)/svn-book.fo
BOOK_XML_SOURCE = $(BOOK_DIR)/book.xml
BOOK_VERSION_SOURCE =  $(BOOK_DIR)/version.xml
BOOK_ALL_SOURCE = $(BOOK_DIR)/*.xml
BOOK_IMAGES = $(BOOK_DIR)/images/*.png
BOOK_INSTALL_DIR = $(INSTALL_DIR)/book

MDOCS_DIR = ${BOOK_TOP}/misc-docs
MDOCS_HTML_TARGET = $(MDOCS_DIR)/misc-docs.html
MDOCS_PDF_TARGET = $(MDOCS_DIR)/misc-docs.pdf
MDOCS_PS_TARGET = $(MDOCS_DIR)/misc-docs.ps
MDOCS_FO_TARGET = $(MDOCS_DIR)/misc-docs.fo
MDOCS_XML_SOURCE = $(MDOCS_DIR)/misc-docs.xml
MDOCS_ALL_SOURCE = $(MDOCS_DIR)/*.xml
MDOCS_INSTALL_DIR = $(INSTALL_DIR)/misc-docs

XSL_FO = ${BOOK_TOP}/../../book/tools/fo-stylesheet.xsl
XSL_HTML = $(BOOK_TOP)/../../book/tools/html-stylesheet.xsl
XSL_HTML_CHUNK = $(BOOK_TOP)/../../book/tools/chunk-stylesheet.xsl

RUN_FOP = ${BOOK_TOP}/../../book/tools/bin/run-fop.sh

# Book xsltproc options for HTML output
# Note: --stringparam arguments no longer go here;
# see tools/html-stylesheet.xsl and tools/chunk-stylesheet.xsl
BOOK_HTML_XSLTPROC_OPTS =

# Book xsltproc options for PDF and PostScript output
# BOOK_PDF_XSLTPROC_OPTS = --stringparam page.height 9in --stringparam page.width 6.4in
# BOOK_PS_XSLTPROC_OPTS = --stringparam page.height 9in --stringparam page.width 6.4in

# Uncomment the following line if you'd like to print on A4 paper
BOOK_PDF_XSLTPROC_OPTS = --stringparam paper.type A4

all: all-html all-pdf all-ps

install: install-book install-misc-docs

all-html: book-html book-html-chunk misc-docs-html

all-pdf: book-pdf misc-docs-pdf

all-ps: book-ps misc-docs-ps

all-book: book-html book-html-chunk book-pdf book-ps

install-book: install-book-html install-book-html-chunk install-book-pdf install-book-ps

all-misc-docs: misc-docs-html misc-docs-pdf book-ps

install-misc-docs: install-misc-html install-misc-pdf \
                   install-misc-ps

clean: book-clean misc-docs-clean

$(BOOK_VERSION_SOURCE): book-version

book-version:
	@if $(SVNVERSION) . > /dev/null; then \
	echo '<!ENTITY svn.version "revisjon '`$(SVNVERSION) .`'">' > $(BOOK_VERSION_SOURCE).tmp; \
	else \
	echo '<!ENTITY svn.version "">' > $(BOOK_VERSION_SOURCE).tmp; \
	fi
	@if cmp -s $(BOOK_VERSION_SOURCE) $(BOOK_VERSION_SOURCE).tmp; then \
	rm $(BOOK_VERSION_SOURCE).tmp; \
	else \
	mv $(BOOK_VERSION_SOURCE).tmp $(BOOK_VERSION_SOURCE); \
	fi

book-html: $(BOOK_HTML_TARGET)

$(BOOK_HTML_TARGET): $(BOOK_ALL_SOURCE) $(BOOK_VERSION_SOURCE)
	$(XSLTPROC) $(BOOK_HTML_XSLTPROC_OPTS) \
           --output $(BOOK_HTML_TARGET) $(XSL_HTML) $(BOOK_XML_SOURCE)

book-html-chunk: $(BOOK_HTML_CHUNK_TARGET)

## This trailing slash is essential that xsltproc will output pages to the dir
$(BOOK_HTML_CHUNK_TARGET): $(BOOK_ALL_SOURCE) $(BOOK_VERSION_SOURCE) \
                           $(BOOK_DIR)/styles.css $(BOOK_IMAGES)
	mkdir -p $(BOOK_HTML_CHUNK_DIR)
	mkdir -p $(BOOK_HTML_CHUNK_DIR)/images
	$(XSLTPROC) $(BOOK_HTML_XSLTPROC_OPTS) \
           --output $(BOOK_HTML_CHUNK_DIR)/ \
	   $(XSL_HTML_CHUNK) $(BOOK_XML_SOURCE)
	cp $(BOOK_DIR)/styles.css $(BOOK_HTML_CHUNK_DIR)
	cp $(BOOK_IMAGES) $(BOOK_HTML_CHUNK_DIR)/images

book-pdf: $(BOOK_PDF_TARGET)

book-ps: $(BOOK_PS_TARGET)

$(BOOK_PDF_TARGET): $(BOOK_ALL_SOURCE) $(BOOK_VERSION_SOURCE) $(BOOK_IMAGES)
	$(XSLTPROC) $(BOOK_PDF_XSLTPROC_OPTS) \
	   --output $(BOOK_FO_TARGET) $(XSL_FO) $(BOOK_XML_SOURCE)
	$(RUN_FOP) $(BOOK_TOP) -fo $(BOOK_FO_TARGET) -pdf $(BOOK_PDF_TARGET)

$(BOOK_PS_TARGET): $(BOOK_ALL_SOURCE) $(BOOK_VERSION_SOURCE) $(BOOK_IMAGES)
	$(XSLTPROC) $(BOOK_PS_XSLTPROC_OPTS) \
	   --output $(BOOK_FO_TARGET) $(XSL_FO) $(BOOK_XML_SOURCE)
	$(RUN_FOP) $(BOOK_TOP) -fo $(BOOK_FO_TARGET) -ps $(BOOK_PS_TARGET)

$(BOOK_INSTALL_DIR):
	$(INSTALL) -d $(BOOK_INSTALL_DIR)

install-book-html: $(BOOK_HTML_TARGET)
	$(INSTALL) -d $(BOOK_INSTALL_DIR)/images
	$(INSTALL) $(BOOK_HTML_TARGET) $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_DIR)/styles.css $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_IMAGES) $(BOOK_INSTALL_DIR)/images

install-book-html-chunk: $(BOOK_HTML_CHUNK_TARGET)
	$(INSTALL) -d $(BOOK_INSTALL_DIR)/images
	$(INSTALL) $(BOOK_HTML_CHUNK_DIR)/*.html $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_DIR)/styles.css $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_IMAGES) $(BOOK_INSTALL_DIR)/images

install-book-pdf: $(BOOK_PDF_TARGET) $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_PDF_TARGET) $(BOOK_INSTALL_DIR)

install-book-ps: $(BOOK_PS_TARGET) $(BOOK_INSTALL_DIR)
	$(INSTALL) $(BOOK_PS_TARGET) $(BOOK_INSTALL_DIR)

book-clean:
	rm -f $(BOOK_VERSION_SOURCE)
	rm -f $(BOOK_HTML_TARGET) $(BOOK_FO_TARGET)
	rm -rf $(BOOK_HTML_CHUNK_DIR)
	rm -f $(BOOK_PDF_TARGET) $(BOOK_PS_TARGET)

misc-docs-html: $(MDOCS_HTML_TARGET)

$(MDOCS_HTML_TARGET): $(MDOCS_ALL_SOURCE)
	$(XSLTPROC) $(XSL_HTML) $(MDOCS_XML_SOURCE) > $(MDOCS_HTML_TARGET)

misc-docs-pdf: $(MDOCS_PDF_TARGET)

misc-docs-ps: $(MDOCS_PS_TARGET)

$(MDOCS_PDF_TARGET): $(MDOCS_ALL_SOURCE)
	$(XSLTPROC) $(XSL_FO) $(MDOCS_XML_SOURCE) > $(MDOCS_FO_TARGET)
	$(RUN_FOP) $(BOOK_TOP) -fo $(MDOCS_FO_TARGET) -pdf $(MDOCS_PDF_TARGET)

$(MDOCS_PS_TARGET): $(MDOCS_ALL_SOURCE)
	$(XSLTPROC) $(XSL_FO) $(MDOCS_XML_SOURCE) > $(MDOCS_FO_TARGET)
	$(RUN_FOP) $(BOOK_TOP) -fo $(MDOCS_FO_TARGET) -ps $(MDOCS_PS_TARGET)

misc-docs-clean:
	rm -f $(MDOCS_HTML_TARGET) $(MDOCS_FO_TARGET)
	rm -f $(MDOCS_PDF_TARGET) $(MDOCS_PS_TARGET)

$(MDOCS_INSTALL_DIR):
	$(INSTALL) -d $(MDOCS_INSTALL_DIR)

install-misc-html: $(MDOCS_HTML_TARGET) $(MDOCS_INSTALL_DIR)
	$(INSTALL) $(MDOCS_HTML_TARGET) $(MDOCS_INSTALL_DIR)

install-misc-pdf: $(MDOCS_PDF_TARGET) $(MDOCS_INSTALL_DIR)
	$(INSTALL) $(MDOCS_PDF_TARGET) $(MDOCS_INSTALL_DIR)

install-misc-ps: $(MDOCS_PS_TARGET) $(MDOCS_INSTALL_DIR)
	$(INSTALL) $(MDOCS_PS_TARGET) $(MDOCS_INSTALL_DIR)

# Stuff below here is added for building the Norwegian version.

COLLAB=http://svn.collab.net/repos/svn

BUILDTMP_DIR = build.tmp
BOOKFILES = appa.xml appb.xml appc.xml appd.xml book.xml ch00.xml \
            ch01.xml ch02.xml ch03.xml ch04.xml ch05.xml ch06.xml \
            ch07.xml ch08.xml ch09.xml colo.xml copyright.xml \
            foreword.xml glossary.xml

BDTMP = bd.tmp
BDFILES = book.xml foreword.xml ch00.xml ch01.xml ch02.xml ch03.xml \
          ch04.xml appa.xml ch05.xml ch06.xml ch07.xml ch08.xml \
          ch09.xml appb.xml appc.xml appd.xml copyright.xml colo.xml \
          glossary.xml

build: dircheck htmlbook copytobuild

dircheck:
	mkdir $(BUILDTMP_DIR)
	rmdir $(BUILDTMP_DIR)

htmlbook:
	$(MAKE) clean && $(MAKE) all-html

copytobuild:
	svn co http://svn.sunbase.org/repos/svnbook_nb/build $(BUILDTMP_DIR)
	find $(BUILDTMP_DIR) -type f | grep -v '/\.svn/' | xargs rm
	cp -a $(BOOK_HTML_CHUNK_DIR) $(BUILDTMP_DIR)/
	cp -a $(BOOK_HTML_TARGET) $(BUILDTMP_DIR)/
	cp -a $(BOOK_IMAGES) $(BUILDTMP_DIR)/images/
	cp -a $(BOOK_DIR)/styles.css $(BUILDTMP_DIR)/
	cp -a $(MDOCS_HTML_TARGET) $(BUILDTMP_DIR)/$(MDOCS_DIR)/
	cp -a $(BOOK_DIR)/styles.css $(BUILDTMP_DIR)/$(MDOCS_DIR)/

editmode:
	$(MAKE) convfiles CONV_PARAM=-e

commitmode:
	$(MAKE) convfiles CONV_PARAM=

convfiles:
	@for _prc in $(BOOKFILES); do \
		echo -n "$$_prc: convert..."; \
		dest=book/$$_prc; \
		swp=book/.$$_prc.swp; \
		if [ -e $$swp ]; then \
			echo "$$swp: Swap file found. Is the file being edited?" >&2; \
			exit 1; \
		fi; \
		bin/clean_files $(CONV_PARAM) $$dest >$$dest.tmp || exit 1; \
		echo -n "move..."; \
		mv $$dest.tmp $$dest; \
		echo "OK."; \
	done

bookdiff:
	mkdir $(BDTMP)
	svn co -N -r`cat LAST_UPDATED` $(COLLAB)/trunk/doc/book/book $(BDTMP)/eng
	for _ptd in $(BDFILES); do \
		cat $(BDTMP)/eng/$$_ptd | bin/clean_files -e >>$(BDTMP)/eng.txt; \
		cat book/$$_ptd | bin/clean_files -e >>$(BDTMP)/norw.txt; \
	done
	vimdiff $(BDTMP)/norw.txt $(BDTMP)/eng.txt
	@echo The temporary files are still in $(BDTMP)/ for you to play with.

sync:
	mkdir sync.LOCK
	svn update
	@echo ======= START svn status =======
	@svn status -q
	@echo ======== END svn status ========
	@echo If there are no local changes, press ENTER...
	@read
	svnversion . >HEADREV
	$(MAKE) editmode
	svn merge -r$$(cat LAST_UPDATED):$$(cat HEADREV) $(COLLAB)/trunk/doc/book .
	mv HEADREV LAST_UPDATED
	rmdir sync.LOCK

# $Id$
# vim: set ts=4 sw=4 sts=4 noet :
