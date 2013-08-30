/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 * Steve Walsh <steve.walsh@sap.com> 2012 (c)
 */
#include <linux/pagemap.h>

#include "ioctl.h"
#include "trace.h"
#include "hutils.h"
#include "hproc.h"
#include "mr.h"


#include "struct.h"
#include "base.h"
#include "conn.h"
#include "pull.h"
#include "push.h"
#include "ops.h"
#include "task.h"


/*
 * conn_element funcs
 */
struct heca_connection *search_rb_conn(int node_ip)
{
        struct heca_transport_manager *htm = get_heca_module_state()->htm;
        struct rb_root *root;
        struct rb_node *node;
        struct heca_connection *this = 0;
        unsigned long seq;

        do {
                seq = read_seqbegin(&htm->connections_lock);
                root = &htm->connections_rb_tree_root;
                for (node = root->rb_node; node; this = 0) {
                        this = rb_entry(node, struct heca_connection, rb_node);

                        if (node_ip < this->remote_node_ip)
                                node = node->rb_left;
                        else if (node_ip > this->remote_node_ip)
                                node = node->rb_right;
                        else
                                break;
                }
        } while (read_seqretry(&htm->connections_lock, seq));

        return this;
}

void insert_rb_conn(struct heca_connection *conn)
{
        struct heca_transport_manager *htm = get_heca_module_state()->htm;
        struct rb_root *root;
        struct rb_node **new, *parent = NULL;
        struct heca_connection *this;

        write_seqlock(&htm->connections_lock);
        root = &htm->connections_rb_tree_root;
        new = &root->rb_node;
        while (*new) {
                this = rb_entry(*new, struct heca_connection, rb_node);
                parent = *new;
                if (conn->remote_node_ip < this->remote_node_ip)
                        new = &((*new)->rb_left);
                else if (conn->remote_node_ip > this->remote_node_ip)
                        new = &((*new)->rb_right);
        }
        rb_link_node(&conn->rb_node, parent, new);
        rb_insert_color(&conn->rb_node, root);
        write_sequnlock(&htm->connections_lock);
}

void erase_rb_conn(struct heca_connection *conn)
{
        struct heca_transport_manager *htm = get_heca_module_state()->htm;

        write_seqlock(&htm->connections_lock);
        rb_erase(&conn->rb_node, &htm->connections_rb_tree_root);
        write_sequnlock(&htm->connections_lock);
}


int unmap_ps(struct hecaioc_ps *udata)
{
        int r = -EFAULT;
        struct heca_space *hspace = NULL;
        struct heca_process *local_hproc = NULL;
        struct heca_memory_region *mr = NULL;
        struct mm_struct *mm = find_mm_by_pid(udata->pid);

        if (!mm) {
                heca_printk(KERN_ERR "can't find pid %d", udata->pid);
                goto out;
        }

        local_hproc = find_local_hproc_from_mm(mm);
        if (!local_hproc)
                goto out;

        hspace = local_hproc->hspace;

        mr = search_heca_mr_by_addr(local_hproc, (unsigned long) udata->addr);
        if (!mr)
                goto out;

        r = unmap_range(hspace, mr->descriptor, udata->pid, (unsigned long)
                        udata->addr, udata->sz);

out:
        if (local_hproc)
                hproc_put(local_hproc);
        return r;
}

int pushback_ps(struct hecaioc_ps *udata)
{
        int r = -EFAULT;
        unsigned long addr, start_addr;
        struct page *page;
        struct mm_struct *mm = find_mm_by_pid(udata->pid);

        if (!mm) {
                heca_printk(KERN_ERR "can't find pid %d", udata->pid);
                goto out;
        }

        addr = start_addr = ((unsigned long) udata->addr) & PAGE_MASK;
        for (addr = start_addr; addr < start_addr + udata->sz;
                        addr += PAGE_SIZE) {
                page = heca_find_normal_page(mm, addr);
                if (!page || !trylock_page(page))
                        continue;

                r = !push_back_if_remote_heca_page(page);
                if (r)
                        unlock_page(page);
        }

out:
        return r;
}

