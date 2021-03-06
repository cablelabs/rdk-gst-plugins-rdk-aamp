/*
* Copyright 2018 RDK Management
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Library General Public
* License as published by the Free Software Foundation, version 2
* of the license.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Library General Public License for more details.
*
* You should have received a copy of the GNU Library General Public
* License along with this library; if not, write to the
* Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
* Boston, MA 02110-1301, USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include "gstaamp.h"
#include "main_aamp.h"
#include "priv_aamp.h"

GST_DEBUG_CATEGORY_STATIC (gst_aamp_debug_category);
#define GST_CAT_DEFAULT gst_aamp_debug_category

#define MAX_BYTES_TO_SEND (188*1024)

#define  GST_AAMP_LOG_TIMING(msg...) GST_FIXME_OBJECT(aamp, msg)

static const gchar *g_aamp_expose_hls_caps = NULL;

static GstStateChangeReturn
gst_aamp_change_state(GstElement * element, GstStateChange transition);
static void gst_aamp_finalize(GObject * object);
static gboolean gst_aamp_query(GstElement * element, GstQuery * query);

static GstFlowReturn gst_aamp_sink_chain(GstPad * pad, GstObject *parent, GstBuffer * buffer);
static gboolean gst_aamp_sink_event(GstPad * pad, GstObject *parent, GstEvent * event);

static gboolean gst_aamp_src_event(GstPad * pad, GstObject *parent, GstEvent * event);
static gboolean gst_aamp_src_query(GstPad * pad, GstObject *parent, GstQuery * query);

static void gst_aamp_configure(GstAamp * aamp, StreamOutputFormat format, StreamOutputFormat audioFormat);
static gboolean gst_aamp_ready(GstAamp *aamp);

#ifdef AAMP_JSCONTROLLER_ENABLED
extern "C"
{
	void setAAMPPlayerInstance(PlayerInstanceAAMP *, int);
	void unsetAAMPPlayerInstance(PlayerInstanceAAMP *);
}
#endif

enum
{
	PROP_0
};

class GstAampStreamer : public StreamSink, public AAMPEventListener
{
public:
	GstAampStreamer(GstAamp * aamp)
	{
		this->aamp = aamp;
		rate = 1.0;
		srcPadCapsSent = true;
		format = FORMAT_INVALID;
		audioFormat = FORMAT_NONE;
		readyToSend = false;
		gst_segment_init(&segment, GST_FORMAT_TIME);
	}

	void Configure(StreamOutputFormat format, StreamOutputFormat audioFormat)
	{
		GST_INFO_OBJECT(aamp, "Enter format = %d audioFormat = %d", format, audioFormat);
		this->format = format;
		this->audioFormat = audioFormat;
		gst_aamp_configure(aamp, format, audioFormat);
	}

	void SendPendingEvents(media_stream* stream, GstClockTime pts)
	{
		if (stream->streamStart)
		{
			GST_INFO_OBJECT(aamp, "sending new_stream_start\n");
			gboolean ret = gst_pad_push_event(stream->srcpad, gst_event_new_stream_start(aamp->stream_id));
			if (!ret)
			{
				GST_ERROR_OBJECT(aamp, "%s: stream start error\n", __FUNCTION__);

			}
			GST_INFO_OBJECT(aamp, "%s: sending caps\n", __FUNCTION__);
			ret = gst_pad_push_event(stream->srcpad, gst_event_new_caps(stream->caps));
			if (!ret)
			{
				GST_ERROR_OBJECT(aamp, "%s: caps evt error\n", __FUNCTION__);
			}
			stream->streamStart = FALSE;
		}
		if (stream->flush)
		{
			gboolean ret = gst_pad_push_event(stream->srcpad, gst_event_new_flush_start());
			if (!ret)
			{
				GST_ERROR_OBJECT(aamp, "%s: flush start error\n", __FUNCTION__);
			}
#ifdef USE_GST1
			GstEvent* event = gst_event_new_flush_stop(FALSE);
#else
			GstEvent* event = gst_event_new_flush_stop();
#endif
			ret = gst_pad_push_event(stream->srcpad, event);
			if (!ret)
			{
				GST_ERROR_OBJECT(aamp, "%s: flush stop error\n", __FUNCTION__);
			}
			stream->flush = FALSE;
		}
		if (stream->resetPosition)
		{
#ifdef USE_GST1
			GstSegment segment;
			gst_segment_init(&segment, GST_FORMAT_TIME);
			segment.start = pts;
			segment.position = 0;
			segment.rate = 1.0;
			segment.applied_rate = rate;
			GST_INFO_OBJECT(aamp, "Sending segment event. start %" G_GUINT64_FORMAT " stop %" G_GUINT64_FORMAT " rate %f\n", segment.start, segment.stop, segment.rate);
			GstEvent* event = gst_event_new_segment (&segment);
#else
			GstEvent* event = gst_event_new_new_segment(FALSE, 1.0, GST_FORMAT_TIME, pts, GST_CLOCK_TIME_NONE, 0);
#endif
			if (!gst_pad_push_event(stream->srcpad, event))
			{
				GST_ERROR_OBJECT(aamp, "%s: gst_pad_push_event segment error\n", __FUNCTION__);
			}
			stream->resetPosition = FALSE;
		}
		stream->eventsPending = FALSE;
	}

	void Send(MediaType mediaType, const void *ptr, size_t len0, double fpts, double fdts, double fDuration)
	{
		GstPad* srcpad = NULL;
		gboolean discontinuity = FALSE;

#ifdef AAMP_DISCARD_AUDIO_TRACK
		if (mediaType == eMEDIATYPE_AUDIO)
		{
			GST_WARNING_OBJECT(aamp, "Discard audio track- not sending data\n");
			return;
		}
#endif

		const char* mediaTypeStr = (mediaType==eMEDIATYPE_AUDIO)?"eMEDIATYPE_AUDIO":"eMEDIATYPE_VIDEO";
		GST_INFO_OBJECT(aamp, "Enter len = %d fpts %f mediaType %s", (int)len0, fpts, mediaTypeStr);
		if (!readyToSend)
		{
			if (!gst_aamp_ready(aamp))
			{
				GST_WARNING_OBJECT(aamp, "Not ready to consume data type %s\n", mediaTypeStr);
				return;
			}
			readyToSend = true;
		}
		media_stream* stream = &aamp->stream[mediaType];
		srcpad = stream->srcpad;
		if (!srcpad)
		{
			GST_WARNING_OBJECT(aamp, "Pad NULL mediaType: %s (%d)  len = %d fpts %f\n", mediaTypeStr, mediaType, (int)len0, fpts);
			return;
		}

		GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
		GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
//#define TRACE_PTS_TRACK 0xff
#ifdef TRACE_PTS_TRACK
		if (( mediaType == TRACE_PTS_TRACK ) || (0xFF == TRACE_PTS_TRACK))
		{
			printf("%s : fpts %f pts %llu", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", fpts, (unsigned long long)pts);
			GstClock *clock = GST_ELEMENT_CLOCK(aamp);
			if (clock)
			{
				GstClockTime curr = gst_clock_get_time(clock);
				printf(" provided clock time %lu diff (pts - curr) %lu (%lu ms)", (unsigned long)curr, (unsigned long)(pts-curr), (unsigned long)(pts-curr)/GST_MSECOND);
			}
			logprintf("\n");
		}
#endif

		if(stream->eventsPending)
		{
			SendPendingEvents(stream , pts);
			discontinuity = TRUE;

		}
#ifdef GSTAAMP_DUMP_STREAM
		static FILE* fp[2] = {NULL,NULL};
		static char filename[128];
		if (!fp[mediaType])
		{
			sprintf(filename, "gstaampdump%d.ts",mediaType );
			fp[mediaType] = fopen(filename, "w");
		}
		fwrite(ptr, 1, len0, fp[mediaType] );
#endif

		while (aamp->player_aamp->aamp->DownloadsAreEnabled())
		{
			size_t len = len0;
			if (len > MAX_BYTES_TO_SEND)
			{
				len = MAX_BYTES_TO_SEND;
			}
#ifdef USE_GST1
			GstBuffer *buffer = gst_buffer_new_allocate(NULL, (gsize) len, NULL);
			GstMapInfo map;
			gst_buffer_map(buffer, &map, GST_MAP_WRITE);
			memcpy(map.data, ptr, len);
			gst_buffer_unmap(buffer, &map);
			GST_BUFFER_PTS(buffer) = pts;
			GST_BUFFER_DTS(buffer) = dts;
#else
			GstBuffer *buffer = gst_buffer_new_and_alloc((guint) len);
			memcpy(GST_BUFFER_DATA(buffer), ptr, len);
			GST_BUFFER_TIMESTAMP(buffer) = pts;
#endif
			if (discontinuity)
			{
				GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
				discontinuity = FALSE;
			}
			GstFlowReturn ret;
			ret = gst_pad_push(srcpad, buffer);
			if (ret != GST_FLOW_OK)
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_push error: %s mediaTypeStr %s\n", gst_flow_get_name(ret), mediaTypeStr);
				break;
			}
			ptr = len + (unsigned char *) ptr;
			len0 -= len;
			if (len0 == 0)
			{
				break;
			}
		}
		GST_TRACE_OBJECT(aamp, "Exit");
	}

	void Send(MediaType mediaType, GrowableBuffer* pBuffer, double fpts, double fdts, double fDuration)
	{
		GstPad* srcpad = NULL;
		gboolean discontinuity = FALSE;

#ifdef AAMP_DISCARD_AUDIO_TRACK
		if (mediaType == eMEDIATYPE_AUDIO)
		{
			GST_WARNING_OBJECT(aamp, "Discard audio track- not sending data\n");
			return;
		}
#endif

		const char* mediaTypeStr = (mediaType == eMEDIATYPE_AUDIO) ? "eMEDIATYPE_AUDIO" : "eMEDIATYPE_VIDEO";
		GST_INFO_OBJECT(aamp, "Enter len = %d fpts %f mediaType %s", (int) pBuffer->len, fpts, mediaTypeStr);
		if (!readyToSend)
		{
			if (!gst_aamp_ready(aamp))
			{
				GST_WARNING_OBJECT(aamp, "Not ready to consume data type %s\n", mediaTypeStr);
				return;
			}
			readyToSend = true;
		}
		media_stream* stream = &aamp->stream[mediaType];
		srcpad = stream->srcpad;
		if (!srcpad)
		{
			GST_WARNING_OBJECT(aamp, "Pad NULL mediaType: %s (%d)  len = %d fpts %f\n", mediaTypeStr, mediaType,
			        (int) pBuffer->len, fpts);
			return;
		}

		GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
		GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
//#define TRACE_PTS_TRACK 0xff
#ifdef TRACE_PTS_TRACK
		if (( mediaType == TRACE_PTS_TRACK ) || (0xFF == TRACE_PTS_TRACK))
		{
			printf("%s : fpts %f pts %llu", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", fpts, (unsigned long long)pts);
			GstClock *clock = GST_ELEMENT_CLOCK(aamp);
			if (clock)
			{
				GstClockTime curr = gst_clock_get_time(clock);
				printf(" provided clock time %lu diff (pts - curr) %lu (%lu ms)", (unsigned long)curr, (unsigned long)(pts-curr), (unsigned long)(pts-curr)/GST_MSECOND);
			}
			printf("\n");
		}
#endif

		if (stream->eventsPending)
		{
			SendPendingEvents(stream, pts);
			discontinuity = TRUE;
		}
#ifdef GSTAAMP_DUMP_STREAM
		static FILE* fp[2] =
		{	NULL,NULL};
		static char filename[128];
		if (!fp[mediaType])
		{
			sprintf(filename, "gstaampdump%d.ts",mediaType );
			fp[mediaType] = fopen(filename, "w");
		}
		fwrite(buffer->ptr, 1, buffer->len, fp[mediaType] );
#endif

		if (aamp->player_aamp->aamp->DownloadsAreEnabled())
		{
#ifdef USE_GST1
			GstBuffer* buffer = gst_buffer_new_wrapped (pBuffer->ptr ,pBuffer->len);
			GST_BUFFER_PTS(buffer) = pts;
			GST_BUFFER_DTS(buffer) = dts;
#else
			GstBuffer* buffer = gst_buffer_new();
			GST_BUFFER_SIZE(buffer) = pBuffer->len;
			GST_BUFFER_MALLOCDATA(buffer) = pBuffer->ptr;
			GST_BUFFER_DATA (buffer) = GST_BUFFER_MALLOCDATA(buffer)
			GST_BUFFER_TIMESTAMP(buffer) = pts;
			GST_BUFFER_DURATION(buffer) = duration;
#endif
			if (discontinuity)
			{
				GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
				discontinuity = FALSE;
			}
			GstFlowReturn ret;
			ret = gst_pad_push(srcpad, buffer);
			if (ret != GST_FLOW_OK)
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_push error: %s mediaTypeStr %s\n", gst_flow_get_name(ret),
				        mediaTypeStr);
			}
		}
		/*Since ownership of buffer is given to gstreamer, reset pBuffer */
		memset(pBuffer, 0x00, sizeof(GrowableBuffer));
		GST_TRACE_OBJECT(aamp, "Exit");
	}

	void UpdateRate(gdouble rate)
	{
		if ( rate != this->rate)
		{
			this->rate = rate;
		}
	}
	void EndOfStreamReached(MediaType type)
	{
		GstEvent* event = gst_event_new_eos();
		if (NULL != aamp->stream[type].srcpad)
		{
			if (!gst_pad_push_event(aamp->stream[type].srcpad, gst_event_ref(event)))
			{
				GST_ERROR_OBJECT(aamp, "Send EOS failed for type:%d\n", type);
			}
		}
	}
	void EOS()
	{
		GstEvent* event = gst_event_new_eos();
		if (NULL != aamp->stream[eMEDIATYPE_AUDIO].srcpad)
		{
			if (!gst_pad_push_event(aamp->stream[eMEDIATYPE_AUDIO].srcpad, gst_event_ref(event)))
			{
				GST_ERROR_OBJECT(aamp, "Send EOS failed\n");
			}
		}
		if (!gst_pad_push_event(aamp->stream[eMEDIATYPE_VIDEO].srcpad, event))
		{
			GST_ERROR_OBJECT(aamp, "Send EOS failed\n");
		}
	}
	bool Discontinuity(MediaType mediaType)
	{
		aamp->stream[mediaType].resetPosition = TRUE;
		aamp->stream[mediaType].eventsPending = TRUE;
		return false;
	}
	void Flush(double position, float rate)
	{
		for (int i = 0; i < AAMP_TRACK_COUNT; i++)
		{
			aamp->stream[i].resetPosition = TRUE;
			aamp->stream[i].flush = TRUE;
			aamp->stream[i].eventsPending = TRUE;
		}
	}
	void Event(const AAMPEvent& event);
