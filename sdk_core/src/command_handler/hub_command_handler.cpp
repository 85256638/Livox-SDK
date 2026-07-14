//
// The MIT License (MIT)
//
// Copyright (c) 2019 Livox. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "hub_command_handler.h"
#include "base/network/network_util.h"

namespace livox {

void HubCommandHandlerImpl::Uninit() {
  std::unique_ptr<CommandChannel> channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    channel = std::move(channel_);
    is_valid_ = false;
  }
  if (channel) {
    channel->Uninit();
  }
}

bool HubCommandHandlerImpl::AddDevice(const DeviceInfo &info) {
  std::unique_ptr<CommandChannel> channel(
      new CommandChannel(info.cmd_port, info.handle, info.ip, this));
  if (!channel->Bind(loop_)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (is_valid_) {
    return false;
  }
  is_valid_ = true;
  hub_info_ = info;
  channel_ = std::move(channel);
  return true;
}

livox_status HubCommandHandlerImpl::SendCommand(uint8_t, const Command &command) {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (channel_ == NULL) {
    return kStatusChannelNotExist;
  }
  return channel_->SendAsync(command) ? kStatusSuccess
                                      : kStatusChannelNotExist;
}

bool HubCommandHandlerImpl::RemoveDevice(uint8_t) {
  std::unique_ptr<CommandChannel> channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    is_valid_ = false;
    channel = std::move(channel_);
  }
  if (channel) {
    channel->Uninit();
  }
  return false;
}

}  // namespace livox
