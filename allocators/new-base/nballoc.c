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
#include "declarations.h"
#include "futex.h"
#include "../../utils/stats_macros.h"

// Some global variables, filled out by premain():
static unsigned long ALL_CPUS; // number of (configured) CPUs on the system
static unsigned long nodes_per_cpu;
static unsigned char *memory = NULL; // pointer to the memory managed by the buddy system
static cpu_zone *zones = NULL; // a list of arrays of heads and stacks
static node *nodes = NULL; // a list of all nodes, one for each page
// FIXME: find a way to get rid of this vvv
static lock *locks = NULL; // to emulate disabling preemption we take a lock on the cpu_zone
static pthread_t *workers;
static futex_t *cleanup_requested;

#ifdef FAST_FREE
extern void update_model(unsigned);
extern int should_fast_free(unsigned);
#endif

#define INDEX(n_ptr) (n_ptr - nodes)
#define BUDDY_INDEX(idx, order) (idx ^ (0x1 << order))
#define OWNER(n_ptr) ((INDEX(n_ptr) / nodes_per_cpu) % ALL_CPUS)

// =============================================================================
// Lock ops
// =============================================================================

static inline int try_lock(lock *l) {
	if (l->var != 0)
		return 0;
	
	return __sync_lock_test_and_set(&l->var, 1) == 0;
}

static inline void unlock_lock(lock *l) {
	assert(l->var == 1);
	
	__sync_lock_release(&l->var);
}

static inline void init_lock(lock *l) {
	l->var = 0;
}

// =============================================================================
// List ops
// =============================================================================

static void debug(unsigned long p) {
	printf("pack: %lx\n", p);
	printf("next : %p ", UNPACK_NEXT(p));
	printf("state: %llx ", UNPACK_STATE(p));
	printf("order: %llx ", UNPACK_ORDER(p));
	printf("reach: %llx\n", UNPACK_REACH(p));
}

static int insert_node(node *n) {
	retry_acquire:;
	node *target = &n->owner_heads[n->order];

	assert(target->state == HEAD);
	assert(target->reach == LIST);
	assert(n->order == target->order);

	unsigned long old = GET_PACK(n);
	if (UNPACK_REACH(old) != UNLINK || UNPACK_STATE(old) != FREE) {
		return 0;
	}
	
	unsigned long new = MAKE_PACK_NODE(NEXT(target), UNPACK_ORDER(old), 
			UNPACK_STATE(old), LIST);

	if (!set_pack_node(n, old, new))
		goto retry_acquire;
	
	// no need for atomics when updating prev pointers, as no more than one 
	// thread at a time uses them
	n->prev = target;
	
	retry_insert:;
	// now only the current thread can insert the node
	
	old = GET_PACK(target);
	new = MAKE_PACK_NODE(n, UNPACK_ORDER(old), HEAD, LIST);
	
	if (!set_pack_node(target, old, new))
		goto retry_insert;
	
	NEXT(n)->prev = n; // again no need for atomics

	return 1;
}

