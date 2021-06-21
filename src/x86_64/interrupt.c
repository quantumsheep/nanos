#include <kernel.h>
#include <kvm_platform.h>
#include <region.h>
#include <apic.h>
#include <symtab.h>
#include <drivers/acpi.h>

//#define INT_DEBUG
#ifdef INT_DEBUG
#define int_debug(x, ...) do {log_printf("  INT", x, ##__VA_ARGS__);} while(0)
#else
#define int_debug(x, ...)
#endif

#define INTERRUPT_VECTOR_START 32 /* end of exceptions; defined by architecture */
#define MAX_INTERRUPT_VECTORS  256 /* as defined by architecture; we may have less */

typedef struct inthandler {
    struct list l;
    thunk t;
    const char *name;
} *inthandler;

static const char *interrupt_names[MAX_INTERRUPT_VECTORS] = {
    "Divide by 0",
    "Reserved",
    "NMI Interrupt",
    "Breakpoint (INT3)",
    "Overflow (INTO)",
    "Bounds range exceeded (BOUND)",
    "Invalid opcode (UD2)",
    "Device not available (WAIT/FWAIT)",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 FPU error",
    "Alignment check",
    "Machine check",
    "SIMD Floating-Point Exception",
    "reserved 14",
    "reserved 15",
    "reserved 16",
    "reserved 17",
    "reserved 18",
    "reserved 19",
    "reserved 1a",
    "reserved 1b",
    "reserved 1c",
    "reserved 1d",
    "reserved 1e",
    "reserved 1f"};

static char* textoreg[] = {
    "   rax", //0
    "   rbx", //1
    "   rcx", //2
    "   rdx", //3
    "   rsi", //4
    "   rdi", //5
    "   rbp", //6
    "   rsp", //7
    "    r8", //8
    "    r9", //9
    "   r10", //10
    "   r11", //11
    "   r12", //12
    "   r13", //13
    "   r14", //14
    "   r15", //15
    "   rip", //16
    "rflags", //17
    "    ss", //18
    "    cs", //19
    "    ds", //20
    "    es", //21
    "fsbase", //22
    "gsbase", //23
    "vector", //24
};

static inline char *register_name(u64 s)
{
    return textoreg[s];
}

static u64 *idt;

static inline void *idt_from_interrupt(int interrupt)
{
    return pointer_from_u64((u64_from_pointer(idt) + 2 * sizeof(u64) * interrupt));
}

/* XXX Sigh...the noinline is a workaround for an issue where a clang
   build on macos is somehow leading to incorrect IDT entries. This
   needs more investigation.

   https://github.com/nanovms/nanos/issues/1060
*/
static void __attribute__((noinline)) write_idt(int interrupt, u64 offset, u64 ist)
{
    u64 selector = 0x08;
    u64 type_attr = 0x8e;
    u64 *target = idt_from_interrupt(interrupt);

    target[0] = ((selector << 16) | (offset & MASK(16)) | /* 31 - 0 */
                 (((offset >> 16) & MASK(16)) << 48) | (type_attr << 40) | (ist << 32)); /* 63 - 32 */
    target[1] = offset >> 32;   /*  95 - 64 */
}

static thunk *handlers;
u32 spurious_int_vector;

extern void *text_start;
extern void *text_end;
void frame_trace(u64 *fp)
{
    for (unsigned int frame = 0; frame < FRAME_TRACE_DEPTH; frame++) {
        if (!validate_virtual(fp, sizeof(u64)) ||
            !validate_virtual(fp + 1, sizeof(u64)))
            break;

        u64 n = fp[1];
        if (n == 0)
            break;
        print_u64(u64_from_pointer(fp + 1));
        rputs(":   ");
        fp = pointer_from_u64(fp[0]);
        print_u64_with_sym(n);
        rputs("\n");
    }
}

void print_frame_trace_from_here(void)
{
    u64 rbp;
    asm("movq %%rbp, %0" : "=r" (rbp));
    frame_trace(pointer_from_u64(rbp));
}

void print_stack(context c)
{
    rputs("\nframe trace:\n");
    frame_trace(pointer_from_u64(c[FRAME_RBP]));

    rputs("\nstack trace:\n");
    u64 *sp = pointer_from_u64(c[FRAME_RSP]);
    for (u64 *x = sp; x < (sp + STACK_TRACE_DEPTH) &&
             validate_virtual(x, sizeof(u64)); x++) {
        print_u64(u64_from_pointer(x));
        rputs(":   ");
        print_u64_with_sym(*x++);
        rputs("\n");
    }
    rputs("\n");
}

