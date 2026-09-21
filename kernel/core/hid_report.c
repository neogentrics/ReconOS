/* Walking a HID report descriptor.
 *
 * See `hid_report.h` for why this exists and what this pass answers. This file
 * is the walk.
 *
 * --- The rule that this format is famous for getting wrong ---
 *
 * A short item's prefix carries its data length in its bottom two bits, and
 * they encode **0, 1, 2 and 4**. Not 0, 1, 2, 3. A parser that advances by the
 * raw two-bit value handles the first three cases perfectly and then, on the
 * first four-byte item, advances by three and lands one byte short -- on a
 * data byte, which it reads as a prefix.
 *
 * What makes it expensive is what happens next. It does not crash and it does
 * not stop: it reads plausible items out of the middle of somebody's logical
 * maximum and carries on, and the descriptor still "parses". Four-byte items
 * are uncommon in mouse descriptors and ordinary in others, so this is a
 * parser that works on the device you tested and not on the device you ship.
 *
 * --- And the other way to lose your place ---
 *
 * A long item is prefixed `0xFE` and its length lives in the byte after,
 * rather than in the prefix. Nothing here understands any long item, and it
 * does not need to -- but it has to skip exactly the right number of bytes,
 * because skipping the wrong number puts the walk back into the middle of
 * data with no way to notice.
 *
 * Both are below and both are tested, because in each case the wrong answer is
 * a descriptor that parses into something.
 */
#include <recon/kernel/console.h>
#include <recon/kernel/hid_report.h>
#include <recon/kernel/kstring.h>

u32 hid_item_data_length(u8 prefix)
{
	/* **0, 1, 2, 4.** The fourth value is not three. */
	switch (prefix & 0x03u) {
	case 0:  return 0;
	case 1:  return 1;
	case 2:  return 2;
	default: return 4;
	}
}

/* An item's data, as an unsigned value.
 *
 * Unsigned on purpose, and it is worth saying why rather than leaving it to be
 * discovered: logical minimum is signed and report size is not, and this pass
 * reads only the unsigned ones. A signed reader is needed for field ranges and
 * belongs with the pass that uses them -- reading report count as signed would
 * turn a count of 0x80 into a negative number and then into an enormous one.
 */
static u32 item_value(const u8 *data, u32 n)
{
	u32 v = 0;
	u32 i;

	for (i = 0; i < n; i++)
		v |= ((u32)data[i]) << (8u * i);

	return v;
}

static void note_report_id(struct hid_report_info *info, u8 id)
{
	unsigned i;

	info->uses_report_id = true;

	for (i = 0; i < info->report_id_count; i++)
		if (info->report_ids[i] == id)
			return;

	if (info->report_id_count >= HID_MAX_REPORT_IDS) {
		info->report_ids_truncated = true;
		return;
	}

	info->report_ids[info->report_id_count++] = id;
}

/* --- the two halves of the state, and why they are not one ----------------
 *
 * **Global items persist until changed. Local items are cleared after every
 * Main item.** That is the rule the whole second pass turns on, and getting it
 * backwards does not fail -- it produces a map.
 *
 * Concretely: a descriptor says Usage(X), Usage(Y), then Input. Those two
 * usages belong to that one Input item and are gone afterwards. Report Size
 * and Report Count, set before them, are still in force for the *next* Input
 * item too. A parser that keeps the usages around hands X and Y to a later
 * field that is really a wheel; a parser that clears the sizes asks for
 * zero-bit fields and every offset after that is wrong.
 *
 * So `local` is wiped at the bottom of every Main item and `global` is not.
 */
struct hid_local {
	u16 usages[HID_MAX_FIELDS];
	unsigned usage_count;
	bool usage_min_max_valid;
	u16 usage_min;
	u16 usage_max;
};

static void local_clear(struct hid_local *l)
{
	l->usage_count = 0;
	l->usage_min_max_valid = false;
	l->usage_min = 0;
	l->usage_max = 0;
}

static void withhold(struct hid_report_info *info, const char *why)
{
	/* First reason wins. The later ones are usually consequences of the
	 * first, and a message naming the third thing that went wrong sends
	 * the reader to the wrong place. */
	if (info->fields_usable) {
		info->fields_usable = false;
		info->fields_unusable = why;
	}
}

/* The usage for field `n` of a Variable Input item.
 *
 * Three shapes, and the third is the one worth naming: an explicit list of
 * usages, a minimum-to-maximum range (which is how a row of buttons is
 * written), or fewer usages than fields. The last case is a real descriptor
 * pattern and this pass does not claim to know the rule for it, so it returns
 * false and the caller withholds the map rather than inventing a usage.
 */
static bool usage_for(const struct hid_local *l, u32 n, u16 *usage)
{
	if (l->usage_min_max_valid) {
		u32 u = (u32)l->usage_min + n;

		if (u > l->usage_max)
			return false;

		*usage = (u16)u;
		return true;
	}

	if (n < l->usage_count) {
		*usage = l->usages[n];
		return true;
	}

	return false;
}