private:
	GstAamp * aamp;
	GstSegment segment;
	gdouble rate;
	bool srcPadCapsSent;
	StreamOutputFormat format;
	StreamOutputFormat audioFormat;
	bool readyToSend;
};

#define AAMP_TYPE_INIT_CODE { \
	GST_DEBUG_CATEGORY_INIT (gst_aamp_debug_category, "aamp", 0, \
		"debug category for aamp element"); \
	}
static GstStaticPadTemplate gst_aamp_sink_template_hls = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
		GST_STATIC_CAPS("application/x-hls;"
						"application/x-aamp;"));
static GstStaticPadTemplate gst_aamp_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
		GST_STATIC_CAPS("application/x-aamp;"));

#define AAMP_SRC_CAPS_STR "video/mpegts, " \
        "  systemstream=(boolean)true, "\
        "  packetsize=(int)188;"
#define AAMP_SRC_AUDIO_CAPS_STR \
    "audio/mpeg, " \
      "mpegversion = (int) 1;" \
    "audio/mpeg, " \
      "mpegversion = (int) 2, " \
      "stream-format = (string) adts; " \
    "audio/x-ac3; audio/x-eac3;"

static GstStaticPadTemplate gst_aamp_src_template_video =
    GST_STATIC_PAD_TEMPLATE ("video_%02x",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
		GST_STATIC_CAPS(AAMP_SRC_CAPS_STR));

