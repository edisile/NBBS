#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>

#include "../../utils/utils.h"
#include "gcc_wrappers.h"
#include "nballoc.h"
#include "structs.h"

#ifdef TSX
	// Support for Intel TSX instructions
	#include "transaction.h"
#endif

// Some global variables, filled out by init():
static unsigned long ALL_CPUS; // number of (configured) CPUs on the system
static unsigned long nodes_per_cpu;
static unsigned char *memory = NULL; // pointer to the memory managed by the buddy system
static cpu_zone *zones = NULL; // a list of arrays of heads and stacks
static node *nodes = NULL; // a list of all nodes, one for each page
// FIXME: find a way to get rid of this
static lock *locks = NULL; // to emulate disabling preemption we take a lock on the cpu_zone

#define INDEX(n_ptr) (n_ptr - nodes)
#define BUDDY_INDEX(idx, order) (idx ^ (0x1 << order))
#define OWNER(n_ptr) (INDEX(n_ptr) / nodes_per_cpu)

// =============================================================================
// Lock ops
// =============================================================================

static inline int try_lock(lock *l) {
	if (l->var != 0)
		return 0;
	
	return bCAS(&l->var, 0, 1);
}

static inline void unlock_lock(lock *l) {
	assert(l->var == 1);
	// while (!bCAS(&l->var, 1, 0)); // it's not necessary to be atomic
	l->var = 0; // lazy lock release
}

static inline void init_lock(lock *l) {
	l->var = 0;
}

// =============================================================================
// List ops
// =============================================================================

static void debug(unsigned long p) {
	printf("pack: %p\n", p);
	printf("next : %p ", UNPACK_NEXT(p));
	printf("state: %p ", UNPACK_STATE(p));
	printf("order: %p ", UNPACK_ORDER(p));
	printf("reach: %p\n", UNPACK_REACH(p));
}

static int insert_node(node *n) {
	retry_acquire:;
	unsigned cpu = OWNER(n);
	node *target = &zones[cpu].heads[n->order];

	assert(target->state == HEAD && target->reach == LIST && 
		n->order == target->order);

	unsigned long old = GET_PACK(n);
	if (UNPACK_REACH(old) != UNLINK || UNPACK_STATE(old) != FREE) {
		return 0;
	}
	
	unsigned long new = MAKE_PACK_NODE(NEXT(target), UNPACK_ORDER(old), 
			UNPACK_STATE(old), BUSY);

	if (!set_pack_node(n, old, new))
		goto retry_acquire;
	
	retry_next:;
	// now only the current thread can insert the node
	n->prev = target;
	
	old = GET_PACK(target);
	new = MAKE_PACK_NODE(n, UNPACK_ORDER(old), HEAD, LIST);
	
	if (!set_pack_node(target, old, new)) {
		// since target is always a HEAD and its order, status and reachability 
		// can't change there must have been some concurrent insert or remove; 
		// update the next pointer of n
		n->next = PACK_NEXT(NEXT(target));
		goto retry_next;
	}

	// can a node be removed at this point while it's being added? nope
	// n->reach was set as BUSY before n was visible in the list so it would be 
	// impossible to remove it
	
	retry_prev:
	if (!bCAS(&(NEXT(n)->prev), target, n))
		goto retry_prev;

	retry_release:
	old = GET_PACK(n);
	new = MAKE_PACK_NODE(NEXT(n), UNPACK_ORDER(old), UNPACK_STATE(old), LIST);

	// MAYBE: could this be done in a lazier way? e.g. just n->reach = LIST
	if (!set_pack_node(n, old, new))
		goto retry_release;
	// insertion is done, anyone can remove the node from the list now

	return 1;
}

