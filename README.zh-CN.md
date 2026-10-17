![OpenIPC logo][logo]

# OpenIPC Cerberus

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![CI](https://github.com/OpenIPC/cerberus/actions/workflows/ci.yml/badge.svg)](https://github.com/OpenIPC/cerberus/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/OpenIPC/cerberus)](https://github.com/OpenIPC/cerberus/releases/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-OpenIPC-brightgreen)](https://openipc.org)

适用于运行 [OpenIPC](https://openipc.org) 固件摄像机的云代理：一个用 C 编写的小型守护进程，**运行在摄像机内部**，
将摄像机接入私有 WireGuard 网络。这样无论身在何处，都能访问摄像机的视频、ONVIF、设置和 SSH，
摄像机与云之间除了隧道别无他物。

它只做五件事：

1. 首次启动时生成设备的 X25519 密钥对和认领码（claim code），仅此一次；
2. 通过 TLS 向协调器注册（`POST /enroll`），获取入网材料（join material）；
3. 通过 `fw_setenv` 将密钥、认领码和入网材料保存在 **U-Boot 环境变量**中——这是摄像机上唯一能在刷写固件后保留下来的地方；
4. 启动 `wg0` 并从两方面监控隧道：来自 `wg` 的握手时间，**以及**在隧道*内部*对 hub 的 ICMP 探测——
   死链路上的新鲜握手看起来也像是活的；
5. 通过 `POST /nodes/{id}/heartbeat` 上报存活状态，并执行响应中返回的内容：变更后的入网材料和一次性命令。

**mbedTLS** 是唯一的依赖，其余全部是 libc。它驱动固件自带的内核 WireGuard，因此静态编译后只有几百 KB，
而不是一个用户态 WireGuard 实现。

## 安装

在摄像机上通过 SSH 执行一行命令，并附上云控制面板中的注册令牌（面板会直接给出已填好的命令）：

```sh
curl -fsSL https://github.com/OpenIPC/cerberus/releases/latest/download/install.sh | sh -s -- tok_...
```

安装脚本会根据摄像机的 CPU 选择对应的构建，使用同一发布中的 `SHA256SUMS` 进行校验，安装
`/usr/sbin/cerberus`，由它写入 `/etc/init.d/S99cerberus`，将地址和令牌写入 U-Boot 环境变量并启动代理，无需重启。
出错时会打印一个简短的代码，例如 `cerberus: E13`，以及解释该代码的页面：
<https://cloud.openipc.ru/docs/openipc-camera/#codes>。

完全卸载：

```sh
curl -fsSL https://github.com/OpenIPC/cerberus/releases/latest/download/install.sh | sh -s -- --uninstall
```

同一发布中的 `uninstall.sh` 仍然可用，效果完全相同——保留它是为了两个脚本合并之前已经给出的命令。

手动安装，效果相同：

```sh
# cerberus-arm、cerberus-mipsel 等——见下文“发布”
curl -fsSL -o /usr/sbin/cerberus https://github.com/OpenIPC/cerberus/releases/latest/download/cerberus-arm
chmod +x /usr/sbin/cerberus
cerberus init-script                   # 写入 /etc/init.d/S99cerberus，由它启动这个二进制文件
fw_setenv cerberus_api https://cloud-api.openipc.ru/v1
fw_setenv cerberus_enrollment_token tok_...
/etc/init.d/S99cerberus restart
```

旧的说明会从发布中下载 `S99cerberus`，现在会得到 404：该文件已不再发布。请改为运行 `cerberus init-script`，如上所示。

在面板中为单台摄像机签发的令牌会把摄像机绑定到签发者名下。使用批量令牌时，代理会打印认领码
（`cerberus status` 也会显示），在面板中输入该认领码即可认领摄像机。

## 发布

只有一个发布：[`latest`](https://github.com/OpenIPC/cerberus/releases/latest)，标题为 `Cerberus <版本>`；每当版本号变化时都会重新发布。
它为每种 CPU 架构提供一个**静态**二进制文件：musl 和 mbedTLS 已编译在内，因此无论固件使用哪种 libc、
带有哪个版本的 mbedTLS，甚至没有 mbedTLS，都没有关系：

| 文件               | 适用于                                                                                               |
|--------------------|------------------------------------------------------------------------------------------------------|
| `cerberus-arm`     | 从 ARMv5 到 ARMv7 的所有 ARM 摄像机——海思、国科、星宸、富瀚……（软浮点，因此也支持没有 VFP 的 ARMv5） |
| `cerberus-mipsel`  | MIPS 摄像机——君正 T10、T20、T21、T31、T40……                                                          |
| `cerberus-aarch64` | 64 位 ARM                                                                                            |
| `cerberus-x86_64`  | 虚拟机和测试环境                                                                                     |
| `install.sh`       | 安装脚本；加 `--uninstall` 则卸载 agent                                                              |
| `uninstall.sh`     | `install.sh --uninstall` 的旧地址，为已经给出的命令保留                                              |
| `SHA256SUMS`       | 以上所有文件的校验和，安装脚本据此校验                                                               |
| `LICENSE.md`       | 许可证                                                                                               |

文件名中故意不带版本号：`releases/latest/download/<文件名>` 始终指向最新发布。

每个新版本的二进制文件也会发布到 OpenIPC 的 Telegram 频道（[我们的频道](https://openipc.org/our-channels)），用于存档和测试下载。

各版本的变更记录见 [CHANGELOG.md](CHANGELOG.md)。

| 二进制文件         | 字节    |
|--------------------|---------|
| `cerberus-arm`     | 304 300 |
| `cerberus-mipsel`  | 397 772 |
| `cerberus-aarch64` | 329 240 |
| `cerberus-x86_64`  | 312 904 |

其中大部分是只保留 TLS 客户端所需功能的 mbedTLS（`ci/build.sh` 列出了关闭的选项，并在每次构建时用本地服务器检查 TLS），代理本身只有约 30 KB。

## 摄像机需要具备

| 需要                        | 用途                   | 缺失时                               |
|-----------------------------|------------------------|--------------------------------------|
| 内核 WireGuard + `wg`       | 整个数据平面           | 代理无法工作                         |
| `ip`                        | `wg0` 上的地址和链路   | 同上                                 |
| `fw_setenv` / `fw_printenv` | 刷写固件后仍保留的存储 | 退回到文件存储（刷写后不会保留）     |
| `ping`                      | 隧道内探测             | 探测关闭并给出警告，握手监控仍然有效 |
| `reboot`                    | `Reboot` 命令          | 命令记录为无法执行                   |
| 支持 TLS 的 `curl`          | 安装脚本               | 手动下载文件                         |

## 配置

`S99cerberus` 读取 U-Boot 环境变量，并以 `CERBERUS_*` 变量的形式传给代理：

| U-Boot 键                   | 变量                        | 默认值                                            | 含义                                                           |
|-----------------------------|-----------------------------|---------------------------------------------------|----------------------------------------------------------------|
| `cerberus_api`              | `CERBERUS_API`              | —                                                 | 协调器地址，包含 `/v1`                                         |
| `cerberus_enrollment_token` | `CERBERUS_ENROLLMENT_TOKEN` | —                                                 | 所有者签发的一次性令牌                                         |
| `cerberus_hostname`         | `CERBERUS_HOSTNAME`         | —                                                 | 摄像机在面板中显示的名称                                       |
| `cerberus_ca`               | `CERBERUS_CA`               | `/etc/ssl/certs/ca-certificates.crt`              | TLS 使用的 CA 文件或目录                                       |
| `cerberus_iface`            | `CERBERUS_IFACE`            | `wg0`                                             | WireGuard 接口                                                 |
| `cerberus_subnet`           | `CERBERUS_SUBNET`           | `10.80.0.0/16`                                    | 仅作后备：网络子网随入网材料下发                               |
| `cerberus_heartbeat`        | `CERBERUS_HEARTBEAT`        | `20`                                              | 心跳间隔（秒），`0` 表示关闭                                   |
| `cerberus_ping_target`      | `CERBERUS_PING_TARGET`      | hub                                               | 隧道内探测的目标                                               |
| `cerberus_ping_interval`    | `CERBERUS_PING_INTERVAL`    | `60`                                              | 探测间隔（秒），`0` 表示关闭                                   |
| `cerberus_ping_timeout`     | `CERBERUS_PING_TIMEOUT`     | `3`                                               | 单次探测等待的秒数                                             |
| `cerberus_ping_fails`       | `CERBERUS_PING_FAILS`       | `3`                                               | 连续多少次探测无响应后重建接口                                 |
| `cerberus_events`           | `CERBERUS_EVENTS`           | `1`                                               | 移动事件；`0` 表示不安装钩子、不发送事件                       |
| `cerberus_snapshot_url`     | `CERBERUS_SNAPSHOT_URL`     | `http://127.0.0.1/image.jpg?width=640&height=360` | majestic 的截图；需要密码时写成 `http://user:pass@127.0.0.1/…` |

`cerberus_ca` 比看上去更重要：固件可能根本不带 CA 证书包，此时注册会因协调器证书校验失败而中断，
除了重新刷写固件之外无法修复——请将其指向根证书（Let's Encrypt 使用 ISRG Root X1）。

仅由代理读取的变量：`CERBERUS_STORE`（`auto` \| `uboot` \| `file:/path`）、`CERBERUS_STALE_AFTER`
（`180`，多少秒没有握手后重新设置对端）、`CERBERUS_RETRY_MIN` / `CERBERUS_RETRY_MAX`（`5` / `300`，
注册重试的退避时间）、`CERBERUS_INSECURE=1`（跳过 TLS 校验，仅用于调试）、`CERBERUS_DRY_RUN=1`
（只打印 `ip`/`wg` 命令而不执行）、`CERBERUS_MOTION_HOOK`（`/usr/sbin/motion.sh`）、
`CERBERUS_MOTION_QUIET`（`10`，多少秒没有运动后结束一个事件）、`CERBERUS_MOTION_MAX`（`600`，
无论如何，事件持续这么多秒后被截断）、`CERBERUS_PIDFILE`（`/var/run/cerberus.pid`）、`CERBERUS_SEED`（`/etc/cerberus/seed`，在两次启动之间保存的随机种子，
使启动无需等待内核）、`CERBERUS_RANDOM_DEV`（`/dev/urandom`）、`CERBERUS_GETRANDOM=0`（不先调用 getrandom(2)）。

```
cerberus run       # enroll + up + watch（默认；S99cerberus 启动的就是它）
cerberus enroll    # 仅注册，幂等
cerberus up        # 用已保存的材料启动接口
cerberus status    # 已保存的内容、握手时间、隧道是否有响应
cerberus keygen    # 打印密钥对（stdout 为数据，诊断信息输出到 stderr）
cerberus clean     # 移除代理：接口、启动脚本、已保存的材料以及二进制文件本身
cerberus motion    # 像 majestic 的钩子一样，通知正在运行的代理“有移动”
cerberus snapshot  # 从 CERBERUS_SNAPSHOT_URL 取一帧到 /tmp/cerberus-snapshot.jpg
cerberus init-script [PATH|-]  # 写入启动本二进制文件的启动脚本 [/etc/init.d/S99cerberus]
```

### 以前的 `streq_*`

在本仓库之前，这些键名为 `streq_*`，变量名为 `STREQ_*`。当时安装的摄像机无需任何操作即可继续工作：

- 启动时，代理会把自己的键——设备密钥、认领码、入网材料——从 `streq_*` 复制到 `cerberus_*`，
  逐一校验，此后只写入新名称；
- `streq_*` 副本**保留不动**，因此旧版 `S99cerberus` 或回退到的旧版二进制文件仍能找到一台可用的摄像机；
- `S99cerberus` 读取 `cerberus_<名称>`，不存在时回退到 `streq_<名称>`，因此按旧说明写入的设置依然有效；
  `STREQ_*` 变量同样被接受；
- 注销（节点被撤销）、`cerberus clean` 和 `install.sh --uninstall` 会同时删除两种名称，因此旧副本永远无法让已撤销的节点复活；
- 当没有需要复制的内容时，启动过程不会向闪存写入任何东西。

## 工作原理

### 数据存放位置

设备密钥、节点 ID、overlay 地址、hub 地址和密钥、allowed IPs、网络子网、hub 的 overlay 地址以及认领码——
全部保存在 U-Boot 环境变量中。

**密钥也放在那里，这是有意为之。** 在摄像机上，`/etc` 是 squashfs 或刷写固件时会被覆盖的分区上的 overlay，
而 U-Boot 环境变量中的节点 ID 能在刷写后保留。如果摄像机保留了节点 ID 却丢失了密钥，它会带着一个 hub
认识的身份回来，却没有能证明这一身份的密钥——而只要保存了节点 ID，就会跳过注册。密钥和节点 ID 必须同生共死。
`wg set private-key` 需要的是文件路径，因此密钥只在一次调用期间写入 `/tmp`（0600），随即删除，从不写入 rootfs。

### 控制平面：REST

完整节点与协调器之间保持 SignalR 连接。用 C 实现这样的客户端并不适合这个程序，因此摄像机改用对应的 REST
接口：每隔 `CERBERUS_HEARTBEAT` 秒发送一次心跳，而心跳的响应就是它全部的反向通道：

```jsonc
{
  "command": "None",          // None | Restart | Reboot —— 一次性命令，只下发一次
  "join": {                   // 当前的入网材料，总是完整的，而不是增量
    "kind": "Wireguard", "overlayIp": "10.80.0.7",
    "hubEndpoint": "hub.example:51820", "hubPublicKey": "base64…", "allowedIps": "10.80.0.7/32"
  },
  "mapVersion": 42
}
```

代理将 `join` 与已保存的内容比较，**只应用差异**：当 hub 迁移或更换密钥时，摄像机会自行更新存储并重新配置接口。
`Restart` 会重新执行代理本身；`Reboot` 即 `sync` 加 `reboot`。心跳返回 `404` 表示协调器已不再认识该节点：
代理会丢弃注册信息并停止，以便下次启动时重新注册，而不是永远用一个失效的节点 ID 反复请求。

### 隧道监控

握手新鲜并不代表隧道可用：即使隧道中没有任何流量，WireGuard 也会每两分钟重新握手一次——路由被抢占、
接口被他人占用、hub 不转发任何数据。只有返回的数据包才能说明隧道可用。恢复措施按代价从低到高排列，
摄像机会自行逐级执行：

| 观察到的情况                            | 采取的措施                              |
|-----------------------------------------|-----------------------------------------|
| 握手时间超过 `CERBERUS_STALE_AFTER`     | 重新设置对端（同时重新解析 hub 的域名） |
| 连续 `CERBERUS_PING_FAILS` 次探测无响应 | 拆除并重建接口                          |
| 达到两倍次数——重建无效                  | 代理重新执行自身                        |

协调器不可达时 `cerberus run` 也不会退出：摄像机上的 init 只启动它一次，此后不再关注，因此注册会以倍增的间隔
不断重试，直到成功。

视频不经过代理：中继服务器通过隧道直接从摄像机拉取 `rtsp://{overlayIp}:554/stream0`。该代理不做点对点连接——
摄像机始终走经由 hub 的路径，而这条路径总是可用的。

## 构建

代理就是 `src/` 目录：`src/cerberus.c` 和它的 `Makefile`——构建和安装不需要其他任何文件。文件中没有启动脚本：
它由二进制文件自带并写入（`cerberus init-script`），因此启动脚本不可能与它所启动的代理来自不同的发布。
顶层的 `Makefile` 只是把命令转交给它。

```sh
make                                   # 本机构建，链接系统的 mbedTLS（3.x 或 2.28 LTS）
make STATIC=1                          # 静态构建
make VERSION=1.2.0                     # 代理上报的版本（默认取自 src/cerberus.c）
make install DESTDIR=/tmp/rootfs       # /usr/sbin/cerberus + 由它写入的 /etc/init.d/S99cerberus
```

发布用的二进制文件由 `ci/build.sh` 构建，只需要 Docker：它会下载 [Bootlin](https://toolchains.bootlin.com)
的 musl 工具链和 mbedTLS 源码（均按 sha256 校验），进行静态构建，检查 ELF 架构，然后在 QEMU 下**实际运行**
每个二进制文件，并将其 X25519 密钥与 `wg pubkey` 的结果进行比对：

```sh
ci/build.sh                            # arm、mipsel、aarch64、x86_64 → dist/cerberus-<arch>
ci/build.sh mipsel                     # 其中之一
ci/assemble.sh                         # + 安装脚本、SHA256SUMS → dist/ 中的完整发布
```

在固件构建环境中，使用固件自带的工具链，指向 `src/`：

```sh
make -C src CC=mipsel-openipc-linux-musl-gcc MBEDTLS=<buildroot-staging> STATIC=1
make -C src install DESTDIR=<target-rootfs> RUN=<host-dir>/bin/qemu-mipsel
```

`make install` 通过运行刚构建的二进制文件来获得启动脚本，而交叉构建自身做不到这一点：请在 `RUN` 中指定用户态模拟器
（Buildroot 的 `host-qemu`；静态二进制文件不需要 sysroot），或者传入 `INIT_SCRIPT=no`，由固件在摄像机上运行
`cerberus init-script`。两者都没有时，安装会停止，而不是装上一个没有任何东西去启动的代理。

## 测试

```sh
make test                                                  # Alpine：musl + mbedTLS 3.x
CERBERUS_TEST_IMAGE=debian:bookworm-slim tests/run.sh      # glibc + mbedTLS 2.28
```

测试只需要 Docker。测试套件使用 `-Werror` 构建代理，并通过真实的 TLS 让它与一个模拟协调器交互：身份、TLS 校验、
注册、反向通道（命令、迁移或更换密钥的 hub、协调器不可用、节点被撤销）、启动时的退避、在模拟 U-Boot 环境上的
`streq_*` 迁移、从磁盘上的发布目录端到端运行安装脚本——以及一条连接到独立网络命名空间中 hub 的真实内核 WireGuard
隧道，包括隧道内探测必须发现并修复的被抢占路由。数据平面部分需要宿主机支持内核 WireGuard；`REQUIRE_WG=1`
会将其缺失视为失败，CI 就是这样运行的。

## CI 与发布

每次 push 和 pull request 都会运行 shellcheck、两代 mbedTLS 上的测试套件以及四个发布构建。当推送到 `master` 带来新的
`CERBERUS_VERSION`（`src/cerberus.c`）时，CI 会用这些二进制文件连同安装脚本、许可证和 `SHA256SUMS` 替换唯一的发布
`latest`，标题为 `Cerberus <版本>`；不带新版本号的推送不会发布任何内容。编译进二进制文件的版本号就是
`CERBERUS_VERSION` 本身，不再使用 `v*` 标签。发布方法：在 pull request 中提升版本号并添加其 `CHANGELOG.md` 章节，然后合并。

版本号格式为 `MAJOR.MINOR.PATCH`（`src/cerberus.c` 中的 `CERBERUS_VERSION`）：PATCH 表示修复和小改动，MINOR
表示新功能，MAJOR 表示不兼容的变更或大规模更新，仅由维护者决定。修改 `src/`、安装脚本、`Makefile` 或 `ci/`
的 PR 需要在合并前的最后一个提交中提升版本号，并在 `CHANGELOG.md` 中添加对应章节；仅修改文档、测试或
`.github/` 中工作流的 PR 不提升版本号。CI 会在每个 PR 中检查这一点（`.github/check-version.sh`）。

不改变二进制文件的代码或构建修改（整理构建脚本、注释）可以改用 `no-release` 标签：不提升版本号，也不发布。
没有按版本的发布可供固定：回滚就是 revert 加上一个新版本号。

## 许可证

[The Prosperity Public License 3.0.0](LICENSE.md)：非商业用途免费，商业用途可试用 30 天。完整条款见
[LICENSE.md](LICENSE.md)。

## 支持

- 问题与缺陷反馈：[GitHub Issues](https://github.com/OpenIPC/cerberus/issues)
- 文档：[openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
