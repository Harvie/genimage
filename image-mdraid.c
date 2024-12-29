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
Unlike mdadm this works completely in userspace and does not need kernel to create the ondisk structures.
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
#define BITMAP_SECTORS_MAX	256
#define MDRAID_ALIGN_BYTES	8*512	//(should be divisible by 8 sectors to keep 4kB alignment)

static time_t mdraid_time = 0; //Array creation timestamp has to be identical across all the raid members, so we share it between invocations


/*
 * bitmap structures:
 * Taken from Linux kernel drivers/md/md-bitmap.h
 * (Currently it's missing from linux-libc-dev debian package, so cannot be included)
 */

#define BITMAP_MAGIC 0x6d746962

/* use these for bitmap->flags and bitmap->sb->state bit-fields */
enum bitmap_state {
	BITMAP_STALE	   = 1,  /* the bitmap file is out of date or had -EIO */
	BITMAP_WRITE_ERROR = 2, /* A write error has occurred */
	BITMAP_HOSTENDIAN  =15,
};

/* the superblock at the front of the bitmap file -- little endian */
typedef struct bitmap_super_s {
	__le32 magic;        /*  0  BITMAP_MAGIC */
	__le32 version;      /*  4  the bitmap major for now, could change... */
	__u8  uuid[16];      /*  8  128 bit uuid - must match md device uuid */
	__le64 events;       /* 24  event counter for the bitmap (1)*/
	__le64 events_cleared;/*32  event counter when last bit cleared (2) */
	__le64 sync_size;    /* 40  the size of the md device's sync range(3) */
	__le32 state;        /* 48  bitmap state information */
	__le32 chunksize;    /* 52  the bitmap chunk size in bytes */
	__le32 daemon_sleep; /* 56  seconds between disk flushes */
	__le32 write_behind; /* 60  number of outstanding write-behind writes */
	__le32 sectors_reserved; /* 64 number of 512-byte sectors that are
				  * reserved for the bitmap. */
	__le32 nodes;        /* 68 the maximum number of nodes in cluster. */
	__u8 cluster_name[64]; /* 72 cluster name to which this md belongs */
	__u8  pad[256 - 136]; /* set to zero */
} bitmap_super_t;

/* notes:
 * (1) This event counter is updated before the eventcounter in the md superblock
 *    When a bitmap is loaded, it is only accepted if this event counter is equal
 *    to, or one greater than, the event counter in the superblock.
 * (2) This event counter is updated when the other one is *if*and*only*if* the
 *    array is not degraded.  As bits are not cleared when the array is degraded,
 *    this represents the last time that any bits were cleared.
 *    If a device is being added that has an event count with this value or
 *    higher, it is accepted as conforming to the bitmap.
 * (3)This is the number of sectors represented by the bitmap, and is the range that
 *    resync happens across.  For raid1 and raid5/6 it is the size of individual
 *    devices.  For raid10 it is the size of the array.
 */


