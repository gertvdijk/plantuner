subdir = contrib/plantuner
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = plantuner
DOCS = README.plantuner
REGRESS = plantuner
OBJS=plantuner.o

include $(top_srcdir)/contrib/contrib-global.mk
