#ifndef foosourceextfoo
#define foosourceextfoo

#include "userdata.h"

struct pa_source;

struct pa_null_source {
    char            *name;
    pa_source       *source;
};

struct pa_source_evsubscr {
    pa_hook_slot    *put;
    pa_hook_slot    *unlink;
};

struct pa_source_evsubscr *pa_source_ext_subscription(struct userdata *);
void  pa_source_ext_subscription_free(struct pa_source_evsubscr *);
void  pa_source_ext_discover(struct userdata *);
const char *pa_source_ext_get_name(struct pa_source *);
int   pa_source_ext_set_mute(struct userdata *, const char *, int);
int   pa_source_ext_set_ports(struct userdata *, const char *);
struct pa_null_source *pa_source_ext_init_null_source(const char *name);
void pa_source_ext_null_source_free(struct pa_null_source *);

#endif /* foosourceextfoo */

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
