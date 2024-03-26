# Copyright 2024 Bull SAS

PREFIX=./build

OPENMP_CFLAGS = -DHAVE_OPENMP -fopenmp
OPENMP_LDFLAGS = -liomp5  # requres intel compilers
MPI_FLAGS = -DHAVE_MPI
OPTFLAGS = -g -O2

CFLAGS = -std=c99 $(OPTFLAGS) -fPIC -Werror -Wall 
CPPFLAGS = $(MPI_FLAGS) $(OPENMP_CFLAGS)
LDFLAGS = $(OPENMP_LDFLAGS) 

DEBUG?=0
TRACE?=0

ifeq (${DEBUG},1)
  CFLAGS += -D__DEBUG
endif

MPICC=mpicc

all: libmoc.so moc

install: all
	install -D libmoc.so $(PREFIX)/lib/libmoc.so
	install -D moc $(PREFIX)/bin/moc
	install -D doc/doc.rst $(PREFIX)/doc/doc.rst

%.o: %.c
	$(MPICC) ${CFLAGS} $(CPPFLAGS) -c $< -o $@

libmoc.so: moc.o
	$(MPICC) -shared -fPIC $(LDFLAGS) $^ -o $@

moc: moc_command.c
	 $(CC) -W -Wall -Wextra -o $@ $<


clean:
	$(RM) -f *.so moc *.o *~



