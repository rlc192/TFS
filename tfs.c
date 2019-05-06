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
int inode_capacity = (BLOCK_SIZE - (BLOCK_SIZE % sizeof(struct inode))) / sizeof(struct inode);
int dirent_capacity = (BLOCK_SIZE - (BLOCK_SIZE % sizeof(struct dirent))) / sizeof(struct dirent);

/*
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	unsigned char buf[BLOCK_SIZE];
	bio_read(sb.i_bitmap_blk, buf);

	// Step 2: Traverse inode bitmap to find an available slot
	int curr_ino = 0;
	while(get_bitmap((bitmap_t)buf, curr_ino)) {
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
	while(get_bitmap((bitmap_t)buf, curr_blkno)) {
		curr_blkno++;
	}

	// Step 3: Update data block bitmap and write to disk
	set_bitmap((bitmap_t)buf, curr_blkno);
	bio_write(sb.d_bitmap_blk, buf);
	return sb.d_start_blk + curr_blkno;

}

/*
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	// Step 1: Get the inode's on-disk block number
	unsigned int inode_blkno = sb.i_start_blk + (unsigned int)ino / inode_capacity;

	// Step 2: Get offset of the inode in the inode on-disk block
	unsigned int inode_offset = (unsigned int) (ino % inode_capacity) * sizeof(struct inode);
	
	// Step 3: Read the block from disk and then copy into inode structure
	char *buf = malloc(sizeof(char) * BLOCK_SIZE);
	bio_read(inode_blkno, buf);
	struct inode *temp_inode = malloc(sizeof(struct inode));
	memcpy(temp_inode, buf + inode_offset, sizeof(struct inode));
	*inode = *temp_inode;
	return 0;

}

int writei(uint16_t ino, struct inode *inode) {
	
	int inode_capacity = (BLOCK_SIZE - (BLOCK_SIZE % sizeof(struct inode))) / sizeof(struct inode);
	// Step 1: Get the block number where this inode resides on disk
	unsigned int inode_blkno = sb.i_start_blk + (unsigned int)ino / inode_capacity;

	// Step 2: Get the offset in the block where this inode resides on disk
	unsigned int inode_offset = (unsigned int) (ino % inode_capacity) * sizeof(struct inode);
	
	// Step 3: Write inode to disk
	char buf[BLOCK_SIZE];
	bio_read(inode_blkno, buf);
	memcpy((buf + inode_offset), inode, sizeof(struct inode));
	bio_write(inode_blkno, buf);
	return 0;

}


/*
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode *curr_inode = malloc(sizeof(struct inode));
	readi(ino, curr_inode);

	// Step 2: Get data block of current directory from inode
	// Step 3: Read directory's data block and check each directory entry.

	int curr_direct;
	int curr_offset;
	char buf[BLOCK_SIZE];
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], buf);
		for(curr_offset = 0; curr_offset <= (dirent_capacity - 1) * sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {
			if(strcmp(((struct dirent *)(buf + curr_offset))->name, fname) == 0) {
				memcpy(curr_dirent, buf + curr_offset, sizeof(struct dirent));
				goto found;
			}
		}
	}
	free(curr_dirent);
	return -1; //Not Found

	//If the name matches, then copy directory entry to dirent structure
	found:
		*dirent = *curr_dirent;
		return 0;

}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	if(dir_find(dir_inode.ino, fname, name_len, curr_dirent) != -1) {
		return -1; //Duplicate Name
	}

	// Step 3: Add directory entry in dir_inode's data block and write to disk
	char buf[BLOCK_SIZE] = {};
	int block = dir_inode.size / (dirent_capacity * sizeof(struct dirent));
	int offset = dir_inode.size % (dirent_capacity * sizeof(struct dirent));

	// Allocate a new data block for this directory if it does not exist
	if(dir_inode.direct_ptr[block] == -1) {
		dir_inode.direct_ptr[block] = get_avail_blkno();
		dir_inode.link++;
		bio_read(dir_inode.direct_ptr[block], buf);
		memset(buf, 0, BLOCK_SIZE);
	}
	else {
		bio_read(dir_inode.direct_ptr[block], buf);
	}
	memcpy(curr_dirent, buf + offset, sizeof(struct dirent));
	curr_dirent->ino = f_ino;
	memcpy(curr_dirent->name, fname, name_len);
	curr_dirent->valid = 1;
	memcpy(buf + offset, curr_dirent, sizeof(struct dirent));

	// Update directory inode
	dir_inode.size += sizeof(struct dirent);
	writei(dir_inode.ino, &dir_inode);

	// Write directory entry
	bio_write(dir_inode.direct_ptr[block], buf);
	return 0;

}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	int curr_direct;
	int curr_offset;
	char buf[BLOCK_SIZE] = {};
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(dir_inode.direct_ptr[curr_direct] == -1)
			continue;
		bio_read(dir_inode.direct_ptr[curr_direct], buf);

		// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
		for(curr_offset = 0; (dirent_capacity - 1) * sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {

			// Step 2: Check if fname exist
			if(strcmp(((struct dirent *)(buf + curr_offset))->name, fname) == 0)
				goto found;
		}
	}
	return -1; //Name Not Found

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk
	found: ;
		unsigned char lastbuf[BLOCK_SIZE] = {};
		dir_inode.size -= sizeof(struct dirent);
		int lastblock = dir_inode.size / (dirent_capacity * sizeof(struct dirent));
		int lastoffset = dir_inode.size % (dirent_capacity * sizeof(struct dirent));
		bio_read(dir_inode.direct_ptr[lastblock], lastbuf);
		if(lastblock != curr_direct) {
			memcpy((struct dirent *)(buf + curr_offset), (struct dirent *)(lastbuf + lastoffset), sizeof(struct dirent));
			memset(lastbuf + lastoffset, 0, sizeof(struct dirent));
			bio_write(dir_inode.direct_ptr[lastblock], lastbuf);
		}
		else {
			memcpy((struct dirent *)(buf + curr_offset), (struct dirent *)(buf + lastoffset), sizeof(struct dirent));
			memset(buf + lastoffset, 0, sizeof(struct dirent));
		}
		bio_write(dir_inode.direct_ptr[curr_direct], buf);
		if(dir_inode.size % BLOCK_SIZE == 0) { //Deleting the only dirent in a block
			unsigned char bitmapbuf[BLOCK_SIZE] = {};
			dir_inode.direct_ptr[lastblock] = -1; //Remove pointer to direct block
			bio_read(sb.d_bitmap_blk, bitmapbuf);
			unset_bitmap((bitmap_t)bitmapbuf, lastblock);
			bio_write(sb.d_bitmap_blk, bitmapbuf); //Free direct block in bitmap
			dir_inode.link--;
		}
		writei(dir_inode.ino, &dir_inode);
		return 0;
}

/*
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	char* token = strdup(path);
	char* rest;
	struct dirent *target = malloc(sizeof(struct dirent));
	token = strtok_r(token, "/", &rest);
	if (token == NULL) {
		readi(ino, inode);
		return 0;
	}
	if (dir_find(ino, token, strlen(token), target) == 0) {
		return get_node_by_path(rest, target->ino, inode);
	}
	else {
		return -1;
	}
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
	sb.d_start_blk = (MAX_INUM / inode_capacity) * sizeof(struct inode) + 4;
	char buff[BLOCK_SIZE];
	memcpy(buff, &sb, sizeof(sb));
	bio_write(0, buff);

	// initialize inode bitmap
	unsigned char inode_bitmap [MAX_INUM / 8];
	memset(inode_bitmap, 0, MAX_INUM / 8);

	// update bitmap information for root directory
	set_bitmap((bitmap_t)inode_bitmap, 0);
	bio_write(sb.i_bitmap_blk, inode_bitmap);

	// initialize data block bitmap
	unsigned char data_bitmap [MAX_DNUM / 8];
	memset(data_bitmap, 0, MAX_DNUM / 8);
	bio_write(sb.d_bitmap_blk, data_bitmap);

	// update inode for root directory
	struct inode root;
	root.ino = 0;
	root.valid = 1;
	root.size = 0;
	root.type = 1;
	root.link = 2;

	int i;
	for(i = 0; i <= 15; i++) {
		root.direct_ptr[i] = -1;
		if(i <= 7)
			root.indirect_ptr[i] = -1;
	}

	root.vstat.st_mtime = time(0);
	root.vstat.st_atime = time(0);
	writei(root.ino, &root);

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
	// nothing to deallocate

	// Step 2: Close diskfile
	dev_close();

}

static int tfs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));
	if(get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}

	// Step 2: fill attribute of file into stbuf from inode
	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid(); 
	stbuf->st_ino = curr_inode->ino;
	stbuf->st_nlink = curr_inode->link;
	stbuf->st_size = curr_inode->size;
	stbuf->st_blksize = BLOCK_SIZE;
	if(curr_inode->type == 0) {
		stbuf->st_mode = S_IFREG | 0644;
	}
	else {
		stbuf->st_mode = S_IFDIR | 0755;
	}
	stbuf->st_atime = time(0);
	stbuf->st_mtime = time(0);
	free(curr_inode);
	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));

	// Step 2: If not find, return -1
	if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 1) {
		free(curr_inode);
		return -ENOTDIR;
	}
	free(curr_inode);
	return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));
	if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 1) {
		free(curr_inode);
		return -ENOTDIR;
	}

	// Step 2: Read directory entries from its data blocks, and copy them to filler
	int curr_direct;
	int curr_offset;
	char buf[BLOCK_SIZE] = {};
	struct dirent *curr = malloc(sizeof(struct dirent));
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], buf);
		for(curr_offset = 0; (dirent_capacity - 1) * sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {
			memcpy(curr, buf + curr_offset, sizeof(struct dirent));
			if(strcmp(curr->name, "") == 0)
				return 0;
			filler(buffer, curr->name, NULL, 0);
		}
	}
	free(curr_inode);
	return 0;
}


static int tfs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode *parentdir = malloc(sizeof(struct inode));
	if (get_node_by_path(dpath, 0, parentdir) != 0) {
		free(parentdir);
		return -ENOENT;
	}
	if (parentdir->type != 1) {
		free(parentdir);
		return -ENOENT;
	}

	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	if(dir_add(*parentdir, (uint16_t)ino, bname, strlen(bname)) != 0) 		{		
		unsigned char buf[BLOCK_SIZE] = {};
		bio_read(sb.i_bitmap_blk, buf);
		unset_bitmap((bitmap_t)buf, ino);
		bio_write(sb.i_bitmap_blk, buf);
		return -ENFILE;
	}

	// Step 5: Update inode for target directory
	struct inode *new = malloc(sizeof(struct inode));
	readi(ino, new);
	new->ino = (uint16_t)ino;
	new->valid = 1;
	new->size = 0;
	new->type = 1;
	new->link = 2;

	int i;
	for(i = 0; i <= 15; i++) {
		new->direct_ptr[i] = -1;
		if(i <= 7)
			new->indirect_ptr[i] = -1;
	}

	new->vstat.st_mtime = time(0);
	new->vstat.st_atime = time(0);

	// Step 6: Call writei() to write inode to disk
	writei(ino, new);

	return 0;
}

static int tfs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);

	// Step 2: Call get_node_by_path() to get inode of target directory
	struct inode *curr_inode = malloc(sizeof(struct inode));
	if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 1) {
		free(curr_inode);
		return -ENOTDIR;
	}
	if (strcmp(path, "/") == 0) {
		printf("Somehow, somewhere, I am angrily shaking my fist at you for trying to delete the root directory. Try it again and see what happens, punk.\n");
		return -EPERM;	
	}

	unsigned char bitmap_buf[BLOCK_SIZE] = {};
	char data_buf[BLOCK_SIZE] = {};
	int curr_direct;

	// Step 3: Clear data block bitmap of target directory
	bio_read(sb.d_bitmap_blk, bitmap_buf);

	for (curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], data_buf);
		memset(data_buf, 0, BLOCK_SIZE);
		bio_write(curr_inode->direct_ptr[curr_direct], data_buf);
		unset_bitmap((bitmap_t)bitmap_buf, curr_inode->direct_ptr[curr_direct]);		
		curr_inode->direct_ptr[curr_direct] = -1;		
	}
	bio_write(sb.d_bitmap_blk, bitmap_buf);

	// Step 4: Clear inode bitmap and its data block
	curr_inode->valid = 0;
	curr_inode->type = 0;
	curr_inode->link = 0;
	writei(curr_inode->ino, curr_inode);	

	bio_read(sb.i_bitmap_blk, bitmap_buf);
	unset_bitmap((bitmap_t)bitmap_buf, curr_inode->ino);
	bio_write(sb.i_bitmap_blk, bitmap_buf);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	if (get_node_by_path(dpath, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 1) {
		free(curr_inode);
		return -ENOENT;
	}

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
	dir_remove(*curr_inode, bname, strlen(bname));
	free(curr_inode);
	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);
	struct inode *parentdir = malloc(sizeof(struct inode));

	// Step 2: Call get_node_by_path() to get inode of parent directory
	if (get_node_by_path(dpath, 0, parentdir) != 0) {
		free(parentdir);
		return -ENOENT;
	}
	if (parentdir->type != 1) {
		free(parentdir);
		return -ENOENT;
	}

	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();

	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	if(dir_add(*parentdir, (uint16_t)ino, bname, strlen(bname)) != 0) {		
		unsigned char buf[BLOCK_SIZE] = {};
		bio_read(sb.i_bitmap_blk, buf);
		unset_bitmap((bitmap_t)buf, ino);
		bio_write(sb.i_bitmap_blk, buf);
		return -ENFILE;
	}

	// Step 5: Update inode for target file
	struct inode *new = malloc(sizeof(struct inode));
	readi(ino, new);
	new->ino = (uint16_t)ino;
	new->valid = 1;
	new->size = 0;
	new->type = 0;
	new->link = 1;
	int i;
	for(i = 0; i <= 15; i++) {
		new->direct_ptr[i] = -1;
		if(i <= 7)
			new->indirect_ptr[i] = -1;
	}

	// Step 6: Call writei() to write inode to disk
	writei(ino, new);
	free(parentdir);
	free(new);
	printf("Created.\n");
	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {
	struct inode *opened = malloc(sizeof(struct inode));

	// Step 1: Call get_node_by_path() to get inode from path
	if(get_node_by_path(path, 0, opened) != 0 || opened->type != 0) {
		// Step 2: If not find, return -1
		free(opened);
		return -ENOENT;
	}
	free(opened);
	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	int size_cp = size;

	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));
		if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 0) {
		free(curr_inode);
		return -EISDIR;
	}

	// Step 2: Based on size and offset, read its data blocks from disk
	int start = offset / BLOCK_SIZE;
	int read_offset = 0;
	int indirect_capacity = BLOCK_SIZE/sizeof(int);
	char buf[BLOCK_SIZE] = {};
	int curr_direct = start, curr_indirect = start;

	// Step 2a: Write direct blocks from buffer
	for (; (curr_direct <= 15 && size_cp > 0); curr_direct++){
		if (curr_inode->direct_ptr[curr_direct] == -1) {
			printf("Trying to read more than is allocated!\n");
		}
		bio_read(curr_inode->direct_ptr[curr_direct], buf); //read block so data is not overwritten as null
		if(curr_direct == start){
			if(offset % BLOCK_SIZE + size_cp < BLOCK_SIZE) {
				memcpy(buffer, (buf + (offset % BLOCK_SIZE)), size_cp);
				read_offset += size_cp;
				size_cp = 0;
			}
			else {
				memcpy(buffer, (buf + (offset % BLOCK_SIZE)), (BLOCK_SIZE - (offset % BLOCK_SIZE)));
				read_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
				size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
			}
		} 
		else {
			if(size_cp > BLOCK_SIZE){
				memcpy((buffer + read_offset), buf, BLOCK_SIZE);
				read_offset += BLOCK_SIZE;
				size_cp -= BLOCK_SIZE;
			}
			else {
				memcpy((buffer + read_offset), buf, size_cp);
				size_cp -= size_cp;
			}
		}
	}

	//Step 2b: Copy indirect blocks into buffer
	int indir_only = 0;
	if (curr_direct == curr_indirect) {	//direct blocks not used - offset too large
		curr_direct = (curr_direct - 16) / indirect_capacity;
		curr_indirect = (start - 16) % indirect_capacity;
		indir_only = 1;
	}
	else if (curr_indirect < curr_direct && size > 0) {	//direct blocks used - still more to read
		curr_direct = 0;
		curr_indirect = 0;
	}
	int blocks[BLOCK_SIZE/sizeof(int)];
	for (; curr_direct <= 7 && size_cp > 0; curr_direct++) {
		if (curr_inode->indirect_ptr[curr_direct] == -1) {
			printf("Trying to read more than is allocated!\n");
		}
		bio_read(curr_inode->indirect_ptr[curr_direct], blocks);
		for (; curr_indirect < indirect_capacity && size_cp > 0; curr_indirect++){
			if(blocks[curr_indirect] == 0){ 
				printf("Trying to read more than is allocated!\n");
			}
			bio_read(blocks[curr_indirect], buf);	//read block so data is not overwritten as null
			if(indir_only) {
				if((offset % BLOCK_SIZE) + size_cp < BLOCK_SIZE) {
					memcpy(buffer, (buf + (offset % BLOCK_SIZE)), size_cp);
					read_offset += size_cp;
					size_cp = 0;
				}
				else {
					memcpy(buffer, (buf + (offset % BLOCK_SIZE)), (BLOCK_SIZE - (offset % BLOCK_SIZE)));
					read_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
					size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));

				}
				indir_only = 0;
			} 
			else {
				if(size_cp > BLOCK_SIZE) {
					memcpy((buffer + read_offset), buf, BLOCK_SIZE);
					read_offset += BLOCK_SIZE;
					size_cp -= BLOCK_SIZE;
				}
				else {
					memcpy((buffer + read_offset), buf, size_cp);
					size_cp = 0;
				}
			}
			bio_write(blocks[curr_indirect], buf);
		}
		curr_indirect = 0;
		bio_write(curr_inode->indirect_ptr[curr_direct], blocks); //update indirect block
	}
	free(curr_inode);
	return (size - size_cp);
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	int size_cp = size;

	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));
	if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 0) {
		free(curr_inode);
		return -EISDIR;
	}

	// Step 2: Based on size and offset, read its data blocks from disk
	int start = offset / BLOCK_SIZE;
	int write_offset = 0;
	int indirect_capacity = BLOCK_SIZE / sizeof(int);
	char buf[BLOCK_SIZE] = {};
	int curr_direct = start, curr_indirect = start;

	// Step 2a: Write direct blocks from buffer
	for (; (curr_direct <= 15 && size_cp > 0); curr_direct++){
		if (curr_inode->direct_ptr[curr_direct] == -1){
			curr_inode->direct_ptr[curr_direct] = get_avail_blkno();
			curr_inode->link++;
		}
		bio_read(curr_inode->direct_ptr[curr_direct], buf); //read block so data is not overwritten as null
		if(curr_direct == start){
			if((offset % BLOCK_SIZE + size_cp) < BLOCK_SIZE) {
				memcpy((buf + (offset % BLOCK_SIZE)), buffer, size_cp);
				curr_inode->size += size_cp;
				write_offset += size_cp;
				size_cp = 0;
			}
			else {
				memcpy((buf + (offset % BLOCK_SIZE)), buffer, (BLOCK_SIZE - (offset % BLOCK_SIZE)));
				write_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
				curr_inode->size += (BLOCK_SIZE - (offset % BLOCK_SIZE));
				size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
			}
		} 
		else {
			if(size_cp > BLOCK_SIZE){
				memcpy(buf, (buffer + write_offset), BLOCK_SIZE);
				write_offset += BLOCK_SIZE;
				size_cp -= BLOCK_SIZE;
				curr_inode->size += BLOCK_SIZE;
			}
			else {
				memcpy(buf, (buffer + write_offset), size_cp);
				curr_inode->size += size_cp;
				size_cp = 0;
			}
		}
		bio_write(curr_inode->direct_ptr[curr_direct], buf);
	}

	//step 2b: Write indirect blocks from buffer
	int indir_only = 0;
	if (curr_direct == curr_indirect) {	//direct blocks not used - offset too large
		curr_direct = (start - 16) / indirect_capacity;
		curr_indirect = (start - 16) % indirect_capacity;
		indir_only = 1;
	}
	else if (curr_indirect < curr_direct && size > 0) {	//direct blocks used - still more to read
		curr_direct = 0;
		curr_indirect = 0;
	}
	int blocks[BLOCK_SIZE/sizeof(int)];
	for (; curr_direct <= 7 && size_cp > 0; curr_direct++) {
		if (curr_inode->indirect_ptr[curr_direct] == -1) {
			curr_inode->indirect_ptr[curr_direct] = get_avail_blkno();
		}
		bio_read(curr_inode->indirect_ptr[curr_direct], blocks);
		for (; curr_indirect < indirect_capacity && size_cp > 0; curr_indirect++){
			if(blocks[curr_indirect] == 0){ 
				blocks[curr_indirect] = get_avail_blkno();
			}
			bio_read(blocks[curr_indirect], buf);	//read block so data is not overwritten as null
			if(indir_only) {
				if((offset % BLOCK_SIZE) + size_cp < BLOCK_SIZE) {
					memcpy((buf + (offset % BLOCK_SIZE)), buffer, size_cp);
					write_offset += size_cp;
					curr_inode->size += size_cp;
					size_cp = 0;
					indir_only = 0;
				}
				else {
					memcpy((buf + (offset % BLOCK_SIZE)), buffer, (BLOCK_SIZE - (offset % BLOCK_SIZE)));
					write_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
					curr_inode->size += (BLOCK_SIZE - (offset % BLOCK_SIZE));
					size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
				}
			} 
			else {
				if(size_cp > BLOCK_SIZE) {
					memcpy(buf, (buffer + write_offset), BLOCK_SIZE);
					write_offset += BLOCK_SIZE;
					size_cp -= BLOCK_SIZE;
					curr_inode->size += BLOCK_SIZE;
				}
				else {
					memcpy(buf, (buffer + write_offset), size_cp);
					curr_inode->size += size_cp;
					size_cp = 0;
				}
			}
			bio_write(blocks[curr_indirect], buf);
		}
		curr_indirect = 0;
		bio_write(curr_inode->indirect_ptr[curr_direct], blocks); //update indirect block
	}
	if(size != 0) {
		writei(curr_inode->ino, curr_inode);
	}

	// Step 3: Write the correct amount of data from offset to disk
	//done in above code

	// Step 4: Update the inode info and write it to disk
	//done in above code

	// Note: this function should return the amount of bytes you write to disk*/
	free(curr_inode);
	return (size - size_cp);
}

