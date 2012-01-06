#include <gst/gst.h>
#include "kms-core.h"

static GstElement *pipe = NULL;
G_LOCK_DEFINE_STATIC(mutex);
static gboolean init = FALSE;

static gpointer
gstreamer_thread(gpointer data) {
	GMainLoop *loop;
	loop = g_main_loop_new(NULL, TRUE);
	g_main_loop_run(loop);
	return NULL;
}

static void
bus_msg(GstBus *bus, GstMessage *msg, gpointer not_used) {
	switch (msg->type) {
	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		gchar *dbg_info = NULL;

		gst_message_parse_error (msg, &err, &dbg_info);
		g_warning("ERROR from element %s: %s\n",
						GST_OBJECT_NAME (msg->src),
						err->message);
		g_printerr("Debugging info: %s\n",
						(dbg_info) ? dbg_info : "none");
		g_error_free(err);
		g_free(dbg_info);
		break;
	}
	case GST_MESSAGE_EOS:
		g_warning("EOS message should not be received, "
						"pipeline can be stopped!");
		break;
	default:
		/* No action */
		break;
	}


}

void
kms_init(gint *argc, gchar **argv[]) {
	G_LOCK(mutex);
	if (!init) {
		GstBus *bus;

		g_type_init();
		gst_init(argc, argv);
		pipe = gst_pipeline_new(NULL);
		gst_element_set_state(pipe, GST_STATE_PLAYING);

		bus = gst_element_get_bus(pipe);
		gst_bus_add_watch(bus, gst_bus_async_signal_func, NULL);
		g_object_connect(bus, "signal::message", bus_msg, NULL, NULL);

		g_thread_create(gstreamer_thread, NULL, TRUE, NULL);

		init = TRUE;
	}
	G_UNLOCK(mutex);
}

GstElement*
kms_get_pipeline() {
	return pipe;
}

static void
linked(GstPad *pad, GstPad *peer, gpointer orig) {
	GstElement *dest;

	dest = gst_pad_get_parent_element(pad);
	gst_element_link(orig, dest);
	g_object_unref(dest);
}

static void
unlinked(GstPad *pad, gpointer orig) {
	GstElement *dest;
	GstPad *peer, *sink;

	dest = gst_pad_get_parent_element(pad);
	sink = gst_element_get_static_pad(dest, "sink");
	if (gst_pad_is_linked(sink)) {
		peer = gst_pad_get_peer(sink);
		if (peer != NULL) {
			gst_pad_unlink(peer, sink);
			g_object_unref(peer);
		}
	}
	g_object_unref(dest);
	g_object_unref(sink);
}

void
kms_dynamic_connection(GstElement *orig, GstElement *dest,
						const gchar *pad_name) {
	GstPad *pad;

	pad = gst_element_get_static_pad(dest, pad_name);

	g_object_connect(pad, "signal::linked", linked, orig, NULL);
	g_object_connect(pad, "signal::unlinked", unlinked, orig, NULL);
	g_object_unref(pad);
}

static gboolean
check_template_names(GstPadTemplate *tmp1, GstPadTemplate *tmp2) {
	gboolean ret = FALSE;

	switch (tmp1->direction) {
	case GST_PAD_SINK:
		ret = ret || g_strcmp0(tmp1->name_template, "sink") == 0;
		break;
	case GST_PAD_SRC:
		ret = ret || g_strcmp0(tmp1->name_template, "src") == 0;
		break;
	default:
		return FALSE;
	}

	switch (tmp2->direction) {
	case GST_PAD_SINK:
		ret = ret || g_strcmp0(tmp2->name_template, "sink") == 0;
		break;
	case GST_PAD_SRC:
		ret = ret || g_strcmp0(tmp2->name_template, "src") == 0;
		break;
	default:
		return FALSE;
	}

	return ret;
}

GstElement*
kms_utils_get_element_for_caps(GstElementFactoryListType type, GstRank rank,
				const GstCaps *caps, GstPadDirection direction,
				gboolean subsetonly, const gchar *name) {
	GList *list, *filter, *l;
	GstElement *elem = NULL;

	list = gst_element_factory_list_get_elements(type, rank);
	filter = gst_element_factory_list_filter(list, caps, direction,
								subsetonly);

	l = filter;
	while (l != NULL) {
		if (l->data != NULL) {
			const GList *pads;
			pads = gst_element_factory_get_static_pad_templates(
								l->data);
			if (g_list_length((GList *)pads) == 2) {
				GstPadTemplate *tmp1, *tmp2;
				gboolean finish = FALSE;

				tmp1 = gst_static_pad_template_get(pads->data);
				tmp2 = gst_static_pad_template_get(
							pads->next->data);

				finish = check_template_names(tmp1, tmp2);

				g_object_unref(tmp1);
				g_object_unref(tmp2);

				if (finish)
					break;
			}
		}
		l = l->next;
	}

	if (l == NULL || l->data == NULL)
		goto end;

	g_print("%p\n", l->data);
	g_print("Factory >%s<\n", gst_element_factory_get_klass(l->data));

	elem = gst_element_factory_create(l->data, name);

end:
	gst_plugin_feature_list_free(list);
	gst_plugin_feature_list_free(filter);

	return elem;
}

GstElement*
kms_generate_bin_with_caps(GstElement *elem, GstCaps *sink_caps,
							GstCaps *src_caps) {
	GstElement *bin;
	GstPad *gsink, *gsrc, *sink, *src;
	GstPadTemplate *sink_temp, *src_temp;

	bin = gst_bin_new(GST_ELEMENT_NAME(elem));
	gst_bin_add(GST_BIN(bin), elem);

	src = gst_element_get_static_pad(elem, "src");
	sink = gst_element_get_static_pad(elem, "sink");

	sink_temp = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
						gst_caps_copy(sink_caps));
	src_temp = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
					 gst_caps_copy(src_caps));

	gsink = gst_ghost_pad_new_no_target_from_template("sink", sink_temp);
	gsrc =  gst_ghost_pad_new_no_target_from_template("src", src_temp);

	gst_element_add_pad(bin, gsink);
	gst_element_add_pad(bin, gsrc);

	gst_ghost_pad_set_target(GST_GHOST_PAD(gsrc), src);
	gst_ghost_pad_set_target(GST_GHOST_PAD(gsink), sink);

	g_object_unref(src);
	g_object_unref(sink);
	g_object_unref(sink_temp);
	g_object_unref(src_temp);
	return bin;
}