static GstStaticPadTemplate gst_aamp_src_template_audio =
    GST_STATIC_PAD_TEMPLATE ("audio_%02x",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
		GST_STATIC_CAPS(AAMP_SRC_AUDIO_CAPS_STR));

/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstAamp, gst_aamp, GST_TYPE_ELEMENT, AAMP_TYPE_INIT_CODE);

#ifdef AAMP_CC_ENABLED
// initialize cc
gpointer aamp_cc_handler_func(gpointer data)
{
	//Retrieve CC handle
	GstAamp *aamp = (GstAamp *) data;
	GstStructure *structure;
	GstQuery *query;
	const GValue *val;
	gboolean ret;
	int ccStatus;

	GST_DEBUG_OBJECT(aamp, "Enter aamp_cc_handler_func \n");

	if (!aamp->stream[eMEDIATYPE_VIDEO].srcpad)
	{
		GST_DEBUG_OBJECT(aamp, "No src pad available with AAMP plugin for CC start\n");
		aamp->quit_cc_handler = TRUE;
		return NULL;
	}

	structure = gst_structure_new("get_video_handle", "video_handle", G_TYPE_POINTER, 0, NULL);
#ifdef USE_GST1
	query = gst_query_new_custom(GST_QUERY_CUSTOM, structure);
#else
	query = gst_query_new_application(GST_QUERY_CUSTOM, structure);
#endif
	ret = gst_pad_peer_query(aamp->stream[eMEDIATYPE_VIDEO].srcpad, query);
	if (ret)
	{
		GST_DEBUG_OBJECT(aamp, "Video decoder handle queried successfully\n");
		structure = (GstStructure *) gst_query_get_structure(query);
		val = gst_structure_get_value(structure, "video_handle");

		if (val == NULL)
		{
			GST_ERROR_OBJECT(aamp, "Unable to retrieve video decoder handle from structure\n");
			gst_query_unref(query);
			aamp->quit_cc_handler = TRUE;
			return NULL;
		}

		aamp->video_decode_handle = g_value_get_pointer(val);
		GST_DEBUG_OBJECT(aamp, "video decoder handle: %x\n", aamp->video_decode_handle);
	}
	else
	{
		GST_ERROR_OBJECT(aamp, "Video decoder handle query failed \n");
		gst_query_unref(query);
		return NULL;
	}

	gst_query_unref(query);

	ccStatus = aamp_CCStart(aamp->video_decode_handle);
	if (ccStatus != 0)
	{
		GST_ERROR_OBJECT(aamp, "Unable to initialize CC module \n");
		aamp->quit_cc_handler = TRUE;
		return NULL;
	}
	else
	{
		ccStatus = aamp_CCShow();
	}

	while (aamp->quit_cc_handler == FALSE)
	{
		g_usleep(5000);
	}

	GST_DEBUG_OBJECT(aamp, "Shutting down CC module \n");
	aamp_CCHide();
	aamp_CCStop();

	return NULL;
}

