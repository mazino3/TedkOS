/* kernel.c - the C part of the kernel
 * vim:ts=4 expandtab
 */
#include <stddef.h>
#include <stdint.h>

#include <inc/multiboot.h>
#include <inc/x86/desc_interrupts.h>
#include <inc/x86/desc.h>
#include <inc/klibs/lib.h>
#include <inc/x86/idt_init.h>
#include <inc/i8259.h>
#include <inc/debug.h>
#include <inc/known_drivers.h>
#include <inc/x86/paging.h>
#include <inc/fs/kiss_wrapper.h>
#include <inc/mbi_info.h>
#include <inc/klibs/palloc.h>
#include <inc/proc/tasks.h>
#include <inc/proc/sched.h>
#include <inc/x86/desc.h>
#include <inc/x86/stacker.h>
#include <inc/init.h>
#include <inc/klibs/AutoSpinLock.h>
#include <inc/x86/real.h>
#include <inc/ui/vbe.h>
#include <inc/klibs/stack.h>

using namespace palloc;
using arch::Stacker;
using arch::CPUArchTypes::x86;
using namespace vbe;

/* Initialize runtime library */
extern "C" void _init(void);

// Make sure usable_mem has at least 12KB memory (later it will be 5MB memory.)
// It uses the two aligned arrays declared below.
void kernel_enable_basic_paging();

uint32_t basicPageDir[1024] __attribute__((aligned (4096)));
uint32_t basicPageTable0[1024] __attribute__((aligned (4096)));

/* Check if MAGIC is valid and print the Multiboot information structure
   pointed by ADDR. */
    void
