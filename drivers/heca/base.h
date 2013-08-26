/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */

#ifndef _HECA_BASE_H
#define _HECA_BASE_H

#include "struct.h"
#include "hecatonchire.h"

/* conn */
struct heca_connection *search_rb_conn(int);
void insert_rb_conn(struct heca_connection *);
void erase_rb_conn(struct heca_connection *);

/* ps */
int pushback_ps(struct hecaioc_ps *udata);
int unmap_ps(struct hecaioc_ps *udata);

/* hcm */
int create_hcm_listener(struct heca_module_state *, unsigned long,
                unsigned short);
int destroy_hcm_listener(struct heca_module_state *);
int init_hcm(void);
int fini_hcm(void);

#endif /* _HECA_BASE_H */