// video decoder handle is available after brcmvideodecoder instantiated
static void gst_aamp_cc_start(GstAamp * aamp)
{
	GST_DEBUG_OBJECT(aamp, "Enter gst_aamp_cc_start \n");
	//Check if CC disabled in configuration
	if (!aamp_IsCCEnabled())
	{
		GST_DEBUG_OBJECT(aamp, "CC disabled via aamp.cfg or aamp options");
		return;
	}

	//Check if CC was already invoked
	if (aamp->cc_handler_id == NULL)
	{
		aamp->quit_cc_handler = FALSE;
#ifdef USE_GST1
		aamp->cc_handler_id = g_thread_new("aamp_cc_handler", aamp_cc_handler_func, aamp);
#else
		aamp->cc_handler_id = g_thread_create("aamp_cc_handler", aamp_cc_handler_func, TRUE, aamp);
#endif
		if (aamp->cc_handler_id == NULL)
		{
			aamp->quit_cc_handler = TRUE;
			GST_ERROR_OBJECT(aamp, "Failed to start CC handler thread \n");
		}
	}

	GST_DEBUG_OBJECT(aamp, "Exit gst_aamp_cc_start \n");
}

static void gst_aamp_cc_stop(GstAamp * aamp)
{
	GST_DEBUG_OBJECT(aamp, "Enter gst_aamp_cc_stop \n");

	//Check if CC disabled in configuration.
	if (!aamp_IsCCEnabled() && aamp->cc_handler_id == NULL)
	{
		GST_DEBUG_OBJECT(aamp, "CC disabled via aamp.cfg or aamp options");
		return;
	}

	aamp->quit_cc_handler = TRUE;

	if (aamp->cc_handler_id != NULL)
	{
		g_thread_join(aamp->cc_handler_id);
		aamp->cc_handler_id = NULL;
	}

	GST_DEBUG_OBJECT(aamp, "Exit gst_aamp_cc_stop \n");
}

#endif // AAMP_CC_ENABLED

static void gst_aamp_update_audio_src_pad(GstAamp * aamp)
{
#ifndef AAMP_DISCARD_AUDIO_TRACK
	if (NULL != aamp->stream[eMEDIATYPE_AUDIO].srcpad)
	{
		gboolean enable_audio;
		if ( aamp->rate != 1.0F)
		{
			enable_audio = FALSE;
		}
		else
		{
			enable_audio = TRUE;
		}
		if (enable_audio && !aamp->audio_enabled)
		{
			GST_INFO_OBJECT(aamp, "Enable aud and add pad");
			if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_AUDIO].srcpad, TRUE))
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_set_active failed");
			}
			if (FALSE == gst_element_add_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_AUDIO].srcpad))
			{
				GST_WARNING_OBJECT(aamp, "gst_element_add_pad stream[eMEDIATYPE_AUDIO].srcpad failed");
			}
			aamp->stream[eMEDIATYPE_AUDIO].streamStart = TRUE;
			aamp->stream[eMEDIATYPE_AUDIO].eventsPending = TRUE;
			aamp->audio_enabled = TRUE;
		}
		else if (!enable_audio && aamp->audio_enabled)
		{
			GST_INFO_OBJECT(aamp, "Disable aud and remove pad");
			if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_AUDIO].srcpad, FALSE))
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_set_active FALSE failed");
			}
			if (FALSE == gst_element_remove_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_AUDIO].srcpad))
			{
				GST_WARNING_OBJECT(aamp, "gst_element_remove_pad stream[eMEDIATYPE_AUDIO].srcpad failed");
			}
			aamp->audio_enabled = FALSE;
		}
	}
#endif
}

