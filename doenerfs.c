#define FUSE_USE_VERSION  26

#include <unistd.h>
#include "doenerfs.h"   
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>

FILE *logger = 0;

static int *hits = 0;
static int hit_counter = 0;
static uint32_t write_pages = 0;

static size_t detached_allocated = 0;
static size_t sparse_memory = 0;
static char *cowfilename = 0;

static struct timeval start;

static int doener_getattr(const char *path, struct stat *stbuf)
{
    //fprintf(logger, "getattr %s\n", path);

    int res = 0;
    memset(stbuf, 0, sizeof(struct stat));
    if(strcmp(path, "/") == 0) {
	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_nlink = 2;
    }
    else if(path[0] == '/' && strcmp(path + 1, thefile) == 0) {
	stbuf->st_mode = S_IFREG | 0644;
	stbuf->st_nlink = 1;
	stbuf->st_size = thefilesize;
    }
    else
	res = -ENOENT;
  
    return res;
}
  
static int doener_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
    //fprintf(logger, "readdir %s\n", path);
    (void) offset;
    (void) fi;
  
    if(strcmp(path, "/") != 0)
	return -ENOENT;
  
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, thefile, NULL, 0);
  
    return 0;
}
  
static int doener_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;
  
//    if((fi->flags & 3) != O_RDONLY)
//	return -EACCES;
  
   fi->keep_cache = 1;
    return 0;
}

struct buffer_combo {
    unsigned char *out_buffer;
    
    uint32_t part;
    pthread_mutex_t lock;
    int free;
    int index;
    int used;
};

static int used_counter = 0;

struct buffer_combo *coms;    
static unsigned int com_count = 1;

pthread_mutex_t picker = PTHREAD_MUTEX_INITIALIZER, seeker = PTHREAD_MUTEX_INITIALIZER;;

FILE *pack;

static const unsigned char *doener_uncompress(uint32_t part)
{
    struct buffer_combo *com;

    //fprintf(logger, "doener_uncompress %d %d %d\n", part, parts, wparts);

    pthread_mutex_lock(&picker);
    int index = -1;
    unsigned int i;
    for (i = 0; i < com_count; ++i)
    {
	if (coms[i].part == part)
	{
	    index = i;
	    break;
	}
    }
    if (index == -1)
    {
	index = 0;
	for (i = 0; i < com_count -1; ++i)
	{
	    if (coms[i].free) {
		index = i;
		break;
	    }
	}
	for (i = index + 1; i < com_count; ++i)
	{
	    if (coms[i].free && coms[index].used > coms[i].used)
		index = i;
	}
    }
    com = coms + index;
    pthread_mutex_lock(&com->lock);
    com->free = 0;
    com->used = used_counter++;
    pthread_mutex_unlock(&picker);

    if (com->part == part)
    {
	const unsigned char *buf = com->out_buffer;
	com->free = 1;
	pthread_mutex_unlock(&com->lock);
	return buf;
    }

    com->part = part;

    if (!hits[part]) {
      if (logger) fprintf(logger, "first hit %d\n", part );
      hits[part] = ++hit_counter;
    }

    pthread_mutex_lock(&seeker);
    unsigned char *inbuffer = malloc(sizes[part]);
    struct timeval begin, end;
    gettimeofday(&begin, 0);
    size_t readin = doener_readpart(inbuffer, part);
    gettimeofday(&end, 0);

    //#if defined(DEBUG)
      if (logger) fprintf(logger, "uncompress %d %d %ld %ld (read took %ld - started %ld)\n", part, com->index, (long)offs[part], (long)sizes[part], (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_usec - begin.tv_usec) / 1000, (begin.tv_sec - start.tv_sec) * 1000 + (begin.tv_usec - start.tv_usec) / 1000 );
    //#endif
    if (!readin)
      return 0;
    pthread_mutex_unlock(&seeker);

    doener_decompress_part(com->out_buffer, inbuffer, readin);
    free(inbuffer);

    com->part = part;
    com->free = 1;

    pthread_mutex_unlock(&com->lock);

    return com->out_buffer;
}

static void doener_log_access(size_t block)
{
   if (!logger) return;

   static size_t firstblock = 0;
   static ssize_t lastblock = -1;

   if (lastblock >= 0 && block != (size_t)(lastblock + 1))
   {
       fprintf(logger, "access %ld+%ld\n", (long)firstblock, (long)lastblock-firstblock);
       firstblock = block;
   }
   lastblock = block;
   if (block > firstblock + 30) 
   {
      fprintf(logger, "access %ld+%ld\n", (long)firstblock, (long)lastblock-firstblock);
      firstblock = block;
   }
}

static size_t doener_read_block(char *buf, size_t block);

