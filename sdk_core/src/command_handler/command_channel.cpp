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

#include "command_channel.h"
#include <cstring>
#include <functional>
#include <atomic>
#include "base/logging.h"
#include "base/network/network_util.h"
#include "command_impl.h"
#include "device_manager.h"
#include "livox_def.h"
#include <stdio.h>

using std::bind;
using std::list;
using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::chrono::steady_clock;

namespace livox {

CommandChannel::CommandChannel(uint16_t port,
                               uint8_t handle,
                               const string &remote_ip,
                               CommandChannelDelegate *cb)
    : handle_(handle),
      port_(port),
      loop_(),
      callback_(cb),
      comm_port_(new CommPort),
      remote_ip_(remote_ip),
      heartbeat_time_(),
      last_heartbeat_() {}


bool CommandChannel::Bind(std::weak_ptr<IOLoop> loop) {
  if (loop.expired()) {
    return false;
  }
  loop_ = loop;
  sock_ = util::CreateSocket(port_);
  if (sock_ == -1) {
    return false;
  }

  loop_.lock()->AddDelegate(sock_, this);
  last_heartbeat_ = steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(accepted_commands_mutex_);
    accepting_commands_ = true;
  }
  return true;
}

void CommandChannel::OnData(socket_t , void *) {
  struct sockaddr addr;
  int addrlen = sizeof(addr);
  uint32_t buf_size = 0;
  uint8_t *cache_buf = comm_port_->FetchCacheFreeSpace(&buf_size);
  int size = buf_size;
  size = util::RecvFrom(sock_, reinterpret_cast<char *>(cache_buf), buf_size, 0, &addr, &addrlen);
  if (size <= 0) {
    return;
  }
  comm_port_->UpdateCacheWrIdx(size);
  CommPacket packet;
  memset(&packet, 0, sizeof(packet));

  while ((kParseSuccess == comm_port_->ParseCommStream(&packet))) {
    if (packet.packet_type == kCommandTypeAck) {
      uint16_t seq = packet.seq_num;
      map<uint16_t, pair<Command, TimePoint> >::iterator command_it =
          commands_.find(seq);
      const bool command_identity_matches =
          command_it != commands_.end() &&
          command_it->second.first.packet.cmd_set == packet.cmd_set &&
          command_it->second.first.packet.cmd_code == packet.cmd_code;
      if (command_identity_matches) {
        Command command = command_it->second.first;
        command.packet = packet;
        commands_.erase(command_it);
        ForgetAcceptedCommand(command);
        if (ClaimCommandCompletion(command)) {
          if (callback_) {
            callback_->OnCommand(handle_, command);
          } else if (command.cb) {
            (*command.cb)(kStatusSuccess, handle_, command.packet.data);
          }
        }
      } else if (packet.cmd_set == kCommandSetGeneral && packet.cmd_code == kCommandIDGeneralHeartbeat) {
        if (packet.data == NULL ||
            packet.data_len < sizeof(HeartbeatResponse)) {
          LOG_WARN("Ignore malformed heartbeat ACK: payload length {}",
                   packet.data_len);
          continue;
        }
        HeartbeatResponse heartbeat;
        std::memcpy(&heartbeat, packet.data, sizeof(heartbeat));
        OnHeartbeatAck(heartbeat);
        if (callback_) {
          callback_->OnHeartbeatStateUpdate(handle_, heartbeat);
        }
      } else if (command_it != commands_.end()) {
        LOG_WARN("Ignore mismatched ACK: seq {}, expected set/id {}/{}, got "
                 "{}/{}",
                 seq, command_it->second.first.packet.cmd_set,
                 command_it->second.first.packet.cmd_code, packet.cmd_set,
                 packet.cmd_code);
      }
    } else if (packet.packet_type == kCommandTypeMsg) {
      if (callback_) {
        Command command;
        command.packet = packet;
        callback_->OnCommand(handle_, command);
      }
    }
  }
}

bool CommandChannel::SendAsync(const Command &command) {
  std::shared_ptr<IOLoop> loop = loop_.lock();
  if (!loop) {
    return false;
  }

  Command cmd = DeepCopy(command);
  WeakProtector w_ptr;
  {
    std::lock_guard<std::mutex> lock(accepted_commands_mutex_);
    if (!accepting_commands_ ||
        accepted_commands_.find(cmd.packet.seq_num) !=
            accepted_commands_.end()) {
      return false;
    }
    accepted_commands_[cmd.packet.seq_num] = cmd;
    w_ptr = WeakProtector(protector_);
  }

  loop->PostTask([this, w_ptr, cmd]() {
    if (w_ptr.expired()) {
      CommandChannel::CompleteCommandOnce(cmd, kStatusNotConnected);
      return;
    }
    Send(cmd);
  });
  return true;
}