static int remove_node(node *n) {
	// first try to mark n->next, if it's marked already leave and return false
	retry_acquire:;
	unsigned long old = GET_PACK(n);
	node *next_n = UNPACK_NEXT(old);
	if (UNPACK_REACH(old) != LIST || UNPACK_STATE(old) == FREE)
		return 0; // someone else is removing or has removed the node
	
	unsigned long new = MAKE_PACK_NODE(UNPACK_NEXT(old), UNPACK_ORDER(old), 
			UNPACK_STATE(old), MARK);

	if (!set_pack_node(n, old, new))
		goto retry_acquire;

	retry_next:;
	node *prev_n = n->prev;
	
	old = GET_PACK(prev_n);
	assert(UNPACK_NEXT(old) == n);
	assert(UNPACK_REACH(old) == LIST);

	new = MAKE_PACK_NODE(next_n, UNPACK_ORDER(old), UNPACK_STATE(old), LIST);

	if (!set_pack_node(prev_n, old, new)) {
		goto retry_next;
	}

	// no need for atomics when updating prev pointers, as no more than one 
	// thread at a time uses them; updates will get flushed when executing the 
	// CAS below anyway
	next_n->prev = prev_n;

	retry_release:;
	old = GET_PACK(n);

	assert(UNPACK_REACH(old) == MARK);
	
	new = MAKE_PACK_NODE(NULLN, UNPACK_ORDER(old), UNPACK_STATE(old), UNLINK);
	if (!set_pack_node(n, old, new))
		goto retry_release;
	n->prev = NULLN; // no need for atomics
	
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
		unsigned owner = OWNER(&nodes[i]);
		nodes[i] = (node) {
			.next = PACK_NEXT(NULLN),
			.prev = NULLN,
			.state = INV,
			.reach = UNLINK,
			.order = 0,
			.owner = owner,
			.owner_heads = (node *) &zones[owner].heads,
			.owner_stacks = (stack *) &zones[owner].stacks,
		};
		
		// Initially only MAX_ORDER-sized blocks are marked as available
		if (i % (0x1 << MAX_ORDER) == 0) {
			nodes[i].state = FREE;
			nodes[i].order = MAX_ORDER;
		}
	}

	// printf("\tall nodes filled, inserting MAX_ORDER blocks\n");

	// insert the blocks in the buddy system
	for (unsigned long i = 0; i < TOTAL_NODES; i += (0x1 << MAX_ORDER)) {
		insert_node(&nodes[i]);
	}
}

// Initialize the structure of the buddy system
__attribute__ ((constructor)) void premain() {
	// printf("initializing buddy system state:\n");
	// Get the number of CPUs configured on the system; there might be some 
	// CPUs that are configured but not available (e.g. shut down to save power)
	ALL_CPUS = sysconf(_SC_NPROCESSORS_CONF);
	// make sure the number of nodes per CPU is a power of 2; this might lead to 
	// some CPU zones having slightly more memory compared to the others
	nodes_per_cpu = (TOTAL_NODES / ALL_CPUS) >> MAX_ORDER << MAX_ORDER;
	if (nodes_per_cpu == 0) abort(); // not enough memory has been assigned

	#ifndef NDEBUG
	printf("buddy system manages %lluB of memory\n", TOTAL_MEMORY);
	printf("initial state has %llu %luB blocks\n", TOTAL_MEMORY / MAX_ALLOCABLE_BYTES, MAX_ALLOCABLE_BYTES);
	#endif
	// Allocate the memory for all necessary components
	memory = aligned_alloc(PAGE_SIZE, TOTAL_MEMORY);
	nodes = aligned_alloc(PAGE_SIZE, sizeof(node) * TOTAL_NODES);
	zones = aligned_alloc(PAGE_SIZE, sizeof(cpu_zone) * ALL_CPUS);
	locks = malloc(sizeof(lock) * ALL_CPUS);
	workers = malloc(sizeof(pthread_t) * ALL_CPUS);
	cleanup_requested = aligned_alloc(CACHE_LINE_SIZE, sizeof(futex_t) * ALL_CPUS);
	
	// Cleanup if any allocation failed
	if (memory == NULL || nodes == NULL || zones == NULL || locks == NULL || 
			workers == NULL || cleanup_requested == NULL) {
		if(memory != NULL) free(memory);
		if(nodes != NULL) free(nodes);
		if(zones != NULL) free(zones);
		if(locks != NULL) free(locks);
		if(workers != NULL) free(workers);
		if(cleanup_requested != NULL) free(cleanup_requested);

		fprintf(stderr, "An allocation failed:\n\tmemory: %p\n\tnodes: %p\n\tzones: %p\n\tlocks: %p\n\tworkers: %p\n\tcleanup_requested: %p\n", 
				memory, nodes, zones, locks, workers, cleanup_requested);

		abort();
	}

	// printf("\tallocations done\n");

	// memset(memory, 0, TOTAL_MEMORY); // not necessary

	// All the structures needed are allocated, time to initialize them

	// First initialize and connect all the heads in the cpu_zones
	for (unsigned long i = 0; i < ALL_CPUS; i++) {
		init_lock(&locks[i]);
		init_futex(&cleanup_requested[i]);

		#ifndef NWORKERS
		// launch worker threads
		pthread_create(&workers[i], NULL, cleanup_thread_job, (void *) i);
		#endif

		for (unsigned j = 0; j <= MAX_ORDER; j++) {

			zones[i].heads[j] = (node) {
				.next = PACK_NEXT(&zones[(i + 1) % ALL_CPUS].heads[j]),
				.order = j,
				.state = HEAD,
				.reach = LIST,
				.prev = &zones[(i + ALL_CPUS - 1) % ALL_CPUS].heads[j],
				.owner = i,
			};

			#ifdef DELAY2
			// for all heads the initial D is 0 since all nodes are going to be 
			// inserted in the list (D = N -2S - L, and initially N == L)
			zones[i].heads[j].D = 0;
			#endif

			stack_init(&zones[i].stacks[j]);
		}
	}

	// printf("\tlist heads set up, inserting memory blocks\n");

	setup_memory_blocks();

	// printf("buddy system is ready\n");
}