static int remove_node(node *n) {
	// first try to mark n->next, if it's marked already leave and return false
	retry_acquire:;
	unsigned long old = GET_PACK(n);
	node *next = UNPACK_NEXT(old);
	if (UNPACK_REACH(old) != LIST || UNPACK_STATE(old) == FREE)
		return 0; // someone else is removing or has removed the node
	
	unsigned long new = MAKE_PACK_NODE(UNPACK_NEXT(old), UNPACK_ORDER(old), 
			UNPACK_STATE(old), BUSY);

	if (!set_pack_node(n, old, new))
		goto retry_acquire;

	retry_1: ;
	node *prev = n->prev;
	
	// if (NEXT(prev) != n) {
	// 	// there's been an insertion in front of n
	// 	// TODO: add an assert, this should never happen while removing the node
	// 	// unless it's done in a transaction (?)
	// 	if (!unmark_ptr(&n->next))
	// 		goto retry_1;

	// 	return;
	// }

	old = GET_PACK(prev);
	assert(UNPACK_NEXT(old) == n && UNPACK_REACH(old) == LIST);

	new = MAKE_PACK_NODE(next, UNPACK_ORDER(old), UNPACK_STATE(old), LIST);

	if (!set_pack_node(prev, old, new)) {
		goto retry_1;
	}

	retry_2:
	if (!bCAS(&next->prev, n, prev))
		goto retry_2;

	retry_release: // MAYBE: does this have to be atomic?
	old = GET_PACK(n);

	assert(UNPACK_REACH(old) == BUSY);
	
	new = MAKE_PACK_NODE(NULLN, UNPACK_ORDER(old), UNPACK_STATE(old), UNLINK);
	if (!set_pack_node(n, old, new))
		goto retry_release;
	n->prev = NULLN;
	
	return 1;
}

// =============================================================================
// Stack ops
// =============================================================================

static void stack_init(stack *s) {
	s->head = PACK_HEAD(NULLN);
	s->aba = 0;
}

static int stack_push(stack *s, node *n) {
	retry_acquire:;
	unsigned long old = GET_PACK(n);
	if (UNPACK_REACH(old) != UNLINK || UNPACK_STATE(old) != OCC) {
		printf("Stack push failed:\n");
		debug(old);
		return 0;
	}
	
	unsigned long new = MAKE_PACK_NODE(HEAD(s), UNPACK_ORDER(old), OCC, STACK);
	if (!set_pack_node(n, old, new))
		goto retry_acquire;

	retry_push:;
	unsigned long old_head = GET_PACK(s);
	
	n->next = PACK_NEXT(UNPACK_HEAD(old_head));
	n->stack_len = LEN(s) + 1;
	
	unsigned long new_head = MAKE_PACK_STACK(s, n);
	
	if (!set_pack_stack(s, old_head, new_head)) {
		// n->next = PACK_NEXT(NULLN);
		// n->reach = UNLINK;
		// n->prev = NULLN;
		goto retry_push;
	}

	return 1;
}

static node *stack_pop(stack *s) {
	retry:;
	unsigned long old = GET_PACK(s);
	node *head = UNPACK_HEAD(old);

	if (head == NULLN)
		return NULLN;

	if (head->reach != STACK)
		goto retry; // a thread might sleep with an old value

	node *next = NEXT(head);
	unsigned long new = MAKE_PACK_STACK(s, next);

	if (!set_pack_stack(s, old, new)) {
		goto retry;
	}

	// clean head fields
	head->next = PACK_NEXT(NULLN);
	head->reach = UNLINK;
	head->prev = NULLN;

	return head;
}

static node *stack_clear(stack *s) {
	retry:;
	unsigned long old = GET_PACK(s);
	node *head = UNPACK_HEAD(old);

	if (head == NULLN)
		return NULLN;

	if (head->reach != STACK)
		goto retry; // a thread might sleep with an old value

	// make the stack point to NULLN
	unsigned long new = MAKE_PACK_STACK(s, NULLN);

	if (!set_pack_stack(s, old, new)) {
		goto retry;
	}

	// now (hopefully) only the current thread can access head and all the 
	// following nodes
	return head;
}

// =============================================================================
// Initial setup
// =============================================================================

