plugin_LTLIBRARIES = libgstvideocrc.la

libgstvideocrc_la_SOURCES = \
	gstvideocrc.c \
	gstvideocrc.h

noinst_HEADERS = \
	gstvideocrc.h

libgstvideocrc_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) \
			  $(GST_BASE_CFLAGS) \
			  $(GST_CFLAGS) \
			  $(GST_VIDEOCRC_CFLAGS)

libgstvideocrc_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
			  $(GST_BASE_LIBS) \
			  $(GST_LIBS) \
			  -lgstvideo-$(GST_API_VERSION) \
			  $(top_builddir)/gst-libs/gst/ionbuf/libgstionbuf-$(GST_API_VERSION).la \
			  $(GST_VIDEOCRC_LIBS)

AM_CPPFLAGS = -DQCOM_HARDWARE
libgstvideocrc_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvideocrc_la_LIBTOOLFLAGS =$(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gstvideocrc.h
