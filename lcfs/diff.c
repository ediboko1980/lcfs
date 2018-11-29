#include "includes.h"

static void lc_addName(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
                       mode_t mode, uint16_t len, ino_t lastIno,
                       enum lc_changeType ctype);

/* Return the type of changed based on inode number */
static inline enum lc_changeType
lc_changeInode(ino_t ino, ino_t lastIno) {
    return (ino > lastIno) ? LC_ADDED : LC_MODIFIED;
}

/* Add a file to the change list */
static void
lc_addFile(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
           uint16_t len, enum lc_changeType ctype) {
    struct cfile *cfile = cdir->cd_file, **prev = &cdir->cd_file;
    bool found = false;

    assert(cdir->cd_type != LC_REMOVED);

    /* Check if the file already in the list */
    while (cfile) {
        if ((cfile->cf_len == len) && !strncmp(cfile->cf_name, name, len)) {
            found = true;
            break;
        }
        prev = &cfile->cf_next;
        cfile = cfile->cf_next;
    }

    /* If an entry exists already, return after updating it */
    if (found) {
        if ((cfile->cf_type == LC_REMOVED) && (ctype == LC_ADDED)) {
            cfile->cf_type = LC_MODIFIED;
        } else {
            assert((cfile->cf_type == LC_ADDED) ||
                   (cfile->cf_type == LC_MODIFIED));
            assert(ctype != LC_REMOVED);
        }
        return;
    }

    /* Create a new entry and add at the end of the list */
    cfile = lc_malloc(fs, sizeof(struct cfile), LC_MEMTYPE_CFILE);
    cfile->cf_type = ctype;
    cfile->cf_name = name;
    cfile->cf_len = len;
    cfile->cf_next = NULL;
    *prev = cfile;
}

/* Compare directory entries with parent layer and populate the change list
 * with changes in the directory.
 */
static void
lc_processDirectory(struct fs *fs, struct inode *dir, struct inode *pdir,
                    ino_t lastIno, struct cdir *cdir) {
    struct dirent *dirent, *pdirent, *fdirent, *ldirent, *adirent;
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max;

    assert(dir->i_fs == fs);
    assert(dir->i_fs != pdir->i_fs);

    /* If nothing changed in the directory, return */
    if (dir->i_flags & LC_INODE_SHARED) {
        return;
    }

    /* Traverse parent directory entries looking for missing entries */
    if (hashed) {
        assert(pdir->i_flags & LC_INODE_DHASHED);
        max = LC_DIRCACHE_SIZE;
    } else {
        assert(!(pdir->i_flags & LC_INODE_DHASHED));
        max = 1;
    }
    for (i = 0; i < max; i++) {
        if (hashed) {
            pdirent = pdir->i_hdirent[i];
            dirent = dir->i_hdirent[i];
        } else {
            pdirent = pdir->i_dirent;
            dirent = dir->i_dirent;
        }
        fdirent = dirent;
        adirent = NULL;

        /* Directory entries have the same order in both layers */
        while (pdirent) {
            ldirent = dirent;
            while (dirent && (dirent->di_ino != pdirent->di_ino)) {
                dirent = dirent->di_next;
            }

            /* Check if the file was renamed */
            if (dirent) {
                if (adirent == NULL) {
                    adirent = dirent;
                }
                assert(dirent->di_ino == pdirent->di_ino);
                if ((dirent->di_size != pdirent->di_size) ||
                    (strcmp(pdirent->di_name, dirent->di_name))) {
                    lc_addName(fs, cdir, pdirent->di_ino, pdirent->di_name,
                               pdirent->di_mode, pdirent->di_size,
                               lastIno, LC_REMOVED);
                    lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                               dirent->di_mode, dirent->di_size, lastIno,
                               LC_ADDED);
                }
                dirent = dirent->di_next;
            } else {

                /* If the entry is not present in the layer, add a record for
                 * the removed file.
                 */
                lc_addName(fs, cdir, pdirent->di_ino, pdirent->di_name,
                           pdirent->di_mode, pdirent->di_size,
                           lastIno, LC_REMOVED);
                dirent = ldirent;
            }
            pdirent = pdirent->di_next;
        }


        /* Process any newly created entries */
        dirent = fdirent;
        while (dirent != adirent) {
            lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                       dirent->di_mode, dirent->di_size, lastIno, LC_ADDED);
            dirent = dirent->di_next;
        }
    }
}

