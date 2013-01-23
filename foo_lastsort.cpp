#include "../SDK/foobar2000.h"
#include "../helpers/helpers.h"
#include "xmlParser.h"
#include <map>

/**************************
Providing information about a component.

We can provide some information about our component that
users will be able to view under Preferences > Components.
**************************/

DECLARE_COMPONENT_VERSION(
	// component name
	"Last.fm Sort",
	// component version
	"0.1",
	// about text
	"Sort by Last.fm playcount\n"
	"By Vegard Øye"
);

pfc::string8 get_url(pfc::string8 url, abort_callback &p_abort) {
	pfc::string8 data;
	http_client::ptr client;

	try {
		if (!service_enum_t<http_client>().first(client)) {
			console::print("feed downloading: error, unable to start http service");
			return data;
		}

		http_request::ptr request = client->create_request("GET");
		file::ptr file_ptr = request->run(url.get_ptr(), p_abort);
		char buffer[1025];
		t_size bytes_read;

		while (bytes_read = file_ptr->read(buffer, 1024, p_abort)) {
			data.add_string(buffer, bytes_read);
		}

		return data;
	} catch (exception_aborted) {
		console::print("feed downloading: aborted");
		throw exception_aborted();
	} catch (...) {
		console::print("feed downloading: error, aborted");
		return data;
	}
}

pfc::string8 get_track_info(const metadb_handle_ptr track, abort_callback &p_abort) {
	pfc::string8 api_key = "803d3cbea0bbe50c61ab81c4fe5fe20f";
	pfc::string8 url, page, artist_enc, title_enc;
	const file_info *file_info;
	const char *artist;
	const char *title;
	bool meta;

	static_api_ptr_t<metadb> db;
	db->database_lock();
	meta = track->get_info_async_locked(file_info);
	db->database_unlock();

	if (meta && file_info->meta_exists("artist") &&
	    file_info->meta_exists("title")) {
		artist = file_info->meta_get("artist", 0);
		title  = file_info->meta_get("title",  0);
	} else {
		throw pfc::exception("Unknown track");
	}

	pfc::urlEncode(artist_enc, artist);
	pfc::urlEncode(title_enc, title);

	url << "http://ws.audioscrobbler.com/2.0/?method=track.getInfo"
	    << "&api_key=" << api_key << "&artist=" << artist_enc << "&track=" << title_enc;

	console::print(url);

	page = get_url(url, p_abort);

	if (page.get_length() < 10) {
		throw pfc::exception("Last.fm returned an empty page");
	}

	return page;
}

XMLNode get_track_xml(const metadb_handle_ptr track, abort_callback &p_abort) {
	pfc::string8 page = get_track_info(track, p_abort);
	t_size page_wsize = pfc::stringcvt::estimate_utf8_to_wide(page, ~0);
	wchar_t *page_w = new wchar_t[page_wsize];
	pfc::stringcvt::convert_utf8_to_wide(page_w, page_wsize, page, ~0);
	XMLNode xml = XMLNode::parseString(page_w, L"lfm");
	delete[] page_w;
	return xml;
}

int get_track_count(const metadb_handle_ptr track, abort_callback &p_abort) {
	pfc::string8 page;
	XMLNode root_node, track_node, playcount_node;

	try {
		root_node = get_track_xml(track, p_abort);
	} catch (pfc::exception) {
		return -1;
	}

	if (root_node.isEmpty()) {
		throw pfc::exception("Last.fm returned an empty page");
	}

	track_node = root_node.getChildNode(L"track");
	playcount_node = track_node.getChildNode(L"playcount");

	if (playcount_node.nText() != 1) {
		return -1;
	}

	return atoi(pfc::stringcvt::string_utf8_from_wide(playcount_node.getText()));
}

