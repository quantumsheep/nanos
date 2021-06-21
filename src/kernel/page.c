/* TODO: implement drain */

#include <kernel.h>

#ifdef KERNEL
static struct spinlock pt_lock;
#define pagetable_lock() u64 _savedflags = spin_lock_irq(&pt_lock)
#define pagetable_unlock() spin_unlock_irq(&pt_lock, _savedflags)
#else
#define pagetable_lock()
#define pagetable_unlock()
#endif

//#define PAGE_INIT_DEBUG
//#define PAGE_DEBUG
//#define PAGE_UPDATE_DEBUG
//#define PAGE_TRAVERSE_DEBUG

#if defined(PAGE_DEBUG) && !defined(BOOT)
#define page_debug(x, ...) do {log_printf("PAGE", "%s: " x, __func__, ##__VA_ARGS__);} while(0)
#else
#define page_debug(x, ...)
#endif

#ifdef PAGE_INIT_DEBUG
#define page_init_debug(x) early_debug(x)
#define page_init_debug_u64(x) early_debug_u64(x)
#else
#define page_init_debug(x)
#define page_init_debug_u64(x)
#endif

#define PAGEMEM_ALLOC_SIZE PAGESIZE_2M

static struct {
    range current_phys;
    heap pageheap;
    void *initial_map;
    u64 initial_physbase;
} pagemem;

#ifndef physical_from_virtual
physical physical_from_virtual(void *x)
{
    u64 a = u64_from_pointer(x);
    if (is_huge_backed_address(a))
        return phys_from_huge_backed_virt(a);
    u64 p;
    pagetable_lock();
    p = __physical_from_virtual_locked(x);
    pagetable_unlock();
    return p;
}
#endif

u64 *pointer_from_pteaddr(u64 pa)
{
#ifdef KERNEL
    if (!pagemem.pageheap) {
        u64 offset = pa - pagemem.initial_physbase;
        assert(pagemem.initial_map);
        assert(pa >= pagemem.initial_physbase); /* may legitimately extend past end */
        return pagemem.initial_map + offset;
    }
    return virt_from_huge_backed_phys(pa);
#else
    return pointer_from_u64(pa);
#endif
}

#ifdef KERNEL
void *allocate_table_page(u64 *phys)
{
    page_init_debug("allocate_table_page:");
    if (range_span(pagemem.current_phys) == 0) {
        assert(pagemem.pageheap);
        page_init_debug(" [new alloc, va: ");
        u64 va = allocate_u64(pagemem.pageheap, PAGEMEM_ALLOC_SIZE);
        if (va == INVALID_PHYSICAL) {
            msg_err("failed to allocate page table memory\n");
            return INVALID_ADDRESS;
        }
        page_init_debug_u64(va);
        page_init_debug("] ");
        assert(is_huge_backed_address(va));
        pagemem.current_phys = irangel(phys_from_huge_backed_virt(va), PAGEMEM_ALLOC_SIZE);
    }

    *phys = pagemem.current_phys.start;
    pagemem.current_phys.start += PAGESIZE;
    void *p = pointer_from_pteaddr(*phys);
    page_init_debug(" phys: ");
    page_init_debug_u64(*phys);
    zero(p, PAGESIZE);
    return p;
}
#else
/* Bootloader use: single, identity-mapped pages */
void *allocate_table_page(u64 *phys)
{
    void *p = allocate_zero(pagemem.pageheap, PAGESIZE);
    *phys = u64_from_pointer(p);
    return p;
}
#endif

