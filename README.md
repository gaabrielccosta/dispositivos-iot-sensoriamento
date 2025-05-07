# Análise de Dados IoT com pthreads

Este repositório contém um programa em C que processa um arquivo CSV de dados de dispositivos IoT em paralelo, utilizando pthreads. O resultado é um CSV resumido com valores mínimos, médios e máximos mensais para cada sensor de cada dispositivo.

---

## 1. Compilação e Execução

### Requisitos

- GCC (suporte a pthreads)

### Compilar

```bash
gcc -pthread main.c -o main
```

### Executar

1. Coloque o `devices.csv` no mesmo diretório do executável.
2. Rode:

   ```bash
   ./main
   ```

3. O programa gerará `resumo.csv` com as estatísticas.

---

## 2. Carregamento do CSV

- O programa abre `devices.csv` com `fopen` e lê linha a linha via `fgets`.
- Descarta o cabeçalho (primeira linha).
- Usa `strtok(line, "|")` para separar campos pelo delimitador `|`.
- Extrai:

  1. **Device** (string)
  2. **Contagem** (ignorado)
  3. **Data** (string completa em formato `YYYY-MM-DD HH:MM:SS...`)
  4. **Seis sensores**: temperatura, umidade, luminosidade, ruído, eco2, etvoc.

- Para a data:

  - Copia apenas os 10 primeiros caracteres (`YYYY-MM-DD`) em `date_only`.
  - Converte para `struct tm` com `strptime`.
  - Ajusta `ano = tm_year + 1900` e `mes = tm_mon + 1`.

- **Filtra** registros anteriores a março de 2024 (ano < 2024 ou mês < 3 em 2024).
- Armazena cada registro válido em um vetor `Record *records` realocado dinamicamente.

---

## 3. Distribuição de Carga entre Threads

- Detecta automaticamente o número de núcleos disponíveis:

  ```c
  int nthreads = sysconf(_SC_NPROCESSORS_ONLN);
  ```

- Divide o vetor de `n` registros em `nthreads` blocos contíguos de tamanho `chunk = n / nthreads`.
- Cria `nthreads` `pthread_t`, passando a cada thread um `ThreadArg` com:

  - Ponteiro `records`
  - Índices `start` e `end` do seu sub-vetor
  - Estrutura `local_map` para estatísticas locais

- Cada thread processa apenas seu intervalo, evitando contenção.

---

## 4. Análise de Dados por Thread

- Cada thread executa `thread_func(ThreadArg *ta)`:

  1. Inicializa um `StatMap local_map` vazio (capacidade inicial 1024).
  2. Para cada `Record r` em `records[start]` até `records[end-1]`:

     - Para cada sensor de `0` a `5`, chama:

       ```c
       update_map(&local_map, r->device, r->ano, r->mes, sensor_id, r->sensor[sensor_id]);
       ```

     - `update_map`:

       - Usa `find_entry` para checar se já existe estatística para aquele `(device, ano, mes, sensor_id)`.
       - Se não existe, cria um novo `StatEntry` inicializando `min = max = valor`, `soma = valor`, `count = 1`.
       - Se existe, atualiza `min`, `max`, acumula `soma += valor` e incrementa `count++`.

     - Sempre chama `ensure_capacity` antes de inserir, dobrando o buffer se necessário.

  3. Ao fim, guarda todas as estatísticas parciais em `local_map`.

---

## 5. Mesclagem e Geração do CSV de Saída

- Após `pthread_join` de todas as threads, a thread principal:

  1. Inicializa um `StatMap global`.
  2. Para cada thread e para cada entrada `e` em seu `local_map`:

     - Procura o índice em `global` com `find_entry`.
     - Se não existe, insere `*e` em `global`.
     - Se existe, atualiza `min`, `max`, soma `soma`, incrementa `count`.

  3. Abre `resumo.csv` e escreve o cabeçalho:

     ```csv
     device;ano-mes;sensor;valor_maximo;valor_medio;valor_minimo
     ```

  4. Para cada `StatEntry g` em `global.entries`, calcula média `g.soma / g.count` e escreve no CSV.

---

## 6. Execução das Threads: Modo Usuário vs. Modo Núcleo

As threads são criadas utilizando a biblioteca `pthread`, o que significa que, em um sistema Linux, cada thread gerenciada por `pthread` é mapeada para uma **kernel thread**. Isso permite que o **kernel** controle a execução e agendamento das threads.

- **Modo Usuário**:
  - O código das threads é executado no **modo usuário**, o que significa que as threads possuem permissões restritas e não têm acesso direto aos recursos críticos do sistema.
- **Modo Núcleo (Kernel)**:
  - Embora as threads sejam executadas no modo usuário, o **kernel** é responsável por agendar as threads, alternando entre elas de acordo com o controle de recursos e a política de agendamento do sistema.

Em resumo, **o código da thread roda em modo usuário**, mas o **agendamento e troca de contexto são gerenciados pelo kernel**.

---

## 7. Possíveis Problemas de Concorrência

- **Race conditions** ao acessar estruturas compartilhadas: mitigado isolando cada thread em seu `local_map`.
- **Contenção** na fusão final: ocorre apenas na thread principal, sem concorrência.
- **Order of insertion**: a ordem de estatísticas no CSV não é garantida; se precisar de ordenação, pode ordenar `global.entries` antes de escrever.
- **Memória**: buffers iniciais podem crescer (via `ensure_capacity`) causando `realloc` múltiplos.

---
