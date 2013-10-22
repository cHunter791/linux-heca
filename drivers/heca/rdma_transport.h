#ifndef _HECA_RDMA_H
#define _HECA_RDMA_H

#include <linux/seqlock.h>
#include <linux/in.h>
#include <linux/kobject.h>
#include <linux/list.h>

#include "transport.h"

#define RDMA	1

struct rdma_transport {
        struct kobject kobj;

	struct rdma_cm_id *cm_id;
        struct ib_device *dev;
        struct ib_pd *pd;
        struct ib_mr *mr;
        struct ib_cq *listen_cq;

	struct sockaddr_in sin;
};

int create_rdma(struct heca_module_state *, struct heca_transport_manager *,
                struct transport *, unsigned short);
int destroy_rdma(struct rdma_transport *);
#endif /* _HECA_RDMA_H */
