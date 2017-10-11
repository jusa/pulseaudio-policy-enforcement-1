#include <stdio.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/client.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/core.h>
#include <pulsecore/hook-list.h>

#include "classify.h"
#include "policy-group.h"
#include "client-ext.h"
#include "sink-ext.h"
#include "source-ext.h"
#include "card-ext.h"
#include "sink-input-ext.h"
#include "source-output-ext.h"



static const char *find_group_for_client(struct userdata *, struct pa_client *,
                                         pa_proplist *, uint32_t *);
#if 0
static char *arg_dump(int, char **, char *, size_t);
#endif

static void  pid_hash_free(struct pa_classify_pid_hash **);
static void  pid_hash_insert(struct pa_classify_pid_hash **, pid_t,
                             const char *, enum pa_classify_method,
                             const char *, const char *);
static void  pid_hash_remove(struct pa_classify_pid_hash **, pid_t,
                             const char *, enum pa_classify_method,
                             const char *);
static char *pid_hash_get_group(struct pa_classify_pid_hash **, pid_t,
                                pa_proplist *);
static struct pa_classify_pid_hash
            *pid_hash_find(struct pa_classify_pid_hash **, pid_t,
                           const char *, enum pa_classify_method, const char *,
                           struct pa_classify_pid_hash **);

static void streams_free(struct pa_classify_stream_def *);
static void streams_add(struct pa_classify_stream_def **, const char *,
                        enum pa_classify_method, const char *, const char *,
                        const char *, uid_t, const char *, const char *, uint32_t);
static const char *streams_get_group(struct pa_classify_stream_def **, pa_proplist *,
                                     const char *, uid_t, const char *, uint32_t *);
static struct pa_classify_stream_def
            *streams_find(struct pa_classify_stream_def **, pa_proplist *,
                          const char *, const char *, uid_t, const char *,
                          struct pa_classify_stream_def **);

static void devices_free(struct pa_classify_device *);
static void devices_add(struct userdata *,
                        struct pa_classify_device **, const char *,
                        const char *,  enum pa_classify_method, const char *, pa_hashmap *,
                        const char *module, const char *module_args,
                        uint32_t);
static int devices_classify(struct pa_classify_device *, pa_proplist *,
                            const char *, uint32_t, uint32_t, struct pa_classify_result **result);
static int devices_is_typeof(struct pa_classify_device_def *, pa_proplist *,
                             const char *, const char *,
                             struct pa_classify_device_data **);

static void cards_free(struct pa_classify_card *);
static void cards_add(struct pa_classify_card **, char *,
                      enum pa_classify_method[2], char **, char **, uint32_t[2]);
static int  cards_classify(struct pa_classify_card *, const char *, pa_hashmap *card_profiles,
                           uint32_t,uint32_t, bool reclassify, struct pa_classify_result **result);
static int card_is_typeof(struct pa_classify_card_def *, const char *,
                          const char *, struct pa_classify_card_data **, int *priority);

static int port_device_is_typeof(struct pa_classify_device_def *, const char *,
                                 const char *,
                                 struct pa_classify_device_data **);

static const char *method_str(enum pa_classify_method);

const char *get_property(const char *, pa_proplist *, const char *);

static pa_hook_result_t module_unlink_hook_cb(pa_core *c, pa_module *m, struct pa_classify *cl);


static struct pa_classify_result *classify_result_malloc(uint32_t type_count)
{
    struct pa_classify_result *r;

    r = pa_xmalloc(sizeof(struct pa_classify_result) +
                   sizeof(char *) * (type_count > 0 ? type_count - 1 : 0));
    r->count = 0;

    return r;
}

static void classify_result_append(struct pa_classify_result **r, const char *type)
{
    (*r)->types[(*r)->count] = type;
    (*r)->count++;
}

static void unload_module(pa_module *m)
{
    if (m) {
#if (PULSEAUDIO_VERSION >= 8)
        pa_module_unload(m, true);
#else
        pa_module_unload(u->core, m, true);
#endif
    }
}

struct pa_classify *pa_classify_new(struct userdata *u)
{
    struct pa_classify *cl;

    cl = pa_xnew0(struct pa_classify, 1);

    cl->sinks   = pa_xnew0(struct pa_classify_device, 1);
    cl->sources = pa_xnew0(struct pa_classify_device, 1);
    cl->cards   = pa_xnew0(struct pa_classify_card, 1);

    return cl;
}

void pa_classify_free(struct userdata *u)
{
    struct pa_classify *cl = u->classify;
    uint32_t i;

    if (cl) {
        pid_hash_free(cl->streams.pid_hash);
        streams_free(cl->streams.defs);
        devices_free(cl->sinks);
        devices_free(cl->sources);
        cards_free(cl->cards);
        if (cl->module_unlink_hook_slot)
            pa_hook_slot_free(cl->module_unlink_hook_slot);

        for (i = 0; i < PA_POLICY_MODULE_COUNT; i++)
            unload_module(cl->module[i].module);

        pa_xfree(cl);
    }
}

void pa_classify_add_sink(struct userdata *u, const char *type, const char *prop,
                          enum pa_classify_method method, const char *arg,
                          pa_hashmap *ports,
                          const char *module, const char *module_args,
                          uint32_t flags)
{
    struct pa_classify *classify;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sinks);
    pa_assert(type);
    pa_assert(prop);
    pa_assert(arg);

    devices_add(u, &classify->sinks, type, prop, method, arg, ports,
                module, module_args, flags);
}

