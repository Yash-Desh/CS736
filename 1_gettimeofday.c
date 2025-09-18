#include <stdio.h>
#include <sys/time.h> // Required for gettimeofday

int main() {
    struct timeval tv;
    int result;

    result = gettimeofday(&tv, NULL);

    if (result == -1) {
        perror("gettimeofday");
        return 1; // Indicate an error
    }

    printf("Current time: %ld seconds, %ld microseconds since Epoch\n",
           tv.tv_sec, tv.tv_usec);

    return 0; // Indicate success
}