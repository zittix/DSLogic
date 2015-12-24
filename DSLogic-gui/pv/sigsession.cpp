/*
 * This file is part of the DSLogic-gui project.
 * DSLogic-gui is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <dreamsourcelab@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef ENABLE_DECODE
#include <libsigrokdecode/libsigrokdecode.h>
#endif

#include "sigsession.h"
#include "mainwindow.h"

#include "devicemanager.h"
#include "device/device.h"
#include "device/file.h"

#include "data/analog.h"
#include "data/analogsnapshot.h"
#include "data/dso.h"
#include "data/dsosnapshot.h"
#include "data/logic.h"
#include "data/logicsnapshot.h"
#include "data/group.h"
#include "data/groupsnapshot.h"
#include "data/decoderstack.h"
#include "data/decode/decoder.h"

#include "view/analogsignal.h"
#include "view/dsosignal.h"
#include "view/logicsignal.h"
#include "view/groupsignal.h"
#include "view/decodetrace.h"

#include <assert.h>
#include <stdexcept>
#include <sys/stat.h>
#include <extdef.h>

#include <math.h>

#include <QDebug>
#include <QMessageBox>

#include <boost/foreach.hpp>
#include <sstream>
#include <fstream>
 
using boost::dynamic_pointer_cast;
using boost::function;
using boost::lock_guard;
using boost::mutex;
using boost::shared_ptr;
using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;
using std::deque;
using std::min;

namespace pv {

const float SigSession::Oversampling = 2.0f;

// TODO: This should not be necessary
SigSession* SigSession::_session = NULL;

SigSession::SigSession(DeviceManager &device_manager) :
	_device_manager(device_manager),
    _capture_state(Init),
    _instant(false)
{
	// TODO: This should not be necessary
	_session = this;
    _hot_attach = false;
    _hot_detach = false;
    _adv_trigger = false;
    _group_cnt = 0;
    ds_trigger_init();
    register_hotplug_callback();
}

SigSession::~SigSession()
{
	stop_capture();       
    if (_hotplug_handle) {
        stop_hotplug_proc();
        deregister_hotplug_callback();
    }
    ds_trigger_destroy();

    _dev_inst->release();

	// TODO: This should not be necessary
	_session = NULL;
}

boost::shared_ptr<device::DevInst> SigSession::get_device() const
{
    return _dev_inst;
}

void SigSession::set_device(shared_ptr<device::DevInst> dev_inst) throw(QString)
{
    using pv::device::Device;

    // Ensure we are not capturing before setting the device
    stop_capture();

    if (_dev_inst) {
        sr_session_datafeed_callback_remove_all();
        _dev_inst->release();
    }

    _dev_inst = dev_inst;
#ifdef ENABLE_DECODE
    _decode_traces.clear();
#endif
    _group_traces.clear();

    if (_dev_inst) {
        _dev_inst->use(this);
        sr_session_datafeed_callback_add(data_feed_in_proc, NULL);
        device_setted();
    }
}


void SigSession::set_file(const string &name) throw(QString)
{
    // Deslect the old device, because file type detection in File::create
    // destorys the old session inside libsigrok.
    set_device(shared_ptr<device::DevInst>());
    set_device(shared_ptr<device::DevInst>(device::File::create(name)));
}

void SigSession::save_file(const std::string &name){
    if (get_device()->dev_inst()->mode == LOGIC) {
        const deque< boost::shared_ptr<pv::data::LogicSnapshot> > &snapshots =
        _logic_data->get_snapshots();
        if (snapshots.empty())
            return;

        const boost::shared_ptr<pv::data::LogicSnapshot> &snapshot =
            snapshots.front();

        sr_session_save(name.c_str(), _dev_inst->dev_inst(),
                        (unsigned char*)snapshot->get_data(),
                        snapshot->unit_size(),
                        snapshot->get_sample_count());
    } else if (get_device()->dev_inst()->mode == DSO) {
        const deque< boost::shared_ptr<pv::data::DsoSnapshot> > &snapshots =
            _dso_data->get_snapshots();
        
        if (snapshots.empty())
            return NULL;

        const boost::shared_ptr<pv::data::DsoSnapshot> &snapshot =
            snapshots.front();

        


        const uint16_t number_channels = snapshot->get_channel_num();

        const double samplerate = _dev_inst->get_sample_rate();

        const unsigned long long last_sample = std::max((unsigned long long)(snapshot->get_sample_count() - 1), (unsigned long long)0);

        const uint8_t *const samples = snapshot->get_samples(0, last_sample, 0);
        assert(samples);

        
        

        std::ofstream ss(name.c_str());

        ss << std::setprecision( std::numeric_limits<int>::max() );

        ss << "Time (ms),";
        for(uint16_t ch=0;ch<number_channels;ch++) {
            ss << "Channel " << ch << " (V)";
            
            if(ch+1<number_channels) {
                ss << ",";
            } else {
                ss << std::endl;
            }
        }
        
        unsigned int ii=0;

        uint64_t valueVDivs[number_channels];

        for (GSList* l = _dev_inst->dev_inst()->channels; l && ii<number_channels; l = l->next, ii++) {
            sr_channel *const probe = (sr_channel*)l->data;
            assert(probe);

            GVariant* gvar = _dev_inst->get_config(probe, NULL, SR_CONF_VDIV);


            if (gvar != NULL) {
                valueVDivs[ii] = g_variant_get_uint64(gvar);
                g_variant_unref(gvar);
            } else {
                valueVDivs[ii]=0;
            }
        }

        for(unsigned long long i=0;i<=last_sample;i++) {
            ss << i/(double)samplerate*1000.0 << ",";
            for(uint16_t ch=0;ch<number_channels;ch++) {
        
                uint8_t sample = samples[i*number_channels+ch];
                double sample_int = sample/255.0-0.5;

                double value = (valueVDivs[ch]*(DS_CONF_DSO_VDIVS))*sample_int/1000.0;

                ss << value;

                if(ch+1<number_channels) {
                    ss << ",";
                } else {
                    ss << std::endl;
                }
            }
        }

        //std::cout << ss.str();


    }
}

void SigSession::set_default_device()
{
    shared_ptr<pv::device::DevInst> default_device;
    const list< shared_ptr<device::DevInst> > &devices =
        _device_manager.devices();

    if (!devices.empty()) {
        // Fall back to the first device in the list.
        default_device = devices.front();

        // Try and find the DSLogic device and select that by default
        BOOST_FOREACH (shared_ptr<pv::device::DevInst> dev, devices)
            if (dev->dev_inst() &&
                strcmp(dev->dev_inst()->driver->name,
                "DSLogic") == 0) {
                default_device = dev;
                break;
            }
        set_device(default_device);
    }
}

void SigSession::release_device(device::DevInst *dev_inst)
{
    (void)dev_inst;
    assert(_dev_inst.get() == dev_inst);

    assert(_capture_state != Running);
    _dev_inst = shared_ptr<device::DevInst>();
    //_dev_inst.reset();
}

SigSession::capture_state SigSession::get_capture_state() const
{
	boost::lock_guard<boost::mutex> lock(_sampling_mutex);
	return _capture_state;
}

void SigSession::start_capture(bool instant,
    boost::function<void (const QString)> error_handler)
{
	stop_capture();

	// Check that a device instance has been selected.
    if (!_dev_inst) {
		qDebug() << "No device selected";
		return;
	}

    assert(_dev_inst->dev_inst());

	// Check that at least one probe is enabled
	const GSList *l;
    for (l = _dev_inst->dev_inst()->channels; l; l = l->next) {
        sr_channel *const probe = (sr_channel*)l->data;
		assert(probe);
		if (probe->enabled)
			break;
	}
	if (!l) {
		error_handler(tr("No probes enabled."));
		return;
	}

	// Begin the session
    _instant = instant;
	_sampling_thread.reset(new boost::thread(
        &SigSession::sample_thread_proc, this, _dev_inst,
        error_handler));
}

void SigSession::stop_capture()
{
    if (get_capture_state() != Running)
		return;
	sr_session_stop();

	// Check that sampling stopped
	if (_sampling_thread.get())
		_sampling_thread->join();
	_sampling_thread.reset();
}

vector< boost::shared_ptr<view::Signal> > SigSession::get_signals()
{
	boost::lock_guard<boost::mutex> lock(_signals_mutex);
	return _signals;
}

vector< boost::shared_ptr<view::GroupSignal> > SigSession::get_group_signals()
{
    boost::lock_guard<boost::mutex> lock(_signals_mutex);
    return _group_traces;
}

set< shared_ptr<data::SignalData> > SigSession::get_data() const
{
    lock_guard<mutex> lock(_signals_mutex);
    set< shared_ptr<data::SignalData> > data;
    BOOST_FOREACH(const shared_ptr<view::Signal> sig, _signals) {
        assert(sig);
        data.insert(sig->data());
    }

    return data;
}

void* SigSession::get_buf(int& unit_size, uint64_t &length)
{
    if (_dev_inst->dev_inst()->mode == LOGIC) {
        const deque< boost::shared_ptr<pv::data::LogicSnapshot> > &snapshots =
            _logic_data->get_snapshots();
        if (snapshots.empty())
            return NULL;

        const boost::shared_ptr<pv::data::LogicSnapshot> &snapshot =
            snapshots.front();

        unit_size = snapshot->unit_size();
        length = snapshot->get_sample_count();
        return snapshot->get_data();
    } else if (_dev_inst->dev_inst()->mode == DSO) {
        const deque< boost::shared_ptr<pv::data::DsoSnapshot> > &snapshots =
            _dso_data->get_snapshots();
        if (snapshots.empty())
            return NULL;

        const boost::shared_ptr<pv::data::DsoSnapshot> &snapshot =
            snapshots.front();

        unit_size = snapshot->unit_size();
        length = snapshot->get_sample_count();
        return snapshot->get_data();
    } else {
        const deque< boost::shared_ptr<pv::data::AnalogSnapshot> > &snapshots =
            _analog_data->get_snapshots();
        if (snapshots.empty())
            return NULL;

        const boost::shared_ptr<pv::data::AnalogSnapshot> &snapshot =
            snapshots.front();

        unit_size = snapshot->unit_size();
        length = snapshot->get_sample_count();
        return snapshot->get_data();
    }
}

void SigSession::set_sample_rate(uint64_t sample_rate)
{
    if (_capture_state != Stopped)
        sample_rate_changed(sample_rate);
}

void SigSession::set_capture_state(capture_state state)
{
	boost::lock_guard<boost::mutex> lock(_sampling_mutex);
	_capture_state = state;
    data_updated();
	capture_state_changed(state);
}

void SigSession::sample_thread_proc(shared_ptr<device::DevInst> dev_inst,
                                    boost::function<void (const QString)> error_handler)
{
    assert(dev_inst);
    assert(dev_inst->dev_inst());
    assert(error_handler);

    if (_instant) {
        /* disable trigger under instant mode */
        ds_trigger_set_en(false);
    } else if (!_adv_trigger) {
        /* simple trigger check trigger_enable */
        ds_trigger_set_en(false);
        BOOST_FOREACH(const boost::shared_ptr<view::Signal> s, _signals)
        {
            assert(s);
            if (s->get_trig() != 0) {
                ds_trigger_set_en(true);
                s->set_trig(s->get_trig());
            }
        }
    } else {
        /* advanced trigger check trigger_enable */
        ds_trigger_set_en(true);
    }

    try {
        dev_inst->start();
    } catch(const QString e) {
        error_handler(e);
        return;
    }

    receive_data(0);
    set_capture_state(Running);

    dev_inst->run();
    set_capture_state(Stopped);

    // Confirm that SR_DF_END was received
    assert(!_cur_logic_snapshot);
    assert(!_cur_dso_snapshot);
    assert(!_cur_analog_snapshot);
}

