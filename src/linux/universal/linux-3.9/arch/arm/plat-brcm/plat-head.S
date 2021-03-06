/*
 * Platform-specific assembly head
 * intended to perform whatever fixup is needed
 * adter the boot loader.
 * Also includes the secondary entry point for SMP.
 */


#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/memory.h>
#include <asm/mach-types.h>
#include <plat/plat-bcm5301x.h>

#ifndef CONFIG_DEBUG_LL
ENTRY(printch)
	mov	pc,lr
ENDPROC(printch)

ENTRY(printhex8)
	mov	pc,lr
ENDPROC(printhex8)
#endif

/*
 * This is called from head.S at the beginning.
 * If the boot monitor provides corect r1, r2 values
 * then these need to be preserved.
 *
 * On return, the requirements are: 
 * MMU = off, D-cache = off, I-cache = dont care, 
 * r0 = 0, r1 = machine nr, r2 = atags pointer, or 0.
 */
	__HEAD
ENTRY(__mach_head_fixup)
	mov	r12,lr		@ save return address
	@ Disable d-chace and MMU for UART to work for sure
	mrc	p15, 0, r0, c1, c0, 0		@ Read SCTLR
	bic	r0, #CR_C|CR_M
	mcr	p15, 0, r0, c1, c0, 0		@ Write SCTLR
	/* Clean up any residule in caches */
	bl	v7_all_dcache_invalidate
	mov	r0, #'.'
	bl	printch
	mov	r0,#0
	ldr	r3, =L2CC_BASE_PA	@ L2 cache controller, control reg
	str	r0, [r3, #0x100]	@ Disable L2 cache
	ldr	r0, =0xffff
	str	r0, [r3, #0x77c]	@ Invalidate by all Ways
	nop
	mov	r0, #'.'
	bl	printch
	/* fixup MaskROM LUT holding pen */
	ldr	r1, =SOC_ROM_BASE_PA
	ldr	r0, =0xffff002c
	str	r0, [r1, #SOC_ROM_LUT_OFF]
	nop

#ifndef	CONFIG_XIP_KERNEL
	/* Copy ourselves to RAM if loaded in wrong address */
	mov	r0, #0xff000000
	orr	r0, #0x00ff0000
	mov	r8, pc
	and	r8, r8, r0
	ldr	r9, =PHYS_OFFSET
	and	r9, r9, r0
	cmp	r8, r9
	beq	5f
	add	r9, # TEXT_OFFSET
	mov	r12, r9			@ start again in RAM
	ldr	r10, =__virt_to_phys(__bss_start)

	mov	r0, #'X'
	bl	printch
	mov	r0, r8
        bl      printhex8
	mov	r0, #':'
	bl	printch
	mov	r0, r9
        bl      printhex8
	mov	r0, #'-'
	bl	printch
	mov	r0, r10
        bl      printhex8
	nop

2:	
	ldmia	r8!, { r0 - r7 }
	stmia	r9!, { r0 - r7 }
	mov	r0, r9 
	mov	r1, #0xff
	orr	r1, r1, #0xff00
	tst	r0, r1
	bne	2b
	mov	r0, #'.'
	bl	printch
	cmp	r9, r10
	blo	2b

	mov	r0, #'!'
	bl	printch
	mov	r0, r9
        bl      printhex8
	nop
5:
	mov	r0, #'.'
	bl	printch
	nop
#endif	/* !CONFIG_XIP_KERNEL */

szmem:
	ldr	r3,=0x80000000
	ldr	r2,=0xaa55beef
	ldr	r1,=0x00000000
	ldr	r6,[r3,#0x3c]			/* Save the value in r6 */
	str	r1,[r3,#0x3c]
	ldr	r0,=(1 << 20)
	ldr	r5,=(1 << 30)
1:
	ldr	r4,=0x80000000
	add	r4,r4,r0
	ldr	r7,[r4,#0x3c]			/* Save the value in r7 */
	str	r2,[r4,#0x3c]
	ldr	r1,[r4,#0x3c]			/* Read back to ensure completion */
	ldr	r1,[r3,#0x3c]
	str	r7,[r4,#0x3c]			/* Restore the value from r7 */
	cmp	r1,r2
	beq	2f

	lsl	r0,r0,#1
	cmp	r0,r5
	bne	1b

	/* Didn't find an alias, must be 128MB */
2:
	str	r6,[r3,#0x3c]			/* Restore the value from r6 */
	adr	r7,__mach_head_fixup
	ldr	r8,=__mach_head_fixup
	sub	r7,r7,r8
	ldr	r8,=_memsize
	add	r7,r8,r7
	str	r0,[r7]

	@ Override machine type and atags pointers
	ldr	r1, =(machine_arch_type)
	mov	r0, #0
	mov	r2, r0
	mov	pc, r12		@ return
	nop
ENDPROC(__mach_head_fixup)
	.ltorg

        .global _memsize
        .type   _memsize, %object
        .size   _memsize, 4
        .align  2
_memsize:
	.word   0

#ifdef	CONFIG_SMP
/*
 * Platform specific entry point for secondary CPUs.  This
 * provides a "holding pen" into which all secondary cores are held
 * until we're ready for them to initialise.
 */
	__CPUINIT
ENTRY(platform_secondary_startup)
	/*
	 * Get hardware CPU id of ours
	 */

	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #15
	/*
	 * Wait on <pen_release> variable by physical address
	 * to contain our hardware CPU id
	 */
	adr	r4, 1f
	ldmia	r4, {r5, r6}
	sub	r4, r4, r5
	add	r6, r6, r4
pen:	ldr	r7, [r6]
	cmp	r7, r0
	bne	pen
	nop
	/*
	 * In case L1 cache has unpredictable contents at power-up
	 * clean its contents without flushing.
	 */
	bl	v7_l1_cache_invalidate
	nop
	/*
	 * we've been released from the holding pen: secondary_stack
	 * should now contain the SVC stack for this core
	 */
	b	secondary_startup

	.align
1:	.long 	.
	.long	pen_release

ENDPROC(platform_secondary_startup)
	.ltorg
#endif	/* CONFIG_SMP */
