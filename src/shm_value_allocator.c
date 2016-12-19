//shm_value_allocator.c

#include <errno.h>
#include "sched_thread.h"
#include "shm_object_pool.h"
#include "shm_striping_allocator.h"
#include "shm_list.h"
#include "shmopt.h"
#include "shm_hashtable.h"
#include "shm_value_allocator.h"

static int shm_value_striping_alloc(struct shm_striping_allocator
        *allocator, const int size, struct shm_value *value)
{
    value->offset = shm_striping_allocator_alloc(allocator, size);
    if (value->offset < 0) {
        return ENOMEM;
    }

    value->index = allocator->index;
    value->size = size;
    return 0;
}

static int shm_value_allocator_do_alloc(struct shmcache_context *context,
        const int size, struct shm_value *value)
{
    int64_t allocator_offset;
    int64_t removed_offset;
    struct shm_striping_allocator *allocator;

    allocator_offset = shm_object_pool_first(&context->value_allocator.doing);

    logInfo("function: %s, allocator_offset: %"PRId64, __FUNCTION__, allocator_offset);

    while (allocator_offset > 0) {
        allocator = (struct shm_striping_allocator *)(context->segments.
                hashtable.base + allocator_offset);
        if (shm_value_striping_alloc(allocator, size, value) == 0) {
            return 0;
        }

        if ((shm_striping_allocator_free_size(allocator) <= context->config.
                va_policy.discard_memory_size) || (allocator->fail_times >
                context->config.va_policy.max_fail_times))
        {
            removed_offset = shm_object_pool_remove(&context->value_allocator.doing);
            if (removed_offset == allocator_offset) {
                allocator->in_which_pool = SHMCACHE_STRIPING_ALLOCATOR_POOL_DONE;
                shm_object_pool_push(&context->value_allocator.done, allocator_offset);
            } else {
                logCrit("file: "__FILE__", line: %d, "
                        "shm_object_pool_remove fail, "
                        "offset: %"PRId64" != expect: %"PRId64, __LINE__,
                        removed_offset, allocator_offset);
            }
        }

        allocator_offset = shm_object_pool_next(&context->value_allocator.doing);
    }

    return ENOMEM;
}

static int shm_value_allocator_recycle(struct shmcache_context *context)
{
    int64_t entry_offset;
    int64_t allocator_offset;
    struct shm_hash_entry *entry;
    struct shm_striping_allocator *allocator;
    struct shmcache_buffer key;
    int index;

    while ((entry_offset=shm_list_first(&context->list)) > 0) {
        logInfo("file: "__FILE__", line: %d, "
                "entry_offset: %"PRId64, __LINE__, entry_offset);

        entry = HT_ENTRY_PTR(context, entry_offset);
        index = entry->value.index.striping;
        key.data = entry->key;
        key.length = entry->key_len;
        if (shm_ht_delete(context, &key) != 0) {
            logCrit("file: "__FILE__", line: %d, "
                    "shm_ht_delete fail, index: %d, "
                    "entry offset: %"PRId64, __LINE__,
                    index, entry_offset);
            return EFAULT;
        }

        allocator = context->value_allocator.allocators + index;
        if (shm_striping_allocator_try_reset(allocator) == 0) {  //empty
            if (allocator->in_which_pool == SHMCACHE_STRIPING_ALLOCATOR_POOL_DONE) {
                allocator_offset =  (char *)allocator - context->segments.hashtable.base;
                if (shm_object_pool_remove_by(&context->value_allocator.done,
                            allocator_offset) >= 0) {
                    allocator->in_which_pool = SHMCACHE_STRIPING_ALLOCATOR_POOL_DOING;
                    shm_object_pool_push(&context->value_allocator.doing, allocator_offset);
                } else {
                    logCrit("file: "__FILE__", line: %d, "
                            "shm_object_pool_remove_by fail, "
                            "index: %d, offset: %"PRId64, __LINE__,
                            index, allocator_offset);
                    return EFAULT;
                }
            }
            return 0;
        }
    }

    logError("file: "__FILE__", line: %d, "
            "unable to recycle value memory", __LINE__);
    return ENOMEM;
}

int shm_value_allocator_alloc(struct shmcache_context *context,
        const int size, struct shm_value *value)
{
    int result;
    bool recycle;
    int64_t allocator_offset;
    struct shm_striping_allocator *allocator;

    if (shm_value_allocator_do_alloc(context, size, value) == 0) {
        return 0;
    }

    if (context->memory->vm_info.segment.count.current >=
            context->memory->vm_info.segment.count.max)
    {
        recycle = true;
    } else {
        allocator_offset = shm_object_pool_first(&context->value_allocator.done);
        if (allocator_offset > 0) {
            allocator = (struct shm_striping_allocator *)(context->segments.
                    hashtable.base + allocator_offset);
            recycle = (context->config.va_policy.avg_key_ttl > 0 && get_current_time() -
                    allocator->first_alloc_time >= context->config.va_policy.avg_key_ttl);
        } else {
            recycle = false;
        }
    }

    if (recycle && (result=shm_value_allocator_recycle(context)) != 0) {
        return result;
    } else if ((result=shmopt_create_value_segment(context)) != 0) {
        return result;
    }
    if ((result=shm_value_allocator_do_alloc(context, size, value)) != 0) {
            logError("file: "__FILE__", line: %d, "
                    "malloc %d bytes from shm fail", __LINE__, size);
    }
    return result;
}

int shm_value_allocator_free(struct shmcache_context *context,
        struct shm_value *value)
{
    struct shm_striping_allocator *allocator;

    allocator = context->value_allocator.allocators + value->index.striping;
    shm_striping_allocator_free(allocator, value->size);
    return 0;
}

