/* Packet buffers, and the checksum everything above Ethernet shares.
 *
 * A buffer is one page from the physical allocator, which makes three things
 * true at once and is why it is a page rather than a heap allocation:
 *
 *   - it is **physically contiguous**, which a device DMA-ing into it requires
 *     and a heap allocation spanning two pages cannot promise;
 *   - it has a **physical address the driver can hand to hardware**, obtained
 *     by subtracting the direct map base, which only works because the page
 *     came from the allocator (KF-193 is what happens when it did not);
 *   - it either succeeds or it does not. There is no half-allocated buffer to
 *     unwind on the receive path, which runs in an interrupt.
 *
 * The cost is a page for a 64-byte ACK. That is the right trade here: the
 * alternative is a slab allocator sized per layer, and inventing one before
 * anything has measured a shortage is how an interface gets fixed in place
 * before it is understood.
 */
#include <recon/kernel/net.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>
#include <recon/kernel/lock.h>

const struct mac_addr MAC_BROADCAST = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
const struct mac_addr MAC_ZERO      = { { 0, 0, 0, 0, 0, 0 } };

/* Counters, and every one of them is printed. */
static u64 allocated;
static u64 freed;
static u64 alloc_failed;
static u64 push_refused;
static u64 pull_refused;
static u64 put_refused;

static struct spinlock buf_lock;
static bool lock_ready;

/* A free list, because the receive path allocates and frees one buffer per
 * frame and going to the page allocator each time takes its lock on every
 * packet. The list holds pages, not structures: a recycled buffer is reset
 * completely rather than trusted to be clean, since a field left over from the
 * last packet is the kind of fault that only shows up under load. */
static struct netbuf *free_list;
static unsigned free_count;

#define FREE_KEEP 32

bool mac_equal(const struct mac_addr *a, const struct mac_addr *b)
{
	return kmemcmp(a->b, b->b, MAC_LEN) == 0;
}