// =============================================================================
// Convenience functions
// =============================================================================

static inline int change_state(node *n, unsigned old_state, unsigned new_state, 
		unsigned exp_reach, unsigned exp_order) {
	unsigned long old = GET_PACK(n);

	if (UNPACK_STATE(old) != old_state || UNPACK_REACH(old) != exp_reach || 
			UNPACK_ORDER(old) != exp_order) {
		return 0;
	}

	unsigned long new = MAKE_PACK_NODE(UNPACK_NEXT(old), exp_order, new_state, 
			exp_reach);

	return set_pack_node(n, old, new);
}

static inline int change_order(node *n, unsigned old_order, unsigned new_order, 
		unsigned exp_state, unsigned exp_reach) {
	
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

	return id & 0xfffULL; // TODO: fix this ^^^ and take NUMA into account
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
	unsigned owner = n->owner;
	if (try_lock(&locks[owner])) {
		ok = remove_node(n);
		
		if (ok) {
			ok = insert_node(n);
		}

		unlock_lock(&locks[owner]);
	}
	return ok;
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

	#ifdef DELAY2
	if (ok) {
		// we took a node from the list, so we have to update the value of D 
		// for the current level: D = N - 2S - L, L -= 1 ==> D += 1
		atomic_add(&n->owner_heads[n_order].D, 1);
	}
	#endif

	return ok;
}

