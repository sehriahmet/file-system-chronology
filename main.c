#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "ext2fs.h" 

int img_fd = -1;

FILE *state_output_fp = NULL;

uint32_t block_size = 0;
uint32_t inodes_per_grp = 0;
uint16_t inode_size = 0;
uint32_t blocks_per_group = 0;

struct ext2_super_block super_block;
struct ext2_block_group_descriptor *group_desc = NULL;

struct iNodeItem {
    uint32_t inode;
    char name[EXT2_MAX_NAME_LENGTH + 1]; 
    int is_dir; 
    int is_ghost; 
};

void read_superblock() {

    pread(img_fd, &super_block, sizeof(super_block), EXT2_SUPER_BLOCK_POSITION);

    block_size = 1 << (10 + super_block.log_block_size);

    inode_size = super_block.inode_size;
    inodes_per_grp = super_block.inodes_per_group;
    blocks_per_group = super_block.blocks_per_group;

    // TODO: if all complies after all delete the line below!
    // blocks_per_group = (super_block.block_count + super_block.blocks_per_group - 1) / super_block.blocks_per_group;
    // if (super_block.inode_size) {
    //     inode_size = super_block.inode_size;
    // } else {
    //     inode_size = 128;
    // }

}

void read_block_group_descriptors() {

    int gdt_offset; 

    if (block_size <= 1024) {
        gdt_offset = EXT2_SUPER_BLOCK_POSITION + EXT2_SUPER_BLOCK_SIZE; 
    } else {
        gdt_offset = block_size; 
    }

    int desc_size = blocks_per_group * sizeof(struct ext2_block_group_descriptor);
    group_desc = malloc(desc_size);

    pread(img_fd, group_desc, desc_size, gdt_offset);

}

void read_inode(uint32_t inode_no, struct ext2_inode *inode_out) {
    if (inode_no == 0 || inode_no > super_block.inode_count) {
        printf("inode number: %d", inode_no);
    }
    uint32_t index = inode_no - 1;
    uint32_t grp = index / inodes_per_grp;
    uint32_t idx_in_grp = index % inodes_per_grp;
    int inode_offset = group_desc[grp].inode_table * block_size + idx_in_grp * inode_size;
    pread(img_fd, inode_out, sizeof(struct ext2_inode), inode_offset);
    
}

void add_block(uint32_t **dir_blocks_ptr, int *dir_cap, int *block_count, uint32_t b) {
    int new_cap;
    
    if (*block_count >= *dir_cap) {

        if ((*dir_cap) == 0) {
            new_cap = 16;
        } else {
            new_cap = (*dir_cap) *2;
        }
        uint32_t *tmp = realloc(*dir_blocks_ptr, new_cap * sizeof(uint32_t));

        *dir_blocks_ptr = tmp;
        *dir_cap = new_cap;
    }
    (*dir_blocks_ptr)[(*block_count)] = b;
    (*block_count)++;
}

