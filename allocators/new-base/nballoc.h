#ifndef NEW_BUDDY_SYSTEM

	#define NEW_BUDDY_SYSTEM

	#include <stddef.h>

	#define CACHE_LINE_SIZE 64ULL

	#ifndef MAX_ORDER
		#define MAX_ORDER 10ULL
	#endif

	#ifndef PAGE_SIZE
		#define PAGE_SIZE 4096ULL
	#endif

	#ifndef MIN_ALLOCABLE_BYTES
		#define MIN_ALLOCABLE_BYTES PAGE_SIZE
	#endif

	#ifndef MAX_ALLOCABLE_BYTES
		#define MAX_ALLOCABLE_BYTES (MIN_ALLOCABLE_BYTES << MAX_ORDER)
	#endif

	#if (MAX_ALLOCABLE_BYTES >> MAX_ORDER) != MIN_ALLOCABLE_BYTES
		#error Definitions for MAX_ALLOCABLE_BYTES and MAX_ORDER conflict
	#endif

	#ifndef NUM_LEVELS
		#define NUM_LEVELS 16ULL // 128MB
	#endif
	
	#ifndef STACK_THRESH
		#define STACK_THRESH 32ULL // Determines the laziness of the buddy system
	#endif

	// Number of nodes, one per page
	#define TOTAL_NODES (1ULL << (NUM_LEVELS - 1))
	// Amount of memory the buddy system manages
	#define TOTAL_MEMORY (TOTAL_NODES * MIN_ALLOCABLE_BYTES)

	#define MIN(x,y) (x < y ? x : y)
	#define MAX(x,y) (x > y ? x : y)

	// Exposed APIs for memory allocation
	void *bd_xx_malloc(size_t size);
	void bd_xx_free(void* addr);

	void _debug_test_nodes();

#endif