void pa_classify_add_source(struct userdata *u, const char *type, const char *prop,
                            enum pa_classify_method method, const char *arg,
                            pa_hashmap *ports,
                            const char *module, const char *module_args,
                            uint32_t flags)
{
    struct pa_classify *classify;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sources);
    pa_assert(type);
    pa_assert(prop);
    pa_assert(arg);

    devices_add(u, &classify->sources, type, prop, method, arg, ports,
                module, module_args, flags);
}

void pa_classify_add_card(struct userdata *u, char *type,
                          enum pa_classify_method method[2], char **arg,
                          char **profiles, uint32_t flags[2])
{
    struct pa_classify *classify;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->cards);
    pa_assert(type);
    pa_assert(arg[0]);

    cards_add(&classify->cards, type, method, arg, profiles, flags);
}


void pa_classify_add_stream(struct userdata *u, const char *prop,
                            enum pa_classify_method method, const char *arg,
                            const char *clnam, const char *sname, uid_t uid,
                            const char *exe, const char *grnam,
                            uint32_t flags, const char *port)
{
    struct pa_classify     *classify;
    struct pa_policy_group *group;

    pa_assert(u);
    pa_assert_se((classify = u->classify));

    if (((prop && method && arg) || uid != (uid_t)-1 || exe) && grnam) {
        if (port) {
            if ((group = pa_policy_group_find(u, grnam)) == NULL) {
                flags &= ~PA_POLICY_LOCAL_ROUTE;
                pa_log("can't find group '%s' for stream", grnam);
            }
            else {
                group->portname = pa_xstrdup(port);
                pa_log_debug("set portname '%s' for group '%s'", port, grnam);
            }
        }

        streams_add(&classify->streams.defs, prop,method,arg,
                    clnam, sname, uid, exe, grnam, flags);
    }
}

void pa_classify_update_stream_route(struct userdata *u, const char *sname)
{
    struct pa_classify_stream_def *stream;

    pa_assert(u);
    pa_assert(u->classify);

    for (stream = u->classify->streams.defs;  stream;  stream = stream->next) {
        if (stream->sname) {
            if (pa_streq(stream->sname, sname))
                stream->sact = 1;
            else
                stream->sact = 0;
            pa_log_debug("stream group %s changes to %s state", stream->group, stream->sact ? "active" : "inactive");
        }
    }
}

void pa_classify_register_pid(struct userdata *u, pid_t pid, const char *prop,
                              enum pa_classify_method method, const char *arg,
                              const char *group)
{
    struct pa_classify *classify;

    pa_assert(u);
    pa_assert_se((classify = u->classify));

    if (pid && group) {
        pid_hash_insert(classify->streams.pid_hash, pid,
                        prop, method, arg, group);
    }
}

void pa_classify_unregister_pid(struct userdata *u, pid_t pid, const char *prop,
                                enum pa_classify_method method, const char *arg)
{
    struct pa_classify *classify;
    
    pa_assert(u);
    pa_assert_se((classify = u->classify));

    if (pid) {
        pid_hash_remove(classify->streams.pid_hash, pid, prop, method, arg);
    }
}

const char *pa_classify_sink_input(struct userdata *u, struct pa_sink_input *sinp,
                                   uint32_t *flags)
{
    struct pa_client     *client;
    const char           *group;

    pa_assert(u);
    pa_assert(sinp);

    client = sinp->client;
    group  = find_group_for_client(u, client, sinp->proplist, flags);

    return group;
}

const char *pa_classify_sink_input_by_data(struct userdata *u,
                                           struct pa_sink_input_new_data *data,
                                           uint32_t *flags)
{
    struct pa_client     *client;
    const char           *group;

    pa_assert(u);
    pa_assert(data);

    client = data->client;
    group  = find_group_for_client(u, client, data->proplist, flags);

    return group;
}

const char *pa_classify_source_output(struct userdata *u,
                                      struct pa_source_output *sout)
{
    struct pa_client     *client;
    const char           *group;

    pa_assert(u);
    pa_assert(sout);

    client = sout->client;
    group  = find_group_for_client(u, client, sout->proplist, NULL);

    return group;
}

const char *
pa_classify_source_output_by_data(struct userdata *u,
                                  struct pa_source_output_new_data *data)
{
    struct pa_client     *client;
    const char           *group;

    pa_assert(u);
    pa_assert(data);

    client = data->client;
    group  = find_group_for_client(u, client, data->proplist, NULL);

    return group;
}

int pa_classify_sink(struct userdata *u, struct pa_sink *sink,
                     uint32_t flag_mask, uint32_t flag_value,
                     struct pa_classify_result **result)
{
    struct pa_classify *classify;
    struct pa_classify_device *devices;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sinks);
    pa_assert_se((devices = classify->sinks));
    pa_assert(result);

    name = pa_sink_ext_get_name(sink);

    return devices_classify(devices, sink->proplist, name,
                            flag_mask, flag_value, result);
}

int pa_classify_source(struct userdata *u, struct pa_source *source,
                       uint32_t flag_mask, uint32_t flag_value,
                       struct pa_classify_result **result)
{
    struct pa_classify *classify;
    struct pa_classify_device *devices;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sources);
    pa_assert_se((devices = classify->sources));
    pa_assert(result);

    name = pa_source_ext_get_name(source);

    return devices_classify(devices, source->proplist, name,
                            flag_mask, flag_value, result);
}

int pa_classify_card(struct userdata *u, struct pa_card *card,
                     uint32_t flag_mask, uint32_t flag_value,
                     bool reclassify, struct pa_classify_result **result)
{
    struct pa_classify *classify;
    struct pa_classify_card *cards;
    const char *name;
    pa_hashmap *profs;

    pa_assert(u);
    pa_assert(result);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->cards);
    pa_assert_se((cards = classify->cards));

    name  = pa_card_ext_get_name(card);
    profs = pa_card_ext_get_profiles(card);

    return cards_classify(cards, name, profs, flag_mask,flag_value, reclassify, result);
}

