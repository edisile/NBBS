#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "nballoc.h"

#define THREADS 64
#define ROUNDS (1024*1024)
#define HISTORY 64

pthread_barrier_t barrier;

void *thread_job(void *arg) {
	unsigned long id = (unsigned long) arg;
	void *values[HISTORY];
	unsigned long tid = gettid();

	// printf("%d is waiting\n", arg);
	pthread_barrier_wait(&barrier);
	// printf("%d is starting\n", arg);

	for (int i = 0; i < ROUNDS; i++) {
		// printf("%d is allocating\n", arg);
		void *addr = bd_xx_malloc(32768);
		values[i % HISTORY] = addr;

		// printf("%d got %p\n", id, addr);
		// pthread_barrier_wait(&barrier);
		// usleep(100000);

		if (addr == NULL) {
			printf("%d got a big fat NULL\n", arg);
		} else {
			bd_xx_free(addr);
			// printf("%d is deallocating %p\n", arg, addr);
		}
		// usleep(100);
	}
	printf("%d is done\n", arg);

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
