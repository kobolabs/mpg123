#include "mpg123lib_intern.h"
#include "getbits.h"

#ifdef GAPLESS
#define SAMPLE_ADJUST(x)   ((x) - ((mh->p.flags & MPG123_GAPLESS) ? mh->begin_os : 0))
#define SAMPLE_UNADJUST(x) ((x) + ((mh->p.flags & MPG123_GAPLESS) ? mh->begin_os : 0))
#else
#define SAMPLE_ADJUST(x)   (x)
#define SAMPLE_UNADJUST(x) (x)
#endif

#define SEEKFRAME(mh) ((mh)->ignoreframe < 0 ? 0 : (mh)->ignoreframe)

static int initialized = 0;

#ifdef GAPLESS
/*
	Take the buffer after a frame decode (strictly: it is the data from frame fr->num!) and cut samples out.
	fr->buffer.fill may then be smaller than before...
*/
static void frame_buffercheck(mpg123_handle *fr)
{
	/* The first interesting frame: Skip some leading samples. */
	if(fr->firstoff && fr->num == fr->firstframe)
	{
		off_t byteoff = samples_to_bytes(fr, fr->firstoff);
		if(fr->buffer.fill > byteoff)
		{
			fr->buffer.fill -= byteoff;
			/* buffer.p != buffer.data only for own buffer */
			debug6("cutting %li samples/%li bytes on begin, own_buffer=%i at %p=%p, buf[1]=%i",
			        (long)fr->firstoff, (long)byteoff, fr->own_buffer, (void*)fr->buffer.p, (void*)fr->buffer.data, ((short*)fr->buffer.p)[2]);
			if(fr->own_buffer) fr->buffer.p = fr->buffer.data + byteoff;
			else memmove(fr->buffer.data, fr->buffer.data + byteoff, fr->buffer.fill);
			debug3("done cutting, buffer at %p =? %p, buf[1]=%i",
			        (void*)fr->buffer.p, (void*)fr->buffer.data, ((short*)fr->buffer.p)[2]);
		}
		else fr->buffer.fill = 0;
		fr->firstoff = 0; /* Only enter here once... when you seek, firstoff should be reset. */
	}
	/* The last interesting (planned) frame: Only use some leading samples. */
	if(fr->lastoff && fr->num == fr->lastframe)
	{
		off_t byteoff = samples_to_bytes(fr, fr->lastoff);
		if(fr->buffer.fill > byteoff)
		{
			fr->buffer.fill = byteoff;
		}
		fr->lastoff = 0; /* Only enter here once... when you seek, lastoff should be reset. */
	}
}
#endif


int mpg123_init(void)
{
	if((sizeof(short) != 2) || (sizeof(long) < 4)) return MPG123_BAD_TYPES;

	init_layer2(); /* inits also shared tables with layer1 */
	init_layer3();
#ifndef OPT_MMX_ONLY
	prepare_decode_tables();
#endif
	check_decoders();
	initialized = 1;
	return MPG123_OK;
}

void mpg123_exit(void)
{
	/* nothing yet, but something later perhaps */
	if(initialized) return;
}

/* create a new handle with specified decoder, decoder can be "", "auto" or NULL for auto-detection */
mpg123_handle *mpg123_new(const char* decoder, int *error)
{
	return mpg123_parnew(NULL, decoder, error);
}

