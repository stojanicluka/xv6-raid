#include "types.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "param.h"
#include "proc.h"
#include "fs.h"
#include "virtio.h"
#include "memlayout.h"

#define DISK_COUNT (VIRTIO_RAID_DISK_END - VIRTIO_RAID_DISK_START + 1)
#define RAID_MAGIC_NUMBER 0x47

enum RAID_TYPE {RAID0, RAID1, RAID0_1, RAID4, RAID5};

struct raid_info {
	enum RAID_TYPE type;
	uint16 disk_count;
	uint64 disk_blk_count;
	uint64 disk_blk_size;
	uint64 blk_count;
};

enum DISK_OPERATION_STATUS { DISK_IDLE, DISK_BUSY };
enum DISK_HEALTH_STATUS { DISK_OK, DISK_FAIL, DISK_UNRECOVERABLE };

static struct raid_controller {
	uint8 active;
	uint8 controller_lock;
	struct spinlock main_lock;
	struct raid_info info;
	struct spinlock info_lock[DISK_COUNT];
	enum DISK_OPERATION_STATUS op_status[DISK_COUNT];
	enum DISK_HEALTH_STATUS h_status[DISK_COUNT];
} raid_ctrl;

struct master_blk_t {
	uint8 magic_number;
	uint8 disk_id;
	enum DISK_HEALTH_STATUS health;
	struct raid_info info;
	uint8 empty[BSIZE];
};

void raid_lock_controller() {
	acquire(&raid_ctrl.main_lock);
	while (raid_ctrl.controller_lock)
		sleep(&raid_ctrl.controller_lock, &raid_ctrl.main_lock);
	raid_ctrl.controller_lock = 1;
	release(&raid_ctrl.main_lock);
}

void raid_unlock_controller() {
	acquire(&raid_ctrl.main_lock);
	raid_ctrl.controller_lock = 0;
	wakeup(&raid_ctrl.controller_lock);
	release(&raid_ctrl.main_lock);
}

void raid_lock_init() {
	initlock(&raid_ctrl.main_lock, "raid_main_lock");
}

uint64 raid_system_init() {

	raid_ctrl.active = 0;

	if (DISK_COUNT < 1)
		return (uint64)-1;

	struct master_blk_t master_blk_ref;
	read_block(1, 0, (uchar*)&master_blk_ref);
	if (master_blk_ref.magic_number != RAID_MAGIC_NUMBER || DISK_COUNT != master_blk_ref.info.disk_count)
		return (uint64)-1;

	struct raid_info info = master_blk_ref.info;

	for (uint8 i = 1; i < DISK_COUNT; i++) {
		read_block(i+1, 0, (uchar*)&master_blk_ref);
		if (memcmp(&info, &master_blk_ref.info, sizeof(struct raid_info)) || i != master_blk_ref.disk_id) {
			return (uint64)-1;
		}
		raid_ctrl.h_status[i] = master_blk_ref.health;
	}

	raid_ctrl.info = info;

	for (uint8 i = 0; i < DISK_COUNT; i++) {
		initlock(&raid_ctrl.info_lock[i], "raid_info_lock");
	}

	raid_ctrl.active = 1;
	return 0;
}

// CAUTION: Disk IDs must be sorted in order to avoid deadlock!
uint8 acquire_disks(uint8 disks[], uint8 cnt) {
	for (uint8 i = 0; i < cnt; i++) {
		acquire(&raid_ctrl.info_lock[disks[i]]);
		while (raid_ctrl.op_status[disks[i]] == DISK_BUSY) { sleep(&raid_ctrl.op_status[disks[i]], &raid_ctrl.info_lock[disks[i]]); }
		raid_ctrl.op_status[disks[i]] = DISK_BUSY;
		release(&raid_ctrl.info_lock[disks[i]]);
	}

	return 0;
}

uint8 release_disks(uint8 disks[], uint8 cnt) {
	for (uint8 i = 0; i < cnt; i++) {
		acquire(&raid_ctrl.info_lock[disks[i]]);
		raid_ctrl.op_status[disks[i]] = DISK_IDLE;
		wakeup(&raid_ctrl.op_status[disks[i]]);
		release(&raid_ctrl.info_lock[disks[i]]);
	}

	return 0;
}