// Initialize the state of the nodes
static void setup_memory_blocks() {
	for (unsigned long i = 0; i < TOTAL_NODES; i++) {
		nodes[i] = (node) {
			.next = PACK_NEXT(NULLN),
			.prev = NULLN,
			.state = INV,
			.reach = UNLINK,
			.order = 0,
		};
		
		// Initially only MAX_ORDER-sized blocks are marked as available
		if (i % (0x1 << MAX_ORDER) == 0) {
			nodes[i].state = FREE;
			nodes[i].order = MAX_ORDER;
		}
	}

	printf("\tall nodes filled, inserting MAX_ORDER blocks\n");

	// insert the blocks in the buddy system
	for (unsigned long i = 0; i < TOTAL_NODES; i += (0x1 << MAX_ORDER)) {
		insert_node(&nodes[i]);
	}
}

static void setup_memory_state() {
	// TODO: split a few MAX_ORDER blocks and put them in the stacks
}

// Initialize the structure of the buddy system
__attribute__ ((constructor)) void premain() {
	printf("initializing buddy system state:\n");
	// Get the number of CPUs configured on the system; there might be some 
	// CPUs that are configured but not available (e.g. shut down to save power)
	ALL_CPUS = sysconf(_SC_NPROCESSORS_CONF);
	nodes_per_cpu = TOTAL_NODES / ALL_CPUS;

	printf("buddy system manages %luB of memory\n", TOTAL_MEMORY);
	// Allocate the memory for all necessary components
	memory = aligned_alloc(PAGE_SIZE, TOTAL_MEMORY);
	nodes = aligned_alloc(PAGE_SIZE, sizeof(node) * TOTAL_NODES);
	zones = aligned_alloc(PAGE_SIZE, sizeof(cpu_zone) * ALL_CPUS);
	locks = malloc(sizeof(lock) * ALL_CPUS);
	
	// Cleanup if any allocation failed
	if (memory == NULL || nodes == NULL || zones == NULL || locks == NULL) {
		if(memory != NULL) free(memory);
		if(nodes != NULL) free(nodes);
		if(zones != NULL) free(zones);

		if(locks != NULL) free(locks);
		fprintf(stderr, "An allocation failed:\n\tmemory: %p\n\tnodes: %p\n\tzones: %p\n\tlocks: %p\n", 
				memory, nodes, zones, locks);

		abort();
	}

	printf("\tallocations done\n");

	// memset(memory, 0, TOTAL_MEMORY); // not necessary

	// All the structures needed are allocated, time to initialize them

	// First initialize and connect all the heads in the cpu_zones
	for (int i = 0; i < ALL_CPUS; i++) {
		init_lock(&locks[i]);

		for (int j = 0; j <= MAX_ORDER; j++) {

			zones[i].heads[j] = (node) {
				.next = PACK_NEXT(&zones[(i + 1) % ALL_CPUS].heads[j]),
				.order = j,
				.state = HEAD,
				.reach = LIST,
				.prev = &zones[(i + ALL_CPUS - 1) % ALL_CPUS].heads[j],
			};

			stack_init(&zones[i].stacks[j]);
		}
	}

	printf("\tlist heads set up, inserting memory blocks\n");

	setup_memory_blocks();
	setup_memory_state();

	printf("buddy system is ready\n");
}

// =============================================================================
// Convenience functions
// =============================================================================

static inline int change_state(node *n, short old_state, short new_state, 
		short exp_reach, short exp_order) {
	int ok = 0;
	unsigned long old = GET_PACK(n);

	if (UNPACK_STATE(old) != old_state || UNPACK_REACH(old) != exp_reach || 
			UNPACK_ORDER(old) != exp_order) {
		return 0;
	}

	unsigned long new = MAKE_PACK_NODE(UNPACK_NEXT(old), exp_order, new_state, 
			exp_reach);

	return set_pack_node(n, old, new);
}

static inline int change_order(node *n, short old_order, short new_order, 
		short exp_state, short exp_reach) {
	
	assert(exp_reach != STACK && exp_state != HEAD);

	unsigned long old = GET_PACK(n);

	if (UNPACK_STATE(old) != exp_state || UNPACK_REACH(old) != exp_reach || 
			UNPACK_ORDER(old) != old_order) {
		return 0;
	}

	unsigned long new = MAKE_PACK_NODE(UNPACK_NEXT(old), new_order, exp_state, 
			exp_reach);

	return set_pack_node(n, old, new);
}

