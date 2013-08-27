#ifndef HECA_TRANSPORT_H_
#define HECA_TRANSPORT_H_

#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/in.h>
#include "struct.h"

struct heca_connections_manager {
        int node_ip;

        struct rdma_cm_id *cm_id;
        struct ib_device *dev;
        struct ib_pd *pd;
        struct ib_mr *mr;

        struct ib_cq *listen_cq;

        struct mutex hcm_mutex;

        struct rb_root connections_rb_tree_root;
        seqlock_t connections_lock;

        struct sockaddr_in sin;
};

int create_hcm_listener(struct heca_module_state *, unsigned long,
                unsigned short);
int destroy_hcm_listener(struct heca_module_state *);
int init_hcm(void);
int fini_hcm(void);

#endif /* HECA_TRANSPORT_H_ */
