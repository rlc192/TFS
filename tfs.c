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
	printf("In get_avail_ino()\n");
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
	printf("In get_avail_blkno()\n");
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
	printf("In readi(). Args: %d, some inode\n", (int)ino);
	// Step 1: Get the inode's on-disk block number
	unsigned int inode_blkno = sb.i_start_blk + (unsigned int)ino / (BLOCK_SIZE / sizeof(struct inode));
	// Step 2: Get offset of the inode in the inode on-disk block
	unsigned int inode_offset = (unsigned int) ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode);
	printf("Block %d, Offset %d\n", (int) inode_blkno, (int)inode_offset);
	// Step 3: Read the block from disk and then copy into inode structure
	char *buf = malloc(sizeof(char) * BLOCK_SIZE);
	bio_read(inode_blkno, buf);
	struct inode *temp_inode = malloc(sizeof(struct inode));
	memcpy(temp_inode, buf + inode_offset, sizeof(struct inode));
	*inode = *temp_inode;
	printf("Ended up reading inode with number %d.\n", (int)(temp_inode->ino));
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	printf("In writei(). Args: %d, some inode\n", (int)ino);
	// Step 1: Get the block number where this inode resides on disk
	unsigned int inode_blkno = sb.i_start_blk + (unsigned int)ino / (BLOCK_SIZE / sizeof(struct inode));
	// Step 2: Get the offset in the block where this inode resides on disk
	unsigned int inode_offset = (unsigned int) ino % (BLOCK_SIZE / sizeof(struct inode)) * sizeof(struct inode);
	printf("Block %d, Offset %d\n", (int) inode_blkno, (int)inode_offset);
	// Step 3: Write inode to disk
	char buf[BLOCK_SIZE];
	bio_read(inode_blkno, buf);
	memcpy((buf + inode_offset), inode, sizeof(struct inode));
	bio_write(inode_blkno, buf);
	printf("Ended up writing inode with number %d.\n", (int)(((struct inode *)(buf + inode_offset))->ino));
	return 0;
}


