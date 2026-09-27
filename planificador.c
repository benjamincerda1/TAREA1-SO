#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

#define MAX_NODOS    20000
#define MAX_ID        64
#define MAX_NOMBRE   128
#define MAX_LINEA   8192
#define HASH_SIZE   32768
#define TIEMPO_MIN    100
#define TIEMPO_MAX   5000

typedef enum {
    PENDIENTE,
    LISTO,
    EJECUTANDO,
    OK,
    FALLIDO,
    ABORTADO
} Estado;

typedef struct {
    char   id[MAX_ID];
    char   nombre[MAX_NOMBRE];
    int    tiempo_ms;
    Estado estado;
    pid_t  pid;
    int    in_degree;
    int   *sucesores;
    int    n_sucesores;
    int    cap_sucesores;
} Nodo;

typedef struct EntradaHash {
    char                 id[MAX_ID];
    int                  indice;
    struct EntradaHash  *sig;
} EntradaHash;

typedef struct {
    Nodo         *nodos;
    int           n_nodos;
    EntradaHash  *tabla[HASH_SIZE];
} Grafo;

/* ------------------------------------------------------------------ */
/* Utilidades de texto                                                 */
/* ------------------------------------------------------------------ */

/* Elimina espacios, tabulaciones y retornos de carro (\r de Windows)
   al inicio y al final. Modifica la cadena en el lugar. */
