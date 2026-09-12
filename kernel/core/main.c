/* kmain -- the first portable code that runs.
 *
 * Everything above this point was written for one machine. Everything below
 * it is written once. The whole point of the arch/ split is that this file
 * looks identical no matter what booted it.
 */
#include <recon/kernel/arch.h>
#include <recon/kernel/acpi.h>
#include <recon/kernel/block.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/elf.h>
#include <recon/kernel/crc32.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/durability.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/partition.h>
#include <recon/kernel/fat32.h>
#include <recon/kernel/install.h>
#include <recon/kernel/reconfs.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/random.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/smbios.h>
#include <recon/kernel/power.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/process.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/hpet.h>
#include <recon/kernel/i2c.h>
#include <recon/kernel/signal.h>
#include <recon/kernel/ext2.h>
#include <recon/kernel/shm.h>
#include <recon/kernel/swap.h>
#include <recon/kernel/pageage.h>
#include <recon/kernel/evict.h>
#include <recon/kernel/bcache.h>
#include <recon/kernel/pagecache.h>
#include <recon/kernel/irq.h>
#include <recon/kernel/input.h>
#include <recon/kernel/identity.h>
#include <recon/kernel/xhci.h>	/* the USB keyboard's decoder test */
#include <recon/kernel/backtrace.h>
#include <recon/kernel/klog.h>
#include <recon/kernel/aml.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/work.h>
#include <recon/kernel/trap.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>

#ifndef RECONOS_KERNEL_VERSION
#define RECONOS_KERNEL_VERSION "0.0.0"
#endif

static struct cpu_caps cpu;

static void banner(void)
{
	kputs("\n");
	kputs("ReconOS kernel " RECONOS_KERNEL_VERSION "\n");
	kprintf("  architecture : %s\n", arch_name());
}

/* Was `poweroff` asked for?
 *
 * The parser this carried now lives in boot.c. The comment here said *two call
 * sites is not yet a reason to share one*, which was a fair call at two and
 * stopped being one at three -- the volume has to know whether this is a
 * recovery boot. */
static bool asked_for_poweroff(void)
{
	return boot_cmdline_has("poweroff");
}

