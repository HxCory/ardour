/*
    Copyright (C) 2006 Paul Davis

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <algorithm>
#include <cmath>
#include <limits>

#include "pbd/compose.h"

#include "ardour/audio_buffer.h"
#include "ardour/buffer_set.h"
#include "ardour/dB.h"
#include "ardour/meter.h"
#include "ardour/midi_buffer.h"
#include "ardour/session.h"
#include "ardour/rc_configuration.h"
#include "ardour/runtime_functions.h"

using namespace std;

using namespace ARDOUR;

PBD::Signal0<void> Metering::Meter;

PeakMeter::PeakMeter (Session& s, const std::string& name)
    : Processor (s, string_compose ("meter-%1", name))
{
	Kmeterdsp::init(s.nominal_frame_rate());
	Iec1ppmdsp::init(s.nominal_frame_rate());
	Iec2ppmdsp::init(s.nominal_frame_rate());
	Vumeterdsp::init(s.nominal_frame_rate());
	_pending_active = true;
	_meter_type = MeterPeak;
}

PeakMeter::~PeakMeter ()
{
	while (_kmeter.size() > 0) {
		delete (_kmeter.back());
		delete (_iec1meter.back());
		delete (_iec2meter.back());
		delete (_vumeter.back());
		_kmeter.pop_back();
		_iec1meter.pop_back();
		_iec2meter.pop_back();
		_vumeter.pop_back();
	}
}


/** Get peaks from @a bufs
 * Input acceptance is lenient - the first n buffers from @a bufs will
 * be metered, where n was set by the last call to setup(), excess meters will
 * be set to 0.
 *
 * (runs in jack realtime context)
 */
void
PeakMeter::run (BufferSet& bufs, framepos_t /*start_frame*/, framepos_t /*end_frame*/, pframes_t nframes, bool)
{
	if (!_active && !_pending_active) {
		return;
	}

	// cerr << "meter " << name() << " runs with " << bufs.available() << " inputs\n";

	const uint32_t n_audio = min (current_meters.n_audio(), bufs.count().n_audio());
	const uint32_t n_midi  = min (current_meters.n_midi(), bufs.count().n_midi());

	uint32_t n = 0;

	// Meter MIDI in to the first n_midi peaks
	for (uint32_t i = 0; i < n_midi; ++i, ++n) {
		float val = 0.0f;
		const MidiBuffer& buf (bufs.get_midi(i));
		
		for (MidiBuffer::const_iterator e = buf.begin(); e != buf.end(); ++e) {
			const Evoral::MIDIEvent<framepos_t> ev(*e, false);
			if (ev.is_note_on()) {
				const float this_vel = ev.buffer()[2] / 127.0;
				if (this_vel > val) {
					val = this_vel;
				}
			} else {
				val += 1.0 / bufs.get_midi(n).capacity();
				if (val > 1.0) {
					val = 1.0;
				}
			}
		}
		_peak_signal[n] = max (val, _peak_signal[n]);
	}

	// Meter audio in to the rest of the peaks
	for (uint32_t i = 0; i < n_audio; ++i, ++n) {
		if (bufs.get_audio(i).silent()) {
			_peak_signal[n] = .0f;
		} else {
			_peak_signal[n] = compute_peak (bufs.get_audio(i).data(), nframes, _peak_signal[n]);
		}
		if (_meter_type & (MeterKrms | MeterK20 | MeterK14 | MeterK12)) {
			_kmeter[i]->process(bufs.get_audio(i).data(), nframes);
		}
		if (_meter_type & (MeterIEC1DIN | MeterIEC1NOR)) {
			_iec1meter[i]->process(bufs.get_audio(i).data(), nframes);
		}
		if (_meter_type & (MeterIEC2BBC | MeterIEC2EBU)) {
			_iec2meter[i]->process(bufs.get_audio(i).data(), nframes);
		}
		if (_meter_type & MeterVU) {
			_vumeter[i]->process(bufs.get_audio(i).data(), nframes);
		}
	}

	// Zero any excess peaks
	for (uint32_t i = n; i < _peak_signal.size(); ++i) {
		_peak_signal[i] = 0.0f;
	}

	_active = _pending_active;
}

void
PeakMeter::reset ()
{
	for (size_t i = 0; i < _peak_signal.size(); ++i) {
		_peak_signal[i] = 0.0f;
	}

	for (size_t n = 0; n < _kmeter.size(); ++n) {
		_kmeter[n]->reset();
		_iec1meter[n]->reset();
		_iec2meter[n]->reset();
		_vumeter[n]->reset();
	}
}