/* ...the full routine with optional initial parameters to override defaults. */
mpg123_handle *mpg123_parnew(mpg123_pars *mp, const char* decoder, int *error)
{
	mpg123_handle *fr = NULL;
	int err = MPG123_OK;
	if(initialized) fr = (mpg123_handle*) malloc(sizeof(mpg123_handle));
	else err = MPG123_NOT_INITIALIZED;
	if(fr != NULL)
	{
		frame_init_par(fr, mp);
		debug("cpu opt setting");
		if(frame_cpu_opt(fr, decoder) != 1)
		{
			err = MPG123_BAD_DECODER;
			frame_exit(fr);
			free(fr);
			fr = NULL;
		}
	}
	if(fr != NULL)
	{
		if((frame_outbuffer(fr) != 0) || (frame_buffers(fr) != 0))
		{
			err = MPG123_NO_BUFFERS;
			frame_exit(fr);
			free(fr);
			fr = NULL;
		}
		else
		{
			opt_make_decode_tables(fr);
			fr->decoder_change = 1;
			/* happening on frame change instead:
			init_layer3_stuff(fr);
			init_layer2_stuff(fr); */
		}
	}
	else if(err == MPG123_OK) err = MPG123_OUT_OF_MEM;

	if(error != NULL) *error = err;
	return fr;
}

int mpg123_decoder(mpg123_handle *mh, const char* decoder)
{
	enum optdec dt = dectype(decoder);
	if(mh == NULL) return MPG123_ERR;

	if(dt == nodec)
	{
		mh->err = MPG123_BAD_DECODER;
		return MPG123_ERR;
	}
	if(dt == mh->cpu_opts.type) return MPG123_OK;

	/* Now really change. */
	/* frame_exit(mh);
	frame_init(mh); */
	debug("cpu opt setting");
	if(frame_cpu_opt(mh, decoder) != 1)
	{
		mh->err = MPG123_BAD_DECODER;
		frame_exit(mh);
		return MPG123_ERR;
	}
	/* New buffers for decoder are created in frame_buffers() */
	if((frame_outbuffer(mh) != 0) || (frame_buffers(mh) != 0))
	{
		mh->err = MPG123_NO_BUFFERS;
		frame_exit(mh);
		return MPG123_ERR;
	}
	opt_make_decode_tables(mh);
	mh->decoder_change = 1;
	return MPG123_OK;
}

int mpg123_param(mpg123_handle *mh, int key, long val, double fval)
{
	int r;
	if(mh == NULL) return MPG123_ERR;
	r = mpg123_par(&mh->p, key, val, fval);
	if(r != MPG123_OK){ mh->err = r; r = MPG123_ERR; }
	return r;
}

int mpg123_par(mpg123_pars *mp, int key, long val, double fval)
{
	int ret = MPG123_OK;
	switch(key)
	{
		case MPG123_VERBOSE:
			mp->verbose = val;
		break;
		case MPG123_FLAGS:
#ifndef GAPLESS
			if(val & MPG123_GAPLESS) ret = MPG123_NO_GAPLESS;
			else
#endif
			mp->flags = val;
			debug1("set flags to 0x%lx", (unsigned long) mp->flags);
		break;
		case MPG123_ADD_FLAGS:
			mp->flags |= val;
		break;
		case MPG123_FORCE_RATE: /* should this trigger something? */
			if(val > 96000) ret = MPG123_BAD_RATE;
			else mp->force_rate = val < 0 ? 0 : val; /* >0 means enable, 0 disable */
		break;
		case MPG123_DOWN_SAMPLE:
			if(val < 0 || val > 2) ret = MPG123_BAD_RATE;
			else mp->down_sample = (int)val;
		break;
		case MPG123_RVA:
			if(val < 0 || val > MPG123_RVA_MAX) ret = MPG123_BAD_RVA;
			else mp->rva = (int)val;
		break;
		case MPG123_DOWNSPEED:
			mp->halfspeed = val < 0 ? 0 : val;
		break;
		case MPG123_UPSPEED:
			mp->doublespeed = val < 0 ? 0 : val;
		break;
		case MPG123_ICY_INTERVAL:
			mp->icy_interval = val > 0 ? val : 0;
		break;
		case MPG123_OUTSCALE:
#ifdef FLOATOUT
			mp->outscale = fval;
#else
			mp->outscale = val;
#endif
		break;
		case MPG123_TIMEOUT:
#ifndef WIN32
			mp->timeout = val >= 0 ? val : 0;
#else
			ret = MPG123_NO_TIMEOUT;
#endif
		break;
		default:
			ret = MPG123_BAD_PARAM;
	}
	return ret;
}