bool hid_report_parse(const u8 *desc, u32 len, struct hid_report_info *info)
{
	u32 at = 0;
	u32 report_size = 0;
	u32 report_count = 0;
	u32 bit_offset = 0;
	u16 usage_page = 0;
	unsigned depth = 0;
	struct hid_local local;

	kmemset(info, 0, sizeof(*info));
	local_clear(&local);

	/* Usable until something says otherwise. */
	info->fields_usable = true;

	while (at < len) {
		u8  prefix = desc[at];
		u8  tag, type;
		u32 dlen;
		const u8 *data;

		if (prefix == HID_LONG_ITEM_PREFIX) {
			/* prefix, size, tag, then size bytes. */
			if (at + 3 > len)
				return false;

			dlen = desc[at + 1];

			if (at + 3u + dlen > len)
				return false;

			info->long_items++;
			info->items++;
			at += 3u + dlen;
			continue;
		}

		dlen = hid_item_data_length(prefix);

		/* An item declaring data that is not there. Refused rather than
		 * clamped: a clamped item is a value assembled partly from the
		 * item and partly from whatever follows the descriptor. */
		if (at + 1u + dlen > len)
			return false;

		data = desc + at + 1;
		tag  = (u8)(prefix >> 4);
		type = (u8)((prefix >> 2) & 0x03u);

		info->items++;

		if (type == HID_ITEM_LOCAL) {
			switch (tag) {
			case HID_LOCAL_USAGE:
				/* A four-byte Usage carries the page in its
				 * top half. Taking the whole thing as a usage
				 * gives a number that is never matched and a
				 * field that silently means nothing. */
				if (dlen == 4)
					withhold(info, "a usage with its own "
						       "page in it");
				else if (local.usage_count < HID_MAX_FIELDS)
					local.usages[local.usage_count++] =
						(u16)item_value(data, dlen);
				else
					info->fields_truncated = true;
				break;

			case HID_LOCAL_USAGE_MINIMUM:
				local.usage_min = (u16)item_value(data, dlen);
				local.usage_min_max_valid = true;
				break;

			case HID_LOCAL_USAGE_MAXIMUM:
				local.usage_max = (u16)item_value(data, dlen);
				break;

			default:
				break;
			}
		} else if (type == HID_ITEM_GLOBAL) {
			switch (tag) {
			case HID_GLOBAL_USAGE_PAGE:
				usage_page = (u16)item_value(data, dlen);
				break;
			case HID_GLOBAL_PUSH:
			case HID_GLOBAL_POP:
				/* Saving and restoring the global state. Not
				 * implemented, and everything after a Push is
				 * built on a state this walker no longer
				 * tracks -- so the map goes, rather than the
				 * offsets being quietly wrong from here on. */
				withhold(info, "Push/Pop of the global state");
				break;
			case HID_GLOBAL_REPORT_SIZE:
				report_size = item_value(data, dlen);
				break;
			case HID_GLOBAL_REPORT_COUNT:
				report_count = item_value(data, dlen);
				break;
			case HID_GLOBAL_REPORT_ID:
				/* A report ID of zero is not a report ID --
				 * the value is reserved, and treating it as
				 * one would claim every report carries a byte
				 * it does not. */
				if (dlen && item_value(data, dlen))
					note_report_id(info,
						(u8)item_value(data, dlen));
				break;
			default:
				break;
			}
		} else if (type == HID_ITEM_MAIN) {
			info->main_items++;

			switch (tag) {
			case HID_MAIN_INPUT: {
				u8  flags = dlen ? (u8)item_value(data, dlen)
						 : 0;
				bool constant = (flags & HID_INPUT_CONSTANT)
						!= 0;
				bool variable = (flags & HID_INPUT_VARIABLE)
						!= 0;
				u32 n;

				/* Size times count, in bits, and the padding
				 * items count too: an Input item marked
				 * constant is filler with no usage, and it
				 * still occupies its bits in the report. A
				 * walker that skipped them would compute a
				 * report shorter than the device sends. */
				info->input_bits += report_size * report_count;

				if (!variable && !constant) {
					/* An array: the report carries a list
					 * of which usages are active rather
					 * than one value per usage, which is
					 * how a keyboard sends its keys. A
					 * different shape entirely, and
					 * pretending otherwise would place
					 * fields that are not there. */
					withhold(info, "an array Input item, "
						       "which is how keyboards "
						       "report");
				}

				for (n = 0; n < report_count; n++) {
					struct hid_field *f;
					u16 usage = 0;

					if (info->field_count >=
					    HID_MAX_FIELDS) {
						info->fields_truncated = true;
						break;
					}

					/* Padding has no usage and is still
					 * placed, because everything after it
					 * sits at an offset that counts
					 * it. */
					if (!constant && variable &&
					    !usage_for(&local, n, &usage))
						withhold(info,
							 "an Input item with "
							 "fewer usages than "
							 "fields");

					f = &info->fields[info->field_count++];
					f->bit_offset = bit_offset +
							n * report_size;
					f->bit_size   = report_size;
					f->usage_page = constant ? 0
								 : usage_page;
					f->usage      = constant ? 0 : usage;
					f->constant   = constant;
					f->relative   = (flags &
							 HID_INPUT_RELATIVE)
							!= 0;
				}

				bit_offset += report_size * report_count;
				break;
			}

			case HID_MAIN_COLLECTION:
				depth++;

				if (depth > info->collection_depth_max)
					info->collection_depth_max = depth;
				break;

			case HID_MAIN_END_COLLECTION:
				/* More ends than starts. The descriptor is
				 * malformed, and continuing would underflow
				 * the depth into a very large number. */
				if (!depth)
					return false;

				depth--;
				break;

			default:
				break;
			}

			/* **After every Main item, without exception.**
			 *
			 * Not only after an Input: a Collection is a Main item
			 * too, and the Usage that names it -- Usage(Mouse)
			 * before Collection(Application) -- must not still be
			 * sitting in the list when the next Input comes to
			 * take its usages.
			 *
			 * Worth being exact about when it bites, because it is
			 * narrower than it looks and this comment first said
			 * otherwise. An Input whose usages come from a
			 * Usage Minimum/Maximum range is unaffected: the range
			 * is consulted before the list, so a leaked usage sits
			 * there unread. It bites an Input that takes usages
			 * from the list -- the leaked one becomes field zero
			 * and everything real shifts down by one. A boot
			 * mouse's buttons use the range and its axes use the
			 * list, so only the axes could show it, and only if
			 * the leak survived the Input before them. The test
			 * for this uses a descriptor with no range at all. */
			local_clear(&local);
		}

		at += 1u + dlen;
	}

	/* A collection left open means the walk ended inside something, which
	 * usually means it lost its place earlier and the items since were
	 * invented. */
	if (depth)
		return false;

	/* The length is only one number when there is only one report. With
	 * report IDs each has its own, and a single figure here would be a
	 * wrong answer rather than an absent one. */
	if (!info->uses_report_id) {
		info->input_bits_known = true;
		info->input_bytes = (info->input_bits + 7u) / 8u;
	} else {
		info->input_bits = 0;
		info->input_bytes = 0;
	}

	return true;
}

bool hid_report_mouse_layout(const struct hid_report_info *info,
			     struct hid_mouse_layout *out)
{
	unsigned i;
	bool have_x = false, have_y = false;
	u32 last_button_bit = 0;
	u32 end_bits = 0;

	kmemset(out, 0, sizeof(*out));

	/* A map that was withheld is not a map to read. Checked first, because
	 * every loop below would otherwise run over a half-built one and
	 * produce a layout that looks complete. */
	if (!info->fields_usable || !info->field_count)
		return false;

	/* **More than one report id and the field offsets mean nothing.**
	 *
	 * `bit_offset` accumulates across every Input item in the descriptor,
	 * so with two reports the second one's fields are placed after the
	 * first one's rather than at zero. One report id is fine -- there is
	 * still only one report, and its fields begin where the walk began.
	 * Two is a different model than this pass has, and a layout built
	 * anyway would put X somewhere the device never writes. */
	if (info->report_id_count > 1 || info->report_ids_truncated)
		return false;

	for (i = 0; i < info->field_count; i++) {
		const struct hid_field *f = &info->fields[i];

		/* The report's length, from the fields themselves.
		 *
		 * **Not from `input_bits`**, which `hid_report_parse`
		 * deliberately zeroes for a device with report ids because
		 * the length is per-id there. Taking it from that gave a
		 * report of zero bytes, which silently turned off
		 * `hid_mouse_decode`'s short-report check -- `len < 0` is
		 * never true. Padding counts, so this runs before the
		 * constant test below. */
		if (f->bit_offset + f->bit_size > end_bits)
			end_bits = f->bit_offset + f->bit_size;

		if (f->constant)
			continue;

		if (f->usage_page == HID_PAGE_BUTTON && f->usage) {
			/* Buttons are one bit each and consecutive. The first
			 * one fixes the offset; the rest only have to still be
			 * adjacent, and a gap means this is not the simple row
			 * this can describe. */
			if (!out->buttons_count) {
				out->buttons_offset = f->bit_offset;
				out->buttons_count = 1;
				last_button_bit = f->bit_offset;
			} else if (f->bit_offset == last_button_bit +
						   f->bit_size) {
				out->buttons_count++;
				last_button_bit = f->bit_offset;
			}

			continue;
		}

		if (f->usage_page != HID_PAGE_GENERIC_DESKTOP)
			continue;

		switch (f->usage) {
		case HID_USAGE_X:
			out->x_offset = f->bit_offset;
			out->x_size = f->bit_size;
			have_x = true;
			break;
		case HID_USAGE_Y:
			out->y_offset = f->bit_offset;
			out->y_size = f->bit_size;
			have_y = true;
			break;
		case HID_USAGE_WHEEL:
			out->wheel_offset = f->bit_offset;
			out->wheel_size = f->bit_size;
			out->have_wheel = true;
			break;
		default:
			break;
		}
	}

	/* Buttons alone are not a mouse -- that is a gamepad or a foot pedal,
	 * and driving it as a pointer would post motion that does not exist. */
	if (!have_x || !have_y)
		return false;

	out->report_bytes = (end_bits + 7u) / 8u;
	out->found = true;

	return true;
}