_entry (unsigned long magic, unsigned long addr)
{
    /* First thing we do is to assign the global MBI */
    MultiBootInfoAddress = (multiboot_info_t *) addr;

    /* Clear the screen. */
    clear();

    printf ("mbi = 0x%#x\n", (uint32_t) addr);

    /* Print boot info */
    //mbi_info(magic, addr);

    /* Construct an LDT entry in the GDT */
    {
        seg_desc_t the_ldt_desc;
        the_ldt_desc.granularity    = 0;
        the_ldt_desc.opsize         = 1;
        the_ldt_desc.reserved       = 0;
        the_ldt_desc.avail          = 0;
        the_ldt_desc.present        = 1;
        the_ldt_desc.dpl            = 0x0;
        the_ldt_desc.sys            = 0;
        the_ldt_desc.type           = 0x2;

        SET_LDT_PARAMS(the_ldt_desc, &ldt, ldt_size);
        ldt_desc = the_ldt_desc;
        lldt(KERNEL_LDT_SEL);
    }

    /* Construct a TSS entry in the GDT */
    {
        seg_desc_t the_tss_desc;
        the_tss_desc.granularity    = 0;
        the_tss_desc.opsize         = 0;
        the_tss_desc.reserved       = 0;
        the_tss_desc.avail          = 0;
        the_tss_desc.seg_lim_19_16  = TSS_SIZE & 0x000F0000;
        the_tss_desc.present        = 1;
        the_tss_desc.dpl            = 0x0;
        the_tss_desc.sys            = 0;
        the_tss_desc.type           = 0x9;
        the_tss_desc.seg_lim_15_00  = TSS_SIZE & 0x0000FFFF;

        SET_TSS_PARAMS(the_tss_desc, &tss, tss_size);

        tss_desc = the_tss_desc;

        tss.ldt_segment_selector = KERNEL_LDT_SEL;
        tss.ss0 = KERNEL_DS_SEL;
        tss.esp0 = 0x800000;

        // should load kernel ds,es,fs,gs so that at interrupt we have correct data access
        tss.ds = tss.es = tss.fs = tss.gs = KERNEL_DS_SEL;
        ltr(KERNEL_TSS_SEL);
    }

    /* Paging */
    kernel_enable_basic_paging();

    /* Init the PIC */
    i8259_init();

    /* Init the interrupts */
    init_idt();

    /* Initialize runtime library */
    _init();

    /* Initialize file system */
    filesystem::Dispatcher::init();

    /* Initialize devices, memory, filesystem, enable device interrupts on the
     * PIC, any other initialization stuff... */

    for(size_t i = 0; i < num_known_drivers; i++)
    {
        printf("Loading driver '%s' ...", known_drivers[i].name);
        known_drivers[i].init();
        printf(" ... OK!\n");
    }

    // Call this only after C++ intialization has been completed.
    prepareRealMode();

    // ----- START init as a KERNEL thread (because its code is in kernel code) -----

    // should have loaded flags using cli_and_save or pushfl
    uint32_t flags = 0;
    int32_t child_upid = newPausedProcess(-1);

    if(child_upid < 0)
    {
        printf("Weird Error: Out of PIDs\n");
        asm volatile("1: hlt; jmp 1b;");
    }

    ProcessDesc& proc = ProcessDesc::get(child_upid);

    // Here we do NOT use any more memory than PCB & kstack.
    // Because no stack exchange happens for kthread during interrupts.

    // Initialize stack and ESP
    // compatible with x86 32-bit iretl. KTHREAD mode.
    // always no error code on stack before iretl
    Stacker<x86> kstack((uint32_t)proc.mainThreadInfo->kstack + THREAD_KSTACK_SIZE - 1);

    // EFLAGS: Clear V8086 , Clear Trap, Clear Nested Tasks.
    // Set Interrupt Enable Flag. IOPL = 3
    kstack << ((flags & (~0x24100)) | 0x3200);

    kstack << (uint32_t) KERNEL_CS_SEL;
    kstack << (uint32_t) init_main;

    pushal_t regs;
    regs.esp = (uint32_t) kstack.getESP();
    regs.ebp = 0;
    regs.eax = -1;
    regs.ebx = regs.ecx = regs.edx = 0;
    regs.edi = regs.esi = 0;

    kstack << regs;

    proc.mainThreadInfo->pcb.esp0 = (target_esp0)kstack.getESP();
    proc.mainThreadInfo->pcb.isKernelThread = 1;

    Maybe<uint32_t> vmemBase = virtOfPage0();
    char* vmemPage = NULL;
    if(vmemBase)
        vmemPage = (char*)(!vmemBase);
    else
    {
        printf("Fail to allocate virtual mem space for VMEM\n");
        asm volatile("1: hlt; jmp 1b;");
    }
    video_mem = vmemPage + PRE_INIT_VIDEO;

    cpu0_memmap.start();
    cpu0_memmap.loadProcessMap(proc.memmap);


    // refresh TSS so that later interrupts use this new kstack
    tss.esp0 = (uint32_t)kstack.getESP();
    // ltr(KERNEL_TSS_SEL);     WILL CAUSE GENERAL PROTECTION ERROR

    asm volatile (
        "movl %0, %%esp         ;\n"
        "popal                  ;\n"
        "iretl                  ;\n"
        : : "rm" (kstack.getESP()) : "cc");
    // This asm block changes everything but gcc should not worry about them.

    // This part should never be reached.
    printf("Halted.\n");
    asm volatile("1: hlt; jmp 1b;");

}

void kernel_enable_basic_paging()
{
    AutoSpinLock lock(&cpu0_paging_lock);
    int32_t i;
    uint32_t* pageDir   = basicPageDir;
    uint32_t* pageTable = basicPageTable0;
    memset(pageDir  , 0, 0x1000);
    memset(pageTable, 0, 0x1000);
    REDIRECT_PAGE_DIR(pageDir);
    LOAD_4MB_PAGE(1, 1 << 22, PG_WRITABLE);
    LOAD_PAGE_TABLE(0, pageTable, PT_WRITABLE);

    // IMPORTANT!!! Must start from i = 1. NOT i = 0 !!!!!
    for(i = 1; i < 0x400; i++)
    {
        LOAD_4KB_PAGE(0, i, i << 12, PG_WRITABLE);
    }
    enable_paging();
}

extern "C" void __attribute__((externally_visible))
entry (unsigned long magic, unsigned long addr)
{
    _entry(magic, addr);
}
