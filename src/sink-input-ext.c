#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/def.h>
#include <pulse/proplist.h>
#include <pulse/volume.h>
#include <pulsecore/sink.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/core-util.h>

#include "userdata.h"
#include "index-hash.h"
#include "policy-group.h"
#include "sink-input-ext.h"
#include "sink-ext.h"
#include "classify.h"
#include "context.h"

#define VOLUME_LIMIT_FACTOR_KEY "x-policy.volume.factor"

/* hooks */
static pa_hook_result_t sink_input_neew(void *, void *, void *);
static pa_hook_result_t sink_input_fixate(void *, void *, void *);
static pa_hook_result_t sink_input_put(void *, void *, void *);
static pa_hook_result_t sink_input_unlink(void *, void *, void *);
static pa_hook_result_t sink_input_state_changed(pa_core *c, pa_sink_input *si, struct userdata *u);
#if (PULSEAUDIO_VERSION >= 6)
static pa_hook_result_t sink_input_mute_changed(pa_core *c, pa_sink_input *si, struct userdata *u);
#endif

static struct pa_policy_group* get_group(struct userdata *, const char *, pa_proplist *sinp_proplist, uint32_t *);
static struct pa_policy_group* get_group_or_classify(struct userdata *, struct pa_sink_input *, uint32_t *);
static void handle_new_sink_input(struct userdata *u, struct pa_sink_input *si,
                                  uint32_t *preserve_cork_state, uint32_t *preserve_mute_state);
static void handle_sink_input_fixate(struct userdata *u, pa_sink_input_new_data *sinp_data);
static void handle_removed_sink_input(struct userdata *,
                                      struct pa_sink_input *);
static uint32_t update_state_flag(uint32_t flags, enum pa_sink_input_ext_state flag, bool set);

struct pa_sinp_evsubscr *pa_sink_input_ext_subscription(struct userdata *u)
{
    pa_core                 *core;
    pa_hook                 *hooks;
    struct pa_sinp_evsubscr *subscr;
    pa_hook_slot            *neew;
    pa_hook_slot            *fixate;
    pa_hook_slot            *put;
    pa_hook_slot            *unlink;

    pa_assert(u);
    pa_assert_se((core = u->core));

    hooks  = core->hooks;
    
    /* PA_HOOK_EARLY - 2, i.e. before module-match */
    neew   = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_INPUT_NEW,
                             PA_HOOK_EARLY - 2, sink_input_neew, (void *)u);
    fixate = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_INPUT_FIXATE,
                             PA_HOOK_LATE, sink_input_fixate, (void *)u);
    put    = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_INPUT_PUT,
                             PA_HOOK_LATE, sink_input_put, (void *)u);
    unlink = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_INPUT_UNLINK,
                             PA_HOOK_LATE, sink_input_unlink, (void *)u);


    subscr = pa_xnew0(struct pa_sinp_evsubscr, 1);
    
    subscr->neew   = neew;
    subscr->fixate = fixate;
    subscr->put    = put;
    subscr->unlink = unlink;
    /* cork and mute state hooks are dynamically set when corking or muting
     * is done for the first time. This way if corking or muting is never
     * used, we don't need to set up the state hooks. */
    subscr->cork_state = NULL;
    subscr->mute_state = NULL;

    return subscr;
}

void  pa_sink_input_ext_subscription_free(struct pa_sinp_evsubscr *subscr)
{
    if (subscr != NULL) {
        pa_hook_slot_free(subscr->neew);
        pa_hook_slot_free(subscr->fixate);
        pa_hook_slot_free(subscr->put);
        pa_hook_slot_free(subscr->unlink);
        if (subscr->cork_state)
            pa_hook_slot_free(subscr->cork_state);
        if (subscr->mute_state)
            pa_hook_slot_free(subscr->mute_state);

        pa_xfree(subscr);
    }
}

