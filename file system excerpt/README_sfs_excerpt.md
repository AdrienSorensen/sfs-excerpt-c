# SFS excerpt

This file is an excerpt from a simple file system built on FUSE. It focuses on path lookup, directory operations, block table management, and file I O. Types and constants live in your headers.

## What this shows
* Path resolution with `get_entry`, supports root and nested directories
* Directory metadata and listing through `sfs_getattr` and `sfs_readdir`
* File reads with block chain traversal and offset handling in `sfs_read`
* Block table helpers for free, chain, and end markers
* Directory creation and removal with `sfs_mkdir` and `sfs_rmdir`
* File creation, resize, and writes with `sfs_create`, `sfs_truncate`, and `sfs_write`

## On disk model
* Root directory area with fixed entry count
* Block table region that links data blocks
* Data region storing directory entries and file data
* Regular files stored as singly linked chains of fixed size blocks
* Directories stored as fixed arrays of entries

## How to read this quickly
* Start at `get_entry` to see how a path is resolved
* Check `sfs_getattr` and `sfs_readdir` to confirm directory behaviour
* Scan `sfs_read` to see block table traversal and partial block reads
* Review `allocate_block`, `find_free_block`, `free_block_chain` for storage management
* Finish with `sfs_create`, `sfs_truncate`, `sfs_write` to see file life cycle

## Notes for reviewers
* Error codes follow standard errno values used by FUSE
* New blocks are zeroed when allocated
* Chains are always terminated with the end marker after the last block
* The excerpt is self contained for reading; it compiles when linked with the project headers and the disk layer

I am happy to walk through the full file on a call if that is useful.