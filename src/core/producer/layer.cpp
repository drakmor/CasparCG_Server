/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "../StdAfx.h"

#include "layer.h"

#include "frame_producer.h"

#include "../frame/draw_frame.h"
#include "../frame/frame_transform.h"
#include "../video_format.h"

#include <common/timer.h>

#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>

namespace caspar { namespace core {

struct layer::impl
{
    monitor::state state_;

    spl::shared_ptr<frame_producer> foreground_ = frame_producer::empty();
    spl::shared_ptr<frame_producer> background_ = frame_producer::empty();

    boost::optional<int32_t> auto_play_delta_;
    bool                     paused_ = false;

  public:
    void pause() { paused_ = true; }

    void resume() { paused_ = false; }

    void load(spl::shared_ptr<frame_producer> producer, bool preview, const boost::optional<int32_t>& auto_play_delta)
    {
        background_      = std::move(producer);
        auto_play_delta_ = auto_play_delta;
        paused_          = true;

        if (auto_play_delta_ && foreground_ == frame_producer::empty()) {
            play();
        } else if (preview) {
            foreground_ = std::move(background_);
            background_ = frame_producer::empty();
        }
    }

    void play()
    {
        if (background_ != frame_producer::empty()) {
            background_->leading_producer(foreground_);

            foreground_ = std::move(background_);
            background_ = frame_producer::empty();

            auto_play_delta_.reset();
        }

        paused_ = false;
    }

    void stop()
    {
        foreground_ = frame_producer::empty();
        auto_play_delta_.reset();
    }

    draw_frame receive(const video_format_desc& format_desc, int nb_samples)
    {
        try {
            if (foreground_->following_producer() != core::frame_producer::empty()) {
                foreground_ = foreground_->following_producer();
            }

            if (auto_play_delta_) {
                auto time        = static_cast<std::int64_t>(foreground_->frame_number());
                auto duration    = static_cast<std::int64_t>(foreground_->nb_frames());
                auto frames_left = duration - time - static_cast<std::int64_t>(*auto_play_delta_);
                if (frames_left < 1) {
                    play();
                }
            }

            auto frame = paused_ ? core::draw_frame{} : foreground_->receive(nb_samples);
            if (!frame) {
                frame = foreground_->last_frame();
            }

            state_.clear();
            state_.insert_or_assign(foreground_->state());

            return frame;
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
            stop();
            return draw_frame{};
        }
    }
};

layer::layer()
    : impl_(new impl())
{
}
layer::layer(layer&& other)
    : impl_(std::move(other.impl_))
{
}
layer& layer::operator=(layer&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}
void layer::swap(layer& other) { impl_.swap(other.impl_); }
void layer::load(spl::shared_ptr<frame_producer> frame_producer,
                 bool                            preview,
                 const boost::optional<int32_t>& auto_play_delta)
{
    return impl_->load(std::move(frame_producer), preview, auto_play_delta);
}
void       layer::play() { impl_->play(); }
void       layer::pause() { impl_->pause(); }
void       layer::resume() { impl_->resume(); }
void       layer::stop() { impl_->stop(); }
draw_frame layer::receive(const video_format_desc& format_desc, int nb_samples)
{
    return impl_->receive(format_desc, nb_samples);
}
spl::shared_ptr<frame_producer> layer::foreground() const { return impl_->foreground_; }
spl::shared_ptr<frame_producer> layer::background() const { return impl_->background_; }
const monitor::state&           layer::state() const { return impl_->state_; }
}} // namespace caspar::core
