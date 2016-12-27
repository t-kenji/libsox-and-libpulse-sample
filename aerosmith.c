#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pulse/channelmap.h>
#include <pulse/thread-mainloop.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <pulse/error.h>
#include <sox.h>


#define DEBUG(fmt,...) do{fprintf(stderr,"%s:%d " fmt "\n",__FILE__,__LINE__,##__VA_ARGS__);}while(0)

#define UNUSED_VARIABLE(x) (void) (x)


/** モノラル */
#define CH_MONO (1)

/** ステレオ */
#define CH_STEREO (2)

/** 配列長取得 */
#define lengthof(array) (sizeof(array)/sizeof((array)[0]))

/** 作業スレッド列挙子 */
enum WorkerThreads {
    WORKER_FOREGROUND,      /* 前面音声再生スレッド */
    WORKER_BACKGROUND,      /* 後面音声再生スレッド */
    WORKER_LENGTH
};

struct tagAerosmith;

struct sound_play_req_t {
    uint32_t play_id;
    char source[256];
    int be_overwrite;
};

struct sound_volume_req_t {
    uint16_t volume;
};

struct tagPlayItem {
    uint32_t play_id;
    char source[256];
    int channels;
    bool be_cancel;
};

struct tagPlaylist {
    struct tagPlayItem items[20];
    int write_index;
    int read_index;
    int count;
    sem_t sync;
    pthread_mutex_t exclusion;
};

struct tagSinkParam {
    pthread_t thread_id;
    struct tagAerosmith *parent;

    pa_cvolume volume;
    int index;
    int dummy_device;

    struct tagPlaylist playlist;
    struct tagPlayItem *playing;
};

struct tagAerosmith {
    pa_sample_spec sample_spec;
    pa_threaded_mainloop *mainloop;
    pa_context *context;

    int sample_size;
    int frame_size;

    struct tagSinkParam sinks[WORKER_LENGTH];
    struct tagSinkParam *standby;
    pthread_mutex_t play_mutex;
};

#include <sys/syscall.h>
static inline pid_t gettid(void)
{
	return (pid_t) syscall(SYS_gettid);
}

static void playlist_init(struct tagPlaylist *playlist)
{
    playlist->write_index = 0;
    playlist->read_index = 0;
    playlist->count = 0;
    pthread_mutex_init(&playlist->exclusion, NULL);
    sem_init(&playlist->sync, 0, 0);
}

static void playlist_enq(struct tagPlaylist *playlist, struct tagPlayItem *item)
{
    pthread_mutex_lock(&playlist->exclusion);
    if (playlist->count >= lengthof(playlist->items)) {
        DEBUG("playlist full!!");
        pthread_mutex_unlock(&playlist->exclusion);
        return;
    }
    playlist->items[playlist->write_index++] = *item;
    ++playlist->count;
    playlist->write_index %= lengthof(playlist->items);
    DEBUG("wrote: %x %s, index: %d, count: %d", item->play_id, item->source, playlist->write_index, playlist->count);
    pthread_mutex_unlock(&playlist->exclusion);
    sem_post(&playlist->sync);
}

static void playlist_deq(struct tagPlaylist *playlist, struct tagPlayItem *item)
{
    sem_wait(&playlist->sync);
    pthread_mutex_lock(&playlist->exclusion);
    if (playlist->count <= 0) {
        DEBUG("playlist is empty...");
        pthread_mutex_unlock(&playlist->exclusion);
        return;
    }
    *item = playlist->items[playlist->read_index++];
    --playlist->count;
    playlist->read_index %= lengthof(playlist->items);
    DEBUG("read: %x %s, index: %d, count: %d", item->play_id, item->source, playlist->read_index, playlist->count);
    pthread_mutex_unlock(&playlist->exclusion);
}

static void playlist_clear(struct tagPlaylist *playlist)
{
    int sval;

    pthread_mutex_lock(&playlist->exclusion);
    playlist->write_index = 0;
    playlist->read_index = 0;
    playlist->count = 0;

    sem_getvalue(&playlist->sync, &sval);
    if (sval > 0) {
        sem_init(&playlist->sync, 0, 0);
    }
    pthread_mutex_unlock(&playlist->exclusion);
}

static int playlist_count(struct tagPlaylist *playlist)
{
    return playlist->count;
}

