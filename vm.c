#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

enum { PAGINAS = 256, BYTES_POR_PAGINA = 256 };

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Uso: ./vm <endereco>\n");
        return EXIT_FAILURE;
    }

    char *fim;
    long endereco = strtol(argv[1], &fim, 10);
    if (fim == argv[1] || *fim != '\0' || endereco < 0 ||
        endereco >= PAGINAS * BYTES_POR_PAGINA) {
        fprintf(stderr, "Informe um endereco inteiro de 0 a 65535.\n");
        return EXIT_FAILURE;
    }

    long pagina = endereco / BYTES_POR_PAGINA;
    long deslocamento = endereco % BYTES_POR_PAGINA;

    // Todas as paginas comecam ausentes. Este exemplo simula a pagina 66 no frame 0.
    EntradaPagina tabela[PAGINAS] = {0};
    tabela[66] = (EntradaPagina){ .frame = 0, .presente = true };

    printf("Pagina: %ld\nDeslocamento: %ld\n", pagina, deslocamento);

    if (!tabela[pagina].presente) {
        printf("Falta de pagina: %ld ainda nao esta na memoria.\n", pagina);
        return EXIT_SUCCESS;
    }

    int frame = tabela[pagina].frame;
    long fisico = frame * BYTES_POR_PAGINA + deslocamento;
    printf("Frame: %d\nEndereco fisico: %ld\n", frame, fisico);

    return EXIT_SUCCESS;
}
