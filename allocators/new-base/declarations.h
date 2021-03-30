#ifndef NEW_BUDDY_SYSTEM_FUNCS
	#include "nballoc.h"
	#include "structs.h"

	// =========================================================================
	// Lock ops
	// =========================================================================

	static inline int try_lock(lock *);

	static inline void unlock_lock(lock *);

	static inline void init_lock(lock *);

	// =========================================================================
	// List ops
	// =========================================================================

	static int insert_node(node *);
	static int remove_node(node *);

	// =========================================================================
	// Stack ops
	// =========================================================================

	static void stack_init(stack *);

	static int stack_push(stack *, node *);

	static node *stack_pop(stack *);

	static node *stack_clear(stack *);

	// =========================================================================
	// Convenience functions
	// =========================================================================

	static inline int change_state(node *, short, short, short, short);

	static inline int change_order(node *, short, short, short, short);

	static inline unsigned long cpu_id();

	static inline size_t closest_order(size_t);

	// =========================================================================
	// Allocation
	// =========================================================================

	static inline int move_node(node *);

	static inline int handle_free_node(node *, short, node **);

	static inline int handle_occ_node(node *, node **);

	static node *get_free_node(size_t, unsigned);

	static inline node *get_free_node_fast(size_t);

	static void split_node(node *, size_t);

	static void *_alloc(size_t);

	// =========================================================================
	// Deallocation
	// =========================================================================

	static node *try_coalescing(node *);

	static void _free(void *);

	// =========================================================================
	// Helper thread job
	// =========================================================================

	static void *cleanup_thread_job(void *);
#endif