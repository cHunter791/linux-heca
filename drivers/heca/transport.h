#ifndef _HECA_TRANSPORT_H
#define _HECA_TRANSPORT_H

#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/in.h>
#include <linux/kobject.h>
#include <linux/list.h>

struct heca_module_state;

struct heca_transport_manager {
        int node_ip;

        struct mutex htm_mutex;

        struct rb_root connections_rb_tree_root;
        seqlock_t connections_lock;

	struct list_head transport_head;

        struct kobject kobj;
};

struct transport {
        int type;
	void* context;

	struct list_head transport_list;

	struct kset *transport_kset;
};

void teardown_htm(struct heca_transport_manager *);
int create_htm(struct heca_transport_manager *);
int create_htm_listener(struct heca_module_state *, unsigned long,
                unsigned short);
int init_htm(void);
int fini_htm(void);
int find_htm(struct heca_module_state *);
int destroy_htm_listener(struct heca_transport_manager *);
#endif /* _HECA_TRANSPORT_H */
