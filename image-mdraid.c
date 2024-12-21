/*
 * Copyright (c) 2025 Tomas Mudrunka <harviecz@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*

MDRAID Superblock generator
This should create valid mdraid superblock for raid1 with 1 device (more devices can be added once mounted).
It is still very basic, but following seems to be working:

mdadm --examine test.img
losetup /dev/loop1 test.img
mdadm --assemble md /dev/loop1

Some docs:
https://raid.wiki.kernel.org/index.php/RAID_superblock_formats#Sub-versions_of_the_version-1_superblock
https://docs.huihoo.com/doxygen/linux/kernel/3.7/md__p_8h_source.html

*/

#include <confuse.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/raid/md_p.h>

#include "genimage.h"

#define DATA_OFFSET_SECTORS	(2048)
#define DATA_OFFSET_BYTES	(DATA_OFFSET_SECTORS*512)
#define MDRAID_MAGIC		0xa92b4efc
#define MDRAID_ALIGN_BYTES	8*512	//(should be divisible by 8 sectors to keep 4kB alignment)


/*
static void random_uuid(__u8 *buf)
{
	__u32 r[4];
	for (int i = 0; i < 4; i++)
		r[i] = random();
	memcpy(buf, r, 16);
}
*/

static unsigned int calc_sb_1_csum(struct mdp_superblock_1 * sb)
{
	unsigned int disk_csum, csum;
	unsigned long long newcsum;
	int size = sizeof(*sb) + __le32_to_cpu(sb->max_dev)*2;
	unsigned int *isuper = (unsigned int*)sb;

	/* make sure I can count... (needs include cstddef) */
	/*
	if (offsetof(struct mdp_superblock_1,data_offset) != 128 ||
	    offsetof(struct mdp_superblock_1, utime) != 192 ||
	    sizeof(struct mdp_superblock_1) != 256) {
		fprintf(stderr, "WARNING - superblock isn't sized correctly\n");
	}
	*/

	disk_csum = sb->sb_csum;
	sb->sb_csum = 0;
	newcsum = 0;
	for (; size>=4; size -= 4 ) {
		newcsum += __le32_to_cpu(*isuper);
		isuper++;
	}

	if (size == 2)
		newcsum += __le16_to_cpu(*(unsigned short*) isuper);

	csum = (newcsum & 0xffffffff) + (newcsum >> 32);
	sb->sb_csum = disk_csum;
	return __cpu_to_le32(csum);
}