/* Lookup the inode number corresponding to the path */
static struct inode *
lc_pathLookup(struct fs *fs, char *path, uint16_t len) {
    struct inode *dir = fs->fs_rootInode;
    ino_t ino = LC_INVALID_INODE;
    char *name = alloca(len);
    uint16_t i = 1, j = 0;

    assert(path[0] == '/');

    /* Break up the path into components and lookup to see if each of those
     * present in the layer.
     */
    while (i < len) {
        if (path[i] == '/') {
            if (j) {
                name[j] = 0;
                j = 0;
                ino = lc_dirLookup(fs, dir, name);
                dir = (ino == LC_INVALID_INODE) ? NULL :
                    lc_getInode(fs, ino, NULL, false, false);
                if ((dir == NULL) || !S_ISDIR(dir->i_mode)) {
                    break;
                }
            }
        } else {
            name[j++] = path[i];
        }
        i++;
    }
    if (j) {
        name[j] = 0;
        ino = lc_dirLookup(fs, dir, name);
        dir = (ino == LC_INVALID_INODE) ? NULL :
                                lc_getInode(fs, ino, NULL, false, false);
    }
    return (dir && S_ISDIR(dir->i_mode)) ? dir : NULL;
}

/* Compare a newly created directory with directory in the parent layer with
 * same path.
 */
static void
lc_compareDirectory(struct fs *fs, struct inode *dir, struct inode *pdir,
                    ino_t lastIno, struct cdir *cdir) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max = hashed ? LC_DIRCACHE_SIZE : 1;
    ino_t ino = LC_INVALID_INODE;
    struct dirent *dirent;
    uint64_t count = 0;

    if (pdir && ((dir == fs->fs_rootInode) || (pdir->i_ino == dir->i_ino)) &&
        ((dir->i_flags & LC_INODE_DHASHED) ==
         (pdir->i_flags & LC_INODE_DHASHED))) {
        lc_processDirectory(fs, dir, pdir, lastIno, cdir);
        return;
    }

    /* Check for entries currently present */
    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent) {
            if (pdir) {
                ino = lc_dirLookup(fs, pdir, dirent->di_name);
            }
            if (ino == LC_INVALID_INODE) {
                lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                           dirent->di_mode, dirent->di_size, lastIno,
                           LC_ADDED);
            }
            count++;
            dirent = dirent->di_next;
        }
        if (count == dir->i_size) {
            break;
        }
    }
    if (pdir == NULL) {
        return;
    }

    /* Check missing entries */
    hashed = (pdir->i_flags & LC_INODE_DHASHED);
    max = hashed ? LC_DIRCACHE_SIZE : 1;
    count = 0;
    for (i = 0; i < max; i++) {
        dirent = hashed ? pdir->i_hdirent[i] : pdir->i_dirent;
        while (dirent) {
            ino = lc_dirLookup(fs, dir, dirent->di_name);
            if (ino == LC_INVALID_INODE) {
                lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                           dirent->di_mode, dirent->di_size, lastIno,
                           LC_REMOVED);
            }
            count++;
            dirent = dirent->di_next;
        }
        if (count == pdir->i_size) {
            break;
        }
    }
}

