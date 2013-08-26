/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/heca.h>
#include <linux/heca_hook.h>

#include "hutils.h"
#include "hspace.h"
#include "ioctl.h"

#include "base.h"
#include "push.h"
#include "task.h"

#define HECA_MODULE_VERSION     "0.2.0"
#define HECA_MODULE_AUTHOR      "Benoit Hudzia"
#define HECA_MODULE_DESCRIPTION "Hecatonchire Module"
#define HECA_MODULE_LICENSE     "GPL"

#define HECA_NAME               "HECA"
#define HECA_NAME_MINOR         "heca"

#define HECA_RX_WQ              "heca_rx_wq"
#define HECA_TX_WQ              "heca_tx_wq"

#define HECA_MODULE_KOBJECT     "heca"
#define HSPACES_KSET            "spaces"
#define TRANSPORTS_KSET         "transports"

/*
 * Macro Helper
 */

#define to_hms(s)               container_of(s, struct heca_module_state, \
                                                root_kobj)
#define to_hms_attr(sa)         container_of(sa, struct hms_attr, attr)

/*
 * create the actual trace functions needed for heca.ko
 */
#define CREATE_TRACE_POINTS
#include "trace.h"

#ifdef CONFIG_HECA_DEBUG
static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level (0 = disable)");
#endif


static struct heca_module_state *heca_state;

inline struct heca_module_state *get_heca_module_state(void)
{
        return heca_state;
}

struct heca_module_state *create_heca_module_state(void)
{
        heca_state = kzalloc(sizeof(struct heca_module_state), GFP_KERNEL);
        BUG_ON(!(heca_state));
        INIT_RADIX_TREE(&heca_state->hspaces_tree_root,
                        GFP_KERNEL & ~__GFP_WAIT);
        INIT_RADIX_TREE(&heca_state->mm_tree_root, GFP_KERNEL & ~__GFP_WAIT);
        INIT_LIST_HEAD(&heca_state->hspaces_list);
        mutex_init(&heca_state->heca_state_mutex);
        spin_lock_init(&heca_state->radix_lock);
        heca_state->heca_tx_wq = alloc_workqueue(HECA_RX_WQ,
                        WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM , 0);
        heca_state->heca_rx_wq = alloc_workqueue(HECA_TX_WQ,
                        WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM , 0);
        return heca_state;
}

void destroy_heca_module_state(void)
{
        struct list_head *curr, *next;
        struct heca_space *hspace;

        list_for_each_safe (curr, next, &heca_state->hspaces_list) {
                hspace = list_entry(curr, struct heca_space, hspace_ptr);
                remove_hspace(hspace);
        }

        destroy_hcm_listener(heca_state);
        mutex_destroy(&heca_state->heca_state_mutex);
        destroy_workqueue(heca_state->heca_tx_wq);
        destroy_workqueue(heca_state->heca_rx_wq);
        kfree(heca_state);
        heca_state = NULL;
}

/*
 * Heca Root Kobject
 */

struct hms_attr {
        struct attribute attr;
        ssize_t(*show)(struct heca_module_state *, char *);
        ssize_t(*store)(struct heca_module_state *, char *, size_t);
};

static void kobj_hms_release(struct kobject *k)
{
        heca_printk(KERN_DEBUG, "Releasing kobject %p", k);
}

static ssize_t hms_show(struct kobject *k, struct attribute *a,
                char *buffer)
{
        struct heca_module_state *hms = to_hms(k);
        struct hms_attr *hms_attr = to_hms_attr(a);
        if (hms_attr->show)
                return hms_attr->show(hms,buffer);
        return 0;
}

static ssize_t hms_version_show(struct heca_module_state * hms, char *data)
{
        return sprintf(data,HECA_MODULE_VERSION"\n");
}

INSTANCE_ATTR(struct hms_attr, version, S_IRUGO, hms_version_show, NULL);

static struct hms_attr *hms_attr[] = {
        &ATTR_NAME(version),
        NULL
};

static struct sysfs_ops hms_ops = {
        .show = hms_show,
};

static struct kobj_type ktype_hms = {
        .release = kobj_hms_release,
        .sysfs_ops = &hms_ops,
        .default_attrs = (struct attribute **) hms_attr,
};


static int setup_heca_module_state_ksets(struct heca_module_state *heca_state)
{
        int retval;
        retval = kobject_init_and_add(&heca_state->root_kobj, &ktype_hms, NULL,
                        HECA_MODULE_KOBJECT);
        if(retval)
                return retval;
        heca_state->hspaces_kset = kset_create_and_add(HSPACES_KSET, NULL,
                        &heca_state->root_kobj);
        if(!heca_state->hspaces_kset)
                goto err_root;
        heca_state->transports_kset = kset_create_and_add(TRANSPORTS_KSET,
                        NULL, &heca_state->root_kobj);
        if(!heca_state->transports_kset)
                goto err_hspaces;

        return 0;

err_hspaces:
        kset_unregister(heca_state->hspaces_kset);
err_root:
        kobject_del(&heca_state->root_kobj);
        return -ENOMEM;
}

static void cleanup_heca_module_state_ksets(
                struct heca_module_state *heca_state)
{
        kset_unregister(heca_state->transports_kset);
        kset_unregister(heca_state->hspaces_kset);
        kobject_del(&heca_state->root_kobj);
}

/*
 * Heca Module file operations 
 */

static struct file_operations heca_fops = {
        .owner = THIS_MODULE,
        .unlocked_ioctl = heca_ioctl,
        .llseek = noop_llseek,
};

/*
 * Heca Module life Cycle 
 */

static struct miscdevice heca_misc = {
        MISC_DYNAMIC_MINOR,
        HECA_NAME_MINOR,
        &heca_fops,
};

const struct heca_hook_struct my_heca_hook = {
        .name = HECA_NAME,
        .fetch_page = heca_swap_wrapper,
        .pushback_page = push_back_if_remote_heca_page,
        .is_congested = heca_is_congested,
        .write_fault = heca_write_fault,
        .attach_task = heca_attach_task,
        .detach_task = heca_detach_task,
};

static int heca_init(void)
{
        struct heca_module_state *heca_state = create_heca_module_state();
        int rc;

        heca_printk(KERN_DEBUG "<enter>");
        setup_heca_module_state_ksets(heca_state);
        BUG_ON(!heca_state);
        heca_zero_pfn_init();
        rc = misc_register(&heca_misc);
        init_hcm();
        BUG_ON(heca_hook_register(&my_heca_hook));

        heca_printk(KERN_DEBUG "<exit> %d", rc);
        return rc;
}
module_init(heca_init);

static void heca_exit(void)
{
        struct heca_module_state *heca_state = get_heca_module_state();

        heca_printk(KERN_DEBUG "<enter>");
        BUG_ON(heca_hook_unregister());
        fini_hcm();
        misc_deregister(&heca_misc);
        cleanup_heca_module_state_ksets(heca_state);
        heca_zero_pfn_exit();
        destroy_heca_module_state();
        heca_printk(KERN_DEBUG "<exit>");
}
module_exit(heca_exit);

MODULE_VERSION(HECA_MODULE_VERSION);
MODULE_AUTHOR(HECA_MODULE_AUTHOR);
MODULE_DESCRIPTION(HECA_MODULE_DESCRIPTION);
MODULE_LICENSE(HECA_MODULE_LICENSE);
