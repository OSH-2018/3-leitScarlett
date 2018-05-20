#define _OSH_FS_VERSION 2018051000
#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <stdio.h>



//size_t是8个字节大小
static const size_t max_file_num 14 * 64;
static const size_t size 4 * 1024 * 1024 * (size_t)1024;
static void *mem[64 * 1024];
size_t blocknr = sizeof(mem) / sizeof(mem[0]);
size_t blocksize = 64*1024;

struct file_data{
    size_t content[256]; //指向文件对应的数据块
    struct file_data *next; //万一不够还可以扩充
}
struct filenode { //文件节点
    char *filename; //文件名
    size_t order; //文件编号
    struct file_data *data ; //指向文件对应的数据块
    struct stat *st; //文件基本信息块
};
 
struct super_block{
    int file_num;// <= 15 * 64
    //int data_num = 64*1024-16;
    bool *file_map;//记录哪些文件节点占用情况(最多15*64）
    
};

bool *data_map;//记录mem的块占用情况（最多64*1024-1)
static struct filenode *file;//[14 * 64]
//用数组不用指针的原因是可以用i与file_map对应


//获取文件节点
static struct filenode *get_filenode(const char *name)
{
    for(int i = 0; i < max_file_num ;i++){
        if(file_map[i]){
            if(strcmp(file[i].filename, name + 1)==0){
                return (file + i);
            }
        }
    }
    return NULL;
}