uint64 validate_raid() {
	raid_lock_controller();
	if (!raid_ctrl.active) {
		uint64 result = raid_system_init();
		if (result) {
			raid_unlock_controller();
			return result;
		}
	}
	raid_unlock_controller();
	return 0;
}


uint8 write_master_blk(uint8 disk_id_low, uint8 disk_id_high, enum DISK_HEALTH_STATUS health) {
	struct master_blk_t master_blk;

	master_blk.magic_number = RAID_MAGIC_NUMBER;
	master_blk.info = raid_ctrl.info;
	master_blk.health = health;

	uint8 disk[] = { 0 };
	for (uint8 i = disk_id_low; i <= disk_id_high; i++) {
		disk[0] = master_blk.disk_id = i;
		acquire_disks(disk, 1);
		write_block(i+1, 0, (uchar*)&master_blk);
		release_disks(disk, 1);
	}

	return 0;
}

uint8 raid_invalidate(uint disk_id_low, uint disk_id_high) {
	uint8 empty[BSIZE];
	empty[0] = 0;
	for (uint8 i = disk_id_low; i <= disk_id_high; i++) {
		acquire_disks(&i, 1);
		write_block(i+1, 0, (uchar*)&empty);
		release_disks(&i, 1);
	}

	return 0;
}

uint8 zero_all_disks() {
	uint8 buf[BSIZE];
	for (uint16 i = 0; i < BSIZE; ++i)
		buf[i] = 0;


	for (uint64 i = 0; i < raid_ctrl.info.disk_blk_count; ++i) {
		for (uint8 j = 0; j < DISK_COUNT; ++j) {
			write_block(j+1, i, buf);
		}
	}

	return 0;
}

uint8 raid_init(enum RAID_TYPE type) {
	validate_raid();
	raid_lock_controller();

	if (raid_ctrl.active) {
        raid_unlock_controller();
		return (uint8)-1;
	}

	raid_ctrl.info.type = type;
	raid_ctrl.info.disk_count = DISK_COUNT;
	raid_ctrl.info.disk_blk_count = *((uint64*)(VIRTIO0+VIRTIO_OFFSET+VIRTIO_MMIO_CONFIG)) / 2;
	raid_ctrl.info.disk_blk_size = BSIZE;

	switch(type) {
	case RAID0:
		raid_ctrl.info.blk_count = DISK_COUNT * (raid_ctrl.info.disk_blk_count-1);
		zero_all_disks();
		break;
	case RAID1:
		if (DISK_COUNT < 2)
			return (uint8)-1;
		raid_ctrl.info.blk_count = (raid_ctrl.info.disk_blk_count-1)*(DISK_COUNT/2);
		zero_all_disks();
		break;
	case RAID0_1:
		if (DISK_COUNT < 2)
			return (uint8)-1;
		raid_ctrl.info.blk_count = (DISK_COUNT/2) * (raid_ctrl.info.disk_blk_count-1);
		zero_all_disks();
		break;
	case RAID4:
	case RAID5:
		if (DISK_COUNT < 2)
			return (uint8)-1;
		raid_ctrl.info.blk_count = (DISK_COUNT - 1) * (raid_ctrl.info.disk_blk_count-1);
		zero_all_disks();
		break;
	default:
		break;
	}

	for (uint8 i = 0; i < DISK_COUNT; i++) {
		initlock(&raid_ctrl.info_lock[i], "disk_info_lock");
		raid_ctrl.op_status[i] = DISK_IDLE;
		raid_ctrl.op_status[i] = DISK_OK;
	}

	uint8 res = write_master_blk(0, DISK_COUNT-1, DISK_OK);

	raid_ctrl.active = 1U;
	raid_unlock_controller();
	return res;
}