static char hex_digit(u8 v)
{
	return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

char *mac_format(const struct mac_addr *m, char *out)
{
	unsigned i;
	char *p = out;

	for (i = 0; i < MAC_LEN; i++) {
		if (i)
			*p++ = ':';
		*p++ = hex_digit((u8)(m->b[i] >> 4));
		*p++ = hex_digit((u8)(m->b[i] & 0xF));
	}

	*p = 0;
	return out;
}

char *ipv4_format(ipv4_addr a, char *out)
{
	char *p = out;
	int i;

	for (i = 3; i >= 0; i--) {
		u8 octet = (u8)((a >> (i * 8)) & 0xFF);
		u8 h = (u8)(octet / 100);
		u8 t = (u8)((octet / 10) % 10);

		if (i != 3)
			*p++ = '.';

		if (h)
			*p++ = (char)('0' + h);
		if (h || t)
			*p++ = (char)('0' + t);
		*p++ = (char)('0' + (octet % 10));
	}

	*p = 0;
	return out;
}

static void ensure_lock(void)
{
	if (!lock_ready) {
		spin_init(&buf_lock, "netbuf");
		lock_ready = true;
	}
}

struct netbuf *netbuf_alloc(void)
{
	struct netbuf *b;
	paddr_t page;
	u8 *virt;
	u64 flags;

	ensure_lock();

	flags = spin_lock_irq(&buf_lock);
	if (free_list) {
		b = free_list;
		free_list = b->next;
		free_count--;
		allocated++;
		spin_unlock_irq(&buf_lock, flags);

		/* Reset completely. A recycled buffer that kept `src_ip` from
		 * the previous packet answers a question nobody asked with an
		 * address from a conversation that already ended. */
		page = b->page;
		virt = b->head;
		kmemset(b, 0, sizeof(*b));
		b->page = page;
		b->head = virt;
		b->capacity = NET_BUF_CAPACITY;
		b->data = virt + NET_HEADROOM;
		return b;
	}
	spin_unlock_irq(&buf_lock, flags);

	page = pmm_alloc_page();

	if (!page) {
		flags = spin_lock_irq(&buf_lock);
		alloc_failed++;
		spin_unlock_irq(&buf_lock, flags);
		return NULL;
	}

	virt = phys_to_virt(page);
	kmemset(virt, 0, NET_BUF_PAGE_SIZE);

	/* The structure lives in the page's own tail, after the data area.
	 * One allocation, one free, and no second object to leak when the
	 * first one is released. */
	b = (struct netbuf *)(virt + NET_BUF_PAGE_SIZE - sizeof(struct netbuf));

	b->page = page;
	b->head = virt;
	b->capacity = NET_BUF_CAPACITY;
	b->data = virt + NET_HEADROOM;
	b->len = 0;

	flags = spin_lock_irq(&buf_lock);
	allocated++;
	spin_unlock_irq(&buf_lock, flags);

	return b;
}

void netbuf_free(struct netbuf *b)
{
	u64 flags;

	if (!b)
		return;

	ensure_lock();

	flags = spin_lock_irq(&buf_lock);
	freed++;

	if (free_count < FREE_KEEP) {
		b->next = free_list;
		free_list = b;
		free_count++;
		spin_unlock_irq(&buf_lock, flags);
		return;
	}
	spin_unlock_irq(&buf_lock, flags);

	pmm_free_pages(b->page, 1);
}

u8 *netbuf_push(struct netbuf *b, u32 n)
{
	/* The room in front is whatever is left between `head` and `data`. */
	u32 room = (u32)(b->data - b->head);

	if (n > room) {
		push_refused++;
		return NULL;
	}

	b->data -= n;
	b->len += n;
	return b->data;
}

u8 *netbuf_pull(struct netbuf *b, u32 n)
{
	if (n > b->len) {
		/* Not a fault in this kernel. A frame off the wire can claim a
		 * header longer than the frame, and the only correct answer is
		 * to refuse it. */
		pull_refused++;
		return NULL;
	}

	b->data += n;
	b->len -= n;
	return b->data;
}

u8 *netbuf_put(struct netbuf *b, u32 n)
{
	u32 used = (u32)(b->data - b->head) + b->len;
	u8 *at;

	if (used + n > b->capacity) {
		put_refused++;
		return NULL;
	}

	at = b->data + b->len;
	b->len += n;
	return at;
}

/* --- The Internet checksum ------------------------------------------------ */

u32 net_checksum_partial(const void *data, u32 len, u32 sum)
{
	const u8 *p = data;

	/* Pairs of bytes, big-endian, which is what the wire holds. Reading
	 * them as u16 and swapping would be the same arithmetic and would
	 * require the pointer to be aligned; this does not. */
	while (len > 1) {
		sum += net_get16(p);
		p += 2;
		len -= 2;
	}

	/* An odd trailing byte is the high half of a word whose low half is
	 * zero -- not the low half, which is the mistake that makes a checksum
	 * agree with itself and with nothing else. */
	if (len)
		sum += (u32)p[0] << 8;

	return sum;
}

u16 net_checksum_finish(u32 sum)
{
	/* Fold the carries down. Twice: the first fold can itself carry. */
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (u16)(~sum & 0xFFFF);
}

u16 net_checksum(const void *data, u32 len)
{
	return net_checksum_finish(net_checksum_partial(data, len, 0));
}

/* --- What it is doing ------------------------------------------------------ */

void netbuf_print_summary(void)
{
	kprintf("  buffers      : %u taken, %u given back, %u held\n",
		(unsigned)allocated, (unsigned)freed, free_count);

	if (alloc_failed || push_refused || pull_refused || put_refused)
		kprintf("  refusals     : %u no memory, %u no headroom, "
			"%u short header, %u no room\n",
			(unsigned)alloc_failed, (unsigned)push_refused,
			(unsigned)pull_refused, (unsigned)put_refused);
	else
		kprintf("  refusals     : none\n");
}

bool netbuf_self_test(void)
{
	struct netbuf *b = netbuf_alloc();
	u8 *p;
	u32 i;
	bool ok = true;

	if (!b) {
		kprintf("netbuf: no memory for the test\n");
		return false;
	}

	/* Payload, then three headers pushed in front of it, which is the
	 * whole reason this structure exists. */
	p = netbuf_put(b, 100);

	if (!p) {
		kprintf("netbuf: put refused 100 bytes\n");
		netbuf_free(b);
		return false;
	}

	for (i = 0; i < 100; i++)
		p[i] = (u8)i;

	if (b->len != 100) {
		kprintf("netbuf: len %u after putting 100\n", (unsigned)b->len);
		ok = false;
	}

	if (!netbuf_push(b, 20) || b->len != 120) {
		kprintf("netbuf: push of an IP header failed\n");
		ok = false;
	}

	if (!netbuf_push(b, 14) || b->len != 134) {
		kprintf("netbuf: push of an Ethernet header failed\n");
		ok = false;
	}

	/* And the payload is still where it was, byte for byte -- which is the
	 * claim that matters. A stack that copied would pass every length
	 * assertion above and fail this one. */
	if (!netbuf_pull(b, 14) || !netbuf_pull(b, 20)) {
		kprintf("netbuf: pull back off failed\n");
		ok = false;
	} else {
		for (i = 0; i < 100; i++) {
			if (b->data[i] != (u8)i) {
				kprintf("netbuf: payload moved at byte %u\n",
					(unsigned)i);
				ok = false;
				break;
			}
		}
	}

	/* Headroom is finite and running out must be refused, not wrapped. */
	{
		struct netbuf *c = netbuf_alloc();

		if (c) {
			if (netbuf_push(c, NET_HEADROOM + 1)) {
				kprintf("netbuf: pushed past the headroom\n");
				ok = false;
			}
			netbuf_free(c);
		}
	}

	/* A header longer than the frame is refused rather than believed. */
	{
		struct netbuf *c = netbuf_alloc();

		if (c) {
			netbuf_put(c, 4);
			if (netbuf_pull(c, 20)) {
				kprintf("netbuf: pulled a header past the end\n");
				ok = false;
			}
			netbuf_free(c);
		}
	}

	netbuf_free(b);

	/* The checksum, against a case whose answer is written down in
	 * RFC 1071: this header sums to 0xb1e6, so the value carried on the
	 * wire is its complement. Checked against a known answer rather than
	 * against itself, because a checksum tested by re-running the same
	 * function agrees with every bug it contains. */
	{
		static const u8 hdr[20] = {
			0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
			0x40, 0x11, 0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01,
			0xc0, 0xa8, 0x00, 0xc7
		};
		u16 sum = net_checksum(hdr, sizeof(hdr));

		if (sum != 0xb861) {
			kprintf("netbuf: checksum %u, expected 47201\n",
				(unsigned)sum);
			ok = false;
		}

		/* And a header carrying its own correct checksum sums to zero,
		 * which is the property every receiver actually relies on. */
		{
			u8 with[20];
			u16 over;

			kmemcpy(with, hdr, sizeof(hdr));
			net_put16(with + 10, sum);
			over = net_checksum(with, sizeof(with));

			if (over != 0) {
				kprintf("netbuf: a checked header summed to %u,"
					" not 0\n", (unsigned)over);
				ok = false;
			}
		}
	}

	/* Byte order, which is not a formality: a port swapped the wrong way is
	 * a different port and the failure is silent. */
	if (net_htons(0x1234) != 0x3412 || net_htonl(0x12345678) != 0x78563412) {
		kprintf("netbuf: byte order is wrong\n");
		ok = false;
	}

	return ok;
}
