/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/errno.h>

#include "hspace.h"
#include "hecatonchire.h"
#include "hutils.h"
#include "hproc.h"
#include "transport.h"
#include "base.h"


#define HSPACE_KOBJECT          "%u"
#define HPROCS_KSET             "hprocs"

#define to_hspace(s)            container_of(s, struct heca_space, kobj)
#define to_hspace_attr(sa)      container_of(sa, struct hspace_attr, attr)
/*
 * Creator / destructor
 */

static void teardown_hprocs(struct heca_space *hspace)
{
        struct heca_process *hproc;
        struct list_head *pos, *n;

        list_for_each_safe (pos, n, &hspace->hprocs_list) {
                hproc = list_entry(pos, struct heca_process, hproc_ptr);
                teardown_hproc(hproc);
        }

        kset_unregister(hspace->hprocs_kset);
}

void teardown_hspace(struct heca_space *hspace)
{
        heca_printk(KERN_INFO "Tearing Down hspace %p,hspace_id: %u ", hspace,
                        hspace->hspace_id );
        teardown_hprocs(hspace);
        /* we remove sysfs entry */
        kobject_del(&hspace->kobj);
        /* move refcount to zero and free it */
        kobject_put(&hspace->kobj);
}


/*
 * Heca Space  Kobject
 */

struct hspace_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_space *, char *);
        ssize_t(*store)(struct heca_space *, char *, size_t);
};

static void kobj_hspace_release(struct kobject *k)
{
        struct heca_space *hspace = to_hspace(k);
        struct heca_module_state *heca_state = get_heca_module_state();

        BUG_ON(!hspace);

        heca_printk(KERN_INFO "releasing hspace %p, hspace_id: %u ", hspace,
                        hspace->hspace_id);
        mutex_lock(&heca_state->heca_state_mutex);
        list_del(&hspace->hspace_ptr);
        radix_tree_delete(&heca_state->hspaces_tree_root,
                        (unsigned long) hspace->hspace_id);
        mutex_unlock(&heca_state->heca_state_mutex);
        synchronize_rcu();

        mutex_lock(&heca_state->heca_state_mutex);
        kfree(hspace);
        mutex_unlock(&heca_state->heca_state_mutex);

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
 * Main Hspace function
 */

int deregister_hspace(__u32 hspace_id)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        int ret = 0;
        struct heca_space *hspace;
        struct list_head *curr, *next;

        list_for_each_safe (curr, next, &heca_state->hspaces_list) {
                hspace = list_entry(curr, struct heca_space, hspace_ptr);
                if (hspace->hspace_id == hspace_id)
                        teardown_hspace(hspace);
        }

        destroy_htm_listener(heca_state->htm);
        return ret;
}

int register_hspace(struct hecaioc_hspace *hspace_info)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        int rc;

        heca_printk(KERN_INFO "creating htm listener");
        rc = find_htm(heca_state);

        if (!rc) {
                heca_printk(KERN_ERR "htm listener %p already exists",
                                heca_state->htm);
                return rc;
        }

        rc = create_htm_listener(heca_state, hspace_info->local.sin_addr.s_addr,
                        hspace_info->local.sin_port);
        if (rc)
                return rc;

        rc = create_hspace(hspace_info->hspace_id);
        if (rc) {
                heca_printk(KERN_ERR "create_hspace %d failed", rc);
                /* FIXME: add the htm release here*/
        }

        return rc;
}

struct heca_space *find_hspace(u32 id)
{
        struct heca_module_state *heca_state = get_heca_module_state();
        struct heca_space *hspace;
        struct heca_space **hspacep;
        struct radix_tree_root *root;

        rcu_read_lock();
        root = &heca_state->hspaces_tree_root;
repeat:
        hspace = NULL;
        hspacep = (struct heca_space **) radix_tree_lookup_slot(root,
                        (unsigned long) id);
        if (hspacep) {
                hspace = radix_tree_deref_slot((void **) hspacep);
                if (unlikely(!hspace))
                        goto out;
                if (radix_tree_exception(hspace)) {
                        if (radix_tree_deref_retry(hspace))
                                goto repeat;
                }
        }
out:
        rcu_read_unlock();
        return hspace;
}



int create_hspace(__u32 hspace_id)
{
        int r = 0;
        struct heca_space *found_hspace, *new_hspace = NULL;
        struct heca_module_state *heca_state = get_heca_module_state();

        /* already exists? (first check; the next one is under lock */
        found_hspace = find_hspace(hspace_id);
        if (found_hspace) {
                return -EEXIST;
        }

        /* allocate a new hspace */
        new_hspace = kzalloc(sizeof(*new_hspace), GFP_KERNEL);
        if (!new_hspace) {
                return -ENOMEM;
        }
        new_hspace->hspace_id = hspace_id;
        mutex_init(&new_hspace->hspace_mutex);
        INIT_RADIX_TREE(&new_hspace->hprocs_tree_root,
                        GFP_KERNEL & ~__GFP_WAIT);
        INIT_RADIX_TREE(&new_hspace->hprocs_mm_tree_root,
                        GFP_KERNEL & ~__GFP_WAIT);
        INIT_LIST_HEAD(&new_hspace->hprocs_list);

        while (1) {
                r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
                if (!r)
                        break;

                if (r == -ENOMEM) {
                        mdelay(2);
                        continue;
                }

                goto failed;
        }

        spin_lock(&heca_state->radix_lock);
        r = radix_tree_insert(&heca_state->hspaces_tree_root,
                        (unsigned long) new_hspace->hspace_id, new_hspace);
        spin_unlock(&heca_state->radix_lock);
        radix_tree_preload_end();

        if (r)
                goto failed;

        list_add(&new_hspace->hspace_ptr, &heca_state->hspaces_list);
        new_hspace->kobj.kset = heca_state->hspaces_kset;
        r = kobject_init_and_add(&new_hspace->kobj, &ktype_hspace, NULL,
                        HSPACE_KOBJECT, hspace_id);
        if(r)
                goto kobj_fail;
        new_hspace->hprocs_kset = kset_create_and_add(HPROCS_KSET, NULL,
                        &new_hspace->kobj);
        if(!new_hspace->hprocs_kset)
                goto kset_fail;
        heca_printk("registered hspace %p, hspace_id : %u, res: %d",
                        new_hspace, hspace_id, r);
        return r;

failed:
        kfree(new_hspace);
        return r;
kset_fail:
        kobject_del(&new_hspace->kobj);
kobj_fail:
        kobject_put(&new_hspace->kobj);
        return r;


}