void hid_mouse_boot_layout(struct hid_mouse_layout *out, bool with_wheel)
{
	kmemset(out, 0, sizeof(*out));

	/* Three buttons in the low three bits of byte 0, five bits of padding
	 * above them, then two signed bytes. Identical to what the boot
	 * descriptor parses to, which the self-test asserts rather than
	 * assumes. */
	out->buttons_offset = 0;
	out->buttons_count = 3;
	out->x_offset = 8;
	out->x_size = 8;
	out->y_offset = 16;
	out->y_size = 8;

	if (with_wheel) {
		out->have_wheel = true;
		out->wheel_offset = 24;
		out->wheel_size = 8;
		out->report_bytes = 4;
	} else {
		out->report_bytes = 3;
	}

	out->found = true;
}

/* --- reading a field out of a report -------------------------------------- */

bool hid_field_extract(const u8 *report, u32 report_len, u32 bit_offset,
		       u32 bit_size, bool is_signed, i32 *out)
{
	u32 v = 0;
	u32 i;
	u32 last_bit;

	/* Zero bits is not a field, and more than 32 does not fit the result.
	 * Both refused rather than clamped: a clamped field is a value read
	 * from some of the bits the descriptor named. */
	if (!bit_size || bit_size > 32)
		return false;

	/* The field's last bit has to be inside the report. Computed as a bit
	 * position and then rounded up to a byte, rather than comparing bytes
	 * -- a field ending at bit 17 needs three bytes, and byte arithmetic
	 * on the offset alone loses that. */
	last_bit = bit_offset + bit_size;

	if ((last_bit + 7u) / 8u > report_len)
		return false;

	/* Least significant bit first. Bit `i` of the field comes from bit
	 * `(bit_offset + i) % 8` of byte `(bit_offset + i) / 8`. */
	for (i = 0; i < bit_size; i++) {
		u32 b = bit_offset + i;
		u32 bit = ((u32)report[b / 8u] >> (b % 8u)) & 1u;

		v |= bit << i;
	}

	/* Sign extension from the field's own width.
	 *
	 * Guarded at 32 because shifting a 32-bit value by 32 is undefined --
	 * and at 32 bits there is nothing to extend, the value already fills
	 * the result. */
	if (is_signed && bit_size < 32) {
		u32 sign = 1u << (bit_size - 1u);

		if (v & sign)
			v |= ~((1u << bit_size) - 1u);
	}

	*out = (i32)v;
	return true;
}

bool hid_mouse_decode(const struct hid_mouse_layout *m, const u8 *report,
		      u32 len, struct hid_mouse_state *out)
{
	u32 i;

	kmemset(out, 0, sizeof(*out));

	if (!m->found || len < m->report_bytes)
		return false;

	/* Buttons, one bit each, into a mask in declaration order. Read one at
	 * a time rather than as a single field of `buttons_count` bits --
	 * which would be the same answer here and would stop being so the
	 * moment a descriptor puts a gap in the row. */
	for (i = 0; i < m->buttons_count; i++) {
		i32 bit;

		if (!hid_field_extract(report, len, m->buttons_offset + i, 1,
				       false, &bit))
			return false;

		if (bit)
			out->buttons |= 1u << i;
	}

	/* Axes are signed: a mouse reports deltas in both directions. */
	if (!hid_field_extract(report, len, m->x_offset, m->x_size, true,
			       &out->x))
		return false;

	if (!hid_field_extract(report, len, m->y_offset, m->y_size, true,
			       &out->y))
		return false;

	if (m->have_wheel &&
	    !hid_field_extract(report, len, m->wheel_offset, m->wheel_size,
			       true, &out->wheel))
		return false;

	return true;
}

/* --- the self-test --------------------------------------------------------
 *
 * The descriptor below is a real boot-mouse one, and it is used because its
 * answer is independently known: three bytes, no report ID -- which is exactly
 * the layout `usb_hid.c` decodes. A parser that gets 24 bits out of it agrees
 * with a driver that has been reading those three bytes off real hardware.
 *
 * The cases are chosen the same way as the layers above:
 *
 *   - the two size bits encode 0, 1, 2, **4**, checked directly, because every
 *     other check here would pass with a parser that returns 3 until the first
 *     four-byte item appears;
 *   - a four-byte item must not desynchronise the walk, checked by putting one
 *     in front of a descriptor whose answer is known;
 *   - padding Input items count toward the report length;
 *   - an item declaring data past the end must be refused;
 *   - an unbalanced collection must be refused in both directions;
 *   - a report ID must be noticed, because that is the whole reason this file
 *     exists.
 */
static const u8 boot_mouse[] = {
	0x05, 0x01,		/* Usage Page (Generic Desktop)   */
	0x09, 0x02,		/* Usage (Mouse)                  */
	0xA1, 0x01,		/* Collection (Application)       */
	0x09, 0x01,		/*   Usage (Pointer)              */
	0xA1, 0x00,		/*   Collection (Physical)        */
	0x05, 0x09,		/*     Usage Page (Button)        */
	0x19, 0x01,		/*     Usage Minimum (1)          */
	0x29, 0x03,		/*     Usage Maximum (3)          */
	0x15, 0x00,		/*     Logical Minimum (0)        */
	0x25, 0x01,		/*     Logical Maximum (1)        */
	0x95, 0x03,		/*     Report Count (3)           */
	0x75, 0x01,		/*     Report Size (1)            */
	0x81, 0x02,		/*     Input (Data,Var,Abs)       */
	0x95, 0x01,		/*     Report Count (1)           */
	0x75, 0x05,		/*     Report Size (5)            */
	0x81, 0x01,		/*     Input (Const) -- padding   */
	0x05, 0x01,		/*     Usage Page (Generic Desktop) */
	0x09, 0x30,		/*     Usage (X)                  */
	0x09, 0x31,		/*     Usage (Y)                  */
	0x15, 0x81,		/*     Logical Minimum (-127)     */
	0x25, 0x7F,		/*     Logical Maximum (127)      */
	0x75, 0x08,		/*     Report Size (8)            */
	0x95, 0x02,		/*     Report Count (2)           */
	0x81, 0x06,		/*     Input (Data,Var,Rel)       */
	0xC0,			/*   End Collection               */
	0xC0			/* End Collection                 */
};

/* A descriptor read off an actual mouse.
 *
 * **Every other fixture in this file was written by the same hand that wrote
 * the parser**, which means a wrong idea of the format would be spelled the
 * same way in both and every test would agree with the mistake. This one was
 * not: it is 87 bytes pulled out of a real device over
 * `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` on 17 September 2026 --
 * `3554:f54f`, interface 2, the mouse this branch was written on. Its own HID
 * descriptor declares 87 bytes and 87 arrived, which is the first check.
 *
 * It is a much harder case than anything invented here, and that is the point:
 *
 *   - **five** buttons, not three;
 *   - **sixteen-bit** axes, where every synthetic test used eight;
 *   - three-byte items (`16 00 80`, `26 ff 7f`, `0a 38 02`), which is the
 *     size-index rule the walk can get wrong;
 *   - a Physical collection that closes and **reopens twice**, so the depth
 *     goes up and down rather than only up;
 *   - a usage on the Consumer page, which a mouse layout must ignore rather
 *     than mistake for an axis.
 *
 * Hand-decoded to 5 + 3 + 32 + 8 + 8 = 56 bits, ten fields, seven bytes. The
 * parser is checked against that arithmetic below.
 */
static const u8 real_mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
	0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00,
	0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
	0x81, 0x06, 0xC0, 0xA1, 0x00, 0x05, 0x01, 0x09,
	0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95,
	0x01, 0x81, 0x06, 0xC0, 0xA1, 0x00, 0x05, 0x0C,
	0x0A, 0x38, 0x02, 0x95, 0x01, 0x75, 0x08, 0x15,
	0x81, 0x25, 0x7F, 0x81, 0x06, 0xC0, 0xC0
};