#define PTE_ENTRIES U64_FROM_BIT(9)
static boolean recurse_ptes(u64 pbase, int level, u64 vstart, u64 len, u64 laddr, entry_handler ph)
{
    int shift = pt_level_shift(level);
    u64 lsize = U64_FROM_BIT(shift);
    u64 start_idx = vstart > laddr ? ((vstart - laddr) >> shift) : 0;
    u64 x = vstart + len - laddr;
    u64 end_idx = MIN(pad(x, lsize) >> shift, PTE_ENTRIES);
    u64 offset = start_idx << shift;

#ifdef PAGE_TRAVERSE_DEBUG
    rprintf("   pbase 0x%lx, level %d, shift %d, lsize 0x%lx, laddr 0x%lx,\n"
            "      start_idx %ld, end_idx %ld, offset 0x%lx\n",
            pbase, level, shift, lsize, laddr, start_idx, end_idx, offset);
#endif

    assert(start_idx <= PTE_ENTRIES);
    assert(end_idx <= PTE_ENTRIES);

    for (u64 i = start_idx; i < end_idx; i++, offset += lsize) {
        u64 addr = canonize_address(laddr + (i << shift));
        u64 pteaddr = pbase + (i * sizeof(u64));
        u64 *pte = pointer_from_pteaddr(pteaddr);
#ifdef PAGE_TRAVERSE_DEBUG
        rprintf("   idx %d, offset 0x%lx, addr 0x%lx, pteaddr 0x%lx, *pte %p\n",
                i, offset, addr, pteaddr, *pte);
#endif
        if (!apply(ph, level, addr, pte))
            return false;
        if (pte_is_present(*pte) && level < PT_PTE_LEVEL &&
            (level == PT_FIRST_LEVEL || !pte_is_block_mapping(*pte)) &&
            !recurse_ptes(page_from_pte(*pte), level + 1, vstart, len,
                          laddr + offset, ph))
            return false;
    }
    return true;
}

boolean traverse_ptes(u64 vaddr, u64 length, entry_handler ph)
{
#ifdef PAGE_TRAVERSE_DEBUG
    rprintf("traverse_ptes vaddr 0x%lx, length 0x%lx\n", vaddr, length);
#endif
    pagetable_lock();
    boolean result = recurse_ptes(get_pagetable_base(vaddr), PT_FIRST_LEVEL,
                                  vaddr & MASK(VIRTUAL_ADDRESS_BITS), length, 0, ph);
    pagetable_unlock();
    return result;
}

/* called with lock held */
closure_function(0, 3, boolean, validate_entry,
                 int, level, u64, vaddr, pteptr, entry)
{
    return pte_is_present(pte_from_pteptr(entry));
}

/* validate that all pages in vaddr range [base, base + length) are present */
boolean validate_virtual(void * base, u64 length)
{
    page_debug("base %p, length 0x%lx\n", base, length);
    return traverse_ptes(u64_from_pointer(base), length, stack_closure(validate_entry));
}

/* called with lock held */
closure_function(2, 3, boolean, update_pte_flags,
                 pageflags, flags, flush_entry, fe,
                 int, level, u64, addr, pteptr, entry)
{
    /* we only care about present ptes */
    pte orig_pte = pte_from_pteptr(entry);
    if (!pte_is_present(orig_pte) || !pte_is_mapping(level, orig_pte))
        return true;

    pte_set(entry, (orig_pte & ~PAGE_PROT_FLAGS) | bound(flags).w);
#ifdef PAGE_UPDATE_DEBUG
    page_debug("update 0x%lx: pte @ 0x%lx, 0x%lx -> 0x%lx\n", addr, entry, old,
               pte_from_pteptr(entry).w);
#endif
    page_invalidate(bound(fe), addr);
    return true;
}

/* Update access protection flags for any pages mapped within a given area */
void update_map_flags(u64 vaddr, u64 length, pageflags flags)
{
    flags = pageflags_no_minpage(flags);
    page_debug("%s: vaddr 0x%lx, length 0x%lx, flags 0x%lx\n", __func__, vaddr, length, flags.w);

    /* Catch any attempt to change page flags in a huge_backed mapping */
    assert(!intersects_huge_backed(irangel(vaddr, length)));
    flush_entry fe = get_page_flush_entry();
    traverse_ptes(vaddr, length, stack_closure(update_pte_flags, flags, fe));
    page_invalidate_sync(fe, ignore);
}