static void print_sink_properties(pa_proplist *props)
{
    void *state = NULL;

    printf("  Properties are:\n");
    while (1) {
        const char *key;
        if ((key = pa_proplist_iterate(props, &state)) == NULL) {
            break;
        }
        const char *val = pa_proplist_gets(props, key);
        printf("    %s: %s\n", key, val);
    }
}

static void print_sink_info(const pa_sink_input_info *info)
{
    #define DUMP(name, fmt) printf("  " #name ": " #fmt "\n", info->name)
    DUMP(index, %u);
    DUMP(name, %s);
    DUMP(owner_module, %u);
    DUMP(client, %u);
    DUMP(sink, %u);
    DUMP(driver, %s);
    DUMP(mute, %d);
    DUMP(corked, %d);
    DUMP(has_volume, %d);
    DUMP(volume_writable, %d);
    #undef DUMP

    print_sink_properties(info->proplist);
}

static int update_status(sox_bool all_done, void * client_data)
{
    struct tagPlayItem *item = client_data;
	UNUSED_VARIABLE(all_done);
    return (item->be_cancel) ? SOX_EOF : SOX_SUCCESS;
}

static size_t sox_read_wide(sox_format_t *fmt, sox_sample_t *buf, size_t max_length)
{
	size_t read_length = sox_read(fmt, buf, max_length) / fmt->signal.channels;
	if ((read_length == 0) && (fmt->sox_errno != 0)) {
		DEBUG("`%s` %s: %s", fmt->filename, fmt->sox_errstr, sox_strerror(fmt->sox_errno));
	}
	return read_length;
}

static int input_drain(sox_effect_t *effp, sox_sample_t *obuf, size_t *osamp)
{
	sox_format_t **fmt = effp->priv;
	size_t read_length = 0;

	UNUSED_VARIABLE(effp);

	read_length = sox_read_wide(*fmt, obuf, *osamp);
	read_length *= effp->in_signal.channels;
	*osamp = read_length;

	return (read_length > 0) ? SOX_SUCCESS : SOX_EOF;
}

static sox_effect_handler_t const *get_input_effect_handler(void)
{
	static sox_effect_handler_t handler = {
		"input",
		NULL,
		SOX_EFF_MCHAN,
		NULL,
		NULL,
		NULL,
		input_drain,
		NULL,
		NULL,
		sizeof(sox_format_t *)
	};
	return &handler;
}

static int output_flow(sox_effect_t *effp, sox_sample_t const *ibuf, sox_sample_t *obuf, size_t *isamp, size_t *osamp)
{
	sox_format_t **fmt = effp->priv;
	size_t output_length;

	UNUSED_VARIABLE(effp);
	UNUSED_VARIABLE(obuf);

	*osamp = 0;
	output_length = (*isamp > 0) ? sox_write(*fmt, ibuf, *isamp) : 0;
	if (output_length != *isamp) {
		if ((*fmt)->sox_errno) {
			DEBUG("`%s` %s: %s", (*fmt)->filename, (*fmt)->sox_errstr, sox_strerror((*fmt)->sox_errno));
		}
		return SOX_EOF;
	}
	return SOX_SUCCESS;
}

static sox_effect_handler_t const *get_output_effect_handler(void)
{
	static sox_effect_handler_t handler = {
		"output",
		NULL,
		SOX_EFF_MCHAN,
		NULL,
		NULL,
		output_flow,
		NULL,
		NULL,
		NULL,
		sizeof(sox_format_t *)
	};
	return &handler;
}

