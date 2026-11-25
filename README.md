# Simulador de memória virtual

O programa lê endereços lógicos de 16 bits, traduz cada um para um endereço físico e imprime uma linha por endereço na saída padrão, com a posição da TLB, o endereço físico e o valor do byte. A memória física tem 128 frames de 256 bytes. A TLB guarda 16 traduções de página para frame.

## Compilar e executar

Use um compilador C com suporte a POSIX threads e o utilitário `make`. No Linux, WSL ou macOS, abra a pasta do projeto e coloque nela os arquivos do exemplo, `address.txt` e `BACKING_STORE.bin`. Para compilar e executar o exemplo do professor:

```sh
make
./vm address.txt lru fifo
```

O primeiro argumento é o arquivo com os endereços decimais entre 0 e 65535, separados por espaços ou quebras de linha. O segundo escolhe a política da memória física e o terceiro escolhe a política da TLB. Cada política aceita `fifo` ou `lru`. O comando `make clean` remove o executável.

## Como acompanhar uma tradução

O endereço 16916 corresponde à página 66 e ao deslocamento 20, pois 16916 = 66 × 256 + 20. O simulador consulta as 16 entradas da TLB com uma thread por entrada e aguarda todas as respostas. Se encontrar a página, já conhece o frame. Caso precise consultar a tabela de páginas e a página também esteja ausente, lê os 256 bytes da página no disco. Na primeira tradução do exemplo, a página 66 ocupa o frame 0. Assim, o endereço físico é 0 × 256 + 20 = 20.

Na memória física, FIFO usa um índice que percorre os frames de 0 a 127 e volta a 0. O índice avança quando uma página é carregada. LRU mantém uma fila de índices de frames. A cada acesso, o frame usado vai para o fim, inclusive quando a TLB acerta. Assim, quando a memória enche, o primeiro da fila é o frame usado há mais tempo.

A TLB ocupa primeiro as posições livres, da 0 à 15. Com a TLB cheia, FIFO substitui a tradução mais antiga e LRU substitui a usada há mais tempo. Ao reutilizar um frame, o simulador invalida a tradução da página antiga na tabela e na TLB. Quando só uma entrada da TLB é substituída, a página continua na memória física.

## Conferência do exemplo

Para rodar os testes automáticos, use o projeto extraído de `dad.tar`, que inclui a pasta `testes/`. É preciso ter Python 3 e os dois arquivos de exemplo na pasta do projeto.

```sh
make testar
```

No Ubuntu/WSL, para executar a mesma suíte e acrescentar a sessão ao histórico de evidências:

```sh
script -q -e -a -c 'make testar' evidencias.log
```

Os testes compilam e executam o simulador em um diretório temporário. Primeiro verificam cada tradução com 256 e 128 frames e TLB desligada. Depois comparam a busca sequencial e a busca com 16 threads nas quatro combinações de políticas. O teste compara também a saída padrão com as 1.000 traduções do gabarito, usando o SHA-256 esperado.

O executável padrão usa 128 frames e busca paralela. Nas verificações sem TLB, o campo `TLB` recebe `-1` para indicar que nenhuma entrada foi consultada.

As primeiras três linhas do exemplo são

```text
Virtual address: 16916 TLB: 0 Physical address: 20 Value: 0
Virtual address: 62493 TLB: 1 Physical address: 285 Value: 0
Virtual address: 30198 TLB: 2 Physical address: 758 Value: 29
```

O programa termina na última tradução. Mensagens de erro vão para a saída de erro. O histórico de execução está em `evidencias.log`.
