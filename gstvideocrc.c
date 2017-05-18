/*
* This file is part of VideoCRC
*
 * VideoCRC video checksumming element
 * CRC algo is implemented based on standard CRC computation algorithm
 */

/**
 * SECTION:element-videocrc
 * @short_desc: computes 32 bit CRC for every video frame
 *
 * This element accepts selected YUV planar formats NV12, I420, and encoded buffer.
 * The default polynomial used is 0X04c11db7U but it can be changed before processing using crc-mask property.
 * CRC values can be saved to file by the location property
 * CRC values can also be printed on terminal using --gst-debug=videocrc:4
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -m uridecodebin uri=file:///path/to/video.mp4 ! decodebin ! videocrc ! fakesink
 * gst-launch-1.0 -m videotestsrc ! video/x-raw, width=720, height=576, format=NV12 ! videocrc ! fakesink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/mman.h>
#include "gstvideocrc.h"
#ifdef QCOM_HARDWARE
#include "../../gst-libs/gst/ionbuf/gstionbuf_meta.h"
#endif

#define WIDTH  (8 * sizeof(gint))
#define TOPBIT (1 << (WIDTH - 1))
#define ALIGN4K 4096
#define ALIGN128 128
#define ALIGN32 32
#define ALIGN16 16
#define ALIGN( num, to ) (((num) + (to-1)) & (~(to-1)))

#define GST_VIDEO_DEFAULT_CRC_MASK 0x04C11DB7L

#define GST_CAT_DEFAULT gst_videocrc_debug

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC (gst_videocrc_debug);

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_CRC_MASK
};

#define parent_class gst_videocrc_parent_class
G_DEFINE_TYPE (GstVideocrc, gst_videocrc, GST_TYPE_VIDEO_FILTER);

static void gst_videocrc_finalize (GObject * object);
static void gst_videocrc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_videocrc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_videocrc_transform_frame_ip (GstBaseTransform * trans,
        GstBuffer * buf);
static void gst_videocrc_init_crc32bit_table (GstVideocrc * videocrc);
static gboolean gst_videocrc_set_info (GstVideoFilter * filter, GstCaps * incaps,
            GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info);
static gboolean
gst_videocrc_start (GstBaseTransform * trans);
static gboolean
gst_videocrc_stop (GstBaseTransform * trans);
static gboolean
gst_videocrc_set_location (GstVideocrc * videocrc, const gchar * location);


static void
gst_videocrc_class_init (GstVideocrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseTransformClass *gstbasetrans_class;
  GstVideoFilterClass *videofilter_class = GST_VIDEO_FILTER_CLASS (klass);

  gobject_class = G_OBJECT_CLASS (klass);
  gstbasetrans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass), &srctemplate);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass), &sinktemplate);

  gobject_class->set_property = gst_videocrc_set_property;
  gobject_class->get_property = gst_videocrc_get_property;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Location",
         "Location of the file to write CRC message", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CRC_MASK,
      g_param_spec_uint ("crc-mask", "CRC polynomial",
          "CRC computation will use CRC polynomial set by application",
          0, G_MAXUINT, GST_VIDEO_DEFAULT_CRC_MASK, G_PARAM_READWRITE));

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_videocrc_finalize);

  gstbasetrans_class->start = GST_DEBUG_FUNCPTR (gst_videocrc_start);
  gstbasetrans_class->stop = GST_DEBUG_FUNCPTR (gst_videocrc_stop);
  gstbasetrans_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_videocrc_transform_frame_ip);
  videofilter_class->set_info = GST_DEBUG_FUNCPTR (gst_videocrc_set_info);

  gst_element_class_set_metadata (GST_ELEMENT_CLASS (klass),
      "Video CRC Generator",
      "Filter/Video",
      "Generate CRC for every video frame",
      "Zhou Jie <seuzhoujie@gmail.com>");
}

static void
gst_videocrc_reset (GstVideocrc * videocrc)
{
  videocrc->crc_mask = GST_VIDEO_DEFAULT_CRC_MASK;
  videocrc->frame_num = 0;
  videocrc->crc = 0;
  videocrc->logfile = NULL;
}

static void
gst_videocrc_init (GstVideocrc * videocrc)
{
  gst_videocrc_reset (videocrc);
  gst_videocrc_init_crc32bit_table (videocrc);
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (videocrc), FALSE);
}

static void
gst_videocrc_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

void
gst_videocrc_init_crc32bit_table (GstVideocrc * videocrc)
{
  unsigned int i, j;
  unsigned int remainder;
  unsigned int polynomial = videocrc->crc_mask;

  GST_DEBUG_OBJECT (videocrc, "Initialize CRC table using polynomial %0X",
      videocrc->crc_mask);
  for (i = 0; i < 256; i ++) {
    remainder = i << (WIDTH - 8);
    for (j = 0; j < 8; j ++) {
      if (remainder & TOPBIT)
        remainder = (remainder << 1) ^ polynomial;
      else
        remainder = (remainder << 1);
    }
    videocrc->crc32bit_table[i] = remainder;
  }
}

static gboolean
gst_videocrc_set_info (GstVideoFilter * filter, GstCaps * incaps,
            GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info)
{
  gint width, height, stride_w, stride_h, offset, size;
  GstVideocrc * videocrc = GST_VIDEOCRC (filter);

  width = GST_VIDEO_INFO_WIDTH (in_info);
  height = GST_VIDEO_INFO_HEIGHT (in_info);
  size = GST_VIDEO_INFO_SIZE(in_info);
  stride_w = ALIGN(width, ALIGN128);
  stride_h = ALIGN(height, ALIGN32);
  offset = GST_VIDEO_INFO_PLANE_OFFSET (in_info, 1);

  videocrc->width = width;
  videocrc->height = height;
  videocrc->stride_w = stride_w;
  videocrc->stride_h = stride_h;
  videocrc->offset = offset;
  videocrc->size = size;
  GST_DEBUG_OBJECT (videocrc, "width: %d, height: %d, stride_w: %d, stride_h: %d, offset: %d, size: %d", width, height, stride_w, stride_h, offset, size);

  return TRUE;
}

static gboolean
gst_videocrc_start (GstBaseTransform * trans)
{
  GstVideocrc * videocrc = GST_VIDEOCRC (trans);

  GST_DEBUG_OBJECT (videocrc, "start");

  if (videocrc->filename != NULL)
    videocrc->logfile = fopen (videocrc->filename, "w+");
  else
    videocrc->logfile = NULL;

  return TRUE;
}

static gboolean
gst_videocrc_stop (GstBaseTransform * trans)
{
  GstVideocrc * videocrc = GST_VIDEOCRC (trans);

  GST_DEBUG_OBJECT (videocrc, "stop");
  if (videocrc->logfile != NULL) {
    fclose (videocrc->logfile);
    videocrc->logfile = NULL;
  }

  return TRUE;
}

static GstFlowReturn gst_videocrc_transform_frame_ip (GstBaseTransform * trans,
        GstBuffer * buf)
{
  gint i, j, k;
  guint8 LumaPixVal1, LumaPixVal2, CbPixVal, CrPixVal;
  gint width, height, stride_w, stride_h;
  GstMapInfo map_info;
  guint32 CRC, crc_pos;
  guint size, offset, fd;
  guint8 *buf_ptr;


  GstVideocrc * videocrc = GST_VIDEOCRC (trans);
  guint32 *CRC32Table = videocrc->crc32bit_table;

  CRC = 0x0;
  videocrc->crc = 0;

  width = videocrc->width;
  height = videocrc->height;
  stride_w = videocrc->stride_w;
  stride_h = videocrc->stride_h;

  GstIonBufFdMeta *ion_meta;
  ion_meta = gst_buffer_get_ionfd_meta (buf);

  //omxdecoder output ion buffer
  if (ion_meta) {
    fd = ion_meta->fd;
    size = ion_meta->size;
    offset = ion_meta->offset;
    buf_ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED,
            fd, offset);

    /* compute Luma CRC */
    for (i = 0; i < height; i++) {
        for (j = 0, k = 0; j < width >> 1; j++) {
            LumaPixVal1 = buf_ptr[i * stride_w + k++];
            LumaPixVal2 = buf_ptr[i * stride_w + k++];
            crc_pos = ((guint32) (CRC >> 24) ^ LumaPixVal1) & 0xFF;
            CRC = (CRC << 8) ^ CRC32Table[crc_pos];
            crc_pos = ((guint32) (CRC >> 24) ^ LumaPixVal2) & 0xFF;
            CRC = (CRC << 8) ^ CRC32Table[crc_pos];
        }
    }
    CRC = ~CRC;

    /* compute Chroma U CRC */
    for (i = 0; i < height / 2; i++) {
        for (j = 0; j < width; j += 2) {
            CbPixVal = buf_ptr[stride_w * stride_h + i * stride_w + j];
            crc_pos = ((guint32) (CRC >> 24) ^ CbPixVal) & 0xFF;
            CRC = (CRC << 8) ^ CRC32Table[crc_pos];
        }
    }
    CRC = ~CRC;

    /* compute Chroma V CRC */
    for (i = 0; i < height / 2; i++) {
        for (j = 0; j < width; j += 2) {
            CrPixVal = buf_ptr[stride_w * stride_h + i * stride_w + j + 1];
            crc_pos = ((guint32) (CRC >> 24) ^ CrPixVal) & 0xFF;
            CRC = (CRC << 8) ^ CRC32Table[crc_pos];
        }
    }
    CRC = ~CRC;
    munmap (buf_ptr, size);
  }
  else {
    //omxencoder output non ion buffer
    size = videocrc->size;
    gst_buffer_map (buf, &map_info, GST_MAP_READ); 
    for (i = 0; i < map_info.size; i++) {
      CRC = (CRC << 8) ^ CRC32Table[(CRC >> 24) ^ map_info.data[i]];
    }
    CRC = ~CRC;
    gst_buffer_unmap (buf, &map_info);
  }

  videocrc->crc = CRC;

  videocrc->frame_num ++;
  /* print this info using --gst-debug=videocrc:4 */
  GST_INFO_OBJECT (videocrc, "VideoFrame %d crc %08X",
     videocrc->frame_num, videocrc->crc);
  if (videocrc->logfile)
    fprintf (videocrc->logfile, "VideoFrame %d crc %08X\n",
          videocrc->frame_num, videocrc->crc);

  return GST_FLOW_OK;
}

