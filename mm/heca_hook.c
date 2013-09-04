#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/heca_hook.h>
#include <linux/kref.h>

#if defined(CONFIG_HECA) || defined(CONFIG_HECA_MODULE)

#include <linux/writeback.h>
EXPORT_SYMBOL(set_page_dirty_balance);

#include <linux/rmap.h>
EXPORT_SYMBOL(page_lock_anon_vma_read);
EXPORT_SYMBOL(page_remove_rmap);
EXPORT_SYMBOL(page_address_in_vma);
EXPORT_SYMBOL(page_unlock_anon_vma_read);
EXPORT_SYMBOL(page_add_new_anon_rmap);
EXPORT_SYMBOL(anon_vma_prepare);
EXPORT_SYMBOL(page_move_anon_rmap);
EXPORT_SYMBOL(do_page_add_anon_rmap);

#include <linux/ksm.h>
EXPORT_SYMBOL(ksm_might_need_to_copy);
EXPORT_SYMBOL(ksm_madvise);

#include <linux/mm.h>
EXPORT_SYMBOL(__pte_alloc);
EXPORT_SYMBOL(vm_normal_page);
EXPORT_SYMBOL(handle_mm_fault);
EXPORT_SYMBOL(anon_vma_interval_tree_iter_next);
EXPORT_SYMBOL(anon_vma_interval_tree_iter_first);

#include <linux/huge_mm.h>
EXPORT_SYMBOL(split_huge_page);
EXPORT_SYMBOL(split_huge_page_to_list);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
EXPORT_SYMBOL(__split_huge_page_pmd);
#endif

#include <linux/gfp.h>
EXPORT_SYMBOL(alloc_pages_vma);

#include <linux/pagemap.h>
EXPORT_SYMBOL(linear_hugepage_index);
EXPORT_SYMBOL(__lock_page_or_retry);

#include <linux/swapops.h>
EXPORT_SYMBOL(migration_entry_wait);

#include <linux/memcontrol.h>
EXPORT_SYMBOL(mem_cgroup_uncharge_page);
EXPORT_SYMBOL(mem_cgroup_commit_charge_swapin);
EXPORT_SYMBOL(mem_cgroup_try_charge_swapin);
EXPORT_SYMBOL(mem_cgroup_cancel_charge_swapin);
EXPORT_SYMBOL(mem_cgroup_newpage_charge);

#include <linux/swap.h>
EXPORT_SYMBOL(rotate_reclaimable_page);
EXPORT_SYMBOL(lru_add_drain);
EXPORT_SYMBOL(try_to_free_swap);

#include <linux/mmu_notifier.h>
EXPORT_SYMBOL(__mmu_notifier_change_pte);
EXPORT_SYMBOL(__mmu_notifier_invalidate_page);

#include <asm-generic/pgtable.h>
EXPORT_SYMBOL(pmd_clear_bad);
EXPORT_SYMBOL(ptep_clear_flush);
EXPORT_SYMBOL(ptep_set_access_flags);

#include <linux/sched.h>
EXPORT_SYMBOL(find_task_by_vpid);
EXPORT_SYMBOL(find_task_by_pid_ns);

#include "internal.h"
EXPORT_SYMBOL(munlock_vma_page);

static const struct heca_hook_struct *hooks = NULL;
static struct kref hooks_kref;
DEFINE_MUTEX(hooks_mutex);



static void heca_hooks_release(struct kref *kref)
{
        mutex_lock(&hooks_mutex);
        hooks = NULL;
        mutex_unlock(&hooks_mutex);
}


const struct heca_hook_struct *heca_hooks_get(void)
{
        smp_mb();
        if (!hooks)
                return NULL;
        if (kref_get_unless_zero(&hooks_kref))
                return hooks;
        return NULL;
}
EXPORT_SYMBOL(heca_hooks_get);

int heca_hooks_put(void)
{
        return kref_put(&hooks_kref, heca_hooks_release);
}
EXPORT_SYMBOL(heca_hooks_put);

int heca_hook_register(const struct heca_hook_struct *hook)
{
        int r = 0;
        mutex_lock(&hooks_mutex);
        if(hooks){
                r = -EEXIST;
                goto exit;
        }
        hooks = hook;
        kref_init(&hooks_kref);
exit:
        mutex_unlock(&hooks_mutex);
        return r;
}
EXPORT_SYMBOL(heca_hook_register);

int heca_hook_unregister(void)
{
        return kref_put(&hooks_kref, heca_hooks_release);
}
EXPORT_SYMBOL(heca_hook_unregister);

#else
const struct heca_hook_struct *heca_hooks_get(void)
{
        return NULL;
}

int heca_hooks_put(void)
{
        return 0;
}

int heca_hook_register(const struct heca_hook_struct *hook)
{
        return 0;
}

int heca_hook_unregister(void)
{
        return 0;
}

#endif