int mpg123_getparam(mpg123_handle *mh, int key, long *val, double *fval)
{
	int r;
	if(mh == NULL) return MPG123_ERR;
	r = mpg123_getpar(&mh->p, key, val, fval);
	if(r != MPG123_OK){ mh->err = r; r = MPG123_ERR; }
	return r;
}

int mpg123_getpar(mpg123_pars *mp, int key, long *val, double *fval)
{
	int ret = 0;
	switch(key)
	{
		case MPG123_VERBOSE:
			if(val) *val = mp->verbose;
		break;
		case MPG123_FLAGS:
		case MPG123_ADD_FLAGS:
			if(val) *val = mp->flags;
		break;
		case MPG123_FORCE_RATE:
			if(val) *val = mp->force_rate;
		break;
		case MPG123_DOWN_SAMPLE:
			if(val) *val = mp->down_sample;
		break;
		case MPG123_RVA:
			if(val) *val = mp->rva;
		break;
		case MPG123_DOWNSPEED:
			if(val) *val = mp->halfspeed;
		break;
		case MPG123_UPSPEED:
			if(val) *val = mp->doublespeed;
		break;
		case MPG123_ICY_INTERVAL:
			if(val) *val = (long)mp->icy_interval;
		break;
		case MPG123_OUTSCALE:
#ifdef FLOATOUT
			if(fval) *fval = mp->outscale;
#else
			if(val) *val = mp->outscale;
#endif
		break;
		default:
			ret = MPG123_BAD_PARAM;
	}
	return ret;
}

int mpg123_eq(mpg123_handle *mh, int channel, int band, double val)
{
	if(mh == NULL) return MPG123_ERR;
	if(band < 0 || band > 31){ mh->err = MPG123_BAD_BAND; return MPG123_ERR; }
	switch(channel)
	{
		case MPG123_LEFT|MPG123_RIGHT:
			mh->equalizer[0][band] = mh->equalizer[1][band] = DOUBLE_TO_REAL(val);
		break;
		case MPG123_LEFT:  mh->equalizer[0][band] = DOUBLE_TO_REAL(val); break;
		case MPG123_RIGHT: mh->equalizer[1][band] = DOUBLE_TO_REAL(val); break;
		default:
			mh->err=MPG123_BAD_CHANNEL;
			return MPG123_ERR;
	}
	mh->have_eq_settings = TRUE;
	return MPG123_OK;
}


/* plain file access, no http! */
int mpg123_open(mpg123_handle *mh, char *path)
{
	mpg123_close(mh);
	frame_reset(mh);
	return open_stream(mh, path, -1);
}

int mpg123_open_fd(mpg123_handle *mh, int fd)
{
	mpg123_close(mh);
	frame_reset(mh);
	return open_stream(mh, NULL, fd);
}

int mpg123_open_feed(mpg123_handle *mh)
{
	mpg123_close(mh);
	frame_reset(mh);
	return open_feed(mh);
}