void pa_sink_input_ext_discover(struct userdata *u)
{
    void                 *state = NULL;
    pa_idxset            *idxset;
    struct pa_sink_input *sinp;

    pa_assert(u);
    pa_assert(u->core);
    pa_assert_se((idxset = u->core->sink_inputs));

    while ((sinp = pa_idxset_iterate(idxset, &state, NULL)) != NULL)
        handle_new_sink_input(u, sinp, NULL, NULL);
}

void  pa_sink_input_ext_rediscover(struct userdata *u)
{
    void                 *state = NULL;
    pa_idxset            *idxset;
    struct pa_sink_input *sinp;
    struct pa_sink_input_ext *ext;
    uint32_t              old_corked_state;
    uint32_t              old_muted_state;
    const char           *group_name;
    const char           *clear[3] = { PA_PROP_POLICY_GROUP, PA_PROP_POLICY_STREAM_FLAGS, NULL };

    pa_assert(u);
    pa_assert(u->core);
    pa_assert_se((idxset = u->core->sink_inputs));

    while ((sinp = pa_idxset_iterate(idxset, &state, NULL)) != NULL) {
        group_name = pa_proplist_gets(sinp->proplist, PA_PROP_POLICY_GROUP);
        if (!group_name)
            continue;
        if (!pa_streq(group_name, "othermedia"))
            continue;

        pa_log_debug("rediscover sink-input \"%s\"", pa_sink_input_ext_get_name(sinp));
        pa_assert_se((ext = pa_sink_input_ext_lookup(u, sinp)));
        old_corked_state = ext->local.cork_state;
        old_muted_state = ext->local.mute_state;
        /* First remove sink input and then re-classify. */
        handle_removed_sink_input(u, sinp);
        pa_proplist_unset_many(sinp->proplist, clear);
        handle_new_sink_input(u, sinp, &old_corked_state, &old_muted_state);
    }
}

struct pa_sink_input_ext *pa_sink_input_ext_lookup(struct userdata      *u,
                                                   struct pa_sink_input *sinp)
{
    struct pa_sink_input_ext *ext;

    pa_assert(u);
    pa_assert(sinp);

    ext = pa_index_hash_lookup(u->hsi, sinp->index);

    return ext;
}


int pa_sink_input_ext_set_policy_group(struct pa_sink_input *sinp,
                                       const char *group)
{
    int ret;

    assert(sinp);

    if (group) 
        ret = pa_proplist_sets(sinp->proplist, PA_PROP_POLICY_GROUP, group);
    else
        ret = pa_proplist_unset(sinp->proplist, PA_PROP_POLICY_GROUP);

    return ret;
}

const char *pa_sink_input_ext_get_policy_group(struct pa_sink_input *sinp)
{
    const char *group;

    pa_assert(sinp);

    group = pa_proplist_gets(sinp->proplist, PA_PROP_POLICY_GROUP);

    if (group == NULL)
        group = PA_POLICY_DEFAULT_GROUP_NAME;

    return group;
}

const char *sink_input_ext_get_name(pa_proplist *sinp_proplist)
{
    const char *name;

    pa_assert(sinp_proplist);

    name = pa_proplist_gets(sinp_proplist, PA_PROP_MEDIA_NAME);

    if (name == NULL)
        name = "<unknown>";

    return name;
}

const char *pa_sink_input_ext_get_name(struct pa_sink_input *sinp)
{
    pa_assert(sinp);

    return sink_input_ext_get_name(sinp->proplist);
}

static void sink_input_ext_unset_volume_limit(struct pa_sink_input_ext *ext, struct pa_sink_input *si)
{
    pa_assert(ext);
    pa_assert(si);

    if (ext->local.volume_limit_enabled) {
        ext->local.volume_limit_enabled = false;
        pa_sink_input_remove_volume_factor(si, VOLUME_LIMIT_FACTOR_KEY);
    }
}

