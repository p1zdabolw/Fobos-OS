#include "idt.h"
#include "../lib/mem.h"
#include "../lib/printf.h"

struct idt_entry { u16 off_lo; u16 sel; u8 ist; u8 type; u16 off_mid; u32 off_hi; u32 zero; } __attribute__((packed));
struct idt_ptr   { u16 limit; u64 base; } __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_ip;
static irq_handler_t    g_handlers[256];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);
extern void isr128(void);

static inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static void set_gate(int n, u64 handler) {
    g_idt[n].off_lo  = (u16)(handler & 0xFFFF);
    g_idt[n].sel     = 0x08;
    g_idt[n].ist     = 0;
    g_idt[n].type    = 0x8E;
    g_idt[n].off_mid = (u16)((handler >> 16) & 0xFFFF);
    g_idt[n].off_hi  = (u32)((handler >> 32) & 0xFFFFFFFF);
    g_idt[n].zero    = 0;
}

void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x00); outb(0xA1, 0x00);
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_handlers, 0, sizeof(g_handlers));

    void (*isrs[32])(void) = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,
        isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
    };
    void (*irqs[16])(void) = {
        irq0,irq1,irq2,irq3,irq4,irq5,irq6,irq7,irq8,irq9,irq10,irq11,irq12,irq13,irq14,irq15
    };
    for (int i = 0; i < 32; i++) set_gate(i, (u64)isrs[i]);
    for (int i = 0; i < 16; i++) set_gate(32 + i, (u64)irqs[i]);
    set_gate(128, (u64)isr128);

    g_ip.limit = sizeof(g_idt) - 1;
    g_ip.base  = (u64)&g_idt;
    __asm__ volatile("lidt %0" :: "m"(g_ip));
}

void irq_register(int irq, irq_handler_t h) {
    if (irq >= 0 && irq < 256) g_handlers[irq] = h;
}

void isr_dispatch(struct registers *r) {
    if (r->vector < 32) {
        u64 cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("exception %u err=%x\n", (u32)r->vector, (u32)r->error);
        kprintf("  rip=%p  cs=%x  rflags=%x\n",
                (void*)r->rip, (u32)r->cs, (u32)r->rflags);
        kprintf("  rax=%p  rbx=%p  rcx=%p  rdx=%p\n",
                (void*)r->rax, (void*)r->rbx, (void*)r->rcx, (void*)r->rdx);
        kprintf("  rsi=%p  rdi=%p  rbp=%p  cr2=%p\n",
                (void*)r->rsi, (void*)r->rdi, (void*)r->rbp, (void*)cr2);
        for (;;) __asm__ volatile("hlt");
    }
    if (r->vector == 128) {
        extern void syscall_dispatch(struct registers *);
        syscall_dispatch(r);
    } else if (r->vector >= 32 && r->vector < 48) {
        int irq = (int)r->vector - 32;
        if (g_handlers[irq]) g_handlers[irq](r);
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
}