int decode_update(mpg123_handle *mh)
{
	long native_rate = frame_freq(mh);
	debug("updating decoder structure");
	if(mh->af.rate == native_rate) mh->down_sample = 0;
	else if(mh->af.rate == native_rate>>1) mh->down_sample = 1;
	else if(mh->af.rate == native_rate>>2) mh->down_sample = 2;
	else mh->down_sample = 3; /* flexible (fixed) rate */
	switch(mh->down_sample)
	{
		case 0:
		case 1:
		case 2:
			mh->down_sample_sblimit = SBLIMIT>>(mh->down_sample);
			/* With downsampling I get less samples per frame */
			mh->outblock = sizeof(sample_t)*mh->af.channels*(spf(mh)>>mh->down_sample);
		break;
		case 3:
		{
			if(synth_ntom_set_step(mh) != 0) return -1;
			if(frame_freq(mh) > mh->af.rate)
			{
				mh->down_sample_sblimit = SBLIMIT * mh->af.rate;
				mh->down_sample_sblimit /= frame_freq(mh);
			}
			else mh->down_sample_sblimit = SBLIMIT;
			mh->outblock = sizeof(sample_t) * mh->af.channels *
			               ( ( NTOM_MUL-1+spf(mh)
			                   * (((size_t)NTOM_MUL*mh->af.rate)/frame_freq(mh))
			                 )/NTOM_MUL );
		}
		break;
	}

	if(!(mh->p.flags & MPG123_FORCE_MONO))
	{
		if(mh->af.channels == 1) mh->single = SINGLE_MIX;
		else mh->single = SINGLE_STEREO;
	}
	else mh->single = (mh->p.flags & MPG123_FORCE_MONO)-1;
	if(set_synth_functions(mh) != 0) return -1;;
	init_layer3_stuff(mh);
	init_layer2_stuff(mh);
	do_rva(mh);

	return 0;
}

size_t mpg123_safe_buffer()
{
	return sizeof(sample_t)*2*1152*NTOM_MAX;
}

size_t mpg123_outblock(mpg123_handle *mh)
{
	if(mh != NULL) return mh->outblock;
	else return mpg123_safe_buffer();
}

static int get_next_frame(mpg123_handle *mh)
{
	int change = mh->decoder_change;
	do
	{
		int b;
		/* Decode & discard some frame(s) before beginning. */
		if(mh->to_ignore && mh->num < mh->firstframe && mh->num >= mh->ignoreframe)
		{
			debug1("ignoring frame %li", (long)mh->num);
			/* Decoder structure must be current! decode_update has been called before... */
			(mh->do_layer)(mh); mh->buffer.fill = 0;
			mh->to_ignore = mh->to_decode = FALSE;
		}
		/* Read new frame data; possibly breaking out here for MPG123_NEED_MORE. */
		debug("read frame");
		mh->to_decode = FALSE;
		b = read_frame(mh); /* That sets to_decode only if a full frame was read. */
		debug3("read of frame %li returned %i (to_decode=%i)", mh->num, b, mh->to_decode);
		if(b == MPG123_NEED_MORE) return MPG123_NEED_MORE; /* need another call with data */
		else if(b <= 0)
		{
			/* More sophisticated error control? */
			if(b==0 || mh->rdat.filepos == mh->rdat.filelen)
			{ /* We simply reached the end. */
				mh->track_frames = mh->num + 1;
				return MPG123_DONE;
			}
			else return MPG123_ERR; /* Some real error. */
		}
		/* Now, there should be new data to decode ... and also possibly new stream properties */
		if(mh->header_change > 1)
		{
			debug("big header change");
			change = 1;
		}
	} while(mh->num < mh->firstframe);
	/* When we start actually using the CRC, this could move into the loop... */
	/* A question of semantics ... should I fold start_frame and frame_number into firstframe/lastframe? */
	if(mh->lastframe >= 0 && mh->num > mh->lastframe)
	{
		mh->to_decode = mh->to_ignore = FALSE;
		return MPG123_DONE;
	}
	if(change)
	{
		int b = frame_output_format(mh); /* Select the new output format based on given constraints. */
		if(b < 0) return MPG123_ERR; /* not nice to fail here... perhaps once should add possibility to repeat this step */
		if(decode_update(mh) < 0) return MPG123_ERR; /* dito... */
		mh->decoder_change = 0;
		if(b == 1) mh->new_format = 1; /* Store for later... */
#ifdef GAPLESS
		if(mh->fresh)
		{
			b=0;
			/* Prepare offsets for gapless decoding. */
			frame_gapless_realinit(mh);
			frame_set_frameseek(mh, mh->num);
			mh->fresh = 0;
			/* Could this possibly happen? With a real big gapless offset... */
			if(mh->num < mh->firstframe) b = get_next_frame(mh);
			if(b < 0) return b; /* Could be error, need for more, new format... */
		}
#endif
	}
	return MPG123_OK;
}

