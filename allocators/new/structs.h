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

	static const node *NULLN = &NULL_NODE;

	// the lower 6 bits in node pointers are always 0, they are used for other
	// purposes
	#define PTR_MASK (!0x3FULL)
	// nodes with a pointer marked on the lowest bit are "locked" and being 
	// removed
	#define DEL_MARK (0x1ULL)

	#define IS_MARKED(ptr) (ptr & DEL_MARK)
	#define NEXT(node) (node->next & PTR_MASK)

#endif