int pa_classify_is_sink_typeof(struct userdata *u, struct pa_sink *sink,
                               const char *type,
                               struct pa_classify_device_data **d)
{
    struct pa_classify *classify;
    struct pa_classify_device_def *defs;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sinks);
    pa_assert_se((defs = classify->sinks->defs));

    if (!sink || !type)
        return false;

    name = pa_sink_ext_get_name(sink);

    return devices_is_typeof(defs, sink->proplist, name, type, d);
}


int pa_classify_is_source_typeof(struct userdata *u, struct pa_source *source,
                                 const char *type,
                                 struct pa_classify_device_data **d)
{
    struct pa_classify *classify;
    struct pa_classify_device_def *defs;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sources);
    pa_assert_se((defs = classify->sources->defs));

    if (!source || !type)
        return false;

    name = pa_source_ext_get_name(source);

    return devices_is_typeof(defs, source->proplist, name, type, d);
}


int pa_classify_is_card_typeof(struct userdata *u, struct pa_card *card,
                               const char *type, struct pa_classify_card_data **d, int *priority)
{
    struct pa_classify *classify;
    struct pa_classify_card_def *defs;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->cards);
    pa_assert_se((defs = classify->cards->defs));

    if (!card || !type)
        return false;

    name = pa_card_ext_get_name(card);

    return card_is_typeof(defs, name, type, d, priority);
}


int pa_classify_is_port_sink_typeof(struct userdata *u, struct pa_sink *sink,
                                    const char *type,
                                    struct pa_classify_device_data **d)
{
    struct pa_classify *classify;
    struct pa_classify_device_def *defs;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sinks);
    pa_assert_se((defs = classify->sinks->defs));

    if (!sink || !type)
        return false;

    name = pa_sink_ext_get_name(sink);

    return port_device_is_typeof(defs, name, type, d);
}


int pa_classify_is_port_source_typeof(struct userdata *u,
                                      struct pa_source *source,
                                      const char *type,
                                      struct pa_classify_device_data **d)
{
    struct pa_classify *classify;
    struct pa_classify_device_def *defs;
    const char *name;

    pa_assert(u);
    pa_assert_se((classify = u->classify));
    pa_assert(classify->sources);
    pa_assert_se((defs = classify->sources->defs));

    if (!source || !type)
        return false;

    name = pa_source_ext_get_name(source);

    return port_device_is_typeof(defs, name, type, d);
}


static int classify_update_module_load(struct userdata *u,
                                       uint32_t dir,
                                       struct pa_classify_module *m,
                                       struct pa_classify_device_data *devdata) {
    pa_assert(u);
    pa_assert(m);
    pa_assert(devdata);
    pa_assert(!m->module);

    pa_log_debug("Load module for %s: %s %s", dir == PA_POLICY_MODULE_FOR_SINK ? "sink" : "source",
                                              devdata->module,
                                              devdata->module_args ? devdata->module_args : "");

    m->module = pa_module_load(u->core,
                               devdata->module,
                               devdata->module_args);
    if (!m->module) {
        pa_log("Failed to load %s", devdata->module);
        return -1;
    }

    m->module_name = devdata->module;
    m->module_args = devdata->module_args;
    m->flags = devdata->flags;

    return 0;
}


static void classify_update_module_unload(struct userdata *u,
                                          uint32_t dir,
                                          struct pa_classify_module *m) {
    pa_assert(u);
    pa_assert(m);
    pa_assert(m->module);

    pa_log_debug("Unload %smodule for %s: %s",
                 m->flags & PA_POLICY_MODULE_UNLOAD_IMMEDIATELY ? "" : "request for ",
                 dir == PA_POLICY_MODULE_FOR_SINK ? "sink" : "source",
                 m->module_name);

    if (m->flags & PA_POLICY_MODULE_UNLOAD_IMMEDIATELY)
        unload_module(m->module);
    else
        pa_module_unload_request(m->module, true);

    m->module_name = NULL;
    m->module_args = NULL;
    m->module = NULL;
}


int pa_classify_update_module(struct userdata *u,
                              uint32_t dir,
                              struct pa_classify_device_data *devdata) {
    struct pa_classify_module *m;
    int ret = 0;

    pa_assert(u);
    pa_assert(devdata);
    pa_assert(dir < PA_POLICY_MODULE_COUNT);

    m = &u->classify->module[dir];

    if (m->module &&
        !pa_safe_streq(m->module_name, devdata->module) &&
        !pa_safe_streq(m->module_args, devdata->module_args))
        classify_update_module_unload(u, dir, m);

    if (devdata->module && !m->module)
        ret = classify_update_module_load(u, dir, m, devdata);

    return ret;
}


void pa_classify_update_modules(struct userdata *u, uint32_t dir, const char *type) {
    struct pa_classify_device_def *defs;
    struct pa_classify_device_def *d;
    struct pa_classify_device_def *new_def = NULL;
    struct pa_classify_module *m;
    bool need_to_unload = true;

    pa_assert(u);
    pa_assert(u->classify);
    pa_assert(u->classify->sources);
    pa_assert_se((defs = u->classify->sources->defs));

    m = &u->classify->module[dir];

    if (!m->module)
        return;

    for (d = defs;  d->type;  d++) {
        if (pa_streq(type, d->type)) {
            new_def = d;
            break;
        }
    }

    if (!new_def)
        return;

    for (d = defs;  d->type;  d++) {
        if (!pa_streq(type, d->type)) {
            if (d->data.module) {
                if (pa_safe_streq(m->module_name, new_def->data.module) &&
                    pa_safe_streq(m->module_args, new_def->data.module_args)) {
                    need_to_unload = false;
                    break;
                }
            }
        }
    }

    if (need_to_unload)
        classify_update_module_unload(u, dir, m);
}