void SigSession::read_sample_rate(const sr_dev_inst *const sdi)
{
    GVariant *gvar;
    uint64_t sample_rate = 0;

    // Read out the sample rate
    if(sdi->driver)
    {
        const int ret = sr_config_get(sdi->driver, sdi, NULL, NULL, SR_CONF_SAMPLERATE, &gvar);
        if (ret != SR_OK) {
            qDebug("Failed to get samplerate\n");
            return;
        }

        sample_rate = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }

    // Set the sample rate of all data
    const set< shared_ptr<data::SignalData> > data_set = get_data();
    BOOST_FOREACH(shared_ptr<data::SignalData> data, data_set) {
        assert(data);
        data->set_samplerate(sample_rate);
    }
}

void SigSession::feed_in_header(const sr_dev_inst *sdi)
{
    read_sample_rate(sdi);
    //receive_data(0);
}

void SigSession::add_group()
{
    std::list<int> probe_index_list;

    std::vector< boost::shared_ptr<view::Signal> >::iterator i = _signals.begin();
    while (i != _signals.end()) {
        if ((*i)->get_type() == view::Trace::DS_LOGIC && (*i)->selected())
            probe_index_list.push_back((*i)->get_index());
        i++;
    }

    if (probe_index_list.size() > 1) {
        //_group_data.reset(new data::Group(_last_sample_rate));
        if (_group_data->get_snapshots().empty())
            _group_data->set_samplerate(_dev_inst->get_sample_rate());
        const boost::shared_ptr<view::GroupSignal> signal(
                    new view::GroupSignal("New Group",
                                          _group_data, probe_index_list, _group_cnt));
        _group_traces.push_back(signal);
        _group_cnt++;

        if (_capture_state == Stopped) {
            if (!_cur_group_snapshot)
            {
                // Create a new data snapshot
                _cur_group_snapshot = boost::shared_ptr<data::GroupSnapshot>(
                            new data::GroupSnapshot(_logic_data->get_snapshots().front(), signal->get_index_list()));
                //_cur_group_snapshot->append_payload();
                _group_data->push_snapshot(_cur_group_snapshot);
                _cur_group_snapshot.reset();
            }
        }

        signals_changed();
        data_updated();
    }
}

