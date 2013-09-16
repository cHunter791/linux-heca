/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#ifndef HECATONCHIRE_H_
#define HECATONCHIRE_H_


#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/radix-tree.h>
#include <linux/kobject.h>

/* struct placeholder to avoid loading the .h */
struct heca_transport_manager;
struct heca_space;
struct heca_process;

struct heca_module_state {
        struct heca_transport_manager *htm;
        struct mutex heca_state_mutex;
        spinlock_t radix_lock;
        struct radix_tree_root hspaces_tree_root;
        struct radix_tree_root mm_tree_root;
        struct list_head hspaces_list;

        struct workqueue_struct * heca_rx_wq;
        struct workqueue_struct * heca_tx_wq;

        struct kobject root_kobj;
        struct kset *hspaces_kset;
};

/* used to track the info associated with the file descriptor */
struct process_priv_data {
        u32 hspace_id;
        u32 hproc_id;
};

inline struct heca_module_state *get_heca_module_state(void);
struct heca_module_state *create_heca_module_state(void);

#endif /* HECATONCHIRE_H_ */