static boolean map_level(u64 *table_ptr, int level, range v, u64 *p, u64 flags, flush_entry fe);

/* called with lock held */
closure_function(3, 3, boolean, remap_entry,
                 u64, new, u64, old, flush_entry, fe,
                 int, level, u64, curr, pteptr, entry)
{
    u64 offset = curr - bound(old);
    u64 oldentry = pte_from_pteptr(entry);
    u64 new_curr = bound(new) + offset;
    u64 phys = page_from_pte(oldentry);
    u64 flags = flags_from_pte(oldentry);
    int map_order = pte_order(level, oldentry);

#ifdef PAGE_UPDATE_DEBUG
    page_debug("level %d, old curr 0x%lx, phys 0x%lx, new curr 0x%lx, entry 0x%lx, *entry 0x%lx, flags 0x%lx\n",
               level, curr, phys, new_curr, entry, *entry, flags);
#endif

    /* only look at ptes at this point */
    if (!pte_is_present(oldentry) || !pte_is_mapping(level, oldentry))
        return true;

    /* transpose mapped page */
    assert(map_level(pointer_from_pteaddr(get_pagetable_base(new_curr)), PT_FIRST_LEVEL,
                     irangel(new_curr & MASK(VIRTUAL_ADDRESS_BITS), U64_FROM_BIT(map_order)),
                     &phys, flags, bound(fe)));

    /* reset old entry */
    *entry = 0;

    /* invalidate old mapping (map_page takes care of new)  */
    page_invalidate(bound(fe), curr);

    return true;
}

/* We're just going to do forward traversal, for we don't yet need to
   support overlapping moves. Should the latter become necessary
   (e.g. to support MREMAP_FIXED in mremap(2) without depending on
   MREMAP_MAYMOVE), write a "traverse_ptes_reverse" to walk pages
   from high address to low (like memcpy).
*/
void remap_pages(u64 vaddr_new, u64 vaddr_old, u64 length)
{
    page_debug("vaddr_new 0x%lx, vaddr_old 0x%lx, length 0x%lx\n", vaddr_new, vaddr_old, length);
    if (vaddr_new == vaddr_old)
        return;
    assert(range_empty(range_intersection(irange(vaddr_new, vaddr_new + length),
                                          irange(vaddr_old, vaddr_old + length))));
    flush_entry fe = get_page_flush_entry();
    traverse_ptes(vaddr_old, length, stack_closure(remap_entry, vaddr_new, vaddr_old, fe));
    page_invalidate_sync(fe, ignore);
}

/* called with lock held */
closure_function(0, 3, boolean, zero_page,
                 int, level, u64, addr, pteptr, entry)
{
    u64 e = pte_from_pteptr(entry);
    if (pte_is_present(e) && pte_is_mapping(level, e)) {
        u64 size = pte_map_size(level, e);
#ifdef PAGE_UPDATE_DEBUG
        page_debug("addr 0x%lx, size 0x%lx\n", addr, size);
#endif
        zero(pointer_from_u64(addr), size);
    }
    return true;
}

void zero_mapped_pages(u64 vaddr, u64 length)
{
    traverse_ptes(vaddr, length, stack_closure(zero_page));
}

/* called with lock held */
closure_function(2, 3, boolean, unmap_page,
                 range_handler, rh, flush_entry, fe,
                 int, level, u64, vaddr, pteptr, entry)
{
    range_handler rh = bound(rh);
    u64 old_entry = pte_from_pteptr(entry);
    if (pte_is_present(old_entry) && pte_is_mapping(level, old_entry)) {
#ifdef PAGE_UPDATE_DEBUG
        page_debug("rh %p, level %d, vaddr 0x%lx, entry %p, *entry 0x%lx\n",
                   rh, level, vaddr, entry, *entry);
#endif
        *entry = 0;
        page_invalidate(bound(fe), vaddr);
        if (rh) {
            apply(rh, irangel(page_from_pte(old_entry),
                              pte_map_size(level, old_entry)));
        }
    }
    return true;
}

