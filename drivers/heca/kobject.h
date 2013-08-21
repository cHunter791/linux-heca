/*
 *Benoit Hudzia <benoit.hudzia@sap.com> 2013 (c)
 */
#ifndef _HECA_KOBJECT_H
#define _HECA_KOBJECT_H

#include <linux/kobject.h>
#include "struct.h"

int setup_heca_module_state_ksets(struct heca_module_state *);
void cleanup_heca_module_state_ksets(struct heca_module_state *);

#endif /* HECA_KOBJECT_H */