void print_frame(context f)
{
    u64 v = f[FRAME_VECTOR];
    rputs(" interrupt: ");
    print_u64(v);
    if (v < INTERRUPT_VECTOR_START) {
        rputs(" (");
        rputs((char *)interrupt_names[v]);
        rputs(")");
    }
    rputs("\n     frame: ");
    print_u64_with_sym(u64_from_pointer(f));
    rputs("\n");

    if (v == 13 || v == 14) {
	rputs("error code: ");
	print_u64(f[FRAME_ERROR_CODE]);
	rputs("\n");
    }

    // page fault
    if (v == 14)  {
        rputs("   address: ");
        print_u64_with_sym(f[FRAME_CR2]);
        rputs("\n");
    }
    
    rputs("\n");
    for (int j = 0; j < 24; j++) {
        rputs(register_name(j));
        rputs(": ");
        print_u64_with_sym(f[j]);
        rputs("\n");
    }
}

extern u32 n_interrupt_vectors;
extern u32 interrupt_vector_size;
extern void * interrupt_vectors;

NOTRACE
void common_handler()
{
    /* XXX yes, this will be a problem on a machine check or other
       fault while in an int handler...need to fix in interrupt_common */
    cpuinfo ci = current_cpu();
    context f = get_running_frame(ci);
    int i = f[FRAME_VECTOR];

    if (i >= n_interrupt_vectors) {
        console("\nexception for invalid interrupt vector\n");
        goto exit_fault;
    }

    // if we were idle, we are no longer
    atomic_clear_bit(&idle_cpu_mask, ci->id);

    int_debug("[%02d] # %d (%s), state %s, frame %p, rip 0x%lx, cr2 0x%lx\n",
              ci->id, i, interrupt_names[i], state_strings[ci->state],
              f, f[FRAME_RIP], f[FRAME_CR2]);

    /* enqueue an interrupted user thread, unless the page fault handler should take care of it */
    if (ci->state == cpu_user && i >= INTERRUPT_VECTOR_START && !shutting_down) {
        int_debug("int sched %F\n", f[FRAME_RUN]);
        schedule_frame(f);
    }

    if (i == spurious_int_vector)
        frame_return(f);        /* direct return, no EOI */

    /* Unless there's some reason to handle a page fault in interrupt
       mode, this should always be terminal.

       This really should include kernel mode, too, but we're for the
       time being allowing the kernel to take page faults...which
       really isn't sustainable unless we want fine-grained locking
       around the vmaps and page tables. Validating user buffers will
       get rid of this requirement (and allow us to add the check for
       cpu_kernel here too).
    */
    if (ci->state == cpu_interrupt) {
        console("\nexception during interrupt handling\n");
        goto exit_fault;
    }

    if (f[FRAME_FULL]) {
        console("\nframe ");
        print_u64(u64_from_pointer(f));
        console(" already full\n");
        goto exit_fault;
    }
    f[FRAME_FULL] = true;

    /* invoke handler if available, else general fault handler */
    if (handlers[i]) {
        ci->state = cpu_interrupt;
        apply(handlers[i]);
        if (i >= INTERRUPT_VECTOR_START)
            lapic_eoi();
    } else {
        /* fault handlers likely act on cpu state, so don't change it */
        fault_handler fh = pointer_from_u64(f[FRAME_FAULT_HANDLER]);
        if (fh) {
            context retframe = apply(fh, f);
            if (retframe)
                frame_return(retframe);
        } else {
            console("\nno fault handler for frame ");
            print_u64(u64_from_pointer(f));
            /* make a half attempt to identify it short of asking unix */
            /* we should just have a name here */
            if (is_current_kernel_context(f))
                console(" (kernel frame)");
            console("\n");
            goto exit_fault;
        }
    }
    if (is_current_kernel_context(f))
        f[FRAME_FULL] = false;      /* no longer saving frame for anything */
    runloop();
  exit_fault:
    console("cpu ");
    print_u64(ci->id);
    console(", state ");
    console(state_strings[ci->state]);
    console(", vector ");
    print_u64(i);
    console("\n");
    print_frame(f);
    print_stack(f);
    apic_ipi(TARGET_EXCLUSIVE_BROADCAST, 0, shutdown_vector);
    vm_exit(VM_EXIT_FAULT);
}

static id_heap interrupt_vector_heap;
static heap int_general;

u64 allocate_interrupt(void)
{
    u64 res = allocate_u64((heap)interrupt_vector_heap, 1);
    assert(res != INVALID_PHYSICAL);
    return res;
}

void deallocate_interrupt(u64 irq)
{
    deallocate_u64((heap)interrupt_vector_heap, irq, 1);
}

boolean reserve_interrupt(u64 irq)
{
    return id_heap_set_area(interrupt_vector_heap, irq, 1, true, true);
}

