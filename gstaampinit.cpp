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
#include "gstaamp.h"
#include "gstaampsrc.h"
#ifdef DRM_BUILD_PROFILE
#include "gstaampplayreadydecryptor.h"
#include "gstaampwidevinedecryptor.h"
#endif

static gboolean plugin_init(GstPlugin * plugin)
{
	gboolean ret = gst_element_register(plugin, "aamp", GST_RANK_MARGINAL, GST_TYPE_AAMP);
	if (ret)
	{
		ret = gst_element_register(plugin, "aampsrc", GST_RANK_PRIMARY, GST_TYPE_AAMPSRC);
	}
#ifdef DRM_BUILD_PROFILE
	if (ret)
	{
		ret = gst_element_register(plugin, GstPluginNamePR,
				GST_RANK_PRIMARY, GST_TYPE_AAMPPLAYREADYDECRYPTOR );
		if(ret)
		{
			logprintf("aamp plugin_init registered %s element\n", GstPluginNamePR);
		}
		else
		{
			logprintf("aamp plugin_init FAILED to register %s element\n", GstPluginNamePR);
		}
		ret = gst_element_register(plugin, GstPluginNameWV,
				GST_RANK_PRIMARY, GST_TYPE_AAMPWIDEVINEDECRYPTOR );
		if(ret)
		{
			logprintf("aamp plugin_init registered %s element\n", GstPluginNameWV);
		}
		else
		{
			logprintf("aamp plugin_init FAILED to register %s element\n", GstPluginNameWV);
		}
	}
#endif
	return ret;
}

#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "RDK"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "aamp"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://rdkcentral.com/"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
		GST_VERSION_MINOR,
		aamp,
		"Advanced Adaptive Media Player",
		plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