uint8 raid_read_0(uint64 block, uchar *data) {
	uint8 disk[] = { 0 };
	disk[0] = block % DISK_COUNT;
	uint64 disk_blk = block / DISK_COUNT;

	acquire_disks(disk, 1);
	uint8 status = (uint8)-1;

	if (raid_ctrl.h_status[disk[0]] == DISK_OK) {
		read_block(disk[0]+1, disk_blk+1, data);
		status = 0;
	}

	release_disks(disk, 1);
	return status;
}

uint8 raid_read_1_01(uint8 disk0, uint8 disk1, uint8 *last_access, uint64 disk_blk, uchar *data) {
	uint8 disk = (uint8)-1;
	if (raid_ctrl.h_status[disk0] == DISK_OK && raid_ctrl.h_status[disk1] == DISK_OK) {
		disk = (*last_access ? disk0 : disk1);
	} else if (raid_ctrl.h_status[disk0] == DISK_OK) {
		disk = disk0;
	} else if (raid_ctrl.h_status[disk1] == DISK_OK) {
		disk = disk1;
	}

	if (disk == (uint8)-1)
		return (uint8)-1;

	acquire_disks(&disk, 1);
	read_block(disk+1, disk_blk+1, data);
	release_disks(&disk, 1);
	*last_access = (disk == disk0 ? 0 : 1);
	return 0;
}

uint8 raid_read_1(uint64 block, uchar *data) {
	static uint8 last_access[DISK_COUNT/2];

	uint8 disk_pair = block / (raid_ctrl.info.disk_blk_count-1);
	uint64 disk_blk = block % (raid_ctrl.info.disk_blk_count-1);

	uint8 disk0 = disk_pair;
	uint8 disk1 = disk_pair + DISK_COUNT/2;

	return raid_read_1_01(disk0, disk1, &last_access[disk_pair], disk_blk, data);
}

uint8 raid_read_01(uint64 block, uchar *data) {
	static uint8 last_access[DISK_COUNT/2];

	uint8 disk_pair = block % (DISK_COUNT/2);
	uint64 disk_blk = block / (DISK_COUNT/2);

	uint8 disk0 = disk_pair;
	uint8 disk1 = disk_pair + DISK_COUNT/2;

	return raid_read_1_01(disk0, disk1, &last_access[disk_pair], disk_blk, data);
}

uint8 xor_block(uchar *blk0, uchar *blk1) {
	for (uint16 i = 0; i < BSIZE; ++i)
		blk0[i] ^= blk1[i];

	return 0;
}

uint8 raid_read_4_5(uint8 target_disk, uint8 parity_disk, uint64 disk_blk, uchar *data) {
	if (raid_ctrl.h_status[target_disk] == DISK_OK) {
		acquire_disks(&target_disk, 1);
		read_block(target_disk+1, disk_blk+1, data);
		release_disks(&target_disk, 1);
		return 0;
	}

	// If target disk is not OK
	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (i != target_disk && raid_ctrl.h_status[i] != DISK_OK)
			return -1;

	uchar buf[BSIZE];
	uint8 data_set = 0;

	uint8 disks[DISK_COUNT-1];

	uint8 index = 0;
	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (i != target_disk) disks[index++] = i;

	acquire_disks(disks, DISK_COUNT-1);
	for (uint8 i = 0; i < DISK_COUNT-1; ++i) {
		if (!data_set) {
			data_set = 1;
			read_block(disks[i]+1, disk_blk+1, data);
		} else {
			read_block(disks[i]+1, disk_blk+1, buf);
			xor_block(data, buf);
		}
	}
	release_disks(disks, DISK_COUNT-1);

	return 0;
}

uint8 raid_read_4(uint64 block, uchar *data) {
	uint8 disk = block % (DISK_COUNT-1);
	uint8 disk_blk = block / (DISK_COUNT-1);

	return raid_read_4_5(disk, DISK_COUNT-1, disk_blk, data);
}

uint8 raid_read_5(uint64 block, uchar *data) {
	uint8 disk = block % (DISK_COUNT-1);
	uint8 disk_blk = block / (DISK_COUNT-1);
	uint8 parity_disk = DISK_COUNT-1 - (disk_blk % DISK_COUNT);
	if (parity_disk <= disk)
		++disk;

	return raid_read_4_5(disk, parity_disk, disk_blk, data);
}