static pa_hook_result_t module_unlink_hook_cb(pa_core *c, pa_module *m, struct pa_classify *cl) {
    uint32_t i;

    pa_assert(c);
    pa_assert(m);
    pa_assert(cl);

    for (i = 0; i < PA_POLICY_MODULE_COUNT; i++) {
        if (cl->module[i].module == m) {
            pa_log_debug("Module for %s unloading: %s",
                         i == PA_POLICY_MODULE_FOR_SINK ? "sink" : "source",
                         m->name);
            cl->module[i].module = NULL;
            cl->module[i].module_name = NULL;
            cl->module[i].module_args = NULL;
            break;
        }
    }

    return PA_HOOK_OK;
}


static const char *find_group_for_client(struct userdata  *u,
                                         struct pa_client *client,
                                         pa_proplist      *proplist,
                                         uint32_t         *flags_ret)
{
    struct pa_classify *classify;
    struct pa_classify_pid_hash **hash;
    struct pa_classify_stream_def **defs;
    pid_t       pid   = 0;          /* client processs PID */
    const char *clnam = "";         /* client's name in PA */
    uid_t       uid   = (uid_t) -1; /* client process user ID */
    const char *exe   = "";         /* client's binary path */
    const char *group = NULL;
    uint32_t  flags = 0;

    assert(u);
    pa_assert_se((classify = u->classify));

    hash = classify->streams.pid_hash;
    defs = &classify->streams.defs;

    if (client == NULL) {
        /* sample cache initiated sink-inputs don't have a client, but sample's proplist
         * contains PA_PROP_APPLICATION_PROCESS_BINARY anyway. Try to get this value
         * from proplist. This allows using 'exe' matching in stream definitions in xpolicy.conf
         * for sample cache initiated streams as well. */
        if (!(exe = pa_proplist_gets(proplist, PA_PROP_APPLICATION_PROCESS_BINARY)))
            exe = "";

        group = streams_get_group(defs, proplist, clnam, uid, exe, &flags);
    } else {
        pid = pa_client_ext_pid(client);

        if ((group = pid_hash_get_group(hash, pid, proplist)) == NULL) {
            clnam = pa_client_ext_name(client);
            uid   = pa_client_ext_uid(client);
            exe   = pa_client_ext_exe(client);

            group = streams_get_group(defs, proplist, clnam, uid, exe, &flags);
        }
    }

    if (group == NULL)
        group = PA_POLICY_DEFAULT_GROUP_NAME;

    pa_log_debug("%s (%s|%d|%d|%s) => %s,0x%x", __FUNCTION__,
                 clnam?clnam:"<null>", pid, uid, exe?exe:"<null>",
                 group?group:"<null>", flags);

    if (flags_ret != NULL)
        *flags_ret = flags;

    return group;
}

#if 0
static char *arg_dump(int argc, char **argv, char *buf, size_t len)
{
    char *p = buf;
    int   i, l;
    
    if (argc <= 0 || argv == NULL)
        snprintf(buf, len, "0 <null>");
    else {
        l = snprintf(p, len, "%d", argc);
        
        p   += l;
        len -= l;
        
        for (i = 0;  i < argc && len > 0;  i++) {
            l = snprintf(p, len, " [%d]=%s", i, argv[i]);
            
            p   += l;
            len -= l;
        }
    }
    
    return buf;
}
#endif

static void pid_hash_free(struct pa_classify_pid_hash **hash)
{
    struct pa_classify_pid_hash *st;
    int i;

    assert(hash);

    for (i = 0;   i < PA_POLICY_PID_HASH_MAX;   i++) {
        while ((st = hash[i]) != NULL) {
            hash[i] = st->next;

            pa_xfree(st->prop);
            pa_xfree(st->group);
            pa_xfree(st->arg.def);

            if (st->method.type == pa_method_matches)
                regfree(&st->arg.value.rexp);

            pa_xfree(st);
        }
    }
}

static void pid_hash_insert(struct pa_classify_pid_hash **hash, pid_t pid,
                            const char *prop, enum pa_classify_method method,
                            const char *arg, const char *group)
{
    struct pa_classify_pid_hash *st;
    struct pa_classify_pid_hash *prev;

    pa_assert(hash);
    pa_assert(group);

    if ((st = pid_hash_find(hash, pid, prop,method,arg, &prev))) {
        pa_xfree(st->group);
        st->group = pa_xstrdup(group);

        pa_log_debug("pid hash group changed (%u|%s|%s|%s|%s)", st->pid,
                     st->prop ? st->prop : "", method_str(st->method.type),
                     st->arg.def ? st->arg.def : "", st->group);
    }
    else {
        st  = pa_xnew0(struct pa_classify_pid_hash, 1);

        st->next  = prev->next;
        st->pid   = pid;
        st->prop  = prop ? pa_xstrdup(prop) : NULL;
        st->group = pa_xstrdup(group);

        if (!prop)
            st->arg.def = pa_xstrdup("");
        else {
            st->method.type = method;

            switch (method) {

            case pa_method_equals:
                st->method.func = pa_classify_method_equals;
                st->arg.value.string = st->arg.def = pa_xstrdup(arg ? arg:"");
                break;

            case pa_method_startswith:
                st->method.func = pa_classify_method_startswith;
                st->arg.value.string = st->arg.def = pa_xstrdup(arg ? arg:"");
                break;

            case pa_method_matches:
                st->method.func = pa_classify_method_matches;
                st->arg.def = pa_xstrdup(arg ? arg:"");
                if (!arg || regcomp(&st->arg.value.rexp, arg, 0) != 0) {
                    st->method.type = pa_method_true;
                    st->method.func = pa_classify_method_true;
                }
                break;

            default:
            case pa_method_true:
                st->method.func = pa_classify_method_true;
                break;
            }
        }

        prev->next = st;

        pa_log_debug("pid hash added (%u|%s|%s|%s|%s)", st->pid,
                     st->prop ? st->prop : "", method_str(st->method.type),
                     st->arg.def ? st->arg.def : "", st->group);
    }
}