void SigSession::del_group()
{
    std::vector< boost::shared_ptr<view::GroupSignal> >::iterator i = _group_traces.begin();
    while (i != _group_traces.end()) {
        if ((*i)->selected()) {
            std::vector< boost::shared_ptr<view::GroupSignal> >::iterator j = _group_traces.begin();
            while(j != _group_traces.end()) {
                if ((*j)->get_sec_index() > (*i)->get_sec_index())
                    (*j)->set_sec_index((*j)->get_sec_index() - 1);
                j++;
            }

            _group_data->get_snapshots().at((*i)->get_sec_index()).reset();
            std::deque< boost::shared_ptr<data::GroupSnapshot> >::iterator k = _group_data->get_snapshots().begin();
            k += (*i)->get_sec_index();
            _group_data->get_snapshots().erase(k);

            (*i).reset();
            i = _group_traces.erase(i);

            _group_cnt--;
            continue;
        }
        i++;
    }

    signals_changed();
    data_updated();
}

void SigSession::init_signals()
{
    assert(_dev_inst);
    stop_capture();

    boost::shared_ptr<view::Signal> signal;
    unsigned int logic_probe_count = 0;
    unsigned int dso_probe_count = 0;
    unsigned int analog_probe_count = 0;

#ifdef ENABLE_DECODE
    // Clear the decode traces
    _decode_traces.clear();
#endif

    // Detect what data types we will receive
    if(_dev_inst) {
        assert(_dev_inst->dev_inst());
        for (const GSList *l = _dev_inst->dev_inst()->channels;
            l; l = l->next) {
            const sr_channel *const probe = (const sr_channel *)l->data;

            switch(probe->type) {
            case SR_CHANNEL_LOGIC:
                if(probe->enabled)
                    logic_probe_count++;
                break;

            case SR_CHANNEL_DSO:
                dso_probe_count++;
                break;

            case SR_CHANNEL_ANALOG:
                if(probe->enabled)
                    analog_probe_count++;
                break;
            }
        }
    }

    // Create data containers for the coming data snapshots
    {
        if (logic_probe_count != 0) {
            _logic_data.reset(new data::Logic(logic_probe_count));
            assert(_logic_data);

            _group_data.reset(new data::Group());
            assert(_group_data);
            _group_cnt = 0;
        }

        if (dso_probe_count != 0) {
            _dso_data.reset(new data::Dso(dso_probe_count));
            assert(_dso_data);
        }

        if (analog_probe_count != 0) {
            _analog_data.reset(new data::Analog(analog_probe_count));
            assert(_analog_data);
        }
    }

    // Make the logic probe list
    {
        _signals.clear();

        for (const GSList *l = _dev_inst->dev_inst()->channels; l; l = l->next) {
            const sr_channel *const probe =
                (const sr_channel *)l->data;
            assert(probe);
            signal.reset();
            switch(probe->type) {
            case SR_CHANNEL_LOGIC:
                if (probe->enabled)
                    signal = boost::shared_ptr<view::Signal>(
                        new view::LogicSignal(_dev_inst, _logic_data, probe));
                break;

            case SR_CHANNEL_DSO:
                signal = shared_ptr<view::Signal>(
                    new view::DsoSignal(_dev_inst, _dso_data, probe));
                break;

            case SR_CHANNEL_ANALOG:
                if (probe->enabled)
                    signal = shared_ptr<view::Signal>(
                        new view::AnalogSignal(_dev_inst, _analog_data, probe));
                break;
            }
            if(signal.get())
                _signals.push_back(signal);
        }
        signals_changed();
        data_updated();
    }
}

