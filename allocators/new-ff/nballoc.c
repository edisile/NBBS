#define FAST_FREE

#include "../new-base/nballoc.c"

#ifdef FAST_FREE

	static __thread float avg_order = 0; // per-thread average alloc order

	// The average is computed as a exponential moving average with 0.5 weight
	inline void update_model(unsigned order) {
		avg_order *= 0.5;
		avg_order += 0.5 * order;
	}

	inline int should_fast_free(unsigned order) {
		return order >= avg_order;
	}

#endif
