# Layout
When a new device is formatted as a new storage driver filesystem, a superblock with file system specific information is placed at the beginning of the device. This information identifies this device as having a valid filesystem on it when it is mounted again in the future. If a device with no valid superblock is mounted, LCFS storage driver formats the device before mounting.

Each of the layers created in the filesystem has a private superblock for locating data that belongs exclusively to that layer. Each layer in the file system has a unique index. This index stays the same for the lifetime of the layer.

In addition to the layers created for storing images and containers, a global file system layer keeps data that is not part of any other layer. This layer always has an index of 0. It cannot be deleted.

Superblocks of layers taken on a top of a common layer are linked together. Superblocks of the common layer point to one of these top layer superblocks. Thus superblocks of all layers on top of a layer are reachable from the superblock of that layer.

LCFS tracks available space using a list of free extents. There will be a single such extent immediately after the filesystem is formatted. The superblock of layer 0 tracks the blocks where this list is stored. Similarly, all other layers keep track of extents allocated to those layers. These blocks are also reachable from the superblock of those layers.

4 KB is the smallest unit of space allocation or size of I/O to the device, called the filesystem block size. For files larger than 4 KB, multiple blocks can be allocated in a single operation. Every layer shares the whole device, and space can be allocated for any layer anywhere in the underlying device.

Each file created in any layer has an inode to track information specific to that file such as statistical info, dirty data not flushed to disk, and so on. Each inode has a unique identifier in the filesystem called its “inode number.” Files deleted in a layer do not have to maintain any whiteouts as in some union file systems, because their references from the directories are removed in that layer. Inode numbers are not reused even after a file is deleted.

LCFS supports all UNIX file types. 
* For symbolic links, the target name is stored in the same block where inode is written. 
* For directories, separate blocks are allocated for storing directory entries and those blocks are linked in a chain from the inode. 
* For regular files, additional blocks are allocated for storing data and linked from the inode. 
* When a file becomes fragmented, that is, when an entire file cannot be stored contiguously on disk, additional blocks are allocated to track file page offsets and corresponding disk locations where data is stored, in extent format. Such blocks, called “emap blocks,” are linked from the inode as well. If the file has extended attributes, those are stored in additional blocks and linked from the inode as well. Currently, directories, emap blocks, and extended attribute blocks keep entries as linear lists.  They will be switched to better data structures like B-trees, etc., in the future as needed.

All inodes in a layer can be reached from the superblock of the layer. Every inode block is tracked in blocks linked from the superblock. Inodes are not stored in any order on disk. Inodes have their number within the inode.

All metadata (superblocks, inodes, directories, emap, extended attributes, and so on) are always cached in memory (although this may change in the future). They are read from disk when filesystem is mounted and written out when filesystem is unmounted.  Metadata blocks keep track of their checksums and those are validated when read in.

The root directory of the filesystem has inode number 2 and cannot be removed. Anything created under the tmp directory in root directory is considered temporary.

### Layer root directory

LCFS has another directory, called the "layer root directory", in which the roots of all layers are placed. This directory is for system use and cannot be removed once created. 
