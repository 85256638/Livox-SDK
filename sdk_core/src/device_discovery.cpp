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

#include "device_discovery.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <mutex>
#include <iostream>
#include <vector>
#include "base/logging.h"
#include "base/network/network_util.h"
#include "command_handler/command_impl.h"
#include "device_manager.h"
#include "livox_def.h"

using std::tuple;
using std::string;
using std::vector;
using std::chrono::steady_clock;


namespace livox {

namespace {

int32_t LastSocketError() {
#ifdef WIN32
  return static_cast<int32_t>(WSAGetLastError());
#else
  return static_cast<int32_t>(errno);
#endif
}

}  // namespace

bool DeviceDiscovery::Init() {
  if (comm_port_ == NULL) {
    comm_port_.reset(new CommPort());
  }
  return true;
}

bool DeviceDiscovery::Start(std::weak_ptr<IOLoop> loop) {
  if (loop.expired()) {
    return false;
  }
  loop_ = loop;
  sock_ = util::CreateSocket(kListenPort);
  if (sock_ < 0) {
    return false;
  }
  loop_.lock()->AddDelegate(sock_, this);
  return true;
}

void DeviceDiscovery::SetHandshakeCallback(
    const std::function<void(const DeviceHandshakeStatus *)> &cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  handshake_cb_ = cb;
}

livox_status DeviceDiscovery::ResetHandshakeSession(
    const std::string &broadcast_code) {
  if (broadcast_code.empty()) {
    return kStatusFailure;
  }

  DeviceInfo info;
  if (!device_manager().FindDevice(broadcast_code, info)) {
    return kStatusInvalidHandle;
  }
  if (device_manager().IsDeviceReady(info.handle)) {
    return kStatusNotSupported;
  }

  std::shared_ptr<IOLoop> loop = loop_.lock();
  if (!loop) {
    return kStatusNotConnected;
  }

  const TimePoint requested_at = steady_clock::now();
  loop->PostTask([this, broadcast_code, requested_at]() {
    DeviceInfo current;
    if (!device_manager().FindDevice(broadcast_code, current) ||
        device_manager().IsDeviceReady(current.handle)) {
      return;
    }
    const bool provisional =
        device_manager().IsDeviceConnected(current.handle);
    if (provisional) {
      /** Handshake succeeded but DeviceInfo/public connect did not. */
      DeviceReset(current.handle);
      LOG_WARN("Cleared provisional command/data session: code {}, handle {}, ip {}",
               broadcast_code, static_cast<uint16_t>(current.handle),
               current.ip);
    }
    /** Do not cancel a fresh attempt created after this reset was requested. */
    const uint32_t cleared =
        ClearPendingHandshakes(current.handle, requested_at);
    LOG_WARN("Reset local handshake session: code {}, handle {}, provisional {}, pending {}",
             broadcast_code, static_cast<uint16_t>(current.handle),
             provisional, cleared);
    NotifyHandshake(current, kDeviceHandshakeReset,
                    static_cast<int32_t>(cleared));
  });
  return kStatusSuccess;
}

bool DeviceDiscovery::HasPendingHandshake(uint8_t handle) const {
  for (ConnectingDeviceMap::const_iterator ite = connecting_devices_.begin();
       ite != connecting_devices_.end(); ++ite) {
    if (std::get<1>(ite->second).handle == handle) {
      return true;
    }
  }
  return false;
}

uint32_t DeviceDiscovery::ClearPendingHandshakes(uint8_t handle,
                                                 TimePoint not_after) {
  uint32_t cleared = 0;
  ConnectingDeviceMap::iterator ite = connecting_devices_.begin();
  while (ite != connecting_devices_.end()) {
    if (std::get<1>(ite->second).handle != handle ||
        std::get<0>(ite->second) > not_after) {
      ++ite;
      continue;
    }
    const socket_t pending_sock = ite->first;
    if (!loop_.expired()) {
      loop_.lock()->RemoveDelegate(pending_sock, this);
    }
    util::CloseSock(pending_sock);
    connecting_devices_.erase(ite++);
    ++cleared;
  }
  return cleared;
}

void DeviceDiscovery::NotifyHandshake(const DeviceInfo &info,
                                      DeviceHandshakeEvent event,
                                      int32_t detail) {
  std::function<void(const DeviceHandshakeStatus *)> cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = handshake_cb_;
  }
  if (!cb) {
    return;
  }