static inline unsigned long cpu_id() {
	unsigned long id;
	__asm__ __volatile__(
		"rdtscp;"			// serializing read of tsc also puts CPU ID in rcx
		: "=c"(id)			// output to id variable
		: 					// no inputs needed
		: "%rax", "%rdx"	// rax and rdx are clobbered
	);

	// id is not just the CPU core the thread is executed on, it's a value read 
	// from a MSR managed by the OS; in Linux the value is interpreted as 
	// (NUMA_NODE_ID << 12) | (CPU_ID)

	return id % ALL_CPUS; // TODO: fix this ^
}

// Get the minimum allocation order that satisfies a request of size s
static inline size_t closest_order(size_t s) {
	if (s <= MIN_ALLOCABLE_BYTES) return 0;
	
	return 64 - __builtin_clzl(s - 1) - 12;
}

// =============================================================================
// Allocation
// =============================================================================

static inline int move_node(node *n) {
	int ok = 0;
	unsigned owner = OWNER(n);
	if (try_lock(&locks[owner])) {
		// FIXME: this uses locks atm, not very poggers
		
		ok = remove_node(n);
		
		if (ok) {
			ok = insert_node(n);
		}

		unlock_lock(&locks[owner]);
	}
}

// Try to allocate a FREE node, returns 1 if allocation succeeded and 0 
// otherwise; if allocation fails *next_node_ptr is set to point to the next 
// node to be examined
static inline int handle_free_node(node *n, short order, node **next_node_ptr) {
	short n_order = n->order;
	node *next = NEXT(n);
	int ok = 0;
	
	if (n_order >= order) {
		// try to allocate the node
		ok = change_state(n, FREE, OCC, LIST, n_order);
		
		if (!ok && n->state == FREE) {
			// it failed but should try again
			*next_node_ptr = n;
		} else {
			*next_node_ptr = next;
		}

	} else {
		// n->order < order <= target_order means the node is in the wrong list
		*next_node_ptr = next;
		move_node(n);
	}

	return ok;
}

// Attempt to remove a node from the list, returns 1 if removal was successful 
// or 0 otherwise; *next_node_ptr is set to point to the next node to visit
static inline int handle_occ_node(node *n, node **next_node_ptr) {
	node *next = NEXT(n);
	int ok = 0;
	unsigned owner = OWNER(n);

	if (try_lock(&locks[owner])) {
		// FIXME: this uses locks atm, not very poggers
		
		ok = remove_node(n);
		// remove_node fails if (n->state == FREE || n->reach != LIST)

		unlock_lock(&locks[owner]);
	}

	// check if remove_node failed and/or there's been a concurrent free the 
	// current thread should check this node again
	if (!ok && n->state == FREE)
		*next_node_ptr = n;
	else
		*next_node_ptr = next;

	return ok;
}

// Navigate the list in search for a free node and allocate it; returns either 
// a pointer to an actual node n with n->state == OCC or NULLN if no block that 
// can satisfy the request is found
static node *get_free_node(size_t order, unsigned cpus_limit) {
	stack *s;
	node *n, *popped = NULLN, *next_node = NULLN;
	size_t target_order;
	
	restart:;
	unsigned cpu = cpu_id();
	// TODO: ^ this is wrong with more that 1 NUMA node, see the comment in cpu_id

	target_order = order;

	next_order:;
	n = &zones[cpu].heads[target_order];

	unsigned cpus_visited = 0;
	for ( ; ; ) {
		// this might happen if a thread sleeps holding a reference to a node 
		// that gets removed before the thread wakes up again
		if (n->reach == UNLINK || n->reach == STACK) {
			goto restart;
		}

		switch (n->state) {
			case HEAD:
				s = &zones[(cpu + cpus_visited) % ALL_CPUS].stacks[target_order];
				cpus_visited++;
				
				if (cpus_visited < cpus_limit) {
					popped = stack_pop(s);
					if (popped != NULLN) {
						n = popped;

						assert(n->state == OCC && n->reach == UNLINK && 
								n->prev == NULLN && NEXT(n) == NULLN);
						
						goto done;
					}
				} else {
					// all cpu zones have been visited without finding anything
					target_order++;
					
					if (target_order <= MAX_ORDER) {
						goto next_order;
					} else {
						// There's no memory to satisfy this allocation
						n = NULLN;
						goto done;
					}
				}
				n = NEXT(n);

				break;

			case FREE:
				if (handle_free_node(n, target_order, &next_node)) {
					assert(n->state == OCC);
					goto done;
				}
				
				n = next_node;
				break;
			
			case INV:
				n = NEXT(n);
				break;
			
			case OCC:
				handle_occ_node(n, &next_node);
				n = next_node;
				break;
			
			
			default:
				fprintf(stderr, "There's a node with a wrong status value:\n\t%p: %p",
					n, n->state);
				abort();
		}

	}

	done:;
	// printf("Occupied node: %p\n", n);
	return n;
}