void CommandChannel::OnTimer(TimePoint now) {
  list<Command> timeout_commands;
  map<uint16_t, pair<Command, TimePoint> >::iterator ite = commands_.begin();
  while (ite != commands_.end()) {
    pair<Command, TimePoint> &command_pair = ite->second;
    if (now > command_pair.second) {
      timeout_commands.push_back(command_pair.first);
      commands_.erase(ite++);
    } else {
      ++ite;
    }
  }

  for (list<Command>::iterator ite = timeout_commands.begin(); ite != timeout_commands.end(); ++ite) {
    LOG_WARN("Command Timeout: Set {}, Id {}, Seq {}", 
        (uint16_t)ite->packet.cmd_set, ite->packet.cmd_code, ite->packet.seq_num);
    ForgetAcceptedCommand(*ite);
    if (ClaimCommandCompletion(*ite)) {
      ite->packet.packet_type = kCommandTypeAck;
      if (callback_) {
        callback_->OnCommand(handle_, *ite);
      } else if (ite->cb) {
        (*ite->cb)(kStatusTimeout, handle_, NULL);
      }
    }
  }

  auto heartbeat_timeout = std::chrono::seconds(3);
  if (last_work_state_ == kLidarStatePowerSaving ||
      last_work_state_ == kLidarStateStandBy) {
    /** Power-saving(2) or Standby(3): use longer timeout to keep session alive */
    heartbeat_timeout = std::chrono::seconds(15);
  }
  const bool mode_transition_active =
      mode_transition_deadline_ != TimePoint() &&
      now < mode_transition_deadline_;
  if (!mode_transition_active && now - last_heartbeat_ > heartbeat_timeout) {
    DeviceDisconnect(handle_);
  } else {
    HeartBeat(now);
  }
}

void CommandChannel::Uninit() {
  list<Command> canceled_commands;
  {
    std::lock_guard<std::mutex> lock(accepted_commands_mutex_);
    accepting_commands_ = false;
    protector_.reset();
    for (map<uint16_t, Command>::const_iterator ite =
             accepted_commands_.begin();
         ite != accepted_commands_.end(); ++ite) {
      canceled_commands.push_back(ite->second);
    }
    accepted_commands_.clear();
  }
  commands_.clear();

  if (sock_ != -1) {
    if (!loop_.expired()) {
      loop_.lock()->RemoveDelegate(sock_, this);
    }
    util::CloseSock(sock_);
    sock_ = -1;
  }
  callback_ = NULL;
  if (comm_port_) {
    comm_port_.reset(NULL);
  }

  last_heartbeat_ = {};
  heartbeat_time_ = {};
  mode_transition_deadline_ = {};
  mode_transition_target_ = kLidarStateUnknown;
  last_work_state_ = kLidarStateUnknown;
  remote_ip_ = "";

  /** Invoke user code only after all channel containers are detached. */
  for (list<Command>::const_iterator ite = canceled_commands.begin();
       ite != canceled_commands.end(); ++ite) {
    CompleteCommandOnce(*ite, kStatusNotConnected);
  }
}

void CommandChannel::HeartBeat(TimePoint t) {
  if (heartbeat_time_ == TimePoint() || (t - heartbeat_time_) > kHeartbeatTimer) {
    heartbeat_time_ = t;

    Command command(handle_,
                    kCommandTypeCmd,
                    kCommandSetGeneral,
                    kCommandIDGeneralHeartbeat,
                    GenerateSeq(),
                    NULL,
                    0,
                    0,
                    std::shared_ptr<CommandCallback>());
    SendInternal(command);
  }
}

bool CommandChannel::SendInternal(const Command &command) {
  std::vector<uint8_t> buf(kMaxCommandBufferSize + 1);
  uint32_t size = 0;
  if (comm_port_->Pack(buf.data(), kMaxCommandBufferSize, &size,
                       command.packet) != 0 ||
      size == 0) {
    return false;
  }

  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(65000);
  servaddr.sin_addr.s_addr = inet_addr(remote_ip_.c_str());

  int byte_send = sendto(sock_, (const char*)buf.data(), size, 0, (const struct sockaddr *) &servaddr,
            sizeof(servaddr));
  if (byte_send != static_cast<int>(size)) {
    return false;
  }
  return true;
}