void SigSession::reload()
{
    assert(_dev_inst);

    if (_capture_state == Running)
        stop_capture();

    boost::shared_ptr<view::Signal> signal;

    // Make the logic probe list
    {
        _signals.clear();

        for (const GSList *l = _dev_inst->dev_inst()->channels; l; l = l->next) {
            const sr_channel *const probe =
                (const sr_channel *)l->data;
            assert(probe);
            signal.reset();
            switch(probe->type) {
            case SR_CHANNEL_LOGIC:
                if (probe->enabled)
                    signal = boost::shared_ptr<view::Signal>(
                        new view::LogicSignal(_dev_inst, _logic_data, probe));
                break;

            case SR_CHANNEL_DSO:
                signal = shared_ptr<view::Signal>(
                    new view::DsoSignal(_dev_inst,_dso_data, probe));
                break;

            case SR_CHANNEL_ANALOG:
                if (probe->enabled)
                    signal = shared_ptr<view::Signal>(
                        new view::AnalogSignal(_dev_inst, _analog_data, probe));
                break;
            }
            if (signal.get())
                _signals.push_back(signal);
        }
    }

    signals_changed();
}

void SigSession::feed_in_meta(const sr_dev_inst *sdi,
    const sr_datafeed_meta &meta)
{
	(void)sdi;

	for (const GSList *l = meta.config; l; l = l->next) {
        const sr_config *const src = (const sr_config*)l->data;
		switch (src->key) {
		case SR_CONF_SAMPLERATE:
			/// @todo handle samplerate changes
			/// samplerate = (uint64_t *)src->value;
			break;
		default:
			// Unknown metadata is not an error.
			break;
		}
	}
}