// Try to get a memory block from the current CPU zone
static inline node *get_free_node_fast(size_t order) {
	// 2 is because the check that counts the numbers of CPU zones visited is <
	return get_free_node(order, 2);
}

// Repeatedly split n in half until it reaches target_order; the second half 
// will be made valid and inserted in a list or a stack
static void split_node(node *n, size_t target_order) {
	size_t current_order = n->order;
	unsigned long n_index = INDEX(n);
	unsigned owner = OWNER(n);

	// First set the new order of n atomically
	// TODO: this could be done when occupying the node in get_free_node saving 
	// the trouble of doing two atomic CAS calls; give get_free_node another 
	// argument a (size_t *) where the current order of the occupied node will 
	// be put after the allocation
	retry_set_order:;
	assert(n->state == OCC && n->reach != STACK);

	if (!change_order(n, current_order, target_order, OCC, n->reach))
		goto retry_set_order;

	size_t buddy_order = current_order;
	while (buddy_order != target_order) {
		buddy_order--;
		// split n in half, validate its second half and insert it into the 
		// correct list / stack
		unsigned long buddy_index = BUDDY_INDEX(n_index, buddy_order);
		node *buddy = &nodes[buddy_index];
		
		retry_buddy:;
		unsigned long old = GET_PACK(buddy);

		assert(UNPACK_REACH(old) == UNLINK && UNPACK_STATE(old) == INV);
		
		int ok = 0;

		if (try_lock(&locks[owner])) {
			// TODO: this path is used a lot and needs 5 bCAS calls to complete; 
			// make a fast path similar to the one used in _free

			// FIXME: this uses locks atm, not very poggers
			unsigned long new = MAKE_PACK_NODE(NULLN, buddy_order, 
					FREE, UNLINK);
			
			ok = set_pack_node(buddy, old, new); // TODO: can this even fail?
			if (ok)
				insert_node(buddy);
			
			unlock_lock(&locks[owner]);

		} else {
			
			unsigned long new = MAKE_PACK_NODE(NULLN, buddy_order, 
					OCC, UNLINK);

			ok = set_pack_node(buddy, old, new); // TODO: can this even fail?
			if (ok)
				stack_push(&zones[owner].stacks[buddy_order], buddy);

		}

		if (!ok)
			goto retry_buddy;
	}
}

static void *_alloc(size_t size) {
	size_t order = closest_order(size);

	node *n = get_free_node_fast(order);

	if (n == NULLN)
		n = get_free_node(order, ALL_CPUS);
	
	if (n == NULLN) return NULL;

	// check whether n is bigger than expected and needs to be fragmented
	if (n->order > order) {
		split_node(n, order);
	}
	
	// while the split was taking place someone might have removed n gaining 
	// this thread some time
	if (n->reach == UNLINK) goto done;

	#ifdef TSX
	int ok = TRY_TRANSACTION({
		if (n->reach == LIST) {
			// the node was taken from the list and is still there
			remove_node(n)
		}
	})
	
	if (ok) goto done
	#endif

	assert(n->state == OCC);

	// fallback path, check if the node is owned by the current executing CPU; 
	// if it is disable preemption and remove it
	unsigned owner = OWNER(n);
	if (try_lock(&locks[owner])) {
		// FIXME: this uses locks atm, not very poggers
		if (n->reach == LIST) {
			// the node was taken from the list and is still there
			remove_node(n);
		}
		unlock_lock(&locks[owner]);
	} else {
		// not a problem, when a thread on the correct CPU reaches this node 
		// it will try to remove it
	}

	done:
	return (void *) (memory + INDEX(n) * MIN_ALLOCABLE_BYTES);
}