static void pid_hash_remove(struct pa_classify_pid_hash **hash,
                            pid_t pid, const char *prop,
                            enum pa_classify_method method, const char *arg)
{
    struct pa_classify_pid_hash *st;
    struct pa_classify_pid_hash *prev;

    pa_assert(hash);

    if ((st = pid_hash_find(hash, pid, prop,method,arg, &prev)) != NULL) {
        prev->next = st->next;

        pa_xfree(st->prop);
        pa_xfree(st->group);
        pa_xfree(st->arg.def);

        if (st->method.type == pa_method_matches)
            regfree(&st->arg.value.rexp);
        
        pa_xfree(st);
    }
}

static char *pid_hash_get_group(struct pa_classify_pid_hash **hash,
                                pid_t pid, pa_proplist *proplist)
{
    struct pa_classify_pid_hash *st;
    int idx;
    char *propval;
    char *group = NULL;

    pa_assert(hash);
 
    if (pid) {
        idx = pid & PA_POLICY_PID_HASH_MASK;

        for (st = hash[idx];  st != NULL;  st = st->next) {
            if (pid == st->pid) {
                if (!st->prop) {
                    group = st->group;
                    break;
                }

                if ((propval = (char *)pa_proplist_gets(proplist, st->prop)) &&
                    st->method.func(propval, &st->arg.value))
                {
                    group = st->group;
                    break;
                }
            }
        }
    }

    return group;
}

static struct
pa_classify_pid_hash *pid_hash_find(struct pa_classify_pid_hash **hash,
                                    pid_t pid, const char *prop,
                                    enum pa_classify_method method,
                                    const char *arg,
                                    struct pa_classify_pid_hash **prev_ret)
{
    struct pa_classify_pid_hash *st;
    struct pa_classify_pid_hash *prev;
    int                          idx;

    idx = pid & PA_POLICY_PID_HASH_MASK;

    for (prev = (struct pa_classify_pid_hash *)&hash[idx];
         (st = prev->next) != NULL;
         prev = prev->next)
    {
        if (pid && pid == st->pid) {
            if (!prop && !st->prop)
                break;

            if (st->prop && method == st->method.type) {
                if (method == pa_method_true)
                    break;

                if (arg && st->arg.def && !strcmp(arg, st->arg.def))
                    break;
            }
        }
    }

    if (prev_ret)
        *prev_ret = prev;

#if 0
    pa_log_debug("%s(%d,'%s') => %p", __FUNCTION__,
                 pid, stnam?stnam:"<null>", st);
#endif

    return st;
}

static void streams_free(struct pa_classify_stream_def *defs)
{
    struct pa_classify_stream_def *stream;
    struct pa_classify_stream_def *next;

    for (stream = defs;  stream;  stream = next) {
        next = stream->next;

        if (stream->method == pa_classify_method_matches)
            regfree(&stream->arg.rexp);
        else
            pa_xfree((void *)stream->arg.string);

        pa_xfree(stream->prop);
        pa_xfree(stream->exe);
        pa_xfree(stream->clnam);
        pa_xfree(stream->sname);
        pa_xfree(stream->group);

        pa_xfree(stream);
    }
}

static void streams_add(struct pa_classify_stream_def **defs, const char *prop,
                        enum pa_classify_method method, const char *arg, const char *clnam,
                        const char *sname, uid_t uid, const char *exe, const char *group, uint32_t flags)
{
    struct pa_classify_stream_def *d;
    struct pa_classify_stream_def *prev;
    pa_proplist *proplist = NULL;
    char         method_def[256];

    pa_assert(defs);
    pa_assert(group);

    proplist = pa_proplist_new();

    if (prop && arg && (method == pa_method_equals)) {
        pa_proplist_sets(proplist, prop, arg);
    }

    if ((d = streams_find(defs, proplist, clnam, sname, uid, exe, &prev)) != NULL) {
        pa_log_info("redefinition of stream");
        pa_xfree(d->group);
    }
    else {
        d = pa_xnew0(struct pa_classify_stream_def, 1);

        snprintf(method_def, sizeof(method_def), "<no-property-check>");

        if (prop && arg && method > pa_method_min && method < pa_method_max) {
            d->prop = pa_xstrdup(prop);

            switch (method) {

            case pa_method_equals:
                snprintf(method_def, sizeof(method_def),
                         "%s equals:%s", prop, arg);
                d->method = pa_classify_method_equals;
                d->arg.string = pa_xstrdup(arg);
                break;

            case pa_method_startswith:
                snprintf(method_def, sizeof(method_def),
                         "%s startswith:%s",prop, arg);
                d->method = pa_classify_method_startswith;
                d->arg.string = pa_xstrdup(arg);
                break;

            case pa_method_matches:
                snprintf(method_def, sizeof(method_def),
                         "%s matches:%s",prop, arg);
                d->method = pa_classify_method_matches;
                if (regcomp(&d->arg.rexp, arg, 0) != 0) {
                    pa_log("%s: invalid regexp definition '%s'",
                           __FUNCTION__, arg);
                    pa_assert_se(0);
                }
                break;


            case pa_method_true:
                snprintf(method_def, sizeof(method_def), "%s true", prop);
                d->method = pa_classify_method_true;
                memset(&d->arg, 0, sizeof(d->arg));
                break;

            default:
                /* never supposed to get here. just keep the compiler happy */
                pa_assert_se(0);
                break;
            }
        }

        d->uid   = uid;
        d->exe   = exe   ? pa_xstrdup(exe)   : NULL;
        d->clnam = clnam ? pa_xstrdup(clnam) : NULL;
        d->sname = sname ? pa_xstrdup(sname) : NULL;
        d->sact  = sname ? 0 : -1;
        
        prev->next = d;

        pa_log_debug("stream added (%d|%s|%s|%s|%d)", uid, exe?exe:"<null>",
                     clnam?clnam:"<null>", method_def, d->sact);
    }

    d->group = pa_xstrdup(group);
    d->flags = flags;

    pa_proplist_free(proplist);
}