uint8 raid_write_0(uint64 block, uchar *data) {
	uint8 disk = block % DISK_COUNT;
	uint64 disk_blk = block / DISK_COUNT;

	uint8 status = (uint8)-1;

	acquire_disks(&disk, 1);
	if (raid_ctrl.h_status[disk] == DISK_OK) {
		write_block(disk+1, disk_blk+1, data);
		status = 0;
	}
	release_disks(&disk, 1);
	return status;
}

uint8 raid_write_1_01(uint8 disk0, uint8 disk1, uint64 disk_blk, uchar *data) {

	uint8 diskl = (disk0 < disk1 ? disk0 : disk1);
	uint8 diskh = (disk0 > disk1 ? disk0 : disk1);
	uint8 disks[] = {diskl, diskh};

	uint8 status = (uint8)-1;
	acquire_disks(disks, 2);
	if (raid_ctrl.h_status[diskl] == DISK_OK) {
		write_block(diskl+1, disk_blk+1, data);
		status = 0;
	}
	if (raid_ctrl.h_status[diskh] == DISK_OK) {
		write_block(diskh+1, disk_blk+1, data);
		status = 0;
	}

	release_disks(disks, 2);

	return status;
}

uint8 raid_write_1(uint64 block, uchar *data) {
	uint8 disk_pair = block / (raid_ctrl.info.disk_blk_count-1);
	uint64 disk_blk = block % (raid_ctrl.info.disk_blk_count-1);

	uint8 disk0 = disk_pair;
	uint8 disk1 = disk_pair + (DISK_COUNT)/2;
	return raid_write_1_01(disk0, disk1, disk_blk, data);
}

uint8 raid_write_01(uint64 block, uchar *data) {
	uint8 disk_pair = block % ((DISK_COUNT)/2);
	uint64 disk_blk = block / ((DISK_COUNT)/2);

	uint8 disk0 = disk_pair;
	uint8 disk1 = disk_pair + (DISK_COUNT)/2;
	return raid_write_1_01(disk0, disk1, disk_blk, data);
}

uint8 raid_write_4_5(uint8 target_disk, uint8 parity_disk, uint64 disk_blk, uchar *data) {
	uchar temp_buf1[BSIZE];
	uchar temp_buf2[BSIZE];

	if (raid_ctrl.h_status[target_disk] == DISK_OK) {

		if (raid_ctrl.h_status[parity_disk] == DISK_OK) {
			uint8 diskl = (parity_disk < target_disk ? parity_disk : target_disk);
			uint8 diskh = (parity_disk > target_disk ? parity_disk : target_disk);
			uint8 disks[2] = {diskl, diskh};

			acquire_disks(disks, 2);
			read_block(target_disk+1, disk_blk+1, temp_buf1);
			read_block(parity_disk+1, disk_blk+1, temp_buf2);

			xor_block(temp_buf2, temp_buf1);
			xor_block(temp_buf2, data);


			write_block(parity_disk+1, disk_blk+1, temp_buf2);
			write_block(target_disk+1, disk_blk+1, data);
			release_disks(disks, 2);

			return 0;
		}

		acquire_disks(&target_disk, 1);
		write_block(target_disk+1, disk_blk+1, data);
		release_disks(&target_disk, 1);
		return 0;
	}

	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (raid_ctrl.h_status[i] != DISK_OK && i != target_disk) return -1;

	uint8 disks[DISK_COUNT-1];
	uint8 index = 0;

	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (raid_ctrl.h_status[i] == DISK_OK) disks[index++] = i;


	for (uint16 i = 0; i < BSIZE; ++i)
		temp_buf1[i] = data[i];

	acquire_disks(disks, DISK_COUNT-1);
	for (uint8 i = 0; i < DISK_COUNT; ++i) {
		if (i == target_disk || i == parity_disk) continue;
		read_block(i+1, disk_blk+1, temp_buf2);
		xor_block(temp_buf1, temp_buf2);
	}
	write_block(parity_disk + 1, disk_blk+1, temp_buf1);
	release_disks(disks, DISK_COUNT-1);

	return 0;
}