/*
	Put _one_ decoded frame into the frame structure's buffer, accessible at the location stored in <audio>, with <bytes> bytes available.
	The buffer contents will be lost on next call to mpg123_decode_frame.
	MPG123_OK -- successfully decoded the frame, you get your output data
	MPg123_DONE -- This is it. End.
	MPG123_ERR -- some error occured...
	MPG123_NEW_FORMAT -- new frame was read, it results in changed output format -> will be decoded on next call
	MPG123_NEED_MORE  -- that should not happen as this function is intended for in-library stream reader but if you force it...
	MPG123_NO_SPACE   -- not enough space in buffer for safe decoding, also should not happen

	num will be updated to the last decoded frame number (may possibly _not_ increase, p.ex. when format changed).
*/
int mpg123_decode_frame(mpg123_handle *mh, off_t *num, unsigned char **audio, size_t *bytes)
{
	if(mh == NULL) return MPG123_ERR;
	if(mh->buffer.size < mh->outblock) return MPG123_NO_SPACE;
	mh->buffer.fill = 0; /* always start fresh */
	*bytes = 0;
	while(TRUE)
	{
		/* decode if possible */
		if(mh->to_decode)
		{
			if(mh->new_format)
			{
				mh->new_format = 0;
				return MPG123_NEW_FORMAT;
			}
			*num = mh->num;
			debug("decoding");
			mh->clip += (mh->do_layer)(mh);
			mh->to_decode = mh->to_ignore = FALSE;
			mh->buffer.p = mh->buffer.data;
#ifdef GAPLESS
			/* This checks for individual samples to skip, for gapless mode or sample-accurate seek. */
			frame_buffercheck(mh);
#endif
			*audio = mh->buffer.p;
			*bytes = mh->buffer.fill;
			return MPG123_OK;
		}
		else
		{
			int b = get_next_frame(mh);
			if(b < 0) return b;
			debug1("got next frame, %i", mh->to_decode);
		}
	}
	return MPG123_ERR;
}

ssize_t mpg123_read(mpg123_handle *mh, unsigned char *out, size_t size, size_t *done)
{
	return mpg123_decode(mh, NULL, 0, out, size, done);
}

/*
	The old picture:
	while(1) {
		len = read(0,buf,16384);
		if(len <= 0)
			break;
		ret = decodeMP3(&mp,buf,len,out,8192,&size);
		while(ret == MP3_OK) {
			write(1,out,size);
			ret = decodeMP3(&mp,NULL,0,out,8192,&size);
		}
	}
*/

int mpg123_decode(mpg123_handle *mh,unsigned char *inmemory, size_t inmemsize, unsigned char *outmemory, size_t outmemsize, size_t *done)
{
	int ret = MPG123_OK;
	*done = 0;
	if(mh == NULL) return MPG123_ERR;
	if(inmemsize > 0)
	if(feed_more(mh, inmemory, inmemsize) == -1) return MPG123_ERR;
	while(ret == MPG123_OK)
	{
		debug3("decode loop, fill %i (%li vs. %li)", mh->buffer.fill, (long)mh->num, (long)mh->firstframe);
		/* Decode a frame that has been read before.
		   This only happens when buffer is empty! */
		if(mh->to_decode)
		{
			if(mh->new_format)
			{
				mh->new_format = 0;
				return MPG123_NEW_FORMAT;
			}
			if(mh->buffer.size - mh->buffer.fill < mh->outblock) return MPG123_NO_SPACE;
			mh->clip += (mh->do_layer)(mh);
			mh->to_decode = mh->to_ignore = FALSE;
			mh->buffer.p = mh->buffer.data;
			debug2("decoded frame %li, got %li samples in buffer", mh->num, mh->buffer.fill / (samples_to_bytes(mh, 1)));
#ifdef GAPLESS
			frame_buffercheck(mh); /* Seek & gapless. */
#endif
		}
		if(mh->buffer.fill) /* Copy (part of) the decoded data to the caller's buffer. */
		{
			/* get what is needed - or just what is there */
			int a = mh->buffer.fill > (outmemsize - *done) ? outmemsize - *done : mh->buffer.fill;
			debug4("buffer fill: %i; copying %i (%i - %i)", mh->buffer.fill, a, outmemsize, *done);
			memcpy(outmemory, mh->buffer.p, a);
			/* less data in frame buffer, less needed, output pointer increase, more data given... */
			mh->buffer.fill -= a;
			outmemory  += a;
			*done += a;
			mh->buffer.p += a;
			if(!(outmemsize > *done)) return ret;
		}
		else /* If we didn't have data, get a new frame. */
		{
			int b = get_next_frame(mh);
			if(b < 0) return b;
		}
	}
	return ret;
}