static gboolean
gst_videocrc_set_location (GstVideocrc * videocrc, const gchar * location)
{
    GstState state;

    /* the element must be stopped in order to do this */
    GST_OBJECT_LOCK (videocrc);
    state = GST_STATE (videocrc);
    if (state != GST_STATE_READY && state != GST_STATE_NULL)
        goto wrong_state;
    GST_OBJECT_UNLOCK (videocrc);

    g_free (videocrc->filename);

    /* clear the filename if we get a NULL */
    if (location == NULL) {
        videocrc->filename = NULL;
    } else {
        /* we store the filename as received by the application. On Windows this
         *      * should be UTF8 */
        videocrc->filename = g_strdup (location);
        GST_DEBUG_OBJECT (videocrc, "filename: %s", videocrc->filename);
    }

    return TRUE;

    /* ERROR */
wrong_state:
    {
        g_warning ("Changing the `location' property on filesrc when a file is "
                "open is not supported.");
        GST_OBJECT_UNLOCK (videocrc);
        return FALSE;
    }
}

static void
gst_videocrc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVideocrc *videocrc = GST_VIDEOCRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      gst_videocrc_set_location (videocrc, g_value_get_string (value));
      break;
    case PROP_CRC_MASK:
      videocrc->crc_mask = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_videocrc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVideocrc *videocrc = GST_VIDEOCRC (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, videocrc->filename);
      break;
    case PROP_CRC_MASK:
      g_value_set_uint (value, videocrc->crc_mask);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret;

  GST_DEBUG_CATEGORY_INIT (gst_videocrc_debug, "videocrc", 0,
      "videocrc element");

  ret = gst_element_register (plugin, "videocrc", GST_RANK_NONE,
      GST_TYPE_VIDEOCRC);

  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    videocrc,
    "computes video CRC for every video frame",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