static GstCaps* GetGstCaps(StreamOutputFormat format)
{
	GstCaps * caps = NULL;
	switch (format)
	{
		case FORMAT_MPEGTS:
			caps = gst_caps_new_simple ("video/mpegts",
					"systemstream", G_TYPE_BOOLEAN, TRUE,
					"packetsize", G_TYPE_INT, 188, NULL);
			break;
		case FORMAT_ISO_BMFF:
			caps = gst_caps_new_simple("video/quicktime", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_AAC:
			caps = gst_caps_new_simple ("audio/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"stream-format", G_TYPE_STRING, "adts", NULL);
			break;
		case FORMAT_AUDIO_ES_AC3:
			caps = gst_caps_new_simple ("audio/ac3", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_EC3:
			caps = gst_caps_new_simple ("audio/x-eac3", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_H264:
			caps = gst_caps_new_simple ("video/x-h264", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_HEVC:
			caps = gst_caps_new_simple ("video/x-h265", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_MPEG2:
			caps = gst_caps_new_simple ("video/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
			break;
		case FORMAT_INVALID:
		case FORMAT_NONE:
		default:
			g_warning("Unsupported format %d\n", format);
			break;
	}
	return caps;
}

static void gst_aamp_configure(GstAamp * aamp, StreamOutputFormat format, StreamOutputFormat audioFormat)
{
	GstCaps *caps;
	gchar * padname = NULL;

	g_mutex_lock (&aamp->mutex);
	if ( aamp->state >= GST_AAMP_CONFIGURED )
	{
		gst_aamp_update_audio_src_pad(aamp);
		g_mutex_unlock (&aamp->mutex);
		GST_INFO_OBJECT(aamp, "Already configured");
		return;
	}
	g_mutex_unlock (&aamp->mutex);

	caps = GetGstCaps(format);

	if (caps)
	{
		padname = g_strdup_printf ("video_%02x", 1);
		GstPad *srcpad = gst_pad_new_from_static_template(&gst_aamp_src_template_video, padname);
		gst_object_ref(srcpad);
		gst_pad_use_fixed_caps(srcpad);
		GST_OBJECT_FLAG_SET(srcpad, GST_PAD_FLAG_NEED_PARENT);
		gst_pad_set_query_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_query));
		gst_pad_set_event_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_event));
		aamp->stream[eMEDIATYPE_VIDEO].caps= caps;
		aamp->stream[eMEDIATYPE_VIDEO].srcpad = srcpad;
		if (padname)
		{
			GST_INFO_OBJECT(aamp, "Created pad %s", padname);
		    g_free (padname);
		}
		aamp->stream_id = gst_pad_create_stream_id(srcpad, GST_ELEMENT(aamp), NULL);
	}
	else
	{
		GST_INFO_OBJECT(aamp, "Unsupported videoFormat %d", format);
	}

	caps = GetGstCaps(audioFormat);
	if (caps)
	{
		padname = g_strdup_printf ("audio_%02x", 1);
		GstPad *srcpad = gst_pad_new_from_static_template(&gst_aamp_src_template_audio, padname);
		gst_object_ref(srcpad);
		gst_pad_use_fixed_caps(srcpad);
		GST_OBJECT_FLAG_SET(srcpad, GST_PAD_FLAG_NEED_PARENT);
		gst_pad_set_query_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_query));
		gst_pad_set_event_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_event));
		aamp->stream[eMEDIATYPE_AUDIO].srcpad = srcpad;
		aamp->stream[eMEDIATYPE_AUDIO].caps= caps;
	}

	g_mutex_lock (&aamp->mutex);
	aamp->state = GST_AAMP_CONFIGURED;
	g_cond_signal(&aamp->state_changed);
	g_mutex_unlock (&aamp->mutex);
}

static void gst_aamp_class_init(GstAampClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

	g_aamp_expose_hls_caps = g_getenv ("GST_AAMP_EXPOSE_HLS_CAPS");
	if (g_aamp_expose_hls_caps)
	{
		gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_sink_template_hls));
	}
	else
	{
		gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_sink_template));
	}
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_src_template_audio));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_src_template_video));

	gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "Advanced Adaptive Media Player", "Demux",
			"Advanced Adaptive Media Player", "Comcast");

	gobject_class->finalize = gst_aamp_finalize;
	element_class->change_state = GST_DEBUG_FUNCPTR(gst_aamp_change_state);
	element_class->query = GST_DEBUG_FUNCPTR(gst_aamp_query);
}

static void gst_aamp_init(GstAamp * aamp)
{
	GST_AAMP_LOG_TIMING("Enter\n");
	aamp->location = NULL;
	aamp->rate = 1.0F;
	aamp->audio_enabled = FALSE;
	aamp->state = GST_AAMP_NONE;
	aamp->context = new GstAampStreamer(aamp);
	aamp->player_aamp = new PlayerInstanceAAMP(aamp->context);
	aamp->sinkpad = gst_pad_new_from_static_template(&gst_aamp_sink_template_hls, "sink");
	memset(&aamp->stream[0], 0 , sizeof(aamp->stream));
	aamp->stream_id = NULL;
	aamp->idle_id = 0;

	gst_pad_set_chain_function(aamp->sinkpad, GST_DEBUG_FUNCPTR(gst_aamp_sink_chain));
	gst_pad_set_event_function(aamp->sinkpad, GST_DEBUG_FUNCPTR(gst_aamp_sink_event));
	gst_element_add_pad(GST_ELEMENT(aamp), aamp->sinkpad);
	g_mutex_init (&aamp->mutex);
	g_cond_init (&aamp->state_changed);
	aamp->context->Discontinuity(eMEDIATYPE_VIDEO);
	aamp->context->Discontinuity(eMEDIATYPE_AUDIO);
}

