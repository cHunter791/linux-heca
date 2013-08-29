/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef MR_H_
#define MR_H_

#include <linux/types.h>
#include <linux/rbtree.h>

#include "ioctl.h"
#include "hproc.h"

struct heca_memory_region {
        unsigned long addr;
        unsigned long sz;
        u32 descriptor;
        u32 hmr_id;
        u32 flags;
        struct rb_node rb_node;
};


struct heca_memory_region *find_heca_mr(struct heca_process *, u32);
struct heca_memory_region *search_heca_mr_by_addr(struct heca_process *,
                unsigned long);
int create_heca_mr(struct hecaioc_hmr *udata);


#endif /* MR_H_ */
