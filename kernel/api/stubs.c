/*
 * Every kernel service, saying it is not implemented.
 *
 * This file exists so that recon_kernel.h is not merely a promise: the desktop
 * can include it, call it, link against it, and get a status back that its
 * error handling has to deal with. An interface nobody has compiled against is
 * an interface nobody has found the problems in.
 *
 * As each area becomes real it moves out of here into its own file and its
 * stub is deleted. When this file is empty, the kernel is finished -- which is
 * a pleasant way to measure a thing that otherwise has no obvious end.
 *
 * It is not built into the kernel image. It is built as a static library for
 * the desktop to link, because it is the desktop that needs something to call
 * today.
 */
#include <recon_kernel.h>

const char *recon_kernel_status_text(recon_status s)
{
	switch (s) {
	case RECON_K_OK:      return "ok";
	case RECON_K_ENOSYS:  return "not implemented yet";
	case RECON_K_ENODEV:  return "no such device";
	case RECON_K_ENOTSUP: return "the device cannot do that";
	case RECON_K_EINVAL:  return "the request does not make sense";
	case RECON_K_EPERM:   return "not permitted";
	case RECON_K_EBUSY:   return "in use";
	case RECON_K_EIO:     return "the hardware failed";
	case RECON_K_ERANGE:  return "the buffer is too small";
	case RECON_K_ESTALE:  return "the set changed; enumerate again";
	default:              return "unknown status";
	}
}

/* The one call that is not a stub, and the one the About page should ask
 * before claiming the system runs on its own kernel. */
bool recon_kernel_present(void)
{
	return false;
}

#define STUB(sig) sig { return RECON_K_ENOSYS; }

/* Devices */
STUB(recon_status recon_kernel_device_generation(uint64_t *out))
STUB(recon_status recon_kernel_device_count(size_t *out))
STUB(recon_status recon_kernel_device_at(size_t index, struct recon_k_device *out))
STUB(recon_status recon_kernel_device_by_id(uint64_t id, struct recon_k_device *out))

/* Block devices */
STUB(recon_status recon_kernel_block_count(size_t *out))
STUB(recon_status recon_kernel_block_at(size_t index, struct recon_k_block *out))
STUB(recon_status recon_kernel_block_read(uint64_t id, uint64_t first_sector,
					  size_t count, void *buf, size_t buf_size))
STUB(recon_status recon_kernel_block_write(uint64_t id, uint64_t first_sector,
					   size_t count, const void *buf,
					   size_t buf_size))
STUB(recon_status recon_kernel_block_flush(uint64_t id))

/* Time */
STUB(recon_status recon_kernel_time_monotonic_ns(uint64_t *out))
STUB(recon_status recon_kernel_time_wall_ns(uint64_t *out))

/* Randomness */
STUB(recon_status recon_kernel_random(void *buf, size_t len))
STUB(recon_status recon_kernel_random_ready(bool *out))

/* Power */
STUB(recon_status recon_kernel_power_supported(recon_k_power_state state, bool *out))
STUB(recon_status recon_kernel_power_enter(recon_k_power_state state))
