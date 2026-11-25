#define _POSIX_C_SOURCE 200809L
#include "vm.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FRAMES
#define FRAMES 128
#endif
enum { PAGINAS = 256, BYTES_POR_PAGINA = 256, ENTRADAS_TLB = 16 };
enum { SEM_TLB, TLB_SEQUENCIAL, TLB_PARALELA };
#ifndef MODO_TLB
#define MODO_TLB TLB_PARALELA
#endif
typedef enum { FIFO, LRU } Politica;

typedef struct {
    int pagina, frame;
} Traducao;

typedef struct {
    uint16_t endereco;
    int posicao_tlb, fisico, valor;
} Acesso;

typedef struct {
    EntradaPagina tabela[PAGINAS];
    int8_t memoria[FRAMES][BYTES_POR_PAGINA];
    int pagina_do_frame[FRAMES];
    Traducao tlb[ENTRADAS_TLB];
    int proximo_fifo;
    int ordem_lru[FRAMES], ordem_tlb[ENTRADAS_TLB];
} Simulador;

static void erro(const char *mensagem)
{
    fprintf(stderr, "Erro: %s\n", mensagem);
    exit(EXIT_FAILURE);
}

static Politica ler_politica(const char *nome)
{
    if (strcmp(nome, "fifo") == 0) return FIFO;
    if (strcmp(nome, "lru") == 0) return LRU;
    erro("politica desconhecida; use fifo ou lru.");
    return FIFO;
}

// As filas guardam indices. Mover um indice conserva os bytes no mesmo frame.
static void mover_para_fim(int ordem[], int tamanho, int indice)
{
    int i = 0;
    while (ordem[i] != indice) i++;
    for (; i < tamanho - 1; i++)
        ordem[i] = ordem[i + 1];
    ordem[tamanho - 1] = indice;
}

static void iniciar(Simulador *s)
{
    for (int i = 0; i < FRAMES; i++) {
        s->pagina_do_frame[i] = -1;
        s->ordem_lru[i] = i;
    }
    for (int i = 0; i < ENTRADAS_TLB; i++) {
        s->tlb[i].pagina = -1;
        s->ordem_tlb[i] = i;
    }
}

// O arquivo pode separar os enderecos por espacos ou quebras de linha.
static bool ler_endereco(FILE *entrada, int *endereco)
{
    int c;
    do {
        c = fgetc(entrada);
    } while (isspace(c));
    if (ferror(entrada)) erro("falha de leitura no arquivo de enderecos.");
    if (c == EOF) return false;

    int valor = 0;
    while (c != EOF && !isspace(c)) {
        if (c < '0' || c > '9') erro("endereco deve ser um inteiro decimal.");
        valor = valor * 10 + (c - '0');
        if (valor >= PAGINAS * BYTES_POR_PAGINA) erro("endereco fora da faixa de 0 a 65535.");
        c = fgetc(entrada);
    }
    if (ferror(entrada)) erro("falha de leitura no arquivo de enderecos.");
    *endereco = valor;
    return true;
}

static void carregar_pagina(FILE *disco, int pagina, int8_t frame[])
{
    long inicio = (long)pagina * BYTES_POR_PAGINA;
    if (fseek(disco, inicio, SEEK_SET) != 0 ||
        fread(frame, 1, BYTES_POR_PAGINA, disco) != BYTES_POR_PAGINA)
        erro("nao foi possivel ler a pagina de BACKING_STORE.bin.");
}

static int resolver_falta(Simulador *s, FILE *disco, int pagina, Politica politica)
{
    // FIFO percorre os frames em circulo. Em LRU, a fila comeca pelo menos recente.
    int frame = (politica == FIFO) ? s->proximo_fifo : s->ordem_lru[0];
    carregar_pagina(disco, pagina, s->memoria[frame]);

    int anterior = s->pagina_do_frame[frame];
    if (anterior >= 0) {
        s->tabela[anterior].presente = false;
        for (int i = 0; i < ENTRADAS_TLB; i++)
            if (s->tlb[i].pagina == anterior) s->tlb[i].pagina = -1;
    }
    s->pagina_do_frame[frame] = pagina;
    s->tabela[pagina] = (EntradaPagina){ .frame = frame, .presente = true };
    if (politica == FIFO) s->proximo_fifo = (frame + 1) % FRAMES;
    return frame;
}

typedef struct {
    int pagina_da_entrada;
    int procurada;
} Consulta;

static void *comparar(void *arg)
{
    Consulta *consulta = arg;
    if (consulta->pagina_da_entrada == consulta->procurada) return consulta;
    return NULL;
}

