#ifndef _HECA_TRANSPORT_H
#define _HECA_TRANSPORT_H

#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/in.h>
#include <linux/kobject.h>

struct heca_module_state;

struct heca_transport_manager {
        int node_ip;

        struct rdma_cm_id *cm_id;
        struct ib_device *dev;
        struct ib_pd *pd;
        struct ib_mr *mr;

        struct ib_cq *listen_cq;

        struct mutex htm_mutex;

        struct rb_root connections_rb_tree_root;
        seqlock_t connections_lock;

        struct sockaddr_in sin;

        struct kobject kobj;
};

struct transport {
        struct kset *transport_kset;
};

struct rdma_transport {
        struct kobject kobj;
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
