#include <kernel.h>

/* Currently, the only time we suspend a kernel context is to perform
   asynchronous I/O on behalf of a page fault in kernel mode. This can
   only happen on one cpu - whichever one holds the kernel lock - and
   the kernel lock is held until the context is resumed. So a single
   free context is all that is necessary for the moment. As soon as we
   might suspend a context after releasing the kernel lock, or move
   away from a single kernel lock, we'll want to expand the number of
   available contexts to use here. In our world, suspending and
   resuming a kernel context is the exception, not the norm. */

static kernel_context spare_kernel_context;
struct mm_stats mm_stats;

context allocate_frame(heap h)
{
    context f = allocate_zero(h, total_frame_size());
    assert(f != INVALID_ADDRESS);
    init_frame(f);
    f[FRAME_HEAP] = u64_from_pointer(h);
    return f;
}

void deallocate_frame(context f)
{
    deallocate((heap)pointer_from_u64(f[FRAME_HEAP]), f, total_frame_size());
}

void *allocate_stack(heap h, u64 size)
{
    u64 padsize = pad(size, h->pagesize);
    void *base = allocate_zero(h, padsize);
    assert(base != INVALID_ADDRESS);
    return base + padsize - STACK_ALIGNMENT;
}

void deallocate_stack(heap h, u64 size, void *stack)
{
    u64 padsize = pad(size, h->pagesize);
    deallocate(h, u64_from_pointer(stack) - padsize + STACK_ALIGNMENT, padsize);
}

kernel_context allocate_kernel_context(heap h)
{
    u64 frame_size = total_frame_size();
    kernel_context c = allocate_zero(h, KERNEL_STACK_SIZE + frame_size);
    if (c == INVALID_ADDRESS)
        return c;
    init_frame(c->frame);
    // XXX set stack top here?
    c->frame[FRAME_HEAP] = u64_from_pointer(h);
    return c;
}

void deallocate_kernel_context(kernel_context c)
{
    deallocate((heap)pointer_from_u64(c->frame[FRAME_HEAP]), c, KERNEL_STACK_SIZE + total_frame_size());
}

boolean kernel_suspended(void)
{
    return spare_kernel_context == 0;
}

kernel_context suspend_kernel_context(void)
{
    cpuinfo ci = current_cpu();
    assert(spare_kernel_context);
    kernel_context saved = get_kernel_context(ci);
    set_kernel_context(ci, spare_kernel_context);
    spare_kernel_context = 0;
    return saved;
}

void resume_kernel_context(kernel_context c)
{
    cpuinfo ci = current_cpu();
    spare_kernel_context = get_kernel_context(ci);
    set_kernel_context(ci, c);
    frame_return(c->frame);
}

struct cpuinfo cpuinfos[MAX_CPUS];

static void init_cpuinfos(heap backed)
{
    /* We're stuck with a hard limit of 64 for now due to bitmask... */
    build_assert(MAX_CPUS <= 64);

    /* We'd like the aps to allocate for themselves, but we don't have
       per-cpu heaps just yet. */
    for (int i = 0; i < MAX_CPUS; i++) {
        cpuinfo ci = cpuinfo_from_id(i);
        /* state */
        set_running_frame(ci, 0);
        ci->id = i;
        ci->state = cpu_not_present;
        ci->have_kernel_lock = false;
        ci->thread_queue = allocate_queue(backed, MAX_THREADS);
        ci->last_timer_update = 0;
        ci->frcount = 0;

        init_cpuinfo_machine(ci, backed);

        /* frame and stacks */
        set_kernel_context(ci, allocate_kernel_context(backed));
    }

    cpuinfo ci = cpuinfo_from_id(0);
    set_running_frame(ci, frame_from_kernel_context(get_kernel_context(ci)));
    cpu_init(0);
}

void init_kernel_contexts(heap backed)
{
    spare_kernel_context = allocate_kernel_context(backed);
    assert(spare_kernel_context != INVALID_ADDRESS);
    init_cpuinfos(backed);
    current_cpu()->state = cpu_kernel;
}

void install_fallback_fault_handler(fault_handler h)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        context f = frame_from_kernel_context(get_kernel_context(cpuinfo_from_id(i)));
        f[FRAME_FAULT_HANDLER] = u64_from_pointer(h);
    }
}