static int procurar_na_tlb(const Traducao tlb[], int pagina)
{
    if (MODO_TLB == SEM_TLB) return -1;
    if (MODO_TLB == TLB_SEQUENCIAL) {
        for (int i = 0; i < ENTRADAS_TLB; i++)
            if (tlb[i].pagina == pagina) return i;
        return -1;
    }

    Consulta consultas[ENTRADAS_TLB];
    pthread_t threads[ENTRADAS_TLB];
    for (int i = 0; i < ENTRADAS_TLB; i++) {
        consultas[i] = (Consulta){tlb[i].pagina, pagina};
        if (pthread_create(&threads[i], NULL, comparar, &consultas[i]) != 0)
            erro("falha ao criar thread da TLB.");
    }
    int posicao = -1;
    for (int i = 0; i < ENTRADAS_TLB; i++) {
        void *resposta;
        if (pthread_join(threads[i], &resposta) != 0) erro("falha ao aguardar thread da TLB.");
        if (resposta != NULL) posicao = i;
    }
    return posicao;
}

static int inserir_na_tlb(Simulador *s, int pagina, int frame)
{
    int posicao = s->ordem_tlb[0];
    for (int i = 0; i < ENTRADAS_TLB; i++) {
        if (s->tlb[i].pagina < 0) {
            posicao = i;
            break;
        }
    }
    s->tlb[posicao] = (Traducao){pagina, frame};
    mover_para_fim(s->ordem_tlb, ENTRADAS_TLB, posicao);
    return posicao;
}

static void traduzir(Simulador *s, Acesso *acesso, FILE *disco,
                     Politica memoria, Politica tlb)
{
    int pagina = acesso->endereco / BYTES_POR_PAGINA;
    int deslocamento = acesso->endereco % BYTES_POR_PAGINA;
    int posicao = procurar_na_tlb(s->tlb, pagina);
    int frame;
    if (posicao >= 0) {
        frame = s->tlb[posicao].frame;
        if (tlb == LRU) mover_para_fim(s->ordem_tlb, ENTRADAS_TLB, posicao);
    } else {
        if (s->tabela[pagina].presente) {
            frame = s->tabela[pagina].frame;
        } else {
            frame = resolver_falta(s, disco, pagina, memoria);
        }
        if (MODO_TLB != SEM_TLB) posicao = inserir_na_tlb(s, pagina, frame);
    }
    if (memoria == LRU) mover_para_fim(s->ordem_lru, FRAMES, frame);
    acesso->posicao_tlb = posicao;
    acesso->fisico = frame * BYTES_POR_PAGINA + deslocamento;
    acesso->valor = s->memoria[frame][deslocamento];
}

static Acesso *ler_enderecos(FILE *entrada, size_t *quantidade)
{
    size_t capacidade = 0;
    Acesso *acessos = NULL;
    int endereco;
    while (ler_endereco(entrada, &endereco)) {
        if (*quantidade == capacidade) {
            if (capacidade > SIZE_MAX / 2 / sizeof(*acessos))
                erro("arquivo de enderecos muito grande.");
            capacidade = capacidade ? capacidade * 2 : 256;
            Acesso *novos = realloc(acessos, capacidade * sizeof(*acessos));
            if (!novos) erro("memoria insuficiente para os enderecos.");
            acessos = novos;
        }
        acessos[*quantidade].endereco = (uint16_t) endereco;
        (*quantidade)++;
    }
    if (*quantidade == 0) erro("arquivo de enderecos vazio.");
    return acessos;
}

static void escrever_traducoes(FILE *saida, const Acesso *acessos, size_t quantidade)
{
    for (size_t i = 0; i < quantidade; i++) {
        const Acesso *a = &acessos[i];
        if (fprintf(saida, "Virtual address: %d TLB: %d Physical address: %d Value: %d\n",
                    (int)a->endereco, a->posicao_tlb, a->fisico, a->valor) < 0)
            erro("falha ao escrever a traducao.");
    }
}

int main(int argc, char *argv[])
{
    if (argc != 4) erro("uso: ./vm <arquivo de enderecos> <fifo|lru> <fifo|lru>");
    Politica memoria = ler_politica(argv[2]);
    Politica tlb = ler_politica(argv[3]);
    FILE *entrada = fopen(argv[1], "r");
    if (!entrada) erro("nao foi possivel abrir o arquivo de enderecos.");
    FILE *disco = fopen("BACKING_STORE.bin", "rb");
    if (!disco) erro("nao foi possivel abrir BACKING_STORE.bin.");

    size_t quantidade = 0;
    Acesso *acessos = ler_enderecos(entrada, &quantidade);
    fclose(entrada);

    Simulador simulador = {0};
    iniciar(&simulador);
    for (size_t i = 0; i < quantidade; i++)
        traduzir(&simulador, &acessos[i], disco, memoria, tlb);
    fclose(disco);

    escrever_traducoes(stdout, acessos, quantidade);
    if (fflush(stdout) == EOF) erro("falha ao enviar as traducoes.");

    FILE *arquivo = fopen("correct.txt", "w");
    if (arquivo) {
        escrever_traducoes(arquivo, acessos, quantidade);
        if (fclose(arquivo) != 0) erro("falha ao concluir correct.txt.");
    } else if (errno != EACCES && errno != EROFS) {
        erro("nao foi possivel abrir correct.txt para escrita.");
    }
    free(acessos);
    return EXIT_SUCCESS;
}