void
PeakMeter::reset_max ()
{
	for (size_t i = 0; i < _max_peak_power.size(); ++i) {
		_max_peak_power[i] = -std::numeric_limits<float>::infinity();
		_max_peak_signal[i] = 0;
	}

	const size_t n_midi  = min (_peak_signal.size(), (size_t) current_meters.n_midi());

	for (size_t n = 0; n < _peak_signal.size(); ++n) {
		if (n < n_midi) {
			_visible_peak_power[n] = 0;
		} else {
			_visible_peak_power[n] = -std::numeric_limits<float>::infinity();
		}
	}
}

bool
PeakMeter::can_support_io_configuration (const ChanCount& in, ChanCount& out)
{
	out = in;
	return true;
}

bool
PeakMeter::configure_io (ChanCount in, ChanCount out)
{
	if (out != in) { // always 1:1
		return false;
	}

	current_meters = in;

	reset_max_channels (in);

	return Processor::configure_io (in, out);
}

void
PeakMeter::reflect_inputs (const ChanCount& in)
{
	for (uint32_t i = in.n_total(); i < current_meters.n_total(); ++i) {
		if (i < _peak_signal.size()) {
			_peak_signal[i] = 0.0f;
		}
	}
	for (uint32_t i = in.n_audio(); i < current_meters.n_audio(); ++i) {
		if (i >= _kmeter.size()) continue;
		_kmeter[i]->reset();
		_iec1meter[i]->reset();
		_iec2meter[i]->reset();
		_vumeter[i]->reset();
	}

	current_meters = in;
	reset_max();

	ConfigurationChanged (in, in); /* EMIT SIGNAL */
}

void
PeakMeter::reset_max_channels (const ChanCount& chn)
{
	uint32_t const limit = chn.n_total();
	const size_t n_audio = chn.n_audio();

	while (_peak_signal.size() > limit) {
		_peak_signal.pop_back();
		_visible_peak_power.pop_back();
		_max_peak_signal.pop_back();
		_max_peak_power.pop_back();
	}

	while (_peak_signal.size() < limit) {
		_peak_signal.push_back(0);
		_visible_peak_power.push_back(minus_infinity());
		_max_peak_signal.push_back(0);
		_max_peak_power.push_back(minus_infinity());
	}

	assert(_peak_signal.size() == limit);
	assert(_visible_peak_power.size() == limit);
	assert(_max_peak_signal.size() == limit);
	assert(_max_peak_power.size() == limit);

	/* alloc/free other audio-only meter types. */
	while (_kmeter.size() > n_audio) {
		delete (_kmeter.back());
		delete (_iec1meter.back());
		delete (_iec2meter.back());
		delete (_vumeter.back());
		_kmeter.pop_back();
		_iec1meter.pop_back();
		_iec2meter.pop_back();
		_vumeter.pop_back();
	}
	while (_kmeter.size() < n_audio) {
		_kmeter.push_back(new Kmeterdsp());
		_iec1meter.push_back(new Iec1ppmdsp());
		_iec2meter.push_back(new Iec2ppmdsp());
		_vumeter.push_back(new Vumeterdsp());
	}
	assert(_kmeter.size() == n_audio);
	assert(_iec1meter.size() == n_audio);
	assert(_iec2meter.size() == n_audio);
	assert(_vumeter.size() == n_audio);

	reset();
	reset_max();
}

/** To be driven by the Meter signal from IO.
 * Caller MUST hold its own processor_lock to prevent reconfiguration
 * of meter size during this call.
 */

