#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

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
static cpu_zone *zones = NULL; // a list of arrays of nodes, each one being a list head
static node *nodes = NULL; // a list of all nodes, one for each page
// FIXME: find a way to get rid of this
static pthread_mutex_t *locks = NULL; // to emulate disabling preemption we take a lock on the cpu_zone

// =============================================================================
// List ops
// =============================================================================

static void add_node_after(node *n, node *target) {
	// TODO: it's assumed that target can't be a node that can be removed from 
	// the list, so target probably should be calculated in this function based 
	// on the value of n->cpu and n->order
	retry_1:
	if (!NOT_IN_LIST(n) || n->stack.len > 0) {
		return;
	}

	if (NEXT(target) == NULLN) {
		printf("BRUH how\n");
	}

	if (n->status != FREE) {
		printf("adding node that's not free?\n");
		return;
	}

	if (!bCAS(&n->next, NULLN, MARKED(NEXT(target))))
		goto retry_1;
	// n->next = (node *) MARKED(NEXT(target)); // BUG: this could allow a node to be inserted in list and stack
	n->prev = target;
	// the n->next pointer is marked to prevent deletion until insertion ends
	
	if (!bCAS(&target->next, NEXT(n), n)) {
		n->next = NULLN;
		goto retry_1;
	}
	// can a node be removed at this point while it's being added? nope
	// n->next was marked before it was visible in the list so it would be 
	// impossible to remove it
	
	retry_2:
	if (!bCAS(&(NEXT(n)->prev), target, n))
		goto retry_2;

	retry_release:
	if (!unmark_ptr(&n->next))
		goto retry_release;
	// insertion is done, anyone can remove the node from the list now
}

static void remove_node(node *n) {
	// first try to mark n->next, if it's marked already leave and return false
	retry_acquire:
	if (IS_MARKED(n->next) || n->next == NULLN)
		return; // someone else is removing or has removed the node
	
	if (!mark_ptr(&n->next))
		goto retry_acquire;

	retry_1: ;
	node *prev = n->prev;
	
	if (NEXT(prev) != n){
		// there's been an insertion in front of n
		// TODO: add an assert, this should never happen while removing the node
		if (!unmark_ptr(&n->next))
			goto retry_1;

		return;
	}

	if (!bCAS(&prev->next, n, NEXT(n)))
		goto retry_1;

	retry_2:
	if (!bCAS(&NEXT(n)->prev, n, prev))
		goto retry_2;

	n->next = n->prev = NULLN;
	
	mfence();
}

// =============================================================================
// Stack ops
// =============================================================================

static void stack_push(stack *s, node *n) {
	retry:;
	if (!NOT_IN_LIST(n)) {
		// printf("pushing but n shouldn't be in the list: %p\n", n);
		return;
	}
	
	node *s_head = s->head;
	if (!bCAS(&n->next, NULLN, UNMARKED(s_head)))
		goto retry;
	n->stack.len = n->next->stack.len + 1;
	
	if (n->status != OCC) {
		printf("n should be OCC: %p\n", n);
	}

	// make a pointer to n with the current stack epoch in the lower 6 bits
	node *new_head = (node *) STACK_HEAD(n, s_head);

	if (!bCAS(&s->head, s_head, new_head)) {
		n->next = NULLN;
		goto retry;
	}
}

static node *stack_pop(stack *s) {
	retry:;
	node *s_head = s->head;
	node *n = (node *) UNMARKED(s_head);

	if (n == NULLN) return NULLN;

	if (IN_LIST(n))
		goto retry; // a thread might sleep with an old reference

	node *new_head = (node *) STACK_HEAD(n->next, s_head);

	if (!bCAS(&s->head, s_head, new_head)) goto retry;

	n->stack.len = 0;
	n->next = NULLN;
	mfence(); // is this necessary?

	return n;
}

// TODO: rewrite this vvv
/*
// static node *clear_stack(stack *s) {
// 	stack new_stack = {
// 		.head = NULLN,
// 		.len = 0
// 	};

// 	retry:
// 	if (s->head == NULLN) return NULLN;
	
// 	node *head = s->head;
	
// 	// Wow this is ugly
// 	if (!bCAS((__int128 *) s, *((__int128 *) s), *((__int128 *) &new_stack))) // 128 bit CAS
// 		goto retry;
	
// 	return head;
// }
*/

