#include "test.h"

TEST_BOTH(01_all_any) {
    using Bool = Array<bool>;

    scoped_set_log_level ssll(LogLevel::Info);
    for (uint32_t i = 0; i < 100; ++i) {
        uint32_t size = 23*i*i*i + 1;

        Bool f = full<Bool>(false, size),
             t = full<Bool>(true, size);

        jitc_eval(f, t);

        jitc_assert(all(t) && any(t) && !all(f) && !any(f));

        for (int j = 0; j < 100; ++j) {
            uint32_t index = (uint32_t) rand() % size;
            f.write(index, true);
            t.write(index, false);

            if (size == 1)
                jitc_assert(!all(t) && !any(t) && all(f) && any(f));
            else
                jitc_assert(!all(t) && any(t) && !all(f) && any(f));

            f.write(index, false);
            t.write(index, true);
            jitc_assert(all(t) && any(t) && !all(f) && !any(f));
        }
    }
}

TEST_BOTH(02_scan) {
    scoped_set_log_level ssll(LogLevel::Info);
    for (uint32_t i = 0; i < 100; ++i) {
        uint32_t size = 23*i*i*i + 1;

        UInt32 result, ref;

        if (i < 15) {
            result = arange<UInt32>(size);
            ref    = result * (result - 1) / 2;
        } else {
            result = full<UInt32>(1, size);
            ref    = arange<UInt32>(size);
        }
        jitc_eval(result, ref);
        jitc_scan_u32(result.data(), size, result.data());
        jitc_assert(result == ref);
    }
}

TEST_BOTH(03_compress) {
    scoped_set_log_level ssll(LogLevel::Info);
    for (uint32_t i = 0; i < 30; ++i) {
        uint32_t size = 23*i*i*i + 1;
        for (uint32_t j = 0; j <= i; ++j) {
            uint32_t n_ones = 23*j*j*j + 1;

            jitc_log(LogLevel::Info, "===== size=%u, ones=%u =====", size, n_ones);
            uint8_t *data      = (uint8_t *) jitc_malloc(AllocType::Host, size);
            uint32_t *perm     = (uint32_t *) jitc_malloc(Float::IsCUDA ? AllocType::Device :
                                                                          AllocType::Host,
                                                          size * sizeof(uint32_t)),
                     *perm_ref = (uint32_t *) jitc_malloc(AllocType::Host, size * sizeof(uint32_t));
            memset(data, 0, size);

            for (size_t i = 0; i < n_ones; ++i) {
                uint32_t index = rand() % size;
                data[index] = 1;
            }

            uint32_t ref_count = 0;
            for (size_t i = 0; i < size; ++i) {
                if (data[i])
                    perm_ref[ref_count++] = (uint32_t) i;
            }

            data = (uint8_t *) jitc_malloc_migrate(
                data, Float::IsCUDA ? AllocType::Device : AllocType::Host);

            uint32_t count = jitc_compress(data, size, perm);
            perm = (uint32_t *) jitc_malloc_migrate(perm, AllocType::Host);
            jitc_sync_stream();

            jitc_assert(count == ref_count);
            jitc_assert(memcmp(perm, perm_ref, ref_count * sizeof(uint32_t)) == 0);

            jitc_free(data);
            jitc_free(perm);
            jitc_free(perm_ref);
        }
    }
}

TEST_BOTH(04_mkperm) {
    scoped_set_log_level ssll(LogLevel::Info);
    srand(0);
    for (uint32_t i = 0; i < 30; ++i) {
        uint32_t size = 23*i*i*i + 1;
        for (uint32_t j = 0; j <= i; ++j) {
            uint32_t n_buckets = 23*j*j*j + 1;

            jitc_log(LogLevel::Info, "===== size=%u, buckets=%u =====", size, n_buckets);
            uint32_t *data    = (uint32_t *) jitc_malloc(AllocType::Host, size * sizeof(uint32_t)),
                     *perm    = (uint32_t *) jitc_malloc(Float::IsCUDA ? AllocType::Device :
                                                                         AllocType::Host,
                                                         size * sizeof(uint32_t)),
                     *offsets = (uint32_t *) jitc_malloc(Float::IsCUDA ? AllocType::HostPinned :
                                                                         AllocType::Host,
                                                         (n_buckets * 4 + 1) * sizeof(uint32_t));
            uint64_t *ref = new uint64_t[size];

            for (size_t i = 0; i < size; ++i) {
                uint32_t value = rand() % n_buckets;
                data[i] = value;
                ref[i] = (((uint64_t) value) << 32) | i;
            }

            data = (uint32_t *) jitc_malloc_migrate(data, Float::IsCUDA ? AllocType::Device : AllocType::Host);
            uint32_t num_unique = jitc_mkperm(data, size, n_buckets, perm, offsets);

            perm = (uint32_t *) jitc_malloc_migrate(perm, AllocType::Host);
            jitc_sync_stream();

            struct Bucket {
                uint32_t id;
                uint32_t start;
                uint32_t size;
                uint32_t unused;
            };

            Bucket *buckets = (Bucket *) offsets;

            std::sort(ref, ref + size);
            std::sort(buckets, buckets + num_unique,
                [](const Bucket &a, const Bucket &b) {
                    return a.id < b.id;
                }
            );

            uint32_t total_size = 0;
            for (uint32_t i = 0; i < num_unique; ++i)
                total_size += buckets[i].size;
            if (total_size != size)
                jitc_fail("Size mismatch: %u vs %u\n", total_size, size);

            const uint64_t *ref_ptr = ref;
            for (uint32_t i = 0; i < num_unique; ++i) {
                const Bucket &entry = buckets[i];
                uint32_t *perm_cur = perm + entry.start;

#if 0
                for (size_t j = 0; j < entry.size; ++j) {
                    uint64_t ref_value = ref_ptr[j];
                    uint32_t bucket_id = (uint32_t) (ref_value >> 32);
                    uint32_t perm_index = (uint32_t) ref_value;
                    fprintf(stderr, "id=%u/%u perm=%u/%u\n", entry.id, bucket_id, perm_cur[j], perm_index);
                }
#endif

                std::sort(perm_cur, perm_cur + entry.size);

                for (size_t j = 0; j < entry.size; ++j) {
                    uint64_t ref_value = *ref_ptr++;
                    uint32_t bucket_id = (uint32_t) (ref_value >> 32);
                    uint32_t perm_index = (uint32_t) ref_value;

                    if (bucket_id != entry.id)
                        jitc_fail("Mismatched bucket ID");
                    if (perm_index != perm_cur[j])
                        jitc_fail("Mismatched permutation index");
                }
            }
            jitc_free(data);
            jitc_free(perm);
            jitc_free(offsets);
            delete[] ref;
        }
    }
}

TEST_BOTH(05_block_ops) {
    Float a(0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f);

    jitc_log(Info, "block_copy: %s\n", block_copy(a, 3).str());
    jitc_log(Info, "block_sum:  %s\n", block_sum(a, 3).str());
}

TEST_LLVM(06_parallel_scatter_add) {
    scoped_set_log_level ssll(LogLevel::Info);
    UInt32 a = zero<UInt32>(10);

    UInt32 index = block_copy(arange<UInt32>(10), 1024 * 1024 + 10);
    scatter_add(a, UInt32(1), index);
    UInt32 ref = full<UInt32>(1024 * 1024 + 10, 10);
    jitc_assert(ref == a);
}
