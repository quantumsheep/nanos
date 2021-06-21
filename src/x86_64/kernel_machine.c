#include <kernel.h>
#include <apic.h>

/* stub placeholder, short of a real generic interface */
void send_ipi(u64 cpu, u8 vector)
{
    apic_ipi(cpu, 0, vector);
}

void interrupt_exit(void)
{
    lapic_eoi();
}

heap allocate_tagged_region(kernel_heaps kh, u64 tag)
{
    heap h = heap_locked(kh);
    heap p = (heap)heap_physical(kh);
    assert(tag < U64_FROM_BIT(VA_TAG_WIDTH));
    u64 tag_base = KMEM_BASE | (tag << VA_TAG_OFFSET);
    u64 tag_length = U64_FROM_BIT(VA_TAG_OFFSET);
    heap v = (heap)create_id_heap(h, (heap)heap_huge_backed(kh), tag_base, tag_length, p->pagesize, false);
    assert(v != INVALID_ADDRESS);
    heap backed = (heap)allocate_page_backed_heap(h, v, p, p->pagesize, false);
    if (backed == INVALID_ADDRESS)
        return backed;

    /* reserve area in virtual_huge */
    assert(id_heap_set_area(heap_virtual_huge(kh), tag_base, tag_length, true, true));

    /* tagged mcache range of 32 to 1M bytes (131072 table buckets) */
    build_assert(TABLE_MAX_BUCKETS * sizeof(void *) <= 1 << 20);
    return allocate_mcache(h, backed, 5, 20, PAGESIZE_2M);
}

void clone_frame_pstate(context dest, context src)
{
    runtime_memcpy(dest, src, sizeof(u64) * (FRAME_N_PSTATE + 1));
    runtime_memcpy(dest + FRAME_EXTENDED_SAVE, src + FRAME_EXTENDED_SAVE, extended_frame_size);
}

void init_cpuinfo_machine(cpuinfo ci, heap backed)
{
    ci->m.self = &ci->m;
    ci->m.exception_stack = allocate_stack(backed, EXCEPT_STACK_SIZE);
    ci->m.int_stack = allocate_stack(backed, INT_STACK_SIZE);
}

void init_frame(context f)
{
    assert((u64_from_pointer(f) & 63) == 0);
    xsave(f);
}
