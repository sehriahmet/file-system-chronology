#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "ext2fs.h" 

int img_fd = -1;

FILE *state_output_fp = NULL;
FILE *history_output_fp = NULL;

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
    int from_ghost;
};

struct HistoryEvent {
    long timestamp;
    long access_timestamp;
    int type; // typeguide: 1 touch, 2 mkdir, 3 rm, 4 rmdir, 5 mv file , 6 mv directory  
    char path[4096];
    char new_path[4096];
    uint32_t inode;
    uint32_t p_inode;
    uint32_t p_inode2;
};

struct HistoryEvent history_events[2048];
int history_event_count = 0;

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

void add_creation_event(uint32_t inode_no, uint32_t parent_inode, const char *full_path, int is_dir) {
    struct ext2_inode di;
    read_inode(inode_no, &di);
    struct ext2_inode di2;
    read_inode(parent_inode, &di2);

    long ts = di.access_time;

    for (int i =0 ; i<history_event_count; i++) {
        if (inode_no == history_events[i].inode && (history_events[i].type == 5 || history_events[i].type == 6) && (!history_events[i].p_inode2)) {
            history_events[i].p_inode2 = parent_inode;
            // history_events[i].new_path = (full_path);
            strncpy(history_events[i].new_path, full_path, 4096);
            // history_events[i].timestamp = di2.modification_time;

            // struct ext2_inode di3;
            // read_inode(history_events[i].p_inode, &di3);
            history_events[i].timestamp = di.change_time;
            // di is moved file 
            // di2->new path and di3->old path are the parent folders 
            // if (di2.modification_time == di3.modification_time) {
            //     history_events[i].timestamp = -1;
            // }

            return;
        }
    }

    struct HistoryEvent ev;
    ev.timestamp = ts;
    ev.access_timestamp = di.access_time;
    ev.inode = inode_no;
    ev.p_inode = parent_inode;
    ev.p_inode2 = 0; 
    if (is_dir) {
        ev.type = 2;
    } else {
        ev.type = 1;
    }

    strncpy(ev.path, full_path, 4096);
    ev.new_path[0] = '\0';

    if (history_event_count < 2048) {
        history_events[history_event_count] = ev;
        history_event_count++;
    }
}

void add_deletion_event(uint32_t inode_no, uint32_t parent_inode, const char *full_path, int is_dir) {
    struct ext2_inode di;
    read_inode(inode_no, &di);
    struct ext2_inode di2;
    read_inode(parent_inode, &di2);

    long ts = di.deletion_time ? di.deletion_time : (-1);
    
    

    struct HistoryEvent ev;
    ev.timestamp = ts;
    ev.access_timestamp = di.access_time;
    ev.inode = inode_no;
    ev.p_inode = parent_inode;
    ev.p_inode2 = 0; 

    if (is_dir && di.deletion_time != 0) {
        ev.type = 4;
    } else if (!is_dir && di.deletion_time != 0) {
        ev.type = 3;
    } else if (!is_dir && di.deletion_time == 0) {
        ev.type = 5;
        ev.timestamp = -1;
    } else {
        ev.type = 6;
        ev.timestamp = -1;
    }

    strncpy(ev.path, full_path, 4096);
    ev.new_path[0] = '\0';

    if (history_event_count < 2048) {
        history_events[history_event_count] = ev;
        history_event_count++;
    }
}


