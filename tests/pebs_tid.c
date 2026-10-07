/* Exercise the actual perf-ring decoder and optional sample exporter. */
#include <assert.h>
#include <sys/stat.h>
#include "../src/pebs-aggregator.c"

struct test_sample {
    struct perf_event_header header;
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
    uint64_t addr;
};

static void write_sample(char *data, uint64_t size, uint64_t offset,
                         const struct test_sample *sample)
{
    const char *bytes = (const char *)sample;
    for (size_t i = 0; i < sizeof(*sample); i++) {
        data[(offset + i) % size] = bytes[i];
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const bool disabled = strcmp(argv[1], "disabled") == 0;
    const bool stride = strcmp(argv[1], "stride") == 0;
    pact_workload_t wl = {.target_pid = 900};
    pact_context_t ctx = {.workload = &wl};
#ifdef PACT_MAX_WORKLOADS
    ctx.n_workloads = 1;
    ctx.workloads[0].target_pid = wl.target_pid;
    ctx.workload = &ctx.workloads[0];
#endif
    pebs_aggregator_t agg = {.pact_ctx = &ctx};
    const uint64_t data_size = PERF_BUFFER_PAGES * PAGE_SIZE;
    void *buffer = calloc(1, PAGE_SIZE + data_size);
    assert(buffer);
    struct perf_event_mmap_page *metadata = buffer;
    char *data = (char *)buffer + PAGE_SIZE;
    /* The first record wraps, including its pid/tid fields. */
    const uint64_t begin = data_size - 12;
    struct test_sample samples[] = {
        {{.type = PERF_RECORD_SAMPLE, .size = sizeof(struct test_sample)},
         0x1234, 900, 901, 0x400123},
        {{.type = PERF_RECORD_SAMPLE, .size = sizeof(struct test_sample)},
         0x1234, 900, 902, 0x400123},
        {{.type = PERF_RECORD_SAMPLE, .size = sizeof(struct test_sample)},
         0x5678, 999, 901, 0x800456},
        {{.type = PERF_RECORD_SAMPLE, .size = sizeof(struct test_sample)},
         0x5678, 900, 900, 0x800456},
    };
    for (size_t i = 0; i < 4; i++) {
        write_sample(data, data_size, begin + i * sizeof(samples[0]), &samples[i]);
    }
    metadata->data_tail = begin;
    metadata->data_head = begin + sizeof(samples);
    pebs_staged_sample_t staged[3];
    assert(read_one_pebs_mmap(&agg, buffer, staged, 3, 0) == 3);
    assert(metadata->data_tail == metadata->data_head);
    assert(staged[0].pid == 900 && staged[0].tid == 901);
    assert(staged[1].pid == 900 && staged[1].tid == 902);
    assert(staged[2].pid == 900 && staged[2].tid == 900);
    assert(staged[0].drain_time_ns > 0);
    assert(staged[2].drain_time_ns >= staged[0].drain_time_ns);
    assert(read_one_pebs_mmap(&agg, buffer, staged, 3, 0) == 0);

    char path[] = "/tmp/pact-pebs-tid-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    if (disabled) {
        unsetenv("PACT_PEBS_SAMPLES_CSV");
    } else {
        assert(setenv("PACT_PEBS_SAMPLES_CSV", path, 1) == 0);
    }
    assert(setenv("PACT_PEBS_SAMPLES_STRIDE", stride ? "2" : "1", 1) == 0);
    for (int i = 0; i < 3; i++) {
        uint64_t addr, ip;
        uint8_t tier;
        decode_pebs_sample(staged, i, &addr, &tier, &ip);
        assert(tier == 0);
        heat_dump_sample(staged[i].pid, staged[i].tid, ip, addr, staged[i].drain_time_ns);
    }
    if (disabled) {
        assert(g_heat_fp == NULL);
        struct stat st;
        assert(stat(path, &st) == 0 && st.st_size == 0);
    } else {
        assert(g_heat_fp);
        fclose(g_heat_fp);
        g_heat_fp = NULL;
        FILE *f = fopen(path, "r");
        assert(f);
        char header[128];
        assert(fgets(header, sizeof(header), f));
        assert(strcmp(header, "time_ns pid ip addr page_id tid\n") == 0);
        unsigned long long time_ns, region;
        unsigned long ip, addr;
        int pid, tid;
        int row = 0;
        while (fscanf(f, "%llu %d %lx %lx %llu %d", &time_ns, &pid, &ip, &addr,
                      &region, &tid) == 6) {
            int i = stride ? row * 2 : row;
            assert(i < 3 && pid == staged[i].pid && tid == staged[i].tid);
            assert(time_ns == staged[i].drain_time_ns);
            assert(ip == staged[i].ip && region == (addr >> 21));
            row++;
        }
        assert(feof(f) && row == (stride ? 2 : 3));
        fclose(f);
    }
    unlink(path);
    free(buffer);
    puts("PEBS TID decoder/export OK");
    return 0;
}