static int doener_detach(size_t block)
{
    unsigned char *ptr = blockmap[block];
    if (((long)ptr & 0x3) == 1)
    {
	ptr = malloc(4096);
	assert(((long)ptr & 0x3) == 0);
	detached_allocated += 4;
	if (logger && detached_allocated % 1024 ) fprintf(logger, "detached %.3fMB\n", detached_allocated / 1024.);

	doener_read_block((char*)ptr, block);
	blockmap[block] = ptr;
	return 1;
    } 
    if (!blockmap[block])
    {
	blockmap[block] = malloc(4096);
	assert(((long)ptr & 0x3) == 0);
	detached_allocated += 4;
	if (logger && detached_allocated % 1024 ) fprintf(logger, "detached %.3f\n", detached_allocated / 1024.);
	memset(blockmap[block],0,4096);
	return 1;
    }

    return 0;
}

static size_t doener_write_block(const char *buf, off_t block, size_t size)
{
    doener_detach(block);
    memcpy(blockmap[block], buf, size);
    return size;
}

static int doener_write(const char *path, const char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
    //if (logger) fprintf(logger, "write %s %ld %ld\n", path, offset, size);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    if (offset >= (off_t)thefilesize) {
        return 0;
    }

    off_t block = offset / 4096;
    off_t ioff = offset - block * 4096;

    assert(ioff == 0 || ioff + size <= 4096);

    if (size <= 4096) {
	return doener_write_block(buf+ioff, block, size);
    } else {
	size_t wrote = 0;
	do
	{
	    size_t diff = doener_write_block(buf, block, size > 4096 ? 4096 : size);
	    size -= diff;
	    buf += diff;
	    block++;
	    wrote += diff;
	} while (size > 0);

	return wrote;
    }
}


static size_t doener_read_block(char *buf, size_t block)
{
    if (block >= write_pages)
	return 0;

    assert(block < write_pages);
    doener_log_access(block);

    if (!blockmap[block]) { // sparse block 
        memset(buf, 0, 4096);
        return 4096;
    }

    if (((long)blockmap[block] & 0x3) == 0) {
	// detached
	memcpy(buf, blockmap[block], 4096);
	return 4096;
    }

    if (((long)blockmap[block] & 0x3) == 2) {
	// in cow file
	// TODO
	return 4096;
    }

    assert(((long)blockmap[block] & 0x3) == 1); // in read only part
    assert(block < num_pages);

    off_t mapped_block = doener_map_block(block);

    size_t part = (size_t)(mapped_block * 4096 / bsize);
    assert(part < parts);

    const unsigned char *partbuf = doener_uncompress(part);
    assert(partbuf);
    memcpy(buf, partbuf + 4096 * (mapped_block % (bsize / 4096)), 4096);

    return 4096;
}

static int doener_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    //fprintf(stdout, "read %ld %ld %ld\n", offset, size, thefilesize);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    size_t readtotal = 0;

    assert(size % 4096 == 0);
    assert(offset % 4096 == 0);

    do
    {
	if (offset >= (off_t)thefilesize)
		break;
	size_t diff = doener_read_block(buf, offset / 4096);
	if (!diff)
	  break;
	size -= diff;
	buf += diff;
	offset += diff;
	readtotal += diff;
    } while (size > 0);

    return readtotal;
}

static uint32_t doener_find_next_cow()
{
    return 0;
}

static int doener_write_cow()
{
    // TODO: this should be thread safe even if we do single thread only atm
    uint32_t stringlen = thefilesize;
    
    struct stat st;
    stat(cowfilename, &st);
    fseek(cowfile, st.st_size - sizeof(uint32_t), SEEK_SET);
    uint32_t indexlen = doener_readindex(cowfile) + sizeof(uint32_t);
    if (fseek(cowfile, st.st_size - indexlen, SEEK_SET ))
	perror("seek");
    (void)doener_readindex(cowfile); // the file size
    uint32_t cowpages = doener_readindex(cowfile);
    fprintf(stderr, "old cows %ld\n", (long)cowpages);
    uint32_t *oldindex = malloc(num_pages * sizeof(uint32_t));
    memset(oldindex, 0, num_pages * sizeof(uint32_t));
    uint32_t i;
    for (i = 0; i < cowpages; ++i)
    {
	uint32_t pageindex = doener_readindex(cowfile);
	assert(pageindex < num_pages);
	oldindex[pageindex] = i;
    }

    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cowfile);
    indexlen = sizeof(uint32_t) * 2;
    stringlen = 0; // 0 blocks so far
    for (i = 0; i < num_pages; ++i)
    {
	long ptr = (long)blockmap[i];
	if ((ptr & 0x3) == 0) { // detached now
	    fprintf(stderr, "detached %ld %ld\n", (long)i, (long)oldindex[i]);
	    uint32_t cowindex = doener_find_next_cow();
	    fseek(cowfile, cowindex * 4096, SEEK_SET);
	    fwrite(blockmap[i], 4096, 1, cowfile);
	    free(blockmap[i]);
	    blockmap[i] = (unsigned char*)(long)(cowindex << 2) + 2;
	}
    }

    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cowfile);
    fwrite((char*)&indexlen, 1, sizeof(uint32_t), cowfile);
    fflush(cowfile);
    return 0;
}
  