long mpg123_clip(mpg123_handle *mh)
{
	long ret = 0;
	if(mh != NULL)
	{
		ret = mh->clip;
		mh->clip = 0;
	}
	return ret;
}

/*
	Now, where are we? We need to know the last decoded frame... and what's left of it in buffer.
	The current frame number can mean the last decoded frame or the to-be-decoded frame.
	If mh->to_decode, then mh->num frames have been decoded, the frame mh->num now coming next.
	If not, we have the possibility of mh->num+1 frames being decoded or nothing at all.
	Then, there is firstframe...when we didn't reach it yet, then the next data will come from there.
	mh->num starts with -1
*/
off_t mpg123_tell(mpg123_handle *mh)
{
	if(mh == NULL) return MPG123_ERR;
	if(!mh->to_decode && mh->fresh)
	{
		/* Fresh track, need first frame for basic info. */
		int b = get_next_frame(mh);
		if(b < 0) return b;
	}
	/* Now we have all the info at hand. */
	debug5("tell: %li/%i first %li firstoff %li buffer %lu", (long)mh->num, mh->to_decode, (long)mh->firstframe, (long)mh->firstoff, (unsigned long)mh->buffer.fill);
	if((mh->num < mh->firstframe) || (mh->num == mh->firstframe && mh->to_decode)) return SAMPLE_ADJUST(frame_tell_seek(mh));
	else if(mh->to_decode) return SAMPLE_ADJUST(frame_outs(mh, mh->num) - mh->buffer.fill);
	else return SAMPLE_ADJUST(frame_outs(mh, mh->num+1) - mh->buffer.fill);
}

off_t mpg123_tellframe(mpg123_handle *mh)
{
	if(mh == NULL) return MPG123_ERR;
	if(mh->num < mh->firstframe) return mh->firstframe;
	if(mh->to_decode) return mh->num;
	/* Consider firstoff? */
	return mh->buffer.fill ? mh->num : mh->num + 1;
}

static int do_the_seek(mpg123_handle *mh)
{
	int b;
	off_t fnum = SEEKFRAME(mh);
	mh->buffer.fill = 0;
	if(mh->num < mh->firstframe) mh->to_decode = FALSE;
	if(mh->num == fnum && mh->to_decode) return MPG123_OK;
	if(mh->num == fnum-1)
	{
		mh->to_decode = FALSE;
		return MPG123_OK;
	}
	/*frame_buffers_reset(mh);*/
	b = mh->rd->seek_frame(mh, fnum);
	if(b<0) return b;
	/* Only mh->to_ignore is TRUE. */
	if(mh->num < mh->firstframe) mh->to_decode = FALSE;
	return 0;
}