void SigSession::feed_in_trigger(const ds_trigger_pos &trigger_pos)
{
    receive_trigger(trigger_pos.real_pos);
}

void SigSession::feed_in_logic(const sr_datafeed_logic &logic)
{
	boost::lock_guard<boost::mutex> lock(_data_mutex);

	if (!_logic_data)
	{
		qDebug() << "Unexpected logic packet";
		return;
	}

    if (logic.data_error == 1) {
        test_data_error();
    }

	if (!_cur_logic_snapshot)
	{
		// Create a new data snapshot
		_cur_logic_snapshot = boost::shared_ptr<data::LogicSnapshot>(
            new data::LogicSnapshot(logic, _dev_inst->get_sample_limit(), 1));
        if (_cur_logic_snapshot->buf_null())
        {
            stop_capture();
        } else {
            _logic_data->push_snapshot(_cur_logic_snapshot);
        }

        // @todo Putting this here means that only listeners querying
        // for logic will be notified. Currently the only user of
        // frame_began is DecoderStack, but in future we need to signal
        // this after both analog and logic sweeps have begun.
        frame_began();
	}
	else
	{
		// Append to the existing data snapshot
		_cur_logic_snapshot->append_payload(logic);
	}

    receive_data(logic.length/logic.unitsize);
    data_received();
    //data_updated();
}