void
PeakMeter::meter ()
{
	if (!_active) {
		return;
	}

	// TODO block this thread while PeakMeter::reset_max_channels() is
	// reallocating channels.
	// (may happen with Session > New: old session not yet closed,
	// meter-thread still active while new one is initializing and
	// maybe on other occasions, too)
	if (   (_visible_peak_power.size() != _peak_signal.size())
			|| (_max_peak_power.size()     != _peak_signal.size())
			|| (_max_peak_signal.size()    != _peak_signal.size())
			 ) {
		return;
	}

	const size_t limit = min (_peak_signal.size(), (size_t) current_meters.n_total ());
	const size_t n_midi  = min (_peak_signal.size(), (size_t) current_meters.n_midi());

	/* 0.01f ^= 100 Hz update rate */
	const float midi_meter_falloff = Config->get_meter_falloff() * 0.01f;
	/* kmeters: 24dB / 2 sec */
	const float audio_meter_falloff = (_meter_type & (MeterK20 | MeterK14 | MeterK12)) ? 0.12f : midi_meter_falloff;

	for (size_t n = 0; n < limit; ++n) {

		/* grab peak since last read */

		float new_peak = _peak_signal[n]; /* XXX we should use atomic exchange from here ... */
		_peak_signal[n] = 0;              /* ... to here */

		if (n < n_midi) {
			_max_peak_power[n] = -std::numeric_limits<float>::infinity(); // std::max (new_peak, _max_peak_power[n]); // XXX
			_max_peak_signal[n] = 0;
			if (midi_meter_falloff == 0.0f || new_peak > _visible_peak_power[n]) {
				;
			} else {
				/* empirical algorithm WRT to audio falloff times */
				new_peak = _visible_peak_power[n] - sqrt(_visible_peak_power[n] * midi_meter_falloff * 0.0002f);
				if (new_peak < (1.0 / 512.0)) new_peak = 0;
			}
			_visible_peak_power[n] = new_peak;
			continue;
		}

		/* AUDIO */

		/* compute new visible value using falloff */

		_max_peak_signal[n] = std::max(new_peak, _max_peak_signal[n]);

		if (new_peak > 0.0) {
			new_peak = accurate_coefficient_to_dB (new_peak);
		} else {
			new_peak = minus_infinity();
		}

		/* update max peak */

		_max_peak_power[n] = std::max (new_peak, _max_peak_power[n]);

		if (audio_meter_falloff == 0.0f || new_peak > _visible_peak_power[n]) {
			_visible_peak_power[n] = new_peak;
		} else {
			// do falloff
			new_peak = _visible_peak_power[n] - (audio_meter_falloff);
			_visible_peak_power[n] = std::max (new_peak, -std::numeric_limits<float>::infinity());
		}
	}
}

#define CHECKSIZE(MTR) (n < MTR.size() + n_midi && n >= n_midi)

float
PeakMeter::meter_level(uint32_t n, MeterType type) {
	switch (type) {
		case MeterKrms:
		case MeterK20:
		case MeterK14:
		case MeterK12:
			{
				const uint32_t n_midi = current_meters.n_midi();
				if (CHECKSIZE(_kmeter)) {
					return accurate_coefficient_to_dB (_kmeter[n - n_midi]->read());
				}
			}
			break;
		case MeterIEC1DIN:
		case MeterIEC1NOR:
			{
				const uint32_t n_midi = current_meters.n_midi();
				if (CHECKSIZE(_iec1meter)) {
					return accurate_coefficient_to_dB (_iec1meter[n - n_midi]->read());
				}
			}
			break;
		case MeterIEC2BBC:
		case MeterIEC2EBU:
			{
				const uint32_t n_midi = current_meters.n_midi();
				if (CHECKSIZE(_iec2meter)) {
					return accurate_coefficient_to_dB (_iec2meter[n - n_midi]->read());
				}
			}
			break;
		case MeterVU:
			{
				const uint32_t n_midi = current_meters.n_midi();
				if (CHECKSIZE(_vumeter)) {
					return accurate_coefficient_to_dB (_vumeter[n - n_midi]->read());
				}
			}
			break;
		case MeterPeak:
			return peak_power(n);
		case MeterMaxSignal:
			if (n < _max_peak_signal.size()) {
				return _max_peak_signal[n];
			}
			break;
		default:
		case MeterMaxPeak:
			if (n < _max_peak_power.size()) {
				return _max_peak_power[n];
			}
			break;
	}
	return minus_infinity();
}

void
PeakMeter::set_type(MeterType t)
{
	if (t == _meter_type) {
		return;
	}

	_meter_type = t;

	if (t & (MeterKrms | MeterK20 | MeterK14 | MeterK12)) {
		const size_t n_audio = current_meters.n_audio();
		for (size_t n = 0; n < n_audio; ++n) {
			_kmeter[n]->reset();
		}
	}
	if (t & (MeterIEC1DIN | MeterIEC1NOR)) {
		const size_t n_audio = current_meters.n_audio();
		for (size_t n = 0; n < n_audio; ++n) {
			_iec1meter[n]->reset();
		}
	}
	if (t & (MeterIEC2BBC | MeterIEC2EBU)) {
		const size_t n_audio = current_meters.n_audio();
		for (size_t n = 0; n < n_audio; ++n) {
			_iec2meter[n]->reset();
		}
	}
	if (t & MeterVU) {
		const size_t n_audio = current_meters.n_audio();
		for (size_t n = 0; n < n_audio; ++n) {
			_vumeter[n]->reset();
		}
	}

	TypeChanged(t);
}

XMLNode&
PeakMeter::state (bool full_state)
{
	XMLNode& node (Processor::state (full_state));
	node.add_property("type", "meter");
	return node;
}
