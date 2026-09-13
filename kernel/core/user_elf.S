/* The program, carried into the kernel image as a file rather than as code.
 *
 * These bytes are an ELF built by the cross linker from `user/hello.S` against
 * `user/user.ld`. Nothing here knows or cares what is in them: the kernel's
 * loader reads the program headers at run time, exactly as it will when the
 * same bytes arrive from a volume instead.
 *
 * Embedded with `.incbin` rather than `objcopy -I binary`, which names its
 * symbols after the path it was handed -- so the symbol would change when the
 * build directory did, and the link would fail in a way that names a mangled
 * path rather than a mistake.
 *
 * In `.rodata`, because a program image is data. It is also the only sensible
 * place for it: an executable page holding a file that is about to be *copied*
 * into a user mapping would be a second, kernel-side copy of the program's
 * code, executable, for the life of the machine.
 */

	.section .rodata
	.balign 16

	.global user_elf_image
user_elf_image:
	.incbin "hello.elf"

	.global user_elf_image_end
user_elf_image_end:

	.balign 8
	.global user_elf_image_len
user_elf_image_len:
	.quad	user_elf_image_end - user_elf_image