//创建文件
static void create_filenode(const char *filename, const struct stat *st)
{
    int file_order = -1;
    //需要加一个个数超过max_file_num满了吗
    for(int i = 0; i< max_file_num; i++){
        if(!file_map[i]){
            file_order = i;
            file_map[i] = 1;
            break;
        }
    }
    inode[file_order].order = file_order;
    memcpy(inode[file_order].filename, filename, strlen(filename) + 1);
    memcpy(&(inode[file_order].st), st, sizeof(struct stat));
    memset(inode[file_order].data.content, 0, 256*sizeof(size_t));
    inode[file_order].data.next = NULL;
}
// 初始化文件系统信息
static void *oshfs_init(struct fuse_conn_info *conn)
{
    for(int i = 0; i < 16; i++) {
        mem[i] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(mem[i], 0, blocksize);
    }

    struct super_block* information = (struct super_block*)mem[0];
    information->file_num = 0;
    memset(information->file_map, 0, max_file_num*sizeof(bool));
    data_map = (bool *)mem[1];
    memset(data_map, 0, (64*1024-16)*sizeof(bool));
    mem[2] = mmap(NULL, 13 * blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 2; i< 16; i++){
        mem[i] = (char*)mem[2] + blocksize * (i-2);
    }
    file = (struct filenode *)mem[2];
    memset(mem[3], 0, (13*64)*sizeof(blocksize));
    for (int i = 0;i<16;i++){
        ((information*)(mem[0]))->data_map[i] = 1;
    }

    return NULL;
}
// 得到文件属性
static int oshfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    struct filenode *node = get_filenode(path);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        memcpy(stbuf, node->st, sizeof(struct stat));
    } else {
        ret = -ENOENT;
    }
    return ret;
}
// 读文件属性
static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for(int i = 0; i < max_file_num; i++){
        if(file_map[i]){
            filler(buf, file[i].filename, &(file[i].st), 0);
        }
    }
   
    return 0;
}
// 创建一个新的文件
static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    int flag =1;
    for(int i = 0;i<max_file_num;i++){
        if(inode_map[i]==0){
            flag = 0;
        }
    }
    if(flag){
        return -errno;
    }//满了
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_filenode(path + 1, &st);
    return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
    return 0;
}
// 写文件
static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = NULL;
    node = get_filenode(path);
    if(!node){
        return -1;
    }
    //需要用多少块
    size_t block_used_num = node->st.st_size % blocksize ? (node->st.st_size / blocksize + 1) : (node->st.st_size / blocksize);
    size_t new_block_used_num = block_used_num;
    //改文件大小
    if(offset + size > node->st.st_size){
        node->st.st_size = offset + size;
    }
    if(offset + size > block_used_num * blocksize){  
        new_block_used_num = node->st.st_size % blocksize ? (node->st.st_size / blocksize + 1) : (node->st.st_size / blocksize);
        for(size_t i = block_used_num; i<new_block_used_num; i++){
            bool is_mem_full = 1;
            for(int j = 16;j <blocknr; j++){
                if(data_map[j] == 0){
                    is_mem_full = 0;
                    mem[j] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    data_map[j] = 1;
                    node->content[i] = j;//记录文件占用了哪些内存块
                    break;
                }
            }
            if(is_mem_full){
		printf("there is no enough space, please delete some useless files \n");
                return -errno;
            }
        }
    }

    size_t blk1 = offset / blocksize;
    size_t off1 = offset % blocksize;
    size_t blk2 = (offset + size) /blocksize;
    size_t off2 = (offset + size) %blocksize;
    if(blk1 == blk2){
        if(blk1<256)
            memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        else{
	    while(blk1>=256){
		node.data = node.data.next;
		blk1-=256;
	    }
	    memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        }
        return (int)size;
    }else{
        if(blk1<256)
            memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        else{
	    while(blk1>=256){
		node.data = node.data.next;
		blk1-=256;
		blk2-=256;
	    }
	    memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        }
        for(size_t i = blk1+1;i<blk2;i++){
            memcpy(mem[node->content[i]], buf+blocksize-off1 + (i-blk1-1)*blocksize, blocksize);

        }
        memcpy(mem[node->content[blk2]], buf+blocksize-off1 + (blk2-blk1-1)*blocksize, off2);
        return (int)size;
    }

    return (int)size;
}
//截断
static int oshfs_truncate(const char *path, off_t size)
{
    struct filenode *node = NULL;
    node = get_filenode(path);
    if(!node){
        return -1;
    }

    size_t block_used_num = node->st.st_size % blocksize ? (node->st.st_size / blocksize + 1) : (node->st.st_size / blocksize);
    size_t new_block_used_num = block_used_num;

    node->st.st_size = size;
    if(size > block_used_num * blocksize){
        new_block_used_num = node->st.st_size % blocksize ? (node->st.st_size / blocksize + 1) : (node->st.st_size / blocksize);
        for(size_t i = block_used_num; i<new_block_used_num; i++){
            bool is_mem_full = 1;
            for(int j = 16;j <blocknr; j++){

                if(data_map[j] == 0){
                    is_mem_full = 0;
                    mem[j] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    data_map[j] = 1;
                    node->content[i] = j;
                    break;
                }
            }
            if(is_mem_full){
		printf("there is no enough space, please delete some useless files \n");
                return -errno;
            }
        }
    }
    else if(size <= (block_used_num - 1) * blocksize ){
        new_block_used_num = node->st.st_size % blocksize ? (node->st.st_size / blocksize + 1) : (node->st.st_size / blocksize);
        for(size_t i = new_block_used_num; i< block_used_num; i++){
            data_map[node->content[i]] = 0;
            munmap(mem[node->content[i]], blocksize);
        }
    }

    return 0;
}
//读文件
static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    int ret = size;
    if(offset + size > node->st->st_size)
        ret = node->st->st_size - offset;
    int blk1 = offset / blocksize;
    int off1 = offset % blocksize;
    int blk2 = (offset + ret) /blocksize;
    int off2 = (offset + ret) %blocksize;
    if(blk1 == blk2){
        if(blk1<256)
            memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        else{
	    while(blk1>=256){
		node.data = node.data.next;
		blk1-=256;
	    }
	    memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        }
        return ret;
    }else{
        if(blk1<256)
            memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        else{
	    while(blk1>=256){
		node.data = node.data.next;
		blk1-=256;
		blk2-=256;
	    }
	    memcpy(buf, mem[node.data.content[blk1]]+off1,ret);
        }
        for(int i = blk1+1;i<blk2;i++){
            memcpy(buf+blocksize-off1 + (i-blk1-1)*blocksize,mem[node.data.content[i]],blocksize);

        }
        memcpy(buf+blocksize-off1 + (blk2-blk1-1)*blocksize, mem[node.data.content[blk2]], off2);
        return ret;
    }
    return ret;
}
// 删除文件
static int oshfs_unlink(const char *path)
{
    struct filenode *node = NULL;
    node = get_filenode(path);
    if(!node){
        return -1;
    }
    file_order = node->order;
    for(int i = 0;i<256;i++){
        if(file[file_order].data.content[i] < 2){
            break;
        }//因为01不是存文件的块
        //这里需要改进，完全不用循环中这么多，控制下没有了就停下来
        munmap(mem[file[file_order].data.content[i]],blocksize);
        data_map[file[file_order].data.content[i]] = 0;
    }
    while(file[file_order].data.next){
        file[file_order].data = file[file_order].data.next;
        for(int i = 0;i<256;i++){
            if(file[file_order].data.content[i] < 2){
                printf("error!");
                break;
            }
            munmap(mem[file[file_order].data.content[i]],blocksize);
            data_map[file[file_order].data.content[i]] = 0;
        }
    }
    file_map[file_order]=0;
    return 0;
}

static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