static int play_sound(struct tagPlayItem *item)
{
	struct tagSoundFile {
		char *path;
		char const *type;
		sox_signalinfo_t si;
		sox_encodinginfo_t ei;
		sox_oob_t oob;
		sox_format_t *fmt;
	} input, output;
	sox_encodinginfo_t temp_enc;
	sox_oob_t oob;
    sox_effects_chain_t *chain;
	sox_signalinfo_t interm_signal;
    sox_effect_t *effect;
	uint64_t output_length;

	/**
	 * initialize input file.
	 */
	memset(&input, 0, sizeof(input));
	sox_init_encodinginfo(&input.ei);
	input.path = item->source;

	/**
	 * initialize output device.
 	 */
	memset(&output, 0, sizeof(output));
	sox_init_encodinginfo(&output.ei);
	output.path = "default";
	output.type = "pulseaudio";

	signal(SIGINT, SIG_IGN);
    input.fmt = sox_open_read(input.path, &input.si, &input.ei, input.type);
    if (!input.fmt) {
        DEBUG("sox_open_read failed: %d", errno);
        return -1;
    }
	signal(SIGINT, SIG_DFL);

	temp_enc = output.ei;
	if (!temp_enc.encoding) {
		temp_enc.encoding = input.fmt->encoding.encoding;
	}
	if (!temp_enc.bits_per_sample) {
		temp_enc.bits_per_sample = input.fmt->encoding.bits_per_sample;
	}
	if (sox_format_supports_encoding(output.path, output.type, &temp_enc)) {
		output.ei= temp_enc;
	}

	output_length = input.fmt->signal.length / input.fmt->signal.channels;
	if (!output.si.rate) {
		output.si.rate = input.fmt->signal.rate;
	}
	if (!output.si.channels) {
		output.si.channels = input.fmt->signal.channels;
	}
	output.si.length = (uint64_t) (output_length * output.si.channels * output.si.rate / input.fmt->signal.rate + .5);
	
	oob = input.fmt->oob;
	output.fmt = sox_open_write(output.path, &output.si, &output.ei, output.type, &oob, NULL);
    if (!output.fmt) {
        DEBUG("sox_open_write failed: %d", errno);
        sox_close(input.fmt);
        return -1;
    }

    chain = sox_create_effects_chain(&input.fmt->encoding, &output.fmt->encoding);

    interm_signal = input.fmt->signal;

	effect = sox_create_effect(get_input_effect_handler());
	memcpy(effect->priv, &input.fmt, sizeof(input.fmt));
	sox_add_effect(chain, effect, &interm_signal, &output.fmt->signal);
	free(effect);

	effect = sox_create_effect(get_output_effect_handler());
	memcpy(effect->priv, &output.fmt, sizeof(output.fmt));
	if (sox_add_effect(chain, effect, &interm_signal, &output.fmt->signal) != SOX_SUCCESS) {
		DEBUG("sox_add_effect failed.");
		free(effect);
		sox_close(output.fmt);
		sox_close(input.fmt);
	}
	free(effect);

    sox_flow_effects(chain, update_status, item);

    sox_delete_effects_chain(chain);
    sox_close(output.fmt);
    sox_close(input.fmt);

    return 0;
}

static void declarate_to_play_sound(struct tagSinkParam *param, struct tagPlayItem *item)
{
    pthread_mutex_lock(&param->parent->play_mutex);
    param->index = 0;
    param->parent->standby = param;
    param->playing = item;
    //param->dummy_device = 1; /* Ubuntu では probe 用のデバイスが１つ作成されるためスキップする */
    param->dummy_device = 0;
}

static void end_sound_playback(struct tagSinkParam *param)
{
    param->playing = NULL;
    param->index = 0;
}

static int is_playing_foreground(struct tagSinkParam *param)
{
    return ((playlist_count(&param->parent->sinks[WORKER_FOREGROUND].playlist) > 0)
            || (param->parent->sinks[WORKER_FOREGROUND].playing != NULL));
}

static void set_background_volume(struct tagSinkParam *param, pa_volume_t vol)
{
    pa_operation *ope;
    pa_cvolume volume;

    if (param->parent->sinks[WORKER_BACKGROUND].index == 0) {
        return;
    }

    pa_cvolume_set(&volume, param->parent->sinks[WORKER_FOREGROUND].playing->channels, vol);
    ope = pa_context_set_sink_input_volume(param->parent->context, param->parent->sinks[WORKER_BACKGROUND].index, &volume, NULL, NULL);
    if (!ope) {
        DEBUG("pa_context_set_sink_input_volume failed.");
    } else {
        pa_operation_unref(ope);
    }
}

static void context_state_callback(pa_context *ctx, void *userdata)
{
    struct tagAerosmith *self = userdata;

    DEBUG("state: %d", pa_context_get_state(ctx));
    switch (pa_context_get_state(ctx)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            pa_threaded_mainloop_signal(self->mainloop, 0);
            break;
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
    }
}

__attribute__((unused))
static void context_get_sink_input_callback(pa_context *ctx, const pa_sink_input_info *info, int eol, void *userdata)
{
    if (eol < 0) {
        if (pa_context_errno(ctx) == PA_ERR_NOENTITY) {
            return;
        }
        DEBUG("Sink input callback fail.");
        return;
    }
    if (eol > 0) {
        return;
    }

    print_sink_info(info);
}

