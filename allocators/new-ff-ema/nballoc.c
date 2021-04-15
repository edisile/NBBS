#define FAST_FREE

#include "../new-base/nballoc.c"
#include "../new-base/nballoc.h"

#ifdef FAST_FREE

	#define HISTORY_LEN 128

	// Estimate the per-thread allocation order probability distribution
	static __thread float distrib[MAX_ORDER + 1] = { 1.0 / MAX_ORDER };
	static __thread unsigned decision = 0x1 << MAX_ORDER; // MAX_ORDER is always fast-freed
	static __thread unsigned char history[HISTORY_LEN] = { 0 };
	static __thread unsigned long count = 0;

	static inline void update_distrib() {
		unsigned aggregated[MAX_ORDER + 1] = { 0 };

		for (int i = 0; i < HISTORY_LEN; i++) {
			aggregated[history[i]]++;
		}

		for (int order = 0; order <= MAX_ORDER; order++) {
			distrib[order] *= 0.5;
			distrib[order] += (0.5 * aggregated[order]) / HISTORY_LEN;
		}

		// precompute the results of the decision process
		float p = 0;
		decision = 0x1 << MAX_ORDER;
		for (int i = 0; i < MAX_ORDER; i++) {
			p += distrib[i];

			// p >= 1 - p  <->  P(alloc size <= order) >= P(alloc size > order)
			if (p >= 0.5) {
				decision = decision | (0x1 << i);
			}
		}
	}

	inline void update_model(unsigned order) {
		history[count % HISTORY_LEN] = order;
		count++;

		if (count % HISTORY_LEN == 0) {
			update_distrib();
		}
	}

	inline int should_fast_free(unsigned order) {
		return decision & (0x1 << order);
	}

#endif