/*
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	printf("In dir_find(). Args: ino %d, fname %s, name_len %d\n", (int)ino, fname, (int)name_len);
	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode *curr_inode = malloc(sizeof(struct inode));
	readi(ino, curr_inode);
	// Step 2: Get data block of current directory from inode
	int curr_direct;
	int curr_offset;
	unsigned char buf[BLOCK_SIZE];
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		printf("\tCurr direct: %d\n", curr_inode->direct_ptr[curr_direct]);
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], buf);
		//printf("%.*s\n", BLOCK_SIZE, buf);
		for(curr_offset = 0; curr_offset <= BLOCK_SIZE - sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {
			printf("Name: %s ", ((struct dirent*)(buf + curr_offset))->name);
			//printf("\t\tCurr offset: %d\n", curr_offset);
			if(memcmp(((struct dirent *)(buf + curr_offset))->name, fname, name_len) == 0) {
				memcpy(curr_dirent, buf + curr_offset, sizeof(struct dirent));
				goto found;
			}
		}
	}
	free(curr_dirent);
	printf("Dir not found!\n");
	return -1; //Not Found

	// Step 3: Read directory's data block and check each directory entry.
	//If the name matches, then copy directory entry to dirent structure
	found:
		*dirent = *curr_dirent;
		printf("Dir found, its inode number is %d\n", (int)dirent->ino);
		return 0;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	printf("In dir_add(). Args: ino %d, fname %s, name_len %d\n", (int)f_ino, fname, (int)name_len);
	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	// Step 2: Check if fname (directory name) is already used in other entries
	struct dirent *curr_dirent = malloc(sizeof(struct dirent));
	if(dir_find(dir_inode.ino, fname, name_len, curr_dirent) != -1) {
		printf("Duplicate name!\n");
		return -1; //Duplicate Name
	}
	// Step 3: Add directory entry in dir_inode's data block and write to disk

	unsigned char buf[BLOCK_SIZE] = {};
	int block = dir_inode.size / BLOCK_SIZE;
	int offset = dir_inode.size % BLOCK_SIZE;

	// Allocate a new data block for this directory if it does not exist
	if(dir_inode.direct_ptr[block] == -1) {
		dir_inode.direct_ptr[block] = get_avail_blkno();
		printf("New block %d\n", dir_inode.direct_ptr[block]);
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
	if(offset + sizeof(struct dirent) > BLOCK_SIZE)
		dir_inode.size += (BLOCK_SIZE % sizeof(struct dirent)); //Extra padding
	writei(dir_inode.ino, &dir_inode);
	// Write directory entry
	bio_write(dir_inode.direct_ptr[block], buf);
	printf("Dirent added!\n");
	return 0;

}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
	printf("In dir_remove(). Args: fname %s, name_len %d\n", fname, (int)name_len);
	int curr_direct;
	int curr_offset;
	unsigned char buf[BLOCK_SIZE] = {};
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		if(dir_inode.direct_ptr[curr_direct] == -1)
			continue;
		bio_read(dir_inode.direct_ptr[curr_direct], buf);
		// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
		for(curr_offset = 0; curr_offset <= BLOCK_SIZE - sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {
			// Step 2: Check if fname exist
			if(memcmp(((struct dirent *)(buf + curr_offset))->name, fname, name_len) == 0)
				goto found;
		}
	}
	printf("Name not found to remove!\n");
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
			dir_inode.direct_ptr[lastblock] = -1; //Remove pointer to direct block
			bio_read(sb.d_bitmap_blk, bitmapbuf);
			unset_bitmap((bitmap_t)bitmapbuf, lastblock);
			bio_write(sb.d_bitmap_blk, bitmapbuf); //Free direct block in bitmap
			dir_inode.link--;
		}
		writei(dir_inode.ino, &dir_inode);
		printf("Dirent removed!\n");
		return 0;
}

/*
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	printf("In get_node_by_path(). Args: Path %s, ino %d\n", path, (int)ino);
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	char* token = strdup(path);
	char* rest;
	struct dirent *target = malloc(sizeof(struct dirent));
	token = strtok_r(token, "/", &rest);
	printf("Token: %s, Rest: %s\n", token, rest);
	if (token == NULL) {
		readi(ino, inode);
		printf("Ended up with inode with number %d.\n", (int)(inode->ino));
		return 0;
	}
	if (dir_find(ino, token, strlen(token), target) == 0) {
		return get_node_by_path(rest, target->ino, inode);
	}
	else {
		printf("Path searched for not valid!\n");
		return -1;
	}
}

/*
 * Make file system
 */
