/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <cassert>

#ifdef _WIN32
// Windows: Avoid boost/thread namespace pollution (which includes windows.h).
#define NOGDI
#define NORESOURCE
#endif
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "storesession.hpp"

#include <pv/devicemanager.hpp>
#include <pv/session.hpp>
#include <pv/data/analog.hpp>
#include <pv/data/analogsegment.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/logicsegment.hpp>
#include <pv/data/signalbase.hpp>
#include <pv/devices/device.hpp>
#include <pv/view/signal.hpp>

#include <libsigrokcxx/libsigrokcxx.hpp>

using boost::shared_lock;
using boost::shared_mutex;

using std::deque;
using std::dynamic_pointer_cast;
using std::ios_base;
using std::lock_guard;
using std::make_pair;
using std::map;
using std::min;
using std::mutex;
using std::pair;
using std::set;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unordered_set;
using std::vector;

using Glib::VariantBase;

using sigrok::ConfigKey;
using sigrok::Error;
using sigrok::OutputFormat;
using sigrok::OutputFlag;

namespace pv {

const size_t StoreSession::BlockSize = 1024 * 1024;

StoreSession::StoreSession(const std::string &file_name,
	const shared_ptr<OutputFormat> &output_format,
	const map<string, VariantBase> &options,
	const std::pair<uint64_t, uint64_t> sample_range,
	const Session &session) :
	file_name_(file_name),
	output_format_(output_format),
	options_(options),
	sample_range_(sample_range),
	session_(session),
	interrupt_(false),
	units_stored_(0),
	unit_count_(0)
{
}

StoreSession::~StoreSession()
{
	wait();
}

pair<int, int> StoreSession::progress() const
{
	return make_pair(units_stored_.load(), unit_count_.load());
}

const QString& StoreSession::error() const
{
	lock_guard<mutex> lock(mutex_);
	return error_;
}

bool StoreSession::start()
{
	const unordered_set< shared_ptr<view::Signal> > sigs(session_.signals());

	shared_ptr<data::Segment> any_segment;
	shared_ptr<data::LogicSegment> lsegment;
	vector< shared_ptr<data::SignalBase> > achannel_list;
	vector< shared_ptr<data::AnalogSegment> > asegment_list;

	for (shared_ptr<view::Signal> signal : sigs) {
		if (!signal->enabled())
			continue;

		shared_ptr<data::SignalData> data = signal->data();

		if (dynamic_pointer_cast<data::Logic>(data)) {
			// All logic channels share the same data segments
			shared_ptr<data::Logic> ldata = dynamic_pointer_cast<data::Logic>(data);

			const deque< shared_ptr<data::LogicSegment> > &lsegments =
				ldata->logic_segments();

			if (lsegments.empty()) {
				error_ = tr("Can't save logic channel without data.");
				return false;
			}

			lsegment = lsegments.front();
			any_segment = lsegment;
		}

		if (dynamic_pointer_cast<data::Analog>(data)) {
			// Each analog channel has its own segments
			shared_ptr<data::Analog> adata =
				dynamic_pointer_cast<data::Analog>(data);

			const deque< shared_ptr<data::AnalogSegment> > &asegments =
				adata->analog_segments();

			if (asegments.empty()) {
				error_ = tr("Can't save analog channel without data.");
				return false;
			}

			asegment_list.push_back(asegments.front());
			any_segment = asegments.front();

			achannel_list.push_back(signal->channel());
		}
	}

	if (!any_segment) {
		error_ = tr("No channels enabled.");
		return false;
	}

	// Check whether the user wants to export a certain sample range
	uint64_t end_sample;

	if (sample_range_.first == sample_range_.second) {
		start_sample_ = 0;
		sample_count_ =	any_segment->get_sample_count();
	} else {
		if (sample_range_.first > sample_range_.second) {
			start_sample_ = sample_range_.second;
			end_sample = min(sample_range_.first, any_segment->get_sample_count());
			sample_count_ = end_sample - start_sample_;
		} else {
			start_sample_ = sample_range_.first;
			end_sample = min(sample_range_.second, any_segment->get_sample_count());
			sample_count_ = end_sample - start_sample_;
		}
	}

	// Begin storing
	try {
		const auto context = session_.device_manager().context();
		auto device = session_.device()->device();

		map<string, Glib::VariantBase> options = options_;

		if (!output_format_->test_flag(OutputFlag::INTERNAL_IO_HANDLING))
			output_stream_.open(file_name_, ios_base::binary |
					ios_base::trunc | ios_base::out);

		output_ = output_format_->create_output(file_name_, device, options);
		auto meta = context->create_meta_packet(
			{{ConfigKey::SAMPLERATE, Glib::Variant<guint64>::create(
				any_segment->samplerate())}});
		output_->receive(meta);
	} catch (Error error) {
		error_ = tr("Error while saving: ") + error.what();
		return false;
	}

	thread_ = std::thread(&StoreSession::store_proc, this,
		achannel_list, asegment_list, lsegment);
	return true;
}

void StoreSession::wait()
{
	if (thread_.joinable())
		thread_.join();
}

void StoreSession::cancel()
{
	interrupt_ = true;
}

void StoreSession::store_proc(vector< shared_ptr<data::SignalBase> > achannel_list,
	vector< shared_ptr<data::AnalogSegment> > asegment_list,
	shared_ptr<data::LogicSegment> lsegment)
{
	unsigned progress_scale = 0;

	/// TODO: Wrap this in a std::unique_ptr when we transition to C++11
	uint8_t *const ldata = new uint8_t[BlockSize];
	assert(ldata);

	int aunit_size = 0;
	int lunit_size = 0;
	unsigned int lsamples_per_block = INT_MAX;
	unsigned int asamples_per_block = INT_MAX;

	if (!asegment_list.empty()) {
		// We assume all analog channels use the sample unit size
		aunit_size = asegment_list.front()->unit_size();
		asamples_per_block = BlockSize / aunit_size;
	}
	if (lsegment) {
		lunit_size = lsegment->unit_size();
		lsamples_per_block = BlockSize / lunit_size;
	}

	// Qt needs the progress values to fit inside an int. If they would
	// not, scale the current and max values down until they do.
	while ((sample_count_ >> progress_scale) > INT_MAX)
		progress_scale ++;

	unit_count_ = sample_count_ >> progress_scale;

	const unsigned int samples_per_block =
		std::min(asamples_per_block, lsamples_per_block);

	while (!interrupt_ && sample_count_) {
		progress_updated();

		const uint64_t packet_len =
			std::min((uint64_t)samples_per_block, sample_count_);

		try {
			const auto context = session_.device_manager().context();

			for (unsigned int i = 0; i < achannel_list.size(); i++) {
				shared_ptr<sigrok::Channel> achannel = (achannel_list.at(i))->channel();
				shared_ptr<data::AnalogSegment> asegment = asegment_list.at(i);

				const float *adata =
					asegment->get_samples(start_sample_, start_sample_ + packet_len);

				// The srzip format currently only supports packets with one
				// analog channel. See zip_append_analog() in srzip.c
				auto analog = context->create_analog_packet(
					vector<shared_ptr<sigrok::Channel> >{achannel},
					(float *)adata, packet_len,
					sigrok::Quantity::VOLTAGE, sigrok::Unit::VOLT,
					vector<const sigrok::QuantityFlag *>());
				const string adata_str = output_->receive(analog);

				if (output_stream_.is_open())
					output_stream_ << adata_str;

				delete[] adata;
			}

			if (lsegment) {
				lsegment->get_samples(ldata, start_sample_, start_sample_ + packet_len);

				const size_t length = packet_len * lunit_size;
				auto logic = context->create_logic_packet(ldata, length, lunit_size);
				const string ldata_str = output_->receive(logic);

				if (output_stream_.is_open())
					output_stream_ << ldata_str;
			}
		} catch (Error error) {
			error_ = tr("Error while saving: ") + error.what();
			break;
		}

		sample_count_ -= packet_len;
		start_sample_ += packet_len;
		units_stored_ = unit_count_ - (sample_count_ >> progress_scale);
	}

	// Zeroing the progress variables indicates completion
	units_stored_ = unit_count_ = 0;

	progress_updated();

	output_.reset();
	output_stream_.close();

	delete[] ldata;
}

} // pv
