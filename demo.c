#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <N>\n", argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    if (N <= 0) {
        fprintf(stderr, "N must be a positive integer\n");
        return 1;
    }

    // Simulate work for N seconds
    for (int i = 0; i < N; i++) {
        sleep(1);
    }

    return 0;
}
