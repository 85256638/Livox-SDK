# Livox SDK（钛兴科技定制版）

本分支基于官方 [Livox-SDK](https://github.com/Livox-SDK/Livox-SDK)，为配套 ROS Driver 提供可靠的在线模式切换、断线取消和异步命令生命周期保证。

**配套 Driver**：[livox_ros_driver / `network-relay-added`](https://github.com/85256638/livox_ros_driver/tree/network-relay-added)

## 定制行为

- 发送 PowerSaving / Standby 命令成功后立即开启独立 15 秒过渡宽限，不再等到设备首次报告目标状态后才延长心跳超时。
- 稳态 Normal 仍使用 3 秒心跳超时；PowerSaving / Standby 使用 15 秒。
- heartbeat ACK 必须包含完整 `HeartbeatResponse` 才能刷新连接和上报状态；短载荷不会被解引用。
- 异步 API 返回 `kStatusSuccess` 后，ACK、timeout、发送失败、设备移除和 SDK `Uninit()` 中必有且仅有一次终态 callback。
- 入队但尚未发送的命令也会在断线时以 `kStatusNotConnected` 完成；请求 payload 使用 RAII，不再因队列被丢弃而泄漏。
- ACK 同时校验 sequence、command set 和 command id，避免迟到或错误 ACK 完成另一条命令。
- 修复零长度协议 payload 的空指针 `memcpy` UB，以及新编译器下 `<memory>` 直接依赖缺失。
- LiDAR command-channel 查找与移除已加同步，避免 ROS service/timer 发送和 SDK 断线移除并发访问容器。
- command channel 从 I/O callback 内断线时，会保留到 raw delegate 已从 I/O loop 移除后再析构，避免 heartbeat timeout 路径提前释放当前仍在执行的对象。

## 心跳策略

| 场景 | 心跳断线阈值 | 说明 |
|---|---:|---|
| Normal 稳态 | 3 秒 | 保持官方快速断线检测 |
| 已处于 PowerSaving / Standby | 15 秒 | 容忍低功耗心跳间隔 |
| mode 2/3 命令刚实际发出 | 独立 15 秒 deadline | 覆盖尚未收到目标状态的真实过渡窗口 |

## 异步 API 契约与关闭约束

- API 返回非 `kStatusSuccess`：命令未被 SDK 接受，不再持有 `client_data`，也不会随后 callback。
- API 返回 `kStatusSuccess`：调用方必须保持 `client_data` 有效，直到恰好一次终态 callback。
- 调用全局 `Uninit()` 前必须先停止新的 SDK API 调用。
- 不要从 SDK I/O callback 内直接调用全局 `Uninit()`；应调度到外部线程，避免线程自 `Join()`。

## 获取与编译

不要使用官方默认分支替代本定制分支：

```bash
git clone --branch 'network-relay-added' --single-branch https://github.com/85256638/Livox-SDK.git && cd Livox-SDK && cmake -S . -B build && cmake --build build --parallel "$(nproc)" && sudo cmake --install build
```

较新 CMake 若拒绝旧 policy，可在 configure 命令增加：

```bash
-DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

配套 Driver 的 CMake 会固定此 fork 的精确 commit，不允许静默回退到官方 SDK 或系统中未知来源的同名库。

---

## 以下为官方原始文档

---

[中文版本使用说明](<https://github.com/Livox-SDK/Livox-SDK/blob/master/README_CN.md>)

# 1 Introduction

Livox SDK is the software development kit designed for all Livox products. It is developed based on C/C++ following Livox SDK Communication Protocol, and provides easy-to-use C style API. With Livox SDK, users can quickly connect to Livox products and receive point cloud data. 

Livox SDK consists of Livox SDK communication protocol, Livox SDK core, Livox SDK API, Linux sample, and ROS demo. 

## Prerequisites
* Ubuntu 14.04/Ubuntu 16.04/Ubuntu 18.04, both x86 and ARM (Nvidia TX2)
* Windows 7/10, Visual Studio 2015 Update3/2017/2019
* C++11 compiler

# 2 Livox SDK Communication Protocol

Livox SDK communication protocol opens to all users. It is the communication protocol between user programs and Livox products. The protocol consists of control commands and data format. Please refer to the [Livox SDK Communication Protocol](<https://github.com/Livox-SDK/Livox-SDK/wiki/Livox-SDK-Communication-Protocol>) for detailed information.

# 3 Livox SDK Core

Livox SDK provides the implementation of control commands and point cloud data transmission, as well as the C/C++ API. The basic structure of Livox SDK core is shown as below:

![Livox SDK Architecture](doc/images/sdk.png)

User Datagram Protocol (UDP) is used for communication between Livox SDK and LiDAR sensors. Please refer to the Livox SDK Communication Protocol for further information. Point cloud data handler supports point cloud data transmission, while command handler receives and sends control commands. And the C/C++ API is based on command handler and point cloud data handler.

The Livox LiDAR sensors can be connected to host directly or through the Livox Hub. Livox SDK supports both connection methods. When LiDAR units are connected to host directly, the host will establish communication with each LiDAR unit individually. And if the LiDAR units connect to host through Hub, then the host only communicates with the Livox Hub while the Hub communicates with each LiDAR unit.

# 4 Livox SDK API

Livox SDK API provides a set of C style functions which can be conveniently integrated in C/C++ programs. Please refer to the [Livox SDK API Reference](https://livox-sdk.github.io/Livox-SDK-Doc/) for further information.

## 4.1 Installation
The installation procedures in Ubuntu 18.04/16.04/14.04 LTS and Windows 7/10 are shown here as examples. For Ubuntu 18.04/16.04/14.04 32-bit LTS and Mac, you can get it in [Livox-SDK wiki](https://github.com/Livox-SDK/Livox-SDK/wiki).
### 4.1.1 Ubuntu 18.04/16.04/14.04 LTS
#### Dependencies
Livox SDK requires [CMake 3.0.0+](https://cmake.org/) as dependencies. You can install these packages using apt:
```
sudo apt install cmake
```
#### Compile Livox SDK

In the Livox SDK directory, run the following commands to compile the project:
```
git clone https://github.com/Livox-SDK/Livox-SDK.git
cd Livox-SDK
cd build && cmake ..
make
sudo make install
```

### 4.1.2 Windows 7/10

#### Dependencies
Livox SDK supports Visual Studio 2015 Update3/2017/2019 and requires install [CMake 3.0.0+](https://cmake.org/) as dependencies.  

In the Livox SDK directory, run the following commands to create the Visual Studio solution file. 
Generate the 32-bit project:

```
cd Livox-SDK/build
```
For Viusal Studio 2015 Update3/2017:
```
cmake ..
```
For Viusal Studio 2019:
```
cmake .. -G "Visual Studio 16 2019" -A Win32
```
Generate the 64-bit project:
```
cd Livox-SDK/build 
```
For Viusal Studio 2015 Update3:
```
cmake .. -G "Visual Studio 14 2015 Win64"
```
For Viusal Studio 2017:
```
cmake .. -G "Visual Studio 15 2017 Win64"
```
For Viusal Studio 2019:
```
cmake .. -G "Visual Studio 16 2019" -A x64
```

#### Compile Livox SDK
You can now compile the Livox SDK in Visual Studio.

### 4.1.3 ARM-Linux Cross Compile

The procedure of cross compile Livox-SDK in ARM-Linux are shown below.

#### Dependencies

Host machine requires install cmake. You can install these packages using apt:

```
sudo apt install cmake
```

#### Cross Compile Toolchain

If your ARM board vendor provides a cross compile toolchain, you can skip the following step of installing the toolchain and use the vendor-supplied cross compile toolchain instead.

The following commands will install C and C++ cross compiler toolchains for 32bit and 64bit ARM board. You need to install the correct toolchain for your ARM board. For 64bit SoC ARM board, only install 64bit toolchain, and for 32bit SoC ARM board, only install 32bit toolchain.

Install **ARM 32 bits cross compile toolchain**：

```
 sudo apt-get install gcc-arm-linux-gnueabi g++-arm-linux-gnueabi
```

Install **ARM 64 bits cross compile toolchain**：

```
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

#### Cross Compile Livox-SDK

For  **ARM 32 bits toolchain**，In the Livox SDK directory，run the following commands to cross compile the project:

```
cd Livox-SDK
cd build && \
cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=arm-linux-gnueabi-gcc -DCMAKE_CXX_COMPILER=arm-linux-gnueabi-g++
make
```

For **ARM 64 bits toolchain**，In the Livox SDK directory，run the following commands to cross compile the project:

```
cd Livox-SDK
cd build && \
cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
make
```

**Note:**

- gcc  cross compiler need to support C ++11 standard

## 4.2 Run Livox SDK Sample
Two samples are provided in Sample/Lidar and Sample/Hub, which demonstrate how to configure Livox LiDAR units and receive the point cloud data when directly connecting Livox SDK to LiDAR units or by using a Livox Hub, respectively. The sequence diagram is shown as below: 

![](doc/images/sample.png)

### 4.2.1 Ubuntu 18.04/16.04 /14.04 LTS
For Ubuntun 18.04/16.04/14.04 LTS, run the *lidar_sample* if connect with the LiDAR unit(s):
```
cd sample/lidar && ./lidar_sample
```
or run the *hub_sample* if connect with the hub unit(s):
```
cd sample/hub && ./hub_sample
```
### 4.2.2 Windows 7/10
After compiling the Livox SDK as shown in section 4.1.2, you can find `hub_sample.exe` or `lidar_sample.exe` in the {Livox-SDK}\build\sample\hub\Debug or {Livox-SDK}\build\sample\lidar\Debug folder, respectively, which can be run directly. 

Then you can see the information as below:

![](doc/images/sdk_init.png)

### 4.3 Connect to the specific LiDAR units

Samples we provided will connect all the broadcast device in you LAN in default.There are two ways to connect the specific units: 

* run sample with input options 

* edit the Broadcast Code list in source code

**NOTE:**

Each Livox LiDAR unit owns a unique Broadcast Code . The broadcast code consists of its serial number and an additional number (1,2, or 3). The serial number can be found on the body of the LiDAR unit (below the QR code).The Broadcast Code may be used when you want to connect to the specific LiDAR unit(s).  The detailed format is shown as below:

![Broadcast Code](doc/images/broadcast_code.png)

#### 4.3.1 Program Options

We provide the following program options for connecting the specific units and saving log file:
```
[-c]:Register LiDAR units by Broadcast Code. Connect the registered units ONLY. 
[-l]:Save the log file(In the executable file's directory).
[-h]:Show help.
```
Here is the example:
```
./lidar_sample_cc -c "00000000000002&00000000000003&00000000000004" -l
./hub_sample_cc -c "00000000000001" -l
```

#### 4.3.2 Edit Broadcast Code List

 Comment the following code section:

```
/** Connect all the broadcast device. */
int lidar_count = 0;
char broadcast_code_list[kMaxLidarCount][kBroadcastCodeSize];
```

Remove the comment of the following code section, set the BROADCAST_CODE_LIST_SIZE and replace the broadcast code lists in the `main.c` for both LiDAR sample ({Livox-SDK}/sample/lidar/main.c) and Hub sample ({Livox-SDK}/sample/hub/main.c) with the broadcast code of your devices before building.

```
/** Connect the broadcast device in list, please input the broadcast code and modify the BROADCAST_CODE_LIST_SIZE. */
/*#define BROADCAST_CODE_LIST_SIZE  3
int lidar_count = BROADCAST_CODE_LIST_SIZE;
char broadcast_code_list[kMaxLidarCount][kBroadcastCodeSize] = {
  "000000000000002",
  "000000000000003",
  "000000000000004"
};*/
```

### 4.4 Generate the lvx file

We provide the C++ sample to generate the lvx file for hub and LiDAR unit(s). You can use the same way in `4.2.1` and `4.2.2` to run them.

#### 4.4.1 Program Options

You can alse use the program options in `4.3.1` to connect specific device and generate the log file, and we provide two new options for lvx file:
```
[-t] Time to save point cloud to the lvx file.(unit: s)
[-p] Get the extrinsic parameter from standard extrinsic.xml file(The same as viewer) in the executable file's directory.(Especially for LiDAR unit(s) as the hub will calculate the extrinsic parameter by itself)
```
Here is the example:
```
./lidar_lvx_sample -c "00000000000002&00000000000003&00000000000004" -l -t 10 -p
./hub_lvx_sample -c "00000000000001" -l -t 10
```

# 5 Support

You can get support from Livox with the following methods:
* Send email to cs@livoxtech.com with a clear description of your problem and your setup
* Github Issues