off_t mpg123_seek(mpg123_handle *mh, off_t sampleoff, int whence)
{
	off_t pos = mpg123_tell(mh); /* adjusted samples */
debug1("pos=%li", (long)pos);
	if(pos < 0) return pos; /* mh == NULL is covered in mpg123_tell() */
	switch(whence)
	{
		case SEEK_CUR: pos += sampleoff; break;
		case SEEK_SET: pos  = sampleoff; break;
		case SEEK_END:
#ifdef GAPLESS
			if(mh->end_os >= 0) pos = SAMPLE_ADJUST(mh->end_os) - sampleoff;
#else
			if(mh->track_frames > 0) pos = SAMPLE_ADJUST(frame_outs(mh, mh->track_frames)) - sampleoff;
#endif
			else
			{
				mh->err = MPG123_NO_SEEK_FROM_END;
				return MPG123_ERR;
			}
		break;
		default: mh->err = MPG123_BAD_WHENCE; return MPG123_ERR;
	}
	if(pos < 0) pos = 0;
	/* pos now holds the wanted sample offset in adjusted samples */
	frame_set_seek(mh, SAMPLE_UNADJUST(pos));
	pos = do_the_seek(mh);
	if(pos < 0) return pos;

	return mpg123_tell(mh);
}

/*
	A bit more tricky... libmpg123 does not do the seeking itself.
	All it can do is to ignore frames until the wanted one is there.
	The caller doesn't know where a specific frame starts and mpg123 also only knows the general region after it scanned the file.
	Well, it is tricky...
*/
off_t mpg123_feedseek(mpg123_handle *mh, off_t sampleoff, int whence, off_t *input_offset)
{
	off_t pos = mpg123_tell(mh); /* adjusted samples */
	debug3("seek from %li to %li (whence=%i)", (long)pos, (long)sampleoff, whence);
	if(pos < 0) return pos; /* mh == NULL is covered in mpg123_tell() */
	switch(whence)
	{
		case SEEK_CUR: pos += sampleoff; break;
		case SEEK_SET: pos  = sampleoff; break;
		case SEEK_END:
#ifdef GAPLESS
			if(mh->end_os >= 0) pos = SAMPLE_ADJUST(mh->end_os) - sampleoff;
#else
			if(mh->track_frames > 0) pos = SAMPLE_ADJUST(frame_outs(mh, mh->track_frames)) - sampleoff;
#endif
			else
			{
				mh->err = MPG123_NO_SEEK_FROM_END;
				return MPG123_ERR;
			}
		break;
		default: mh->err = MPG123_BAD_WHENCE; return MPG123_ERR;
	}
	if(pos < 0) pos = 0;
	frame_set_seek(mh, SAMPLE_UNADJUST(pos));
	pos = SEEKFRAME(mh);
	mh->buffer.fill = 0;

	/* Shortcuts without modifying input stream. */
	*input_offset = mh->rdat.firstpos + mh->rdat.filelen;
	if(mh->num < mh->firstframe) mh->to_decode = FALSE;
	if(mh->num == pos && mh->to_decode) goto feedseekend;
	if(mh->num == pos-1) goto feedseekend;
	/* Whole way. */
	*input_offset = feed_set_pos(mh, frame_index_find(mh, SEEKFRAME(mh), &pos));
	mh->num = pos-1; /* The next read frame will have num = pos. */
	if(*input_offset < 0) return MPG123_ERR;

feedseekend:
	return mpg123_tell(mh);
}


off_t mpg123_seek_frame(mpg123_handle *mh, off_t offset, int whence)
{
	off_t pos = 0;
	if(mh == NULL) return MPG123_ERR;
	if(!mh->to_decode && mh->fresh)
	{
		/* Fresh track, need first frame for basic info. */
		int b = get_next_frame(mh);
		if(b < 0) return b;
	}
	/* Could play games here with to_decode... */
	pos = mh->num;
	switch(whence)
	{
		case SEEK_CUR: pos += offset; break;
		case SEEK_SET: pos  = offset; break;
		case SEEK_END:
			if(mh->track_frames > 0) pos = mh->track_frames - offset;
			else
			{
				mh->err = MPG123_NO_SEEK_FROM_END;
				return MPG123_ERR;
			}
		break;
		default:
			mh->err = MPG123_BAD_WHENCE;
			return MPG123_ERR;
	}
	if(pos < 0) pos = 0;
	/* Hm, do we need to seek right past the end? */
	else if(mh->track_frames > 0 && pos >= mh->track_frames) pos = mh->track_frames;

	frame_set_frameseek(mh, pos);
	pos = do_the_seek(mh);
	if(pos < 0) return pos;

	return mpg123_tellframe(mh);
}

