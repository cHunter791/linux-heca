/*
 * Benoit Hudzia <benoit.hudzia@gmail.com 2013 (c)
 *
 *
 * Hecatonchire Koject model 
 *
 * Root: Heca 
 * Kset: hspaces - hprocs - mrs - transports- type - interfaces - conns
 * kobject: hspace - hproc - mr - nic - conn
 *
 * Hierarchy:
 *
 *
 *      heca-------hspaces--------hspace-------hprocs--hproc
 *                                         |
 *                                         |---mrs-----mr
 *                                         
 *                                         
 * 
 *      Symlink from hproc to hspace  and mr to hproc 
 *      
 *
 *
 *      Heca-------transports------RDMA---nic1---connection
 *                              |
 *                              ---other?
 *      
 *      Symlink from connections to hproc and hproc to connections
 *
 *
 *
 *
 */

#include "ioctl.h"
#include "struct.h"
#include "base.h"



/*
 * defining teh ksets and kobjects names 
 */

#define HECA_MODULE_KSET        "heca"
#define HSPACES_KSET            "spaces"
#define TRANSPORTS_KSET         "transports"
#define HPROCS_KSET             "process"
#define MRS_KSET                "memory_regions"
#define HSPACE_KOBJECT          "%u"
#define HPROC_KOBJECT           "%u"
#define MR_KOBJECT              "%u"

/*
 * Macro helpers 
 */

#define ATTR_NAME(_name) attr_instance_##_name

#define INSTANCE_ATTR(_type, _name, _mode, _show, _store)  \
        static _type ATTR_NAME(_name) = {  \
                .attr   = {.name = __stringify(_name), .mode = _mode }, \
                .show   = _show,                    \
                .store  = _store,                   \
        };

#define to_hspace(s)            container_of(s, struct heca_space, kobj)
#define to_hspace_attr(sa)      container_of(sa, struct hspace_attr, attr)
#define to_hproc(p)             container_of(s, struct heca_process, kobj)
#define to_hproc_attr(pa)       container_of(pa. struct hproc_attr, attr)
#define to_mr(m)                container_of(m, struct heca_memory_region, kobj)
#define to_mr_attr(ma)          container_of(ma, struct hmr_attr, attr)




/*
 * Heca Space Kset and Kobject
 */

struct hspace_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_space *, char *);
        ssize_t(*store)(struct heca_space *, char *, size_t);
};

static void kobj_hspace_release(struct kobject *k)
{
        heca_printk(KERN_DEBUG, "Releasing kobject %p", k);
}

static ssize_t heca_space_show(struct kobject *k, struct attribute *a,
                char *buffer)
{
        struct heca_space *hspace = to_hspace(k);
        struct hspace_attr *hspace_attr = to_hspace_attr(a);
        if (hspace_attr->show)
                return hspace_attr->show(hspace,buffer);
        return 0;
}


static struct hspace_attr *hspace_attr[] = {
        NULL
};

static struct sysfs_ops heca_space_ops = {
        .show = heca_space_show,
};

static struct kobj_type ktype_hspace = {
        .release = kobj_hspace_release,
        .sysfs_ops = &heca_space_ops,
        .default_attrs = (struct attribute **) hspace_attr,
};


/* 
 * Dynamic function
 */

int setup_heca_module_state_ksets(struct heca_module_state *heca_state)
{
        heca_state->root_kset = kset_create_and_add(HECA_MODULE_KSET, NULL,
                        NULL);
        if(!heca_state->root_kset)
                return -ENOMEM;
        heca_state->hspaces_kset = kset_create_and_add(HSPACES_KSET, NULL,
                        &heca_state->root_kset->kobj);
        if(!heca_state->hspaces_kset)
                goto err_root;
        heca_state->transports_kset = kset_create_and_add(TRANSPORTS_KSET,
                        NULL, &heca_state->root_kset->kobj);
        if(!heca_state->transports_kset)
                goto err_hspaces;

        return 0;

err_hspaces:
        kset_unregister(heca_state->hspaces_kset);
err_root:
        kset_unregister(heca_state->root_kset);
        return -ENOMEM;
}

void cleanup_heca_module_state_ksets(struct heca_module_state *heca_state)
{
        kset_unregister(heca_state->transports_kset);
        kset_unregister(heca_state->hspaces_kset);
        kset_unregister(heca_state->root_kset);
}