/* Be warned: the page table lock is held when rh is called; don't try
   to modify the page table while traversing it */
void unmap_pages_with_handler(u64 virtual, u64 length, range_handler rh)
{
    assert(!((virtual & PAGEMASK) || (length & PAGEMASK)));
    flush_entry fe = get_page_flush_entry();
    traverse_ptes(virtual, length, stack_closure(unmap_page, rh, fe));
    page_invalidate_sync(fe, ignore);
}

#define next_addr(a, mask) (a = (a + (mask) + 1) & ~(mask))
#define INDEX_MASK (PAGEMASK >> 3)
static boolean map_level(u64 *table_ptr, int level, range v, u64 *p, u64 flags, flush_entry fe)
{
    int shift = pt_level_shift(level);
    u64 mask = MASK(shift);
    // XXX this was level > 2, but that didn't seem right - validate me
    u64 vlbase = level > PT_FIRST_LEVEL ? v.start & ~MASK(pt_level_shift(level - 1)) : 0;
    int first_index = (v.start >> shift) & INDEX_MASK;
    int last_index = ((v.end - 1) >> shift) & INDEX_MASK;

    page_init_debug("\nmap_level: table_ptr ");
    page_init_debug_u64(u64_from_pointer(table_ptr));
    page_init_debug(", level ");
    page_init_debug_u64(level);
    page_init_debug("\n   v ");
    page_init_debug_u64(v.start);
    page_init_debug(" - ");
    page_init_debug_u64(v.end);
    page_init_debug(", p ");
    page_init_debug_u64(*p);
    page_init_debug(" first ");
    page_init_debug_u64(first_index);
    page_init_debug(" last ");
    page_init_debug_u64(last_index);
    page_init_debug("\n");
    assert(first_index <= last_index);
    assert(table_ptr && table_ptr != INVALID_ADDRESS);

    for (int i = first_index; i <= last_index; i++, next_addr(v.start, mask)) {
        boolean invalidate = false; /* page at v.start */
        page_init_debug("   index ");
        page_init_debug_u64(i);
        page_init_debug(", v.start ");
        page_init_debug_u64(v.start);
        page_init_debug(", p ");
        page_init_debug_u64(*p);
        u64 pte = table_ptr[i];
        page_init_debug(", pte ");
        page_init_debug_u64(pte);
        page_init_debug("\n");
        if (!pte_is_present(pte)) {
            if (level == PT_PTE_LEVEL) {
                pte = page_pte(*p, flags);
                next_addr(*p, mask);
                invalidate = true;
            } else if (!flags_has_minpage(flags) && level > PT_FIRST_LEVEL && (v.start & mask) == 0 &&
                       (*p & mask) == 0 && range_span(v) >= U64_FROM_BIT(shift)) {
                pte = block_pte(*p, flags);
                next_addr(*p, mask);
                invalidate = true;
            } else {
                page_init_debug("      new level: ");
                void *tp;
                u64 tp_phys;
                if ((tp = allocate_table_page(&tp_phys)) == INVALID_ADDRESS) {
                    msg_err("failed to allocate page table memory\n");
                    return false;
                }
                /* user and writable are AND of flags from all levels */
                pte = new_level_pte(tp_phys);
                u64 end = vlbase | (((u64)(i + 1)) << shift);
                /* length instead of end to avoid overflow at end of space */
                u64 len = MIN(range_span(v), end - v.start);
                page_init_debug("  end ");
                page_init_debug_u64(end);
                page_init_debug(", len ");
                page_init_debug_u64(len);
                page_init_debug("\n");
                if (!map_level(tp, level + 1, irangel(v.start, len), p, flags, fe))
                    return false;
            }
            page_init_debug("      pte @ ");
            page_init_debug_u64(u64_from_pointer(&table_ptr[i]));
            page_init_debug(" = ");
            page_init_debug_u64(pte);
            page_init_debug("\n");
            table_ptr[i] = pte;
            if (invalidate)
                page_invalidate(fe, v.start);
        } else {
            /* fail if page or block already installed */
            if (pte_is_mapping(level, pte)) {
                msg_err("would overwrite entry: level %d, v %R, pa 0x%lx, "
                        "flags 0x%lx, index %d, entry 0x%lx\n", level, v, *p,
                        flags, i, pte);
                return false;
            }
            u64 nexttable = page_from_pte(pte);
            u64 *nexttable_ptr = pointer_from_pteaddr(nexttable);
            u64 end = vlbase | (((u64)(i + 1)) << shift);
            u64 len = MIN(range_span(v), end - v.start);
            if (!map_level(nexttable_ptr, level + 1, irangel(v.start, len), p, flags, fe))
                return false;
        }
    }
    return true;
}

