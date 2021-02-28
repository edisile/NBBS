#ifndef NEW_BUDDY_SYSTEM

	#define NEW_BUDDY_SYSTEM

	#include <stddef.h>

	#define CACHE_LINE_SIZE 64ULL
	#define MAX_ORDER 10ULL

	#ifndef MIN_ALLOCABLE_BYTES
		#define MIN_ALLOCABLE_BYTES PAGE_SIZE
	#endif

	#ifndef MAX_ALLOCABLE_BYTES
		#define MAX_ALLOCABLE_BYTES (PAGE_SIZE << MAX_ORDER)
	#endif

	#ifndef NUM_LEVELS
		#define NUM_LEVELS 16ULL
	#endif
	
	#ifndef STACK_THRESH
		#define STACK_THRESH 16ULL // Determines the laziness of the buddy system
	#endif

	// Number of nodes, one per page
	#define TOTAL_NODES (1 << (NUM_LEVELS - 1))
	// Amount of memory the buddy system manages
	#define TOTAL_MEMORY (TOTAL_NODES * PAGE_SIZE)

	// Get the index of the buddy at idx given its order
	#define BUDDY_IDX(idx, order) (idx ^ (0x1 << order))

	#define MIN(x,y) (x < y ? x : y)
	#define MAX(x,y) (x > y ? x : y)



	// Possible states a memory block could assume:
	enum states {
		HEAD = -1, // node is the head of a list, doesn't represent a page
		FREE = 0, // memory block is free
		INV, // node is invalid, block is actually part of a bigger FREE block
		OCC // memory block is in use
	};

	typedef struct _stack {
		struct _node *head;
		unsigned long len;
	} stack;

	/*
	// A more compact stack that doesn't use 128 bit CAS
	typedef struct _stack {
		union {
			struct _node *head;
			unsigned long len;
		};
	} stack;
	*/

	typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) _node {
		struct _node *prev;
		struct _node *next;
		struct _stack stack;
		
		enum states status;
		unsigned short cpu;
		unsigned short order;
	} node;

	typedef struct _cpu_zone {
		node heads[MAX_ORDER + 1];
	} cpu_zone;


	// Exposed APIs for memory allocation
	void *bd_xx_malloc(size_t size);
	void bd_xx_free(void* addr);

	void _debug_test_nodes();

#endif