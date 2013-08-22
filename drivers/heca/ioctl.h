/*
 * Benoit Hudzia <benoit.hudzia@gmail.com> 2011 (c)
 */

#ifndef _HECA_IOCTL_H
#define _HECA_IOCTL_H


#define HECA_MODULE_VERSION     "0.2.0"
#define HECA_MODULE_AUTHOR      "Benoit Hudzia"
#define HECA_MODULE_DESCRIPTION "Hecatonchire Module"
#define HECA_MODULE_LICENSE     "GPL"

/* print */
void __heca_printk(const char *, int, const char *, const char *, ...);
#define heca_printk(fmt, ...) \
        __heca_printk(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* module */
inline struct heca_module_state *get_heca_module_state(void);
struct heca_module_state *create_heca_module_state(void);
void destroy_heca_module_state(void);

#endif /* _HECA_IOCTL_H */