static char *trim(char *s)
{
    if (s == NULL)
        return NULL;

    while (*s != '\0' && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    char *fin = s + strlen(s) - 1;
    while (fin > s && isspace((unsigned char)*fin)) {
        *fin = '\0';
        fin--;
    }
    return s;
}

/* Divide la linea en como maximo max_campos usando ':' como separador.
   A diferencia de strtok, NO colapsa separadores consecutivos, por lo
   que un campo vacio se preserva como cadena vacia. Devuelve cuantos
   campos encontro. */
static int dividir(char *linea, char *campos[], int max_campos)
{
    int n = 0;
    char *inicio = linea;

    while (n < max_campos) {
        char *sep = strchr(inicio, ':');
        if (sep == NULL) {
            campos[n++] = inicio;
            break;
        }
        *sep = '\0';
        campos[n++] = inicio;
        inicio = sep + 1;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Tabla hash: id (cadena) -> indice en el arreglo de nodos            */
/* ------------------------------------------------------------------ */

static unsigned long djb2(const char *s)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != 0)
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static int hash_buscar(const Grafo *g, const char *id)
{
    unsigned long pos = djb2(id) % HASH_SIZE;
    EntradaHash *e = g->tabla[pos];

    while (e != NULL) {
        if (strcmp(e->id, id) == 0)
            return e->indice;
        e = e->sig;
    }
    return -1;
}

/* Devuelve 0 si inserto, -1 si el id ya estaba registrado. */
static int hash_insertar(Grafo *g, const char *id, int indice)
{
    if (hash_buscar(g, id) != -1)
        return -1;

    unsigned long pos = djb2(id) % HASH_SIZE;
    EntradaHash *e = malloc(sizeof(EntradaHash));
    if (e == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    snprintf(e->id, MAX_ID, "%s", id);
    e->indice  = indice;
    e->sig     = g->tabla[pos];
    g->tabla[pos] = e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Construccion del grafo                                              */
/* ------------------------------------------------------------------ */

static void agregar_sucesor(Nodo *n, int indice_sucesor)
{
    if (n->n_sucesores == n->cap_sucesores) {
        int nueva_cap = (n->cap_sucesores == 0) ? 4 : n->cap_sucesores * 2;
        int *tmp = realloc(n->sucesores, (size_t)nueva_cap * sizeof(int));
        if (tmp == NULL) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        n->sucesores     = tmp;
        n->cap_sucesores = nueva_cap;
    }
    n->sucesores[n->n_sucesores++] = indice_sucesor;
}

static void grafo_init(Grafo *g)
{
    g->nodos = calloc(MAX_NODOS, sizeof(Nodo));
    if (g->nodos == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
    g->n_nodos = 0;
    memset(g->tabla, 0, sizeof(g->tabla));
}

static void grafo_liberar(Grafo *g)
{
    for (int i = 0; i < g->n_nodos; i++)
        free(g->nodos[i].sucesores);
    free(g->nodos);

    for (int i = 0; i < HASH_SIZE; i++) {
        EntradaHash *e = g->tabla[i];
        while (e != NULL) {
            EntradaHash *sig = e->sig;
            free(e);
            e = sig;
        }
    }
}

/* Convierte el campo de tiempo. Si viene vacio asigna un valor
   aleatorio en [TIEMPO_MIN, TIEMPO_MAX]. Devuelve -1 si es invalido. */
static int parsear_tiempo(const char *campo)
{
    if (campo == NULL || *campo == '\0')
        return TIEMPO_MIN + rand() % (TIEMPO_MAX - TIEMPO_MIN + 1);

    char *fin = NULL;
    errno = 0;
    long v = strtol(campo, &fin, 10);

    if (errno != 0 || fin == campo || *fin != '\0' || v <= 0 || v > INT_MAX)
        return -1;

    return (int)v;
}

/* Primera pasada: registra todos los IDs con su nombre y duracion. */
static int primera_pasada(Grafo *g, FILE *f, const char *ruta)
{
    char linea[MAX_LINEA];
    int  n_linea = 0;

    while (fgets(linea, sizeof(linea), f) != NULL) {
        n_linea++;

        char copia[MAX_LINEA];
        snprintf(copia, sizeof(copia), "%s", linea);

        char *sin_espacios = trim(copia);
        if (*sin_espacios == '\0' || *sin_espacios == '#')
            continue;

        char *campos[4] = { NULL, NULL, NULL, NULL };
        int n_campos = dividir(sin_espacios, campos, 4);

        if (n_campos < 2) {
            fprintf(stderr, "%s:%d: formato invalido (se esperaba "
                            "ID : Nombre : tiempo_ms : deps)\n", ruta, n_linea);
            return -1;
        }

        char *id     = trim(campos[0]);
        char *nombre = trim(campos[1]);
        char *tiempo = (n_campos >= 3) ? trim(campos[2]) : (char *)"";

        if (*id == '\0') {
            fprintf(stderr, "%s:%d: ID vacio\n", ruta, n_linea);
            return -1;
        }
        if (strlen(id) >= MAX_ID) {
            fprintf(stderr, "%s:%d: ID demasiado largo\n", ruta, n_linea);
            return -1;
        }
        if (g->n_nodos >= MAX_NODOS) {
            fprintf(stderr, "%s:%d: se supero el maximo de %d actividades\n",
                    ruta, n_linea, MAX_NODOS);
            return -1;
        }

        int ms = parsear_tiempo(tiempo);
        if (ms < 0) {
            fprintf(stderr, "%s:%d: tiempo invalido '%s'\n", ruta, n_linea, tiempo);
            return -1;
        }

        Nodo *n = &g->nodos[g->n_nodos];
        snprintf(n->id,     MAX_ID,     "%s", id);
        snprintf(n->nombre, MAX_NOMBRE, "%s", (*nombre != '\0') ? nombre : id);
        n->tiempo_ms     = ms;
        n->estado        = PENDIENTE;
        n->pid           = -1;
        n->in_degree     = 0;
        n->sucesores     = NULL;
        n->n_sucesores   = 0;
        n->cap_sucesores = 0;

        if (hash_insertar(g, id, g->n_nodos) != 0) {
            fprintf(stderr, "%s:%d: ID duplicado '%s'\n", ruta, n_linea, id);
            return -1;
        }
        g->n_nodos++;
    }

    if (ferror(f)) {
        perror("fgets");
        return -1;
    }
    return 0;
}

/* Segunda pasada: resuelve dependencias y arma las listas de sucesores. */
static int segunda_pasada(Grafo *g, FILE *f, const char *ruta)
{
    char linea[MAX_LINEA];
    int  n_linea = 0;
    int  indice  = 0;

    while (fgets(linea, sizeof(linea), f) != NULL) {
        n_linea++;

        char copia[MAX_LINEA];
        snprintf(copia, sizeof(copia), "%s", linea);

        char *sin_espacios = trim(copia);
        if (*sin_espacios == '\0' || *sin_espacios == '#')
            continue;

        char *campos[4] = { NULL, NULL, NULL, NULL };
        int n_campos = dividir(sin_espacios, campos, 4);

        /* Sin cuarto campo: la actividad no tiene dependencias. */
        if (n_campos < 4) {
            indice++;
            continue;
        }

        char *deps = trim(campos[3]);
        if (*deps == '\0') {
            indice++;
            continue;
        }

        char *guardar = NULL;
        char *tok = strtok_r(deps, ",", &guardar);

        while (tok != NULL) {
            char *dep = trim(tok);

            if (*dep != '\0') {
                int idx_dep = hash_buscar(g, dep);

                if (idx_dep == -1) {
                    fprintf(stderr, "%s:%d: la actividad '%s' depende de '%s', "
                                    "que no existe\n",
                            ruta, n_linea, g->nodos[indice].id, dep);
                    return -1;
                }
                if (idx_dep == indice) {
                    fprintf(stderr, "%s:%d: la actividad '%s' depende de si misma\n",
                            ruta, n_linea, g->nodos[indice].id);
                    return -1;
                }

                agregar_sucesor(&g->nodos[idx_dep], indice);
                g->nodos[indice].in_degree++;
            }
            tok = strtok_r(NULL, ",", &guardar);
        }
        indice++;
    }

    if (ferror(f)) {
        perror("fgets");
        return -1;
    }
    return 0;
}

static int cargar_plan(Grafo *g, const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (f == NULL) {
        fprintf(stderr, "no se pudo abrir '%s': %s\n", ruta, strerror(errno));
        return -1;
    }

    if (primera_pasada(g, f, ruta) != 0) {
        fclose(f);
        return -1;
    }

    if (g->n_nodos == 0) {
        fprintf(stderr, "'%s' no contiene actividades\n", ruta);
        fclose(f);
        return -1;
    }

    rewind(f);

    if (segunda_pasada(g, f, ruta) != 0) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Verificacion                                                        */
/* ------------------------------------------------------------------ */

/* Detecta ciclos y dependencias irresolubles contando cuantos nodos
   alcanzaria un orden topologico (algoritmo de Kahn sobre copias). */
static int detectar_ciclos(const Grafo *g)
{
    int *grado = malloc((size_t)g->n_nodos * sizeof(int));
    int *cola  = malloc((size_t)g->n_nodos * sizeof(int));
    if (grado == NULL || cola == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    int fin = 0;
    for (int i = 0; i < g->n_nodos; i++) {
        grado[i] = g->nodos[i].in_degree;
        if (grado[i] == 0)
            cola[fin++] = i;
    }

    int inicio = 0;
    int visitados = 0;

    while (inicio < fin) {
        int u = cola[inicio++];
        visitados++;

        for (int k = 0; k < g->nodos[u].n_sucesores; k++) {
            int v = g->nodos[u].sucesores[k];
            if (--grado[v] == 0)
                cola[fin++] = v;
        }
    }

    free(grado);
    free(cola);

    if (visitados != g->n_nodos) {
        fprintf(stderr, "el plan contiene un ciclo: %d de %d actividades "
                        "nunca podrian ejecutarse\n",
                g->n_nodos - visitados, g->n_nodos);
        return -1;
    }
    return 0;
}

static void imprimir_dag(const Grafo *g)
{
    printf("Actividades cargadas: %d\n\n", g->n_nodos);
    printf("%-8s %-22s %8s %5s  %s\n", "ID", "NOMBRE", "TIEMPO", "DEPS", "SUCESORES");

    for (int i = 0; i < g->n_nodos; i++) {
        const Nodo *n = &g->nodos[i];

        printf("%-8s %-22s %6d ms %5d  ", n->id, n->nombre, n->tiempo_ms, n->in_degree);

        if (n->n_sucesores == 0) {
            printf("-");
        } else {
            for (int k = 0; k < n->n_sucesores; k++)
                printf("%s%s", (k > 0) ? ", " : "", g->nodos[n->sucesores[k]].id);
        }
        printf("\n");
    }

    printf("\nActividades iniciales (sin dependencias): ");
    int primeras = 0;
    for (int i = 0; i < g->n_nodos; i++) {
        if (g->nodos[i].in_degree == 0)
            printf("%s%s", (primeras++ > 0) ? ", " : "", g->nodos[i].id);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int parsear_k(const char *s)
{
    char *fin = NULL;
    errno = 0;
    long v = strtol(s, &fin, 10);

    if (errno != 0 || fin == s || *fin != '\0' || v <= 0 || v > INT_MAX)
        return -1;

    return (int)v;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "uso: %s <plan.txt> <K>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int K = parsear_k(argv[2]);
    if (K < 0) {
        fprintf(stderr, "K debe ser un entero positivo, se recibio '%s'\n", argv[2]);
        return EXIT_FAILURE;
    }

    srand((unsigned int)time(NULL));

    Grafo g;
    grafo_init(&g);

    if (cargar_plan(&g, argv[1]) != 0) {
        grafo_liberar(&g);
        return EXIT_FAILURE;
    }

    if (detectar_ciclos(&g) != 0) {
        grafo_liberar(&g);
        return EXIT_FAILURE;
    }

    printf("Plan: %s | Concurrencia maxima (K): %d\n\n", argv[1], K);
    imprimir_dag(&g);

    grafo_liberar(&g);
    return EXIT_SUCCESS;
}
