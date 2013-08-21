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
#define HPROCS_KSET             "process"
#define MRS_KSET                "memory_regions"
#define HSPACE_KOBJECT          "hspace-%u"
#define HPROC_KOBJECT           "hproc-%u"
#define MR_KOBJECT              "mr-%u"

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
#define to_hspace_attr(sa)      container_of(sa, struct heca_space_attr, attr)
#define to_hproc(p)             container_of(s, struct heca_process, kobj)
#define to_hproc_attr(pa)       container_of(pa. struct heca_proc_attr, attr)
#define to_mr(m)                container_of(m, struct heca_memory_region, kobj)
#define to_mr_attr(ma)          container_of(ma, struct heca_mr_attr, attr)








