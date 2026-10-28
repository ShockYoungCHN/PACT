/* Regression: a low-score fast region must not be protected by an unreachable
 * MemTotal-sized Top-K when the next higher-score slow region needs space. */
#include <assert.h>
#include "../src/rerank.c"

int main(void)
{
    /* One fast region (512 pages), only 128 free, 512 reserved. The old
     * configured budget keeps BOTH regions and thus never evicts the low one. */
    rerank_granule_t high_slow = {.score = 100, .present = 512, .node = RERANK_NODE_SLOW};
    rerank_granule_t low_fast = {.score = 10, .present = 512, .node = RERANK_NODE_FAST};
    rerank_granule_t *rank[] = {&high_slow, &low_fast};
    uint64_t kept;
    assert(rank_prefix(rank, 2, 2048, &kept) == 2);
    uint64_t budget = usable_budget_4k(2048, 512, 640, 512);
    assert(budget == 640);
    assert(rank_prefix(rank, 2, budget, &kept) == 1);
    assert(kept == 512);
    /* The low-score resident entry is now outside Top-K and can make room. */
    assert(rank[1]->node == RERANK_NODE_FAST);
    /* Below-reserve pressure shrinks the keep set, without unsigned wrapping. */
    assert(usable_budget_4k(4096, 1024, 128, 512) == 640);
    assert(usable_budget_4k(4096, 128, 128, 512) == 0);
    assert(rank_prefix(rank, 2, 0, &kept) == 0 && kept == 0);
    assert(rank_prefix(rank, 2, 511, &kept) == 0 && kept == 0);
    assert(usable_budget_4k(512, 1024, 4096, 512) == 512);
    /* Process startup: free space alone permits initial node1 promotions. */
    assert(usable_budget_4k(4096, 0, 2048, 512) == 1536);
    /* File-backed/shared mappings do not create fictitious reclaimable room. */
    assert(numa_line_fast_4k("100 bind:1 anon=500 N0=23 N1=477 kernelpagesize_kB=4\n") == 23);
    assert(numa_line_fast_4k("100 default file=/lib/libc.so N0=100 kernelpagesize_kB=4\n") == 0);
    assert(numa_line_fast_4k("100 bind:1 anon=512 N1=512 kernelpagesize_kB=4\n") == 0);
    /* Kernel numa_maps counts THP in base pages, explicit hugetlb in huge pages. */
    assert(numa_line_fast_4k("100 bind:1 anon=512 N0=512 kernelpagesize_kB=2048\n") == 512);
    assert(numa_line_fast_4k("100 bind:1 huge anon=2 N0=2 kernelpagesize_kB=2048\n") == 1024);
    puts("rerank reachable-capacity regression OK");
    return 0;
}
