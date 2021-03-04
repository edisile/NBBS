#ifndef NEW_BUDDY_SYSTEM_STRUCTS

	#define NEW_BUDDY_SYSTEM_STRUCTS

	#include "nballoc.h"
	#include "gcc_wrappers.h"

	// =========================================================================
	// Structs
	// =========================================================================

	// Possible states a memory block could assume:
	enum states {
		HEAD = -1, // node is the head of a list, doesn't represent a page
		FREE = 0, // memory block is free
		INV, // node is invalid, block is actually part of a bigger FREE block
		OCC // memory block is in use
	};
	
	/*
	typedef struct _stack {
		struct _node *head;
		unsigned long len;
	} stack;
	*/
	
	// A more compact stack that doesn't use 128 bit CAS
	typedef union _stack {
			struct _node *head;
			unsigned long len;
	} stack;

	typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) _node {
		struct _node *prev;
		struct _node *next;
		union _stack stack;
		
		enum states status;
		unsigned short cpu;
		unsigned short order;
	} node;

	typedef struct _cpu_zone {
		node heads[MAX_ORDER + 1];
	} cpu_zone;

	// =========================================================================
	// Constants
	// =========================================================================

	// Null-object pattern, avoid having nodes in the list pointing to NULL
	static const node NULL_NODE = {
		.prev = (node *) &NULL_NODE, // casting is needed to suppress a warning
		.next = (node *) &NULL_NODE,
		.stack.len = 0,
		.status = INV,
		.cpu = -1,
		.order = -1,
	};

	#define NULLN ((node *) &NULL_NODE)

	// the lower 6 bits in node pointers are always 0, they are used for other
	// purposes
	#define PTR_MASK (~0x3FULL)
	// nodes with a pointer marked on the lowest bit are "locked" and being 
	// removed
	#define DEL_MARK (0x1ULL)

	#define IS_MARKED(ptr) ((unsigned long) ptr & DEL_MARK)
	#define MARKED(ptr) ((unsigned long) ptr | DEL_MARK)
	#define UNMARKED(ptr) ((unsigned long) ptr & PTR_MASK)
	#define NEXT(node_ptr) ((node *) ((unsigned long) node_ptr->next & PTR_MASK))

	#define STACK_HEAD(new, old) (UNMARKED(new) | (((unsigned long) old + 1) & ~PTR_MASK))

	#define IN_LIST(node_ptr) (NEXT(node_ptr) != NULLN && node_ptr->prev != NULLN)
	#define NOT_IN_LIST(node_ptr) (NEXT(node_ptr) == NULLN && node_ptr->prev == NULLN)

	static inline int mark_ptr(node **ptr_addr) {
		if (*ptr_addr == NULLN) return 0; // TODO: add an assert here

		return bCAS(ptr_addr, UNMARKED(*ptr_addr), MARKED(*ptr_addr));
	}

	static inline int unmark_ptr(node **ptr_addr) {
		if (*ptr_addr == NULLN) return 0; // TODO: add an assert here

		return bCAS(ptr_addr, MARKED(*ptr_addr), UNMARKED(*ptr_addr));
	}

#endif