void gst_aamp_finalize(GObject * object)
{
	GstAamp *aamp = GST_AAMP(object);

	if (aamp->location)
	{
		g_free(aamp->location);
		aamp->location = NULL;
	}
	g_mutex_clear (&aamp->mutex);
	delete aamp->context;
	aamp->context=NULL;
	delete aamp->player_aamp;
	g_cond_clear (&aamp->state_changed);

	if (aamp->stream[eMEDIATYPE_AUDIO].caps)
	{
		gst_caps_unref(aamp->stream[eMEDIATYPE_AUDIO].caps);
	}

	if (aamp->stream[eMEDIATYPE_VIDEO].caps)
	{
		gst_caps_unref(aamp->stream[eMEDIATYPE_VIDEO].caps);
	}

	if (aamp->stream[eMEDIATYPE_VIDEO].srcpad)
	{
		gst_object_unref(aamp->stream[eMEDIATYPE_VIDEO].srcpad);
	}

	if (aamp->stream[eMEDIATYPE_AUDIO].srcpad)
	{
		gst_object_unref(aamp->stream[eMEDIATYPE_AUDIO].srcpad);
	}

	if (aamp->stream_id)
	{
		g_free(aamp->stream_id);
	}

	GST_AAMP_LOG_TIMING("Exit\n");
	G_OBJECT_CLASS(gst_aamp_parent_class)->finalize(object);
}

/**
 * @fn void aampClientCallback()
 * @brief This function receives asynchronous events from AAMP
 */
void GstAampStreamer::Event(const AAMPEvent & e )
{
		switch (e.type)
		{
			case AAMP_EVENT_TUNED:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_TUNED");
				break;

			case AAMP_EVENT_TUNE_FAILED:
				GST_WARNING_OBJECT(aamp, "Tune failed");
				g_mutex_lock (&aamp->mutex);
				aamp->state = GST_AAMP_STATE_ERROR;
				g_cond_signal(&aamp->state_changed);
				g_mutex_unlock (&aamp->mutex);
				break;

			case AAMP_EVENT_SPEED_CHANGED:
#ifdef AAMP_CC_ENABLED
				if (aamp_IsCCEnabled())
				{
					if (e.data.speedChanged.rate != 1.0)
					{
						aamp_CCHide();
					}
					else
					{
						aamp_CCShow();
					}
				}
#endif
				break;

			case AAMP_EVENT_EOS:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_EOS");
				aamp->context->EOS();
				break;

			case AAMP_EVENT_PLAYLIST_INDEXED:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_PLAYLIST_INDEXED");
				break;

			case AAMP_EVENT_PROGRESS:
				break;

			case AAMP_EVENT_TIMED_METADATA:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_TIMED_METADATA");
				break;

			default:
				GST_DEBUG_OBJECT(aamp, "unknown event %d\n", e.type);
				break;
		}
}

static gboolean gst_aamp_query_uri(GstAamp *aamp)
{
	gboolean ret = TRUE;
	GstQuery *query = gst_query_new_uri();

	ret = gst_pad_peer_query(aamp->sinkpad, query);
	if (ret)
	{
		gchar *uri;
		gst_query_parse_uri(query, &uri);
		GST_DEBUG_OBJECT(aamp, "uri %s\n", uri);
		if (aamp->location)
		{
			g_free(aamp->location);
		}
		aamp->location = g_strdup(uri);
		g_free(uri);
	}
	return ret;
}

static void gst_aamp_tune_async(GstAamp *aamp)
{
	g_mutex_lock(&aamp->mutex);
	aamp->state = GST_AAMP_TUNING;
	g_mutex_unlock(&aamp->mutex);
	GST_AAMP_LOG_TIMING("Calling aamp->Tune()\n");
	aamp->player_aamp->Tune(aamp->location);
}

static gboolean gst_aamp_configured(GstAamp *aamp)
{
	gboolean ret = FALSE;
	g_mutex_lock(&aamp->mutex);
	if ( aamp->state == GST_AAMP_TUNING )
	{
		g_cond_wait(&aamp->state_changed, &aamp->mutex);
	}
	ret = (aamp->state >= GST_AAMP_CONFIGURED);
	g_mutex_unlock(&aamp->mutex);
	return ret;
}

static gboolean gst_aamp_ready(GstAamp *aamp)
{
	gboolean ret = FALSE;
	g_mutex_lock(&aamp->mutex);
	while (aamp->state < GST_AAMP_READY)
	{
		g_cond_wait(&aamp->state_changed, &aamp->mutex);
	}
	ret = (aamp->state == GST_AAMP_READY);
	g_mutex_unlock(&aamp->mutex);
	return ret;
}

static gboolean gst_aamp_report_on_tune_done(gpointer user_data)
{
	GstAamp *aamp = (GstAamp *) user_data;
	GstElement *pbin = GST_ELEMENT(aamp);
	while (GST_ELEMENT_PARENT(pbin))
	{
		pbin = GST_ELEMENT_PARENT(pbin);
	}
	if (GST_STATE(pbin) == GST_STATE_PLAYING)
	{
		GST_AAMP_LOG_TIMING("LogTuneComplete()");
		aamp->player_aamp->aamp->LogTuneComplete();
		aamp->idle_id = 0;
		return G_SOURCE_REMOVE;
	}
	else
	{
		GST_DEBUG_OBJECT(aamp, "Pipeline Not yet playing");
		return G_SOURCE_CONTINUE;
	}
}

