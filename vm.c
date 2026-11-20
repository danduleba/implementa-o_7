#include <stdio.h>
#include <stdlib.h>

enum { BYTES_POR_PAGINA = 256 };

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Uso: ./vm <endereco>\n");
        return EXIT_FAILURE;
    }

    char *fim;
    long endereco = strtol(argv[1], &fim, 10);
    if (fim == argv[1] || *fim != '\0' || endereco < 0 || endereco > 65535) {
        fprintf(stderr, "Informe um endereco inteiro de 0 a 65535.\n");
        return EXIT_FAILURE;
    }

    // O quociente conta as paginas completas antes do endereco.
    long pagina = endereco / BYTES_POR_PAGINA;
    // O resto indica a posicao do byte dentro da pagina.
    long deslocamento = endereco % BYTES_POR_PAGINA;

    printf("Pagina: %ld\nDeslocamento: %ld\n", pagina, deslocamento);
    return EXIT_SUCCESS;
}
