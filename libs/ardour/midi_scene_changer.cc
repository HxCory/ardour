/*
    Copyright (C) 2014 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "evoral/MIDIEvent.hpp"
#include "midi++/channel.h"
#include "midi++/parser.h"
#include "midi++/port.h"

#include "ardour/event_type_map.h"
#include "ardour/midi_port.h"
#include "ardour/midi_scene_change.h"
#include "ardour/midi_scene_changer.h"
#include "ardour/session.h"

#include "i18n.h"

using namespace ARDOUR;

MIDISceneChanger::MIDISceneChanger (Session& s)
	: SceneChanger (s)
	, _recording (true)
	, last_bank_message_time (-1)
	, last_program_message_time (-1)
	, last_delivered_program (-1)
	, last_delivered_bank (-1)
	  
{
	_session.locations()->changed.connect_same_thread (*this, boost::bind (&MIDISceneChanger::locations_changed, this, _1));
	Location::scene_changed.connect_same_thread (*this, boost::bind (&MIDISceneChanger::gather, this));
}

MIDISceneChanger::~MIDISceneChanger ()
{
}

void
MIDISceneChanger::locations_changed (Locations::Change)
{
	gather ();
}

/** Use the session's list of locations to collect all patch changes.
 * 
 * This is called whenever the locations change in anyway.
 */
void
MIDISceneChanger::gather ()
{
	const Locations::LocationList& locations (_session.locations()->list());
	boost::shared_ptr<SceneChange> sc;

	scenes.clear ();

	for (Locations::LocationList::const_iterator l = locations.begin(); l != locations.end(); ++l) {

		if ((sc = (*l)->scene_change()) != 0) {

			boost::shared_ptr<MIDISceneChange> msc = boost::dynamic_pointer_cast<MIDISceneChange> (sc);

			if (msc) {
				scenes.insert (std::make_pair (msc->time(), msc));
			}
		}
	}
}

void
MIDISceneChanger::deliver (MidiBuffer& mbuf, framepos_t when, boost::shared_ptr<MIDISceneChange> msc)
{
	uint8_t buf[4];
	size_t cnt;

	if ((cnt = msc->get_bank_msb_message (buf, sizeof (buf))) > 0) {
		mbuf.push_back (when, cnt, buf);

		if ((cnt = msc->get_bank_lsb_message (buf, sizeof (buf))) > 0) {
			mbuf.push_back (when, cnt, buf);
		}

		last_delivered_bank = msc->bank();
	}

	if ((cnt = msc->get_program_message (buf, sizeof (buf))) > 0) {
		mbuf.push_back (when, cnt, buf);

		last_delivered_program = msc->program();
	}
}
			

void
MIDISceneChanger::run (framepos_t start, framepos_t end)
{
	if (!output_port || recording()) {
		return;
	}

	/* get lower bound of events to consider */

	Scenes::const_iterator i = scenes.lower_bound (start);
	MidiBuffer& mbuf (output_port->get_midi_buffer (end-start));

	while (i != scenes.end()) {

		if (i->first >= end) {
			break;
		}
		
		deliver (mbuf, i->first - start, i->second);

		++i;
	}
}

void
MIDISceneChanger::locate (framepos_t pos)
{
	Scenes::const_iterator i = scenes.upper_bound (pos);

	if (i == scenes.end()) {
		return;
	}

	if (i->second->program() != last_delivered_program || i->second->bank() != last_delivered_bank) {
		// MidiBuffer& mbuf (output_port->get_midi_buffer (end-start));
		// deliver (mbuf, i->first, i->second);
	}
}		

void
MIDISceneChanger::set_input_port (MIDI::Port* mp)
{
	input_port = mp;

	incoming_connections.drop_connections();
	
	if (input_port) {
		
		/* midi port is asynchronous. MIDI parsing will be carried out
		 * by the MIDI UI thread which will emit the relevant signals
		 * and thus invoke our callbacks as necessary.
		 */

		for (int channel = 0; channel < 16; ++channel) {
			input_port->parser()->channel_bank_change[channel].connect_same_thread (incoming_connections, boost::bind (&MIDISceneChanger::bank_change_input, this, _1, _2, channel));
			input_port->parser()->channel_program_change[channel].connect_same_thread (incoming_connections, boost::bind (&MIDISceneChanger::program_change_input, this, _1, _2, channel));
		}
	}
}

void
MIDISceneChanger::set_output_port (boost::shared_ptr<MidiPort> mp)
{
	output_port = mp;
}

void
MIDISceneChanger::set_recording (bool yn)
{
	_recording = yn;
}

bool
MIDISceneChanger::recording() const 
{
	return _session.transport_rolling() && _session.get_record_enabled();
}

void
MIDISceneChanger::bank_change_input (MIDI::Parser& parser, unsigned short, int)
{
	if (!recording()) {
		return;
	}

	last_bank_message_time = parser.get_timestamp ();
}

void
MIDISceneChanger::program_change_input (MIDI::Parser& parser, MIDI::byte program, int channel)
{
	framecnt_t time = parser.get_timestamp ();

	last_program_message_time = time;

	if (!recording()) {
		jump_to (input_port->channel (channel)->bank(), program);
		return;
	}

	Locations* locations (_session.locations ());
	Location* loc;
	bool new_mark = false;
	framecnt_t slop = (framecnt_t) floor ((Config->get_inter_scene_gap_msecs() / 1000.0) * _session.frame_rate());

	/* check for marker at current location */

	loc = locations->mark_at (time, slop);

	if (!loc) {
		/* create a new marker at the desired position */
		
		std::string new_name;

		if (!locations->next_available_name (new_name, _("Scene "))) {
			std::cerr << "No new marker name available\n";
			return;
		}
		
		loc = new Location (_session, time, time, new_name, Location::IsMark);
		new_mark = true;
	}

	unsigned short bank = input_port->channel (channel)->bank();

	MIDISceneChange* msc =new MIDISceneChange (loc->start(), channel, bank, program & 0x7f);

	loc->set_scene_change (boost::shared_ptr<MIDISceneChange> (msc));
	
	/* this will generate a "changed" signal to be emitted by locations,
	   and we will call ::gather() to update our list of MIDI events.
	*/

	if (new_mark) {
		locations->add (loc);
	}
}

void
MIDISceneChanger::jump_to (int bank, int program)
{
	const Locations::LocationList& locations (_session.locations()->list());
	boost::shared_ptr<SceneChange> sc;
	framepos_t where = max_framepos;

	for (Locations::LocationList::const_iterator l = locations.begin(); l != locations.end(); ++l) {

		if ((sc = (*l)->scene_change()) != 0) {

			boost::shared_ptr<MIDISceneChange> msc = boost::dynamic_pointer_cast<MIDISceneChange> (sc);

			if (msc->bank() == bank && msc->program() == program && (*l)->start() < where) {
				where = (*l)->start();
			}
		}
	}

	if (where != max_framepos) {
		_session.request_locate (where);
	}
}