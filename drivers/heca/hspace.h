/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef HSPACE_H_
#define HSPACE_H_

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/kobject.h>


struct heca_space {
        u32 hspace_id;

        struct radix_tree_root hprocs_tree_root;
        struct radix_tree_root hprocs_mm_tree_root;

        struct mutex hspace_mutex;
        struct list_head hprocs_list;

        struct list_head hspace_ptr;

        int nb_local_hprocs;

        struct kobject kobj;

};

struct heca_space *find_hspace(u32);
void remove_hspace(struct heca_space *);
int create_hspace(__u32);

#endif /* HSPACE_H_ */
