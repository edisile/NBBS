#ifndef NEW_BUDDY_SYSTEM_STRUCTS

	#define NEW_BUDDY_SYSTEM_STRUCTS

	#include "nballoc.h"
	#include "gcc_wrappers.h"

	// =========================================================================
	// Structs
	// =========================================================================

	// Possible states a memory block could assume:
	enum states {
		FREE = 0, // memory block is free
		INV, // node is invalid, block is actually part of a bigger FREE block
		OCC, // memory block is in use
		HEAD, // node is the head of a list, doesn't represent a page
	};

	// Possible ways a memory block could be reached
	enum reachability {
		UNLINK = 0, // node is detached from all other nodes
		LIST, // node is in list
		STACK, // node is in the stack
		BUSY, // node is in the middle of an insertion in / removal from the list
	};

	// Structure of a node, alignment to CACHE_LINE_SIZE (64B) means that the 
	// lower 6 bits of a (node *) are always 0; since x86_64 5 level paging 
	// employs only the lower 57 bits for addressing, the upper 7 bits are free 
	// to be used for other purposes
	typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) _node {
		volatile struct {
			unsigned long next: 51; // pointer to next node: <sign ext>next<000000>
			unsigned long :      5; // padding
			unsigned long order: 4; // 0 up to MAX_ORDER included, 0 <= order <= 15
			unsigned long state: 2; // FREE, INV, OCC, HEAD
			unsigned long reach: 2; // UNLINK, LIST, STACK, BUSY
		} __attribute__ ((__packed__));

		volatile union { // what field is read depends on the value of .reach
			struct _node *prev; // pointer to previous node, only if .reach != STACK
			unsigned long stack_len; // number of nodes in the stack, if .reach == STACK
		};
	} node;

	#if MAX_ORDER > 15
		#error The order field of the node supports only orders <= 15
	#endif

	// A Treiber stack, again only 51 bits are necessary to keep track of the 
	// nodes
	typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) _stack {
		volatile unsigned long head: 51; // pointer to next node: <sign ext>head<000000>
		unsigned long :      1; // padding, just to make hex prints easier to read
		volatile unsigned long aba:  12; // a counter to resolve ABA problems
	} __attribute__ ((__packed__)) stack;

	// An array of nodes that serve as heads for a list, one for each order, 
	// along with an array of stacks
	typedef struct _cpu_zone {
		node heads[MAX_ORDER + 1];
		stack stacks[MAX_ORDER + 1];
	} cpu_zone;

	// =========================================================================
	// Macros
	// =========================================================================

	// Some macros for packing and unpacking
	#define _PACK_PTR(_node_ptr) (( (unsigned long) _node_ptr << 7 ) >> 13)
	#define _SIGN_EXTEND(p) (p & (0xffULL << 56) ? (p | (0xffULL << 56)) : p)
	#define _UNPACK_PTR(p) ((node *) _SIGN_EXTEND(p << 6))

	// Get the first half of the node as an unsigned long, useful for doing CAS
	// operations
	#define GET_PACK(_ptr) (*(volatile unsigned long *) _ptr)
	
	// When read as an unsigned long the order of the fields is reversed, 
	// thank you GCC... -.-'

	#define UNPACK_NEXT(_pack) (_UNPACK_PTR((_pack & 0x7ffffffffffffULL)))
	#define UNPACK_ORDER(_pack) ((_pack >> 56) & 0xfULL)
	#define UNPACK_STATE(_pack) ((_pack >> 60) & 0x3ULL)
	#define UNPACK_REACH(_pack) ((_pack >> 62) & 0x3ULL)
	
	#define PACK_NEXT(_next_ptr) (_PACK_PTR(_next_ptr))
	#define PACK_ORDER(_order) ((unsigned long) _order << 56)
	#define PACK_STATE(_state) ((unsigned long) _state << 60)
	#define PACK_REACH(_reach) ((unsigned long) _reach << 62)

	// Build the first half of the node as an unsigned long
	#define MAKE_PACK_NODE(_next_ptr, _order, _state, _reach) ((unsigned long) \
		PACK_NEXT(_next_ptr) | PACK_ORDER(_order) | \
		PACK_STATE(_state) | PACK_REACH(_reach) )
	
	#define NEXT(__node) (_UNPACK_PTR((__node)->next))

	// Stack packing and unpacking

	#define PACK_HEAD(_head_ptr) (_PACK_PTR(_head_ptr))
	#define UNPACK_HEAD(_pack) (UNPACK_NEXT(_pack))

	#define MAKE_PACK_STACK(_stack_ptr, _new_head_ptr) ((unsigned long) \
		PACK_HEAD(_new_head_ptr) | ((unsigned long) (_stack_ptr)->aba + 1) << 52)
	
	#define HEAD(_stack_ptr) (_UNPACK_PTR((_stack_ptr)->head))
	#define LEN(_stack_ptr) (HEAD(_stack_ptr)->stack_len)

	static inline int set_pack_node(node *n, unsigned long old, unsigned long new) {
		return bCAS((volatile unsigned long *) n, old, new);
	}

	static inline int set_pack_stack(stack *s, unsigned long old, unsigned long new) {
		return bCAS((volatile unsigned long *) s, old, new);
	}

	// =========================================================================
	// Constants
	// =========================================================================

	// Null-object pattern, avoid having nodes in the list pointing to NULL
	static const node NULL_NODE = {
		// .next = PACK_NEXT(&NULL_NODE),
		// ^ this seems to be impossible to do
		.order = -1,
		.state = INV,
		.reach = STACK,
		.stack_len = 0,
	}; // TODO: is this necessary or is a NULL check enough?

	#define NULLN ((node *) &NULL_NODE)
#endif