// Attempt to remove a node from the list, returns 1 if removal was successful 
// or 0 otherwise; *next_node_ptr is set to point to the next node to visit
static inline int handle_occ_node(node *n, node **next_node_ptr) {
	node *next = NEXT(n);
	int ok = 0;
	unsigned owner = n->owner;

	if (try_lock(&locks[owner])) {
		ok = remove_node(n);
		// remove_node fails if (n->state == FREE || n->reach != LIST)

		unlock_lock(&locks[owner]);
	}

	// check if remove_node failed or there's been a concurrent free; in this case 
	// the current thread should check this node again
	if (!ok || n->state == FREE)
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

	target_order = order;

	next_order:;
	n = &zones[cpu].heads[target_order];

	unsigned cpus_visited = 0;
	for ( ; ; ) {
		// this might happen if a thread sleeps holding a reference to a node 
		// that gets removed before the thread wakes up again
		unsigned short n_reach = n->reach;
		if (n_reach == UNLINK || n_reach == STACK) {
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
						
						#ifdef DELAY2
						// we took a node from the stack, update the value of D
						// D = N - 2S - L, S -= 1 ==> D += 2
						atomic_add(&n->owner_heads[target_order].D, 2);
						#endif
						
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
				fprintf(stderr, "There's a node with a wrong status value:\n\t%p: %x",
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

	// First set the new order of n atomically
	// TODO: this could be done when occupying the node in get_free_node saving 
	// the trouble of doing two atomic CAS calls; give get_free_node another 
	// argument a (size_t *) where the current order of the occupied node will 
	// be put after the allocation
	retry_set_order:;
	assert(n->state == OCC && n->reach != STACK);

	if (current_order == MAX_ORDER) {
		FRAG_EVENT();
	}

	if (!change_order(n, current_order, target_order, OCC, n->reach))
		goto retry_set_order;

	size_t buddy_order = current_order;
	while (buddy_order != target_order) {
		buddy_order--;
		// split n in half, validate its second half and insert it into the 
		// correct stack
		unsigned long buddy_index = BUDDY_INDEX(n_index, buddy_order);
		node *buddy = &nodes[buddy_index];
		
		retry_buddy:;
		unsigned long old = GET_PACK(buddy);
		stack *s = &buddy->owner_stacks[buddy_order];

		assert(UNPACK_REACH(old) == UNLINK);
		assert(UNPACK_STATE(old) == INV);
		
		buddy->order = buddy_order;
		buddy->state = OCC;
		int ok = stack_push(s, buddy);

		#ifdef DELAY2
		// no need to update D here:
		// D = N - 2S - L, N += 2 and S += 1 ==> D += 0
		#endif

		if (!ok)
			goto retry_buddy;
	}
}

static void *_alloc(size_t size) {
	size_t order = closest_order(size);

	if (order > MAX_ORDER) return NULL;

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

	assert(n->state == OCC);

	// check if the node is owned by the current executing CPU; 
	// if it is disable preemption (a.k.a. take the lock) and remove it
	unsigned owner = n->owner;
	if (try_lock(&locks[owner])) {
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
	#ifdef FAST_FREE
	if (n != NULLN) update_model(order);
	#endif

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
	unsigned order = n->order;

	node *buddy = &nodes[BUDDY_INDEX(INDEX(n), order)];
	// fast path: if the buddy is not free or wrong order just mark n as free
	if (buddy->state != FREE || buddy->order != order || order == MAX_ORDER) {
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

			#ifdef DELAY2
			// we merged two nodes at this order and removed one from the list
			// D = N - 2S - L, N -= 2 and L -= 1 ==> D -= 1
			atomic_sub(&n->owner_heads[order].D, 1);
			#endif

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
	
	if (order == MAX_ORDER) {
		DEFRAG_EVENT();
	}

	return n;
}

static void _free(void *addr) {
	unsigned long index = ((unsigned char *)addr - memory) / MIN_ALLOCABLE_BYTES;
	node *n = &nodes[index];
	unsigned n_order = n->order;
	unsigned owner = n->owner;
	stack *s = &n->owner_stacks[n_order];

	assert(n->state == OCC && n->reach != STACK);
	
	#if defined(FAST_FREE) && defined(DELAY2)
	#define FAST_RELEASE_CONDITION ((head->D > 0 || should_fast_free(n_order) || LEN(s) <= (STACK_THRESH / 2)) && n->reach == UNLINK)
	#elif defined(FAST_FREE)
	#define FAST_RELEASE_CONDITION ((LEN(s) <= (STACK_THRESH / 2) || should_fast_free(n_order)) && n->reach == UNLINK)
	#elif defined(DELAY2)
	#define FAST_RELEASE_CONDITION (head->D > 0 && n->reach == UNLINK)
	#endif

	retry:;
	#if defined(FAST_FREE) || defined(DELAY2)
	#ifdef DELAY2
	node *head = &n->owner_heads[n_order];
	#endif

	// fast path: if the order of the block is often requested or there's space 
	// in the stack push it there and don't even try doing any other work
	if (FAST_RELEASE_CONDITION) {
		int ok = stack_push(s, n);
		if (!ok)
			goto retry;
		
		#ifdef DELAY2
		// we freed a node to a stack, update D:
		// D = N - 2S - L, S += 1 ==> D -= 2

		atomic_sub(&n->owner_heads[n_order].D, 2);
		#endif
		
		return;
	}
	#endif

	if (try_lock(&locks[owner])) {
		if (n->reach == LIST) {
			// node might not have been removed before being freed
			int ok = remove_node(n);

			if (!ok)
				goto retry; // should never happen but better be sure
		}
		
		n = try_coalescing(n);

		insert_node(n);
		
		#ifdef DELAY2
		n_order = n->order;
		head = &n->owner_heads[n_order];
		// we freed a node to the list, update D:
		// D = N - 2S - L, L += 1 ==> D -= 1
		atomic_sub(&head->D, 1);
		#endif

		unlock_lock(&locks[owner]);
	} else {
		int ok = 0;
		
		while (ok == 0 && n->state == OCC) {
			switch (n->reach) {
				case LIST:
					// node is still in the list as it wasn't removed, just mark as free
					ok = change_state(n, OCC, FREE, LIST, n->order);

					#ifdef DELAY2
					if (ok) {
						// we freed a node in a list, update D:
						// D = N - 2S - L, L += 1 ==> D -= 1
						atomic_sub(&head->D, 1);
					}
					#endif

					break;
				
				case UNLINK:
					ok = stack_push(s, n);

					#ifdef DELAY2
					if (ok) {
						// we freed a node to a stack, update D:
						// D = N - 2S - L, S += 1 ==> D -= 2
						atomic_sub(&head->D, 2);
					}
					#endif

					break;
				
				case MARK:
					// another thread that's looking for a free node is 
					// currently cleaning and removing the node from the list; 
					// try to change the state to FREE while the removal is 
					// taking place:
					// 1. if this operation is successful the other thread will 
					// handle n after its removal
					// 2. if the operation fails this thread will check again
					ok = change_state(n, OCC, FREE, MARK, n->order);
					
					#ifdef DELAY2
					if (ok) {
						// we freed a node in a list, update D:
						// D = N - 2S - L, L += 1 ==> D -= 1
						atomic_sub(&head->D, 1);
					}
					#endif
					
					break;
				
				default:
					// only remaining option is STACK, which shouldn't happen
					printf("Can't free node with n->reach %x\n", n->reach);
					abort();
					break;
			}
		}
	}

	#ifdef DELAY2
	if (head->D < 0) {
		// Since D = N - 2S - L and N >= S + L, D < 0 ==> there's at least 
		// N/2 + 1 free nodes at the current level, so at least a pair of 
		// buddies can be coalesced

		// printf("D = %ld, delay = %lu\n", head->D, cleanup_requested[owner].delay);
		futex_wake(&cleanup_requested[owner]);
	}
	#else
	if (LEN(s) > STACK_THRESH) {
		// wake up worker thread for the CPU that owns n if the stack has 
		// reached its occupation threshold

		futex_wake(&cleanup_requested[owner]);
	}
	#endif
}

void bd_xx_free(void *addr) {
	if ((void *) memory <= addr && addr < (void *) (memory + TOTAL_MEMORY))
		_free(addr);
}

// =============================================================================
// Helper thread job
// =============================================================================

static void clean_cpu_zone(cpu_zone *z, lock *l) {
	// iterate on all stacks and try to pop everything out of them to clean up

	for (unsigned order = 0; order < MAX_ORDER; order++) {
		#ifdef DELAY2
		if (z->heads[order].D >= 0) {
			// printf("D = %ld\n", z->heads[order].D);
			continue;
		}
		#endif
		
		unsigned long pops = 0;
		stack *s = &z->stacks[order];
		if (LEN(s) == 0) continue;
		
		if (!try_lock(l))
			break; // there's someone already working on the list, try later

		#ifdef DELAY2
		#define CONTINUE_CONDITION (z->heads[order].D < 0)
		#else
		#define CONTINUE_CONDITION (LEN(s) > 0)
		#endif

		while (CONTINUE_CONDITION) {
			// popping one element at the time is pretty wasteful but promotes
			// reuse of the memory in the stack; clearing all elements at once 
			// is possible and would be faster but might lead to more splitting 
			// and in turn more work for the cleaning thread
			node *n = stack_pop(s);

			if (n == NULLN) break;

			pops++;
			#ifdef DELAY2
			// we took a node from the stack, update the value of D
			// D = N - 2S - L, S -= 1 ==> D += 2
			atomic_add(&n->owner_heads[order].D, 2);
			#endif
			
			n = try_coalescing(n);

			insert_node(n);
			
			#ifdef DELAY2
			unsigned n_order = n->order;
			node *head = &n->owner_heads[n_order];
			// we freed a node to the list, update D:
			// D = N - 2S - L, L += 1 ==> D -= 1
			atomic_sub(&head->D, 1);
			#endif
		}

		// #ifdef DELAY2
		// printf("Popped %lu nodes at order %u; stack len %lu, D %lu\n", pops, order, LEN(s), z->heads[order].D);
		// #else
		// printf("Popped %lu nodes at order %u; stack len %lu\n", pops, order, LEN(s));
		// #endif

		unlock_lock(l);
	}
}

static void *cleanup_thread_job(void *arg) {
	unsigned long cpu = (unsigned long) arg;
	
	// set CPU affinity for this thread
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);

	if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
		fprintf(stderr, "Couldn't pin worker thread to CPU\n");
		abort();
	}

	cpu_zone *z = &zones[cpu];
	lock *l = &locks[cpu];

	for ( ; ; ) {
		futex_wait(&cleanup_requested[cpu]);

		// while (cleanup_requested[cpu].var == 0) {
		// 	usleep(5000);
		// 	// FIXME: this is so unbelievably filthy, yet it's 2 orders of 
		// 	// magniture faster than using pthread_cond_wait/signal or 
		// 	// sleep+pthread_kill and even futex (without rate limiting); there 
		// 	// must be some better way 
		// 	// to do this
		// }
		
		clean_cpu_zone(z, l);

		reset_futex(&cleanup_requested[cpu]);
	}

	return NULL;
}

// =============================================================================
// Debug
// =============================================================================

void _debug_test_nodes() {
	printf("Checking if all nodes are ok...\n");
	unsigned long free_count = 0;
	unsigned long free_mem = 0;
	unsigned long occ_count = 0;
	unsigned long occ_mem = 0;
	unsigned long inv_count = 0;

	for (unsigned long i = 0; i < TOTAL_NODES; i++) {
		node *n = &nodes[i];

		if (n->state == FREE && n->reach != LIST) {
			printf("FREE node lost: %p { status = %u, order = %u, cpu = %lu, next = %p, prev = %p}\n", 
				n, n->state, n->order, OWNER(n), NEXT(n), n->prev);
		}

		if (n->state == OCC && n->reach != STACK) {
			printf("OCC node out of stack: %p { status = %u, order = %u, cpu = %lu, next = %p, prev = %p}\n", 
				n, n->state, n->order, OWNER(n), NEXT(n), n->prev);
		}

		switch (n->state) {
			case FREE:
				free_count++;
				free_mem += MIN_ALLOCABLE_BYTES << n->order;
				break;
			case OCC:
				occ_count++;
				occ_mem += MIN_ALLOCABLE_BYTES << n->order;
				break;
			case INV:
				inv_count++;
				break;
			default:
				break;
		}
	}

	printf("\tFREE: %lu, %luB\n\tOCC: %lu, %luB\n\tINV: %lu\n", 
			free_count, free_mem, occ_count, occ_mem, inv_count);

	assert(free_mem + occ_mem == TOTAL_MEMORY);

	#ifdef DELAY2
	for (unsigned long i = 0; i < ALL_CPUS; i++) {
		printf("CPU %lu: ", i);
		for (unsigned long j = 0; j <= MAX_ORDER; j++) {
			printf("(%lu, %3ld), ", j, zones[i].heads[j].D);
		}
		printf("\n");
	}
	#endif

	printf("Done!\n");
}
