/*
 *  Copyright (C) 2019 CS416 Spring 2019
 *
 *	Tiny File System
 *
 *	File:	tfs.c
 *  Author: Yujie REN
 *	Date:	April 2019
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "tfs.h"

char diskfile_path[PATH_MAX];
// Declare your in-memory data structures here
struct superblock sb;
/*
 * Get available inode number from bitmap
 */
int get_avail_ino() {
	// Step 1: Read inode bitmap from disk
	unsigned char buf[BLOCK_SIZE];
	bio_read(sb.i_bitmap_blk, buf);
	// Step 2: Traverse inode bitmap to find an available slot
	int curr_ino = 0;
	while(!get_bitmap((bitmap_t)buf, curr_ino)) {
		curr_ino++;
	}
	// Step 3: Update inode bitmap and write to disk 
	set_bitmap((bitmap_t)buf, curr_ino);
	bio_write(sb.i_bitmap_blk, buf);
	return curr_ino;
}

/*
 * Get available data block number from bitmap
 */
int get_avail_blkno() {
	// Step 1: Read data block bitmap from disk
	unsigned char buf[BLOCK_SIZE];
	bio_read(sb.d_bitmap_blk, buf);
	// Step 2: Traverse data block bitmap to find an available slot
	int curr_blkno = 0;
	while(!get_bitmap((bitmap_t)buf, curr_blkno)) {
		curr_blkno++;
	}
	// Step 3: Update data block bitmap and write to disk 
	set_bitmap((bitmap_t)buf, curr_blkno);
	bio_write(sb.d_bitmap_blk, buf);
	return curr_blkno;
}

/*
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
	// Step 1: Get the inode's on-disk block number
	unsigned int inode_blkno = (unsigned int)ino / (BLOCK_SIZE / sizeof(struct inode));
	// Step 2: Get offset of the inode in the inode on-disk block
	unsigned int inode_offset = (unsigned int) ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode);
	// Step 3: Read the block from disk and then copy into inode structure
	char buf[BLOCK_SIZE];
	bio_read(sb.i_start_blk + inode_blkno, buf);
	struct inode *temp_inode = NULL;
	memcpy(temp_inode, (buf + inode_offset), sizeof(struct inode));
	*inode = *temp_inode;
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	// Step 1: Get the block number where this inode resides on disk
	unsigned int inode_blkno = (unsigned int)ino / (BLOCK_SIZE / sizeof(struct inode));
	// Step 2: Get the offset in the block where this inode resides on disk
	unsigned int inode_offset = (unsigned int) ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode);
	// Step 3: Write inode to disk 
	char buf[BLOCK_SIZE];
	bio_read(sb.i_start_blk + inode_blkno, buf);
	memcpy((buf + inode_offset), inode, sizeof(struct inode));
	bio_write(sb.i_start_blk + inode_blkno, buf);
	return 0;
}


/*
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode *curr_inode = NULL;
	readi(ino, curr_inode);
	// Step 2: Get data block of current directory from inode
	int curr_direct;
	int curr_offset;
	unsigned char buf[BLOCK_SIZE];
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(curr_inode->direct_ptr[curr_direct] == 0)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], buf);
		for(curr_offset = 0; curr_offset < BLOCK_SIZE - sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {	
			if(memcmp(((struct dirent *)(buf + curr_offset))->name, fname, name_len) == 0) {
				memcpy(curr_dirent, buf + curr_offset, sizeof(struct dirent));
				goto found;
			}
		}
	}
	free(curr_dirent);
	return -1; //Not Found 

	// Step 3: Read directory's data block and check each directory entry.
	//If the name matches, then copy directory entry to dirent structure
	found:
		dirent = curr_dirent;
		return 0;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	if(dir_find(dir_inode.ino, fname, name_len, curr_dirent) != -1)
		return -1; //Duplicate Name
	// Step 3: Add directory entry in dir_inode's data block and write to disk

	unsigned char buf[BLOCK_SIZE] = {};
	int block = dir_inode.size / BLOCK_SIZE;
	int offset = dir_inode.size % BLOCK_SIZE;

	// Allocate a new data block for this directory if it does not exist
	if(dir_inode.direct_ptr[block] == 0) {
		dir_inode.direct_ptr[block] = get_avail_blkno();
		dir_inode.link++;
	}
	bio_read(dir_inode.direct_ptr[block], buf);

	curr_dirent =(struct dirent *)buf + offset;
	curr_dirent->ino = f_ino;
	memcpy(curr_dirent->name, fname, name_len);
	curr_dirent->valid = 1;
	// Update directory inode
	dir_inode.size += sizeof(struct dirent);
	if(offset + sizeof(struct dirent) > BLOCK_SIZE)
		dir_inode.size += (BLOCK_SIZE % sizeof(struct dirent)); //Extra padding
	// Write directory entry
	bio_write(dir_inode.direct_ptr[block], buf);
	free(curr_dirent);
	return 0;
	
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
	int curr_direct;
	int curr_offset;
	unsigned char buf[BLOCK_SIZE] = {};
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(dir_inode.direct_ptr[curr_direct] == 0)
			continue;
		bio_read(dir_inode.direct_ptr[curr_direct], buf);
		// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
		for(curr_offset = 0; curr_offset < BLOCK_SIZE - sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {	
			// Step 2: Check if fname exist
			if(memcmp(((struct dirent *)(buf + curr_offset))->name, fname, name_len) == 0) 
				goto found;
		}
	}
	return -1; //Name Not Found	

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk
	found: ;
		unsigned char lastbuf[BLOCK_SIZE] = {};
		if(dir_inode.size % BLOCK_SIZE == 0) { //Deleting the last dirent in a block
			dir_inode.size -= (BLOCK_SIZE % sizeof(struct dirent)); //Removes extra padding
		}
		dir_inode.size -= sizeof(struct dirent);
		int lastblock = dir_inode.size / BLOCK_SIZE;
		int lastoffset = dir_inode.size % BLOCK_SIZE;
		bio_read(dir_inode.direct_ptr[lastblock], lastbuf);
		memcpy((struct dirent *)(buf + curr_offset), (struct dirent *)(lastbuf + lastoffset), sizeof(struct dirent));
		memset((struct dirent *)(lastbuf + lastoffset), 0, sizeof(struct dirent));
		bio_write(dir_inode.direct_ptr[curr_direct], buf);
		bio_write(dir_inode.direct_ptr[lastblock], lastbuf);
		if(dir_inode.size % BLOCK_SIZE == 0) { //Deleting the only dirent in a block
			unsigned char bitmapbuf[BLOCK_SIZE] = {};
			dir_inode.direct_ptr[lastblock] = 0; //Remove pointer to direct block
			bio_read(sb.d_bitmap_blk, bitmapbuf);
			unset_bitmap((bitmap_t)bitmapbuf, lastblock);
			bio_write(sb.d_bitmap_blk, bitmapbuf); //Free direct block in bitmap
			dir_inode.link--;
		}
		return 0;
}

/*
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	char* token;
	char* rest;
	struct dirent *target = NULL;
	token = strtok_r((char *)path, "\\", &rest);
	if (token == NULL) {
		readi(ino, inode);
		return 0;
	}
	else if (strcmp(token, "") == 0)
		return get_node_by_path(rest, ino, inode);
	if(dir_find(ino, token, strlen(token), target) == 0)
		return get_node_by_path(rest, target->ino, inode);
	else
		return -1;	
		
}

/*
 * Make file system
 */