uint8 raid_write_4(uint64 block, uchar *data) {
	uint8 disk = block % (DISK_COUNT-1);
	uint64 disk_blk = block / (DISK_COUNT-1);

	return raid_write_4_5(disk, DISK_COUNT-1, disk_blk, data);
}

uint8 raid_write_5(uint64 block, uchar *data) {
	uint8 disk = block % (DISK_COUNT-1);
	uint8 disk_blk = block / (DISK_COUNT-1);
	uint8 parity_disk = DISK_COUNT-1 - (disk_blk % DISK_COUNT);
	if (parity_disk <= disk)
		++disk;

	return raid_write_4_5(disk, parity_disk, disk_blk, data);
}

uint8 raid_copy(uint8 disk_src, uint8 disk_target) {
	uint8 diskl = (disk_src < disk_target ? disk_src : disk_target);
	uint8 diskh = (disk_src > disk_target ? disk_src : disk_target);

	uint8 disks[2] = { diskl, diskh };
	uint8 buf[BSIZE];
	for (uint64 i = 1; i < raid_ctrl.info.disk_blk_count; i++) {
		acquire_disks(disks, 2);
		read_block(disk_src+1, i, buf);
		write_block(disk_target+1, i, buf);
		release_disks(disks, 2);
	}

	return 0;
}

uint8 raid_repair_4_5 (uint8 disk) {
	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (i != disk && raid_ctrl.h_status[i] != DISK_OK) return -1;

	uint8 disks[DISK_COUNT-1];
	uint8 index = 0;
	for (uint8 i = 0; i < DISK_COUNT; ++i)
		if (i != disk) disks[index++] = i;


	uchar res[BSIZE];
	uchar buf[BSIZE];
	for (uint64 i = 1; i < raid_ctrl.info.disk_blk_count; ++i) {

		uint8 res_set = 0;

		acquire_disks(disks, DISK_COUNT-1);
		for (uint8 j = 0; j < DISK_COUNT-1; ++j) {
			if (!res_set) {
				res_set = 1;
				read_block(disks[j]+1, i, res);
			} else {
				read_block(disks[j]+1, i, buf);
				xor_block(res, buf);
			}
		}
		write_block(disk+1, i, res);
		release_disks(disks, DISK_COUNT-1);
	}

	return 0;
}


uint64 sys_init_raid() {
	enum RAID_TYPE type;
	argint(0, (int*)&type);
	return raid_init(type);
}

uint64 sys_read_raid() {
	if (validate_raid())
		return -1;

	int blkn;
	uchar *data;

	argint(0, (int*)&blkn);
	argaddr(1, (uint64*)&data);

	data = (uchar*)(walkaddr(myproc()->pagetable, (uint64)data) + (uint64)data % PGSIZE);

	switch (raid_ctrl.info.type) {
		case RAID0:
			return raid_read_0(blkn, data);
		case RAID1:
			return raid_read_1(blkn, data);
		case RAID0_1:
			return raid_read_01(blkn, data);
		case RAID4:
			return raid_read_4(blkn, data);
		case RAID5:
			return raid_read_5(blkn, data);
		default:
			break;
	}

	return (uint64)-1;
}

uint64 sys_write_raid() {
	if (validate_raid())
		return -1;

	int blkn;
	uchar *data;

	argint(0, (int*)&blkn);
	argaddr(1, (uint64*)&data);

	data = (uchar*)(walkaddr(myproc()->pagetable, (uint64)data) + (uint64)data % PGSIZE);

	switch (raid_ctrl.info.type) {
		case RAID0:
			return raid_write_0(blkn, data);
		case RAID1:
			return raid_write_1(blkn, data);
		case RAID0_1:
			return raid_write_01(blkn, data);
		case RAID4:
			return raid_write_4(blkn, data);
		case RAID5:
			return raid_write_5(blkn, data);
		default:
			break;
	}

	return (uint64)-1;
}

