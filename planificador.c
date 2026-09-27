#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s <plan.txt> <K>\n", argv[0]);
        return 1;
    }
    printf("archivo: %s, K: %s\n", argv[1], argv[2]);
    return 0;
}