static int tfs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);

	// Step 2: Call get_node_by_path() to get inode of target directory
	struct inode *curr_inode = malloc(sizeof(struct inode));
			if (get_node_by_path(path, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 0) {
		free(curr_inode);
		return -EISDIR;
	}

	unsigned char bitmap_buf[BLOCK_SIZE] = {};
	char data_buf[BLOCK_SIZE] = {};
	int curr_direct;
	int curr_indirect;
	int which_block;
	int blocks[BLOCK_SIZE / sizeof(int)];

	// Step 3: Clear data block bitmap of target directory
	bio_read(sb.d_bitmap_blk, bitmap_buf);
	for (curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], data_buf);
		memset(data_buf, 0, BLOCK_SIZE);
		bio_write(curr_inode->direct_ptr[curr_direct], data_buf);
		unset_bitmap((bitmap_t)bitmap_buf, curr_inode->direct_ptr[curr_direct]);
		curr_inode->direct_ptr[curr_direct] = -1;
	}

	for(curr_indirect = 0; curr_indirect <= 7; curr_indirect++) {
		if(curr_inode->indirect_ptr[curr_indirect] == -1)
			continue;
		bio_read(curr_inode->indirect_ptr[curr_indirect], blocks);
		for(which_block = 0; which_block < BLOCK_SIZE / sizeof(int); which_block++) {
			if(blocks[which_block] == 0)
				continue;
			bio_read(blocks[which_block], data_buf);
			memset(data_buf, 0, BLOCK_SIZE);
			bio_write(blocks[which_block], data_buf);
			unset_bitmap((bitmap_t)bitmap_buf, blocks[which_block]);
			blocks[which_block] = 0;
		}
		bio_write(curr_inode->indirect_ptr[curr_indirect], blocks);
		unset_bitmap((bitmap_t)bitmap_buf, curr_inode->indirect_ptr[curr_indirect]);
		curr_inode->indirect_ptr[curr_indirect] = -1;
	}
	bio_write(sb.d_bitmap_blk, bitmap_buf);

	// Step 4: Clear inode bitmap and its data block

	curr_inode->valid = 0;
	curr_inode->type = 0;
	curr_inode->link = 0;
	writei(curr_inode->ino, curr_inode);	
	
	bio_read(sb.i_bitmap_blk, bitmap_buf);
	unset_bitmap((bitmap_t)bitmap_buf, curr_inode->ino);
	bio_write(sb.i_bitmap_blk, bitmap_buf);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	if (get_node_by_path(dpath, 0, curr_inode) != 0) {
		free(curr_inode);
		return -ENOENT;
	}
	if (curr_inode->type != 1) {
		free(curr_inode);
		return -ENOENT;
	}

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
	dir_remove(*curr_inode, bname, strlen(bname));
	free(curr_inode);
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
