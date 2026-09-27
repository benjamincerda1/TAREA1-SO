#!/bin/bash

CANTIDAD=${1:-10000}
 
for ((i = 1; i <= CANTIDAD; i++)); do
    if (( i % 5 == 1 )); then
        # arranca una cadena nueva, sin dependencias
        echo "$i : actividad_$i : 3 :"
    else
        # se cuelga de la anterior
        echo "$i : actividad_$i : 3 : $((i - 1))"
    fi
done
