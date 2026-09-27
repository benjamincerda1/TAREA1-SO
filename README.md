# El Planificador Dieciochero

Tarea 1 — Sistemas Operativos, UDP.

Integrantes: (nombre 1) y (nombre 2)

Esta es nuestra versión del simulador de actividades para las fondas del señor
Loyola. Cada actividad se modela como un nodo dentro de un DAG y corre en su
propio proceso; el programa se encarga de respetar las dependencias entre
ellas y de que nunca haya más de K procesos vivos al mismo tiempo. Todo está
construido solo con `fork()`, `pipe()`, `waitpid()` y `sigaction()`, sin usar
hilos, tal como pedía el enunciado.

## Compilación y uso

```bash
make
./planificador <plan.txt> <K> [probabilidad de falla]
```

El tercer argumento es opcional (va de 0 a 100) y lo agregamos nosotros para
poder probar el aislamiento de errores a voluntad, provocando fallas al azar.
Si se omite, ninguna actividad falla y el programa se ejecuta tal como lo pide
el enunciado.

## Archivos de prueba

Preparamos estos archivos para poder mostrar cada requisito por separado:

| Archivo | Para qué sirve | Cómo ejecutarlo |
|---|---|---|
| `plan.txt` | Ejemplo del enunciado. Parseo, DAG y orden de ejecución | `./planificador plan.txt 3` |
| `falla.txt` | Aislamiento de errores: se cancela sólo la rama afectada | `./planificador falla.txt 2 30` |
| `largo.txt` | Ctrl+C. Duraciones largas para alcanzar a interrumpirlo | `./planificador largo.txt 2` |
| `grande.txt` | Carga de estrés con 10.000 actividades | `time ./planificador grande.txt 8 > /dev/null` |
| `generar_plan.sh` | Genera planes del tamaño que se le pida | `./generar_plan.sh 10000 > grande.txt` |

Si quieres ver el control de concurrencia en acción, basta con correr
`plan.txt` primero con K=1 y después con K=3 y comparar los tiempos.
`falla.txt` conviene correrlo varias veces, porque las fallas salen al azar y
así se ven distintos escenarios de cancelación.

Hicimos una prueba de carga en una VM con Ubuntu: 10.000 actividades se
completaron en 8,25 s con K=8, y en 5,53 s con K=32.

## Funciones implementadas

Así quedó organizado el código, función por función:

| Función | Qué hace |
|---|---|
| `limpiar` | Saca espacios y tabulaciones de los extremos de un texto |
| `separar` | Corta una línea en sus cuatro campos por los `:`, conservando los vacíos |
| `funcion_hash` | Convierte un ID alfanumérico en una posición de la tabla |
| `buscar_id` | Devuelve la posición de una actividad, o -1 si no existe |
| `guardar_id` | Registra un ID nuevo en la tabla |
| `liberar_tabla` | Libera la memoria de la tabla al terminar |
| `leer_archivo` | Lee el plan y crea las actividades con su ID, nombre y duración |
| `conectar_dependencias` | Arma las listas de predecesores y sucesores de cada actividad |
| `mostrar_plan` | Imprime el DAG cargado |
| `encolar` / `hay_en_cola` / `sacar_de_cola` | Cola de actividades listas para ejecutar |
| `lanzar_tarea` | Crea los pipes, hace `fork()` y arranca el proceso hijo |
| `ejecutar_plan` | Ciclo principal: lanza, espera y propaga resultados |
| `cancelar_rama` | Cancela las actividades que dependían de una que falló |
| `abortar_todo` | Termina todas las actividades en curso al llegar Ctrl+C |
| `manejar_sigint` | Manejador de `SIGINT`, sólo levanta una bandera |

## Decisiones de diseño

Acá dejamos por escrito el porqué de cada decisión importante, para que
quede claro que no son arbitrarias:

**Lectura en dos etapas.** Nos dimos cuenta de que una actividad puede
depender de otra que aparece más abajo en el archivo, así que optamos por
cargar primero todas las actividades y recién después resolver las
dependencias.

**Tabla hash.** Como los IDs son alfanuméricos, no nos servían como índice
directo del arreglo. Buscarlos recorriendo todo el arreglo cada vez nos
habría dado un costo cuadrático, algo que no era viable con 10.000
actividades, así que preferimos construir una tabla hash propia.

**Predecesores y sucesores.** Decidimos guardar la relación en los dos
sentidos: los sucesores nos sirven para avisar cuando una actividad termina,
y los predecesores para juntar los mensajes que se le pasan a una actividad
justo antes de lanzarla.

**Cola de listas.** Una actividad entra a la cola apenas su contador de
dependencias pendientes llega a cero. Si una dependencia falla, ese contador
nunca baja del todo, así que la actividad simplemente no se encola —de ahí
sale, de forma natural, el aislamiento por rama.

**`waitpid()` bloqueante.** Cuando no queda cupo disponible, dejamos que el
proceso padre se duerma hasta que termine algún hijo, en vez de consultar en
un ciclo activo que solo gastaría CPU sin hacer nada útil.

**Dos pipes por actividad.** Usamos uno para mandarle al hijo los mensajes de
sus dependencias, y otro para que el hijo devuelva el suyo propio. Cada
mensaje se escribe con su `'\0'` al final, porque pueden llegar varios juntos
en una misma lectura. Optamos por que quien los imprime sea siempre el padre,
para que la salida por pantalla mantenga el orden.

**`nanosleep()` en vez de `usleep()`.** Como las duraciones pueden llegar
hasta los 5000 ms, y `usleep()` no está garantizado para esperas de un
segundo o más, preferimos ir a la fuente más confiable.

**`sigaction()` en vez de `signal()`.** Con `signal()`, `waitpid()` se
reanuda sola apenas llega la señal, y el Ctrl+C no se notaría hasta que
terminara el hijo que estuviera corriendo en ese momento. Con `sa_flags = 0`
conseguimos que devuelva `EINTR`, y así el ciclo principal reacciona de
inmediato.

**El manejador solo levanta una bandera.** Nos pareció la opción más segura,
porque la señal puede llegar en cualquier punto del programa, incluso en
medio de una estructura que se está modificando. Por eso dejamos que el
trabajo real lo haga el ciclo principal, en un momento controlado. La
bandera es `volatile sig_atomic_t` para asegurarnos de que siempre se lea
desde memoria y se escriba de una sola vez.

**El hijo no hereda el manejador.** Justo después del `fork()`, restauramos
el comportamiento por defecto de `SIGINT` en el hijo.

**Falla propia versus aborto general.** El Ctrl+C que llega desde la
terminal alcanza a todos los procesos del grupo, así que los hijos ya mueren
solos. Por eso, antes de interpretar la muerte de un hijo revisamos primero
la bandera, para no terminar cancelando ramas o reportando errores que en
realidad no corresponden.

**Arreglos de tamaño fijo.** Le pusimos un límite conocido al plan (20.000
actividades, 128 dependencias y 128 sucesores por actividad). Si se supera,
el programa simplemente lo informa y termina, en vez de fallar de forma
silenciosa.

**Semilla propia en cada hijo.** Nos dimos cuenta de que, al heredar el
estado del generador del padre, todos los hijos sortearían el mismo valor.
Por eso cada uno vuelve a sembrar la semilla usando su propio pid.