int pa_sink_input_ext_set_volume_limit(struct userdata *u,
                                       struct pa_sink_input *sinp,
                                       pa_volume_t limit)
{
    struct pa_sink_input_ext   *ext;
    pa_cvolume                  volume;
    int                         retval;

    pa_assert(u);
    pa_assert(sinp);

    retval = 0;

    if (limit == 0)
        pa_sink_input_ext_mute(u, sinp, true);
    else {
        pa_sink_input_ext_mute(u, sinp, false);

        if (limit > PA_VOLUME_NORM)
            limit = PA_VOLUME_NORM;

        if (!(ext = pa_sink_input_ext_lookup(u, sinp)))
            retval = -1;
        else {
            sink_input_ext_unset_volume_limit(ext, sinp);
            if (limit < PA_VOLUME_NORM) {
                ext->local.volume_limit_enabled = true;
                pa_cvolume_set(&volume, sinp->sample_spec.channels, limit);
                pa_sink_input_add_volume_factor(sinp, VOLUME_LIMIT_FACTOR_KEY, &volume);
            }
        }
    }

    return retval;
}

void  pa_sink_input_ext_unset_volume_limit(struct userdata *u, struct pa_sink_input *si)
{
    struct pa_sink_input_ext *ext;

    if (!(ext = pa_sink_input_ext_lookup(u, si)))
        return;

    sink_input_ext_unset_volume_limit(ext, si);
}

static pa_hook_result_t sink_input_neew(void *hook_data, void *call_data,
                                       void *slot_data)
{
    static uint32_t         route_flags = PA_POLICY_GROUP_FLAG_SET_SINK |
                                          PA_POLICY_GROUP_FLAG_ROUTE_AUDIO;
    static pa_volume_t      max_volume  = PA_VOLUME_NORM;


    struct pa_sink_input_new_data
                           *data = (struct pa_sink_input_new_data *)call_data;
    struct userdata        *u    = (struct userdata *)slot_data;
    uint32_t                flags;
    const char             *group_name;
    const char             *sinp_name;
    const char             *sink_name;
    int                     local_route;
    int                     local_volume;
    struct pa_policy_group *group;

    pa_assert(u);
    pa_assert(data);

    if ((group_name = pa_classify_sink_input_by_data(u,data,&flags)) != NULL &&
        (group      = pa_policy_group_find(u, group_name)          ) != NULL ){

        /* Let's just set the policy group property here already so that we
         * don't have to classify again when the sink input is put, because we
         * can just retrieve the group from the proplist. Also, this prevents
         * the classification from breaking later because of the proplist
         * overwriting done below. */
        pa_proplist_sets(data->proplist, PA_PROP_POLICY_GROUP, group_name);

        /* Proplist overwriting can also mess up the retrieval of
         * stream-specific flags later on, so we need to store those to the
         * proplist as well (ugly hack). We could probably cope without this
         * one though, since the stream-specific flags don't really seem to be
         * used. */
        pa_proplist_set(data->proplist, PA_PROP_POLICY_STREAM_FLAGS,
                        (void*)&flags, sizeof(flags));

        if (group->properties != NULL) {
            pa_proplist_update(data->proplist, PA_UPDATE_REPLACE, group->properties);
            pa_log_debug("new sink input inserted into %s. "
                         "force the following properties:", group_name);
        }

        if (group->sink != NULL) {
            sinp_name = pa_proplist_gets(data->proplist, PA_PROP_MEDIA_NAME);

            if (!sinp_name)
                sinp_name = "<unknown>";

            local_route  = flags & PA_POLICY_LOCAL_ROUTE;
            local_volume = flags & PA_POLICY_LOCAL_VOLMAX;

            if (group->mutebyrt && !local_route) {
                sink_name = u->nullsink->name;

                pa_log_debug("force stream '%s'/'%s' to sink '%s' due to "
                             "mute-by-route", group_name,sinp_name, sink_name);

#if PULSEAUDIO_VERSION >= 12
                pa_sink_input_new_data_set_sink(data, u->nullsink->sink, false, false);
#else
                pa_sink_input_new_data_set_sink(data, u->nullsink->sink, false);
#endif
            }
            else if (group->flags & route_flags) {
                sink_name = pa_sink_ext_get_name(group->sink);

                pa_log_debug("force stream '%s'/'%s' to sink '%s'",
                             group_name, sinp_name, sink_name); 

#if PULSEAUDIO_VERSION >= 12
                pa_sink_input_new_data_set_sink(data, group->sink, false, false);
#else
                pa_sink_input_new_data_set_sink(data, group->sink, false);
#endif
            }

            if (local_volume) {
                pa_log_debug("force stream '%s'/'%s' volume to %d",
                             group_name, sinp_name,
                             (max_volume * 100) / PA_VOLUME_NORM);
                
                pa_cvolume_set(&data->volume, data->channel_map.channels,
                               max_volume);

                data->volume_is_set      = true;
                data->save_volume        = false;
            }
        }

    }


    return PA_HOOK_OK;
}