/* Add the whole directory tree to the change list */
static void
lc_addDirectoryTree(struct fs *fs, struct inode *dir, struct cdir *cdir,
                    struct cdir *pcdir, ino_t lastIno) {
    ino_t parent = dir->i_parent;
    struct inode *pdir;

    /* Check if an old directory is replaced with a new one.  In that case,
     * compare those directories.
     */
    if (pcdir == NULL) {
        pcdir = fs->fs_changes;
        while (pcdir && (pcdir->cd_ino != parent)) {
            pcdir = pcdir->cd_next;
        }
    }
    if (pcdir->cd_type == LC_MODIFIED) {
        pdir = (dir == fs->fs_rootInode) ? fs->fs_parent->fs_rootInode :
                    lc_pathLookup(fs->fs_parent, cdir->cd_path, cdir->cd_len);
        if (pdir) {
            cdir->cd_type = LC_MODIFIED;
            if (pdir->i_size) {
                lc_compareDirectory(fs, dir, pdir, lastIno, cdir);
                return;
            }
        }
    }

    /* Add everything from the new directory */
    lc_compareDirectory(fs, dir, NULL, lastIno, cdir);
}

/* Add the directory to the change list */
static void
lc_addDirectoryPath(struct fs *fs, ino_t ino, ino_t parent, struct cdir *new,
                    struct cdir *cdir, char *name, uint16_t len) {
    struct cfile *cfile, **prev;
    struct dirent *dirent;
    uint16_t plen;

    /* Root directory is added first */
    if (ino == fs->fs_root) {
        assert(fs->fs_changes == NULL);
        fs->fs_changes = new;
        new->cd_next = NULL;
        new->cd_len = 1;
        new->cd_path = lc_malloc(fs, 1, LC_MEMTYPE_PATH);
        new->cd_path[0] = '/';
    } else {

        /* Find parent directory entry */
        if (cdir == NULL) {
            cdir = fs->fs_changes;
            while (cdir && (cdir->cd_ino != parent)) {
                cdir = cdir->cd_next;
            }
        }
        assert(cdir->cd_ino == parent);

        /* Add the directory after the parent */
        new->cd_next = cdir->cd_next;
        cdir->cd_next = new;

        /* Lookup name if not known */
        if (len == 0) {
            dirent = lc_getDirent(fs, parent, ino, NULL, NULL);
            name = dirent->di_name;
            len = dirent->di_size;
        }

        /* Check if there is a removed entry for this name */
        if (cdir->cd_type == LC_MODIFIED) {
            cfile = cdir->cd_file;
            prev = &cdir->cd_file;
            while (cfile && ((cfile->cf_len != len) ||
                             strncmp(cfile->cf_name, name, len))) {
                prev = &cfile->cf_next;
                cfile = cfile->cf_next;
            }
            if (cfile && (cfile->cf_len == len) &&
                !strncmp(cfile->cf_name, name, len)) {
                assert(new->cd_type == LC_ADDED);
                assert(cfile->cf_type == LC_REMOVED);
                *prev = cfile->cf_next;
                lc_free(fs, cfile, sizeof(struct cfile), LC_MEMTYPE_CFILE);
                new->cd_type = LC_MODIFIED;
            }
        }

        /* Prepare complete path and link to the record */
        plen = (cdir->cd_len > 1) ? cdir->cd_len : 0;
        new->cd_len = plen + len + 1;
        new->cd_path = lc_malloc(fs, new->cd_len, LC_MEMTYPE_PATH);
        if (plen) {
            memcpy(new->cd_path, cdir->cd_path, plen);
        }
        new->cd_path[plen] = '/';
        memcpy(&new->cd_path[plen + 1], name, len);
    }
}