void kmain(void)
{
	arch_early_init();
	banner();

	arch_cpu_caps(&cpu);
	cpu_print_caps(&cpu);

	boot_print_summary();

	pmm_init();
	pmm_print_summary();

	vm_init();
	vm_print_summary();
	addrspace_init();

	/* The screen, as soon as there is a direct map to reach it through.
	 *
	 * Everything above this line went to the serial port only, which is
	 * unavoidable -- the framebuffer is not addressable until the map
	 * exists. Everything below goes to both. */
	fbcon_init();
	fbcon_describe();

	heap_init();
	heap_print_summary();

	/* After the direct map, because the tables are read through it, and
	 * before anything that wants to know what hardware exists. */
	acpi_init(boot_info()->acpi_rsdp);

	/* After the heap, because a fault report is more useful than a fault,
	 * and before anything that might fault. */
	trap_init();

	/* After the fault handlers, because enabling interrupts without
	 * somewhere for them to go is how a machine resets while telling you
	 * nothing. */
	/* The scheduler before the timer, so that the first tick has something
	 * to tick. Started here rather than earlier because it allocates. */
	/* The vector unit, before the scheduler makes the first thread -- a
	 * thread's starting register image is captured from this processor, and
	 * capturing it from a unit that is still disabled would fault. */
	arch_vector_enable();

	process_init();
	sched_init();

	time_init();
	time_print_summary();
	timer_print_summary();
	work_print_summary();

	/* After the clock, because one of its sources is timing jitter and
	 * there is nothing to measure without one. Before anything that might
	 * want a key. */
	random_init();

	/* After the timer, because a processor with no tick cannot be preempted
	 * and the test for that has to have a clock to wait on. */
	smp_init();

	/* The worker thread, after the scheduler and after there is somewhere
	 * for it to sleep. Everything that wants to hand work off to a thread
	 * -- a timer callback, an interrupt handler -- needs this to exist
	 * before it runs. */
	work_init_queue();

	/* And the reaper that hands work to it. After the worker
	 * exists, and before anything is allowed to finish. */
	sched_reaper_init();

	/* And now device interrupts can be pointed somewhere. After the
	 * processors, because the destination is one of them. */
	arch_irq_route_init();
	smp_print_summary();
	arch_irq_print_summary();

	random_print_summary();


	/* Last, because it is the one thing that needs everything: pages to map,
	 * page tables that can express "user may reach this", a fault handler to
	 * catch the program when it is wrong, a thread to run it in, and a timer
	 * to take it back off the processor. */
	user_init();

	/* Last of all, because a driver needs everything: pages for its rings,
	 * a map to reach registers through, a clock to time out against, and a
	 * scheduler to yield to while the hardware thinks. */
	/* After ACPI, which is where the table is, and before anything asks
	 * the time for a measurement it will keep. */
	hpet_init();

	block_init();

	/* **After** block_init, not before it, and this is the second time
	 * that ordering has caught something here. block_init is what walks
	 * the PCI bus; a driver looking for its device before that walk finds
	 * an empty table and reports, correctly and uselessly, that the
	 * machine does not have one. BG-179 was the same mistake with the
	 * interrupt summary, which printed a count of zero on every boot this
	 * kernel had ever made. */
	i2c_init();
	bcache_init();
	pagecache_init();

	/* After the interrupt routing, because the keyboard asks for a
	 * line -- and it is the first thing in this kernel that ever has. */
	input_init();
	block_print_summary();

	/* What the devices can do about interrupts, printed here rather than
	 * beside the processors.
	 *
	 * It used to be part of arch_irq_print_summary, which runs before
	 * block_init -- so it counted the PCI devices that can signal by memory
	 * write at a point where the PCI bus had not been walked and there were
	 * none. It reported zero on every boot this kernel has ever made, and
	 * the zero was believed. Facts about devices are printed after the
	 * devices exist. */
	arch_irq_print_device_summary();
	virtio_blk_print_summary();
	acpi_print_summary();

	/* What the machine says it is, as opposed to what its processor is. */
	smbios_init();
	smbios_print_summary();

	/* The machine's own description of itself, in bytecode. After the fixed
	 * tables, because the FADT is what says where it is. */
	aml_init();
	aml_print_summary();

	/* The machine-readable version of the same thing, for the fixture
	 * harness to compare against what sgdisk and sfdisk say is on the same
	 * disk. Separate from the summary above on purpose: a format that has
	 * to be both readable and parseable ends up being neither, and the one
	 * that gets quietly reformatted is the one under test. */
	block_print_tables();

	/* After the partitions, not merely after the devices. A ReconFS volume
	 * lives in a slice, so mounting before the table is read would find
	 * nothing on every machine that has one -- and report "no filesystem",
	 * which is the same sentence a machine that genuinely has none prints. */
	rootfs_init();

	/* Which device is swap comes from the command line until the installer
	 * marks a partition for it -- the same shape as `reconfs=` and
	 * `durability=`, and for the same reason: this writes over a whole
	 * device from the first eviction, and picking one by looking at it is
	 * how a machine destroys its own system volume.
	 *
	 * Here, before the self-tests, and not with the other late setup. The
	 * first version of this ran after them, so the store attached and the
	 * test that was meant to exercise it had already reported a pass for
	 * having found nothing to test. A skip that reads as a pass is the
	 * thing this project keeps having to catch. */
	swap_init_from_cmdline();
	rootfs_print_summary();

	/* Run at boot rather than in a test harness, because there is no test
	 * harness that can run a kernel yet, and an allocator that is quietly
	 * wrong is the kind of fault that surfaces three checkpoints later as
	 * something else's bug. */
	kputs("\nSelf-tests\n");
	kprintf("  physical allocator : %s\n",
		pmm_self_test() ? "pass" : "FAIL");
	kprintf("  virtual memory     : %s\n",
		vm_self_test() ? "pass" : "FAIL");
	kprintf("  kernel heap        : %s\n",
		heap_self_test() ? "pass" : "FAIL");
	kprintf("  fault handling     : %s\n",
		trap_self_test() ? "pass" : "FAIL");
	kprintf("  clock and tick     : %s\n",
		time_self_test() ? "pass" : "FAIL");
	kprintf("  threads            : %s\n",
		sched_self_test() ? "pass" : "FAIL");
	kprintf("  locking            : %s\n",
		lock_self_test() ? "pass" : "FAIL");
	kprintf("  allocating at once : %s\n",
		pmm_concurrent_test() ? "pass" : "FAIL");
	kprintf("  waiting and waking : %s\n",
		wait_self_test() ? "pass" : "FAIL");
	kprintf("  processes          : %s\n",
		process_self_test() ? "pass" : "FAIL");
	kprintf("  open files         : %s\n",
		vfs_self_test() ? "pass" : "FAIL");
	kprintf("  a pipe between two : %s\n",
		pipe_self_test() ? "pass" : "FAIL");
	kprintf("  devices as files   : %s\n",
		devfs_self_test() ? "pass" : "FAIL");
	kprintf("  what the kernel knows : %s\n",
		procfs_self_test() ? "pass" : "FAIL");
	kprintf("  a clock off the core : %s\n",
		hpet_self_test() ? "pass" : "FAIL");
	kprintf("  the small buses    : %s\n",
		i2c_self_test() ? "pass" : "FAIL");
	kprintf("  something arrives : %s\n",
		user_signal_test() ? "pass" : "FAIL");
	kprintf("  somebody else's disk : %s\n",
		ext2_self_test() ? "pass" : "FAIL");
	kprintf("  memory two can see : %s\n",
		shm_self_test() ? "pass" : "FAIL");
	kprintf("  files in memory    : %s\n",
		ramfs_self_test() ? "pass" : "FAIL");
	kprintf("  somewhere to evict : %s\n",
		swap_self_test() ? "pass" : "FAIL");
	kprintf("  which pages are cold : %s\n",
		page_age_self_test() ? "pass" : "FAIL");
	kprintf("  a page that came back : %s\n",
		evict_self_test() ? "pass" : "FAIL");
	kprintf("  a line somebody wants : %s\n",
		irq_self_test() ? "pass" : "FAIL");
	kprintf("  who may, and who may not : %s\n",
		identity_self_test() ? "pass" : "FAIL");
	kprintf("  a report is a state  : %s\n",
		usb_hid_self_test() ? "pass" : "FAIL");
	kprintf("  somebody typing      : %s\n",
		input_self_test() ? "pass" : "FAIL");
	kprintf("  blocks kept nearby   : %s\n",
		bcache_self_test() ? "pass" : "FAIL");
	kprintf("  what was said, kept : %s\n",
		klog_self_test() ? "pass" : "FAIL");
	kprintf("  how it got there   : %s\n",
		backtrace_self_test() ? "pass" : "FAIL");
	kprintf("  nothing is kept for nobody : %s\n",
		process_reaping_self_test() ? "pass" : "FAIL");
	kprintf("  address spaces     : %s\n",
		addrspace_self_test() ? "pass" : "FAIL");
	kprintf("  refusing a bad program : %s\n",
		elf_self_test() ? "pass" : "FAIL");
	kprintf("  processors         : %s\n",
		smp_self_test() ? "pass" : "FAIL");
	kprintf("  telling them apart : %s\n",
		arch_identity_self_test() ? "pass" : "FAIL");
	kprintf("  something later    : %s\n",
		timer_self_test() ? "pass" : "FAIL");
	kprintf("  work put off       : %s\n",
		work_self_test() ? "pass" : "FAIL");
	kprintf("  an interrupt with no wire : %s\n",
		arch_irq_self_test() ? "pass" : "FAIL");
	kprintf("  user mode          : %s\n",
		user_self_test() ? "pass" : "FAIL");
	kprintf("  the boundary holds : %s\n",
		user_boundary_test() ? "pass" : "FAIL");
	kprintf("  a program from a file : %s\n",
		user_elf_test() ? "pass" : "FAIL");
	kprintf("  machine facts      : %s\n",
		user_facts_test() ? "pass" : "FAIL");
	kprintf("  block devices      : %s\n",
		block_self_test() ? "pass" : "FAIL");
	kprintf("  requests in order  : %s\n",
		block_queue_test() ? "pass" : "FAIL");
	kprintf("  a disk that speaks up : %s\n",
		virtio_blk_self_test() ? "pass" : "FAIL");
	kprintf("  what is in there   : %s\n",
		vfs_list_self_test() ? "pass" : "FAIL");
	kprintf("  one copy, shared   : %s\n",
		pagecache_self_test() ? "pass" : "FAIL");
	kprintf("  room is made       : %s\n",
		pagecache_eviction_self_test() ? "pass" : "FAIL");
	kprintf("  a clean handoff    : %s\n",
		boot_handoff_registers_clear(0, 0) ? "pass" : "FAIL");
	kprintf("  checksums          : %s\n",
		crc32_self_test() ? "pass" : "FAIL");
	kprintf("  randomness         : %s\n",
		random_self_test() ? "pass" : "FAIL");
	kprintf("  partitions         : %s\n",
		partition_self_test() ? "pass" : "FAIL");
	kprintf("  filesystem layout  : %s\n",
		reconfs_layout_self_test() ? "pass" : "FAIL");

	sched_print_summary();

	/* Printed here rather than beside the memory summary, because the
	 * interesting invalidations happen *after* it: the user-mode tests map
	 * three programs at the same address in turn, which is exactly the
	 * replacement case. Reporting it earlier counted only the ones that
	 * happened before there was a second processor to tell. */
	vm_print_shootdowns();

	/* After the user-mode tests, not before: the mappings this is asking
	 * about are made when a program is loaded, and a report taken earlier
	 * describes a machine that has never run one. The shootdown counters
	 * were read too early once for exactly this reason. */
	vm_fault_print_summary();
	vm_user_half_report();
	addrspace_print_summary();
	user_print_summary();
	process_print_summary();
	vfs_print_summary();
	pipe_print_summary();
	devfs_print_summary();
	procfs_print_summary();
	hpet_print_summary();
	i2c_print_summary();
	signal_print_summary();
	ext2_print_summary();
	shm_print_summary();
	ramfs_print_summary();

	/* Here and not beside the block devices, because the store is attached
	 * after they are found -- printing it there reported "none" on a machine
	 * that had one, which is a summary describing the moment it was written
	 * rather than the machine. */
	swap_print_summary();
	page_age_print_summary();
	evict_print_summary();
	block_print_traffic();
	bcache_print_summary();
	pagecache_print_summary();
	input_print_summary();
	irq_print_summary();
	lock_print_summary();
	klog_print_summary();

	/* Only when asked for on the command line, because it writes to every
	 * block it touches. See core/durability.c: it exists to find out whether
	 * a flush on this kernel actually orders writes, which is the property
	 * every crash-consistency scheme a filesystem could use rests on. */
	durability_run();

	/* Same gating, and for a stronger reason: this one formats. It runs only
	 * when `reconfs=<device>` names a device, and refuses one that carries a
	 * partition table -- a self-test that writes a filesystem over somebody's
	 * disk is the most destructive thing this kernel could do by accident. */
	reconfs_run();

	/* After the battery, and this is the only place it can go.
	 *
	 * The test needs a ReconFS volume. On a real machine there is one and it
	 * was mounted at boot; in the verification rig every disk is blank, and
	 * the only volume that ever exists is the one the battery above just
	 * formatted. Running this in the self-test list would therefore report
	 * "no filesystem to test against" on every machine in the rig -- a
	 * truthful sentence, and a test that never once ran. */
	rootfs_run();

	/* After rootfs_run, because it needs a file to be refused and
	 * there is no filesystem until then. */
	identity_run();

	/* And the page cache against a file on that volume, for the same reason
	 * identity_run is here: the volume does not exist when the self-tests
	 * run. */
	addrspace_run();
	pagecache_run();

	/* After it, not before: the counts are zero until something has been
	 * checked, and a summary printed first reports a permission system
	 * that has never been consulted -- which is what this row of the
	 * audit said for months and is exactly the wrong thing to keep
	 * printing once it is no longer true. */
	identity_print_summary();

	reconfs_crash_run();
	fat32_run();
	fat32_write_run();
	recovery_run();
	install_plan_run();
	install_execute_run();

	/* Asked for on the command line, and last, because it does not return.
	 *
	 * It exists to be *tested*: an emulator told to power off exits, and its
	 * exit is something a script can assert on. A shutdown path exercised only
	 * by a person pressing a button is one that rots between the times anybody
	 * presses it. */
	if (asked_for_poweroff())
		power_off_or_say_why();

	kputs("\nNothing else is implemented yet. Idling.\n");

	/* The idle loop, and the first thing in the kernel that has to be right
	 * about the project's central claim: it sleeps until hardware wakes it,
	 * rather than spinning.
	 *
	 * As of checkpoint 8 there is something to wake it: the tick, a hundred
	 * times a second. That is the simple correct thing and it is also, on an
	 * idle machine, a hundred wakeups a second doing nothing -- which is how
	 * a laptop runs warm with nothing running. Making the tick stop when
	 * there is nothing to wake for belongs with the scheduler, and is
	 * recorded in docs/KERNEL.md rather than left to be noticed. */
	for (;;)
		arch_wait_for_interrupt();
}
