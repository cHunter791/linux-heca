/*
 * Benoit Hudzia <benoit.hudzia@gmail.com> 2013 (c)
 */
#include <linux/string.h>
#include <linux/printk.h>

#include "hutils.h"

#ifdef CONFIG_HECA_VERBOSE_PRINTK
/* strip the leading path if the given path is absolute */
static const char *sanity_file_name(const char *path)
{
        if (*path == '/')
                return strrchr(path, '/') + 1;
        else
                return path;
}
#endif

void __heca_printk(const char *file, int line,
                const char *func, const char *format, ...)
{
#if defined(CONFIG_HECA_DEBUG) || defined(CONFIG_HECA_VERBOSE_PRINTK)
        int kern_level;
        va_list args;
        struct va_format vaf;
        char verbose_fmt[] = KERN_DEFAULT "heca:"
#ifdef CONFIG_HECA_VERBOSE_PRINTK
                " %s:%d [%s]"
#endif
                " %pV\n";

        va_start(args, format);
        vaf.fmt = format;
        vaf.va = &args;

        kern_level = printk_get_level(format);
        if (kern_level) {
                const char *end_of_header = printk_skip_level(format);
                memcpy(verbose_fmt, format, end_of_header - format);
                vaf.fmt = end_of_header;
        }

        printk(verbose_fmt,
#ifdef CONFIG_HECA_VERBOSE_PRINTK
                        sanity_file_name(file), line, func,
#endif
                        &vaf);

        va_end(args);
#endif
}
EXPORT_SYMBOL(__heca_printk);