void SigSession::feed_in_dso(const sr_datafeed_dso &dso)
{
    boost::lock_guard<boost::mutex> lock(_data_mutex);

    if(!_dso_data)
    {
        qDebug() << "Unexpected dso packet";
        return;	// This dso packet was not expected.
    }

    if (!_cur_dso_snapshot)
    {
        // Create a new data snapshot
        _cur_dso_snapshot = boost::shared_ptr<data::DsoSnapshot>(
            new data::DsoSnapshot(dso, _dev_inst->get_sample_limit(), _dso_data->get_num_probes()));
        if (_cur_dso_snapshot->buf_null())
            stop_capture();
        else
            _dso_data->push_snapshot(_cur_dso_snapshot);
    }
    else
    {
        // Append to the existing data snapshot
        _cur_dso_snapshot->append_payload(dso);
    }

    receive_data(dso.num_samples);
    data_updated();
}

void SigSession::feed_in_analog(const sr_datafeed_analog &analog)
{
	boost::lock_guard<boost::mutex> lock(_data_mutex);

	if(!_analog_data)
	{
		qDebug() << "Unexpected analog packet";
		return;	// This analog packet was not expected.
	}

	if (!_cur_analog_snapshot)
	{
		// Create a new data snapshot
		_cur_analog_snapshot = boost::shared_ptr<data::AnalogSnapshot>(
            new data::AnalogSnapshot(analog, _dev_inst->get_sample_limit(), _analog_data->get_num_probes()));
        if (_cur_analog_snapshot->buf_null())
            stop_capture();
        else
            _analog_data->push_snapshot(_cur_analog_snapshot);
	}
	else
	{
		// Append to the existing data snapshot
		_cur_analog_snapshot->append_payload(analog);
	}

    receive_data(analog.num_samples);
    data_updated();
}

