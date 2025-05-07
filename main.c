#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#define MAX_LINE    512
#define MAX_DEVICE   64
#define N_SENSORS     6

// Objeto que vai guardar o device, ano, mes e o sensor
typedef struct {
    char device[MAX_DEVICE];
    int ano, mes;
    double sensor[N_SENSORS];
} Record;

// Objeto que vai guardar estatísticas para um dado triplo (device, ano-mes, sensor_id)
// soma - para o cálculo da média
// count - quantidade de leituras
typedef struct {
    char device[MAX_DEVICE];
    int ano, mes, sensor_id;
    double min, max, soma;
    size_t count;
} StatEntry;

// Um vetor para armazenar StatEntry
typedef struct {
    StatEntry *entries;
    size_t size, capacity;
} StatMap;

// Armazena tudo que a thread precisa para processar a sua "fatia" de dados
// records - ponteiro para o vetor compartilhado de registros carregados do CSV
// start, end - índices, no vetor `records`, delimitando o sub-conjunto que esta thread vai processar
// local_map - mapa (vetor dinâmico) onde a thread armazena suas estatísticas parciais (min/max/soma/count)
typedef struct {
    Record *records;
    size_t start, end;
    StatMap local_map;
} ThreadArg;

// Nomes dos sensores
const char *sensor_names[N_SENSORS] = {
    "temperatura", "umidade", "luminosidade",
    "ruido", "eco2", "etvoc"
};

// Alocação segura (abortam imediatamente em caso de falha de alocação, para nunca escrever um ponteiro NULL)
void *xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

