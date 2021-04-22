#ifndef DEBUG_MACROS

	#include <stdio.h>

	#if defined(NDEBUG) || !defined(FRAG)
		// If NDEBUG is defined all the macros do nothing

		#define FRAG_EVENT() ({ do {} while(0); }) // NOP
		#define DEFRAG_EVENT() ({ do {} while(0); }) // NOP

	#else

		typedef struct _frag_event {
			unsigned long ev: 8; // a single char, '+' or '-'
			unsigned long tsc: 56; // lower bits of the tsc register
		} __attribute__ ((__packed__)) frag_event;

		#define EV_HISTORY_LEN 1
		static __thread unsigned long _ev_count = 0ULL;
		static __thread frag_event _events[EV_HISTORY_LEN] = { 0ULL };

		static void dump_ev_history() {
			for (int i = 0; i < EV_HISTORY_LEN; i++) {
				frag_event event = _events[i];
				printf("%lu, %c1\n", event.tsc, event.ev);
				// en external program is needed to reorder the output
			}
		}

		#define _ADD_EVENT(_tsc, _ev) ({									\
			frag_event event = (frag_event) { .tsc = _tsc, .ev = _ev };		\
			_events[_ev_count % EV_HISTORY_LEN] = event;					\
			_ev_count++;													\
			if (_ev_count % EV_HISTORY_LEN == 0) {							\
				dump_ev_history();											\
			}																\
		})

		#define FRAG_EVENT() (_ADD_EVENT(__builtin_ia32_rdtsc(), '+'))
		#define DEFRAG_EVENT() (_ADD_EVENT(__builtin_ia32_rdtsc(), '-'))
	
	#endif

#endif