static pa_hook_result_t sink_input_put(void *hook_data, void *call_data,
                                       void *slot_data)
{
    struct pa_sink_input *sinp = (struct pa_sink_input *)call_data;
    struct userdata      *u    = (struct userdata *)slot_data;

    handle_new_sink_input(u, sinp, NULL, NULL);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_fixate(void *hook_data, void *call_data,
                                          void *slot_data)
{
    pa_sink_input_new_data  *sinp_data = (pa_sink_input_new_data *) call_data;
    struct userdata         *u         = (struct userdata *) slot_data;

    pa_assert(sinp_data);
    pa_assert(u);

    handle_sink_input_fixate(u, sinp_data);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_unlink(void *hook_data, void *call_data,
                                          void *slot_data)
{
    struct pa_sink_input *sinp = (struct pa_sink_input *)call_data;
    struct userdata      *u    = (struct userdata *)slot_data;

    handle_removed_sink_input(u, sinp);

    return PA_HOOK_OK;
}

static struct pa_policy_group* get_group(struct userdata *u, const char *group_name, pa_proplist *sinp_proplist, uint32_t *flags_ret)
{
    struct pa_policy_group *group = NULL;
    const void *flags;
    size_t len_flags = 0;

    pa_assert(u);
    pa_assert(sinp_proplist);

    /* If group name is provided use that, otherwise check sink input proplist. */
    if (!group_name)
        /* Just grab the group name from the proplist to avoid classifying multiple
         * times (and to avoid classifying incorrectly if properties are
         * overwritten when handling PA_CORE_HOOK_SINK_INPUT_NEW).*/
        group_name = pa_proplist_gets(sinp_proplist, PA_PROP_POLICY_GROUP);

    if (group_name && (group = pa_policy_group_find(u, group_name)) != NULL) {
        /* Only update flags if flags_ret is non null */
        if (flags_ret) {
            if (pa_proplist_get(sinp_proplist, PA_PROP_POLICY_STREAM_FLAGS, &flags, &len_flags) < 0 ||
                len_flags != sizeof(uint32_t)) {

                pa_log_warn("No stream flags in proplist or malformed flags.");
                *flags_ret = 0;
            } else
                *flags_ret = *(uint32_t *)flags;
        }
    }

    return group;
}

static struct pa_policy_group* get_group_or_classify(struct userdata *u, struct pa_sink_input *sinp, uint32_t *flags_ret) {
    struct pa_policy_group *group;
    const char *group_name;

    pa_assert(u);
    pa_assert(sinp);
    pa_assert(flags_ret);

    group = get_group(u, NULL, sinp->proplist, flags_ret);

    if (!group) {
        pa_log_info("Sink input '%s' is missing a policy group. "
                    "Classifying...", sink_input_ext_get_name(sinp->proplist));

        /* After classifying, search group struct using get_group, but don't try to update flags,
         * since those are not added to the sink input yet. That will happen in
         * handle_new_sink_input->pa_policy_group_insert_sink_input. pa_classify_sink_input already
         * returns our flags, so we can just have those in the flags_ret. */
        if ((group_name = pa_classify_sink_input(u, sinp, flags_ret)))
            group = get_group(u, group_name, sinp->proplist, NULL);
    }

    return group;
}

static void handle_new_sink_input(struct userdata      *u,
                                  struct pa_sink_input *sinp,
                                  uint32_t *preserve_cork_state,
                                  uint32_t *preserve_mute_state)
{
    struct      pa_policy_group *group = NULL;
    struct      pa_sink_input_ext *ext;
    uint32_t    idx;
    const char *sinp_name;
    uint32_t    flags = 0;

    if (sinp && u) {
        ext = pa_xmalloc0(sizeof(struct pa_sink_input_ext));
        ext->local.route = (flags & PA_POLICY_LOCAL_ROUTE) ? true : false;
        ext->local.mute  = (flags & PA_POLICY_LOCAL_MUTE ) ? true : false;

        if (pa_hashmap_get(sinp->volume_factor_items, VOLUME_LIMIT_FACTOR_KEY))
            ext->local.volume_limit_enabled = true;

        idx  = sinp->index;
        sinp_name = sink_input_ext_get_name(sinp->proplist);
        pa_assert_se((group = get_group_or_classify(u, sinp, &flags)));

        if (preserve_cork_state)
            ext->local.cork_state = *preserve_cork_state;
        else
            ext->local.cork_state = update_state_flag(0,
                                                      PA_SINK_INPUT_EXT_STATE_USER,
                                                      PA_SINK_INPUT_CORKED == pa_sink_input_get_state(sinp));
#if (PULSEAUDIO_VERSION == 5)
        if (preserve_mute_state)
            pa_log_debug("ignoring mute state as PulseAudio version 5 doesn't have mute hook.");
#elif (PULSEAUDIO_VERSION >= 6)
        if (preserve_mute_state)
            ext->local.mute_state = *preserve_mute_state;
        else
            ext->local.mute_state = update_state_flag(0,
                                                      PA_SINK_INPUT_EXT_STATE_USER,
                                                      sinp->muted);
#endif
        pa_index_hash_add(u->hsi, idx, ext);

        pa_policy_context_register(u, pa_policy_object_sink_input, sinp_name, sinp);
        pa_policy_group_insert_sink_input(u, group->name, sinp, flags);

        /* Proplist overwriting can also mess up the retrieval of
         * stream-specific flags later on, so we need to store those to the
         * proplist as well (ugly hack). We could probably cope without this
         * one though, since the stream-specific flags don't really seem to be
         * used. */
        pa_proplist_set(sinp->proplist, PA_PROP_POLICY_STREAM_FLAGS,
                        (void*)&flags, sizeof(flags));

        pa_log_debug("new sink_input %s (idx=%u) (group=%s)", sinp_name, idx, group->name);
    }
}

bool pa_sink_input_ext_cork(struct userdata *u, pa_sink_input *si, bool cork)
{
    struct pa_sink_input_ext *ext;
    bool sink_input_corked;
    bool sink_input_corking_changed = false;

    pa_assert(si);
    pa_assert(u);
    pa_assert(u->core);
    pa_assert(u->ssi);

    pa_assert_se((ext = pa_sink_input_ext_lookup(u, si)));

    pa_assert(!ext->local.ignore_cork_state_change);

    sink_input_corked = pa_sink_input_get_state(si) == PA_SINK_INPUT_CORKED;

    pa_log_debug("sink input cork state before: user: %d policy: %d, request %scork",
                 ext->local.cork_state & PA_SINK_INPUT_EXT_STATE_USER ? 1 : 0,
                 ext->local.cork_state & PA_SINK_INPUT_EXT_STATE_POLICY ? 1 : 0,
                 cork ? "" : "un");

    if (!u->ssi->cork_state) {
        /* Check current sink input state and enable corking state following. */
        u->ssi->cork_state = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED],
                                             PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_state_changed, (void *) u);
        ext->local.cork_state = update_state_flag(ext->local.cork_state,
                                                  PA_SINK_INPUT_EXT_STATE_USER,
                                                  sink_input_corked);
    }

    ext->local.cork_state = update_state_flag(ext->local.cork_state,
                                              PA_SINK_INPUT_EXT_STATE_POLICY,
                                              cork);

    if (!sink_input_corked && ext->local.cork_state) {
        ext->local.ignore_cork_state_change = true;
        sink_input_corking_changed = true;
        pa_sink_input_cork(si, true);
    } else if (sink_input_corked && !ext->local.cork_state) {
        ext->local.ignore_cork_state_change = true;
        sink_input_corking_changed = true;
        pa_sink_input_cork(si, false);
    }

    pa_log_debug("sink input cork state  after: user: %d policy: %d, %s",
                 ext->local.cork_state & PA_SINK_INPUT_EXT_STATE_USER ? 1 : 0,
                 ext->local.cork_state & PA_SINK_INPUT_EXT_STATE_POLICY ? 1 : 0,
                 sink_input_corking_changed ? "updated corking" : "no change to corking");

    return sink_input_corking_changed;
}