void register_interrupt(int vector, thunk t, const char *name)
{
    if (handlers[vector])
        halt("%s: handler for vector %d already registered (%p)\n",
             __func__, vector, handlers[vector]);
    handlers[vector] = t;
    interrupt_names[vector] = name;
}

void unregister_interrupt(int vector)
{
    if (!handlers[vector])
        halt("%s: no handler registered for vector %d\n", __func__, vector);
    handlers[vector] = 0;
    interrupt_names[vector] = 0;
}

closure_function(1, 0, void, shirq_handler,
                 list, handlers)
{
    list_foreach(bound(handlers), l) {
        inthandler h = struct_from_list(l, inthandler, l);
        int_debug("   invoking handler %s (%F)\n", h->name, h->t);
        apply(h->t);
    }
}

u64 allocate_shirq(void)
{
    u64 v = allocate_interrupt();
    list handlers = allocate(int_general, sizeof(struct list));
    assert(handlers != INVALID_ADDRESS);
    list_init(handlers);
    thunk t = closure(int_general, shirq_handler, handlers);
    assert(t != INVALID_ADDRESS);
    register_interrupt(v, t, "shirq");
    return v;
}

void register_shirq(int v, thunk t, const char *name)
{
    if (!handlers[v])
        halt("%s: vector %d not allocated\n", __func__, v);
    list shirq_handlers = closure_member(shirq_handler, handlers[v], handlers);
    inthandler handler = allocate(int_general, sizeof(struct inthandler));
    assert(handler != INVALID_ADDRESS);
    handler->t = t;
    handler->name = name;
    list_push_back(shirq_handlers, &handler->l);
}

#define TSS_SIZE                0x68

extern volatile void * TSS;
static inline void write_tss_u64(int cpu, int offset, u64 val)
{
    u64 * vec = (u64 *)(u64_from_pointer(&TSS) + (TSS_SIZE * cpu) + offset);
    *vec = val;
}

void set_ist(int cpu, int i, u64 sp)
{
    assert(i > 0 && i <= 7);
    write_tss_u64(cpu, 0x24 + (i - 1) * 8, sp);
}

void init_interrupts(kernel_heaps kh)
{
    heap general = heap_general(kh);
    cpuinfo ci = current_cpu();

    /* Read ACPI tables for MADT access */
    init_acpi_tables(kh);

    /* Exception handlers */
    handlers = allocate_zero(general, n_interrupt_vectors * sizeof(thunk));
    assert(handlers != INVALID_ADDRESS);
    interrupt_vector_heap = create_id_heap(general, general, INTERRUPT_VECTOR_START,
                                           n_interrupt_vectors - INTERRUPT_VECTOR_START, 1, false);
    assert(interrupt_vector_heap != INVALID_ADDRESS);

    int_general = general;

    /* Separate stack to keep exceptions in interrupt handlers from
       trashing the interrupt stack */
    set_ist(0, IST_EXCEPTION, u64_from_pointer(ci->m.exception_stack));

    /* External interrupts (> 31) */
    set_ist(0, IST_INTERRUPT, u64_from_pointer(ci->m.int_stack));

    /* IDT setup */
    heap backed = (heap)heap_page_backed(kh);
    idt = allocate(backed, backed->pagesize);
    assert(idt != INVALID_ADDRESS);

    /* Rely on ISTs in lieu of TSS stack switch. */
    u64 vector_base = u64_from_pointer(&interrupt_vectors);
    for (int i = 0; i < INTERRUPT_VECTOR_START; i++)
        write_idt(i, vector_base + i * interrupt_vector_size, IST_EXCEPTION);
    
    for (int i = INTERRUPT_VECTOR_START; i < n_interrupt_vectors; i++)
        write_idt(i, vector_base + i * interrupt_vector_size, IST_INTERRUPT);

    void *idt_desc = idt_from_interrupt(n_interrupt_vectors); /* placed after last entry */
    *(u16*)idt_desc = 2 * sizeof(u64) * n_interrupt_vectors - 1;
    *(u64*)(idt_desc + sizeof(u16)) = u64_from_pointer(idt);
    asm volatile("lidt %0": : "m"(*(u64*)idt_desc));

    u64 v = allocate_interrupt();
    assert(v != INVALID_PHYSICAL);
    spurious_int_vector = v;

    /* APIC initialization */
    init_apic(kh);

    /* GDT64 and TSS for boot cpu */
    install_gdt64_and_tss(0);
}

void triple_fault(void)
{
    disable_interrupts();
    /* zero table limit to induce triple fault */
    void *idt_desc = idt_from_interrupt(n_interrupt_vectors);
    *(u16*)idt_desc = 0;
    asm volatile("lidt %0; int3": : "m"(*(u64*)idt_desc));
    while (1);
}
