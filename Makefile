CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all testar clean
all: vm

vm: vm.c vm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread vm.c -o vm

testar:
	python3 testes/verificar.py

clean:
	rm -f vm
