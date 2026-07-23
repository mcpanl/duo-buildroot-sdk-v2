//#include <platform.h>
#include <arch_helpers.h>

//typedef uint64_t phys_addr_t;
//typedef uintptr_t       size_t;

#define L1_CACHE_BYTES     64
#define ALIGN(x, a)              (((x) + (a) - 1) & ~((a) - 1))

/*
 * dcache.ipa rs1 (invalidate)
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0000001    01010      rs1       000      00000  0001011
 *
 * dcache.cpa rs1 (clean)
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0000001    01001      rs1       000      00000  0001011
 *
 * dcache.cipa rs1 (clean then invalidate)
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0000001    01011      rs1       000      00000  0001011
 *
 * sync.s
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0000000    11001     00000      000      00000  0001011
 */
#define DCACHE_IPA_A0	".long 0x02a5000b"
#define DCACHE_CPA_A0	".long 0x0295000b"
#define DCACHE_CIPA_A0	".long 0x02b5000b"

#define SYNC_S		".long 0x0190000b"

/*
 * Bind address into a0 for each T-Head dcache.*pa opcode. The previous
 * "register asm("a0")" loop form was unsafe with optimization: a0 was not
 * reliably updated each iteration, so large range ops could hit the wrong
 * line repeatedly or confuse the pipeline on shared DRAM with CA53.
 */
#define CACHE_OP_RANGE(OP, start, size) do { \
	unsigned long _a = (unsigned long)(start) & ~(L1_CACHE_BYTES - 1UL); \
	unsigned long _e = ALIGN((unsigned long)(start) + (size), L1_CACHE_BYTES); \
	for (; _a < _e; _a += L1_CACHE_BYTES) { \
		register unsigned long _ra0 asm("a0") = _a; \
		__asm__ __volatile__(OP :: "r"(_ra0) : "memory"); \
	} \
	__asm__ __volatile__(SYNC_S ::: "memory"); \
} while (0)

//void c900_cache_invalidate(phys_addr_t start, size_t size)
void inv_dcache_range(uintptr_t start, size_t size)
{
	CACHE_OP_RANGE(DCACHE_IPA_A0, start, size);
}

//void c900_cache_clean(phys_addr_t start, size_t size)
void clean_dcache_range(uintptr_t start, size_t size)
{
	CACHE_OP_RANGE(DCACHE_CPA_A0, start, size);
}

//void c900_cache_flush(phys_addr_t start, size_t size)
void flush_dcache_range(uintptr_t start, size_t size)
{
	CACHE_OP_RANGE(DCACHE_CIPA_A0, start, size);
}

void enable_dcache(void)
{
	asm volatile(
		"csrs mhcr, %0;" ::"rI"(0x2)
	);
}

void disable_dcache(void)
{
	asm volatile(
		"csrc mhcr, %0;" ::"rI"(0x2)
	);
}