  DeviceHandshakeStatus status;
  memset(&status, 0, sizeof(status));
  strncpy(status.broadcast_code, info.broadcast_code,
          sizeof(status.broadcast_code) - 1);
  strncpy(status.ip, info.ip, sizeof(status.ip) - 1);
  status.handle = info.handle;
  status.event = event;
  status.detail = detail;
  cb(&status);
}

void DeviceDiscovery::OnData(socket_t sock, void *) {
  struct sockaddr addr;
  int addrlen = sizeof(addr);
  uint32_t buf_size = 0;
  uint8_t *cache_buf = comm_port_->FetchCacheFreeSpace(&buf_size);
  int size = buf_size;
  size = util::RecvFrom(sock, reinterpret_cast<char *>(cache_buf), buf_size, 0, &addr, &addrlen);
  if (size < 0) {
    ConnectingDeviceMap::iterator pending = connecting_devices_.find(sock);
    if (pending != connecting_devices_.end()) {
      const int32_t socket_error = LastSocketError();
      DeviceInfo info = std::get<1>(pending->second);
      if (!loop_.expired()) {
        loop_.lock()->RemoveDelegate(sock, this);
      }
      util::CloseSock(sock);
      connecting_devices_.erase(pending);
      LOG_WARN("Handshake receive failed: code {}, errno {}",
               info.broadcast_code, socket_error);
      NotifyHandshake(info, kDeviceHandshakeNetworkError, socket_error);
    }
    return;
  }

  comm_port_->UpdateCacheWrIdx(size);
  CommPacket packet;
  memset(&packet, 0, sizeof(packet));

  while ((kParseSuccess == comm_port_->ParseCommStream(&packet))) {
    if (packet.cmd_set == kCommandSetGeneral && packet.cmd_code == kCommandIDGeneralBroadcast) {
      OnBroadcast(packet, &addr);
    } else if (packet.cmd_set == kCommandSetGeneral && packet.cmd_code == kCommandIDGeneralHandshake) {
      ConnectingDeviceMap::iterator pending = connecting_devices_.find(sock);
      if (pending == connecting_devices_.end()) {
        continue;
      }
      DeviceInfo info = std::get<1>(pending->second);
      if (!loop_.expired()) {
        loop_.lock()->RemoveDelegate(sock, this);
      }
      util::CloseSock(sock);
      connecting_devices_.erase(pending);

      if (packet.packet_type != kCommandTypeAck || packet.data == NULL ||
          packet.data_len < sizeof(uint8_t)) {
        LOG_WARN("Malformed handshake ACK: code {}, packet type {}, payload {}",
                 info.broadcast_code, static_cast<uint16_t>(packet.packet_type),
                 packet.data_len);
        NotifyHandshake(info, kDeviceHandshakeProtocolError, kStatusFailure);
        continue;
      }
      const uint8_t ret_code = *(uint8_t *)packet.data;
      if (ret_code == 0) {
        LOG_INFO("New Device");
        LOG_INFO("Handle: {}", static_cast<uint16_t>(info.handle));
        LOG_INFO("Broadcast Code: {}", info.broadcast_code);
        LOG_INFO("Type: {}", info.type);
        LOG_INFO("IP: {}", info.ip);
        LOG_INFO("Command Port: {}", info.cmd_port);
        LOG_INFO("Data Port: {}", info.data_port);
        DeviceFound(info);
        NotifyHandshake(info, kDeviceHandshakeSuccess, 0);
      } else {
        LOG_WARN("Handshake rejected: code {}, ret_code {}",
                 info.broadcast_code, static_cast<uint16_t>(ret_code));
        NotifyHandshake(info, kDeviceHandshakeRejected,
                        static_cast<int32_t>(ret_code));
      }
    }
  }
}

