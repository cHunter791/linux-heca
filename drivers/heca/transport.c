/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>

#include "transport.h"
#include "hutils.h"

#include "conn.h"
#include "ops.h"
#include "ioctl.h"
#include "pull.h"
#include "rdma_transport.h"

#define HTM_KOBJECT          "transport_manager"

#define to_htm(s)            container_of(s, struct heca_transport_manager, kobj)
#define to_htm_attr(sa)      container_of(sa, struct htm_attr, attr)

static int tmp_char;

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
	struct transport *rtransport, *next;

        heca_printk(KERN_DEBUG "<enter>");

        if (!htm)
                goto done;

        if (!list_empty(&heca_state->hspaces_list)) {
                heca_printk(KERN_INFO "can't delete htm - hspaces exist");
                rc = -EBUSY;
        }

	list_for_each_entry_safe(rtransport, next, &htm->transport_head,
                        transport_list) {
                list_del(&rtransport->transport_list);
		rc = destroy_rdma((struct rdma_transport*)rtransport->context);
                rtransport->context = NULL;
                if (list_empty(&htm->transport_head)) {
                        heca_printk(KERN_INFO "list empty");
                }
	}

        htm_disconnect(htm);
        mutex_destroy(&htm->htm_mutex);
        kfree(htm);
        heca_state->htm = NULL;

done:
        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}

void teardown_htm(struct heca_transport_manager *htm)
{
        heca_printk(KERN_INFO "tearing down htm %p", htm);
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

static ssize_t heca_transport_manager_store(struct kobject *k,
                struct attribute *a, char *buffer, size_t count)
{
        struct heca_transport_manager *htm = to_htm(k);
        struct htm_attr *htm_attr = to_htm_attr(a);
        ssize_t ret;

        ret = htm_attr->store ? htm_attr->store(htm, buffer, count) : -EIO;

        return ret;
}
static ssize_t tmp_show(struct heca_transport_manager * htm, char *data)
{
        return sprintf(data, "%u\n", tmp_char);
}

static ssize_t tmp_store(struct heca_transport_manager * htm, char *data, size_t
                size)
{
        sscanf(data, "%u", &tmp_char);
        return size;
}

INSTANCE_ATTR(struct htm_attr, tmp, S_IWUSR | S_IRUGO, tmp_show, tmp_store);

static struct htm_attr *htm_attr[] = {
        &ATTR_NAME(tmp),
        NULL
};


static struct sysfs_ops heca_transport_manager_ops = {
        .show = heca_transport_manager_show,
        .store = heca_transport_manager_store,
};

static struct kobj_type ktype_htm = {
        .release = kobj_htm_release,
        .sysfs_ops = &heca_transport_manager_ops,
        .default_attrs = (struct attribute **) htm_attr,
};

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

int find_htm(struct heca_module_state *heca_state)
{
        return !!heca_state->htm;
}

int create_htm_listener(struct heca_module_state *heca_state, unsigned long ip,
                unsigned short port)
{
        int ret = 0;
        struct heca_transport_manager *htm = kzalloc(
                        sizeof(struct heca_transport_manager), GFP_KERNEL);
	struct transport *rtransport = kmalloc(sizeof(struct transport),
                        GFP_KERNEL);

        INIT_LIST_HEAD(&htm->transport_head);

        if (!htm)
                return -ENOMEM;

        list_add(&rtransport->transport_list, &htm->transport_head);
        mutex_init(&htm->htm_mutex);
        seqlock_init(&htm->connections_lock);
        htm->node_ip = ip;
        htm->connections_rb_tree_root = RB_ROOT;

        ret = create_rdma(heca_state, htm, rtransport, port);

        if(ret)
                return ret;

        ret = kobject_init_and_add(&htm->kobj, &ktype_htm,
                        &heca_state->root_kobj, HTM_KOBJECT);

        if(ret)
                goto kobj_err;

        return ret;
kobj_err:
        kobject_put(&htm->kobj);
        return ret;
}
