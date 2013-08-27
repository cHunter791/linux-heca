#include <linux/pagemap.h>
#include "ioctl.h"
#include "trace.h"
#include "struct.h"
#include "base.h"
#include "conn.h"
#include "pull.h"
#include "push.h"
#include "sysfs.h"
#include "ops.h"
#include "task.h"
#include "transport_manager.h"
#include "rdma.h"

int init_hcm(void)
{
        init_kmem_heca_request_cache();
        init_kmem_deferred_gup_cache();
        init_heca_cache_kmem();
        init_heca_reader_kmem();
        init_heca_prefetch_cache_kmem();
        heca_init_descriptors();
        return 0;
}

int fini_hcm(void)
{
        destroy_heca_cache_kmem();
        destroy_heca_prefetch_cache_kmem();
        destroy_kmem_heca_request_cache();
        destroy_kmem_deferred_gup_cache();
        heca_destroy_descriptors();
        return 0;
}

int destroy_hcm_listener(struct heca_module_state *heca_state);

int create_hcm_listener(struct heca_module_state *heca_state, unsigned long ip,
                unsigned short port)
{
        int ret = 0;
        struct heca_connections_manager *hcm = kzalloc(
                        sizeof(struct heca_connections_manager), GFP_KERNEL);

        if (!hcm)
                return -ENOMEM;

        mutex_init(&hcm->hcm_mutex);
        seqlock_init(&hcm->connections_lock);
        hcm->node_ip = ip;
        hcm->connections_rb_tree_root = RB_ROOT;

        hcm->cm_id = rdma_create_id(server_event_handler, hcm, RDMA_PS_TCP,
                        IB_QPT_RC);
        if (IS_ERR(hcm->cm_id)) {
                hcm->cm_id = NULL;
                ret = PTR_ERR(hcm->cm_id);
                heca_printk(KERN_ERR "Failed rdma_create_id: %d", ret);
                goto failed;
        }

        hcm->sin.sin_family = AF_INET;
        hcm->sin.sin_addr.s_addr = hcm->node_ip;
        hcm->sin.sin_port = port;

        ret = rdma_bind_addr(hcm->cm_id, (struct sockaddr *)&hcm->sin);
        if (ret) {
                heca_printk(KERN_ERR "Failed rdma_bind_addr: %d", ret);
                goto failed;
        }

        hcm->pd = ib_alloc_pd(hcm->cm_id->device);
        if (IS_ERR(hcm->pd)) {
                ret = PTR_ERR(hcm->pd);
                hcm->pd = NULL;
                heca_printk(KERN_ERR "Failed id_alloc_pd: %d", ret);
                goto failed;
        }

        hcm->listen_cq = ib_create_cq(hcm->cm_id->device, listener_cq_handle,
                        NULL, hcm, 2, 0);
        if (IS_ERR(hcm->listen_cq)) {
                ret = PTR_ERR(hcm->listen_cq);
                hcm->listen_cq = NULL;
                heca_printk(KERN_ERR "Failed ib_create_cq: %d", ret);
                goto failed;
        }

        if ((ret = ib_req_notify_cq(hcm->listen_cq, IB_CQ_NEXT_COMP))) {
                heca_printk(KERN_ERR "Failed ib_req_notify_cq: %d", ret);
                goto failed;
        }

        hcm->mr = ib_get_dma_mr(hcm->pd, IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
        if (IS_ERR(hcm->mr)) {
                ret = PTR_ERR(hcm->mr);
                hcm->mr = NULL;
                heca_printk(KERN_ERR "Failed ib_get_dma_mr: %d", ret);
                goto failed;
        }

        heca_state->hcm = hcm;

        ret = rdma_listen(hcm->cm_id, 2);
        if (ret)
                heca_printk(KERN_ERR "Failed rdma_listen: %d", ret);
        return 0;

failed:
        destroy_hcm_listener(heca_state);
        return ret;
}

static int hcm_disconnect(struct heca_connections_manager *hcm)
{
        struct rb_root *root = &hcm->connections_rb_tree_root;
        struct rb_node *node = rb_first(root);
        struct heca_connection *conn;

        while (node) {
                conn = rb_entry(node, struct heca_connection, rb_node);
                node = rb_next(node);
                if (atomic_cmpxchg(&conn->alive, 1, 0)) {
                        rdma_disconnect(conn->cm_id);
                        destroy_connection(conn);
                }
        }

        while (rb_first(root))
                ;

        return 0;
}

int destroy_hcm_listener(struct heca_module_state *heca_state)
{
        int rc = 0;
        struct heca_connections_manager *hcm = heca_state->hcm;

        heca_printk(KERN_DEBUG "<enter>");

        if (!hcm)
                goto done;

        if (!list_empty(&heca_state->hspaces_list)) {
                heca_printk(KERN_INFO "can't delete hcm - hspaces exist");
                rc = -EBUSY;
        }

        hcm_disconnect(hcm);

        if (!hcm->cm_id)
                goto destroy;

        if (hcm->cm_id->qp) {
                ib_destroy_qp(hcm->cm_id->qp);
                hcm->cm_id->qp = NULL;
        }

        if (hcm->listen_cq) {
                ib_destroy_cq(hcm->listen_cq);
                hcm->listen_cq = NULL;
        }

        if (hcm->mr) {
                ib_dereg_mr(hcm->mr);
                hcm->mr = NULL;
        }

        if (hcm->pd) {
                ib_dealloc_pd(hcm->pd);
                hcm->pd = NULL;
        }

        rdma_destroy_id(hcm->cm_id);
        hcm->cm_id = NULL;

destroy:
        mutex_destroy(&hcm->hcm_mutex);
        kfree(hcm);
        heca_state->hcm = NULL;

done:
        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}