void map(u64 v, physical p, u64 length, pageflags flags)
{
    page_init_debug("map: v ");
    page_init_debug_u64(v);
    page_init_debug(", p ");
    page_init_debug_u64(p);
    page_init_debug(", length ");
    page_init_debug_u64(length);
    page_init_debug(", flags ");
    page_init_debug_u64(flags.w);
    page_init_debug("\n   called from ");
    page_init_debug_u64(u64_from_pointer(__builtin_return_address(0)));
    page_init_debug("\n");

    assert((v & PAGEMASK) == 0);
    assert((p & PAGEMASK) == 0);
    range r = irangel(v & MASK(VIRTUAL_ADDRESS_BITS), pad(length, PAGESIZE));
    flush_entry fe = get_page_flush_entry();
    pagetable_lock();
    u64 *table_ptr = pointer_from_pteaddr(get_pagetable_base(v));
    if (!map_level(table_ptr, PT_FIRST_LEVEL, r, &p, flags.w, fe)) {
        pagetable_unlock();
        rprintf("ra %p\n", __builtin_return_address(0));
        print_frame_trace_from_here();
        halt("map failed for v 0x%lx, p 0x%lx, len 0x%lx, flags 0x%lx\n",
             v, p, length, flags.w);
    }
    page_init_debug("map_level done\n");
    page_invalidate_sync(fe, 0);
    page_init_debug("invalidate sync done\n");
    pagetable_unlock();
}

void unmap(u64 virtual, u64 length)
{
    page_init_debug("unmap v: ");
    page_init_debug_u64(virtual);
    page_init_debug(", length: ");
    page_init_debug_u64(length);
    page_init_debug("\n");
    unmap_pages(virtual, length);
}

/* Unless this is a bootloader build, pageheap must be the huge backed heap. */
void init_page_tables(heap pageheap)
{
    page_init_debug("init_page_tables: pageheap ");
    page_init_debug_u64(u64_from_pointer(pageheap));
    page_init_debug("\n");
#ifdef KERNEL
    /* A map could happen here, so do before setting pageheap. */
    huge_backed_heap_add_physical((backed_heap)pageheap, pagemem.initial_physbase);
#endif
    pagemem.pageheap = pageheap;
}

#ifdef KERNEL
/* Use a fixed area for page table allocation, either before MMU init or with
   only initial mappings set up. */
void init_page_initial_map(void *initial_map, range phys)
{
    page_init_debug("init_page_initial_map: initial_map ");
    page_init_debug_u64(u64_from_pointer(initial_map));
    page_init_debug(", phys ");
    page_init_debug_u64(phys.start);
    page_init_debug(", length ");
    page_init_debug_u64(range_span(phys));
    page_init_debug("\n");
    spin_lock_init(&pt_lock);
    pagemem.current_phys = phys;
    pagemem.pageheap = 0;
    pagemem.initial_map = initial_map;
    pagemem.initial_physbase = phys.start;
}
#endif