// =============================================================================
// Initial setup
// =============================================================================

// Initialize the state of the nodes
static void setup_memory_blocks() {
	for (unsigned long i = 0; i < TOTAL_NODES; i++) {
		nodes[i] = (node) {
			.next = NULLN,
			.prev = NULLN,
			.cpu = i / nodes_per_cpu,
			.status = INV, // most nodes are invalid in the beginning
		};
		
		// Initially only MAX_ORDER-sized blocks are marked as available
		if (i % (0x1 << MAX_ORDER) == 0) {
			nodes[i].status = FREE;
			nodes[i].order = MAX_ORDER;
		}
	}

	printf("\tall nodes filled, inserting MAX_ORDER blocks\n");

	// insert the blocks in the buddy system
	for (unsigned long i = 0; i < TOTAL_NODES; i += (0x1 << MAX_ORDER)) {
		add_node_after(&nodes[i], &zones[nodes[i].cpu].heads[nodes[i].order]);
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

	// ALL_CPUS = sysconf(_SC_NPROCESSORS_CONF); // TODO: enable this again
	ALL_CPUS = 1; // TODO: remove this

	nodes_per_cpu = TOTAL_NODES / ALL_CPUS;

	// Allocate the memory for all necessary components
	memory = aligned_alloc(PAGE_SIZE, TOTAL_MEMORY);
	nodes = aligned_alloc(CACHE_LINE_SIZE, sizeof(node) * TOTAL_NODES);
	zones = aligned_alloc(CACHE_LINE_SIZE, sizeof(cpu_zone) * ALL_CPUS);
	locks = malloc(sizeof(pthread_mutex_t) * ALL_CPUS);
	
	// Cleanup if any allocation failed
	if (memory == NULL || nodes == NULL || zones == NULL || locks == NULL) {
		if(memory != NULL) free(memory);
		if(nodes != NULL) free(nodes);
		if(zones != NULL) free(zones);

		if(locks != NULL) free(locks);
		fprintf(stderr, "An allocation failed:\n\
			\tmemory: %p\n\tnodes: %p\n\tzones: %p\n\tlocks: %p\n", 
			memory, nodes, zones, locks);

		abort();
	}

	printf("\tallocations done\n");

	// All the structures needed are allocated, time to initialize them

	// First initialize and connect all the heads in the cpu_zones
	for (int i = 0; i < ALL_CPUS; i++) {
		pthread_mutex_init(&locks[i], NULL);

		for (int j = 0; j <= MAX_ORDER; j++) {

			zones[i].heads[j] = (node) {
				.next = &zones[(i + 1) % ALL_CPUS].heads[j],
				.prev = &zones[(i + ALL_CPUS - 1) % ALL_CPUS].heads[j],
				.stack.head = NULLN,
				.status = HEAD,
				.cpu = i,
				.order = j,
			};

		}
	}

	printf("\tlist heads set up, inserting memory blocks\n");

	setup_memory_blocks();
	setup_memory_state();

	printf("buddy system is ready\n");
}

// =============================================================================
// Implementation of the buddy system
// =============================================================================

#define mark_node(n, old_status, new_status) (bCAS(&n->status, old_status, new_status))

static inline unsigned long cpu_id() {
	unsigned long id;
	__asm__ __volatile__(
		"rdtscp;"			// serializing read of tsc also puts CPU ID in rcx
		: "=c"(id)			// output to id variable
		: 					// no inputs needed
		: "%rax", "%rdx"	// rax and rdx are clobbered
	);

	// id is not just the CPU core the thread is executed on, it's a value read 
	// from a MSR managed by the OS; in Linux the value read is interpreted as 
	// (NUMA_NODE_ID << 12) | (CPU_ID)

	return id % ALL_CPUS; // TODO: fix this ^
}

// Get the minimum allocation order that satisfies a request of size s
static inline size_t closest_order(size_t s) {
	if (s <= PAGE_SIZE) return 0;
	
	return 64 - __builtin_clzl(s - 1) - 12;
}

// =============================================================================
// Allocation
// =============================================================================

// Navigate the list in search for a free node to allocate
static node *get_free_node(size_t order) {
	stack *s;
	node *n, *popped, *next_node;
	size_t target_order;
	unsigned cpus_visited = 1;

	restart:
	if (cpus_visited > ALL_CPUS + 1) {
		printf("BRUH HOW\n");
	}

	target_order = order;
	n = &zones[cpu_id()].heads[target_order];
	// TODO: 	^ this is wrong with more that 1 NUMA node, see the comment in cpu_id

	retry_stack:
	s = &n->stack;
	popped = NULLN;

	popped = stack_pop(s);
	if (popped != NULLN) {
		n = popped;

		if (n->status != OCC) {
			printf("node is not OCC %p\n", n);
		}
		
		goto done;
	}

	cpus_visited = 1;
	n = NEXT(n);
	// TODO: assert n->next == NEXT(n); this is always true for list heads

	for ( ; ; ) {
		// this might happen if a thread sleeps holding a reference to a node that is removed before the thread wakes up again
		if (n == NULLN) {
			goto restart;
		}

		// TODO: clean this up a bit, there's too much indentation
		switch (n->status) {
			case FREE:
				if (n->order >= order) {
					if (mark_node(n, FREE, OCC))
						goto done; // CAS succeded, all is good
				} else {
					// n->order < order <= target_order means the node is in the wrong list
					next_node = NEXT(n);
					// move_node(n); // TODO: add move_node later
					n = next_node;
				}
				
				break; // someone else marked this node concurrently
			
			case INV:
				// just skip it, someone is coalescing the node and will remove it later
				// it would be better to make this a coin flip o something?
				n = NEXT(n);
				break;
			
			case OCC:
				// attempt to cleanup
				next_node = NEXT(n);

				// make sure the transaction didn't fail because of someone that released the node
				// FIXME: this uses locks atm, not very poggers
				if (0) { // TODO: enable again
				// if (pthread_mutex_trylock(&locks[n->cpu]) == 0) {
					if (n->status != FREE && IN_LIST(n)) {
						remove_node(n);
					}
					pthread_mutex_unlock(&locks[n->cpu]);
				
					retry:
					if (n->status == FREE) {
						// node might have been released during the remove call, if it was try to acquire it
						
						if (!mark_node(n, FREE, OCC))
							goto retry;
						
						if (n->order >= order)
							goto done; // allocate this node
						else
							stack_push(&zones[n->cpu].heads[n->order].stack, n);
					}
				}

				n = next_node;
				break;
			
			case HEAD:
				cpus_visited++;
				if (cpus_visited > ALL_CPUS) {
					target_order++;
					if (target_order <= MAX_ORDER) {
						n = &zones[cpu_id()].heads[target_order];
						goto retry_stack;
					} else {
						// There's no memory to satisfy this allocation
						n = NULLN;
						goto done;
					}
				}

				break;
			
			default:
				fprintf(stderr, "There's a node with a wrong status value:\n\t%p: %p",
					n, n->status);
				abort();
		}

	}

	done:
	// printf("Occupied node: %p\n", n);
	return n;
}

static void split_node(node *n, size_t target_order) {
	while (n->order != target_order) {
		// split node in half, insert the second half to the correct place
		n->order--;
		node *buddy = n + (0x1 << n->order);
		buddy->order = n->order;
		buddy->cpu = n->cpu;

		// FIXME: this uses locks atm, not very poggers
		if (pthread_mutex_trylock(&locks[buddy->cpu]) == 0) {
			while (!mark_node(buddy, INV, FREE)); // atomic CAS on n->status
			// printf("\tbuddy: %p { status = %u, order = %u, cpu = %u}\n", buddy, buddy->status, buddy->order, buddy->cpu);
			add_node_after(buddy, &zones[buddy->cpu].heads[buddy->order]);
			pthread_mutex_unlock(&locks[buddy->cpu]);
		} else {
			while (!mark_node(buddy, INV, OCC)); // atomic CAS on n->status
			// printf("\tbuddy: %p { status = %u, order = %u, cpu = %u}\n", buddy, buddy->status, buddy->order, buddy->cpu);
			stack_push(&zones[buddy->cpu].heads[buddy->order].stack, buddy); // atomic CAS on stack head
		}
	}
}

static void *_alloc(size_t size) {
	size_t order = closest_order(size);

	if (order > MAX_ORDER) {
		return NULL;
	}

	node *n = get_free_node(order);
	if (n == NULLN) return NULL;

	// check whether n is bigger than expected and needs to be fragmented
	if (n->order > order) {
		split_node(n, order);
	}
	
	// while the split was taking place someone might have removed n gaining us some time
	if (NOT_IN_LIST(n)) goto done;

	#ifdef TSX
	// first try removing the node transactionally; this is sub-optimal if we're on the correct CPU, but it protects against race conditions
	ok = TRY_TRANSACTION({
		if (IN_LIST(n)) {
			// the node was taken from the list and is still there
			remove_node(n)
		}
	})
	
	if (ok) goto done
	#endif

	if (n->status != OCC) {
		printf("Wtf how is it not OCC? %p, %p\n", n, n->status);
	}

	// fallback path, check if the node is owned by the current executing CPU; if it is disable preemption and remove it
	// FIXME: this uses locks atm, not very poggers
	if (pthread_mutex_trylock(&locks[n->cpu]) == 0) {
		if (IN_LIST(n)) {
			// the node was taken from the list and is still there
			remove_node(n);
		}
		pthread_mutex_unlock(&locks[n->cpu]);
	} else {
		// not a problem, when a thread on the correct CPU reaches this node it will try to remove it
	}

	done:
	// printf("\tbase memory address: %p\n", memory);
	// printf("\tnode index %p, page address %p\n", n - nodes, memory + (n - nodes) * PAGE_SIZE);
	return (void *)(memory + (n - nodes) * PAGE_SIZE);
}

void* bd_xx_malloc(size_t size) {
	return _alloc(size);
}

// =============================================================================
// Deallocation
// =============================================================================

static node *try_coalescing(node *n) {
	while (!mark_node(n, OCC, INV)) {
		printf("no way, n->status: %d\n", n->status);
	}

	// BUG: yep, this leads to pushing nodes straight into the fucking stack
	for ( ; ; ) {
		unsigned long n_index = n - nodes;
		unsigned long buddy_index = n_index ^ (0x1 << n->order);
		node *buddy = &nodes[buddy_index];
		
		retry:
		if (buddy->status != FREE || buddy->order != n->order) {
			if (buddy->status == HEAD) {
				printf("How the fuck did you get a head?\n\tnode: %p, buddy: %p\n", 
					n, buddy);
			}

			break; // buddy is either in use, in the stack, or it's been fragmented
		}
		
		// printf("\tBuddy free at order %u\n", buddy->order);
		if (!mark_node(buddy, FREE, INV))
			goto retry; // someone might have allocated buddy concurrently
		remove_node(buddy);
		
		n = &nodes[MIN(n_index, buddy_index)];
		n->order++;

		if (n->order == MAX_ORDER) break;
	}
	
	while (!mark_node(n, INV, FREE)) {
		// BUG: sometimes n->status becomes OCC at this point 
		printf("Aight, it failed, n->status: %d\n", n->status);
	}
	
	return n;
}

static void _free(void *addr) {
	unsigned long index = ((unsigned char *)addr - memory) / PAGE_SIZE;
	node *n = &nodes[index];

	if (n->status != OCC)
		printf("wtf it's got to be OCC\n");
	
	if (n->stack.len != 0)
		printf("why is it in the stack?\n");

	// TODO: this causes problems, check later
	// if (zones[n->cpu].heads[n->order].stack.len <= (STACK_THRESH / 2)) {
	// 	stack_push(&zones[n->cpu].heads[n->order].stack, n);
	// 	return;
	// }

	// FIXME: this uses locks atm, not very poggers
	if (pthread_mutex_trylock(&locks[n->cpu]) == 0) {
		if (IN_LIST(n)) {
			// in case a node allocated on another CPU but not removed gets freed on the correct CPU
			remove_node(n);
		}
		
		n = try_coalescing(n); // TODO: enable this
		add_node_after(n, &zones[n->cpu].heads[n->order]);
		if (n->status == FREE) {
		}
		pthread_mutex_unlock(&locks[n->cpu]);

	} else {

		unsigned node_freed = 0; // why is this needed? read below
		if (IN_LIST(n)) {
			// this node is still in the list as it wasn't removed, just mark as free
			while(!mark_node(n, OCC, FREE)); // atomic CAS on n->status
			node_freed = 1;
		}
		
		retry:
		if (NOT_IN_LIST(n)) {
			
			if (n->status == OCC && !node_freed) {
				// this is the most likely path, n was removed from the list before this thread started the release
				stack_push(&zones[n->cpu].heads[n->order].stack, n);
			
			} else if (n->status == FREE && node_freed) {
				// there's a very unlikely situation that can happen and needs to be dealt with safely:
				// another thread (B) is doing a get_free_node() and finds n in an OCC state; B removes n out of transaction just as this thread (A) was marking it as FREE
				if (!mark_node(n, FREE, OCC)) // B and A now compete for the node, first one to mark is as OCC deals with it
					goto retry;
				stack_push(&zones[n->cpu].heads[n->order].stack, n);
			} else {
				unsigned long asd = 0;
			}
			
			// (node->status == OCC && node_freed) means B occupied the node
			// (node->status == FREE && !node_freed) is impossible
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

		if (n->status == FREE && !IN_LIST(n)) {
			printf("FREE node lost: %p { status = %u, order = %u, cpu = %u, next = %p, prev = %p}\n", 
				n, n->status, n->order, n->cpu, n->next, n->prev);
		}

		if (n->status == OCC && !IN_LIST(n)) {
			if (n->stack.len == 0)
				printf("OCC node out of stack: %p { status = %u, order = %u, cpu = %u, next = %p, prev = %p}\n", 
				n, n->status, n->order, n->cpu, n->next, n->prev);
		}

		switch (n->status) {
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
			switch (n->status) {
				case HEAD:
					printf("|HEAD %d|-", n->cpu);
					i++;
					blocks = 0;
					break;
				case FREE:
					blocks++;
					printf("|o|-");
					break;
				default:
					printf("\nsomething's wrong\n");
					printf("\tnode: { status = %u, order = %u, cpu = %u}\n", n->status, n->order, n->cpu);
			}

			n = n->next;
		}
		printf("\n");
	}

	// Try some stack ops
	do { // just to collapse the block in the editor
		printf("\nTrying some stack operations\n");

		node *n = zones[0].heads[MAX_ORDER].next;
		remove_node(n);

		stack_push(&zones[0].heads[MAX_ORDER].stack, n);
		printf("n: %p, stack head: %p\n", n, zones[0].heads[MAX_ORDER].stack.head);
		
		n = stack_pop(&zones[0].heads[MAX_ORDER].stack);
		printf("n: %p, stack head: %p\n", n, zones[0].heads[MAX_ORDER].stack.head);

		stack_push(&zones[0].heads[MAX_ORDER].stack, n);
		printf("n: %p, stack head: %p\n", n, zones[0].heads[MAX_ORDER].stack.head);
		
		n = clear_stack(&zones[0].heads[MAX_ORDER].stack);
		printf("n: %p, stack head: %p\n", n, zones[0].heads[MAX_ORDER].stack.head);

		mark_node(n, FREE, OCC);
		stack_push(&zones[0].heads[MAX_ORDER].stack, n);
	} while (0);

	// Try some allocations
	do { // just to collapse the block in the editor
		printf("\nTrying some allocations\n");
		unsigned char *addr = bd_xx_malloc(100); // we expect a 1 page long area -> order 0
		unsigned index = (addr - memory) / PAGE_SIZE;
		node *n = &nodes[index];
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n, n->status, n->order, n->cpu);

		unsigned char *addr_2 = bd_xx_malloc(4096*12); // we expect a 16 page long area -> order 4
		unsigned index_2 = (addr_2 - memory) / PAGE_SIZE;
		node *n_2 = &nodes[index_2];
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->status, n_2->order, n_2->cpu);

		printf("\nLet's try to release\n");
		bd_xx_free(addr_2);
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->status, n_2->order, n_2->cpu);

		bd_xx_free(addr);
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n, n->status, n->order, n->cpu);
		printf("\tnode: %p { status = %u, order = %u, cpu = %u}\n", n_2, n_2->status, n_2->order, n_2->cpu);

	} while (0);

	return 0;
}
#endif