static const char *streams_get_group(struct pa_classify_stream_def **defs,
                                     pa_proplist *proplist,
                                     const char *clnam, uid_t uid, const char *exe,
                                     uint32_t *flags_ret)
{
    struct pa_classify_stream_def *d;
    const char *group;
    uint32_t flags;

    pa_assert(defs);

    if ((d = streams_find(defs, proplist, clnam, NULL, uid, exe, NULL)) == NULL) {
        group = NULL;
        flags = 0;
    }
    else {
        group = d->group;
        flags = d->flags;
    }

    if (flags_ret != NULL)
        *flags_ret = flags;

    return group;
}

static struct pa_classify_stream_def *
streams_find(struct pa_classify_stream_def **defs, pa_proplist *proplist,
             const char *clnam, const char *sname, uid_t uid, const char *exe,
             struct pa_classify_stream_def **prev_ret)
{
#define PROPERTY_MATCH     (!d->prop || !d->method || \
                           (d->method && d->method(prv, &d->arg)))
#define STRING_MATCH_OF(m) (!d->m || (m && d->m && !strcmp(m, d->m)))
#define ID_MATCH_OF(m)     (d->m == -1 || m == d->m)

    struct pa_classify_stream_def *prev;
    struct pa_classify_stream_def *d;
    char *prv;

    for (prev = (struct pa_classify_stream_def *)defs;
         (d = prev->next) != NULL;
         prev = prev->next)
    {
        if (!proplist || !d->prop ||
            !(prv = (char *)pa_proplist_gets(proplist, d->prop)) || !prv[0])
        {
            prv = (char *)"<unknown>";
        }

#if 0
        if (d->method == pa_classify_method_matches) {
            pa_log_debug("%s: prv='%s' prop='%s' arg=<regexp>",
                         __FUNCTION__, prv, d->prop?d->prop:"<null>");
        }
        else {
            pa_log_debug("%s: prv='%s' prop='%s' arg='%s'",
                         __FUNCTION__, prv, d->prop?d->prop:"<null>",
                         d->arg.string?d->arg.string:"<null>");
        }
#endif

        if (PROPERTY_MATCH         &&
            STRING_MATCH_OF(clnam) &&
            ID_MATCH_OF(uid)       &&
            /* case for dynamically changing active sink. */
            (!sname || (sname && d->sname && !strcmp(sname, d->sname))) &&
            (d->sact == -1 || d->sact == 1) &&
            /* end special case */
            STRING_MATCH_OF(exe)      )
            break;

    }

    if (prev_ret)
        *prev_ret = prev;

#if 0
    {
        char *s = pa_proplist_to_string_sep(proplist, " ");
        pa_log_debug("%s(<%s>,'%s',%d,'%s') => %p", __FUNCTION__,
                     s, clnam?clnam:"<null>", uid, exe?exe:"<null>", d);
        pa_xfree(s);
    }
#endif

    return d;

#undef STRING_MATCH_OF
#undef ID_MATCH_OF
}

void pa_classify_port_entry_free(struct pa_classify_port_entry *port) {
    pa_assert(port);

    pa_xfree(port->device_name);
    pa_xfree(port->port_name);
    pa_xfree(port);
}

static void devices_free(struct pa_classify_device *devices)
{
    struct pa_classify_device_def *d;

    if (devices) {
        for (d = devices->defs;  d->type;  d++) {
            pa_xfree((void *)d->type);

            if (d->data.ports)
                pa_hashmap_free(d->data.ports);

            if (d->method == pa_classify_method_matches)
                regfree(&d->arg.rexp);
            else
                pa_xfree((void *)d->arg.string);

            pa_xfree(d->data.module);
            pa_xfree(d->data.module_args);
        }

        pa_xfree(devices);
    }
}