uint16_t CommandChannel::GenerateSeq() {
  static std::atomic<std::uint16_t> seq(1);
  uint16_t value = seq.load();
  uint16_t desired = 0;
  do {
    if (value == UINT16_MAX) {
      desired = 1;
    } else {
      desired = value + 1;
    }
  } while (!seq.compare_exchange_weak(value, desired));
  return desired;
}

Command CommandChannel::DeepCopy(const Command &cmd) {
  Command result_cmd(cmd);
  if (cmd.packet.data != NULL && cmd.packet.data_len != 0) {
    result_cmd.owned_data = std::make_shared<std::vector<uint8_t> >(
        cmd.packet.data, cmd.packet.data + cmd.packet.data_len);
    result_cmd.packet.data = result_cmd.owned_data->data();
  } else {
    result_cmd.packet.data = NULL;
  }
  return result_cmd;
}

void CommandChannel::OnHeartbeatAck(const HeartbeatResponse &response) {
  last_heartbeat_ = steady_clock::now();
  last_work_state_ = response.state;
  if (mode_transition_target_ != kLidarStateUnknown &&
      response.state == mode_transition_target_) {
    mode_transition_deadline_ = TimePoint();
    mode_transition_target_ = kLidarStateUnknown;
  }
}

void CommandChannel::UpdateModeTransition(const Command &command,
                                          TimePoint now) {
  if (command.packet.cmd_set != kCommandSetLidar ||
      command.packet.cmd_code != kCommandIDLidarSetMode ||
      command.packet.data == NULL || command.packet.data_len < 1) {
    return;
  }

  const uint8_t mode = command.packet.data[0];
  if (mode == kLidarModePowerSaving || mode == kLidarModeStandby) {
    mode_transition_deadline_ = now + std::chrono::seconds(15);
    mode_transition_target_ =
        (mode == kLidarModePowerSaving) ? kLidarStatePowerSaving
                                       : kLidarStateStandBy;
  } else if (mode == kLidarModeNormal) {
    mode_transition_deadline_ = TimePoint();
    mode_transition_target_ = kLidarStateUnknown;
  }
}

bool CommandChannel::ClaimCommandCompletion(const Command &command) {
  if (!command.completion) {
    return false;
  }
  bool expected = false;
  return command.completion->compare_exchange_strong(expected, true);
}

void CommandChannel::CompleteCommandOnce(const Command &command,
                                         livox_status status, void *data) {
  if (ClaimCommandCompletion(command) && command.cb) {
    (*command.cb)(status, command.handle, data);
  }
}

void CommandChannel::ForgetAcceptedCommand(const Command &command) {
  std::lock_guard<std::mutex> lock(accepted_commands_mutex_);
  map<uint16_t, Command>::iterator ite =
      accepted_commands_.find(command.packet.seq_num);
  if (ite != accepted_commands_.end() &&
      ite->second.completion == command.completion) {
    accepted_commands_.erase(ite);
  }
}

bool CommandChannel::IsAcceptedCommand(const Command &command) {
  std::lock_guard<std::mutex> lock(accepted_commands_mutex_);
  map<uint16_t, Command>::const_iterator ite =
      accepted_commands_.find(command.packet.seq_num);
  return ite != accepted_commands_.end() &&
         ite->second.completion == command.completion;
}

void CommandChannel::CompleteCommand(const Command &command,
                                     livox_status status, void *data) {
  ForgetAcceptedCommand(command);
  CompleteCommandOnce(command, status, data);
}

void CommandChannel::DeviceDisconnect(uint8_t handle) {
  DeviceRemove(handle,kEventDisconnect);
}

void CommandChannel::Send(const Command &command) {
  if (!IsAcceptedCommand(command)) {
    return;
  }
  LOG_INFO(" Send Command: Set {} Id {} Seq {}", (uint16_t)command.packet.cmd_set, command.packet.cmd_code, command.packet.seq_num);
  if (!SendInternal(command)) {
    CompleteCommand(command, kStatusSendFailed);
    return;
  }

  const TimePoint now = steady_clock::now();
  UpdateModeTransition(command, now);
  if (!IsAcceptedCommand(command)) {
    return;
  }
  commands_[command.packet.seq_num] = make_pair(
      command, now + std::chrono::milliseconds(command.time_out));
  Command &cmd = commands_[command.packet.seq_num].first;
  cmd.owned_data.reset();
  cmd.packet.data = NULL;
  cmd.packet.data_len = 0;
}
}  // namespace livox