int tfs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);

	// write superblock information
	sb.magic_num = MAGIC_NUM;
	sb.max_inum = MAX_INUM;
	sb.max_dnum = MAX_DNUM;
	sb.i_bitmap_blk = 1;
	sb.d_bitmap_blk = 2;
	sb.i_start_blk = 3;
	sb.d_start_blk = MAX_INUM + 3;
	char buff[BLOCK_SIZE];
	memcpy(buff, &sb, sizeof(sb));
	bio_write(0, buff);

	// initialize inode bitmap
	unsigned char inode_bitmap [MAX_INUM/8];
	memset(inode_bitmap, 0, MAX_INUM/8);
	// update bitmap information for root directory
	inode_bitmap[0] |= (1 << 7);
	bio_write(1, inode_bitmap);

	// initialize data block bitmap
	unsigned char data_bitmap [MAX_DNUM/8];
	memset(data_bitmap, 0, MAX_DNUM/8);
	bio_write(2, data_bitmap);

	// update inode for root directory
	struct inode root;
	root.ino = 0;
	root.valid = 1;
	root.size = 0;
	root.type = 1;
	root.link = 0;

	return 0;
}


/*
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs
	if (access(diskfile_path, F_OK) == -1){	//diskfile does not exist
		tfs_mkfs();
	}

	// Step 1b: If disk file is found, just initialize in-memory data structures
  // and read superblock from disk
	else { //diskfile was found
		dev_open(diskfile_path);
		char buff [BLOCK_SIZE];
		bio_read(0, (void*)buff);
		memcpy(&sb, buff, sizeof(sb));
		if (sb.magic_num != MAGIC_NUM){
			fprintf(stderr, "ERROR:MAGIC_NUM DOES NOT MATCH\n");
			exit(EXIT_FAILURE);
		}
	}
	return NULL;
}

static void tfs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures

	// Step 2: Close diskfile

}

static int tfs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_IFDIR | 0755;
		stbuf->st_nlink  = 2;
		time(&stbuf->st_mtime);

	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

    return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

	return 0;
}


static int tfs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk


	return 0;
}

static int tfs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

static int tfs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int tfs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations tfs_ope = {
	.init		= tfs_init,
	.destroy	= tfs_destroy,

	.getattr	= tfs_getattr,
	.readdir	= tfs_readdir,
	.opendir	= tfs_opendir,
	.releasedir	= tfs_releasedir,
	.mkdir		= tfs_mkdir,
	.rmdir		= tfs_rmdir,

	.create		= tfs_create,
	.open		= tfs_open,
	.read 		= tfs_read,
	.write		= tfs_write,
	.unlink		= tfs_unlink,

	.truncate   = tfs_truncate,
	.flush      = tfs_flush,
	.utimens    = tfs_utimens,
	.release	= tfs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &tfs_ope, NULL);

	return fuse_stat;
}