void traverse_dir(uint32_t inode_no, int depth) {
    int pointers;

    struct ext2_inode dir_inode;
    read_inode(inode_no, &dir_inode);

    uint32_t *dir_blocks = NULL;
    int dir_cap = 0;
    int block_count = 0;

    for (int i = 0; i < EXT2_NUM_DIRECT_BLOCKS; i++) {
        if (dir_inode.direct_blocks[i] != 0) {
            add_block(&dir_blocks, &dir_cap, &block_count, dir_inode.direct_blocks[i]);
        }
    }

    if (dir_inode.single_indirect != 0) {
        
        uint32_t *single_buf = malloc(block_size);

        int single_offset = dir_inode.single_indirect * block_size;

        pread(img_fd, single_buf, block_size, single_offset);
        pointers = block_size / sizeof(uint32_t);
        for (int i = 0; i < pointers; i++) {
            if (single_buf[i] != 0) {
                add_block(&dir_blocks, &dir_cap, &block_count, single_buf[i]);
            }
        }
        free(single_buf);
    }

    if (dir_inode.double_indirect != 0) {

        int dbl_offset = dir_inode.double_indirect * block_size;
        uint32_t *dbl_buf = malloc(block_size);

        pread(img_fd, dbl_buf, block_size, dbl_offset);
        int double_pointers = block_size / sizeof(uint32_t);
        for (int x = 0; x < double_pointers; x++) {
            if (dbl_buf[x] != 0) {

                int single_offset = dbl_buf[x] * block_size;
                uint32_t *single_buf = malloc(block_size);
                pread(img_fd, single_buf, block_size, single_offset);
                pointers = block_size / sizeof(uint32_t);
                for (int i = 0; i < pointers; i++) {
                    if (single_buf[i] != 0) {
                        add_block(&dir_blocks, &dir_cap, &block_count, single_buf[i]);
                    }
                }
                free(single_buf);
            }
        }
        free(dbl_buf);
    }

    if (dir_inode.triple_indirect != 0) {
        
        int tpl_offset = dir_inode.triple_indirect * block_size;
        uint32_t *tpl_buf = malloc(block_size);

        pread(img_fd, tpl_buf, block_size, tpl_offset);
        int triple_pointers = block_size / sizeof(uint32_t);
        for (int i = 0; i < triple_pointers; i++) {
            if (tpl_buf[i] != 0) {

                uint32_t dbl_blk2 = tpl_buf[i];
                off_t dbl_offset2 = (off_t)dbl_blk2 * block_size;
                uint32_t *dbl_buf2 = malloc(block_size);
                
                pread(img_fd, dbl_buf2, block_size, dbl_offset2);
                int double_pointers2 = block_size / sizeof(uint32_t);
                for (int j = 0; j < double_pointers2; j++) {
                    if (dbl_buf2[j] != 0) {
                        int single_offset2 = dbl_buf2[j] * block_size;
                        uint32_t *single_buf2 = malloc(block_size);
                        
                        pread(img_fd, single_buf2, block_size, single_offset2);
                        int s_ptrs2 = block_size / sizeof(uint32_t);
                        for (int c = 0; c < s_ptrs2; c++) {
                            if (single_buf2[c] != 0) {
                                add_block(&dir_blocks, &dir_cap, &block_count, single_buf2[c]);
                            }
                        }
                        free(single_buf2);
                    }
                }
                free(dbl_buf2);
            }
        }
        free(tpl_buf);
    }

    struct iNodeItem *real_items = malloc(sizeof(*real_items)  * 2048);
    struct iNodeItem *ghost_items = malloc(sizeof(*ghost_items) * 2048);
    
    int real_count  = 0;
    int ghost_count = 0;

    for (int i = 0; i < block_count; i++) {
        int blk_offset = dir_blocks[i] * block_size; 
        uint8_t *block_buf = malloc(block_size);

        pread(img_fd, block_buf, block_size, blk_offset);
        /* 3.a) Real girdileri topla */
        int offset = 0;
        while (offset < block_size) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block_buf + offset);

            // if (entry->inode == 0) {
            //     if (entry->length == 0) {
            //         offset += 4;
            //     } else {
            //         offset += entry->length;
            //     }
            //     continue;
            // } 

            // if (entry->length == 0) {
            //     offset += 4;
            //     continue;
            // }

            if (entry->inode == 0) {
                offset += entry->length;
                continue;
            }

            char namebuf[EXT2_MAX_NAME_LENGTH + 1];
            memset(namebuf, 0, sizeof(namebuf));
            memcpy(namebuf, block_buf + offset + sizeof(struct ext2_dir_entry), entry->name_length);
            namebuf[entry->name_length] = '\0';

            if (strcmp(namebuf, ".") != 0 && strcmp(namebuf, "..") != 0) {
                real_items[real_count].inode = entry->inode;
                strncpy(real_items[real_count].name, namebuf, EXT2_MAX_NAME_LENGTH);

                if (entry->file_type == EXT2_D_DTYPE) {
                    real_items[real_count].is_dir = 1;
                } else if (entry->file_type == EXT2_D_FTYPE) {
                    real_items[real_count].is_dir = 0;
                } 

                real_items[real_count].is_ghost = 0;
                real_count++;
            }

            // offset += entry->length ? entry->length : 4;

            offset += entry->length;
            
        }

        offset = 0;
        while (offset < block_size) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block_buf + offset);
            // if (entry->inode == 0 || entry->length == 0) {
            //     offset += entry->length ? entry->length : 4;
            //     continue;
            // }

            if (entry->inode == 0) {
                offset += entry->length;
                continue;
            }

            int minimal_len = sizeof(struct ext2_dir_entry) + entry->name_length;
            minimal_len = ((minimal_len + 3) / 4) * 4;
            
            if (entry->length < minimal_len || entry->length > block_size) {
                // offset += entry->length ? entry->length : 4;
                offset += entry->length;
                continue;
            }

            int ghost_offset = offset + minimal_len;
            int entry_end = offset + entry->length;
            while (ghost_offset + sizeof(struct ext2_dir_entry) <= entry_end) {
                struct ext2_dir_entry *ghost_entry = (struct ext2_dir_entry *)(block_buf + ghost_offset);

                if (ghost_entry->inode != 0 && ghost_entry->length > 0) {
                    char gnamebuf[EXT2_MAX_NAME_LENGTH + 1];
                    memset(gnamebuf, 0, sizeof(gnamebuf));
                    memcpy(gnamebuf, block_buf + ghost_offset + sizeof(struct ext2_dir_entry), ghost_entry->name_length);
                    gnamebuf[ghost_entry->name_length] = '\0';

                    ghost_items[ghost_count].inode = ghost_entry->inode;

                    strncpy(ghost_items[ghost_count].name, gnamebuf, EXT2_MAX_NAME_LENGTH);

                    if (ghost_entry->file_type == EXT2_D_DTYPE) {
                        ghost_items[ghost_count].is_dir = 1;
                    } else if (ghost_entry->file_type == EXT2_D_FTYPE) {
                        ghost_items[ghost_count].is_dir = 0;
                    } 

                    ghost_items[ghost_count].is_ghost = 1;
                    ghost_count++;

                    ghost_offset += ghost_entry->length;

                } else {
                    // printf("%d , %d, %d, %d hello world\n", ghost_entry->inode, ghost_entry->length, entry_end, ghost_offset);
                    break;
                }
            }
            // offset += entry->length ? entry->length : 4;
            offset+= entry->length;
        }

        free(block_buf);
    }
    
    for (int i = 0; i < real_count; i++) {
        fputc('\n', state_output_fp);
        for (int d = 0; d < depth; d++) {
            fputc('-', state_output_fp);
        }
        fputc(' ', state_output_fp);
        if (real_items[i].is_dir) {
            fprintf(state_output_fp, "%u:%s/", real_items[i].inode, real_items[i].name);
            traverse_dir(real_items[i].inode, depth + 1);
        } else {
            fprintf(state_output_fp, "%u:%s", real_items[i].inode, real_items[i].name);
        }
    }
    
    for (int i = 0; i < ghost_count; i++) {
        fputc('\n', state_output_fp);
        for (int d = 0; d < depth; d++) {
            fputc('-', state_output_fp);
        }
        fputc(' ', state_output_fp);
        if (ghost_items[i].is_dir) {
            fprintf(state_output_fp, "(%u:%s/)", ghost_items[i].inode, ghost_items[i].name);
        } else {
            fprintf(state_output_fp, "(%u:%s)", ghost_items[i].inode, ghost_items[i].name);
        }
    }

    free(dir_blocks);
    free(real_items);
    free(ghost_items);
}

int main(int argc, char *argv[]) {

    char *image_path = argv[1];
    char *state_output = argv[2];
    char *history_output = argv[3];
    /* history_output (argv[3]) 3.1.2 aşamasında kullanılmıyor */

    img_fd = open(image_path, O_RDONLY);
    if (img_fd < 0) {
        perror("open(image)");
        return 0;
    }
    state_output_fp = fopen(state_output, "w");

    // if (!state_output_fp) {
    //     perror("fopen(state_output)");
    //     close(img_fd);
    //     return EXIT_FAILURE;
    // }

    read_superblock();
    read_block_group_descriptors();

    fprintf(state_output_fp, "- %u:root/", EXT2_ROOT_INODE);
    traverse_dir(EXT2_ROOT_INODE, 2);

    fclose(state_output_fp);
    close(img_fd);
    free(group_desc);
    return 0;
}
