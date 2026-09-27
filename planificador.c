#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_TAREAS      20000
#define MAX_SUCESORES     128
#define MAX_PREDECESORES  128
#define LARGO_ID           64
#define LARGO_NOMBRE      128
#define LARGO_DEPS        512
#define LARGO_MENSAJE     128
#define LARGO_LINEA      2048
#define TAMANO_TABLA    32768

#define DURACION_MINIMA   100
#define DURACION_MAXIMA  5000

typedef enum {
    ESPERANDO,
    CORRIENDO,
    TERMINADA,
    FALLIDA,
    CANCELADA
} Estado;

typedef struct {
    char   id[LARGO_ID];
    char   nombre[LARGO_NOMBRE];
    int    duracion;

    char   deps_texto[LARGO_DEPS];   /* dependencias tal como venian escritas */
    int    faltantes;                /* cuantas dependencias aun no terminan */

    int    predecesores[MAX_PREDECESORES]; /* de quienes depende esta tarea */
    int    total_predecesores;

    int    sucesores[MAX_SUCESORES];       /* quienes esperan por esta tarea */
    int    total_sucesores;

    Estado estado;
    pid_t  pid;
    char   mensaje[LARGO_MENSAJE];   /* aviso que deja al terminar */
} Tarea;

typedef struct Entrada {
    char            id[LARGO_ID];
    int             indice;
    struct Entrada *siguiente;
} Entrada;

static Tarea    tareas[MAX_TAREAS];
static int      total_tareas = 0;
static Entrada *tabla[TAMANO_TABLA];

/* porcentaje de probabilidad de que una tarea falle, para poder probar
   el aislamiento de errores. Por defecto queda en 0. */
static int probabilidad_fallo = 0;

/* Se levanta cuando el usuario aprieta Ctrl+C. El manejador no hace nada
   mas que marcarla: si tocara las tareas o imprimiera podria quedar a
   medias con lo que esta haciendo el programa en ese momento. */
static volatile sig_atomic_t llego_sigint = 0;

static void manejar_sigint(int senal)
{
    (void)senal;
    llego_sigint = 1;
}

static char vacio[] = "";

/* ---------------- tabla hash: id -> posicion en el arreglo ---------------- */

static unsigned long funcion_hash(const char *texto)
{
    unsigned long suma = 5381;

    while (*texto != '\0') {
        suma = suma * 33 + (unsigned char)*texto;
        texto++;
    }
    return suma % TAMANO_TABLA;
}

static int buscar_id(const char *id)
{
    Entrada *e = tabla[funcion_hash(id)];

    while (e != NULL) {
        if (strcmp(e->id, id) == 0)
            return e->indice;
        e = e->siguiente;
    }
    return -1;
}

static void guardar_id(const char *id, int indice)
{
    unsigned long pos = funcion_hash(id);

    Entrada *nueva = malloc(sizeof(Entrada));
    if (nueva == NULL) {
        perror("malloc");
        exit(1);
    }

    snprintf(nueva->id, LARGO_ID, "%s", id);
    nueva->indice    = indice;
    nueva->siguiente = tabla[pos];
    tabla[pos] = nueva;
}

static void liberar_tabla(void)
{
    for (int i = 0; i < TAMANO_TABLA; i++) {
        Entrada *e = tabla[i];

        while (e != NULL) {
            Entrada *siguiente = e->siguiente;
            free(e);
            e = siguiente;
        }
    }
}

/* ---------------- manejo de texto ---------------- */

/* saca los espacios del inicio y del final */
static char *limpiar(char *texto)
{
    while (*texto != '\0' && isspace((unsigned char)*texto))
        texto++;

    if (*texto == '\0')
        return texto;

    char *final = texto + strlen(texto) - 1;

    while (final > texto && isspace((unsigned char)*final)) {
        *final = '\0';
        final--;
    }
    return texto;
}

/* corta la linea en 4 partes usando los ':'. Si faltan partes las deja vacias */
static void separar(char *linea, char *partes[4])
{
    int n = 0;
    char *actual = linea;

    while (n < 4) {
        char *corte = strchr(actual, ':');

        if (corte == NULL) {
            partes[n] = actual;
            n++;
            break;
        }

        *corte = '\0';
        partes[n] = actual;
        n++;
        actual = corte + 1;
    }

    while (n < 4) {
        partes[n] = vacio;
        n++;
    }
}

