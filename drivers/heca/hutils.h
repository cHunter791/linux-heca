/*
 * Benoit Hudzia <benoit.hudzia@gmail.com> 2013(c) 
 */

/*
 *  Printk Helper
 */


void __heca_printk(const char *, int, const char *, const char *, ...);
#define heca_printk(fmt, ...) \
        __heca_printk(__FILE__, __LINE__, __func__, fmt,##__VA_ARGS__)


/*
 * Kobject Macro helpers 
 */

#define ATTR_NAME(_name) attr_instance_##_name

#define INSTANCE_ATTR(_type, _name, _mode, _show, _store)  \
        static _type ATTR_NAME(_name) = {  \
                .attr   = {.name = __stringify(_name), .mode = _mode }, \
                .show   = _show,                    \
                .store  = _store,                   \
        };