int tfs_mkfs() {
	printf("No file found, making the file system!\n");
	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);

	// write superblock information
	sb.magic_num = MAGIC_NUM;
	sb.max_inum = MAX_INUM;
	sb.max_dnum = MAX_DNUM;
	sb.i_bitmap_blk = 1;
	sb.d_bitmap_blk = 2;
	sb.i_start_blk = 3;
	sb.d_start_blk = MAX_INUM * sizeof(struct inode) / BLOCK_SIZE + 3;
	char buff[BLOCK_SIZE];
	memcpy(buff, &sb, sizeof(sb));
	bio_write(0, buff);

	// initialize inode bitmap
	unsigned char inode_bitmap [MAX_INUM/8];
	memset(inode_bitmap, 0, MAX_INUM/8);
	// update bitmap information for root directory
	set_bitmap((bitmap_t)inode_bitmap, 0);
	bio_write(sb.i_bitmap_blk, inode_bitmap);

	// initialize data block bitmap
	unsigned char data_bitmap [MAX_DNUM/8];
	memset(data_bitmap, 0, MAX_DNUM/8);
	bio_write(sb.d_bitmap_blk, data_bitmap);

	// update inode for root directory
	struct inode root;
	root.ino = 0;
	root.valid = 1;
	root.size = 0;
	root.type = 1;
	root.link = 1;

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
	int check = get_node_by_path(path, 0, curr_inode);
	printf("Check: %d\n", check);
	if(check == 0) {
		// Step 2: fill attribute of file into stbuf from inode
		//*stbuf = curr_inode->vstat;
		//stbuf->st_dev =
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
			stbuf->st_mode   = S_IFDIR | 0755;
		}
		stbuf->st_atime = time(0);
		stbuf->st_mtime = time(0);
		return 0;
	}
	return -ENOENT;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {
	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));

	// Step 2: If not find, return -1
	if ((get_node_by_path(path, 0, curr_inode) != 0) || (curr_inode->type != 1))
		return -ENOENT;
	printf("%d\n\n\n\n\n", curr_inode->type);
	return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));


	//free before return? ^^^^^

	if ((get_node_by_path(path, 0, curr_inode) != 0) || (curr_inode->type != 1)){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", path);
		return -ENOENT;
	}
	if (curr_inode->type != 1){
		fprintf(stderr, "ERROR:NOT A DIRECTORY\n");
		return -EBADF;
	}

	printf("In readdir.\n");
	// Step 2: Read directory entries from its data blocks, and copy them to filler
	int curr_direct;
	int curr_offset;
	char buf[BLOCK_SIZE] = {};
	struct dirent *curr = malloc(sizeof(struct dirent));
	for(curr_direct = 0; curr_direct <= 15; curr_direct++) {
		//printf("\tCurr direct: %d\n", curr_inode->direct_ptr[curr_direct]);
		if(curr_inode->direct_ptr[curr_direct] == -1)
			continue;
		bio_read(curr_inode->direct_ptr[curr_direct], buf);
		for(curr_offset = 0; curr_offset <= BLOCK_SIZE - sizeof(struct dirent); curr_offset += sizeof(struct dirent)) {
			//printf("Name: %s ", ((struct dirent*)(buf + curr_offset))->name);
			//printf("\t\tCurr offset: %d\n", curr_offset);
			memcpy(curr, buf + curr_offset, sizeof(struct dirent));
			if(strcmp(curr->name, "") == 0)
				return 0;
			filler(buffer, curr->name, NULL, 0);
		}
	}
	return 0;
}


static int tfs_mkdir(const char *path, mode_t mode) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);
	printf("In mkdir(). Dir and base: %s, %s\n", dpath, bname);
	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode *curr_inode = malloc(sizeof(struct inode));

	//free ater step 6? ^^^^^

	if (get_node_by_path(dpath, 0, curr_inode) != 0){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", dpath);
		return -1;
	}
	printf("Temp inode number: %d", (int) curr_inode->ino);
	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	printf("Trying to make the dir...\n");
	dir_add(*curr_inode, ino, bname, strlen(bname));

	// Step 5: Update inode for target directory
	curr_inode->ino = ino;
	curr_inode->valid = 1;
	curr_inode->size = 0;
	curr_inode->type = 1;
	curr_inode->link = 1;

	int i;
	for(i = 0; i <= 15; i++) {
		curr_inode->direct_ptr[i] = -1;
		if(i <= 7)
			curr_inode->indirect_ptr[i] = -1;
	}

	curr_inode->vstat.st_mtime = time(0);
	curr_inode->vstat.st_atime = time(0);

	// Step 6: Call writei() to write inode to disk
	writei(ino, curr_inode);

	return 0;
}

