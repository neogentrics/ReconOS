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

bool hid_report_parse(const u8 *desc, u32 len, struct hid_report_info *info)
{
	u32 at = 0;
	u32 report_size = 0;
	u32 report_count = 0;
	unsigned depth = 0;

	kmemset(info, 0, sizeof(*info));

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

		if (type == HID_ITEM_GLOBAL) {
			switch (tag) {
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
			case HID_MAIN_INPUT:
				/* Size times count, in bits, and the padding
				 * items count too: an Input item marked
				 * constant is filler with no usage, and it
				 * still occupies its bits in the report. A
				 * walker that skipped them would compute a
				 * report shorter than the device sends. */
				info->input_bits += report_size * report_count;
				break;

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

bool hid_report_self_test(void)
{
	struct hid_report_info info;
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
		struct hid_report_info after;

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
		struct hid_report_info after;

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