pfc::list_t<metadb_handle_ptr> get_sorted_playlist(const pfc::list_base_const_t<metadb_handle_ptr> &data, threaded_process_status &p_status, abort_callback &p_abort) {
	std::multimap<int, metadb_handle_ptr, std::greater<int>> temp;
	std::multimap<int, metadb_handle_ptr, std::greater<int>>::iterator it;
	pfc::list_t<metadb_handle_ptr> result;
	pfc::lores_timer timer;
	pfc::string8 message, msg;
	int size = data.get_count();

	for (int i = 0; i < size; i++) {
		message.reset();
		message << "Track " << i + 1 << " of " << size;
		p_status.set_item(message);
		p_status.set_progress(i + 1, size);

		timer.start();

		const metadb_handle_ptr track = data[i];
		int count = get_track_count(track, p_abort);

		msg.reset();
		msg << count;
		console::print(msg);

		// don't make more than 5 requests per second
		// (averaged over a 5 minute period)
		if (timer.query() < 0.2) {
			p_abort.sleep(0.2);
		}

		if (i && (i % 100 == 0)) {
			p_abort.sleep(10);
		}

		if (count > 0) {
			temp.insert(std::pair<int, metadb_handle_ptr>(count, track));
		} else {
			temp.insert(std::pair<int, metadb_handle_ptr>(0, track));
		}
	}

	for (it = temp.begin(); it != temp.end(); it++) {
		metadb_handle_ptr track = it->second;
		result.add_item(track);
	}

	return result;
}

class playlist_sort_worker : public threaded_process_callback {
private:
	bool success;
	pfc::list_t<metadb_handle_ptr> selected_items;
	pfc::list_t<metadb_handle_ptr> all_items;
	pfc::list_t<metadb_handle_ptr> sorted_playlist;
	bit_array_bittable select_mask;
	t_size active_playlist;
	t_size playlist_length;
	int first_pos;
public:
	playlist_sort_worker() {
		success = false;
	}
	virtual void on_init(HWND p_wnd) {
		static_api_ptr_t<playlist_manager> pm;
		active_playlist = pm->get_active_playlist();
		pm->activeplaylist_get_all_items(all_items);
		pm->activeplaylist_get_selected_items(selected_items);
		playlist_length = all_items.get_count();
		first_pos = all_items.find_item(selected_items[0]);
		select_mask = bit_array_bittable(playlist_length);
		pm->activeplaylist_get_selection_mask(select_mask);
	}
	virtual void run(threaded_process_status &p_status, abort_callback &p_abort) {
		try {
			p_status.set_item("Downloading track info from Last.fm...");
			sorted_playlist = get_sorted_playlist(selected_items, p_status, p_abort);
			success = true;
		} catch (...) {
			popup_message::g_show("Error, see the console log", "foo_last_sort", popup_message::icon_error);
		}
	}
	virtual void on_done(HWND p_wnd, bool p_was_aborted) {
		if (success) {
			static_api_ptr_t<playlist_manager> pm;
			pm->playlist_remove_items(active_playlist, select_mask);
			pm->playlist_insert_items(active_playlist, first_pos, sorted_playlist, bit_array_true());
		}
	}
};

/**************************
Context menu commands.
**************************/

class my_contextmenu : public contextmenu_item_simple {
	// Return the number of commands we provide.
	virtual unsigned get_num_items() {
		return 1;
	}

	// This name is used to identify the command in the menu.
	virtual void get_item_name(unsigned p_index, pfc::string_base &p_out) {
		p_out = "Sort by Last.fm";
	}

	// Execute command.
	virtual void context_command(unsigned p_index, const pfc::list_base_const_t<metadb_handle_ptr> &p_data, const GUID &p_caller) {
		service_ptr_t<threaded_process_callback> tpc = new service_impl_t<playlist_sort_worker>();
		threaded_process::g_run_modeless(tpc, threaded_process::flag_show_abort | threaded_process::flag_show_progress | threaded_process::flag_show_item, core_api::get_main_window(), "Sorting by playcount");
	}

	// All commands are identified by a GUID.
	virtual GUID get_item_guid(unsigned p_index) {
		// {239CF925-EA60-44a6-A69C-61F4D77CC5F4}
		static const GUID guid_sort_by_chart = { 0x239cf925, 0xea60, 0x44a6, { 0xa6, 0x9c, 0x61, 0xf4, 0xd7, 0x7c, 0xc5, 0xf4 } };
		return guid_sort_by_chart;
	}

	// Set p_out to the description for the n-th command.
	virtual bool get_item_description(unsigned p_index, pfc::string_base &p_out) {
		p_out = "Sorts the tracks according to the playcount on Last.fm";
		return true;
	}
};

// We need to create a service factory for our menu item class,
// otherwise the menu commands won't be known to the system.
static contextmenu_item_factory_t<my_contextmenu> foo_contextmenu;