static int tfs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);
	printf("In rmdir(). Dir and base: %s, %s\n", dpath, bname);

	// Step 2: Call get_node_by_path() to get inode of target directory
	struct inode *curr_inode = malloc(sizeof(struct inode));


	//free ater step 6? ^^^^^

	if (get_node_by_path(path, 0, curr_inode) || curr_inode->type != 1){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", dpath);
		return -1;
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
		curr_inode->direct_ptr[curr_direct] = -1;
		unset_bitmap((bitmap_t)bitmap_buf, curr_inode->direct_ptr[curr_direct]);
	}
	bio_write(sb.d_bitmap_blk, bitmap_buf);

	printf("Data block stuff done.\n");
	// Step 4: Clear inode bitmap and its data block
	curr_inode->valid = 0;
	curr_inode->type = 0;
	curr_inode->link = 0;
	
	bio_read(sb.i_bitmap_blk, bitmap_buf);
	unset_bitmap((bitmap_t)bitmap_buf, curr_inode->ino);
	bio_write(sb.i_bitmap_blk, bitmap_buf);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	if (get_node_by_path(dpath, 0, curr_inode) != 0 || curr_inode->type != 1){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", dpath);
		return -1;
	}

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
	dir_remove(*curr_inode, bname, strlen(bname));
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
	unsigned char buf[BLOCK_SIZE];
	// Step 2: Call get_node_by_path() to get inode of parent directory
	int check = get_node_by_path(dpath, 0, parentdir);
	printf("Check: %d\n", check);
	if(check != 0 || parentdir->type != 1)
		return -1;
	// Step 3: Call get_avail_ino() to get an available inode number
	int ino = get_avail_ino();
	// Step 4: Call dir_add() to add directory entry of target file to parent directory
	if(dir_add(*parentdir, (uint16_t)ino, bname, strlen(bname)) != 0) {		
		printf("We got issues...\n");
		bio_read(sb.i_bitmap_blk, buf);
		unset_bitmap((bitmap_t)buf, ino);
		return -1;
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
	//set stat
	// Step 6: Call writei() to write inode to disk
	writei(ino, new);
	printf("Created.\n");
	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {
	struct inode *opened = malloc(sizeof(struct inode));
	// Step 1: Call get_node_by_path() to get inode from path
	if(get_node_by_path(path, 0, opened) != 0 || opened->type != 0) {
		// Step 2: If not find, return -1
		return -1;
	}
	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	printf("In tfs_read(). Offset here is %d, start here is %d\n", (int) offset, (int)offset / BLOCK_SIZE);
	int size_cp = size;
	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));


	//free ater step 4? ^^^^^

	if (get_node_by_path(path, 0, curr_inode) != 0 || curr_inode->type != 0){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", path);
		return -1;
	}
	// Step 2: Based on size and offset, read its data blocks from disk
	int start = offset / BLOCK_SIZE;
	int write_offset = 0;
	int indirect_capacity = BLOCK_SIZE/sizeof(int);
	char buf[BLOCK_SIZE] = {};
	int curr_direct = start, curr_indirect = start;

	// Step 2a: Write direct blocks from buffer
	for (; (curr_direct <= 15 && size_cp > 0); curr_direct++){
		printf("Curr Direct: %d. Size: %d\n", curr_direct, size_cp); 
		if (curr_inode->direct_ptr[curr_direct] == -1) {
			printf("Trying to read more than is allocated!\n");
		}
		bio_read(curr_inode->direct_ptr[curr_direct], buf); //read block so data is not overwritten as null
		if(curr_direct == start){
			if(offset % BLOCK_SIZE + size_cp < BLOCK_SIZE) {
				memcpy(buffer, (buf + (offset % BLOCK_SIZE)), size_cp);
				write_offset += size_cp;
				size_cp = 0;
			}
			else {
				memcpy(buffer, (buf + (offset % BLOCK_SIZE)), (BLOCK_SIZE - (offset % BLOCK_SIZE)));
				write_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
				size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
			}
		} 
		else {
			if(size_cp > BLOCK_SIZE){
				memcpy((buffer + write_offset), buf, BLOCK_SIZE);
				write_offset += BLOCK_SIZE;
				size_cp -= BLOCK_SIZE;
			}
			else {
				memcpy((buffer + write_offset), buf, size_cp);
				size_cp -= size_cp;
			}
		}
	}

	//step 2b: Copy indirect blocks into buffer
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
		printf("Curr (in)Direct: %d. Size: %d\n", curr_direct, size_cp); 
		if (curr_inode->indirect_ptr[curr_direct] == -1) {
			printf("Trying to read more than is allocated!\n");
		}
		bio_read(curr_inode->indirect_ptr[curr_direct], blocks);
		for (; curr_indirect < indirect_capacity && size_cp > 0; curr_indirect++){
			printf("\tCurr Indirect: %d. Size: %d\n", curr_indirect, size_cp); 
			if(blocks[curr_indirect] == 0){ 
				printf("Trying to read more than is allocated!\n");
			}
			bio_read(blocks[curr_indirect], buf);	//read block so data is not overwritten as null
			if(indir_only) {
				if((offset % BLOCK_SIZE) + size_cp < BLOCK_SIZE) {
					memcpy(buffer, (buf + (offset % BLOCK_SIZE)), size_cp);
					write_offset += size_cp;
					size_cp = 0;
				}
				else {
					memcpy(buffer, (buf + (offset % BLOCK_SIZE)), (BLOCK_SIZE - (offset % BLOCK_SIZE)));
					write_offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
					size_cp -= (BLOCK_SIZE - (offset % BLOCK_SIZE));

				}
				indir_only = 0;
			} 
			else {
				if(size_cp > BLOCK_SIZE) {
					memcpy((buffer + write_offset), buf, BLOCK_SIZE);
					write_offset += BLOCK_SIZE;
					size_cp -= BLOCK_SIZE;
				}
				else {
					memcpy((buffer + write_offset), buf, size_cp);
					size_cp = 0;
				}
			}
			bio_write(blocks[curr_indirect], buf);
		}
		bio_write(curr_inode->indirect_ptr[curr_direct], blocks); //update indirect block
	}
	printf("Remaining: %d\n", (int)(size - size_cp));
	return (size - size_cp);
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	printf("In tfs_write(). Offset here is %d, start here is %d\n", (int) offset, (int)offset / BLOCK_SIZE);
	int size_cp = size;
	// Step 1: You could call get_node_by_path() to get inode from path
	struct inode *curr_inode = malloc(sizeof(struct inode));


	//free ater step 4? ^^^^^

	if (get_node_by_path(path, 0, curr_inode) != 0 || curr_inode->type != 0){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", path);
		return -1;
	}
	// Step 2: Based on size and offset, read its data blocks from disk
	int start = offset / BLOCK_SIZE;
	int write_offset = 0;
	int indirect_capacity = BLOCK_SIZE/sizeof(int);
	char buf[BLOCK_SIZE] = {};
	int curr_direct = start, curr_indirect = start;

	// Step 2a: Write direct blocks from buffer
	for (; (curr_direct <= 15 && size_cp > 0); curr_direct++){
		printf("Curr Direct: %d. Size: %d\n", curr_direct, size_cp); 
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
		printf("Curr (in)Direct: %d. Size: %d\n", curr_direct, size_cp); 
		if (curr_inode->indirect_ptr[curr_direct] == -1) {
			curr_inode->indirect_ptr[curr_direct] = get_avail_blkno();
		}
		bio_read(curr_inode->indirect_ptr[curr_direct], blocks);
		for (; curr_indirect < indirect_capacity && size_cp > 0; curr_indirect++){
			printf("\tCurr Indirect: %d. Size: %d\n", curr_indirect, size_cp); 
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
	return (size - size_cp);
}

static int tfs_unlink(const char *path) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char *dirc, *basec, *bname, *dpath;
	dirc = strdup(path);
	basec = strdup(path);
	dpath = dirname(dirc);
	bname = basename(basec);
	printf("In rmdir(). Dir and base: %s, %s\n", dpath, bname);

	// Step 2: Call get_node_by_path() to get inode of target directory
	struct inode *curr_inode = malloc(sizeof(struct inode));


	//free ater step 6? ^^^^^

	if (get_node_by_path(path, 0, curr_inode) != 0 || curr_inode->type != 0){
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", dpath);
		return -1;
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
		curr_inode->direct_ptr[curr_direct] = -1;
		unset_bitmap((bitmap_t)bitmap_buf, curr_inode->direct_ptr[curr_direct]);
	}
	bio_write(sb.d_bitmap_blk, bitmap_buf);
	printf("Data block stuff done.\n");
	// Step 4: Clear inode bitmap and its data block

	curr_inode->valid = 0;
	curr_inode->type = 0;
	curr_inode->link = 0;
	
	bio_read(sb.i_bitmap_blk, bitmap_buf);
	unset_bitmap((bitmap_t)bitmap_buf, curr_inode->ino);
	bio_write(sb.i_bitmap_blk, bitmap_buf);

	// Step 5: Call get_node_by_path() to get inode of parent directory
	if (get_node_by_path(dpath, 0, curr_inode) != 0 || curr_inode->type != 1) {
		fprintf(stderr, "ERROR:NO NODE FOUND AT PATH \"%s\"\n", dpath);
		return -1;
	}

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
	dir_remove(*curr_inode, bname, strlen(bname));
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