void SigSession::data_feed_in(const struct sr_dev_inst *sdi,
    const struct sr_datafeed_packet *packet)
{
	assert(sdi);
	assert(packet);

	switch (packet->type) {
	case SR_DF_HEADER:
		feed_in_header(sdi);
		break;

	case SR_DF_META:
		assert(packet->payload);
		feed_in_meta(sdi,
            *(const sr_datafeed_meta*)packet->payload);
		break;

    case SR_DF_TRIGGER:
        assert(packet->payload);
        feed_in_trigger(*(const ds_trigger_pos*)packet->payload);
        break;

	case SR_DF_LOGIC:
		assert(packet->payload);
        feed_in_logic(*(const sr_datafeed_logic*)packet->payload);
		break;

    case SR_DF_DSO:
        assert(packet->payload);
        feed_in_dso(*(const sr_datafeed_dso*)packet->payload);
        break;

	case SR_DF_ANALOG:
		assert(packet->payload);
        feed_in_analog(*(const sr_datafeed_analog*)packet->payload);
		break;

	case SR_DF_END:
	{
		{
            boost::lock_guard<boost::mutex> lock(_data_mutex);
            BOOST_FOREACH(const boost::shared_ptr<view::GroupSignal> g, _group_traces)
            {
                assert(g);

                _cur_group_snapshot = boost::shared_ptr<data::GroupSnapshot>(
                            new data::GroupSnapshot(_logic_data->get_snapshots().front(), g->get_index_list()));
                _group_data->push_snapshot(_cur_group_snapshot);
            }
            _cur_logic_snapshot.reset();
            _cur_dso_snapshot.reset();
            _cur_analog_snapshot.reset();
		}
#ifdef ENABLE_DECODE
        for (vector< shared_ptr<view::DecodeTrace> >::iterator i =
            _decode_traces.begin();
            i != _decode_traces.end();
            i++)
            (*i)->decoder()->stop_decode();
#endif
        frame_ended();
		break;
	}
	}
}

void SigSession::data_feed_in_proc(const struct sr_dev_inst *sdi,
    const struct sr_datafeed_packet *packet, void *cb_data)
{
	(void) cb_data;
	assert(_session);
	_session->data_feed_in(sdi, packet);
}

/*
 * hotplug function
 */
int SigSession::hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev, 
                                  libusb_hotplug_event event, void *user_data) {

    (void)ctx;
    (void)dev;
    (void)user_data;

    if (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED == event) {
        _session->_hot_attach = true;
        qDebug("DSLogic attached!\n");
    }else if (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT == event) {
        _session->_hot_detach = true;
        qDebug("DSLogic detached!\n");
    }else{
        qDebug("Unhandled event %d\n", event);
    }

    return 0;
}

void SigSession::hotplug_proc(boost::function<void (const QString)> error_handler)
{
    struct timeval tv;

    (void)error_handler;

    if (!_dev_inst)
        return;

    tv.tv_sec = tv.tv_usec = 0;
    try {
        while(_session) {
            libusb_handle_events_timeout(NULL, &tv);
            if (_hot_attach) {
                qDebug("DSLogic hardware attached!");
                device_attach();
                _hot_attach = false;
            }
            if (_hot_detach) {
                qDebug("DSLogic hardware detached!");
                device_detach();
                _logic_data.reset();
                _dso_data.reset();
                _analog_data.reset();
                _hot_detach = false;
            }
            boost::this_thread::sleep(boost::posix_time::millisec(100));
        }
    } catch(...) {
        qDebug("Interrupt exception for hotplug thread was thrown.");
    }
    qDebug("Hotplug thread exit!");
}

void SigSession::register_hotplug_callback()
{
    int ret;

    ret = libusb_hotplug_register_callback(NULL, (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT), 
                                           (libusb_hotplug_flag)LIBUSB_HOTPLUG_ENUMERATE, 0x2A0E, 0x0001,
                                           LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL,
                                           &_hotplug_handle);
    if (LIBUSB_SUCCESS != ret){
	qDebug() << "Error creating a hotplug callback\n";
    }
}

void SigSession::deregister_hotplug_callback()
{
    libusb_hotplug_deregister_callback(NULL, _hotplug_handle);
}

void SigSession::start_hotplug_proc(boost::function<void (const QString)> error_handler)
{

    // Begin the session
    qDebug() << "Starting a hotplug thread...\n";
    _hot_attach = false;
    _hot_detach = false;
    _hotplug.reset(new boost::thread(
        &SigSession::hotplug_proc, this, error_handler));

}

void SigSession::stop_hotplug_proc()
{
    if (_hotplug.get()) {
        _hotplug->interrupt();
        _hotplug->join();
    }
    _hotplug.reset();
}


/*
 * Tigger
 */