static GstStateChangeReturn gst_aamp_change_state(GstElement * element, GstStateChange trans)
{
	GstAamp *aamp;
	GstStateChangeReturn ret;

	aamp = GST_AAMP(element);
	GST_DEBUG_OBJECT(aamp, "Enter");

	switch (trans)
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_NULL_TO_READY");
			aamp->player_aamp->RegisterEvents(aamp->context);
			if ( FALSE == gst_aamp_query_uri( aamp) )
			{
				return GST_STATE_CHANGE_FAILURE;
			}
#ifdef AAMP_JSCONTROLLER_ENABLED
			{
				int sessionId = 0;
				if (aamp->location)
				{
					char *sessionValue = strstr(aamp->location, "?sessionId=");
					if (sessionValue != NULL)
					{
						sscanf(sessionValue + 1, "sessionId=%d", &sessionId);
					}
				}
				setAAMPPlayerInstance(aamp->player_aamp, sessionId);
			}
#endif
			gst_aamp_tune_async( aamp);
			aamp->report_tune = TRUE;
			aamp->player_aamp->aamp->ResumeTrackDownloads(eMEDIATYPE_VIDEO);
			aamp->player_aamp->aamp->ResumeTrackDownloads(eMEDIATYPE_AUDIO);
			break;

		case GST_STATE_CHANGE_READY_TO_PAUSED:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_READY_TO_PAUSED\n");
			g_mutex_lock (&aamp->mutex);
			if (NULL != aamp->stream[eMEDIATYPE_VIDEO].srcpad)
			{
				if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_VIDEO].srcpad, TRUE))
				{
					GST_WARNING_OBJECT(aamp, "gst_pad_set_active failed");
				}
				if (FALSE == gst_element_add_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_VIDEO].srcpad))
				{
					GST_WARNING_OBJECT(aamp, "gst_element_add_pad srcpad failed");
				}
				aamp->stream[eMEDIATYPE_VIDEO].streamStart = TRUE;
				aamp->stream[eMEDIATYPE_VIDEO].eventsPending = TRUE;
			}
			gst_aamp_update_audio_src_pad(aamp);
			aamp->state = GST_AAMP_READY;
			g_cond_signal(&aamp->state_changed);
			g_mutex_unlock (&aamp->mutex);
			gst_element_no_more_pads (element);
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
#ifdef AAMP_CC_ENABLED
			gst_aamp_cc_start(aamp);
#endif
			if (aamp->report_tune)
			{
				aamp->idle_id = g_timeout_add(50, gst_aamp_report_on_tune_done, aamp);
				aamp->report_tune = FALSE;
			}
			break;

		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(gst_aamp_parent_class)->change_state(element, trans);
	if (ret == GST_STATE_CHANGE_FAILURE)
	{
		GST_ERROR_OBJECT(aamp, "Parent state change failed\n");
		return ret;
	}
	else
	{
		GST_DEBUG_OBJECT(aamp, "Parent state change :  %s\n", gst_element_state_change_return_get_name(ret));
	}

	switch (trans)
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			if (aamp->idle_id)
			{
				g_source_remove(aamp->idle_id);
				aamp->idle_id = 0;
			}
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_PLAYING_TO_PAUSED");
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_PAUSED_TO_READY");
			g_mutex_lock(&aamp->mutex);
			aamp->state = GST_AAMP_SHUTTING_DOWN;
			g_cond_signal(&aamp->state_changed);
			g_mutex_unlock(&aamp->mutex);
			aamp->player_aamp->Stop();
#ifdef AAMP_CC_ENABLED
			gst_aamp_cc_stop(aamp);
#endif
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_READY_TO_NULL");
			aamp->player_aamp->RegisterEvents(NULL);
#ifdef AAMP_JSCONTROLLER_ENABLED
			unsetAAMPPlayerInstance(aamp->player_aamp);
#endif
			break;
		case GST_STATE_CHANGE_NULL_TO_READY:
			if (!gst_aamp_configured(aamp))
			{
				GST_ERROR_OBJECT(aamp, "Not configured");
				return GST_STATE_CHANGE_FAILURE;
			}
			else
			{
				GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_NULL_TO_READY Complete");
			}
			break;
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			if (aamp->player_aamp->aamp->IsLive())
			{
				GST_INFO_OBJECT(aamp, "LIVE stream");
				ret = GST_STATE_CHANGE_NO_PREROLL;
			}
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_READY_TO_PAUSED");
			break;
		default:
			break;
	}
	GST_DEBUG_OBJECT(aamp, "Exit");

	return ret;
}

static gboolean gst_aamp_query(GstElement * element, GstQuery * query)
{
	GstAamp *aamp = GST_AAMP(element);
	gboolean ret = FALSE;

	GST_DEBUG_OBJECT(aamp, " query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_POSITION:
		{
			GstFormat format;

			gst_query_parse_position(query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gst_query_set_position(query, GST_FORMAT_TIME, (aamp->player_aamp->aamp->GetPositionMs()*GST_MSECOND));
				ret = TRUE;
			}
			break;
		}

		case GST_QUERY_DURATION:
		{
			GstFormat format;
			gst_query_parse_duration (query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 duration = aamp->player_aamp->aamp->GetDurationMs()*GST_MSECOND;
				gst_query_set_duration (query, format, duration);
				GST_TRACE_OBJECT(aamp, " GST_QUERY_DURATION returning duration %" G_GUINT64_FORMAT "\n", duration);
				ret = TRUE;
			}
			else
			{
				const GstFormatDefinition* def =  gst_format_get_details(format);
				GST_WARNING_OBJECT(aamp, " GST_QUERY_DURATION format %s %s\n", def->nick, def->description);
			}
			break;
		}

		case GST_QUERY_SCHEDULING:
		{
			gst_query_set_scheduling (query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
			ret = TRUE;
			break;
		}

		default:
			break;
	}

	if (FALSE == ret )
	{
		ret = GST_ELEMENT_CLASS(gst_aamp_parent_class)->query(element, query);
	}
	else
	{
		gst_query_unref(query);
	}
	return ret;
}

static GstFlowReturn gst_aamp_sink_chain(GstPad * pad, GstObject *parent, GstBuffer * buffer)
{
	GstAamp *aamp;

	aamp = GST_AAMP(parent);
	GST_DEBUG_OBJECT(aamp, "chain");
	return GST_FLOW_OK;
}

static gboolean gst_aamp_sink_event(GstPad * pad, GstObject *parent, GstEvent * event)
{
	gboolean res = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_DEBUG_OBJECT(aamp, " EVENT %s\n", gst_event_type_get_name(GST_EVENT_TYPE(event)));

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_EOS:
			GST_WARNING_OBJECT(aamp, "sink pad : Got EOS\n" );
			gst_event_unref(event);
			res = TRUE;
			break;
#ifdef AAMP_SEND_SEGMENT_EVENTS
		case GST_EVENT_SEGMENT:
			GST_INFO_OBJECT(aamp, "sink pad : Got Segment\n" );
			gst_event_unref(event);
			res = TRUE;
			break;
#endif
		default:
			res = gst_pad_event_default(pad, parent, event);
			break;
	}
	return res;
}