bool hid_report_self_test(void)
{
	/* Static rather than automatic: the struct now carries a 32-field map
	 * and this function holds two at once, which is most of a kilobyte on a
	 * stack that has other plans. A boot-time self-test runs once. */
	static struct hid_report_info info;
	bool ok = true;

	/* --- 0, 1, 2, 4 -------------------------------------------------- */
	if (hid_item_data_length(0x00) != 0 ||
	    hid_item_data_length(0x01) != 1 ||
	    hid_item_data_length(0x02) != 2 ||
	    hid_item_data_length(0x03) != 4) {
		kprintf("  hidrep: the item size bits decode as %u %u %u %u, "
			"expected 0 1 2 4 -- the fourth is four, and a parser "
			"that advances by three lands on a data byte and "
			"reads it as an item\n",
			hid_item_data_length(0x00), hid_item_data_length(0x01),
			hid_item_data_length(0x02), hid_item_data_length(0x03));
		ok = false;
	}

	/* --- a real boot mouse ------------------------------------------- */
	if (!hid_report_parse(boot_mouse, sizeof(boot_mouse), &info)) {
		kputs("  hidrep: a boot mouse descriptor was refused\n");
		ok = false;
	} else {
		if (info.uses_report_id) {
			kputs("  hidrep: a boot mouse descriptor was read as "
			      "using report ids, and it has none\n");
			ok = false;
		}

		/* 3 buttons at 1 bit, 5 bits of padding, 2 axes at 8 bits. */
		if (!info.input_bits_known || info.input_bits != 24) {
			kprintf("  hidrep: the mouse's input report came to "
				"%u bits, expected 24 -- three buttons, five "
				"bits of padding, and two eight-bit axes\n",
				info.input_bits);
			ok = false;
		}

		if (info.input_bytes != 3) {
			kprintf("  hidrep: that is %u bytes, expected 3, "
				"which is what usb_hid.c decodes off real "
				"hardware\n", info.input_bytes);
			ok = false;
		}

		if (info.collection_depth_max != 2) {
			kprintf("  hidrep: the collections nested %u deep, "
				"expected 2\n", info.collection_depth_max);
			ok = false;
		}
	}

	/* --- a four-byte item must not shift the walk --------------------
	 *
	 * The same descriptor with a four-byte Usage Page in front. The answer
	 * must not change. A parser advancing by three reads the last byte of
	 * that item as a prefix and everything after it is invented.
	 */
	{
		u8 shifted[sizeof(boot_mouse) + 5];
		static struct hid_report_info after;

		shifted[0] = 0x07;		/* Usage Page, four bytes */
		shifted[1] = 0x01;
		shifted[2] = 0x00;
		shifted[3] = 0x00;
		shifted[4] = 0x00;
		kmemcpy(shifted + 5, boot_mouse, sizeof(boot_mouse));

		if (!hid_report_parse(shifted, sizeof(shifted), &after)) {
			kputs("  hidrep: a descriptor opening with a "
			      "four-byte item was refused\n");
			ok = false;
		} else if (!after.input_bits_known || after.input_bits != 24 ||
			   after.collection_depth_max != 2) {
			kprintf("  hidrep: after a four-byte item the same "
				"mouse reads as %u bits and %u deep, not 24 "
				"and 2 -- the walk advanced by three and lost "
				"its place\n", after.input_bits,
				after.collection_depth_max);
			ok = false;
		}
	}

	/* --- a report ID is noticed --------------------------------------- */
	{
		static const u8 with_id[] = {
			0xA1, 0x01,		/* Collection (Application) */
			0x85, 0x02,		/*   Report ID (2)          */
			0x75, 0x08,		/*   Report Size (8)        */
			0x95, 0x03,		/*   Report Count (3)       */
			0x81, 0x02,		/*   Input                  */
			0xC0			/* End Collection           */
		};

		if (!hid_report_parse(with_id, sizeof(with_id), &info)) {
			kputs("  hidrep: a descriptor with a report id was "
			      "refused\n");
			ok = false;
		} else {
			if (!info.uses_report_id) {
				kputs("  hidrep: a Report ID item was not "
				      "noticed, which is the one question "
				      "this file exists to answer\n");
				ok = false;
			}

			if (info.report_id_count != 1 ||
			    info.report_ids[0] != 2) {
				kprintf("  hidrep: %u report id(s), first "
					"%u, expected one and 2\n",
					info.report_id_count,
					info.report_ids[0]);
				ok = false;
			}

			/* And no length is claimed, because with report ids
			 * there is more than one answer. */
			if (info.input_bits_known || info.input_bits) {
				kprintf("  hidrep: a length of %u bits was "
					"claimed for a device with report "
					"ids, where each report has its own\n",
					info.input_bits);
				ok = false;
			}
		}
	}

	/* --- a report ID of zero is not one ------------------------------- */
	{
		static const u8 zero_id[] = {
			0xA1, 0x01,
			0x85, 0x00,		/* Report ID (0) -- reserved */
			0x75, 0x08,
			0x95, 0x01,
			0x81, 0x02,
			0xC0
		};

		if (!hid_report_parse(zero_id, sizeof(zero_id), &info)) {
			kputs("  hidrep: a descriptor with a zero report id "
			      "was refused\n");
			ok = false;
		} else if (info.uses_report_id) {
			kputs("  hidrep: report id zero was taken as a real "
			      "one, which would claim every report carries a "
			      "byte it does not\n");
			ok = false;
		}
	}

	/* --- an item running past the end --------------------------------- */
	{
		static const u8 truncated[] = { 0x05 };	/* wants one byte */

		if (hid_report_parse(truncated, sizeof(truncated), &info)) {
			kputs("  hidrep: an item declaring data that is not "
			      "there was accepted\n");
			ok = false;
		}
	}

	/* --- collections that do not balance ------------------------------ */
	{
		static const u8 unclosed[] = { 0xA1, 0x01 };
		static const u8 extra_end[] = { 0xC0 };

		if (hid_report_parse(unclosed, sizeof(unclosed), &info)) {
			kputs("  hidrep: a collection that never closes was "
			      "accepted, so the walk ended inside it\n");
			ok = false;
		}

		if (hid_report_parse(extra_end, sizeof(extra_end), &info)) {
			kputs("  hidrep: an End Collection with nothing open "
			      "was accepted, and the depth would have "
			      "underflowed\n");
			ok = false;
		}
	}

	/* --- a long item is skipped by the right amount -------------------
	 *
	 * Nothing here understands long items. Skipping one wrongly leaves the
	 * walk inside its data, so this puts one in front of the mouse and
	 * checks the mouse still reads the same.
	 */
	{
		u8 with_long[sizeof(boot_mouse) + 7];
		static struct hid_report_info after;

		with_long[0] = HID_LONG_ITEM_PREFIX;
		with_long[1] = 4;		/* four bytes of data */
		with_long[2] = 0x0F;		/* some long tag */
		with_long[3] = 0xA1;		/* data that looks like items */
		with_long[4] = 0x01;
		with_long[5] = 0xC0;
		with_long[6] = 0xC0;
		kmemcpy(with_long + 7, boot_mouse, sizeof(boot_mouse));

		if (!hid_report_parse(with_long, sizeof(with_long), &after)) {
			kputs("  hidrep: a descriptor opening with a long "
			      "item was refused\n");
			ok = false;
		} else {
			if (after.long_items != 1) {
				kprintf("  hidrep: %u long items seen, "
					"expected 1\n", after.long_items);
				ok = false;
			}

			/* The long item's data was chosen to look like a
			 * collection and two ends. If the skip is wrong, those
			 * are walked as items and the depth check fails. */
			if (!after.input_bits_known ||
			    after.input_bits != 24 ||
			    after.collection_depth_max != 2) {
				kprintf("  hidrep: after a long item the "
					"mouse reads as %u bits and %u deep, "
					"not 24 and 2 -- its data was walked "
					"as items\n", after.input_bits,
					after.collection_depth_max);
				ok = false;
			}
		}

		/* A long item declaring more than is left. */
		{
			static const u8 bad_long[] = {
				HID_LONG_ITEM_PREFIX, 200, 0x0F
			};

			if (hid_report_parse(bad_long, sizeof(bad_long),
					     &info)) {
				kputs("  hidrep: a long item declaring 200 "
				      "bytes inside a 3-byte descriptor was "
				      "accepted\n");
				ok = false;
			}
		}
	}

	/* --- the field map, against a layout already known ---------------
	 *
	 * `usb_hid.c` reads a boot mouse as: buttons in bit 0 of byte 0, X in
	 * byte 1, Y in byte 2. That driver has been doing it against real
	 * hardware, so those offsets are an answer from outside this file --
	 * buttons at bit 0, X at bit 8, Y at bit 16.
	 */
	if (!hid_report_parse(boot_mouse, sizeof(boot_mouse), &info)) {
		kputs("  hidrep: the boot mouse descriptor was refused on the "
		      "second pass\n");
		ok = false;
	} else if (!info.fields_usable) {
		kprintf("  hidrep: the field map was withheld for a plain "
			"boot mouse: %s\n",
			info.fields_unusable ? info.fields_unusable
					     : "no reason given");
		ok = false;
	} else {
		struct hid_mouse_layout m;

		/* Three buttons, one padding field, two axes. */
		if (info.field_count != 6) {
			kprintf("  hidrep: the mouse came to %u fields, "
				"expected 6 -- three buttons, one padding, "
				"two axes\n", info.field_count);
			ok = false;
		}

		if (!hid_report_mouse_layout(&info, &m)) {
			kputs("  hidrep: a boot mouse descriptor did not "
			      "yield a mouse layout\n");
			ok = false;
		} else {
			if (m.buttons_offset != 0 || m.buttons_count != 3) {
				kprintf("  hidrep: buttons at bit %u count "
					"%u, expected bit 0 count 3 -- "
					"usb_hid.c reads them from bit 0 of "
					"byte 0\n", m.buttons_offset,
					m.buttons_count);
				ok = false;
			}

			if (m.x_offset != 8 || m.x_size != 8) {
				kprintf("  hidrep: X at bit %u size %u, "
					"expected bit 8 size 8 -- that is "
					"byte 1, which is where usb_hid.c "
					"reads it\n", m.x_offset, m.x_size);
				ok = false;
			}

			if (m.y_offset != 16 || m.y_size != 8) {
				kprintf("  hidrep: Y at bit %u size %u, "
					"expected bit 16 size 8\n",
					m.y_offset, m.y_size);
				ok = false;
			}

			if (m.have_wheel) {
				kputs("  hidrep: a wheel was found on a "
				      "three-button boot mouse that has "
				      "none\n");
				ok = false;
			}

			if (m.report_bytes != 3) {
				kprintf("  hidrep: the mouse report is %u "
					"bytes, expected 3\n",
					m.report_bytes);
				ok = false;
			}
		}

		/* The padding field must be *present* and marked, not dropped.
		 * Dropping it would leave the map correct only because the
		 * axes' offsets were computed separately. */
		{
			unsigned i, constants = 0;

			for (i = 0; i < info.field_count; i++)
				if (info.fields[i].constant)
					constants++;

			if (constants != 1) {
				kprintf("  hidrep: %u padding fields, "
					"expected 1 -- the five bits after "
					"the buttons are real estate even "
					"though they mean nothing\n",
					constants);
				ok = false;
			}
		}

		/* The axes are relative on a mouse and the buttons are not.
		 * Reading a relative axis as absolute would warp the pointer
		 * rather than move it. */
		{
			unsigned i;

			for (i = 0; i < info.field_count; i++) {
				const struct hid_field *f = &info.fields[i];

				if (f->usage_page == HID_PAGE_GENERIC_DESKTOP &&
				    f->usage == HID_USAGE_X && !f->relative) {
					kputs("  hidrep: X read as absolute; "
					      "a mouse reports deltas, and an "
					      "absolute reading warps the "
					      "pointer instead of moving "
					      "it\n");
					ok = false;
				}

				if (f->usage_page == HID_PAGE_BUTTON &&
				    f->relative) {
					kputs("  hidrep: a button read as "
					      "relative\n");
					ok = false;
				}
			}
		}
	}

	/* --- the usages must not leak past their Main item ---------------
	 *
	 * Usage(Mouse) names the collection, and a Collection is a Main item,
	 * so that usage must be gone before the next Input asks for one.
	 *
	 * **The boot mouse above cannot show this, and the first version of
	 * this test wrongly claimed it could.** Its buttons come from a Usage
	 * Minimum/Maximum range, and `usage_for` consults the range before the
	 * list -- so a leaked Usage(Mouse) is sitting there being ignored, and
	 * a walker that clears local state only after Input gives the same
	 * answer. Breaking the clear on purpose left this test green.
	 *
	 * The descriptor below has no range. Its Input takes two usages from
	 * the list, so a leaked Usage(Mouse) becomes field 0, X slides into
	 * field 1, and Y falls off the end.
	 */
	{
		static const u8 leaky[] = {
			0x05, 0x01,	/* Usage Page (Generic Desktop) */
			0x09, 0x02,	/* Usage (Mouse) -- names the next */
			0xA1, 0x01,	/* Collection (Application)     */
			0x09, 0x30,	/*   Usage (X)                  */
			0x09, 0x31,	/*   Usage (Y)                  */
			0x75, 0x08,	/*   Report Size (8)            */
			0x95, 0x02,	/*   Report Count (2)           */
			0x81, 0x06,	/*   Input (Data,Var,Rel)       */
			0xC0		/* End Collection               */
		};

		if (!hid_report_parse(leaky, sizeof(leaky), &info)) {
			kputs("  hidrep: a two-axis descriptor was refused\n");
			ok = false;
		} else if (!info.fields_usable) {
			kprintf("  hidrep: a two-axis descriptor was withheld: "
				"%s\n", info.fields_unusable
					? info.fields_unusable : "no reason");
			ok = false;
		} else if (info.field_count != 2) {
			kprintf("  hidrep: %u fields, expected 2\n",
				info.field_count);
			ok = false;
		} else if (info.fields[0].usage != HID_USAGE_X ||
			   info.fields[1].usage != HID_USAGE_Y) {
			kprintf("  hidrep: the two axes came back as usages "
				"%02x and %02x, expected 30 and 31 -- "
				"Usage(Mouse) named the collection and "
				"survived into the Input item\n",
				info.fields[0].usage, info.fields[1].usage);
			ok = false;
		}
	}

	/* And the boot mouse's own usages, which its range does cover. */
	{
		unsigned i;

		if (hid_report_parse(boot_mouse, sizeof(boot_mouse), &info) &&
		    info.fields_usable) {
			for (i = 0; i < info.field_count; i++) {
				const struct hid_field *f = &info.fields[i];

				if (f->usage_page == HID_PAGE_GENERIC_DESKTOP &&
				    f->usage == HID_USAGE_MOUSE) {
					kprintf("  hidrep: field %u came back "
						"as Usage(Mouse), which names "
						"the collection -- a local "
						"usage survived its Main "
						"item\n", i);
					ok = false;
				}
			}

			/* And the buttons must be 1, 2, 3 in order, from the
			 * Usage Minimum/Maximum range rather than from a
			 * list. */
			if (info.field_count >= 3) {
				if (info.fields[0].usage != 1 ||
				    info.fields[1].usage != 2 ||
				    info.fields[2].usage != 3) {
					kprintf("  hidrep: the buttons are "
						"usages %u %u %u, expected 1 "
						"2 3 from the minimum-maximum "
						"range\n",
						info.fields[0].usage,
						info.fields[1].usage,
						info.fields[2].usage);
					ok = false;
				}
			}
		}
	}

	/* --- a feature this pass does not implement withholds the map ----
	 *
	 * **This descriptor has to be otherwise valid, and the first version
	 * was not.** It had an Input item with no usages at all, so it was
	 * withheld for "fewer usages than fields" whether or not Push was
	 * handled -- and deleting the Push handling on purpose left the test
	 * green. It has proper usages now, so the only thing that can withhold
	 * it is the Push.
	 */
	{
		static const u8 with_push[] = {
			0x05, 0x01,		/* Usage Page (Generic Desktop) */
			0xA1, 0x01,		/* Collection (Application) */
			0xA4,			/*   Push                   */
			0x09, 0x30,		/*   Usage (X)              */
			0x09, 0x31,		/*   Usage (Y)              */
			0x75, 0x08,		/*   Report Size (8)        */
			0x95, 0x02,		/*   Report Count (2)       */
			0x81, 0x06,		/*   Input                  */
			0xB4,			/*   Pop                    */
			0xC0
		};

		if (!hid_report_parse(with_push, sizeof(with_push), &info)) {
			kputs("  hidrep: a descriptor using Push was refused "
			      "outright; it parses, its map is just not "
			      "trustworthy\n");
			ok = false;
		} else if (info.fields_usable) {
			kputs("  hidrep: a descriptor using Push produced a "
			      "field map, and every offset after the Push is "
			      "built on state this walker does not track\n");
			ok = false;
		} else if (!info.fields_unusable) {
			kputs("  hidrep: the map was withheld with no reason "
			      "attached, which a boot log cannot act on\n");
			ok = false;
		}
	}

	/* --- an array Input item is a keyboard, not a pointer ------------- */
	{
		static const u8 keyboard_ish[] = {
			0xA1, 0x01,
			0x75, 0x08,
			0x95, 0x06,
			0x81, 0x00,		/* Input (Data, Array) */
			0xC0
		};

		if (!hid_report_parse(keyboard_ish, sizeof(keyboard_ish),
				      &info)) {
			kputs("  hidrep: an array Input item was refused "
			      "outright\n");
			ok = false;
		} else if (info.fields_usable) {
			kputs("  hidrep: an array Input item produced a "
			      "field map; an array carries a list of active "
			      "usages, not one value per field\n");
			ok = false;
		}
	}

	/* --- two reports, and the offsets that belong to neither ---------
	 *
	 * **This guard was unreachable by every test that existed.** The real
	 * controller in `sdp.c` has forty-four report ids and is refused, but
	 * one step earlier, by the usages check -- deleting this guard left
	 * that test green. The single-report-id case in `bt_mouse.c` does not
	 * reach it either, because one is not more than one.
	 *
	 * So: two reports, each otherwise complete and well-formed. Nothing
	 * else here objects to them. What makes a layout impossible is that
	 * `bit_offset` accumulates across every Input item in the descriptor,
	 * so the second report's fields are placed after the first's rather
	 * than at zero -- and every offset in the map then belongs to no
	 * report that the device actually sends.
	 */
	{
		static const u8 two_reports[] = {
			0x05, 0x01, 0x09, 0x02,
			0xA1, 0x01,
			0x85, 0x01,		/*   Report ID 1          */
			0x09, 0x30, 0x09, 0x31,	/*   Usage X, Usage Y     */
			0x15, 0x81, 0x25, 0x7F,
			0x75, 0x08, 0x95, 0x02,
			0x81, 0x06,		/*   Input: bits 0..15    */
			0x85, 0x02,		/*   Report ID 2          */
			0x09, 0x30, 0x09, 0x31,
			0x15, 0x81, 0x25, 0x7F,
			0x75, 0x08, 0x95, 0x02,
			0x81, 0x06,		/*   Input: bits 16..31   */
			0xC0
		};
		struct hid_mouse_layout m;

		if (!hid_report_parse(two_reports, sizeof(two_reports),
				      &info)) {
			kputs("  hidrep: a two-report descriptor was "
			      "refused outright\n");
			ok = false;
		} else if (info.report_id_count != 2) {
			kprintf("  hidrep: %u report ids counted, expected "
				"2\n", info.report_id_count);
			ok = false;
		} else if (!info.fields_usable) {
			kprintf("  hidrep: the two-report descriptor was "
				"withheld for another reason (%s), so this "
				"case still does not reach the guard it is "
				"about\n",
				info.fields_unusable ? info.fields_unusable
						     : "none given");
			ok = false;
		} else if (hid_report_mouse_layout(&info, &m)) {
			kprintf("  hidrep: two reports produced a mouse "
				"layout with X at bit %u -- the second "
				"report's fields sit after the first's, so "
				"no offset in it matches a report the device "
				"sends\n", m.x_offset);
			ok = false;
		}
	}

	/* --- buttons with no axes are not a mouse ------------------------- */
	{
		static const u8 buttons_only[] = {
			0xA1, 0x01,
			0x05, 0x09,		/* Usage Page (Button) */
			0x19, 0x01,
			0x29, 0x04,
			0x95, 0x04,
			0x75, 0x01,
			0x81, 0x02,
			0x95, 0x01,
			0x75, 0x04,
			0x81, 0x01,		/* padding */
			0xC0
		};
		struct hid_mouse_layout m;

		if (!hid_report_parse(buttons_only, sizeof(buttons_only),
				      &info)) {
			kputs("  hidrep: a buttons-only descriptor was "
			      "refused\n");
			ok = false;
		} else if (hid_report_mouse_layout(&info, &m)) {
			kputs("  hidrep: four buttons and no axes was read "
			      "as a mouse, and driving it as one posts "
			      "motion that does not exist\n");
			ok = false;
		}
	}

	/* --- pulling a value out of a field ------------------------------ */
	{
		i32 v;
		/* A boot mouse report: left button down, x = +5, y = -5. */
		static const u8 rep[3] = { 0x01, 0x05, 0xFB };

		/* One bit, the first button. */
		if (!hid_field_extract(rep, 3, 0, 1, false, &v) || v != 1) {
			kprintf("  hidrep: button 1 read as %d, expected 1\n",
				v);
			ok = false;
		}

		/* The second and third, which are up. If these read as down,
		 * the bits are being taken from the top of the byte. */
		if (!hid_field_extract(rep, 3, 1, 1, false, &v) || v != 0) {
			kprintf("  hidrep: button 2 read as %d, expected 0 -- "
				"bits are packed least-significant first\n",
				v);
			ok = false;
		}

		/* Eight bits, signed, byte-aligned. This is exactly what
		 * usb_hid.c does with (i32)(i8)report[2], so the two must
		 * agree on the same byte. */
		if (!hid_field_extract(rep, 3, 16, 8, true, &v) || v != -5) {
			kprintf("  hidrep: y read as %d, expected -5 -- "
				"usb_hid.c casts the same byte through i8 and "
				"gets -5\n", v);
			ok = false;
		}

		if (v != (i32)(i8)rep[2]) {
			kprintf("  hidrep: y read as %d and usb_hid.c's cast "
				"gives %d from the same byte\n", v,
				(i32)(i8)rep[2]);
			ok = false;
		}

		/* The same bits unsigned. These must differ, or the signed
		 * test above proves nothing about sign extension. */
		if (!hid_field_extract(rep, 3, 16, 8, false, &v) || v != 251) {
			kprintf("  hidrep: y read unsigned as %d, expected "
				"251\n", v);
			ok = false;
		}

		/* A field that runs off the end. */
		if (hid_field_extract(rep, 3, 16, 16, true, &v)) {
			kputs("  hidrep: a 16-bit field at bit 16 of a "
			      "3-byte report was read; it ends eight bits "
			      "past the end\n");
			ok = false;
		}

		/* And one that ends exactly at the end, which must work --
		 * an off-by-one in the bounds check shows up here and
		 * nowhere else. */
		if (!hid_field_extract(rep, 3, 16, 8, true, &v)) {
			kputs("  hidrep: a field ending exactly at the end "
			      "of the report was refused\n");
			ok = false;
		}

		if (hid_field_extract(rep, 3, 0, 0, false, &v) ||
		    hid_field_extract(rep, 3, 0, 33, false, &v)) {
			kputs("  hidrep: a zero-bit or 33-bit field was "
			      "accepted\n");
			ok = false;
		}
	}

	/* --- a field that is not byte-aligned, and is not eight bits -----
	 *
	 * Twelve bits starting at bit 4, which is how a high-resolution mouse
	 * packs two axes into three bytes. Every shortcut that works on a boot
	 * mouse fails here: it crosses a byte boundary, it is not a whole
	 * number of bytes, and its sign bit is bit 11.
	 */
	{
		i32 v;
		/* x = 12 bits at offset 4 = 0xFFB, which is -5.
		 * byte 0 = 0xB0 (low nibble of x in the high nibble),
		 * byte 1 = 0xFF. */
		static const u8 packed[3] = { 0xB0, 0xFF, 0x00 };

		if (!hid_field_extract(packed, 3, 4, 12, true, &v) ||
		    v != -5) {
			kprintf("  hidrep: a 12-bit field at bit 4 read as "
				"%d, expected -5 -- it crosses a byte and its "
				"sign bit is bit 11, not bit 7\n", v);
			ok = false;
		}

		/* Unsigned, the same bits are 4091. A reader that fails to
		 * sign-extend from twelve bits returns exactly this, so the
		 * two must differ or the test above cannot tell them apart. */
		if (!hid_field_extract(packed, 3, 4, 12, false, &v) ||
		    v != 4091) {
			kprintf("  hidrep: the same 12 bits unsigned read as "
				"%d, expected 4091\n", v);
			ok = false;
		}
	}

	/* --- a positive value whose bit 7 is set -------------------------
	 *
	 * **The 0xFFB above cannot catch sign extension from the wrong bit**,
	 * and the first version of this block stopped there. Testing a reader
	 * that takes bit 7 as the sign of a twelve-bit field: 0xFFB has bit 7
	 * set *and* bit 11 set, so both the right rule and the wrong one call
	 * it negative and both produce -5. Breaking the sign bit on purpose
	 * left the test green.
	 *
	 * 0x080 is 128 -- positive in twelve bits, with bit 7 set. The right
	 * rule leaves it at 128; taking bit 7 as the sign gives -3968.
	 */
	{
		i32 v;
		static const u8 positive[3] = { 0x00, 0x08, 0x00 };

		if (!hid_field_extract(positive, 3, 4, 12, true, &v) ||
		    v != 128) {
			kprintf("  hidrep: a 12-bit +128 read as %d -- its "
				"bit 7 is set and its sign bit, bit 11, is "
				"not\n", v);
			ok = false;
		}
	}

	/* --- the whole chain, descriptor to values -----------------------
	 *
	 * Parse the boot mouse, build its layout, and decode a report through
	 * it. The answer is checked against `usb_hid.c`'s own arithmetic on
	 * the same three bytes, so this asserts that a descriptor-driven read
	 * and a hardcoded one agree.
	 */
	if (hid_report_parse(boot_mouse, sizeof(boot_mouse), &info) &&
	    info.fields_usable) {
		struct hid_mouse_layout m;
		struct hid_mouse_state s;
		/* Buttons 1 and 3 down, x = +5, y = -5. */
		static const u8 rep[3] = { 0x05, 0x05, 0xFB };

		if (!hid_report_mouse_layout(&info, &m)) {
			kputs("  hidrep: no mouse layout on the way to "
			      "decoding\n");
			ok = false;
		} else if (!hid_mouse_decode(&m, rep, 3, &s)) {
			kputs("  hidrep: a three-byte report was refused by "
			      "a layout that says three bytes\n");
			ok = false;
		} else {
			if (s.buttons != 0x05) {
				kprintf("  hidrep: buttons decoded as %02x, "
					"expected 05 -- one and three down\n",
					s.buttons);
				ok = false;
			}

			if (s.x != 5 || s.y != -5) {
				kprintf("  hidrep: motion decoded as x=%d "
					"y=%d, expected 5 and -5\n", s.x, s.y);
				ok = false;
			}

			/* Against the driver that has been doing this on real
			 * hardware, on the same bytes. */
			if (s.x != (i32)(i8)rep[1] ||
			    s.y != (i32)(i8)rep[2]) {
				kprintf("  hidrep: descriptor-driven decode "
					"gives x=%d y=%d, usb_hid.c's casts "
					"give x=%d y=%d\n", s.x, s.y,
					(i32)(i8)rep[1], (i32)(i8)rep[2]);
				ok = false;
			}

			if (s.wheel) {
				kprintf("  hidrep: a wheel value of %d came "
					"from a mouse with no wheel\n",
					s.wheel);
				ok = false;
			}
		}

		/* A report shorter than the layout expects is a device
		 * disagreeing with its own descriptor. */
		if (hid_report_mouse_layout(&info, &m) &&
		    hid_mouse_decode(&m, rep, 2, &s)) {
			kputs("  hidrep: a two-byte report was decoded "
			      "through a three-byte layout\n");
			ok = false;
		}
	}

	/* --- a short report the field bounds cannot catch ----------------
	 *
	 * **The check above is not the one that matters, and the first
	 * version of this test did not notice.** On the plain boot mouse, a
	 * two-byte report fails anyway because Y's own bounds check runs off
	 * the end -- so deleting `len < report_bytes` from `hid_mouse_decode`
	 * left the test green.
	 *
	 * The descriptor below ends with eight bits of padding, so its report
	 * is four bytes while every *named* field fits in three. A three-byte
	 * report then passes every field bound and is still a device sending
	 * less than it said it would.
	 */
	{
		static const u8 trailing_pad[] = {
			0x05, 0x01,	/* Usage Page (Generic Desktop) */
			0x09, 0x02,	/* Usage (Mouse)                */
			0xA1, 0x01,	/* Collection (Application)     */
			0x05, 0x09,	/*   Usage Page (Button)        */
			0x19, 0x01,	/*   Usage Minimum (1)          */
			0x29, 0x03,	/*   Usage Maximum (3)          */
			0x95, 0x03,	/*   Report Count (3)           */
			0x75, 0x01,	/*   Report Size (1)            */
			0x81, 0x02,	/*   Input -- bits 0..2         */
			0x95, 0x01,	/*   Report Count (1)           */
			0x75, 0x05,	/*   Report Size (5)            */
			0x81, 0x01,	/*   Input (Const) -- bits 3..7 */
			0x05, 0x01,	/*   Usage Page (Generic Desktop) */
			0x09, 0x30,	/*   Usage (X)                  */
			0x09, 0x31,	/*   Usage (Y)                  */
			0x75, 0x08,	/*   Report Size (8)            */
			0x95, 0x02,	/*   Report Count (2)           */
			0x81, 0x06,	/*   Input -- bits 8..23        */
			0x95, 0x01,	/*   Report Count (1)           */
			0x75, 0x08,	/*   Report Size (8)            */
			0x81, 0x01,	/*   Input (Const) -- bits 24..31 */
			0xC0		/* End Collection               */
		};
		struct hid_mouse_layout m;
		struct hid_mouse_state s;
		static const u8 three[3] = { 0x01, 0x05, 0xFB };

		if (!hid_report_parse(trailing_pad, sizeof(trailing_pad),
				      &info) || !info.fields_usable) {
			kputs("  hidrep: a descriptor with trailing padding "
			      "did not parse into a usable map\n");
			ok = false;
		} else if (!hid_report_mouse_layout(&info, &m)) {
			kputs("  hidrep: no mouse layout from the "
			      "trailing-padding descriptor\n");
			ok = false;
		} else if (m.report_bytes != 4) {
			kprintf("  hidrep: that report is %u bytes, expected "
				"4 -- three of fields and one of padding\n",
				m.report_bytes);
			ok = false;
		} else if (hid_mouse_decode(&m, three, 3, &s)) {
			kputs("  hidrep: a three-byte report was decoded "
			      "through a four-byte layout -- every named "
			      "field fits in three, so only the report "
			      "length catches this\n");
			ok = false;
		}
	}

	/* --- the boot layout and the parser must agree -------------------
	 *
	 * `hid_mouse_boot_layout` is a hardcoded constant and
	 * `hid_report_mouse_layout` derives one from bytes. They describe the
	 * same thing, so they must come out the same -- and if they ever
	 * disagree, one of them is wrong and this says so rather than letting
	 * a driver pick whichever it happened to call.
	 */
	if (hid_report_parse(boot_mouse, sizeof(boot_mouse), &info) &&
	    info.fields_usable) {
		struct hid_mouse_layout parsed, fixed;

		if (!hid_report_mouse_layout(&info, &parsed)) {
			kputs("  hidrep: the boot descriptor yielded no "
			      "layout to compare against\n");
			ok = false;
		} else {
			hid_mouse_boot_layout(&fixed, parsed.have_wheel);

			if (fixed.buttons_offset != parsed.buttons_offset ||
			    fixed.buttons_count != parsed.buttons_count ||
			    fixed.x_offset != parsed.x_offset ||
			    fixed.x_size != parsed.x_size ||
			    fixed.y_offset != parsed.y_offset ||
			    fixed.y_size != parsed.y_size ||
			    fixed.report_bytes != parsed.report_bytes) {
				kprintf("  hidrep: the hardcoded boot layout "
					"and the parsed one disagree -- fixed "
					"says buttons %u@%u x %u@%u y %u@%u "
					"in %u bytes, parsed says %u@%u "
					"%u@%u %u@%u in %u\n",
					fixed.buttons_count,
					fixed.buttons_offset, fixed.x_size,
					fixed.x_offset, fixed.y_size,
					fixed.y_offset, fixed.report_bytes,
					parsed.buttons_count,
					parsed.buttons_offset, parsed.x_size,
					parsed.x_offset, parsed.y_size,
					parsed.y_offset, parsed.report_bytes);
				ok = false;
			}
		}
	}

	/* --- a descriptor nobody here wrote ------------------------------
	 *
	 * The one fixture in this file that can disagree with the parser's
	 * author. See the comment above `real_mouse`.
	 */
	if (!hid_report_parse(real_mouse, sizeof(real_mouse), &info)) {
		kputs("  hidrep: the real mouse's descriptor was refused\n");
		ok = false;
	} else if (!info.fields_usable) {
		kprintf("  hidrep: the real mouse's field map was withheld: "
			"%s\n", info.fields_unusable ? info.fields_unusable
						     : "no reason given");
		ok = false;
	} else {
		struct hid_mouse_layout m;

		if (info.uses_report_id) {
			kputs("  hidrep: the real mouse was read as using "
			      "report ids and its descriptor has none\n");
			ok = false;
		}

		/* 5 buttons + 3 padding + two 16-bit axes + wheel + pan. */
		if (info.input_bits != 56 || info.input_bytes != 7) {
			kprintf("  hidrep: the real mouse's report came to %u "
				"bits (%u bytes), hand-decoded as 56 and 7\n",
				info.input_bits, info.input_bytes);
			ok = false;
		}

		if (info.field_count != 10) {
			kprintf("  hidrep: %u fields, hand-decoded as 10 -- "
				"five buttons, one padding, two axes, a wheel "
				"and a pan\n", info.field_count);
			ok = false;
		}

		/* Depth goes up and down and up again here, which a walker
		 * that only counts opens gets wrong. */
		if (info.collection_depth_max != 2) {
			kprintf("  hidrep: collections nested %u deep, "
				"expected 2 -- three Physical collections "
				"open and close inside one Application\n",
				info.collection_depth_max);
			ok = false;
		}

		if (!hid_report_mouse_layout(&info, &m)) {
			kputs("  hidrep: a real mouse's descriptor did not "
			      "yield a mouse layout\n");
			ok = false;
		} else {
			if (m.buttons_offset != 0 || m.buttons_count != 5) {
				kprintf("  hidrep: buttons at bit %u count "
					"%u, expected bit 0 count 5\n",
					m.buttons_offset, m.buttons_count);
				ok = false;
			}

			/* **Sixteen bits, not eight.** Every synthetic test
			 * here used an eight-bit axis, so this is the first
			 * one that would catch a size assumed rather than
			 * read. */
			if (m.x_offset != 8 || m.x_size != 16) {
				kprintf("  hidrep: X at bit %u size %u, "
					"expected bit 8 size 16 -- this mouse "
					"reports sixteen-bit axes\n",
					m.x_offset, m.x_size);
				ok = false;
			}

			if (m.y_offset != 24 || m.y_size != 16) {
				kprintf("  hidrep: Y at bit %u size %u, "
					"expected bit 24 size 16\n",
					m.y_offset, m.y_size);
				ok = false;
			}

			if (!m.have_wheel || m.wheel_offset != 40 ||
			    m.wheel_size != 8) {
				kprintf("  hidrep: wheel present=%u at bit %u "
					"size %u, expected present at bit 40 "
					"size 8\n", (unsigned)m.have_wheel,
					m.wheel_offset, m.wheel_size);
				ok = false;
			}

			if (m.report_bytes != 7) {
				kprintf("  hidrep: the real mouse's report is "
					"%u bytes, expected 7\n",
					m.report_bytes);
				ok = false;
			}

			/* --- and decode a report through it ---
			 *
			 * Buttons 1 and 3 down, X = +300, Y = -300, wheel -1.
			 * X and Y are little-endian sixteen-bit values, which
			 * is the first time anything here decodes a field
			 * wider than a byte from real geometry.
			 */
			{
				struct hid_mouse_state s;
				static const u8 rep[7] = {
					0x05,		/* buttons 1 and 3 */
					0x2C, 0x01,	/* x = +300 */
					0xD4, 0xFE,	/* y = -300 */
					0xFF,		/* wheel = -1 */
					0x00		/* pan = 0 */
				};

				if (!hid_mouse_decode(&m, rep, 7, &s)) {
					kputs("  hidrep: a seven-byte report "
					      "was refused by a seven-byte "
					      "layout\n");
					ok = false;
				} else if (s.buttons != 0x05 || s.x != 300 ||
					   s.y != -300 || s.wheel != -1) {
					kprintf("  hidrep: the real mouse's "
						"report decoded as buttons "
						"%02x x=%d y=%d wheel=%d, "
						"expected 05, 300, -300, "
						"-1\n", s.buttons, s.x, s.y,
						s.wheel);
					ok = false;
				}
			}
		}

		/* The Consumer-page pan must not have been taken for an
		 * axis. It is the last field, and a layout that grabbed it
		 * would have put something at bit 48. */
		{
			unsigned i, consumer = 0;

			for (i = 0; i < info.field_count; i++)
				if (info.fields[i].usage_page == 0x0C)
					consumer++;

			if (consumer != 1) {
				kprintf("  hidrep: %u fields on the Consumer "
					"page, expected 1 (AC Pan)\n",
					consumer);
				ok = false;
			}
		}
	}

	/* --- an empty descriptor is not a failure, it is an empty one ----- */
	if (!hid_report_parse(boot_mouse, 0, &info)) {
		kputs("  hidrep: a zero-length descriptor was refused; it has "
		      "no items, which is a different fact from malformed\n");
		ok = false;
	} else if (info.items || info.input_bits) {
		kputs("  hidrep: a zero-length descriptor produced items\n");
		ok = false;
	}

	return ok;
}
