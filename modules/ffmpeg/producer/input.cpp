/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "..\stdafx.h"

#include "input.h"
#include "../ffmpeg_error.h"

#include <core/video_format.h>

#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>

#include <tbb/concurrent_queue.h>
#include <tbb/queuing_mutex.h>

#include <boost/exception/error_info.hpp>
#include <boost/thread/once.hpp>
#include <boost/thread/thread.hpp>
#include <boost/regex.hpp>

#include <errno.h>
#include <system_error>
		
#if defined(_MSC_VER)
#pragma warning (disable : 4244)
#endif

extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
}

namespace caspar {
		
struct input::implementation : boost::noncopyable
{		
	static const size_t PACKET_BUFFER_COUNT = 25;

	safe_ptr<diagnostics::graph> graph_;

	std::shared_ptr<AVFormatContext> format_context_;	// Destroy this last

	std::shared_ptr<AVCodecContext>	video_codec_context_;
	std::shared_ptr<AVCodecContext>	audio_codex_context_;
	
	const std::wstring filename_;

	bool loop_;
	int video_s_index_;
	int	audio_s_index_;
	const int start_frame_;
	const int end_frame_;
	int eof_count_;
		
	tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>> video_packet_buffer_;
	tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>> audio_packet_buffer_;

	boost::condition_variable cond_;
	boost::mutex mutex_;
	
	std::exception_ptr exception_;
	executor executor_;
public:
	explicit implementation(const safe_ptr<diagnostics::graph>& graph, const std::wstring& filename, bool loop, int start_frame, int end_frame) 
		: graph_(graph)
		, loop_(loop)
		, video_s_index_(-1)
		, audio_s_index_(-1)
		, filename_(filename)
		, executor_(print())
		, start_frame_(std::max(start_frame, 0))
		, end_frame_(end_frame)
		, eof_count_(end_frame-start_frame)
	{			
		if(end_frame_ > 0 && end_frame <= start_frame_)
			BOOST_THROW_EXCEPTION(
				invalid_argument() << 
				source_info(narrow(print())) << 
				msg_info("End-frame cannot be lower than start-frame."));	

		graph_->set_color("input-buffer", diagnostics::color(1.0f, 1.0f, 0.0f));
		graph_->set_color("seek", diagnostics::color(0.5f, 1.0f, 0.5f));	

		int errn;
		AVFormatContext* weak_format_context_ = nullptr;
		if((errn = av_open_input_file(&weak_format_context_, narrow(filename).c_str(), nullptr, 0, nullptr)) < 0 || weak_format_context_ == nullptr)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				source_info(narrow(print())) << 
				msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("av_open_input_file") <<
				boost::errinfo_errno(AVUNERROR(errn)) <<
				boost::errinfo_file_name(narrow(filename)));

		format_context_.reset(weak_format_context_, av_close_input_file);
			
		if((errn = av_find_stream_info(format_context_.get())) < 0)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				source_info(narrow(print())) << 
				msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("av_find_stream_info") <<
				boost::errinfo_errno(AVUNERROR(errn)));

		video_codec_context_ = open_stream(CODEC_TYPE_VIDEO, video_s_index_);
		if(!video_codec_context_)
			CASPAR_LOG(warning) << print() << " Could not open any video stream.";
		else
			fix_time_base(video_codec_context_.get());
		
		audio_codex_context_ = open_stream(CODEC_TYPE_AUDIO, audio_s_index_);
		if(!audio_codex_context_)
			CASPAR_LOG(warning) << print() << " Could not open any audio stream.";
		else
			fix_time_base(video_codec_context_.get());

		if(!video_codec_context_ && !audio_codex_context_)
			BOOST_THROW_EXCEPTION(
				file_read_error() << 
				msg_info(av_error_str(errn)) <<
				source_info(narrow(print())) << 
				msg_info("No video or audio codec context found."));	
		
		if(start_frame_ != 0)			
			seek_frame(start_frame_);
					