/* Add a directory to the change list */
static struct cdir *
lc_addDirectory(struct fs *fs, struct inode *dir, char *name, uint16_t len,
                ino_t lastIno, enum lc_changeType ctype) {
    ino_t ino = dir->i_ino, parent = dir->i_parent;
    struct cdir *cdir, *new, *pcdir = NULL;
    struct inode *pdir;
    bool path = true;

    if ((dir->i_fs != fs) && (dir->i_fs->fs_root == parent)) {
        parent = fs->fs_root;
    }
    //lc_printf("Directory %ld new parent %ld ctype %d\n", ino, parent, ctype);

retry:

    /* Check if the directory entry exists already */
    cdir = pcdir ? pcdir : fs->fs_changes;
    while (cdir && (cdir->cd_ino != ino)) {
        cdir = cdir->cd_next;
    }

    /* Check if an entry is found */
    if (cdir) {
        assert(cdir->cd_ino == ino);
        new = cdir;
        goto out;
    }
    assert(!(dir->i_flags & LC_INODE_CTRACKED));

    /* Add all directories in the path */
    if ((ino != parent) && path) {
        pdir = lc_getInode(fs, parent, NULL, false, false);
        if (!(pdir->i_flags & LC_INODE_CTRACKED)) {
            pcdir = lc_addDirectory(fs, pdir, NULL, 0, lastIno,
                                    lc_changeInode(pdir->i_ino, lastIno));
        }
        lc_inodeUnlock(pdir);
        path = false;
        goto retry;
    }

    /* Create a new entry for this directory */
    new = lc_malloc(fs, sizeof(struct cdir), LC_MEMTYPE_CDIR);
    new->cd_ino = ino;
    new->cd_type = ctype;
    new->cd_file = NULL;

    /* Add this directory to the change list */
    lc_addDirectoryPath(fs, ino, parent, new, pcdir, name, len);

out:
    if ((dir->i_fs == fs) && !(dir->i_flags & LC_INODE_CTRACKED)) {
        dir->i_flags |= LC_INODE_CTRACKED;
        if (ino == parent) {
            pcdir = new;
        }

        /* Add the complete directory tree */
        lc_addDirectoryTree(fs, dir, new, pcdir, lastIno);
    }
    return new;
}

/* Add an inode to the change list */
static void
lc_addModifiedInode(struct fs *fs, struct inode *inode, ino_t lastIno) {
    ino_t ino = inode->i_ino, parent = LC_INVALID_INODE;
    uint32_t nlink = inode->i_nlink, plink = 0;
    struct hldata *hldata = fs->fs_hlinks;
    struct dirent *dirent;
    struct inode *dir;
    struct cdir *cdir;
    int hash = 0;

    assert(!(inode->i_flags & LC_INODE_CTRACKED));
    assert(inode->i_fs == fs);

    /* Add each link of the inode to the change list */
    while (nlink) {
        if (!(inode->i_flags & LC_INODE_MLINKS)) {
            parent = inode->i_parent;
            plink = 1;
        } else {

            /* Find next directory with a link to this inode */
            while (hldata && (hldata->hl_ino != ino)) {
                hldata = hldata->hl_next;
            }
            assert(hldata->hl_nlink > 0);
            parent = hldata->hl_parent;
            if (parent == LC_ROOT_INODE) {
                parent = fs->fs_root;
            }
            plink = hldata->hl_nlink;
            hldata = hldata->hl_next;
        }
        if ((inode->i_fs != fs) && (inode->i_fs->fs_root == parent)) {
            parent = fs->fs_root;
        }

        /* Find the entry for parent directory */
        cdir = fs->fs_changes;
        while (cdir && (cdir->cd_ino != parent)) {
            cdir = cdir->cd_next;
        }

        /* If an entry for the parent doesn't exist, add one */
        if (cdir == NULL) {
            dir = lc_getInode(fs, parent, NULL, false, false);
            assert(dir->i_ino < lastIno);
            cdir = lc_addDirectory(fs, dir, NULL, 0, lastIno, LC_MODIFIED);
            lc_inodeUnlock(dir);
        }
        assert(cdir->cd_ino == parent);
        assert(!(inode->i_flags & LC_INODE_CTRACKED));
        assert(plink <= nlink);
        nlink -= plink;
        dirent = NULL;
        hash = 0;

        /* Add each link from the directory to the change list */
        while (plink) {
            dirent = lc_getDirent(fs, parent, ino, &hash, dirent);
            lc_addFile(fs, cdir, ino, dirent->di_name, dirent->di_size,
                       lc_changeInode(ino, lastIno));
            plink--;
        }
    }
    inode->i_flags |= LC_INODE_CTRACKED;
}