/* ---------------- carga del plan ---------------- */

static int leer_archivo(const char *ruta)
{
    FILE *archivo = fopen(ruta, "r");
    if (archivo == NULL) {
        fprintf(stderr, "no se pudo abrir el archivo %s\n", ruta);
        return -1;
    }

    char linea[LARGO_LINEA];
    int numero_linea = 0;

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        numero_linea++;

        char *sin_espacios = limpiar(linea);
        if (*sin_espacios == '\0')
            continue;

        char *partes[4];
        separar(sin_espacios, partes);

        char *id       = limpiar(partes[0]);
        char *nombre   = limpiar(partes[1]);
        char *duracion = limpiar(partes[2]);
        char *deps     = limpiar(partes[3]);

        if (*id == '\0') {
            fprintf(stderr, "linea %d: falta el id\n", numero_linea);
            fclose(archivo);
            return -1;
        }

        if (buscar_id(id) != -1) {
            fprintf(stderr, "linea %d: el id %s esta repetido\n", numero_linea, id);
            fclose(archivo);
            return -1;
        }

        if (total_tareas >= MAX_TAREAS) {
            fprintf(stderr, "el plan tiene mas de %d tareas\n", MAX_TAREAS);
            fclose(archivo);
            return -1;
        }

        Tarea *t = &tareas[total_tareas];

        snprintf(t->id,         LARGO_ID,     "%s", id);
        snprintf(t->nombre,     LARGO_NOMBRE, "%s", (*nombre != '\0') ? nombre : id);
        snprintf(t->deps_texto, LARGO_DEPS,   "%s", deps);

        /* si no viene la duracion se sortea entre 100 y 5000 ms */
        if (*duracion == '\0')
            t->duracion = DURACION_MINIMA + rand() % (DURACION_MAXIMA - DURACION_MINIMA + 1);
        else
            t->duracion = atoi(duracion);

        if (t->duracion <= 0) {
            fprintf(stderr, "linea %d: la duracion no es valida\n", numero_linea);
            fclose(archivo);
            return -1;
        }

        t->faltantes          = 0;
        t->total_predecesores = 0;
        t->total_sucesores    = 0;
        t->estado          = ESPERANDO;
        t->pid             = -1;
        t->mensaje[0]      = '\0';

        guardar_id(id, total_tareas);
        total_tareas++;
    }

    fclose(archivo);

    if (total_tareas == 0) {
        fprintf(stderr, "el archivo no tiene tareas\n");
        return -1;
    }
    return 0;
}

/* Las dependencias se resuelven recien cuando ya estan todas las tareas
   cargadas, porque una tarea puede depender de otra que aparece mas abajo
   en el archivo. */
static int conectar_dependencias(void)
{
    for (int i = 0; i < total_tareas; i++) {

        if (tareas[i].deps_texto[0] == '\0')
            continue;

        char copia[LARGO_DEPS];
        snprintf(copia, sizeof(copia), "%s", tareas[i].deps_texto);

        /* los corchetes se aceptan como separadores porque el enunciado
           escribe la lista como [dep1, dep2] */
        char *resto = NULL;
        char *dep = strtok_r(copia, ",[] \t", &resto);

        while (dep != NULL) {
            char *id_dep = limpiar(dep);

            if (*id_dep != '\0') {
                int pos = buscar_id(id_dep);

                if (pos == -1) {
                    fprintf(stderr, "la tarea %s depende de %s, que no existe\n",
                            tareas[i].id, id_dep);
                    return -1;
                }

                if (pos == i) {
                    fprintf(stderr, "la tarea %s depende de si misma\n", tareas[i].id);
                    return -1;
                }

                if (tareas[pos].total_sucesores >= MAX_SUCESORES) {
                    fprintf(stderr, "la tarea %s tiene demasiados sucesores\n",
                            tareas[pos].id);
                    return -1;
                }

                if (tareas[i].total_predecesores >= MAX_PREDECESORES) {
                    fprintf(stderr, "la tarea %s tiene demasiadas dependencias\n",
                            tareas[i].id);
                    return -1;
                }

                /* la relacion se guarda en los dos sentidos: hacia adelante
                   para saber a quien avisar, y hacia atras para poder juntar
                   despues los mensajes de las dependencias */
                tareas[pos].sucesores[tareas[pos].total_sucesores] = i;
                tareas[pos].total_sucesores++;

                tareas[i].predecesores[tareas[i].total_predecesores] = pos;
                tareas[i].total_predecesores++;

                tareas[i].faltantes++;
            }

            dep = strtok_r(NULL, ",[] \t", &resto);
        }
    }
    return 0;
}