		executor_.start();
		executor_.begin_invoke([this]{read_file();});
		CASPAR_LOG(info) << print() << " Started.";
	}

	~implementation()
	{
		executor_.clear();
		executor_.stop();
		cond_.notify_all();
		CASPAR_LOG(info) << print() << " Stopped.";
	}
			
	void fix_time_base(AVCodecContext* context) // Some files give an invalid numerator, try to fix it.
	{
		if(context && context->time_base.num == 1)
			context->time_base.num = static_cast<int>(std::pow(10.0, static_cast<int>(std::log10(static_cast<float>(context->time_base.den)))-1));
	}

	std::shared_ptr<AVCodecContext> open_stream(int codec_type, int& s_index)
	{		
		AVStream** streams_end = format_context_->streams+format_context_->nb_streams;
		AVStream** stream = std::find_if(format_context_->streams, streams_end, 
			[&](AVStream* stream) { return stream != nullptr && stream->codec->codec_type == codec_type ;});
		
		if(stream == streams_end) 
			return nullptr;
		
		auto codec = avcodec_find_decoder((*stream)->codec->codec_id);			
		if(codec == nullptr)
			return nullptr;
			
		if((-avcodec_open((*stream)->codec, codec)) > 0)		
			return nullptr;
		
		s_index = (*stream)->index;

		return std::shared_ptr<AVCodecContext>((*stream)->codec, avcodec_close);
	}
	
	std::shared_ptr<AVCodecContext>& get_default_context()
	{
		return video_codec_context_ ? video_codec_context_ : audio_codex_context_;
	}

	bool is_eof(int errn)
	{
		if(end_frame_ != -1)
			return get_default_context()->frame_number > eof_count_;		

		return errn == AVERROR_EOF || errn == AVERROR_IO;
	}
		
	void read_file()
	{		
		try
		{
			AVPacket tmp_packet;
			safe_ptr<AVPacket> read_packet(&tmp_packet, av_free_packet);	

			auto read_frame_ret = av_read_frame(format_context_.get(), read_packet.get());
			if(is_eof(read_frame_ret))
			{
				if(loop_)
				{
					seek_frame(start_frame_, AVSEEK_FLAG_BACKWARD);
					// AVCodecContext.frame_number is not reset. Increase the target frame_number.
					eof_count_ += end_frame_ - start_frame_; 
					graph_->add_tag("seek");	
				}	
				else
					stop();
			}
			else if(read_frame_ret < 0)
			{
				BOOST_THROW_EXCEPTION(
					invalid_operation() <<
					msg_info(av_error_str(read_frame_ret)) <<
					source_info(narrow(print())) << 
					boost::errinfo_api_function("av_read_frame") <<
					boost::errinfo_errno(AVUNERROR(read_frame_ret)));
			}
			else
			{
				auto packet = std::make_shared<aligned_buffer>(read_packet->data, read_packet->data + read_packet->size);
				if(read_packet->stream_index == video_s_index_) 		
					video_packet_buffer_.try_push(std::move(packet));	
				else if(read_packet->stream_index == audio_s_index_) 	
					audio_packet_buffer_.try_push(std::move(packet));	
			}
						
			graph_->update_value("input-buffer", static_cast<float>(video_packet_buffer_.size())/static_cast<float>(PACKET_BUFFER_COUNT));		
		}
		catch(...)
		{
			stop();
			CASPAR_LOG_CURRENT_EXCEPTION();
			return;
		}
				
		executor_.begin_invoke([this]{read_file();});		
		boost::unique_lock<boost::mutex> lock(mutex_);
		while(executor_.is_running() && audio_packet_buffer_.size() > PACKET_BUFFER_COUNT && video_packet_buffer_.size() > PACKET_BUFFER_COUNT)
			cond_.wait(lock);		
	}

	void stop()
	{
		executor_.stop();
		CASPAR_LOG(info) << print() << " eof";
	}
		
	std::shared_ptr<aligned_buffer> get_video_packet()
	{
		return get_packet(video_packet_buffer_);
	}

	std::shared_ptr<aligned_buffer> get_audio_packet()
	{
		return get_packet(audio_packet_buffer_);
	}

	bool has_packet() const
	{
		return !video_packet_buffer_.empty() || !audio_packet_buffer_.empty();
	}
	
	void seek_frame(int64_t frame, int flags = 0)
	{  	
		// Convert from frames into seconds.
		auto ts = frame*static_cast<int64_t>((AV_TIME_BASE*get_default_context()->time_base.num) / get_default_context()->time_base.den);

		const int errn = av_seek_frame(format_context_.get(), -1, ts, flags | AVSEEK_FLAG_FRAME);

		if(errn < 0)
			BOOST_THROW_EXCEPTION(
				invalid_operation() << 
				source_info(narrow(print())) << 
				msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("seek_frame") <<
				boost::errinfo_errno(AVUNERROR(errn)));
		
		// Notify decoders to flush buffers.
		video_packet_buffer_.try_push(std::make_shared<aligned_buffer>());	
		audio_packet_buffer_.try_push(std::make_shared<aligned_buffer>());
	}
	
	std::shared_ptr<aligned_buffer> get_packet(tbb::concurrent_bounded_queue<std::shared_ptr<aligned_buffer>>& buffer)
	{
		cond_.notify_all();
		std::shared_ptr<aligned_buffer> packet;
		return buffer.try_pop(packet) ? packet : nullptr;
	}
			
	double fps()
	{
		return static_cast<double>(get_default_context()->time_base.den) / static_cast<double>(get_default_context()->time_base.num);
	}

	std::wstring print() const
	{
		return L"ffmpeg_input[" + filename_ + L"]";
	}
};

input::input(const safe_ptr<diagnostics::graph>& graph, const std::wstring& filename, bool loop, int start_frame, int end_frame) : impl_(new implementation(graph, filename, loop, start_frame, end_frame)){}
const std::shared_ptr<AVCodecContext>& input::get_video_codec_context() const{return impl_->video_codec_context_;}
const std::shared_ptr<AVCodecContext>& input::get_audio_codec_context() const{return impl_->audio_codex_context_;}
bool input::has_packet() const{return impl_->has_packet();}
bool input::is_running() const {return impl_->executor_.is_running();}
std::shared_ptr<aligned_buffer> input::get_video_packet(){return impl_->get_video_packet();}
std::shared_ptr<aligned_buffer> input::get_audio_packet(){return impl_->get_audio_packet();}
double input::fps() const { return impl_->fps(); }
}