/* Add a record to the change list */
static void
lc_addName(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
           mode_t mode, uint16_t len, ino_t lastIno,
           enum lc_changeType ctype) {
    struct inode *dir, *inode;

    if (S_ISDIR(mode) && (ctype != LC_REMOVED)) {
        dir = lc_getInode(fs, ino, NULL, false, false);
        if (!(dir->i_flags & LC_INODE_CTRACKED) || (ctype == LC_ADDED)) {
            lc_addDirectory(fs, dir, name, len, lastIno, ctype);
        }
        lc_inodeUnlock(dir);
    } else {
        lc_addFile(fs, cdir, ino, name, len, ctype);

        /* Flag the inode as tracked in change list */
        if (ctype != LC_REMOVED) {
            inode = lc_lookupInodeCache(fs, ino, -1);
            if (inode && ((ino > lastIno) ||
                          !(inode->i_flags & LC_INODE_MLINKS))) {
                assert(inode->i_fs == fs);
                inode->i_flags |= LC_INODE_CTRACKED;
            }
        }
    }
}

/* Respond with diff data */
static void
lc_replyDiff(fuse_req_t req, struct fs *fs) {
    char buf[LC_BLOCK_SIZE];
    struct pchange *pchange;
    struct cfile *cfile;
    int size = 0, plen;
    struct cdir *cdir;

    /* Traverse change list */
    while ((cdir = fs->fs_changes)) {
        if (cdir->cd_ino == fs->fs_root) {
            cdir->cd_type = LC_NONE;
        }

        /* Add a record for the new or modified directory */
        if ((cdir->cd_type != LC_NONE) || cdir->cd_file) {
            plen = cdir->cd_len + sizeof(struct pchange);
            if ((size + plen) >= LC_BLOCK_SIZE) {
                break;
            }
            pchange = (struct pchange *)&buf[size];
            pchange->ch_type = cdir->cd_type;
            pchange->ch_len = cdir->cd_len;
            memcpy(&pchange->ch_path, cdir->cd_path, cdir->cd_len);
            cdir->cd_type = LC_NONE;
            size += plen;
        }

        /* Add records for changes in the directory */
        while ((cfile = cdir->cd_file)) {
            plen = cfile->cf_len + sizeof(struct pchange);
            if ((size + plen) >= LC_BLOCK_SIZE) {
                goto out;
            }
            pchange = (struct pchange *)&buf[size];
            pchange->ch_type = cfile->cf_type;
            pchange->ch_len = cfile->cf_len;
            memcpy(&pchange->ch_path, cfile->cf_name, cfile->cf_len);
            size += plen;
            cdir->cd_file = cfile->cf_next;
            lc_free(fs, cfile, sizeof(struct cfile), LC_MEMTYPE_CFILE);
        }
        if (cdir->cd_path) {
            lc_free(fs, cdir->cd_path, cdir->cd_len, LC_MEMTYPE_PATH);
        }

        /* Remove the last record after all records are returned */
        if ((cdir->cd_next != NULL) || (size == 0)) {
            fs->fs_changes = cdir->cd_next;
            lc_free(fs, cdir, sizeof(struct cdir), LC_MEMTYPE_CDIR);
        } else {
            cdir->cd_path = NULL;
            break;
        }
    }

out:
    if (size != LC_BLOCK_SIZE) {
        memset(&buf[size], 0, LC_BLOCK_SIZE - size);
    }
    fuse_reply_buf(req, buf, LC_BLOCK_SIZE);
    if (size == 0) {
        lc_printf("Diff done on layer %d\n", fs->fs_gindex);
    }
}

