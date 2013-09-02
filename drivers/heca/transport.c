/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/errno.h>
#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>

#include "transport.h"
#include "hutils.h"

#include "conn.h"
#include "ops.h"
#include "ioctl.h"
#include "pull.h"

#define HTM_KOBJECT          "transport_manager"

#define to_htm(s)            container_of(s, struct heca_transport_manager, kobj)
#define to_htm_attr(sa)      container_of(sa, struct htm_attr, attr)

static int htm_disconnect(struct heca_transport_manager *htm)
{
        struct rb_root *root = &htm->connections_rb_tree_root;
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

int destroy_htm_listener(struct heca_transport_manager *htm)
{
        int rc = 0;
        struct heca_module_state *heca_state = get_heca_module_state();

        heca_printk(KERN_DEBUG "<enter>");

        if (!htm)
                goto done;

        if (!list_empty(&heca_state->hspaces_list)) {
                heca_printk(KERN_INFO "can't delete htm - hspaces exist");
                rc = -EBUSY;
        }

        htm_disconnect(htm);

        if (!htm->cm_id)
                goto destroy;

        if (htm->cm_id->qp) {
                ib_destroy_qp(htm->cm_id->qp);
                htm->cm_id->qp = NULL;
        }

        if (htm->listen_cq) {
                ib_destroy_cq(htm->listen_cq);
                htm->listen_cq = NULL;
        }

        if (htm->mr) {
                ib_dereg_mr(htm->mr);
                htm->mr = NULL;
        }

        if (htm->pd) {
                ib_dealloc_pd(htm->pd);
                htm->pd = NULL;
        }

        rdma_destroy_id(htm->cm_id);
        htm->cm_id = NULL;

destroy:
        mutex_destroy(&htm->htm_mutex);
        kfree(htm);
        heca_state->htm = NULL;

done:
        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}

void teardown_htm(struct heca_transport_manager *htm)
{
        heca_printk(KERN_INFO "tearing down htm %p htm id: %p", htm,
                        htm->cm_id);
        /* we remove sysfs entry */
        kobject_del(&htm->kobj);
        /* move refcount to zero and free it */
        kobject_put(&htm->kobj);
}

struct htm_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_transport_manager *, char *);
        ssize_t(*store)(struct heca_transport_manager *, char *, size_t);
};

static void kobj_htm_release(struct kobject *k)
{
        struct heca_transport_manager *htm = to_htm(k);
        destroy_htm_listener(htm);
}

static ssize_t heca_transport_manager_show(struct kobject *k,
                struct attribute *a, char *buffer)
{
        struct heca_transport_manager *htm = to_htm(k);
        struct htm_attr *htm_attr = to_htm_attr(a);
        if (htm_attr->show)
                return htm_attr->show(htm,buffer);
        return 0;
}

static struct htm_attr *htm_attr[] = {
        NULL
};

static struct sysfs_ops heca_transport_manager_ops = {
        .show = heca_transport_manager_show,
};

static struct kobj_type ktype_htm = {
        .release = kobj_htm_release,
        .sysfs_ops = &heca_transport_manager_ops,
        .default_attrs = (struct attribute **) htm_attr,
};

int create_htm(struct heca_transport_manager *htm)
{
        int r = 0;

        struct heca_transport_manager *new_htm = htm;
        struct heca_module_state *heca_state = get_heca_module_state();

        heca_printk(KERN_INFO "creating hspace");
        r = kobject_init_and_add(&new_htm->kobj, &ktype_htm,
                        &heca_state->root_kobj, HTM_KOBJECT);

        return r;
}

int init_htm(void)
{
        init_kmem_heca_request_cache();
        init_kmem_deferred_gup_cache();
        init_heca_cache_kmem();
        init_heca_reader_kmem();
        init_heca_prefetch_cache_kmem();
        heca_init_descriptors();
        return 0;
}

int fini_htm(void)
{
        destroy_heca_cache_kmem();
        destroy_heca_prefetch_cache_kmem();
        destroy_kmem_heca_request_cache();
        destroy_kmem_deferred_gup_cache();
        heca_destroy_descriptors();
        return 0;
}


int create_htm_listener(struct heca_module_state *heca_state, unsigned long ip,
                unsigned short port)
{
        int ret = 0;
        struct heca_transport_manager *htm = kzalloc(
                        sizeof(struct heca_transport_manager), GFP_KERNEL);

        if (!htm)
                return -ENOMEM;

        mutex_init(&htm->htm_mutex);
        seqlock_init(&htm->connections_lock);
        htm->node_ip = ip;
        htm->connections_rb_tree_root = RB_ROOT;

        htm->cm_id = rdma_create_id(server_event_handler, htm, RDMA_PS_TCP,
                        IB_QPT_RC);
        if (IS_ERR(htm->cm_id)) {
                htm->cm_id = NULL;
                ret = PTR_ERR(htm->cm_id);
                heca_printk(KERN_ERR "Failed rdma_create_id: %d", ret);
                goto failed;
        }

        htm->sin.sin_family = AF_INET;
        htm->sin.sin_addr.s_addr = htm->node_ip;
        htm->sin.sin_port = port;

        ret = rdma_bind_addr(htm->cm_id, (struct sockaddr *)&htm->sin);
        if (ret) {
                heca_printk(KERN_ERR "Failed rdma_bind_addr: %d", ret);
                goto failed;
        }

        htm->pd = ib_alloc_pd(htm->cm_id->device);
        if (IS_ERR(htm->pd)) {
                ret = PTR_ERR(htm->pd);
                htm->pd = NULL;
                heca_printk(KERN_ERR "Failed id_alloc_pd: %d", ret);
                goto failed;
        }

        htm->listen_cq = ib_create_cq(htm->cm_id->device, listener_cq_handle,
                        NULL, htm, 2, 0);
        if (IS_ERR(htm->listen_cq)) {
                ret = PTR_ERR(htm->listen_cq);
                htm->listen_cq = NULL;
                heca_printk(KERN_ERR "Failed ib_create_cq: %d", ret);
                goto failed;
        }

        if ((ret = ib_req_notify_cq(htm->listen_cq, IB_CQ_NEXT_COMP))) {
                heca_printk(KERN_ERR "Failed ib_req_notify_cq: %d", ret);
                goto failed;
        }

        htm->mr = ib_get_dma_mr(htm->pd, IB_ACCESS_LOCAL_WRITE |
                        IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
        if (IS_ERR(htm->mr)) {
                ret = PTR_ERR(htm->mr);
                htm->mr = NULL;
                heca_printk(KERN_ERR "Failed ib_get_dma_mr: %d", ret);
                goto failed;
        }

        heca_state->htm = htm;

        ret = rdma_listen(htm->cm_id, 2);
        if (ret)
                heca_printk(KERN_ERR "Failed rdma_listen: %d", ret);

        create_htm(htm);
        return 0;

failed:
        teardown_htm(htm);
        return ret;
}