// Alocação segura (abortam imediatamente em caso de falha de alocação, para nunca escrever um ponteiro NULL)
void *xrealloc(void *old, size_t sz) {
    void *p = realloc(old, sz);
    if (!p) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

// Inicializa o mapa das estatísticas
void init_map(StatMap *m) {
    m->size = 0;
    m->capacity = 1024;
    m->entries = xmalloc(m->capacity * sizeof(StatEntry));
}

// Desaloca o bloco de memória utilizado pelo mapa de estatísticas
void free_map(StatMap *m) {
    free(m->entries);
}

// Garante que o mapa sempre tenha espaço livre para a adição de um elemento
void ensure_capacity(StatMap *m) {
    if (m->size >= m->capacity) {
        m->capacity *= 2;
        m->entries = xrealloc(m->entries,
                              m->capacity * sizeof(StatEntry));
    }
}

// Retorna índice ou -1
ssize_t find_entry(StatMap *m,
                   const char *dev,
                   int ano, int mes,
                   int sid) {
    for (size_t i = 0; i < m->size; i++) {
        StatEntry *e = &m->entries[i];
        if (e->sensor_id == sid &&
            e->ano == ano && e->mes == mes &&
            strcmp(e->device, dev) == 0) {
            return i;
        }
    }
    return -1;
}

// Faz find_entry para ver se já existe estatística para esse (device, ano, mes, sensor_id) e, se não, adiciona um novo StatEntry.
void update_map(StatMap *m,
                const char *dev,
                int ano, int mes,
                int sid, double val) {
    ssize_t idx = find_entry(m, dev, ano, mes, sid);
    if (idx < 0) {
        ensure_capacity(m);
        StatEntry *e = &m->entries[m->size++];
        strncpy(e->device, dev, MAX_DEVICE-1);
        e->device[MAX_DEVICE-1] = '\0';
        e->ano = ano; e->mes = mes; e->sensor_id = sid;
        e->min = e->max = val;
        e->soma = val;
        e->count = 1;
    } else {
        StatEntry *e = &m->entries[idx];
        if (val < e->min) e->min = val;
        if (val > e->max) e->max = val;
        e->soma += val;
        e->count++;
    }
}

// Processa a "fatia" do vetor de registros e acumula estatísticas no seu próprio local_map
void *thread_func(void *arg) {
    ThreadArg *ta = arg;
    init_map(&ta->local_map);
    for (size_t i = ta->start; i < ta->end; i++) {
        Record *r = &ta->records[i];
        for (int s = 0; s < N_SENSORS; s++) {
            update_map(&ta->local_map,
                       r->device, r->ano, r->mes,
                       s, r->sensor[s]);
        }
    }
    return NULL;
}

int main() {
    // 1) Carrega o CSV
    FILE *f = fopen("devices.csv", "r");
    if (!f) {
        perror("devices.csv");
        return 1;
    }

    char line[MAX_LINE];

    // Pula o header
    if (!fgets(line, MAX_LINE, f)) {
        fprintf(stderr, "Erro ao ler header\n");
        fclose(f);
        return 1;
    }

    Record *records = NULL;
    size_t cap = 0, n = 0;
    struct tm tm;

    while (fgets(line, MAX_LINE, f)) {
        char *tok = strtok(line, "|");
        if (!tok) continue; // Linha em formato inválido

        // Pega o device
        tok = strtok(NULL, "|");
        if (!tok) continue;

        char dev[MAX_DEVICE];
        strncpy(dev, tok, MAX_DEVICE-1);
        dev[MAX_DEVICE-1] = '\0';

        // Pega a contagem (ignora)
        tok = strtok(NULL, "|");
        if (!tok) continue;

        // Pega a data
        tok = strtok(NULL, "|");
        if (!tok) continue;

        // Extrai só os 10 primeiros caracteres (YYYY-MM-DD)
        char date_only[11];
        strncpy(date_only, tok, 10);
        date_only[10] = '\0';
        memset(&tm, 0, sizeof(tm));

        if (!strptime(date_only, "%Y-%m-%d", &tm)) continue; // Data inválida

        int ano = tm.tm_year + 1900;
        int mes = tm.tm_mon + 1;
        if (ano < 2024 || (ano == 2024 && mes < 3))
            continue;

        // Prepara espaço para novo record
        if (n >= cap) {
            cap = cap ? cap * 2 : 1024;
            records = xrealloc(records, cap * sizeof(Record));
        }

        Record *r = &records[n++];
        strncpy(r->device, dev, MAX_DEVICE-1);
        r->device[MAX_DEVICE-1] = '\0';
        r->ano = ano;
        r->mes = mes;

        // Lê os sensores
        int valid = 1;
        for (int s = 0; s < N_SENSORS; s++) {
            tok = strtok(NULL, "|");
            if (!tok) {
                fprintf(stderr,
                        "Linha %zu: sensor %d faltando, pulando registro\n",
                        n, s);
                valid = 0;
                break;
            }
            r->sensor[s] = atof(tok);
        }
        if (!valid) {
            n--; // Desfaz incremento se não for válido
            continue;
        }
    }

    fclose(f);

    if (n == 0) {
        fprintf(stderr, "Nenhum registro válido após 2024-03. Encerrando.\n");
        free(records);
        return 1;
    }

    // 2) Detecta nº de threads
    int nthreads = sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1) nthreads = 1;

    pthread_t threads[nthreads];
    ThreadArg args[nthreads];
    size_t chunk = n / nthreads;

    // 3) Cria threads
    for (int i = 0; i < nthreads; i++) {
        args[i].records = records;
        args[i].start   = i * chunk;
        args[i].end     = (i == nthreads-1 ? n : (i+1)*chunk);
        pthread_create(&threads[i], NULL, thread_func, &args[i]);
    }

    // 4) Join + merge
    StatMap global;
    init_map(&global);
    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
        StatMap *lm = &args[i].local_map;
        for (size_t j = 0; j < lm->size; j++) {
            StatEntry *e = &lm->entries[j];
            ssize_t idx = find_entry(&global,
                                     e->device, e->ano, e->mes,
                                     e->sensor_id);
            if (idx < 0) {
                ensure_capacity(&global);
                global.entries[global.size++] = *e;
            } else {
                StatEntry *g = &global.entries[idx];
                if (e->min < g->min) g->min = e->min;
                if (e->max > g->max) g->max = e->max;
                g->soma  += e->soma;
                g->count += e->count;
            }
        }

        free_map(lm);
    }

    // 5) Escreve o CSV de saída
    FILE *out = fopen("resumo.csv", "w");
    if (!out) {
        perror("resumo.csv");
        return 1;
    }

    printf("Resultado:\n");
    fprintf(out, "device;ano-mes;sensor;valor_maximo;valor_medio;valor_minimo\n");
    printf("device;ano-mes;sensor;valor_maximo;valor_medio;valor_minimo\n");
    for (size_t i = 0; i < global.size; i++) {
        StatEntry *e = &global.entries[i];
        double med = e->soma / e->count;
        
        char printMsg[500];
        snprintf(printMsg, sizeof printMsg,
            "%s;%04d-%02d;%s;%.2f;%.2f;%.2f\n",
            e->device, e->ano, e->mes,
            sensor_names[e->sensor_id],
            e->max, med, e->min);

        fprintf(out, "%s",
                printMsg);
        printf("%s", printMsg);
    }

    fclose(out);

    free_map(&global);
    free(records);
    printf("resumo.csv gerado com sucesso.\n");
    return 0;
}