void process_and_write_history() {
    for (int i = 0; i < history_event_count - 1; i++) {
        for (int j = 0; j < history_event_count - i - 1; j++) {
            if (history_events[j].timestamp > history_events[j + 1].timestamp) {
                struct HistoryEvent temp = history_events[j];
                history_events[j] = history_events[j + 1];
                history_events[j + 1] = temp;
            }
        }
    }
    
    for (int i=0; i < history_event_count; i++) {
        for (int j=i+1; j<history_event_count; j++) {
            if (history_events[i].timestamp == history_events[j].timestamp) {
                history_events[i].timestamp = -1;
            }
        }
    }

    for (int i = 0; i < history_event_count; i++) {

        if (i == 0) {
            if (history_events[i].timestamp == -1) {
            fprintf(history_output_fp,"%c ", '?');
            } else {
                fprintf(history_output_fp,"%ld ", history_events[i].timestamp);
            }
        } else {
            if (history_events[i].timestamp == -1) {
                fprintf(history_output_fp,"\n%c ", '?');
            } else {
                fprintf(history_output_fp,"\n%ld ", history_events[i].timestamp);
            }
        }

        switch (history_events[i].type) { // typeguide: 1 touch, 2 mkdir, 3 rm, 4 rmdir, 5 mv file , 6 mv directory  
            case 1:
                fprintf(history_output_fp, "touch [%s] [%u] [%u]", history_events[i].path, history_events[i].p_inode, history_events[i].inode);
                break;
            case 2:
                fprintf(history_output_fp, "mkdir [%s] [%u] [%u]", history_events[i].path, history_events[i].p_inode, history_events[i].inode);
                break;
            case 3:
                fprintf(history_output_fp, "rm [%s] [%u] [%u]", history_events[i].path, history_events[i].p_inode, history_events[i].inode);
                break;
            case 4:
                fprintf(history_output_fp, "rmdir [%s] [%u] [%u]", history_events[i].path, history_events[i].p_inode, history_events[i].inode);
                break;
            case 5:
                if (history_events[i].timestamp == -1 && history_events[i].p_inode2 == 0) fprintf(history_output_fp, "mv [%s %s] [%u %c] [%u]", history_events[i].path, "?", history_events[i].p_inode, '?', history_events[i].inode);
                else fprintf(history_output_fp, "mv [%s %s] [%u %u] [%u]", history_events[i].path, history_events[i].new_path, history_events[i].p_inode, history_events[i].p_inode2, history_events[i].inode);
                break;
            case 6:
                if (history_events[i].timestamp == -1 && history_events[i].p_inode2 == 0) fprintf(history_output_fp, "mv [%s %s] [%u %c] [%u]", history_events[i].path, "?", history_events[i].p_inode, '?', history_events[i].inode);
                else fprintf(history_output_fp, "mv [%s %s] [%u %u] [%u]", history_events[i].path, history_events[i].new_path, history_events[i].p_inode, history_events[i].p_inode2, history_events[i].inode);
                break;
        }

    }
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

void traverse_dir(uint32_t inode_no, int depth, uint32_t parent_inode, const char *parent_path, int frm_ghost) {
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

                char full_path[4096];
                if (strcmp(parent_path, "/") == 0) {
                    snprintf(full_path, 4096, "/%s", namebuf);
                } else {
                    snprintf(full_path, 4096, "%s/%s", parent_path, namebuf);
                }
                
                int flag =0;
                for (int i =0;i<history_event_count;i++) {
                    if (history_events[i].inode == inode_no) flag = 1;
                }


                add_creation_event(entry->inode, inode_no, full_path, real_items[real_count-1].is_dir);

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
                    ghost_items[ghost_count].from_ghost = frm_ghost;
                    ghost_count++;

                    char full_path[4096];
                    if (strcmp(parent_path, "/") == 0) {
                        snprintf(full_path, 4096, "/%s", gnamebuf);
                    } else {
                        snprintf(full_path, 4096, "%s/%s", parent_path, gnamebuf);
                    }
                    add_creation_event(ghost_entry->inode, inode_no, full_path, ghost_items[ghost_count-1].is_dir);
                    add_deletion_event(ghost_entry->inode, inode_no, full_path, ghost_items[ghost_count-1].is_dir);

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

            char child_path[4096];
            if (strcmp(parent_path, "/") == 0) {
                snprintf(child_path, 4096, "/%s", real_items[i].name);
            } else {
                snprintf(child_path, 4096, "%s/%s", parent_path, real_items[i].name);
            }

            traverse_dir(real_items[i].inode, depth + 1, inode_no, child_path, 0);
            
        } else {
            fprintf(state_output_fp, "%u:%s", real_items[i].inode, real_items[i].name);
        }
    }
    
    for (int i = 0; i < ghost_count; i++) {
        if (!ghost_items[i].from_ghost) {
            fputc('\n', state_output_fp);
            for (int d = 0; d < depth; d++) {
                fputc('-', state_output_fp);
            }
            fputc(' ', state_output_fp);
        }
        if (ghost_items[i].is_dir) {
            if (!ghost_items[i].from_ghost) fprintf(state_output_fp, "(%u:%s/)", ghost_items[i].inode, ghost_items[i].name);

            char child_path[4096];
            if (strcmp(parent_path, "/") == 0) {
                snprintf(child_path, 4096, "/%s", ghost_items[i].name);
            } else {
                snprintf(child_path, 4096, "%s/%s", parent_path, ghost_items[i].name);
            }

            traverse_dir(ghost_items[i].inode, depth + 1, inode_no, child_path, 1);
        } else {
            if (!ghost_items[i].from_ghost) fprintf(state_output_fp, "(%u:%s)", ghost_items[i].inode, ghost_items[i].name);
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
    // if (img_fd < 0) {
    //     perror("open(image)");
    //     return 0;
    // }

    state_output_fp = fopen(state_output, "w");
    history_output_fp = fopen(history_output, "w");

    // if (!state_output_fp) {
    //     perror("fopen(state_output)");
    //     close(img_fd);
    //     return EXIT_FAILURE;
    // }

    read_superblock();
    read_block_group_descriptors();

    fprintf(state_output_fp, "- %u:root/", EXT2_ROOT_INODE);
    traverse_dir(EXT2_ROOT_INODE, 2, EXT2_ROOT_INODE, "/", 0);

    process_and_write_history();

    fclose(state_output_fp);
    fclose(history_output_fp);
    close(img_fd);
    free(group_desc);
    return 0;
}