static void devices_add(struct userdata *u, struct pa_classify_device **p_devices, const char *type,
                        const char *prop, enum pa_classify_method method, const char *arg,
                        pa_hashmap *ports, const char *module, const char *module_args,
                        uint32_t flags)
{
    struct pa_classify_device *devs;
    struct pa_classify_device_def *d;
    size_t newsize;
    const char *method_name;
    char *ports_string = NULL; /* Just for log output. */
    pa_strbuf *buf; /* For building ports_string. */

    pa_assert(p_devices);
    pa_assert_se((devs = *p_devices));

    newsize = sizeof(*devs) + sizeof(devs->defs[0]) * (devs->ndef + 1);

    devs = *p_devices = pa_xrealloc(devs, newsize);

    d = devs->defs + devs->ndef;

    memset(d+1, 0, sizeof(devs->defs[0]));

    d->type  = pa_xstrdup(type);
    d->prop  = pa_xstrdup(prop);

    buf = pa_strbuf_new();

    if (ports && !pa_hashmap_isempty(ports)) {
        struct pa_classify_port_entry *port;
        void *state;
        bool first = true;

        /* Copy the ports hashmap to d->data.ports. */

        d->data.ports = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                            pa_idxset_string_compare_func,
                                            NULL,
                                            (pa_free_cb_t) pa_classify_port_entry_free);
        PA_HASHMAP_FOREACH(port, ports, state) {
            struct pa_classify_port_entry *port_copy =
                pa_xnew(struct pa_classify_port_entry, 1);

            port_copy->device_name = pa_xstrdup(port->device_name);
            port_copy->port_name = pa_xstrdup(port->port_name);

            pa_hashmap_put(d->data.ports, port_copy->device_name, port_copy);

            if (!first) {
                pa_strbuf_putc(buf, ',');
            }
            first = false;

            pa_strbuf_printf(buf, "%s:%s", port->device_name, port->port_name);
        }
    }

    d->data.module = module ? pa_xstrdup(module) : NULL;
    d->data.module_args = module_args ? pa_xstrdup(module_args) : NULL;

    if (d->data.module && !u->classify->module_unlink_hook_slot)
        u->classify->module_unlink_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_MODULE_UNLINK],
                                                               PA_HOOK_NORMAL,
                                                               (pa_hook_cb_t) module_unlink_hook_cb,
                                                               u->classify);

    d->data.flags = flags;

    switch (method) {

    case pa_method_equals:
        method_name = "equals";
        d->method = pa_classify_method_equals;
        d->arg.string = pa_xstrdup(arg);
        break;

    case pa_method_startswith:
        method_name = "startswidth";
        d->method = pa_classify_method_startswith;
        d->arg.string = pa_xstrdup(arg);
        break;

    case pa_method_matches:
        method_name = "matches";
        if (regcomp(&d->arg.rexp, arg, 0) == 0) {
            d->method = pa_classify_method_matches;
            break;
        }
        /* intentional fall trough */

    default:
        pa_log("%s: invalid device definition %s", __FUNCTION__, type);
        memset(d, 0, sizeof(*d));
        pa_strbuf_free(buf);
        return;
    }

    devs->ndef++;

#if (PULSEAUDIO_VERSION >= 8)
    ports_string = pa_strbuf_to_string_free(buf);
#else
    ports_string = pa_strbuf_tostring_free(buf);
#endif

    pa_log_info("device '%s' added (%s|%s|%s|%s|0x%04x)",
                type, d->prop, method_name, arg, ports_string, d->data.flags);

    pa_xfree(ports_string);
}

static int devices_classify(struct pa_classify_device *devices,
                            pa_proplist *proplist, const char *name,
                            uint32_t flag_mask, uint32_t flag_value,
                            struct pa_classify_result **result)
{
    struct pa_classify_device_def *d;
    const char *propval;

    pa_assert(result);

    *result = classify_result_malloc(devices->ndef);

    for (d = devices->defs;  d->type;  d++) {
        propval = get_property(d->prop, proplist, name);

        if (d->method(propval, &d->arg)) {
            if ((d->data.flags & flag_mask) == flag_value) {
                pa_assert((*result)->count < devices->ndef);
                classify_result_append(result, d->type);
            }
        }
    }

    return (*result)->count;
}

static int devices_is_typeof(struct pa_classify_device_def *defs,
                             pa_proplist *proplist, const char *name,
                             const char *type,
                             struct pa_classify_device_data **data)
{
    struct pa_classify_device_def *d;
    const char *propval;

    for (d = defs;  d->type;  d++) {
        if (!strcmp(type, d->type)) {
            propval = get_property(d->prop, proplist, name);

            if (d->method(propval, &d->arg)) {
                if (data != NULL)
                    *data = &d->data;

                return true;
            }
        }
    }

    return false;
}

static void cards_free(struct pa_classify_card *cards)
{
    struct pa_classify_card_def *d;
    int i;

    if (cards) {
        for (d = cards->defs;  d->type;  d++) {

            pa_xfree((void *)d->type);

            for (i = 0; i < 2; i++) {
                pa_xfree((void *)d->data[i].profile);

                if (d->data[i].method == pa_classify_method_matches)
                    regfree(&d->data[i].arg.rexp);
                else
                    pa_xfree((void *)d->data[i].arg.string);
            }
        }

        pa_xfree(cards);
    }
}

