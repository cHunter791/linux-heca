/*
 * Benoit Hudzia <benoit.hudzia@gmail.com> 2011 (c)
 */

#ifndef _HECA_IOCTL_H
#define _HECA_IOCTL_H

#include <include/linux/fs.h>

long heca_ioctl(struct file *, unsigned int , unsigned long );

#endif /* _HECA_IOCTL_H */