static gboolean gst_aamp_src_query(GstPad * pad, GstObject *parent, GstQuery * query)
{
	gboolean ret = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_TRACE_OBJECT(aamp, " query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_CAPS:
		{
			GstCaps* caps;
			if(aamp->stream[eMEDIATYPE_VIDEO].srcpad == pad )
			{
				caps = aamp->stream[eMEDIATYPE_VIDEO].caps;
			}
			else if(aamp->stream[eMEDIATYPE_AUDIO].srcpad == pad )
			{
				caps = aamp->stream[eMEDIATYPE_AUDIO].caps;
			}
			else
			{
				GST_WARNING_OBJECT(aamp, "Unknown pad %p", pad);
			}
			gst_query_set_caps_result(query, caps);
			ret = TRUE;
			break;
		}
		case GST_QUERY_POSITION:
		{
			GstFormat format;

			gst_query_parse_position(query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 posMs = aamp->player_aamp->aamp->GetPositionMs();
				GST_TRACE_OBJECT(aamp, " GST_QUERY_POSITION position %" G_GUINT64_FORMAT " seconds\n", posMs/1000);
				gst_query_set_position(query, GST_FORMAT_TIME, (posMs*GST_MSECOND ));
				ret = TRUE;
			}
			break;
		}

		case GST_QUERY_DURATION:
		{
			GstFormat format;
			gst_query_parse_duration (query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 duration = aamp->player_aamp->aamp->GetDurationMs()*GST_MSECOND;
				gst_query_set_duration (query, format, duration);
				GST_TRACE_OBJECT(aamp, " GST_QUERY_DURATION returning duration %" G_GUINT64_FORMAT "\n", duration);
				ret = TRUE;
			}
			else
			{
				const GstFormatDefinition* def =  gst_format_get_details(format);
				GST_WARNING_OBJECT(aamp, " GST_QUERY_DURATION format %s %s\n", def->nick, def->description);
			}
			break;
		}
		case GST_QUERY_SCHEDULING:
		{
			gst_query_set_scheduling (query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
			ret = TRUE;
			break;
		}

		case GST_QUERY_CUSTOM:
		{
		//g_print("\n\n\nReceived custom event\n\n\n");
			GstStructure *structure = gst_query_writable_structure(query);
			if (structure && gst_structure_has_name(structure, "get_aamp_instance"))
			{
				GValue val = { 0, };
				g_value_init(&val, G_TYPE_POINTER);
				g_value_set_pointer(&val, (gpointer) aamp->player_aamp->aamp);
				gst_structure_set_value(structure, "aamp_instance", &val);
				ret = TRUE;
			} else
			{
				ret = FALSE;
			}
			break;
		}

		default:
			break;
	}
	if (FALSE == ret )
	{
		GST_DEBUG_OBJECT(aamp, "Execute default handler for query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));
		ret = gst_pad_query_default(pad, parent, query);
	}
	return ret;
}

static gboolean gst_aamp_src_event(GstPad * pad, GstObject *parent, GstEvent * event)
{
	gboolean res = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_DEBUG_OBJECT(aamp, " EVENT %s\n", gst_event_type_get_name(GST_EVENT_TYPE(event)));

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_SEEK:
		{
			gdouble rate = 0;
			GstFormat format;
			GstSeekFlags flags;
			GstSeekType start_type;
			gint64 start = 0;
			GstSeekType stop_type;
			gint64 stop;
			gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
			if (format == GST_FORMAT_TIME)
			{
				GST_INFO_OBJECT(aamp, "sink pad : seek GST_FORMAT_TIME: rate %f, pos %" G_GINT64_FORMAT "\n", rate, start );
				if (flags & GST_SEEK_FLAG_FLUSH)
				{
					GST_DEBUG_OBJECT(aamp, "flush start");
					gst_pad_push_event(aamp->stream[eMEDIATYPE_VIDEO].srcpad, gst_event_new_flush_start());
					GST_DEBUG_OBJECT(aamp, "flush stop");
					gst_pad_push_event(aamp->stream[eMEDIATYPE_VIDEO].srcpad, gst_event_new_flush_stop(TRUE));
					if (aamp->audio_enabled)
					{
						GST_DEBUG_OBJECT(aamp, "flush start - aud");
						gst_pad_push_event(aamp->stream[eMEDIATYPE_AUDIO].srcpad, gst_event_new_flush_start());
						GST_DEBUG_OBJECT(aamp, "flush stop -aud");
						gst_pad_push_event(aamp->stream[eMEDIATYPE_AUDIO].srcpad, gst_event_new_flush_stop(TRUE));
					}
				}
				if (rate != aamp->rate)
				{
					aamp->context->UpdateRate(rate);
					aamp->rate  = rate;
				}

				if (start_type == GST_SEEK_TYPE_NONE)
				{
					aamp->player_aamp->SetRate(rate);
				}
				else if (start_type == GST_SEEK_TYPE_SET)
				{
					double pos;
					if (rate < 0)
					{
						pos = stop / GST_SECOND;
					}
					else
					{
						pos = start / GST_SECOND;
					}
					aamp->player_aamp->SetRateAndSeek(rate, pos);
				}
				else
				{
					GST_WARNING_OBJECT(aamp, "Not supported");
				}
				res = TRUE;
			}
			break;
		}

		default:
			break;
	}
	if (FALSE == res )
	{
		res = gst_pad_event_default(pad, parent, event);
	}
	else
	{
		gst_event_unref(event);
	}

	return res;
}