uint64 sys_disk_fail_raid() {
	if (validate_raid())
		return -1;

	int disk;
	argint(0, (int*)&disk);
	if (disk >= DISK_COUNT)
		return (uint64)-1;
	uint8 diskB = (uint)disk;

	acquire_disks(&diskB, 1);
	raid_ctrl.h_status[diskB] = DISK_FAIL;
	release_disks(&diskB, 1);

	write_master_blk(disk, disk, DISK_FAIL);
	uchar buf[1024];
	for (uint16 i = 0; i < 1024; ++i) buf[i] = 0;

	acquire_disks(&diskB, 1);
	for (uint64 i = 1; i <raid_ctrl.info.disk_blk_count; ++i)
		write_block(disk+1, i, buf);
	release_disks(&diskB, 1);
	return 0;
}

uint64 sys_disk_repaired_raid() {
	if (validate_raid())
		return -1;

	int disk;
	argint(0, (int*)&disk);
	if (disk >= DISK_COUNT)
		return (uint64)-1;
	uint8 diskB = (uint)disk;
	uint8 result = (uint8)-1;
	switch (raid_ctrl.info.type) {
		case RAID1:
			if (disk >= DISK_COUNT/2) {
				result =  raid_copy(disk-DISK_COUNT/2, disk);
			} else
				result =  raid_copy(disk+DISK_COUNT/2, disk);
			break;
		case RAID0_1:
			if (disk >= DISK_COUNT/2)
				result = raid_copy(disk-DISK_COUNT/2, disk);
			else
				result = raid_copy(disk+DISK_COUNT/2, disk);
			break;
		case RAID4:
		case RAID5:
			result = raid_repair_4_5(disk);
			break;
		default:
			break;
	}


	if (!result) {
		write_master_blk(disk, disk, DISK_OK);
		acquire_disks(&diskB, 1);
		raid_ctrl.h_status[diskB] = DISK_OK;
		release_disks(&diskB, 1);
	} else
		raid_ctrl.h_status[diskB] = DISK_UNRECOVERABLE;

	return result;
}

uint64 sys_info_raid() {
	if (validate_raid())
		return -1;

	uint *blkn;
	uint *blks;
	uint *diskn;

	argaddr(0, (uint64*)&blkn);
	argaddr(1, (uint64*)&blks);
	argaddr(2, (uint64*)&diskn);

	blkn = (uint*)(walkaddr(myproc()->pagetable, (uint64)blkn) + (uint64)blkn % PGSIZE);
	blks = (uint*)(walkaddr(myproc()->pagetable, (uint64)blks) + (uint64)blks % PGSIZE);
	diskn = (uint*)(walkaddr(myproc()->pagetable, (uint64)diskn) + (uint64)diskn % PGSIZE);

	*blkn = raid_ctrl.info.blk_count;
	*blks = BSIZE;
	*diskn = (raid_ctrl.info.disk_count / 2) * 2;
	return 0;
}

uint64 sys_destroy_raid() {
	if (validate_raid())
		return -1;

	uint8 status;
	raid_lock_controller();
	status = raid_invalidate(0, DISK_COUNT-1);
	raid_ctrl.active = 0;
	raid_unlock_controller();

	return status;
}

uint64 sys_raid_debug_write_disk() {
    int diskn;

    int blkn;
    uchar *data;

    argint(0, (int*)&diskn);
    argint(1, (int*)&blkn);
    argaddr(2, (uint64*)&data);
    uint8 diskn8 = diskn;
    data = (uchar*)(walkaddr(myproc()->pagetable, (uint64)data) + (uint64)data % PGSIZE);

    acquire_disks(&diskn8, 1);
    write_block(diskn8, blkn, data);
    release_disks(&diskn8, 1);
    return 0;
}

uint64 sys_raid_debug_read_disk() {
    int diskn;

    int blkn;
    uchar *data;

    argint(0, (int*)&diskn);
    argint(1, (int*)&blkn);
    argaddr(2, (uint64*)&data);
    uint8 diskn8 = diskn;
    data = (uchar*)(walkaddr(myproc()->pagetable, (uint64)data) + (uint64)data % PGSIZE);

    acquire_disks(&diskn8, 1);
    read_block(diskn8, blkn, data);
    release_disks(&diskn8, 1);
    return 0;
}