static void context_subscribe_callback(pa_context *ctx, pa_subscription_event_type_t type, uint32_t idx, void *userdata)
{
    struct tagAerosmith *self = userdata;
    pa_operation *ope;
    unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        return;
    }

    type &= PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    switch (type) {
        case PA_SUBSCRIPTION_EVENT_NEW:
            DEBUG("new index: %d", idx);
            if (self->standby->dummy_device > 0) {
                DEBUG("skip dummy device.");
                --self->standby->dummy_device;
                return;
            }
            {
                char to_string[256];
                pa_cvolume_snprint(to_string, sizeof(to_string), &self->standby->volume);
                DEBUG("volume: %s", to_string);
            }
            ope = pa_context_set_sink_input_volume(ctx, idx, &self->standby->volume, NULL, NULL);
            if (!ope) {
                DEBUG("pa_context_set_sink_input_volume failed.");
            } else {
                pa_operation_unref(ope);
            }
#if 0
            ope = pa_context_get_sink_input_info(ctx, idx, context_get_sink_input_callback, NULL);
            if (!ope) {
                DEBUG("pa_context_get_sink_input_info failed.");
            } else {
                pa_operation_unref(ope);
            }
#endif
            self->standby->index = idx;
            DEBUG("now playing: %08x %s(%d)", self->standby->playing->play_id, self->standby->playing->source, self->standby->index);
            pthread_mutex_unlock(&self->play_mutex);
            DEBUG("mutex unlocked");
            break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
            DEBUG("change index: %d", idx);
            break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            DEBUG("remove index: %d", idx);
            break;
		default:
			DEBUG("event: %d, index: %d", type, idx);
			break;
    }
}

static void terminate_sox(struct tagAerosmith *self)
{
    sox_quit();
}

static int initialize_sox(struct tagAerosmith *self)
{
    sox_globals.verbosity = 0;

    if (sox_init() != SOX_SUCCESS) {
        DEBUG("sox_init failed.");
        return -1;
    }

    return 0;
}

static void terminate_pulseaudio(struct tagAerosmith *self)
{
    pa_threaded_mainloop_stop(self->mainloop);

    if (self->context) {
        pa_context_disconnect(self->context);

        pa_context_set_state_callback(self->context, NULL, NULL);
        pa_context_set_subscribe_callback(self->context, NULL, NULL);

        pa_context_unref(self->context);
        self->context = NULL;
    }

    pa_threaded_mainloop_free(self->mainloop);
    self->mainloop = NULL;
}

