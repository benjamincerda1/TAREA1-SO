CFLAGS = -Wall -Wextra -std=c17 -g

planificador: planificador.c
	gcc $(CFLAGS) -o planificador planificador.c

clean:
	rm -f planificador