static void cards_add(struct pa_classify_card **p_cards, char *type,
                      enum pa_classify_method method[2], char **arg,
                      char **profiles, uint32_t flags[2])
{
    struct pa_classify_card *cards;
    struct pa_classify_card_def *d;
    struct pa_classify_card_data *data;
    size_t newsize;
    char *method_name[2];
    int i;

    pa_assert(p_cards);
    pa_assert_se((cards = *p_cards));

    newsize = sizeof(*cards) + sizeof(cards->defs[0]) * (cards->ndef + 1);

    cards = *p_cards = pa_xrealloc(cards, newsize);

    d = cards->defs + cards->ndef;

    memset(d+1, 0, sizeof(cards->defs[0]));

    d->type    = pa_xstrdup(type);

    for (i = 0; i < 2 && profiles[i]; i++) {

        data = &d->data[i];

        data->profile = profiles[i] ? pa_xstrdup(profiles[i]) : NULL;
        data->flags   = flags[i];

        switch (method[i]) {

        case pa_method_equals:
            method_name[i] = "equals";
            data->method = pa_classify_method_equals;
            data->arg.string = pa_xstrdup(arg[i]);
            break;

        case pa_method_startswith:
            method_name[i] = "startswidth";
            data->method = pa_classify_method_startswith;
            data->arg.string = pa_xstrdup(arg[i]);
            break;

        case pa_method_matches:
            method_name[i] = "matches";
            if (regcomp(&data->arg.rexp, arg[i], 0) == 0) {
                data->method = pa_classify_method_matches;
                break;
            }
            /* intentional fall trough */

        default:
            pa_log("%s: invalid card definition %s", __FUNCTION__, type);
            memset(d, 0, sizeof(*d));
            return;
        }
    }

    cards->ndef++;

    pa_log_info("card '%s' added (%s|%s|%s|0x%04x)", type, method_name[0], arg[0],
                d->data[0].profile ? d->data[0].profile : "", d->data[0].flags);
    if (d->data[1].profile)
        pa_log_info("  :: added (%s|%s|%s|0x%04x)", method_name[1], arg[1],
                    d->data[1].profile ? d->data[1].profile : "", d->data[1].flags);
}

static int cards_classify(struct pa_classify_card *cards,
                          const char *name, pa_hashmap *card_profiles,
                          uint32_t flag_mask, uint32_t flag_value,
                          bool reclassify, struct pa_classify_result **result)
{
    struct pa_classify_card_def  *d;
    struct pa_classify_card_data *data;
    pa_card_profile *cp;
    int              i;
    bool             supports_profile;

    pa_assert(result);

    *result = classify_result_malloc(cards->ndef);

    for (d = cards->defs;  d->type;  d++) {

        /* Check for both data[0] and data[1] */

        for (i = 0; i < 2 && d->data[i].profile; i++) {

            data = &d->data[i];

            if (data->method(name, &data->arg)) {
                supports_profile = false;

                if (data->profile == NULL)
                    supports_profile = true;
                else {
                    if ((cp = pa_hashmap_get(card_profiles, data->profile))) {
                        if (!reclassify || cp->available != PA_AVAILABLE_NO)
                            supports_profile = true;
                    }
                }

                if (supports_profile && (data->flags & flag_mask) == flag_value) {
                    pa_assert((*result)->count < cards->ndef);
                    classify_result_append(result, d->type);
                }
            }
        }

    }

    return (*result)->count;
}

static int card_is_typeof(struct pa_classify_card_def *defs, const char *name,
                          const char *type, struct pa_classify_card_data **data, int *priority)
{
    struct pa_classify_card_def *d;
    int i;

    for (d = defs;  d->type;  d++) {
        if (!strcmp(type, d->type)) {

            for (i = 0; i < 2 && d->data[i].profile; i++) {
                if (d->data[i].method(name, &d->data[i].arg)) {
                    if (data != NULL)
                        *data = &d->data[i];
                    if (priority != NULL)
                        *priority = i;

                    return true;
                }
            }
        }
    }

    return false;
}

static int port_device_is_typeof(struct pa_classify_device_def *defs,
                                 const char *name, const char *type,
                                 struct pa_classify_device_data **data)
{
    struct pa_classify_device_def *d;

    for (d = defs;  d->type;  d++) {
        if (pa_streq(type, d->type)) {
            if (d->data.ports && pa_hashmap_get(d->data.ports, name)) {
                if (data != NULL)
                    *data = &d->data;

                return true;
            }
        }
    }

    return false;
}

const char *get_property(const char *propname, pa_proplist *proplist, const char *name)
{
    const char *propval = NULL;

    if (propname != NULL && proplist != NULL && name != NULL) {
        if (!strcmp(propname, "name"))
            propval = name;
        else
            propval = pa_proplist_gets(proplist, propname);
    }

    if (propval == NULL || propval[0] == '\0')
        propval = "<unknown>";

    return propval;
}

const char *pa_classify_method_str(enum pa_classify_method method)
{
    switch (method) {
        case pa_method_equals:      return "equals";
        case pa_method_startswith:  return "startswidth";
        case pa_method_matches:     return "matches";
        case pa_method_true:        return "true";
        default:                    return "<unknown>";
    };
}

int pa_classify_method_equals(const char *string,
                              union pa_classify_arg *arg)
{
    int found;

    if (!string || !arg || !arg->string)
        found = false;
    else
        found = !strcmp(string, arg->string);

    return found;
}

int pa_classify_method_startswith(const char *string,
                                  union pa_classify_arg *arg)
{
    int found;

    if (!string || !arg || !arg->string)
        found = false;
    else
        found = !strncmp(string, arg->string, strlen(arg->string));

    return found;
}

int pa_classify_method_matches(const char *string,
                               union pa_classify_arg *arg)
{
#define MAX_MATCH 5

    regmatch_t m[MAX_MATCH];
    regoff_t   end;
    int        found;
    
    found = false;

    if (string && arg) {
        if (regexec(&arg->rexp, string, MAX_MATCH, m, 0) == 0) {
            end = strlen(string);

            if (m[0].rm_so == 0 && m[0].rm_eo == end && m[1].rm_so == -1)
                found = true;
        }  
    }


    return found;

#undef MAX_MATCH
}

int pa_classify_method_true(const char *string,
                            union pa_classify_arg *arg)
{
    (void)string;
    (void)arg;

    return true;
}

static const char *method_str(enum pa_classify_method method)
{
    switch (method) {
    default:
    case pa_method_unknown:      return "unknown";
    case pa_method_equals:       return "equals";
    case pa_method_startswith:   return "startswith";
    case pa_method_matches:      return "matches";
    case pa_method_true:         return "true";
    }
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