/* ---------------- salida por pantalla ---------------- */

static void mostrar_plan(void)
{
    printf("tareas cargadas: %d\n\n", total_tareas);
    printf("%-8s %-22s %10s  %-14s %s\n",
           "ID", "NOMBRE", "DURACION", "DEPENDE DE", "SUCESORES");

    for (int i = 0; i < total_tareas; i++) {
        printf("%-8s %-22s %7d ms  ",
               tareas[i].id, tareas[i].nombre, tareas[i].duracion);

        char lista[LARGO_DEPS] = "";
        for (int j = 0; j < tareas[i].total_predecesores; j++) {
            if (j > 0)
                strncat(lista, ", ", sizeof(lista) - strlen(lista) - 1);
            strncat(lista, tareas[tareas[i].predecesores[j]].id,
                    sizeof(lista) - strlen(lista) - 1);
        }
        printf("%-14s ", (lista[0] != '\0') ? lista : "-");

        if (tareas[i].total_sucesores == 0) {
            printf("-");
        } else {
            for (int j = 0; j < tareas[i].total_sucesores; j++) {
                if (j > 0)
                    printf(", ");
                printf("%s", tareas[tareas[i].sucesores[j]].id);
            }
        }
        printf("\n");
    }

    printf("\ntareas que pueden partir de inmediato: ");

    int encontradas = 0;
    for (int i = 0; i < total_tareas; i++) {
        if (tareas[i].faltantes == 0) {
            if (encontradas > 0)
                printf(", ");
            printf("%s", tareas[i].id);
            encontradas++;
        }
    }
    printf("\n");
}

/* ---------------- cola de tareas listas ---------------- */

static int cola[MAX_TAREAS];
static int inicio_cola = 0;
static int fin_cola    = 0;

static void encolar(int indice)
{
    cola[fin_cola] = indice;
    fin_cola++;
}

static int hay_en_cola(void)
{
    return inicio_cola < fin_cola;
}

static int sacar_de_cola(void)
{
    int indice = cola[inicio_cola];
    inicio_cola++;
    return indice;
}

/* ---------------- ejecucion ---------------- */

typedef struct {
    pid_t pid;
    int   indice;
    int   fd_aviso;   /* por aca llega el mensaje que deja el hijo */
} Proceso;

static Proceso en_ejecucion[MAX_TAREAS];

/* Crea el proceso hijo que simula una tarea.
   Se arman dos pipes: uno para mandarle al hijo los mensajes de las tareas
   de las que depende, y otro para que el hijo devuelva su propio mensaje. */
static int lanzar_tarea(int i)
{
    int canal_insumos[2];   /* padre -> hijo */
    int canal_aviso[2];     /* hijo  -> padre */

    if (pipe(canal_insumos) < 0 || pipe(canal_aviso) < 0) {
        perror("pipe");
        exit(1);
    }

    /* se vacia lo que quede pendiente de imprimir para que el hijo no
       herede texto sin escribir */
    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* ---- proceso hijo ---- */

        /* el hijo no usa el manejador del padre: si llega Ctrl+C simplemente
           termina, que es el comportamiento por defecto */
        signal(SIGINT, SIG_DFL);

        close(canal_insumos[1]);
        close(canal_aviso[0]);

        /* recibe los avisos de sus dependencias. Los lee hasta que el padre
           cierra el pipe; quien los muestra por pantalla es el padre, para
           que la salida salga en orden. */
        char buffer[LARGO_MENSAJE * MAX_PREDECESORES];
        int  total = 0;
        int  leidos;

        while ((leidos = read(canal_insumos[0], buffer + total,
                              sizeof(buffer) - (size_t)total)) > 0) {
            total += leidos;
        }
        close(canal_insumos[0]);

        /* simula el trabajo */
        struct timespec espera;
        espera.tv_sec  = tareas[i].duracion / 1000;
        espera.tv_nsec = (tareas[i].duracion % 1000) * 1000000L;
        nanosleep(&espera, NULL);

        /* cada hijo necesita su propia semilla, si no todos sortean lo
           mismo porque heredan el estado del padre */
        srand((unsigned int)getpid());

        if (probabilidad_fallo > 0 && rand() % 100 < probabilidad_fallo) {
            close(canal_aviso[1]);
            _exit(1);
        }

        /* deja su mensaje para las tareas que lo esperan */
        char mensaje[LARGO_MENSAJE];
        int largo = snprintf(mensaje, sizeof(mensaje), "%s listo (%d ms)",
                             tareas[i].nombre, tareas[i].duracion);

        write(canal_aviso[1], mensaje, (size_t)largo + 1);
        close(canal_aviso[1]);

        fflush(stdout);
        _exit(0);
    }

    /* ---- proceso padre ---- */
    close(canal_insumos[0]);
    close(canal_aviso[1]);

    tareas[i].pid    = pid;
    tareas[i].estado = CORRIENDO;

    printf("[inicia ] %-22s (%d ms)\n", tareas[i].nombre, tareas[i].duracion);

    /* le manda al hijo los mensajes que dejaron sus dependencias */
    for (int j = 0; j < tareas[i].total_predecesores; j++) {
        int anterior = tareas[i].predecesores[j];

        if (tareas[anterior].mensaje[0] != '\0') {
            write(canal_insumos[1], tareas[anterior].mensaje,
                  strlen(tareas[anterior].mensaje) + 1);

            printf("           recibe: %s\n", tareas[anterior].mensaje);
        }
    }
    close(canal_insumos[1]);

    return canal_aviso[0];
}

