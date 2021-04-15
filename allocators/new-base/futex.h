#ifndef FUTEX_WRAPPERS
	
	#define FUTEX_WRAPPERS
	
	#include <stdint.h>
	#include <unistd.h>
	#include <linux/futex.h>
	#include <sys/syscall.h>

	#include "nballoc.h"
	#include "gcc_wrappers.h"

	typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) _futex_t {
		uint32_t var; // the variable the futex watches
		uint64_t tsc; // timestamp of last reset operation
		uint64_t delay; // minimum delay between wakeups, effectively rate limiting
	} futex_t;

	enum futex_states {
		SLEEP = 0,
		WAKE = 1,
	};

	static inline void init_futex(futex_t *f) {
		f->delay = 0x1 << 24; // TODO: find a way to justify this or make it adaptive
		f->tsc = 0;
		f->var = SLEEP;
	}
	
	static inline void reset_futex(futex_t *f) {
		retry:;
		if (f->var == SLEEP) return;

		f->tsc = __builtin_ia32_rdtsc();
		if (!bCAS(&f->var, WAKE, SLEEP)) goto retry;
	}

	// Wrapper for syscall (SYS_futex, ...) because libc doesn't expose it
	static inline long futex(uint32_t *uaddr, int futex_op, uint32_t val, 
			const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
		
		return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
	}

	// Wait while f->var == SLEEP, if it's not return immediately
	static inline long futex_wait(futex_t *f) {
		if (f->var != SLEEP) return 0;
		
		return futex((uint32_t *) &f->var, FUTEX_WAIT_PRIVATE, SLEEP, 
				NULL, NULL, 0);
	}

	// Try to wake up a sleeper thread; if not enough time passed since last 
	// time just return immediately
	static inline long futex_wake(futex_t *f) {
		uint64_t time_passed = __builtin_ia32_rdtsc() - f->tsc;
		if (time_passed < f->delay || f->var == WAKE) {
			return 0;
		}

		if (!bCAS(&f->var, SLEEP, WAKE))
			return 0; // best effort, if it fails it fails

		return futex((uint32_t *) &f->var, FUTEX_WAKE_PRIVATE, 1, 
				NULL, NULL, 0);
	}

#endif