int mpg123_meta_check(mpg123_handle *mh)
{
	if(mh != NULL) return mh->metaflags;
	else return 0;
}

int mpg123_id3(mpg123_handle *mh, mpg123_id3v1 **v1, mpg123_id3v2 **v2)
{
	if(v1 != NULL) *v1 = NULL;
	if(v2 != NULL) *v2 = NULL;
	if(mh == NULL) return MPG123_ERR;

	if(mh->metaflags & MPG123_ID3)
	{
		if(v1 != NULL && mh->rdat.flags & READER_ID3TAG) *v1 = (mpg123_id3v1*) mh->id3buf;
		if(v2 != NULL) *v2 = &mh->id3v2;
		mh->metaflags |= MPG123_ID3;
		mh->metaflags &= ~MPG123_NEW_ID3;
	}
	return MPG123_OK;
}

int mpg123_icy(mpg123_handle *mh, char **icy_meta)
{
	*icy_meta = NULL;
	if(mh == NULL) return MPG123_ERR;

	if(mh->metaflags & MPG123_ICY)
	{
		*icy_meta = mh->icy.data;
		mh->metaflags |= MPG123_ICY;
		mh->metaflags &= ~MPG123_NEW_ICY;
	}
	return MPG123_OK;
}

int mpg123_close(mpg123_handle *mh)
{
	if(mh == NULL) return MPG123_ERR;
	if(mh->rd != NULL && mh->rd->close != NULL) mh->rd->close(mh);
	mh->rd = NULL;
	return MPG123_OK;
}

void mpg123_delete(mpg123_handle *mh)
{
	if(mh != NULL)
	{
		mpg123_close(mh);
		frame_exit(mh); /* free buffers in frame */
		free(mh); /* free struct; cast? */
	}
}

static const char *mpg123_error[] =
{
	"No error... (code 0)",
	"Unable to set up output format! (code 1)",
	"Invalid channel number specified. (code 2)",
	"Invalid sample rate specified. (code 3)",
	"Unable to allocate memory for 16 to 8 converter table! (code 4)",
	"Bad parameter id! (code 5)",
	"Bad buffer given -- invalid pointer or too small size. (code 6)",
	"Out of memory -- some malloc() failed, (code 7)",
	"You didn't initialize the library! (code 8)",
	"Invalid decoder choice. (code 9)",
	"Invalid mpg123 handle. (code 10)",
	"Unable to initialize frame buffers (out of memory?)! (code 11)",
	"Invalid RVA mode. (code 12)",
	"This build doesn't support gapless decoding. (code 13)"
	"Not enough buffer space. (code 14)",
	"Incompatible numeric data types. (code 15)",
	"Bad equalizer band. (code 16)",
	"Null pointer given where valid storage address needed. (code 17)",
	"Some problem reading the stream. (code 18)",
	"Cannot seek from end (end is not known). (code 19)",
	"Invalid \"whence\" for seek function. (code 20)",
	"Build does not support stream timeouts. (code 21)"
};

const char* mpg123_plain_strerror(int errcode)
{
	if(errcode >= 0 && errcode < sizeof(mpg123_error)/sizeof(char*))
	return mpg123_error[errcode];
	else return "I have no idea - an unknown error code!";
}

int mpg123_errcode(mpg123_handle *mh)
{
	if(mh != NULL) return mh->err;
	return MPG123_BAD_HANDLE;
}

const char* mpg123_strerror(mpg123_handle *mh)
{
	return mpg123_plain_strerror(mpg123_errcode(mh));
}