/* Marca como canceladas todas las tareas que dependian, directa o
   indirectamente, de una tarea que fallo. Devuelve cuantas cancelo.
   El recorrido es por niveles: se parte de la tarea fallida y se van
   agregando sus sucesores a una lista por revisar. */
static int cancelar_rama(int fallida)
{
    static int por_revisar[MAX_TAREAS];

    int cantidad   = 0;
    int revisadas  = 0;
    int canceladas = 0;

    por_revisar[cantidad] = fallida;
    cantidad++;

    while (revisadas < cantidad) {
        int actual = por_revisar[revisadas];
        revisadas++;

        for (int j = 0; j < tareas[actual].total_sucesores; j++) {
            int sucesor = tareas[actual].sucesores[j];

            if (tareas[sucesor].estado == ESPERANDO) {
                tareas[sucesor].estado = CANCELADA;
                canceladas++;

                printf("[cancela] %-22s depende de %s\n",
                       tareas[sucesor].nombre, tareas[actual].nombre);

                por_revisar[cantidad] = sucesor;
                cantidad++;
            }
        }
    }
    return canceladas;
}

/* Corta todas las actividades: mata a las que estan corriendo, espera a
   que mueran para no dejar procesos sueltos, y marca como canceladas las
   que todavia no alcanzaron a partir. */
static void abortar_todo(int activos)
{
    printf("\n[seremi ] llego la inspeccion, se abortan todas las actividades\n");

    for (int j = 0; j < activos; j++)
        kill(en_ejecucion[j].pid, SIGTERM);

    for (int j = 0; j < activos; j++) {
        while (waitpid(en_ejecucion[j].pid, NULL, 0) < 0 && errno == EINTR)
            ;

        close(en_ejecucion[j].fd_aviso);
        tareas[en_ejecucion[j].indice].estado = CANCELADA;

        printf("[cancela] %-22s estaba en ejecucion\n",
               tareas[en_ejecucion[j].indice].nombre);
    }

    for (int i = 0; i < total_tareas; i++) {
        if (tareas[i].estado == ESPERANDO)
            tareas[i].estado = CANCELADA;
    }
}

