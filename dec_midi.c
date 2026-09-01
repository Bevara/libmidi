/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / MIDI decoder filter
 *  based on TiMidity++ (https://timidity.sourceforge.net/)
 *
 *  Renders a Standard MIDI File to PCM by driving TiMidity++'s own
 *  "wave" output driver (writes a real WAV file), then re-parses that
 *  WAV to extract raw interleaved S16 PCM matching this project's other
 *  audio decoders' output contract.
 */

#include <gpac/filters.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "timidity.h"
#include "common.h"
#include "output.h"
#include "controls.h"
#include "instrum.h"
#include "playmidi.h"
#include "readmidi.h"

extern PlayMode wave_play_mode;
extern int got_a_configuration;
extern void timidity_start_initialize(void);
extern int read_config_file(char *name, int self);
extern void timidity_init_player(void);
extern int timidity_play_main(int nfiles, char **files);

typedef struct
{
	GF_FilterPid *ipid, *opid;

	Bool is_playing;
	Bool lib_inited;
	u32 nb_calls;
} GF_MidiDecCtx;

static GF_Err mididec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_MidiDecCtx *ctx = (GF_MidiDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop)
		return GF_NOT_SUPPORTED;
	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));

	return GF_OK;
}

/* Scan a RIFF/WAVE buffer for the "fmt " and "data" chunks. Returns GF_TRUE on success. */
static Bool parse_wav(const u8 *buf, u32 size, u32 *sample_rate, u32 *channels, const u8 **pcm, u32 *pcm_size)
{
	u32 pos;
	if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return GF_FALSE;

	pos = 12;
	*sample_rate = 0;
	*channels = 0;
	*pcm = NULL;
	*pcm_size = 0;

	while (pos + 8 <= size)
	{
		u32 chunk_size;
		char id[5];
		memcpy(id, buf + pos, 4);
		id[4] = 0;
		memcpy(&chunk_size, buf + pos + 4, 4);
		/* little-endian */
		chunk_size = ((u32)buf[pos + 4]) | ((u32)buf[pos + 5] << 8) | ((u32)buf[pos + 6] << 16) | ((u32)buf[pos + 7] << 24);

		if (!strcmp(id, "fmt ") && pos + 8 + 16 <= size)
		{
			*channels = buf[pos + 8 + 2] | (buf[pos + 8 + 3] << 8);
			*sample_rate = buf[pos + 8 + 4] | (buf[pos + 8 + 5] << 8) | (buf[pos + 8 + 6] << 16) | (buf[pos + 8 + 7] << 24);
		}
		else if (!strcmp(id, "data"))
		{
			u32 avail = size - (pos + 8);
			u32 dsize = (chunk_size > avail) ? avail : chunk_size;
			*pcm = buf + pos + 8;
			*pcm_size = dsize;
		}

		pos += 8 + chunk_size + (chunk_size & 1);
	}

	return (*sample_rate && *channels && *pcm && *pcm_size) ? GF_TRUE : GF_FALSE;
}

static GF_Err mididec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size;
	char in_path[64], out_path[64];
	FILE *fp;
	u8 *wav_buf;
	long wav_size;
	u32 sample_rate = 0, channels = 0, pcm_size = 0;
	const u8 *pcm = NULL;
	char *files[1];
	GF_MidiDecCtx *ctx = (GF_MidiDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);

	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	if (!ctx->lib_inited)
	{
		timidity_start_initialize();
		/* freepats.cfg references patches (Tone_000/..., Drum_000/...) via
		 * paths resolved relative to the current working directory, not
		 * relative to the config file itself - chdir there first. */
		if (chdir("/libmidi_patches") != 0)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[MIDI] Failed to chdir to /libmidi_patches\n"));
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_SERVICE_ERROR;
		}
		if (read_config_file("freepats.cfg", 0))
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[MIDI] Failed to load freepats.cfg\n"));
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_SERVICE_ERROR;
		}
		got_a_configuration = 1;
		play_mode = &wave_play_mode;
		timidity_init_player();
		ctx->lib_inited = GF_TRUE;
	}

	snprintf(in_path, sizeof(in_path), "/tmp/gpac_mididec_in_%u.mid", ctx->nb_calls);
	snprintf(out_path, sizeof(out_path), "/tmp/gpac_mididec_out_%u.wav", ctx->nb_calls);
	ctx->nb_calls++;

	fp = fopen(in_path, "wb");
	if (!fp)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}
	fwrite(data, 1, size, fp);
	fclose(fp);

	play_mode->name = out_path;
	files[0] = in_path;
	timidity_play_main(1, files);

	remove(in_path);

	fp = fopen(out_path, "rb");
	if (!fp)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	fseek(fp, 0, SEEK_END);
	wav_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	wav_buf = (u8 *)gf_malloc(wav_size);
	if (!wav_buf)
	{
		fclose(fp);
		remove(out_path);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	fread(wav_buf, 1, wav_size, fp);
	fclose(fp);
	remove(out_path);

	if (!parse_wav(wav_buf, (u32)wav_size, &sample_rate, &channels, &pcm, &pcm_size))
	{
		gf_free(wav_buf);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT((channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, pcm_size, &output);
	if (!dst_pck)
	{
		gf_free(wav_buf);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, pcm_size);
	gf_free(wav_buf);

	gf_filter_pck_merge_properties(pck, dst_pck);
	gf_filter_pck_set_dependency_flags(dst_pck, 0);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_EOS;
}

static const GF_FilterCapability MidiDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_4CC('M', 'I', 'D', 'I')),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister MidiDecoderRegister = {
	.name = "mididec",
	GF_FS_SET_DESCRIPTION("MIDI decoder")
		GF_FS_SET_HELP("This filter renders Standard MIDI Files to PCM audio using TiMidity++ and the bundled freepats instrument set.")
			.private_size = sizeof(GF_MidiDecCtx),
	SETCAPS(MidiDecCaps),
	.configure_pid = mididec_configure_pid,
	.process = mididec_process,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE mididec_register(GF_FilterSession *session)
{
	return &MidiDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_mididec(void) {
    gf_filter_auto_register("mididec", mididec_register);
}