/* Free the list created for tracking changes in the layer */
void
lc_freeChangeList(struct fs *fs) {
    struct cdir *cdir = fs->fs_changes, *dir;
    struct cfile *cfile, *file;

    while (cdir) {
        cfile = cdir->cd_file;
        while (cfile) {
            file = cfile;
            cfile = cfile->cf_next;
            lc_free(fs, file, sizeof(struct cfile), LC_MEMTYPE_CFILE);
        }
        if (cdir->cd_path) {
            lc_free(fs, cdir->cd_path, cdir->cd_len, LC_MEMTYPE_PATH);
        }
        dir = cdir;
        cdir = cdir->cd_next;
        lc_free(fs, dir, sizeof(struct cdir), LC_MEMTYPE_CDIR);
    }
    fs->fs_changes = NULL;
}

/* Produce diff between a layer and its parent layer */
int
lc_layerDiff(fuse_req_t req, const char *name, size_t size) {
    struct gfs *gfs = getfs();
    struct fs *fs, *rfs;
    struct inode *inode;
    ino_t ino, lastIno;
    char *data;
    int i;

    /* Respond to plugin checking whether swapping of layers enabled or not */
    if (!strcmp(name, ".")) {
        assert(size == sizeof(uint64_t));
        data = alloca(sizeof(uint64_t));
        if (gfs->gfs_swapLayersForCommit) {
            memset(data, 0xff, sizeof(uint64_t));
        } else {
            memset(data, 0, sizeof(uint64_t));
        }
        fuse_reply_buf(req, data, sizeof(uint64_t));
        return 0;
    }
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);
    ino = lc_getRootIno(rfs, name, NULL, true);
    if (ino == LC_INVALID_INODE) {
        lc_unlock(rfs);
        return EINVAL;
    }
    fs = lc_getLayerLocked(ino, true);
    assert(fs->fs_root == lc_getInodeHandle(ino));

    /* Layer diff is bypassed when layers are swapped during commit */
    if (gfs->gfs_swapLayersForCommit) {
        assert(size == sizeof(uint64_t));
        fuse_reply_buf(req, (char *)&fs->fs_size, sizeof(uint64_t));
        goto out;
    }
    assert(size == LC_BLOCK_SIZE);
    if (fs->fs_removed || fs->fs_rfs->fs_restarted ||
        (fs->fs_parent == NULL)) {
        lc_unlock(fs);
        lc_unlock(rfs);
        fuse_reply_err(req, EIO);
        return 0;
    }

    /* If this is a continuation request, respond with remaining diff data */
    if (fs->fs_changes) {
        lc_replyDiff(req, fs);
        lc_unlock(fs);
        lc_unlock(rfs);
        return 0;
    }
    lc_printf("Starting diff on layer %d\n", fs->fs_gindex);

    lc_lock(fs->fs_parent, false);
    lastIno = fs->fs_parent->fs_super->sb_lastInode;

    /* Add the root inode to the change list first */
    lc_addDirectory(fs, fs->fs_rootInode, NULL, 0, lastIno, LC_MODIFIED);

    /* Traverse inode cache, looking for modified directories in this layer */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {

            /* Skip removed directories and those already processed */
            if (S_ISDIR(inode->i_mode) &&
                !(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_CTRACKED))) {
                lc_addDirectory(fs, inode, NULL, 0, lastIno,
                                lc_changeInode(inode->i_ino, lastIno));
            }
            inode = inode->i_cnext;
        }
    }

    /* Traverse inode cache, looking for modified files in this layer */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {

            /* Skip removed files and those already processed */
            if (!(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_CTRACKED)) &&
                !S_ISDIR(inode->i_mode)) {
                lc_addModifiedInode(fs, inode, lastIno);
            }
            inode = inode->i_cnext;
        }
    }
    lc_unlock(fs->fs_parent);
    lc_replyDiff(req, fs);

    /* Reset LC_INODE_CTRACKED flags on inodes */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {
            inode->i_flags &= ~LC_INODE_CTRACKED;
            inode = inode->i_cnext;
        }
    }

out:
    lc_unlock(fs);
    lc_unlock(rfs);
    return 0;
}