static int initialize_pulseaudio(struct tagAerosmith *self)
{
    self->sample_spec.format = PA_SAMPLE_FLOAT32;
    self->sample_spec.rate = 44100;
    self->sample_spec.channels = CH_STEREO;

    self->sample_size = pa_sample_size(&self->sample_spec);
    self->frame_size = pa_frame_size(&self->sample_spec);

    self->mainloop = pa_threaded_mainloop_new();
    if (!self->mainloop) {
        DEBUG("pa_threaded_mainloop_new failed.");
        return -1;
    }
    if (pa_threaded_mainloop_start(self->mainloop) < 0) {
        DEBUG("pa_threaded_mainloop_start failed.");
        pa_threaded_mainloop_free(self->mainloop);
        self->mainloop = NULL;
        return -1;
    }
    pa_threaded_mainloop_lock(self->mainloop);

    self->context = pa_context_new(pa_threaded_mainloop_get_api(self->mainloop), "Aerosmith");
    if (!self->context) {
        DEBUG("pa_context_new failed.");
        pa_threaded_mainloop_unlock(self->mainloop);
        terminate_pulseaudio(self);
        return -1;
    }
    DEBUG("connect to server");

    pa_context_set_state_callback(self->context, context_state_callback, self);
    pa_context_set_subscribe_callback(self->context, context_subscribe_callback, self);

    if (pa_context_connect(self->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        DEBUG("pa_context_connect failed.");
        pa_threaded_mainloop_unlock(self->mainloop);
        terminate_pulseaudio(self);
        return -1;
    }

    /* wait for ready */
    while (1) {
        pa_context_state_t state;

        state = pa_context_get_state(self->context);

        if (!PA_CONTEXT_IS_GOOD(state)) {
            DEBUG("failed to connect: %s", pa_strerror(pa_context_errno(self->context)));
            pa_threaded_mainloop_unlock(self->mainloop);
            terminate_pulseaudio(self);
            return -1;
        }

        if (state == PA_CONTEXT_READY) {
            break;
        }

        pa_threaded_mainloop_wait(self->mainloop);
    }
    DEBUG("connected");

    pa_context_subscribe(self->context, PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);

    pa_threaded_mainloop_unlock(self->mainloop);

    return 0;
}

static void terminate(struct tagAerosmith *self)
{
    terminate_sox(self);
    terminate_pulseaudio(self);

    return;
}

static int initialize(struct tagAerosmith *self)
{
    int i;

    for (i = 0; i < lengthof(self->sinks); ++i) {
        self->sinks[i].parent = self;
        playlist_init(&self->sinks[i].playlist);
    }
    pthread_mutex_init(&self->play_mutex, NULL);

    if (initialize_pulseaudio(self) != 0) {
        return -1;
    }
    if (initialize_sox(self) != 0) {
        return -1;
    }

    return 0;
}

static void *fg_worker(void *userdata)
{
    struct tagSinkParam *param = userdata;
    struct tagPlayItem item;
    pa_volume_t vol;

    while (1) {
        playlist_deq(&param->playlist, &item);
        DEBUG("deq: %x %s", item.play_id, item.source);

        declarate_to_play_sound(param, &item);

        pa_cvolume_set(&param->volume, item.channels, PA_VOLUME_NORM);

        vol = PA_VOLUME_NORM / 2;
        set_background_volume(param, vol);
        play_sound(&item);
        if (playlist_count(&param->playlist) == 0){
            vol = PA_VOLUME_NORM;
            set_background_volume(param, vol);
        }

        end_sound_playback(param);
    }

    return NULL;
}

static void *bg_worker(void *userdata)
{
    struct tagSinkParam *param = userdata;
    struct tagPlayItem item;
    pa_volume_t vol;

    while (1) {
        playlist_deq(&param->playlist, &item);
        DEBUG("deq: %x %s", item.play_id, item.source);

        declarate_to_play_sound(param, &item);

        if (is_playing_foreground(param)) {
            vol = PA_VOLUME_NORM / 2;
        } else {
            vol = PA_VOLUME_NORM;
        }
        pa_cvolume_set(&param->volume, item.channels, vol);
        play_sound(&item);

        end_sound_playback(param);
    }

    return NULL;
}

int main(int argc, char **argv)
{
	enum {
		ARGV_SELF,
		ARGV_BG_SOUND,
		ARGV_FG_SOUND,
		ARGV_LENGTH
	};
    static struct tagAerosmith self;

    if (initialize(&self) != 0) {
        return 1;
    }
    
    /* foreground worker */
    if (pthread_create(&self.sinks[WORKER_FOREGROUND].thread_id,
			NULL, fg_worker, &self.sinks[WORKER_FOREGROUND]) != 0) {
        DEBUG("pthread_create failed.");
        terminate(&self);
        return 1;
    }
    pthread_detach(self.sinks[WORKER_FOREGROUND].thread_id);

    /* background worker */
    if (pthread_create(&self.sinks[WORKER_BACKGROUND].thread_id,
			NULL, bg_worker, &self.sinks[WORKER_BACKGROUND]) != 0) {
        DEBUG("pthread_create failed.");
        terminate(&self);
        return 2;
    }
    pthread_detach(self.sinks[WORKER_BACKGROUND].thread_id);

#if 1
    {
        struct tagPlayItem item;
        uint32_t play_id = 0;
        int i;

        for (i = 0; i < 10; ++i) {
            item.play_id = ++play_id;
            strncpy(item.source, argv[ARGV_BG_SOUND], sizeof(item.source));
            item.channels = self.sample_spec.channels;
            item.be_cancel = false;

            playlist_enq(&self.sinks[WORKER_BACKGROUND].playlist, &item);
        }
    }
	if (argc >= ARGV_LENGTH) {
   	     struct tagPlayItem item;
   	     uint32_t play_id = 0x100;
   	     int i;

        for (i = 0; i < 10; ++i) {
			sleep(5);
            item.play_id = ++play_id;
            strncpy(item.source, argv[ARGV_FG_SOUND], sizeof(item.source));
            item.channels = self.sample_spec.channels;
            item.be_cancel = false;

            playlist_enq(&self.sinks[WORKER_FOREGROUND].playlist, &item);
        }
    }
    sleep(120);
#else
    bool be_exit = false;
    mqd_t fg_mq, bg_mq, vol_mq;
    struct mq_attr attr;
    struct sound_play_req_t *play_req;
    struct sound_volume_req_t *vol_req;
    char *buf;
    ssize_t received;
    struct tagPlayItem item;

    fg_mq = mq_open("/aerosmith-fg", O_RDONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0666, NULL);
    if (fg_mq == (mqd_t) -1) {
        DEBUG("mq_open failed: %d", errno);
        terminate(&self);
        return 3;
    }
    bg_mq = mq_open("/aerosmith-bg", O_RDONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0666, NULL);
    if (bg_mq == (mqd_t) -1) {
        DEBUG("mq_open failed: %d", errno);
        mq_close(fg_mq);
        terminate(&self);
        return 4;
    }
    vol_mq = mq_open("/aerosmith-vol", O_RDONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0666, NULL);
    if (vol_mq == (mqd_t) -1) {
        DEBUG("mq_open failed: %d", errno);
        mq_close(bg_mq);
        mq_close(fg_mq);
        terminate(&self);
        return 5;
    }
    mq_getattr(fg_mq, &attr);
    buf = malloc(attr.mq_msgsize);
    while (!be_exit) {
        received = mq_receive(fg_mq, buf, attr.mq_msgsize, NULL);
        if (received == sizeof(*play_req)) {
            play_req = (struct sound_play_req_t *) buf;
            if (play_req->be_overwrite) {
                playlist_clear(&self.sinks[WORKER_FOREGROUND].playlist);
                if (self.sinks[WORKER_FOREGROUND].playing) {
                    self.sinks[WORKER_FOREGROUND].playing->be_cancel = true;
                }
            }
            if (play_req->play_id == 0xFFFFFFFF) {
                be_exit = true;
            } else if (play_req->play_id > 0) {
                item.play_id = play_req->play_id;
                strncpy(item.source, play_req->source, sizeof(item.source));
                item.channels = 2;
                item.be_cancel = false;
                playlist_enq(&self.sinks[WORKER_FOREGROUND].playlist, &item);
            }
        } else if ((received == -1) && (errno != EAGAIN)) {
            DEBUG("mq_receive failed: %d", errno);
        }
        received = mq_receive(bg_mq, buf, attr.mq_msgsize, NULL);
        if (received == sizeof(*play_req)) {
            play_req = (struct sound_play_req_t *) buf;
            if (play_req->be_overwrite) {
                playlist_clear(&self.sinks[WORKER_BACKGROUND].playlist);
                if (self.sinks[WORKER_BACKGROUND].playing) {
                    self.sinks[WORKER_BACKGROUND].playing->be_cancel = true;
                }
            }
            if (play_req->play_id == 0xFFFFFFFF) {
                be_exit = true;
            } else if (play_req->play_id > 0) {
                item.play_id = play_req->play_id;
                strncpy(item.source, play_req->source, sizeof(item.source));
                item.channels = 2;
                item.be_cancel = false;
                playlist_enq(&self.sinks[WORKER_BACKGROUND].playlist, &item);
            }
        } else if ((received == -1) && (errno != EAGAIN)) {
            DEBUG("mq_receive failed: %d", errno);
        }
        received = mq_receive(vol_mq, buf, attr.mq_msgsize, NULL);
        if (received == sizeof(*vol_req)) {
            pa_cvolume volume;
            pa_operation *ope;
            vol_req = (struct sound_volume_req_t *) buf;
            pa_cvolume_set(&volume, 2, (pa_volume_t) vol_req->volume);
            ope = pa_context_set_sink_volume_by_index(self.context, 0, &volume, NULL, NULL);
            if (!ope) {
                DEBUG("pa_context_set_sink_volume_by_index failed: %d", pa_context_errno(self.context));
            } else {
                pa_operation_unref(ope);
            }
        } else if ((received == -1) && (errno != EAGAIN)) {
            DEBUG("mq_receive failed: %d", errno);
        }
        usleep(100000);
    }
    mq_close(vol_mq);
    mq_close(bg_mq);
    mq_close(fg_mq);
#endif

    terminate(&self);

    return 0;
}
