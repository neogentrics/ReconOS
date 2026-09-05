#include <recon/kernel/cpu.h>
#include <recon/kernel/console.h>

static void yes_no(const char *label, bool value)
{
	kprintf("  %s%s\n", label, value ? "yes" : "no");
}

void cpu_print_caps(const struct cpu_caps *c)
{
	kprintf("\nProcessor\n");
	kprintf("  vendor       : %s\n", c->vendor);
	kprintf("  model        : %s\n", c->brand);

	if (c->cores || c->threads)
		kprintf("  topology     : %u cores, %u threads\n",
			c->cores, c->threads);
	else
		kputs("  topology     : not reported by the CPU\n");

	kprintf("  addressing   : %u physical bits, %u virtual\n",
		c->phys_addr_bits, c->virt_addr_bits);

	/* Printed on its own line because it is the one capability that changes
	 * how the kernel will map memory rather than merely how fast something
	 * runs, and checkpoint 4 branches on it. */
	kprintf("  large pages  : 2MB %s, 1GB %s\n",
		c->page_2m ? "yes" : "no", c->page_1g ? "yes" : "no");

	yes_no("no-execute   : ", c->no_execute);
	yes_no("hw random    : ", c->hw_random);
	yes_no("fixed timer  : ", c->invariant_timer);

	kprintf("  extensions   : %s\n",
		c->extensions[0] ? c->extensions : "none reported");
}
