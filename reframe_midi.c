/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / MIDI audio reframer filter
 *  (feeds the timidity-based decoder)
 *
 */

#include <gpac/filters.h>
#include <string.h>

typedef struct
{
	GF_FilterPid *ipid;
	GF_FilterPid *opid;
	u32 src_timescale;
	Bool owns_timescale;
	u32 codec_id;

	Bool initial_play_done;
	Bool is_playing;
} GF_ReframeMidiCtx;

static GF_Err rfmidi_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_ReframeMidiCtx *ctx = gf_filter_get_udta(filter);
	const GF_PropertyValue *p;

	if (is_remove)
	{
		ctx->ipid = NULL;
		return GF_OK;
	}

	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	gf_filter_pid_set_framing_mode(pid, GF_TRUE);
	ctx->ipid = pid;
	ctx->codec_id = 0;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p)
		ctx->src_timescale = p->value.uint;

	if (ctx->src_timescale && !ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
	}
	ctx->is_playing = GF_TRUE;
	return GF_OK;
}

static Bool rfmidi_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_FilterEvent fevt;
	GF_ReframeMidiCtx *ctx = gf_filter_get_udta(filter);
	if (evt->base.on_pid != ctx->opid)
		return GF_TRUE;
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		if (ctx->is_playing)
			return GF_TRUE;
		ctx->is_playing = GF_TRUE;
		if (!ctx->initial_play_done)
		{
			ctx->initial_play_done = GF_TRUE;
			return GF_TRUE;
		}
		GF_FEVT_INIT(fevt, GF_FEVT_SOURCE_SEEK, ctx->ipid);
		fevt.seek.start_offset = 0;
		gf_filter_pid_send_event(ctx->ipid, &fevt);
		return GF_TRUE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		break;
	}
	return GF_TRUE;
}

static GF_Err rfmidi_process(GF_Filter *filter)
{
	GF_ReframeMidiCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck, *dst_pck;
	GF_Err e;
	u8 *data;
	u32 size;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->opid)
				gf_filter_pid_set_eos(ctx->opid);
			ctx->is_playing = GF_FALSE;
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);

	if (!ctx->opid || !ctx->codec_id)
	{
		if (size < 4 || memcmp(data, "MThd", 4))
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		ctx->codec_id = GF_4CC('M', 'I', 'D', 'I');
		ctx->opid = gf_filter_pid_new(filter);
		if (!ctx->opid)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_SERVICE_ERROR;
		}

		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(ctx->codec_id));

		if (!gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_TIMESCALE))
		{
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000));
			ctx->owns_timescale = GF_TRUE;
		}

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NB_FRAMES, &PROP_UINT(1));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PLAYBACK_MODE, &PROP_UINT(GF_PLAYBACK_MODE_FASTFORWARD));
	}

	e = GF_OK;

	dst_pck = gf_filter_pck_new_ref(ctx->opid, 0, size, pck);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	gf_filter_pck_merge_properties(pck, dst_pck);
	if (ctx->owns_timescale)
	{
		gf_filter_pck_set_cts(dst_pck, 0);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_set_duration(dst_pck, 1000);
	}

	gf_filter_pck_send(dst_pck);
	gf_filter_pid_drop_packet(ctx->ipid);

	return e;
}

static const char *rfmidi_probe_data(const u8 *data, u32 size, GF_FilterProbeScore *score)
{
	if (size < 4)
		return NULL;
	if (!memcmp(data, "MThd", 4))
	{
		*score = GF_FPROBE_SUPPORTED;
		return "audio/midi";
	}
	return NULL;
}

static const GF_FilterCapability ReframeMidiCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "mid|midi|kar|smf"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/midi|audio/x-midi"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_4CC('M', 'I', 'D', 'I')),
};

GF_FilterRegister ReframeMidiRegister = {
	.name = "rfmidi",
	GF_FS_SET_DESCRIPTION("MIDI reframer")
		GF_FS_SET_HELP("This filter parses Standard MIDI Files (SMF) as a whole and outputs a single audio PID/frame, meant to be rendered by the mididec filter.\n")
			.private_size = sizeof(GF_ReframeMidiCtx),
	SETCAPS(ReframeMidiCaps),
	.configure_pid = rfmidi_configure_pid,
	.probe_data = rfmidi_probe_data,
	.process = rfmidi_process,
	.process_event = rfmidi_process_event};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE midi_reframe_register(GF_FilterSession *session)
{
	return &ReframeMidiRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_midi_reframe(void) {
    gf_filter_auto_register("midi_reframe", midi_reframe_register);
}
