/* Excerpt from sfs.c
   Focus: path lookup, directory ops, block table management,
          file IO including growth and truncation.
   Constants and structs are defined in sfs.h and friends.
   This excerpt compiles when linked against the original headers and disk layer.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sfs.h"
#include "diskio.h"

// helper that walks a path and returns the directory entry and its offset
static int get_entry(const char *path, struct sfs_entry *ret_entry, unsigned *ret_entry_off) {
    if (path == NULL || ret_entry == NULL || ret_entry_off == NULL) return -EINVAL;

    // root directory shortcut
    if (strcmp(path, "/") == 0) {
        memset(ret_entry, 0, sizeof(*ret_entry));
        ret_entry->size = SFS_DIRECTORY;
        *ret_entry_off = SFS_ROOTDIR_OFF;
        return 0;
    }

    char *path_copy = strdup(path);
    if (!path_copy) return -ENOMEM;

    char *component = strtok(path_copy, "/");
    if (!component) { free(path_copy); return -ENOENT; }

    // start at root
    unsigned current_off = SFS_ROOTDIR_OFF;
    unsigned entries_per_dir = SFS_ROOTDIR_NENTRIES;
    struct sfs_entry current_entry;

    while (component) {
        int found = 0;

        // scan the current directory entries
        for (unsigned i = 0; i < entries_per_dir; i++) {
            disk_read(&current_entry, sizeof(current_entry),
                      current_off + i * sizeof(struct sfs_entry));

            if (strlen(current_entry.filename) > 0 &&
                strcmp(current_entry.filename, component) == 0) {
                found = 1;
                *ret_entry = current_entry;
                *ret_entry_off = current_off + i * sizeof(struct sfs_entry);

                // move to next component if any
                component = strtok(NULL, "/");
                if (!component) { free(path_copy); return 0; }

                // must be a directory if there are more components
                if (!(current_entry.size & SFS_DIRECTORY)) {
                    free(path_copy);
                    return -ENOTDIR;
                }

                // descend into subdirectory
                current_off = SFS_DATA_OFF + current_entry.first_block * SFS_BLOCK_SIZE;
                entries_per_dir = SFS_DIR_NENTRIES;
                break;
            }
        }

        if (!found) { free(path_copy); return -ENOENT; }
    }

    free(path_copy);
    return -ENOENT;
}

// getattr maps file or directory metadata to struct stat
static int sfs_getattr(const char *path, struct stat *st) {
    struct sfs_entry entry;
    unsigned entry_off;
    int res;

    memset(st, 0, sizeof(struct stat));
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = time(NULL);
    st->st_mtime = time(NULL);

    res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;

    if (entry.size & SFS_DIRECTORY) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = entry.size & SFS_SIZEMASK;
    }

    return 0;
}

// readdir lists names only, type info comes through getattr
static int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi) {
    (void)offset;
    (void)fi;

    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (!(entry.size & SFS_DIRECTORY)) return -ENOTDIR;

    // add standard entries
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // pick directory area
    unsigned dir_off;
    unsigned entries_count;
    if (strcmp(path, "/") == 0) {
        dir_off = SFS_ROOTDIR_OFF;
        entries_count = SFS_ROOTDIR_NENTRIES;
    } else {
        dir_off = SFS_DATA_OFF + entry.first_block * SFS_BLOCK_SIZE;
        entries_count = SFS_DIR_NENTRIES;
    }

    // read each entry and add names
    struct sfs_entry curr;
    for (unsigned i = 0; i < entries_count; i++) {
        disk_read(&curr, sizeof(curr), dir_off + i * sizeof(struct sfs_entry));
        if (strlen(curr.filename) > 0) filler(buf, curr.filename, NULL, 0);
    }

    return 0;
}

// read copies up to size bytes from offset, respects end of file
static int sfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void)fi;

    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (entry.size & SFS_DIRECTORY) return -EISDIR;

    uint32_t file_size = entry.size & SFS_SIZEMASK;
    if (offset >= file_size) return 0;

    if (offset + size > file_size) size = file_size - offset;

    blockidx_t current_block = entry.first_block;
    unsigned bytes_read = 0;
    off_t current_offset = offset;

    // skip full blocks to reach starting offset
    while (current_offset >= SFS_BLOCK_SIZE) {
        blockidx_t next_block;
        disk_read(&next_block, sizeof(next_block),
                  SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
        if (next_block == SFS_BLOCKIDX_END) return bytes_read;
        current_block = next_block;
        current_offset -= SFS_BLOCK_SIZE;
    }

    // read from the chain
    while (bytes_read < size) {
        unsigned block_offset = current_offset;
        unsigned can_read = SFS_BLOCK_SIZE - block_offset;
        if (can_read > size - bytes_read) can_read = size - bytes_read;

        disk_read(buf + bytes_read, can_read,
                  SFS_DATA_OFF + current_block * SFS_BLOCK_SIZE + block_offset);

        bytes_read += can_read;
        current_offset = 0;

        if (bytes_read < size) {
            blockidx_t next_block;
            disk_read(&next_block, sizeof(next_block),
                      SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
            if (next_block == SFS_BLOCKIDX_END) break;
            current_block = next_block;
        }
    }

    return (int)bytes_read;
}

// scan the block table for a free block
static blockidx_t find_free_block(void) {
    blockidx_t block_idx;
    for (unsigned i = 0; i < SFS_BLOCKTBL_NENTRIES; i++) {
        disk_read(&block_idx, sizeof(block_idx),
                  SFS_BLOCKTBL_OFF + i * sizeof(blockidx_t));
        if (block_idx == SFS_BLOCKIDX_EMPTY) return i;
    }
    return SFS_BLOCKIDX_EMPTY;
}

// allocate a free block and return it
static int allocate_block(blockidx_t *block) {
    blockidx_t new_block = find_free_block();
    if (new_block == SFS_BLOCKIDX_EMPTY) return -ENOSPC;
    *block = new_block;
    return 0;
}

// free a chain of blocks by walking the table
static void free_block_chain(blockidx_t start_block) {
    while (start_block != SFS_BLOCKIDX_END && start_block != SFS_BLOCKIDX_EMPTY) {
        blockidx_t next_block;
        disk_read(&next_block, sizeof(next_block),
                  SFS_BLOCKTBL_OFF + start_block * sizeof(blockidx_t));

        blockidx_t empty = SFS_BLOCKIDX_EMPTY;
        disk_write(&empty, sizeof(empty),
                   SFS_BLOCKTBL_OFF + start_block * sizeof(blockidx_t));

        start_block = next_block;
    }
}

// find a free directory slot and return its offset
static int find_free_entry(unsigned dir_off, unsigned num_entries, unsigned *ret_entry_off) {
    struct sfs_entry entry;
    for (unsigned i = 0; i < num_entries; i++) {
        disk_read(&entry, sizeof(entry), dir_off + i * sizeof(struct sfs_entry));
        if (strlen(entry.filename) == 0) {
            *ret_entry_off = dir_off + i * sizeof(struct sfs_entry);
            return 0;
        }
    }
    return -ENOSPC;
}

// check whether a directory contains no named entries
static int check_dir_empty(unsigned dir_off, unsigned num_entries) {
    struct sfs_entry entry;
    for (unsigned i = 0; i < num_entries; i++) {
        disk_read(&entry, sizeof(entry), dir_off + i * sizeof(struct sfs_entry));
        if (strlen(entry.filename) > 0) return -ENOTEMPTY;
    }
    return 0;
}

// mkdir creates a new directory entry and initialises its blocks
static int sfs_mkdir(const char *path, mode_t mode) {
    (void)mode;

    char *last_slash = strrchr(path, '/');
    if (!last_slash || strlen(last_slash + 1) >= SFS_FILENAME_MAX) return -ENAMETOOLONG;

    // already exists
    struct sfs_entry existing;
    unsigned existing_off;
    if (get_entry(path, &existing, &existing_off) == 0) return -EEXIST;

    // locate parent
    char *parent_path = strdup(path);
    if (!parent_path) return -ENOMEM;
    parent_path[last_slash - path] = '\0';
    if (strlen(parent_path) == 0) strcpy(parent_path, "/");

    struct sfs_entry parent;
    unsigned parent_off;
    int res = get_entry(parent_path, &parent, &parent_off);
    if (res < 0) { free(parent_path); return res; }
    if (!(parent.size & SFS_DIRECTORY)) { free(parent_path); return -ENOTDIR; }

    unsigned parent_dir_off = (strcmp(parent_path, "/") == 0)
        ? SFS_ROOTDIR_OFF
        : SFS_DATA_OFF + parent.first_block * SFS_BLOCK_SIZE;
    unsigned num_entries = (strcmp(parent_path, "/") == 0)
        ? SFS_ROOTDIR_NENTRIES
        : SFS_DIR_NENTRIES;
    free(parent_path);

    // directory uses a fixed array of entries that fits in a two block chain
    blockidx_t first_block = find_free_block();
    if (first_block == SFS_BLOCKIDX_EMPTY) return -ENOSPC;
    blockidx_t second_block = find_free_block();
    if (second_block == SFS_BLOCKIDX_EMPTY) return -ENOSPC;

    // link first to second and close with END
    disk_write(&second_block, sizeof(blockidx_t),
               SFS_BLOCKTBL_OFF + first_block * sizeof(blockidx_t));
    blockidx_t end_marker = SFS_BLOCKIDX_END;
    disk_write(&end_marker, sizeof(blockidx_t),
               SFS_BLOCKTBL_OFF + second_block * sizeof(blockidx_t));

    // zero out directory entry array
    struct sfs_entry empty_entry = {0};
    empty_entry.first_block = SFS_BLOCKIDX_EMPTY;
    for (unsigned i = 0; i < SFS_DIR_NENTRIES; i++) {
        disk_write(&empty_entry, sizeof(empty_entry),
                   SFS_DATA_OFF + first_block * SFS_BLOCK_SIZE +
                   i * sizeof(struct sfs_entry));
    }

    // allocate slot in parent and write the new directory entry
    unsigned new_entry_off;
    res = find_free_entry(parent_dir_off, num_entries, &new_entry_off);
    if (res < 0) return res;

    struct sfs_entry new_dir = {0};
    strncpy(new_dir.filename, last_slash + 1, SFS_FILENAME_MAX - 1);
    new_dir.first_block = first_block;
    new_dir.size = SFS_DIRECTORY;

    disk_write(&new_dir, sizeof(new_dir), new_entry_off);
    return 0;
}

// rmdir removes an empty directory and frees its block chain
static int sfs_rmdir(const char *path) {
    if (strcmp(path, "/") == 0) return -EBUSY;

    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (!(entry.size & SFS_DIRECTORY)) return -ENOTDIR;

    unsigned dir_off = SFS_DATA_OFF + entry.first_block * SFS_BLOCK_SIZE;
    res = check_dir_empty(dir_off, SFS_DIR_NENTRIES);
    if (res < 0) return res;

    // free the chain
    blockidx_t current_block = entry.first_block;
    while (current_block != SFS_BLOCKIDX_END) {
        blockidx_t next_block;
        disk_read(&next_block, sizeof(next_block),
                  SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
        blockidx_t empty_marker = SFS_BLOCKIDX_EMPTY;
        disk_write(&empty_marker, sizeof(empty_marker),
                   SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
        current_block = next_block;
    }

    // clear the directory entry in parent
    struct sfs_entry empty_entry = {0};
    empty_entry.first_block = SFS_BLOCKIDX_EMPTY;
    disk_write(&empty_entry, sizeof(empty_entry), entry_off);

    return 0;
}

// unlink removes a regular file, frees blocks, and clears its directory entry
static int sfs_unlink(const char *path) {
    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (entry.size & SFS_DIRECTORY) return -EISDIR;

    free_block_chain(entry.first_block);

    struct sfs_entry empty_entry = {0};
    empty_entry.first_block = SFS_BLOCKIDX_EMPTY;
    disk_write(&empty_entry, sizeof(empty_entry), entry_off);
    return 0;
}

// create makes an empty file entry in the parent directory
static int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)mode;
    (void)fi;

    char *last_slash = strrchr(path, '/');
    if (!last_slash || strlen(last_slash + 1) >= SFS_FILENAME_MAX) return -ENAMETOOLONG;

    struct sfs_entry existing;
    unsigned existing_off;
    if (get_entry(path, &existing, &existing_off) == 0) return -EEXIST;

    // locate parent
    char *parent_path = strdup(path);
    if (!parent_path) return -ENOMEM;
    parent_path[last_slash - path] = '\0';
    if (strlen(parent_path) == 0) strcpy(parent_path, "/");

    struct sfs_entry parent;
    unsigned parent_off;
    int res = get_entry(parent_path, &parent, &parent_off);
    if (res < 0) { free(parent_path); return res; }

    unsigned parent_dir_off = (strcmp(parent_path, "/") == 0)
        ? SFS_ROOTDIR_OFF
        : SFS_DATA_OFF + parent.first_block * SFS_BLOCK_SIZE;
    unsigned num_entries = (strcmp(parent_path, "/") == 0)
        ? SFS_ROOTDIR_NENTRIES
        : SFS_DIR_NENTRIES;
    free(parent_path);

    unsigned new_entry_off;
    res = find_free_entry(parent_dir_off, num_entries, &new_entry_off);
    if (res < 0) return res;

    struct sfs_entry new_file = {0};
    strncpy(new_file.filename, last_slash + 1, SFS_FILENAME_MAX - 1);
    new_file.first_block = SFS_BLOCKIDX_END; // empty file
    new_file.size = 0;

    disk_write(&new_file, sizeof(new_file), new_entry_off);
    return 0;
}

// truncate grows or shrinks a file to the requested size
static int sfs_truncate(const char *path, off_t size) {
    if (size < 0) return -EINVAL;
    if ((unsigned)size > SFS_SIZEMASK) return -EFBIG;

    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (entry.size & SFS_DIRECTORY) return -EISDIR;

    uint32_t current_size = entry.size & SFS_SIZEMASK;

    if (size < (off_t)current_size) {
        // shrink
        blockidx_t current_block = entry.first_block;
        off_t blocks_needed = (size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;

        while (blocks_needed > 0 && current_block != SFS_BLOCKIDX_END) {
            blockidx_t next_block;
            disk_read(&next_block, sizeof(next_block),
                      SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
            blocks_needed--;
            current_block = next_block;
        }

        if (current_block != SFS_BLOCKIDX_END) {
            free_block_chain(current_block);
            blockidx_t end_marker = SFS_BLOCKIDX_END;
            disk_write(&end_marker, sizeof(end_marker),
                       SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
        }
    } else if (size > (off_t)current_size) {
        // grow
        blockidx_t current_block = entry.first_block;
        blockidx_t prev_block = SFS_BLOCKIDX_EMPTY;

        if (current_block == SFS_BLOCKIDX_END) {
            res = allocate_block(&current_block);
            if (res < 0) return res;
            entry.first_block = current_block;
        } else {
            while (current_block != SFS_BLOCKIDX_END) {
                prev_block = current_block;
                disk_read(&current_block, sizeof(current_block),
                          SFS_BLOCKTBL_OFF + prev_block * sizeof(blockidx_t));
            }
            current_block = prev_block;
        }

        size_t remaining = (size_t)(size - current_size);
        while (remaining > 0) {
            blockidx_t new_block;
            res = allocate_block(&new_block);
            if (res < 0) return res;

            disk_write(&new_block, sizeof(new_block),
                       SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));

            char zeros[SFS_BLOCK_SIZE] = {0};
            disk_write(zeros, SFS_BLOCK_SIZE,
                       SFS_DATA_OFF + new_block * SFS_BLOCK_SIZE);

            current_block = new_block;
            remaining = remaining > SFS_BLOCK_SIZE ? remaining - SFS_BLOCK_SIZE : 0;
        }

        blockidx_t end_marker = SFS_BLOCKIDX_END;
        disk_write(&end_marker, sizeof(end_marker),
                   SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
    }

    entry.size = (uint32_t)size;
    disk_write(&entry, sizeof(entry), entry_off);
    return 0;
}

// write copies from buf to file, grows the chain if needed, returns bytes written
static int sfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
    (void)fi;

    struct sfs_entry entry;
    unsigned entry_off;
    int res = get_entry(path, &entry, &entry_off);
    if (res < 0) return res;
    if (entry.size & SFS_DIRECTORY) return -EISDIR;

    uint32_t new_size = (offset + size > (entry.size & SFS_SIZEMASK))
        ? (uint32_t)(offset + size)
        : (entry.size & SFS_SIZEMASK);

    // ensure there is at least a first block
    if (entry.first_block == SFS_BLOCKIDX_END) {
        blockidx_t new_block = find_free_block();
        if (new_block == SFS_BLOCKIDX_EMPTY) return -ENOSPC;
        entry.first_block = new_block;
        blockidx_t end_marker = SFS_BLOCKIDX_END;
        disk_write(&end_marker, sizeof(end_marker),
                   SFS_BLOCKTBL_OFF + new_block * sizeof(blockidx_t));
    }

    blockidx_t current_block = entry.first_block;
    off_t current_offset = 0;

    // walk to the block that contains the starting offset
    while (current_offset + SFS_BLOCK_SIZE <= offset &&
           current_block != SFS_BLOCKIDX_END) {
        disk_read(&current_block, sizeof(current_block),
                  SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));
        current_offset += SFS_BLOCK_SIZE;
    }

    // if we need to step through empty space to reach offset
    while (current_offset + SFS_BLOCK_SIZE <= offset) {
        blockidx_t new_block = find_free_block();
        if (new_block == SFS_BLOCKIDX_EMPTY) return -ENOSPC;

        disk_write(&new_block, sizeof(new_block),
                   SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));

        current_block = new_block;
        current_offset += SFS_BLOCK_SIZE;
    }

    // write data across blocks
    size_t written = 0;
    while (written < size) {
        size_t block_off = offset + written - current_offset;
        size_t can_write = SFS_BLOCK_SIZE - block_off;
        if (can_write > size - written) can_write = size - written;

        disk_write(buf + written, can_write,
                   SFS_DATA_OFF + current_block * SFS_BLOCK_SIZE + block_off);

        written += can_write;

        if (written < size) {
            blockidx_t next_block;
            disk_read(&next_block, sizeof(next_block),
                      SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));

            if (next_block == SFS_BLOCKIDX_END) {
                next_block = find_free_block();
                if (next_block == SFS_BLOCKIDX_EMPTY) break;

                disk_write(&next_block, sizeof(next_block),
                           SFS_BLOCKTBL_OFF + current_block * sizeof(blockidx_t));

                blockidx_t end_marker = SFS_BLOCKIDX_END;
                disk_write(&end_marker, sizeof(end_marker),
                           SFS_BLOCKTBL_OFF + next_block * sizeof(blockidx_t));
            }

            current_block = next_block;
            current_offset += SFS_BLOCK_SIZE;
        }
    }

    // update file size if we extended
    if (new_size > (entry.size & SFS_SIZEMASK)) {
        entry.size = new_size;
        disk_write(&entry, sizeof(entry), entry_off);
    }

    return (int)written;
}