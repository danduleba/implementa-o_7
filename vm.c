#define _POSIX_C_SOURCE 200809L
#include "vm.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    EntradaPagina tabela[PAGINAS];
    int8_t memoria[FRAMES][BYTES_POR_PAGINA];
    int pagina_do_frame[FRAMES];
    Traducao tlb[ENTRADAS_TLB];
    int proximo_fifo;
    int ordem_lru[FRAMES], ordem_tlb[ENTRADAS_TLB];
    unsigned long long acessos, faltas, acertos;
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
    s->faltas++;
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

static void traduzir(Simulador *s, int endereco, FILE *disco, FILE *saida,
                     Politica memoria, Politica tlb)
{
    int pagina = endereco / BYTES_POR_PAGINA;
    int deslocamento = endereco % BYTES_POR_PAGINA;
    int posicao = procurar_na_tlb(s->tlb, pagina);
    int frame;
    if (posicao >= 0) {
        frame = s->tlb[posicao].frame;
        s->acertos++;
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
    s->acessos++;
    int fisico = frame * BYTES_POR_PAGINA + deslocamento;
    if (fprintf(saida, "Virtual address: %d TLB: %d Physical address: %d Value: %d\n",
                endereco, posicao, fisico, s->memoria[frame][deslocamento]) < 0)
        erro("falha ao escrever a traducao.");
}

static char temporario[] = "correct.txt.XXXXXX";
static void limpar_temporario(void)
{
    if (temporario[0]) remove(temporario);
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

    int endereco;
    if (!ler_endereco(entrada, &endereco)) erro("arquivo de enderecos vazio.");
    int fd = mkstemp(temporario);
    if (fd < 0) erro("nao foi possivel criar a saida.");
    if (atexit(limpar_temporario) != 0) {
        limpar_temporario();
        erro("falha ao preparar a saida.");
    }
    FILE *saida = fdopen(fd, "w");
    if (!saida) erro("nao foi possivel abrir a saida.");

    Simulador simulador = {0};
    iniciar(&simulador);
    do {
        traduzir(&simulador, endereco, disco, saida, memoria, tlb);
    } while (ler_endereco(entrada, &endereco));

    fprintf(saida, "Number of Translated Addresses = %llu\nPage Faults = %llu\n"
            "Page Fault Rate = %.3f\nTLB Hits = %llu\nTLB Hit Rate = %.3f\n",
            simulador.acessos, simulador.faltas, (double)simulador.faltas / simulador.acessos,
            simulador.acertos, (double)simulador.acertos / simulador.acessos);
    fclose(entrada);
    fclose(disco);
    int falha = ferror(saida);
    if (fclose(saida) != 0 || falha) erro("falha ao concluir a gravacao.");
    if (rename(temporario, "correct.txt") != 0) erro("nao foi possivel salvar correct.txt.");
    temporario[0] = '\0';
    return EXIT_SUCCESS;
}
