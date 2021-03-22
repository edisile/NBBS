#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "nballoc.h"

#define THREADS 16
#define ROUNDS 1024
#define HISTORY 12288
#define SIZE (1ULL << 12)

pthread_barrier_t barrier;

void *thread_job(void *arg) {
	unsigned long id = (unsigned long) arg;
	unsigned long fail = 0;
	void *values[HISTORY];
	unsigned long tid = gettid();

	// printf("%d is waiting\n", arg);
	// pthread_barrier_wait(&barrier);
	// printf("%d is starting\n", arg);

	for (int i = 0; i < ROUNDS; i++) {
		// printf("%d is allocating\n", arg);
		int j = 0;
		while (j < HISTORY) {
			values[j] = bd_xx_malloc(SIZE);
			if (values[j] != NULL) {
				j++;
			} else {
				fail++;
			}
		}

		for (int j = 0; j < HISTORY; j++) {
			bd_xx_free(values[j]);
		}
	}
	printf("%d is done; got %lu failures\n", arg, fail);

	return NULL;
}

int main(int argc, char const *argv[]) {
	pthread_t threads[THREADS];
	printf("Launching %lu threads allocating %lu blocks of size %luB\n", THREADS, HISTORY, SIZE);
	fflush(stdout);

	pthread_barrier_init(&barrier, NULL, THREADS);

	for (unsigned long i = 0; i < THREADS; i++) {
		pthread_create(&threads[i], NULL, thread_job, (void *)i);
	}

	for (unsigned long i = 0; i < THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	pthread_barrier_destroy(&barrier);

	_debug_test_nodes();
	
	return 0;
}