static void ejecutar_plan(int limite)
{
    for (int i = 0; i < total_tareas; i++) {
        if (tareas[i].faltantes == 0)
            encolar(i);
    }

    int activos    = 0;
    int procesadas = 0;

    while (procesadas < total_tareas) {

        if (llego_sigint) {
            abortar_todo(activos);
            break;
        }

        /* se lanzan tareas mientras quede cupo */
        while (activos < limite && hay_en_cola()) {
            int i = sacar_de_cola();

            int fd = lanzar_tarea(i);

            en_ejecucion[activos].pid      = tareas[i].pid;
            en_ejecucion[activos].indice   = i;
            en_ejecucion[activos].fd_aviso = fd;
            activos++;
        }

        /* nada corriendo y nada que lanzar: el resto quedo bloqueado */
        if (activos == 0) {
            printf("\nquedaron %d tareas que no pudieron ejecutarse\n",
                   total_tareas - procesadas);
            break;
        }
        /* el padre se queda dormido aca hasta que termine algun hijo */
        int estado;
        pid_t pid_terminado = waitpid(-1, &estado, 0);

        if (pid_terminado < 0) {
            /* la señal interrumpio la espera: se vuelve al inicio del
               ciclo para revisar la bandera */
            if (errno == EINTR)
                continue;

            perror("waitpid");
            break;
        }

        int pos = -1;
        for (int j = 0; j < activos; j++) {
            if (en_ejecucion[j].pid == pid_terminado) {
                pos = j;
                break;
            }
        }
        if (pos == -1)
            continue;

        int i  = en_ejecucion[pos].indice;
        int fd = en_ejecucion[pos].fd_aviso;

        /* el hijo ya termino, asi que su mensaje esta esperando en el pipe */
        int leidos = read(fd, tareas[i].mensaje, LARGO_MENSAJE);
        if (leidos > 0)
            tareas[i].mensaje[leidos - 1] = '\0';
        else
            tareas[i].mensaje[0] = '\0';

        close(fd);

        /* se saca de la lista moviendo el ultimo a su lugar */
        en_ejecucion[pos] = en_ejecucion[activos - 1];
        activos--;
        procesadas++;

        if (WIFEXITED(estado) && WEXITSTATUS(estado) == 0) {
            tareas[i].estado = TERMINADA;
            printf("[termina] %-22s -> %s\n", tareas[i].nombre, tareas[i].mensaje);

            /* avisar a los que dependian de esta tarea */
            for (int j = 0; j < tareas[i].total_sucesores; j++) {
                int sucesor = tareas[i].sucesores[j];

                tareas[sucesor].faltantes--;
                if (tareas[sucesor].faltantes == 0)
                    encolar(sucesor);
            }
        } else {
            tareas[i].estado = FALLIDA;

            if (WIFSIGNALED(estado))
                printf("[falla  ] %-22s (terminada por señal %d)\n",
                       tareas[i].nombre, WTERMSIG(estado));
            else
                printf("[falla  ] %-22s (codigo %d)\n",
                       tareas[i].nombre, WEXITSTATUS(estado));

            /* el plan sigue: solo se cancela lo que dependia de esta tarea */
            procesadas += cancelar_rama(i);
        }
    }

    int listas = 0, fallidas = 0, canceladas = 0;

    for (int i = 0; i < total_tareas; i++) {
        if (tareas[i].estado == TERMINADA)      listas++;
        else if (tareas[i].estado == FALLIDA)   fallidas++;
        else if (tareas[i].estado == CANCELADA) canceladas++;
    }

    printf("\nresumen: %d completadas, %d fallidas, %d canceladas (de %d)\n",
           listas, fallidas, canceladas, total_tareas);
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "uso: %s <plan.txt> <K> [probabilidad de falla 0-100]\n",
                argv[0]);
        return 1;
    }

    int limite = atoi(argv[2]);
    if (limite <= 0) {
        fprintf(stderr, "K tiene que ser un numero mayor que 0\n");
        return 1;
    }

    if (argc == 4) {
        probabilidad_fallo = atoi(argv[3]);

        if (probabilidad_fallo < 0 || probabilidad_fallo > 100) {
            fprintf(stderr, "la probabilidad de falla va entre 0 y 100\n");
            return 1;
        }
    }

    srand(time(NULL));

    /* se registra el manejador de Ctrl+C. Se usa sigaction en vez de signal
       porque asi waitpid se interrumpe cuando llega la señal, en lugar de
       quedarse esperando a que termine el hijo que estaba corriendo. */
    struct sigaction accion;

    memset(&accion, 0, sizeof(accion));
    accion.sa_handler = manejar_sigint;
    sigemptyset(&accion.sa_mask);
    accion.sa_flags = 0;

    if (sigaction(SIGINT, &accion, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    if (leer_archivo(argv[1]) != 0) {
        liberar_tabla();
        return 1;
    }

    if (conectar_dependencias() != 0) {
        liberar_tabla();
        return 1;
    }

    printf("plan: %s | limite de procesos: %d\n\n", argv[1], limite);
    mostrar_plan();

    printf("\n--- ejecucion ---\n");
    ejecutar_plan(limite);

    liberar_tabla();
    return 0;
}