void SigSession::set_adv_trigger(bool adv_trigger)
{
    _adv_trigger = adv_trigger;
}


uint16_t SigSession::get_dso_ch_num()
{
    uint16_t num_channels = 0;
    BOOST_FOREACH(const shared_ptr<view::Signal> s, _signals)
    {
        assert(s);
        //if (dynamic_pointer_cast<view::DsoSignal>(s) && s->enabled()) {
        if (dynamic_pointer_cast<view::DsoSignal>(s)) {
            num_channels++;
        }
    }
    return num_channels;
}

#ifdef ENABLE_DECODE
bool SigSession::add_decoder(srd_decoder *const dec)
{
    bool ret = false;
    map<const srd_channel*, shared_ptr<view::LogicSignal> > probes;
    shared_ptr<data::DecoderStack> decoder_stack;

    try
    {
        //lock_guard<mutex> lock(_signals_mutex);

        // Create the decoder
        decoder_stack = shared_ptr<data::DecoderStack>(
            new data::DecoderStack(*this, dec));

        // Make a list of all the probes
        std::vector<const srd_channel*> all_probes;
        for(const GSList *i = dec->channels; i; i = i->next)
            all_probes.push_back((const srd_channel*)i->data);
        for(const GSList *i = dec->opt_channels; i; i = i->next)
            all_probes.push_back((const srd_channel*)i->data);

        assert(decoder_stack);
        assert(!decoder_stack->stack().empty());
        assert(decoder_stack->stack().front());
        decoder_stack->stack().front()->set_probes(probes);

        // Create the decode signal
        shared_ptr<view::DecodeTrace> d(
            new view::DecodeTrace(*this, decoder_stack,
                _decode_traces.size()));
        if (d->create_popup()) {
            _decode_traces.push_back(d);
            ret = true;
        }
    }
    catch(std::runtime_error e)
    {
        return false;
    }

    if (ret) {
        signals_changed();
        // Do an initial decode
        decoder_stack->begin_decode();
        data_updated();
    }

    return ret;
}

vector< shared_ptr<view::DecodeTrace> > SigSession::get_decode_signals() const
{
    lock_guard<mutex> lock(_signals_mutex);
    return _decode_traces;
}

void SigSession::remove_decode_signal(view::DecodeTrace *signal)
{
    for (vector< shared_ptr<view::DecodeTrace> >::iterator i =
        _decode_traces.begin();
        i != _decode_traces.end();
        i++)
        if ((*i).get() == signal)
        {
            _decode_traces.erase(i);
            signals_changed();
            return;
        }
}

void SigSession::remove_decode_signal(int index)
{
    int cur_index = 0;
    for (vector< shared_ptr<view::DecodeTrace> >::iterator i =
        _decode_traces.begin();
        i != _decode_traces.end();
        i++)
    {
        if (cur_index == index)
        {
            _decode_traces.erase(i);
            signals_changed();
            return;
        }
        cur_index++;
    }
}

void SigSession::rst_decoder(int index)
{
    int cur_index = 0;
    for (vector< shared_ptr<view::DecodeTrace> >::iterator i =
        _decode_traces.begin();
        i != _decode_traces.end();
        i++)
    {
        if (cur_index == index)
        {
            if ((*i)->create_popup())
            {
                (*i)->decoder()->stop_decode();
                (*i)->decoder()->begin_decode();
                data_updated();
            }
            return;
        }
        cur_index++;
    }
}

void SigSession::rst_decoder(view::DecodeTrace *signal)
{
    for (vector< shared_ptr<view::DecodeTrace> >::iterator i =
        _decode_traces.begin();
        i != _decode_traces.end();
        i++)
        if ((*i).get() == signal)
        {
            if ((*i)->create_popup())
            {
                (*i)->decoder()->stop_decode();
                (*i)->decoder()->begin_decode();
                data_updated();
            }
            return;
        }
}
#endif

} // namespace pv