void* bd_xx_malloc(size_t size) {
	if ((size / MIN_ALLOCABLE_BYTES) > (0x1 << MAX_ORDER))
		return NULL; // this size is bigger than the max the allocator supports

	return _alloc(size);
}

// =============================================================================
// Deallocation
// =============================================================================

static node *try_coalescing(node *n) {
	retry1:;
	short order = n->order;

	node *buddy = &nodes[BUDDY_INDEX(INDEX(n), order)];
	// fast path: if the buddy is not free or wrong order just mark n as free
	if (buddy->state != FREE || buddy->order != order) {
		if (!change_state(n, OCC, FREE, UNLINK, order))
			goto retry1;
		
		return n;
	}

	if (!change_state(n, OCC, INV, UNLINK, order))
		goto retry1;
	
	for ( ; ; ) {
		if (order == MAX_ORDER) break; // can't go higher than this

		node *buddy = &nodes[BUDDY_INDEX(INDEX(n), order)];
		
		unsigned long old = GET_PACK(buddy);
		if (UNPACK_STATE(old) != FREE || UNPACK_ORDER(old) != order) {
			break;
		}

		int ok = change_state(buddy, FREE, INV, UNPACK_REACH(old), order);
		
		if (ok) {
			// both buddies are marked as INV and have been logically merged
			remove_node(buddy);
			
			// the leftmost buddy is the one that "absorbs" the other
			n = MIN(n, buddy);

			order++;
			n->order = order;
		}
	}

	retry2:;
	if (!change_state(n, INV, FREE, UNLINK, order))
		goto retry2;

	return n;
}

static void _free(void *addr) {
	unsigned long index = ((unsigned char *)addr - memory) / MIN_ALLOCABLE_BYTES;
	node *n = &nodes[index];
	unsigned owner = OWNER(n);
	stack *s = &zones[owner].stacks[n->order];

	assert(n->state == OCC && n->reach != STACK);

	retry_fast:;
	// fast path: if there's just few elements in the stack push there and 
	// don't even care about doing any other work
	if (LEN(s) <= (STACK_THRESH / 2) && n->reach == UNLINK) {
		int ok = stack_push(s, n);
		if (!ok)
			goto retry_fast;
		
		return;
	}
	
	if (try_lock(&locks[owner])) {
		// FIXME: this uses locks atm
		if (n->reach == LIST) {
			// node might not have been removed before being freed
			int ok = remove_node(n);
		}
		
		n = try_coalescing(n);
		insert_node(n);

		unlock_lock(&locks[owner]);

	} else {
		int ok = 0;
		
		while (ok == 0 && n->state == OCC) {
			switch (n->reach) {
				case LIST:
					// node is still in the list as it wasn't removed, just mark as free
					ok = change_state(n, OCC, FREE, LIST, n->order);
					break;
				
				case UNLINK:
					ok = stack_push(s, n);
					break;
				
				case BUSY:
					// another thread is currently removing the node from the 
					// list, just change the state to FREE while the removal is 
					// taking place
					// ok = change_state(n, OCC, FREE, BUSY, n->order);
					// TODO: is this actually correct? Yes. Does it send 
					// handle_free_node in an infinite loop? Yes.
					
					// FIXME: yep, just busy loop
					break;
				
				default:
					// only remaining option is STACK, which shouldn't happen
					printf("Can't free node with n->reach %p\n", n->reach);
					break;
			}
		}
	}
}

void bd_xx_free(void *addr) {
	if ((void *) memory <= addr && addr < (void *) (memory + TOTAL_MEMORY))
		_free(addr);
}

// =============================================================================
// Debug
// =============================================================================