static int mdraid_generate(struct image *image) {
	struct image *img_in = image->handler_priv;

	int max_devices = 1;

	char *name = cfg_getstr(image->imagesec, "label");

	size_t superblock_size = sizeof(struct mdp_superblock_1) + max_devices*2;
	struct mdp_superblock_1 *sb = xzalloc(superblock_size);

	/* constant array information - 128 bytes */
	sb->magic = MDRAID_MAGIC;	/* MD_SB_MAGIC: 0xa92b4efc - little endian */
	sb->major_version = 1;	/* 1 */
	sb->feature_map = 0; //MD_FEATURE_BITMAP_OFFSET;	/* bit 0 set if 'bitmap_offset' is meaningful */ //TODO: internal bitmap bit is ignored, unless there is correct bitmap with BITMAP_MAGIC in place
	sb->pad0 = 0;		/* always set to 0 when writing */

	char *raid_uuid = cfg_getstr(image->imagesec, "raid-uuid");
	if (!raid_uuid) raid_uuid = uuid_random();
	uuid_parse(raid_uuid, sb->set_uuid);  /* user-space generated. U8[16]*/

	strncpy(sb->set_name, name, 32); sb->set_name[31] = 0;	/* set and interpreted by user-space. CHAR[32] */

	long int timestamp = cfg_getint(image->imagesec, "timestamp");
	if (timestamp >= 0) {
		sb->ctime = timestamp & 0xffffffffff;
	} else {
		sb->ctime = time(NULL) & 0xffffffffff;	/* lo 40 bits are seconds, top 24 are microseconds or 0*/
	}

	sb->level = 1;		/* -4 (multipath), -1 (linear), 0,1,4,5 */
	//sb->layout = 2;		/* only for raid5 and raid10 currently */
	sb->size = (image->size - DATA_OFFSET_BYTES)/512;	/* used size of component devices, in 512byte sectors */

	sb->chunksize = 0;		/* in 512byte sectors - not used in raid 1 */
	sb->raid_disks = 1;
	sb->bitmap_offset = 8;	/* sectors after start of superblock that bitmap starts
					 * NOTE: signed, so bitmap can be before superblock
					 * only meaningful of feature_map[0] is set.
					 */

	/* constant this-device information - 64 bytes */
	sb->data_offset = DATA_OFFSET_SECTORS;	/* sector start of data, often 0 */
	sb->data_size = image->size / 512 - sb->data_offset;	/* sectors in this device that can be used for data */
	sb->super_offset = 8;	/* sector start of this superblock */

	sb->dev_number = 0;	/* permanent identifier of this  device - not role in raid */
	sb->cnt_corrected_read = 0; /* number of read errors that were corrected by re-writing */

	char *disk_uuid = cfg_getstr(image->imagesec, "disk-uuid");
	if (!disk_uuid) disk_uuid = uuid_random();
	uuid_parse(disk_uuid, sb->device_uuid);  /* user-space setable, ignored by kernel U8[16] */

	sb->devflags = 0;	/* per-device flags.  Only two defined...*/
		//#define	WriteMostly1	1	/* mask for writemostly flag in above */
		//#define	FailFast1	2	/* Should avoid retries and fixups and just fail */

		/* Bad block log.  If there are any bad blocks the feature flag is set.
		* If offset and size are non-zero, that space is reserved and available
		*/
	sb->bblog_shift = 9;    /* shift from sectors to block size */ //TODO: not sure why this is 9
	sb->bblog_size = 8; /* number of sectors reserved for list */
	sb->bblog_offset = 16;   /* sector offset from superblock to bblog,
			* signed - not unsigned */

	/* array state information - 64 bytes */
	sb->utime = 0;		/* 40 bits second, 24 bits microseconds */
	sb->events = 0;		/* incremented when superblock updated */
	sb->resync_offset = 0;	/* data before this offset (from data_offset) known to be in sync */
	sb->max_dev = max_devices; /* size of devs[] array to consider */
	//__u8	pad3[64-32];	/* set to 0 when writing */

	/* device state information. Indexed by dev_number.
	 * 2 bytes per device
	 * Note there are no per-device state flags. State information is rolled
	 * into the 'roles' value.  If a device is spare or faulty, then it doesn't
	 * have a meaningful role.
	 */
	//__le16	dev_roles[];	/* role in array, or 0xffff for a spare, or 0xfffe for faulty */


	//Calculate checksum
	sb->sb_csum = calc_sb_1_csum(sb);


	//construct image file
	int ret;
	ret = prepare_image(image, image->size);
	if (ret) return ret;
	ret = insert_data(image, sb, imageoutfile(image), superblock_size, 8*512);
	if (ret) return ret;
	if (img_in) {
		ret = insert_image(image, img_in, img_in->size, DATA_OFFSET_BYTES, 0);
		if (ret) return ret;
	}

	free(sb);

	return 0;
}

static int mdraid_setup(struct image *image, cfg_t *cfg) {
	srand(time(NULL)); //TODO: Should we seed UUID in more unique way?

	int raid_level = cfg_getint(image->imagesec, "level");

	if (raid_level != 1) {
		image_error(image, "MDRAID Currently only supporting raid level 1 (mirror)!\n");
		return 1;
	}

	struct image *img_in = NULL;
	struct partition *part;
	list_for_each_entry(part, &image->partitions, list) {
		if (strcmp(part->name, "data")) {
			image_info(image, "MDRAID partition has to be called 'data' instead of '%s'\n", part->name);
		} else {
			if (img_in) {
				image_error(image, "MDRAID cannot contain more than one data partition!\n");
				return 2;
			}
			if (part->image) {
				image_info(image, "MDRAID using data from [%s]: %s\n", part->name, part->image);
				img_in = image_get(part->image);
				if (image->size == 0)
					image->size = roundup(img_in->size + DATA_OFFSET_BYTES, MDRAID_ALIGN_BYTES);
				if (image->size < (img_in->size + DATA_OFFSET_BYTES)) {
					image_error(image, "MDRAID image too small to fit %s\n", part->image);
					return 3;
				}
			}
		}
	}
	if (!img_in) {
		image_info(image, "MDRAID is created without data.\n");
	}
	image->handler_priv = img_in;


	//Make sure size is aligned
	if (image->size != roundup(image->size, MDRAID_ALIGN_BYTES)) {
		image_error(image, "MDRAID image size has to be aligned to %d bytes!\n", MDRAID_ALIGN_BYTES);
		return 4;
	}

	return 0;
}

static cfg_opt_t mdraid_opts[] = {
	CFG_STR("label", "localhost:42", CFGF_NONE),
	CFG_INT("level", 1, CFGF_NONE),
	CFG_INT("timestamp", -1, CFGF_NONE),
	CFG_STR("raid-uuid", NULL, CFGF_NONE),
	CFG_STR("disk-uuid", NULL, CFGF_NONE),
	CFG_END()
};

struct image_handler mdraid_handler = {
	.type = "mdraid",
	.generate = mdraid_generate,
	.setup = mdraid_setup,
	.opts = mdraid_opts,
};
