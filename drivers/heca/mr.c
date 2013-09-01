/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/gfp.h>

#include "mr.h"
#include "hutils.h"
#include "hproc.h"

#include "ops.h"

#define MR_KOBJECT              "%u"

#define to_mr(m)                container_of(m, struct heca_memory_region, kobj)
#define to_mr_attr(ma)          container_of(ma, struct hmr_attr, attr)


void heca_memory_region_release(struct heca_memory_region * hmr)
{

                heca_printk(KERN_INFO "Releasing MR :  mr_id: %u", hmr->hmr_id);
                synchronize_rcu();
                kfree(hmr);

}

struct heca_memory_region *find_heca_mr(struct heca_process *hproc,
                u32 id)
{
        struct heca_memory_region *mr, **mrp;
        struct radix_tree_root *root;

        rcu_read_lock();
        root = &hproc->hmr_id_tree_root;
repeat:
        mr = NULL;
        mrp = (struct heca_memory_region **) radix_tree_lookup_slot(root,
                        (unsigned long) id);
        if (mrp) {
                mr = radix_tree_deref_slot((void **) mrp);
                if (unlikely(!mr))
                        goto out;
                if (radix_tree_exception(mr)) {
                        if (radix_tree_deref_retry(mr))
                                goto repeat;
                }
        }
out:
        rcu_read_unlock();
        return mr;
}

struct heca_memory_region *search_heca_mr_by_addr(struct heca_process *hproc,
                unsigned long addr)
{
        struct rb_root *root = &hproc->hmr_tree_root;
        struct rb_node *node;
        struct heca_memory_region *this = hproc->hmr_cache;
        unsigned long seq;

        /* try to follow cache hint */
        if (likely(this)) {
                if (addr >= this->addr && addr < this->addr + this->sz)
                        goto out;
        }

        do {
                seq = read_seqbegin(&hproc->hmr_seq_lock);
                for (node = root->rb_node; node; this = 0) {
                        this = rb_entry(node, struct heca_memory_region,
                                        rb_node);

                        if (addr < this->addr)
                                node = node->rb_left;
                        else if (addr > this->addr)
                                if (addr < (this->addr + this->sz))
                                        break;
                                else
                                        node = node->rb_right;
                        else
                                break;
                }
        } while (read_seqretry(&hproc->hmr_seq_lock, seq));

        if (likely(this))
                hproc->hmr_cache = this;

out:
        return this;
}

static int insert_heca_mr(struct heca_process *hproc,
                struct heca_memory_region *mr)
{
        struct rb_root *root = &hproc->hmr_tree_root;
        struct rb_node **new = &root->rb_node, *parent = NULL;
        struct heca_memory_region *this;
        int r;

        r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
        if (r)
                goto fail;

        write_seqlock(&hproc->hmr_seq_lock);

        /* insert to radix tree */
        r = radix_tree_insert(&hproc->hmr_id_tree_root,
                        (unsigned long) mr->hmr_id, mr);
        if (r)
                goto out;

        /* insert to rb tree */
        while (*new) {
                this = rb_entry(*new, struct heca_memory_region, rb_node);
                parent = *new;
                if (mr->addr < this->addr)
                        new = &((*new)->rb_left);
                else if (mr->addr > this->addr)
                        new = &((*new)->rb_right);
        }

        rb_link_node(&mr->rb_node, parent, new);
        rb_insert_color(&mr->rb_node, root);
out:
        radix_tree_preload_end();
        write_sequnlock(&hproc->hmr_seq_lock);
fail:
        return r;
}


int create_heca_mr(struct hecaioc_hmr *udata)
{
        int ret = 0, i;
        struct heca_space *hspace;
        struct heca_memory_region *mr = NULL;
        struct heca_process *local_hproc = NULL;

        hspace = find_hspace(udata->hspace_id);
        if (!hspace) {
                heca_printk(KERN_ERR "can't find hspace %d", udata->hspace_id);
                ret = -EFAULT;
                goto out;
        }

        local_hproc = find_local_hproc_from_list(hspace);
        if (!local_hproc) {
                heca_printk(KERN_ERR "can't find local hproc for hspace %d",
                                udata->hspace_id);
                ret = -EFAULT;
                goto out;
        }

        /* FIXME: Validate against every kind of overlap! */
        if (search_heca_mr_by_addr(local_hproc, (unsigned long) udata->addr)) {
                heca_printk(KERN_ERR "mr already exists at addr 0x%lx",
                                udata->addr);
                ret = -EEXIST;
                goto out;
        }

        mr = kzalloc(sizeof(struct heca_memory_region), GFP_KERNEL);
        if (!mr) {
                heca_printk(KERN_ERR "can't allocate memory for MR");
                ret = -ENOMEM;
                goto out_free;
        }

        mr->hmr_id = udata->hmr_id;
        mr->addr = (unsigned long) udata->addr;
        mr->sz = udata->sz;

        if (insert_heca_mr(local_hproc, mr)){
                heca_printk(KERN_ERR "insert MR failed  addr 0x%lx",
                                udata->addr);
                ret = -EFAULT;
                goto out_free;
        }
        mr->descriptor = heca_get_descriptor(hspace->hspace_id,
                        udata->hproc_ids);
        if (!mr->descriptor) {
                heca_printk(KERN_ERR "can't find MR descriptor for hproc_ids");
                ret = -EFAULT;
                goto out_remove_tree;
        }

        for (i = 0; udata->hproc_ids[i]; i++) {
                struct heca_process *owner;
                u32 hproc_id = udata->hproc_ids[i];

                owner = find_hproc(hspace, hproc_id);
                if (!owner) {
                        heca_printk(KERN_ERR "[i=%d] can't find hproc %d",
                                        i, hproc_id);
                        ret = -EFAULT;
                        goto out_remove_tree;
                }

                if (is_hproc_local(owner)) {
                        mr->flags |= MR_LOCAL;
                }

                hproc_put(owner);
        }

        if (udata->flags & UD_COPY_ON_ACCESS) {
                mr->flags |= MR_COPY_ON_ACCESS;
                if (udata->flags & UD_SHARED)
                        goto out_remove_tree;
        } else if (udata->flags & UD_SHARED) {
                mr->flags |= MR_SHARED;
        }

        if (!(mr->flags & MR_LOCAL) && (udata->flags & UD_AUTO_UNMAP)) {
                ret = unmap_range(hspace, mr->descriptor, local_hproc->pid,
                                mr->addr, mr->sz);
        }

        goto out;

out_remove_tree:
        rb_erase(&mr->rb_node, &local_hproc->hmr_tree_root);
out_free:
        kfree(mr);
out:
        if (local_hproc)
                hproc_put(local_hproc);
        heca_printk(KERN_INFO "id [%d] addr [0x%lx] sz [0x%lx] --> ret %d",
                        udata->hmr_id, udata->addr, udata->sz, ret);
        return ret;
}