static pa_hook_result_t sink_input_state_changed(pa_core *c, pa_sink_input *sinp, struct userdata *u)
{
    struct pa_sink_input_ext *ext;
    bool corked_by_client;

    pa_assert(c);
    pa_assert(sinp);
    pa_assert(u);

    pa_assert_se((ext = pa_sink_input_ext_lookup(u, sinp)));
    if (ext->local.ignore_cork_state_change) {
        ext->local.ignore_cork_state_change = false;
        return PA_HOOK_OK;
    }

    corked_by_client = PA_SINK_INPUT_CORKED == pa_sink_input_get_state(sinp);
    ext->local.cork_state = update_state_flag(ext->local.cork_state,
                                              PA_SINK_INPUT_EXT_STATE_USER,
                                              corked_by_client);

    pa_log_debug("sink input user corking %d", corked_by_client);

    return PA_HOOK_OK;
}

bool pa_sink_input_ext_mute(struct userdata *u, pa_sink_input *si, bool mute)
{
#if (PULSEAUDIO_VERSION == 5)
    pa_assert(u);
    pa_assert(si);

    pa_sink_input_set_mute(si, mute, true);

#elif (PULSEAUDIO_VERSION >= 6)

    struct pa_sink_input_ext *ext;
    bool sink_input_muting_changed = false;

    pa_assert(si);
    pa_assert(u);
    pa_assert(u->core);
    pa_assert(u->ssi);

    pa_assert_se((ext = pa_sink_input_ext_lookup(u, si)));

    pa_assert(!ext->local.ignore_mute_state_change);

    pa_log_debug("sink input mute state before: user: %d policy: %d, request %smute",
                 ext->local.mute_state & PA_SINK_INPUT_EXT_STATE_USER ? 1 : 0,
                 ext->local.mute_state & PA_SINK_INPUT_EXT_STATE_POLICY ? 1 : 0,
                 mute ? "" : "un");

    if (!u->ssi->mute_state) {
        /* Check current sink input mute state and enable muting state following. */
        u->ssi->mute_state = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED],
                                             PA_HOOK_EARLY, (pa_hook_cb_t) sink_input_mute_changed, (void *) u);
        ext->local.mute_state = update_state_flag(ext->local.mute_state,
                                                  PA_SINK_INPUT_EXT_STATE_USER,
                                                  si->muted);
    }

    ext->local.mute_state = update_state_flag(ext->local.mute_state,
                                              PA_SINK_INPUT_EXT_STATE_POLICY,
                                              mute);

    if (!si->muted && ext->local.mute_state) {
        ext->local.ignore_mute_state_change = true;
        sink_input_muting_changed = true;
        pa_sink_input_set_mute(si, true, true);
    } else if (si->muted && !ext->local.mute_state) {
        ext->local.ignore_mute_state_change = true;
        sink_input_muting_changed = true;
        pa_sink_input_set_mute(si, false, true);
    }

    pa_log_debug("sink input mute state  after: user: %d policy: %d, %s",
                 ext->local.mute_state & PA_SINK_INPUT_EXT_STATE_USER ? 1 : 0,
                 ext->local.mute_state & PA_SINK_INPUT_EXT_STATE_POLICY ? 1 : 0,
                 sink_input_muting_changed ? "updated muting" : "no change to muting");

    return sink_input_muting_changed;