void DeviceDiscovery::OnTimer(TimePoint now) {
  ConnectingDeviceMap::iterator ite = connecting_devices_.begin();
  while (ite != connecting_devices_.end()) {
    tuple<TimePoint, DeviceInfo> &device_tuple = ite->second;
    if (now - std::get<0>(device_tuple) > std::chrono::milliseconds(500)) {
      DeviceInfo info = std::get<1>(device_tuple);
      const socket_t pending_sock = ite->first;
      if (!loop_.expired()) {
        loop_.lock()->RemoveDelegate(pending_sock, this);
      }
      util::CloseSock(pending_sock);
      connecting_devices_.erase(ite++);
      /** Driver-side diagnostics edge/periodically throttle repeated timeouts. */
      LOG_DEBUG("Handshake timeout: code {}, handle {}", info.broadcast_code,
                static_cast<uint16_t>(info.handle));
      NotifyHandshake(info, kDeviceHandshakeTimeout, kStatusTimeout);
    } else {
      ++ite;
    }
  }
}

void DeviceDiscovery::Uninit() {
  if (sock_ > 0) {
    if (!loop_.expired()) {
      loop_.lock()->RemoveDelegate(sock_, this);
    }
    util::CloseSock(sock_);
    sock_ = -1;
  }

  for (ConnectingDeviceMap::iterator ite = connecting_devices_.begin();
       ite != connecting_devices_.end(); ++ite) {
    util::CloseSock(ite->first);
  }
  connecting_devices_.clear();

  if (comm_port_) {
    comm_port_.reset(NULL);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handshake_cb_ = NULL;
  }
  loop_.reset();
}