void _debug_test_nodes() {
	printf("Checking if all nodes are ok...\n");
	unsigned long free_count = 0;
	unsigned long occ_count = 0;
	unsigned long inv_count = 0;

	for (unsigned long i = 0; i < TOTAL_NODES; i++) {
		node *n = &nodes[i];

		if (n->state == FREE && n->reach != LIST) {
			printf("FREE node lost: %p { status = %u, order = %u, cpu = %u, next = %p, prev = %p}\n", 
				n, n->state, n->order, OWNER(n), NEXT(n), n->prev);
		}

		if (n->state == OCC && n->reach != STACK) {
			printf("OCC node out of stack: %p { status = %u, order = %u, cpu = %u, next = %p, prev = %p}\n", 
				n, n->state, n->order, OWNER(n), NEXT(n), n->prev);
		}

		switch (n->state) {
			case FREE:
				free_count++;
				break;
			case OCC:
				occ_count++;
				break;
			case INV:
				inv_count++;
				break;
			default:
				break;
		}
	}

	printf("\tFREE: %lu\n\tOCC: %lu\n\tINV: %lu\n", free_count, occ_count, inv_count);

	printf("Done!\n");
}

#ifdef TEST_MAIN
int main(int argc, char const *argv[]) {
	unsigned long expected_blocks = nodes_per_cpu >> MAX_ORDER;
	printf("System has %lu CPUS; there's %lu pages per CPU, expecting %lu MAX_ORDER blocks per CPU\n", 
		ALL_CPUS, nodes_per_cpu, expected_blocks);

	// Check the structure of the list
	for (int j = 0; j <= MAX_ORDER; j++) {
		node *n = &zones[0].heads[j];
		int i = 0;
		int blocks = 0;

		printf("Order %2d: ", j);
		while (i <= ALL_CPUS) {
			switch (n->state) {
				case HEAD:
					printf("|HEAD %d|-", i);
					i++;
					blocks = 0;
					break;
				case FREE:
					blocks++;
					printf("|o|-");
					break;
				default:
					printf("\nsomething's wrong\n");
					printf("\tnode: { status = %u, order = %u, cpu = %u}\n", n->state, n->order, OWNER(n));
			}

			n = NEXT(n);
		}
		printf("\n");
	}

	// Try some stack ops
	do { // just to collapse the block in the editor
		printf("\nTrying some stack operations\n");

		node *n = NEXT(&zones[0].heads[MAX_ORDER]);
		remove_node(n);

		stack_push(&zones[0].stacks[MAX_ORDER], n);
		printf("n: %p, stack head: %p\n", n, zones[0].stacks[MAX_ORDER]);
		
		n = stack_pop(&zones[0].stacks[MAX_ORDER]);
		printf("n: %p, stack head: %p\n", n, zones[0].stacks[MAX_ORDER]);

		stack_push(&zones[0].stacks[MAX_ORDER], n);
		printf("n: %p, stack head: %p\n", n, zones[0].stacks[MAX_ORDER]);
		
		n = stack_clear(&zones[0].stacks[MAX_ORDER]);
		printf("n: %p, stack head: %p\n", n, zones[0].stacks[MAX_ORDER]);

		n->state = OCC;
		n->reach = UNLINK;
		stack_push(&zones[0].stacks[MAX_ORDER], n);
	} while (0);

	// Try some allocations
	do { // just to collapse the block in the editor
		printf("\nTrying some allocations\n");
		unsigned char *addr = bd_xx_malloc(100); // we expect a 1 page long area -> order 0
		unsigned index = (addr - memory) / MIN_ALLOCABLE_BYTES;
		node *n = &nodes[index];
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n, n->state, n->order, OWNER(n));

		unsigned char *addr_2 = bd_xx_malloc(4096*12); // we expect a 16 page long area -> order 4
		unsigned index_2 = (addr_2 - memory) / MIN_ALLOCABLE_BYTES;
		node *n_2 = &nodes[index_2];
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->state, n_2->order, OWNER(n_2));

		printf("\nLet's try to release\n");
		bd_xx_free(addr_2);
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->state, n_2->order, OWNER(n_2));

		bd_xx_free(addr);
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n, n->state, n->order, OWNER(n));
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->state, n_2->order, OWNER(n_2));

	} while (0);

	return 0;
}
#endif