#else
#error Compiling against unsupported PulseAudio version.
#endif
}

#if (PULSEAUDIO_VERSION >= 6)
static pa_hook_result_t sink_input_mute_changed(pa_core *c, pa_sink_input *sinp, struct userdata *u)
{
    struct pa_sink_input_ext *ext;

    pa_assert(c);
    pa_assert(sinp);
    pa_assert(u);

    pa_assert_se((ext = pa_sink_input_ext_lookup(u, sinp)));
    if (ext->local.ignore_mute_state_change) {
        ext->local.ignore_mute_state_change = false;
        return PA_HOOK_OK;
    }

    ext->local.mute_state = update_state_flag(ext->local.mute_state,
                                              PA_SINK_INPUT_EXT_STATE_USER,
                                              sinp->muted);

    pa_log_debug("sink input user muting %d", sinp->muted);

    return PA_HOOK_OK;
}
#endif

static void handle_sink_input_fixate(struct userdata *u,
                                     pa_sink_input_new_data *sinp_data)
{
    struct pa_policy_group *group = NULL;
    const char *sinp_name;
    int         group_volume;
    pa_cvolume  group_limit;
    uint32_t flags;

    pa_assert(u);
    pa_assert(sinp_data);

    pa_assert_se((group = get_group(u, NULL, sinp_data->proplist, &flags)));
    sinp_name = sink_input_ext_get_name(sinp_data->proplist);
    group_volume = group->flags & PA_POLICY_GROUP_FLAG_LIMIT_VOLUME;

    /* Set volume factor in sink_input_fixate() so that we have our target sink and
     * channel_map defined properly. */
    if (group_volume && !group->mutebyrt &&
             group->limit > 0 && group->limit < PA_VOLUME_NORM)
    {
        pa_log_debug("set stream '%s'/'%s' volume factor to %d",
                     group->name, sinp_name,
                     (group->limit * 100) / PA_VOLUME_NORM);

        pa_cvolume_set(&group_limit,
                       sinp_data->channel_map.channels,
                       group->limit);

        /* we need to check in handle_new_sink_input() if the volume limit was applied,
         * and update our internal data structure. */
        pa_sink_input_new_data_add_volume_factor(sinp_data, VOLUME_LIMIT_FACTOR_KEY, &group_limit);
    }
}