static int doener_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    // TODO write out cow
    if (logger) {
	fprintf(logger, "flush\n");
	fflush(logger);
    }
    doener_write_cow();
    return 0;
}

static int doener_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    (void)datasync;
    // TODO write out cow
    if (logger) {
	fprintf(logger, "sync\n");
	fflush(logger);
    }
    doener_write_cow();
    fsync(fileno(cowfile));
    return 0;
}

static struct fuse_operations doener_oper = {
    .getattr   = doener_getattr,
    .readdir = doener_readdir,
    .open   = doener_open,
    .read   = doener_read,
    .write  = doener_write,
    .flush  = doener_flush,
    .fsync = doener_fsync
};
  
static void doener_init_buffer(int i)
{
    coms[i].part = -1;
    coms[i].used = 0;
    coms[i].index = i + 1;
    coms[i].free = 1;
    coms[i].out_buffer = malloc(bsize);
    pthread_mutex_init(&coms[i].lock, 0);
}

char *packfilename = 0;
char *logfile = 0;

enum  { FUSE_OPT_MEMORY, FUSE_OPT_LOGGER, FUSE_OPT_COWFILE };

struct fuse_opt doener_opt[] = {
    FUSE_OPT_KEY("-m %s", FUSE_OPT_MEMORY),
    FUSE_OPT_KEY("-l %s", FUSE_OPT_LOGGER),
    FUSE_OPT_KEY("-c %s", FUSE_OPT_COWFILE),
    FUSE_OPT_END
};

int doener_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
    (void)data;
    (void)outargs;

    switch (key) {
	case FUSE_OPT_KEY_NONOPT:
	    if (packfilename == NULL) {
		packfilename = strdup(arg);
		return 0;
	    }
	    break;
	case FUSE_OPT_MEMORY:
	     sparse_memory = atoi(arg+2);
	     return 0;
	     break;
	case FUSE_OPT_LOGGER:
	     logfile = strdup(arg+2);
	     return 0;
	     break;
	case FUSE_OPT_COWFILE:
	     cowfilename = strdup(arg+2);
	     return 0;
	     break;
    }
	
    return 1;
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, NULL, doener_opt, doener_opt_proc) == -1) {
      perror("fuse_opt_part");
      return 1;
    }

    if (logfile) {
      if (!strcmp(logfile, "-"))
	logger = stderr;
      else
	logger = fopen(logfile, "w");
      if (!logger) {
	perror("open");
        return 1;
      }
    }

    // not sure why but multiple threads make it slower
    fuse_opt_add_arg(&args, "-s");

    if (!packfilename || (cowfile && sparse_memory)) {
	fprintf(stderr, "usage: [-m <mb>] [-l <logfile|->] [-c <cowfile>] <packfile> <mntpoint>\n");
	if (cowfile && sparse_memory) {
	    fprintf(stderr, "writes can go either into cowfile or memory\n");
	}
        return 1;
    }

    if (doenerfs_read_pack(packfilename)) {
      perror("read_pack");
      return 1;
    }

    if (cowfilename) {
	if (access(cowfilename, W_OK)) {
	    FILE *cow = fopen(cowfilename, "w");
	    uint32_t stringlen = (thefilesize / 4096 * 4096) + 512 * 1024 * 1024;
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    stringlen = 0;
	    // there are 0 blocks
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    // the whole index is 8 bytes long
	    stringlen = sizeof(uint32_t) * 2;
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    fclose(cow);
	}
	if (doenerfs_read_cow(cowfilename))
	    return 1;
    }

    // fake for write
    if (sparse_memory) {
      thefilesize = (thefilesize / 4096 * 4096) + sparse_memory * 1024 * 1024;
      write_pages = thefilesize / 4096;
      blockmap = realloc(blockmap, sizeof(unsigned char*)*write_pages);
    } else
      write_pages = num_pages;

    uint32_t i;
 
    for (i = num_pages; i < write_pages; ++i)
	blockmap[i] = 0;

    hits = malloc(sizeof(int)*parts);
    for (i = 0; i < parts; ++i)
    {
        hits[i] = 0;
    }
    
    com_count = 6000000 / bsize; // get 6MB of cache
    coms = malloc(sizeof(struct buffer_combo) * com_count);
    for (i = 0; i < com_count; ++i)
	doener_init_buffer(i);

    gettimeofday(&start, 0);
    int ret = fuse_main(args.argc, args.argv, &doener_oper, NULL);
    if (logger) fclose(logger);
    return ret;
}