static unsigned int calc_sb_1_csum(struct mdp_superblock_1 * sb)
{
	unsigned int disk_csum, csum;
	unsigned long long newcsum;
	int size = sizeof(*sb) + __le32_to_cpu(sb->max_dev)*2;
	unsigned int *isuper = (unsigned int*)sb;

	/* Just for debug. To make sure I can count... (needs include cstddef) */
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

	char *name = cfg_getstr(image->imagesec, "label");
	__le16 max_devices = cfg_getint(image->imagesec, "devices");
	__le16 role = cfg_getint(image->imagesec, "role");

	if (role > MD_DISK_ROLE_MAX) {
		image_error(image, "MDRAID role has to be >= 0 and <= %d.\n", MD_DISK_ROLE_MAX);
		return 6;
	}

	if (role >= max_devices) {
		image_error(image, "MDRAID role of this image has to be lower than total number of %d devices (roles are counted from 0).\n", max_devices);
		return 5;
	}

	size_t superblock_size = sizeof(struct mdp_superblock_1) + max_devices*2;
	struct mdp_superblock_1 *sb = xzalloc(superblock_size);
	bitmap_super_t bsb = {0};

	/* constant array information - 128 bytes */
	sb->magic = MD_SB_MAGIC;	/* MD_SB_MAGIC: 0xa92b4efc - little endian. This is actualy just char string saying "bitm" :-) */
	sb->major_version = 1;	/* 1 */
	sb->feature_map = MD_FEATURE_BITMAP_OFFSET;	/* bit 0 set if 'bitmap_offset' is meaningful */ //TODO: internal bitmap bit is ignored, unless there is correct bitmap with BITMAP_MAGIC in place
	sb->pad0 = 0;		/* always set to 0 when writing */

	char *raid_uuid = cfg_getstr(image->imagesec, "raid-uuid");
	if (!raid_uuid) raid_uuid = uuid_random();
	uuid_parse(raid_uuid, sb->set_uuid);  /* user-space generated. U8[16]*/

	strncpy(sb->set_name, name, 32); sb->set_name[31] = 0;	/* set and interpreted by user-space. CHAR[32] */

	long int timestamp = cfg_getint(image->imagesec, "timestamp");
	if (timestamp >= 0) {
		sb->ctime = timestamp & 0xffffffffff;
	} else {
		sb->ctime = mdraid_time & 0xffffffffff;	/* lo 40 bits are seconds, top 24 are microseconds or 0*/
	}

	sb->level = 1;		/* -4 (multipath), -1 (linear), 0,1,4,5 */
	//sb->layout = 2;		/* only for raid5 and raid10 currently */
	sb->size = (image->size - DATA_OFFSET_BYTES)/512;	/* used size of component devices, in 512byte sectors */

	sb->chunksize = 0;		/* in 512byte sectors - not used in raid 1 */
	sb->raid_disks = max_devices;
	sb->bitmap_offset = 8;	/* sectors after start of superblock that bitmap starts
					 * NOTE: signed, so bitmap can be before superblock
					 * only meaningful of feature_map[0] is set.
					 */

	/* constant this-device information - 64 bytes */
	sb->data_offset = DATA_OFFSET_SECTORS;	/* sector start of data, often 0 */
	sb->data_size = sb->size;	/* sectors in this device that can be used for data */
	sb->super_offset = 8;	/* sector start of this superblock */

	sb->dev_number = role;	/* permanent identifier of this  device - not role in raid. But there is no reason not to have dev_number and role equal when creating fresh array. */
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
	sb->bblog_offset = sb->bitmap_offset+BITMAP_SECTORS_MAX+8;   /* sector offset from superblock to bblog, signed - not unsigned */

	/* array state information - 64 bytes */
	sb->utime = sb->ctime;	/* 40 bits second, 24 bits microseconds */
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
	__le16	*dev_roles = (__le16 *) ((char *) sb + sizeof(struct mdp_superblock_1)); /* role in array, or 0xffff for a spare, or 0xfffe for faulty */
	//memset(dev_roles, 0xFF, max_devices*2); //All devices in array are set as inactive initialy
	for(int i = 0; i<max_devices; i++) { //All devices are assigned roles equal to their dev_number initialy
		dev_roles[i] = i; //Assign active role to all devices
	}

	//Calculate superblock checksum
	sb->sb_csum = calc_sb_1_csum(sb);

	//Prepare bitmap superblock (bitmaps don't have checksums for performance reasons)
	bsb.magic = BITMAP_MAGIC;        /*  0  BITMAP_MAGIC */
	bsb.version = 4;      /* v4 is compatible with mdraid v1.2,   4  the bitmap major for now, could change... */
	memcpy(bsb.uuid, sb->set_uuid, sizeof(bsb.uuid));	/*  8  128 bit uuid - must match md device uuid */
	//bsb.events = 0;       /* 24  event counter for the bitmap (1)*/
	//bsb.events_cleared = 0;/*32  event counter when last bit cleared (2) */
	bsb.sync_size = sb->data_size;    /* 40  the size of the md device's sync range(3) */
	//bsb.state = 0;        /* 48  bitmap state information */
	bsb.chunksize = 64*1024*1024; /* 52  the bitmap chunk size in bytes, 64MB is default on linux */
	bsb.daemon_sleep = 5; /* 5 is considered safe default. 56  seconds between disk flushes */
	//bsb.write_behind = 0; /* 60  number of outstanding write-behind writes */
	bsb.sectors_reserved = roundup(bsb.sync_size / bsb.chunksize, 8); /* 64 number of 512-byte sectors that are reserved for the bitmap. */
	//bsb.nodes;        /* 68 the maximum number of nodes in cluster. */
	//bsb.cluster_name[64]; /* 72 cluster name to which this md belongs */
	//__u8  pad[256 - 136]; /* set to zero */

	//Increase bitmap chunk size till we fit in sectors max
	while(bsb.sectors_reserved > BITMAP_SECTORS_MAX) {
		bsb.chunksize *= 2;
		bsb.sectors_reserved = roundup(bsb.sync_size / bsb.chunksize, 8);
	}

	//Construct image file
	int ret;
	ret = prepare_image(image, image->size);
	if (ret) return ret;
	//Write superblock
	ret = insert_data(image, sb, imageoutfile(image), superblock_size, sb->super_offset * 512);
	if (ret) return ret;
	//Write bitmap
	if ( sb->feature_map & MD_FEATURE_BITMAP_OFFSET ) {
		ret = insert_data(image, &bsb, imageoutfile(image), sizeof(bsb), (sb->super_offset + sb->bitmap_offset) * 512);
		if (ret) return ret;
	}
	//Write data
	if (img_in) {
		ret = insert_image(image, img_in, img_in->size, DATA_OFFSET_BYTES, 0);
		if (ret) return ret;
	}

	free(sb);

	return 0;
}