static void handle_removed_sink_input(struct userdata      *u,
                                      struct pa_sink_input *sinp)
{
    struct pa_policy_group *group = NULL;
    struct pa_sink_input_ext *ext;
    struct pa_sink *sink;
    uint32_t        idx;
    const char     *snam;
    uint32_t        flags;

    if (sinp && u) {
        idx  = sinp->index;
        sink = sinp->sink;
        snam = sink_input_ext_get_name(sinp->proplist);
        pa_assert_se((group = get_group_or_classify(u, sinp, &flags)));

        if (flags & PA_POLICY_LOCAL_ROUTE)
            pa_sink_ext_restore_port(u, sink);

        if (flags & PA_POLICY_LOCAL_MUTE)
            pa_policy_groupset_restore_volume(u, sink);
            
        pa_policy_context_unregister(u, pa_policy_object_sink_input,
                                     snam, sinp, sinp->index);
        pa_policy_group_remove_sink_input(u, sinp->index);

        if ((ext = pa_index_hash_remove(u->hsi, idx)) == NULL)
            pa_log("no extension found for sink-input '%s' (idx=%u)",snam,idx);
        else {
            pa_xfree(ext);
        }

        pa_log_debug("removed sink_input '%s' (idx=%d) (group=%s)",
                     snam, idx, group->name);
    }
}

static uint32_t update_state_flag(uint32_t flags, enum pa_sink_input_ext_state flag, bool set)
{
    if (set)
        return flags | flag;
    else
        return flags & ~flag;
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
