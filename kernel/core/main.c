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
#include <recon/kernel/net.h>
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
#include <recon/kernel/logport.h>
#include <recon/kernel/aml.h>
#include <recon/kernel/aml_eval.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/display.h>
#include <recon/kernel/fbdev.h>
#include <recon/kernel/amd_display.h>
#include <recon/kernel/intel_display.h>
#include <recon/kernel/virtio_gpu.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/suspend.h>
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

/* And `restart`, for the same reason and with the same argument.
 *
 * `power_restart` is reached from user mode through SYS_POWER, which is where
 * it is meant to be used from -- and a path whose only caller is a system call
 * no test can invoke without ending the guest is a path nobody has run. This
 * is how it gets run: an emulator told to restart, with -no-reboot, exits, and
 * its exit is something a script can assert on. */
static bool asked_for_restart(void)
{
	return boot_cmdline_has("restart");
}

void kmain(void)
{
	arch_early_init();
	banner();

	arch_cpu_caps(&cpu);
	cpu_print_caps(&cpu);

	boot_print_summary();

	/* **Before vm_init**, and that is the whole of why it is here rather
	 * than beside time_init.
	 *
	 * The loader handed over on the firmware's page tables, and the kernel
	 * is still running on them. Firmware runtime code is mapped where
	 * firmware expects it exactly until vm_init replaces those tables, so
	 * this is the one window where the date can be had for the cost of a
	 * call. Silent on a machine with no UEFI. */
	if (time_capture_firmware_clock())
		kputs("  firmware     : it knows what the date is, and said so\n");

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

	/* **After block_init, because that is what walks the PCI bus**, and the
	 * display adapter is on it. Before this line the console is whatever
	 * firmware left -- on the PVH and direct-kernel paths, nothing at all.
	 * This is where a machine with an adapter and no framebuffer gets one. */
	display_init();

	/* Before the partition scan would want it, and after there is a direct
	 * map to reach the image through. */
	initrd_init();

	/* **After** block_init, not before it, and this is the second time
	 * that ordering has caught something here. block_init is what walks
	 * the PCI bus; a driver looking for its device before that walk finds
	 * an empty table and reports, correctly and uselessly, that the
	 * machine does not have one. KF-179 was the same mistake with the
	 * interrupt summary, which printed a count of zero on every boot this
	 * kernel had ever made. */
	i2c_init();
	bcache_init();
	pagecache_init();

	/* After the interrupt routing, because the keyboard asks for a
	 * line -- and it is the first thing in this kernel that ever has. */
	input_init();
	block_print_summary();
	display_print_summary();
	intel_display_print_summary();
	amd_display_print_summary();
	virtio_gpu_print_summary();
	suspend_print_summary();

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

	/* Every zero-argument method run, and the outcomes counted.
	 *
	 * A measurement rather than a feature: it says how much of *this*
	 * machine's description is within reach of the evaluator, which is the
	 * distance to reading a trackpad's `_CRS` and is different on every
	 * machine. Safe anywhere -- everything that could affect the machine is
	 * refused before it happens, so trying all of them cannot change one. */
	aml_eval_survey();
	aml_eval_print_summary();

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

	/* That each /dev/fbN names the display at its own position.
	 *
	 * Not that fb1 opens -- a kernel where it is an alias for fb0 opens it,
	 * reads the same pixels and passes anything that only asks whether the
	 * device is there, which is what a half-finished version of this looks
	 * like. The matrix gives QEMU two adapters in one machine so the
	 * assertion has something to be about. */
	kprintf("  a screen each      : %s\n",
		fbdev_nodes_self_test() ? "pass" : "FAIL");
	kprintf("  what the kernel knows : %s\n",
		procfs_self_test() ? "pass" : "FAIL");
	kprintf("  a clock off the core : %s\n",
		hpet_self_test() ? "pass" : "FAIL");
	kprintf("  the small buses    : %s\n",
		i2c_self_test() ? "pass" : "FAIL");
	kprintf("  a mode of our own  : %s\n",
		display_self_test() ? "pass" : "FAIL");

	/* **What the one above cannot ask.** `display_self_test` sets modes and
	 * reads them back, which proves a mode was established and says nothing
	 * about whether anything reached the glass -- on a display whose pixels
	 * are guest memory, reading back what was written is reading back what
	 * was written, and it passes against a black screen (GX-003). This one
	 * asks the device instead. */
	kprintf("  a present that lands : %s\n",
		virtio_gpu_self_test() ? "pass" : "FAIL");

	/* The recognition table for real graphics hardware, checked on machines
	 * that have none -- which is every machine in this matrix. What it
	 * refuses matters more than what it claims: the ids it must decline are
	 * the host bridge sitting beside the graphics in the same package, and
	 * Intel display engines of generations this kernel has no code for. */
	kprintf("  graphics it knows  : %s\n",
		intel_display_self_test() ? "pass" : "FAIL");

	/* And the other real-hardware table. Its refusals include two identifier
	 * collisions that exist in pci.ids today -- 164e is AMD's Raphael and
	 * Broadcom's NetXtreme II, and 164e is the graphics in this project's own
	 * desktop -- plus the USB controllers AMD puts on its graphics cards. */
	kprintf("  the graphics cards it knows : %s\n",
		amd_display_self_test() ? "pass" : "FAIL");

	/* The arithmetic a Gen9 modeset rests on, checked on machines that have
	 * no Gen9 -- which is all of them. Every field in that engine stores one
	 * less than the number it describes, and a pipe told it is 1921 pixels
	 * wide accepts it: nothing but a known answer catches that. It also
	 * asserts that the two register blocks have not been confused, which is
	 * the mistake that nearly shipped. */
	kprintf("  a mode in numbers  : %s\n",
		intel_modeset_self_test() ? "pass" : "FAIL");

	/* After the sweep above, which sets seven modes in turn and moves the
	 * framebuffer each time. A program's mapping is of one physical address;
	 * taking it before the last mode change would be a mapping of wherever
	 * the pixels used to be. */
	kprintf("  a screen to draw on : %s\n",
		user_framebuffer_test() ? "pass" : "FAIL");
	kprintf("  a tick that stops  : %s\n",
		power_idle_self_test() ? "pass" : "FAIL");
	kprintf("  stopping, and starting again : %s\n",
		suspend_self_test() ? "pass" : "FAIL");
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
	kprintf("  asking to stop     : %s\n",
		user_power_test() ? "pass" : "FAIL");
	kprintf("  the format is kept : %s\n",
		console_format_self_test() ? "pass" : "FAIL");
	kprintf("  running a method   : %s\n",
		aml_eval_self_test() ? "pass" : "FAIL");
	kprintf("  a program from a file : %s\n",
		user_elf_test() ? "pass" : "FAIL");

	kprintf("  a program written in C : %s\n",
		user_c_program_test() ? "pass" : "FAIL");
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
	kprintf("  the network stack  : %s\n",
		net_self_test() ? "pass" : "FAIL");
	kprintf("  sockets a program can reach : %s\n",
		socket_syscall_test() ? "pass" : "FAIL");
	kprintf("  a socket held by a program   : %s\n",
		user_socket_probe_test() ? "pass" : "FAIL");

	/* From here to the kernel-log summary is per-subsystem detail: lock
	 * contention, cache hit rates, the timer wheel's reach. It still goes
	 * to the serial port and into the ring exactly as before -- the rig
	 * reads all of it, and so does the boot-log file on the medium -- and
	 * the panel is spared it unless `verbose` is asked for.
	 *
	 * **Everything above this line still reaches the panel.** That is where
	 * drivers say what they found and what refused them, and it is
	 * deliberate: the lines that found KF-219 and KF-223 were printed by
	 * drivers starting, not by summaries. A quiet mode that could hide
	 * either would have cost more than it saved. */
	if (!boot_cmdline_has("verbose"))
		console_screen_quiet(true);

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
	/* Ports are read once during the storage probe; from here they are read
	 * every half second, so a stick pushed in after boot is noticed. */
	usb_hotplug_start();

	net_bring_up();
	usb_print_summary();
	net_print_summary();
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

	/* Back on for everything that follows: the self-tests, the boot log's
	 * own line, and whatever a program draws afterwards. */
	console_screen_quiet(false);

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

	/* Everything said so far, onto the medium it was said on.
	 *
	 * **Last, and after every other summary**, so the file holds the whole
	 * report rather than the part printed before it. On a machine with a
	 * serial port this is redundant; on a laptop it is the only way the
	 * report survives being looked at, and every diagnosis on real hardware
	 * so far has come from photographing a panel and losing what scrolled.
	 *
	 * Before power-off, because that does not return. */
	klog_save_to_medium();

	/* And over the network, for the machine where the line above cannot
	 * work.
	 *
	 * `klog_save_to_medium` writes to the medium the kernel booted from, and
	 * on the Gateway that medium is a USB stick whose driver is the thing
	 * being diagnosed (KF-256). A machine whose fault prevents recording the
	 * evidence of that fault is a machine diagnosed by photographing a
	 * panel, which this project has now done four times.
	 *
	 * **The network does not depend on the disk**, which is the entire
	 * argument. Off unless the command line says `logport`, and it prints
	 * nothing at all when nobody asked -- see logport.h for why it reads
	 * from nobody and why it is not SSH.
	 *
	 * Started after the log is saved rather than before, so the two are not
	 * competing for the same report, and after the summaries so that what a
	 * reader gets is the whole boot rather than the part printed by the time
	 * a socket opened. */
	logport_start();
	logport_print_summary();

	/* Asked for on the command line, and last, because it does not return.
	 *
	 * It exists to be *tested*: an emulator told to power off exits, and its
	 * exit is something a script can assert on. A shutdown path exercised only
	 * by a person pressing a button is one that rots between the times anybody
	 * presses it. */
	if (asked_for_poweroff())
		power_off_or_say_why();

	if (asked_for_restart())
		power_restart_or_say_why();

	kputs("\nThe kernel has nothing else to do.\n");

	/* And the first thing on this machine that is not a self-test.
	 *
	 * It does not end: it draws what somebody sees and stays up.
	 * Everything above this line proves the machine works; this is the
	 * machine working.
	 *
	 * **After the last thing the kernel prints**, and that ordering is the
	 * whole of what makes the screen readable: the console and a program
	 * both draw on the same framebuffer, so whichever goes last is what
	 * anybody sees. The first boot had this before the final two lines and
	 * the console painted straight across it.
	 *
	 * That is not a fix, it is an ordering. The fix is the entry in
	 * docs/KERNEL-WANTS.md about the console and a program both owning the
	 * screen, and it needs the kernel to be able to give the screen away.
	 *
	 * Last also means the boot report is already saved, so a fault in here
	 * cannot take one with it. */
	kputs("Idling.\n");

	/*
	 * **Nothing is printed after this line, and that is the point.**
	 *
	 * The console and a program draw on the same framebuffer, so whichever
	 * goes last is what anybody sees -- and the console does not draw only
	 * when it is handed something new: a scroll repaints the whole window.
	 * Moving this call after the final message was not enough, because the
	 * final message was still a message. The screen somebody sees is the
	 * screen nothing has written to since.
	 *
	 * This is not a fix for the entry in docs/KERNEL-WANTS.md about the
	 * console and a program both owning the screen. It is an arrangement
	 * that works while there is exactly one program; the second one will
	 * need the kernel able to give the screen away.
	 */
	/* Unless the command line says not to.
	 *
	 * The first screen is a program painting on the framebuffer the console
	 * writes to, so it covers the boot report. On a machine that found a
	 * writable ReconOS medium that costs nothing -- the report is in a file
	 * on the medium. On one that did not, **the screen is the only copy**,
	 * and it is covered before anybody reads it.
	 *
	 * That has now cost three round trips to a laptop, so there is a word
	 * for it. The command line is read off the medium (KF-131), which means
	 * asking for this on the next boot is writing `\reconos\cmdline` --
	 * a file, not a reflash.
	 *
	 * It is not a fix for the entry in KERNEL-WANTS about the console and a
	 * program both owning the screen. It is a way to see the report while
	 * that is still true. */
	if (boot_cmdline_has("noinit")) {
		/* The few lines worth reading, printed again at the very end.
		 *
		 * The Gateway's report is about two hundred lines and its panel
		 * holds about fifty. Everything that answered the two questions
		 * it was booted to answer -- what the evaluator made of real
		 * firmware, and whether the kernel can see the stick it booted
		 * from -- had scrolled off the top before anybody could
		 * photograph it, and the only other copy was a file on the
		 * machine's own internal disk, which is the hardest place to
		 * reach it.
		 *
		 * **Re-called rather than reformatted.** Each of these owns its
		 * own wording; a second copy of that wording here would be a
		 * second place to fix when one of them changes, and the two
		 * would disagree about the same fact the first time somebody
		 * edited one.
		 *
		 * Only under `noinit`, for two reasons rather than taste: on a
		 * machine whose report fits, a repeat is noise -- and the
		 * verification rig greps these logs, so a line appearing twice
		 * where a check counts occurrences is a check that has started
		 * lying. The matrix never passes this word. */
		/* Longest first, most wanted last.
		 *
		 * `block_print_summary` lists every device on the PCI bus,
		 * which is a dozen lines on a real machine and is the least
		 * urgent thing here. Printing it first means that if anything
		 * still scrolls off, it is that -- and the two lines this whole
		 * arrangement exists for, the USB port count and the evaluator's
		 * tally, are the last things on the screen.
		 *
		 * The bottom of a screen is the only part guaranteed to survive
		 * a scroll, so what matters most goes there. */
		kputs("\nThe short version, last so the scroll cannot eat it\n");
		block_print_summary();
		aml_print_summary();
		boot_print_medium();
		boot_print_menu();
		usb_print_summary();
		aml_eval_print_summary();

		kputs("\nnoinit: the first screen was not started, so this "
		      "report stays on the screen.\n");
	} else {
		user_start_first_screen();
	}

	/* And from here this thread is not work any more.
	 *
	 * Said before the loop rather than assumed by it. The scheduler has
	 * always known how to keep an idle thread out of the round, and this
	 * thread was never marked as one -- so a program that yielded handed
	 * its turn to a thread that sleeps for up to a second, and every
	 * NVMe completion waited for a timer instead of for the drive. One
	 * directory on an installed volume took seventy seconds.
	 *
	 * Nothing below may block. `wait_sleep` refuses an idle thread, which
	 * is the rule saying so and not a limitation to work around. */
	/* KF-258's reproduction, and it is placed here on purpose: the fault
	 * only appears once a user program has started, so a probe that ran
	 * with the self-tests would pass and prove nothing. */
	if (boot_cmdline_has("sleepprobe"))
		timer_sleep_probe_start();

	sched_this_thread_is_now_idle();

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
	/* **Offered to the scheduler before the halt, not after it.** (KF-258)
	 *
	 * This loop used to be `power_idle_wait()` and nothing else, and that
	 * is a processor which, once idle, stops asking whether anything has
	 * become runnable. The halt ends on an interrupt and the loop halts
	 * again; the only thing that could hand the processor to a ready thread
	 * was a preemption, and `arch_wait_tickless` masks this processor's tick
	 * for the duration of the halt, so on the boot processor there is no
	 * preemption to be had.
	 *
	 * The measured cost was a thread created and then not run for three
	 * seconds on an otherwise empty machine -- quantised to the one-second
	 * idle ceiling, which is the tell: it started when the halt timed out,
	 * not when it became runnable.
	 *
	 * Yield first and halt second, rather than the other way round. The
	 * order is the whole of the fix: halting first spends up to a full
	 * ceiling before asking a question whose answer was already yes. */
	for (;;) {
		sched_yield();
		power_idle_wait();
	}
}
