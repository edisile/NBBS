#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "nballoc.h"

#define THREADS 64
#define ROUNDS 1048576

pthread_barrier_t barrier;

void *thread_job(void *arg) {
	unsigned long id = (unsigned long) arg;

	// printf("%d is waiting\n", arg);
	pthread_barrier_wait(&barrier);
	// printf("%d is starting\n", arg);

	for (int i = 0; i < ROUNDS; i++) {
		// printf("%d is allocating\n", arg);
		void *addr = bd_xx_malloc(1000);

		if (addr == NULL) {
			printf("%d got a big fat NULL\n", arg);
		} else {
			bd_xx_free(addr);
		}
		// usleep(100);
		// printf("%d is deallocating %p\n", arg, addr);
	}

	return NULL;
}

int main(int argc, char const *argv[]) {
	pthread_t threads[THREADS];

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