void DeviceDiscovery::OnBroadcast(const CommPacket &packet,  struct sockaddr *addr) {
  const uint32_t broadcast_payload_size =
      static_cast<uint32_t>(offsetof(BroadcastDeviceInfo, ip));
  if (packet.data == NULL || packet.data_len < broadcast_payload_size) {
    return;
  }

  BroadcastDeviceInfo device_info;
  memset(&device_info, 0, sizeof(device_info));
  memcpy(&device_info, packet.data, broadcast_payload_size);
  device_info.broadcast_code[sizeof(device_info.broadcast_code) - 1] = '\0';
  string broadcast_code = device_info.broadcast_code;
  LOG_INFO(" Broadcast broadcast code: {}", broadcast_code);

  char ip[16];
  memset(&ip, 0, sizeof(ip));
  inet_ntop(AF_INET, &((struct sockaddr_in*)addr)->sin_addr, ip, INET_ADDRSTRLEN);
  strncpy(device_info.ip, ip, sizeof(device_info.ip));
  
  device_manager().BroadcastDevices(&device_info);
 
  DeviceInfo lidar_info;
  bool found = device_manager().FindDevice(broadcast_code, lidar_info);

  if (!found) {
    LOG_INFO("Broadcast code : {} not add to connect", broadcast_code);
  }

  if (!found || device_manager().IsDeviceConnected(lidar_info.handle)) {
    return;
  }

  OnTimer(steady_clock::now());
  if (HasPendingHandshake(lidar_info.handle)) {
    return;
  }

  /** Stable, per-handle ports keep retries bounded and cannot wrap after a
   *  long broadcast-only outage. Handles are allocated in [0, 31]. */
  const uint16_t port_slot = static_cast<uint16_t>(lidar_info.handle) + 1;
  strncpy(lidar_info.broadcast_code, broadcast_code.c_str(), sizeof(lidar_info.broadcast_code)-1);
  lidar_info.broadcast_code[sizeof(lidar_info.broadcast_code) - 1] = '\0';
  lidar_info.cmd_port = kListenPort + kCmdPortOffset + port_slot;
  lidar_info.data_port = kListenPort + kDataPortOffset + port_slot;
  lidar_info.sensor_port = kListenPort + kSensorPortOffset + port_slot;
  lidar_info.type = device_info.dev_type;
  lidar_info.state = kLidarStateUnknown;
  lidar_info.feature = kLidarFeatureNone;
  memset(&lidar_info.status, 0, sizeof(lidar_info.status));

  strncpy(lidar_info.ip, ip, sizeof(lidar_info.ip));
  lidar_info.ip[sizeof(lidar_info.ip) - 1] = '\0';

  socket_t cmd_sock = util::CreateSocket(lidar_info.cmd_port);
  if (cmd_sock < 0) {
    const int32_t socket_error = LastSocketError();
    LOG_WARN("Create handshake socket failed: code {}, port {}, errno {}",
             lidar_info.broadcast_code, lidar_info.cmd_port, socket_error);
    NotifyHandshake(lidar_info, kDeviceHandshakeNetworkError, socket_error);
    return;
  }
  std::shared_ptr<IOLoop> loop = loop_.lock();
  if (!loop) {
    util::CloseSock(cmd_sock);
    NotifyHandshake(lidar_info, kDeviceHandshakeNetworkError,
                    kStatusNotConnected);
    return;
  }

  connecting_devices_[cmd_sock] =
      std::make_tuple(steady_clock::now(), lidar_info);
  loop->AddDelegate(cmd_sock, this);

  bool result = false;
  DeviceHandshakeEvent failure_event = kDeviceHandshakeNetworkError;
  int32_t failure_detail = kStatusFailure;
  do {
    HandshakeRequest handshake_req;
    uint32_t local_ip = 0;
    if (util::FindLocalIp(*(struct sockaddr_in*)addr, local_ip) == false) {
      LOG_WARN("LocalIp and DeviceIp are not in same subnet: code {}, ip {}",
               lidar_info.broadcast_code, lidar_info.ip);
      break;
    }
    LOG_INFO("LocalIP: {}", inet_ntoa(*(struct in_addr *)&local_ip));
    LOG_INFO("DeviceIP: {}", inet_ntoa(((struct sockaddr_in *)addr)->sin_addr));
    LOG_INFO("Command Port: {}", lidar_info.cmd_port);
    LOG_INFO("Data Port: {}", lidar_info.data_port);
    CommPacket packet;
    memset(&packet, 0, sizeof(packet));
    handshake_req.ip_addr = local_ip;
    handshake_req.cmd_port = lidar_info.cmd_port;
    handshake_req.data_port = lidar_info.data_port;
    handshake_req.sensor_port = lidar_info.sensor_port;
    packet.packet_type = kCommandTypeAck;
    packet.seq_num = CommandChannel::GenerateSeq();
    packet.cmd_set = kCommandSetGeneral;
    packet.cmd_code = kCommandIDGeneralHandshake;
    packet.data_len = sizeof(handshake_req);
    packet.data = (uint8_t *)&handshake_req;

    vector<uint8_t> buf(kMaxCommandBufferSize + 1);
    uint32_t o_len = 0;
    if (comm_port_->Pack(buf.data(), kMaxCommandBufferSize, &o_len, packet) != 0 ||
        o_len == 0) {
      failure_event = kDeviceHandshakeProtocolError;
      LOG_WARN("Pack handshake failed: code {}", lidar_info.broadcast_code);
      break;
    }
    const int expected_size = static_cast<int>(o_len);
    int byte_send = sendto(cmd_sock, reinterpret_cast<const char *>(buf.data()),
                           expected_size, 0, addr, sizeof(*addr));
    if (byte_send != expected_size) {
      failure_detail = byte_send < 0 ? LastSocketError() : kStatusSendFailed;
      LOG_WARN("Send handshake failed: code {}, sent {}, expected {}, errno/status {}",
               lidar_info.broadcast_code, byte_send, expected_size,
               failure_detail);
      break;
    }
    std::get<0>(connecting_devices_[cmd_sock]) = steady_clock::now();
    result = true;
  } while (0);

  if (result == false) {
    ClearPendingHandshakes(lidar_info.handle, (TimePoint::max)());
    NotifyHandshake(lidar_info, failure_event, failure_detail);
  }
}


DeviceDiscovery &device_discovery() {
  static DeviceDiscovery discovery;
  return discovery;
}

}  // namespace livox