static int mdraid_setup(struct image *image, cfg_t *cfg) {
	if (!mdraid_time) {
		mdraid_time = time(NULL);
		srandom(mdraid_time); //For UUID generation
	}

	int raid_level = cfg_getint(image->imagesec, "level");

	if (raid_level != 1) {
		image_error(image, "MDRAID Currently only supporting raid level 1 (mirror)!\n");
		return 1;
	}

	//Find data image to be put inside the array
	struct image *img_in = NULL;
	char *src = cfg_getstr(image->imagesec, "image");
	if (src) {
		struct partition *part;
		part = xzalloc(sizeof *part);
		part->image = src;
		list_add_tail(&part->list, &image->partitions);

		if (part->image) {
			image_info(image, "MDRAID using data from: %s\n", part->image);
			img_in = image_get(part->image); //TODO: will this work for pre-existing images not generated by genimage???
			if (!img_in) {
				image_error(image, "MDRAID cannot get image definition: %s\n", part->image);
				return 8;
			}
			if (image->size == 0)
				image->size = roundup(img_in->size + DATA_OFFSET_BYTES, MDRAID_ALIGN_BYTES);
			if (image->size < (img_in->size + DATA_OFFSET_BYTES)) {
				image_error(image, "MDRAID image too small to fit %s\n", part->image);
				return 3;
			}
		}
	} else {
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
	CFG_INT("devices", 1, CFGF_NONE),
	CFG_INT("role", 0, CFGF_NONE),
	CFG_INT("timestamp", -1, CFGF_NONE),
	CFG_STR("raid-uuid", NULL, CFGF_NONE),
	CFG_STR("disk-uuid", NULL, CFGF_NONE),
	CFG_STR("image", NULL, CFGF_NONE),
	CFG_END()
};

struct image_handler mdraid_handler = {
	.type = "mdraid",
	.generate = mdraid_generate,
	.setup = mdraid_setup,
	.opts = mdraid_opts,
};
