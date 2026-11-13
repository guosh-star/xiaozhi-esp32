# 原 ESP32-S3 布谷鸟停止/取消安全修复记录

日期：2026-11-13。

## 1. 版本与发布范围

- 仓库：[guosh-star/xiaozhi-esp32](https://github.com/guosh-star/xiaozhi-esp32)。
- 基线：`cuckoo-clock-full-20261024`，完整 SHA `24dfd87496090bb96ff08617ee175e78a32822c2`（2026-10-28 提交）。本次不是旧 main 或 S31 移植工程。
- 修复分支：`fix/cuckoo-stop-safety-20261113`。
- 程序与测试提交：[`ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd`](https://github.com/guosh-star/xiaozhi-esp32/commit/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd)，源码 tree `bd522708f7de2a45b39ab462fa6362ff1a746f57`。
- 代码先单独推送，并以 `git ls-remote` 和 GitHub API 核验同一完整 SHA；本文在代码推送确认后，作为同分支的独立文档提交。没有写入原基线分支、force push、自动合并、创建 PR 或更改 CI。
- 最终审查结论：**PASS_WITH_DOCUMENTED_BUILD_LIMIT**。独立 QA 关闭六项停止/归属问题后，最终定向复核又确认了 ducking 局部修正，并重新运行公开全部七套测试。放行范围仅为**新源码分支发布**。
- **完整 S3 SDK 固件构建仍为 BLOCKED；没有烧录或硬件验收。** `safeToFlash=false`，`hardwareAccepted=false`。

## 2. 错误现象、触发与修改理由

### 2.1 停止之后继续动作，或旧任务混入新表演

原 `StopAll` 主要清 running 并停止当时的输出，却没有为整轮工作、嵌套调用及排队 child 绑定不可变身份。长等待返回后，旧函数仍可能执行 Sweep、前进、补偿反转、关门、上电或 cleanup。running 被下一轮重新设为 true 时，旧任务还会把新 true 当成自己的许可。

固定注入的宿主负例中，`MusicDogIntro` 停止后仍出现120次非零狗尾 PWM、1次 M3 驱动及1次重新上电请求；Dance、KidsRest、音乐结束补偿和旧 loop 重启交错也复现了旧写。**这些数字仅属于所测宿主调度，不是实机次数或发生概率。**

因此不能只再加一个 bool、volatile、atomic 或循环 if：它们不能区分哪一轮工作，也不能消除“检查后被抢占、实际写之前已停止”的窗口。

### 2.2 只换 epoch 仍会遗留已置位的旧输出

首轮修复的独立 QA 发现：自动 Intro 已让 M2 正转时，新 Show 接纳推进 epoch，旧 Intro 提前返回；它后面的 Stop 不能再执行，M2 的旧电平却仍锁存。创建新 worker 失败时也一样。

最终接纳事务先 `RetireMotion`：在短 fence 中退役所有电机端点、舵机 PWM、LED、供电及相关运动计数/intro/outro/flash 状态，再占用 running、推进 epoch。失败路径再次退役并暂停自动动作。**没有给失效旧 cleanup 开后门**，也不依赖新 worker 以后碰巧覆盖旧输出。

### 2.3 无父 scope 的真实唤醒 StopAlarm

Application→Board 的唤醒停止闹钟入口并不经过 MCP scope。状态字段改为受 fence 管理后，如果公共 `StopAlarm` 不自建 scope，停止标志写入会被拒绝，实际 AlarmTask 无法正常结束。

最终由 `StopAlarm` 自身绑定 scope；MCP/已有有效父 scope 仍继承原票，失效父 scope 不能借此换票。新增实际 AlarmTask 在无父 scope 唤醒后退出、unmute 的回归。

### 2.4 背景曲目权不能代替设备音频恢复权

半点表演仍正常运行时，`StopMusic` 或同 epoch 新曲会撤销背景 generation。如果解除 mute、恢复 wake 也要求旧曲 generation，表演结束后可能永久保持 mute/wake 关闭。

最终分为两类：`PerformanceMedia` 只允许当前 performance ticket 做设备 voice/wake/mute/threshold 的 setup/recovery；`PerformanceTrack` 还要求原曲目 generation，才可输出 PCM、停止播放器、清 ring/reset/drain。这样不会因停止音乐剥夺当前表演的恢复责任，也不会放宽旧曲目对新音乐的写权限。新表演仍用新 epoch 拒绝旧恢复。

### 2.5 旧在线 defer 覆盖新离线选歌，或旧播放收尾清新流

只在在线入口推进 pending 编号不够：Speaking 时在线 A 排队，随后离线 `PlayMusic(2)` 被接受，旧 A 仍可能在 Speaking 结束后反抢。

最终显式新曲接纳统一撤销旧 pending；延迟请求持有自己的 URL/request，实际在线 start 在媒体事务中再次核对 expected_request。背景 generation 独立于机械 epoch：已接受背景音乐可跨机械停止继续，显式 StopMusic/新曲则撤销旧 token，旧 decoder/finalizer 不得清新 ring。

### 2.6 真实离线 queue、fallback 与对象寿命

原永久播放 worker 轮询共享 pending 字段，真实离线自然结束不会退役新增 background owner；后续 fallback chime 又可能因陈旧 bool 被误判成应保留的音乐。

最终本地文件/chime 使用不可变 `OfflineJob`，携带 track/hour、sequence、原 ticket、background 类型及媒体 token 回调。chime 在实际调度与每个 PCM chunk 仍核旧归属；自然 EOF/失败只退役自己的状态，不能清新离线或新 stream。私有 decoder/input/output 避免跨任务复用或关闭别人的解码器。

每播放器 lease 在可能等待媒体锁之前就持有，覆盖排队、创建失败、取消、closing 和析构；析构 cancel 后等待 lease 释放。既有网络 transport 未交还共享通道时，新离线接纳按原 Stop 请求作最多5秒有界等待，未交还则不 publish。**本次未全面重写或验收 HTTP/Opus 网络播放器。**

### 2.7 创建失败、PCM 释放与 FreeRTOS 自删

创建失败不能只清 running：如果已经关闭 voice/wake，还必须由仍拥有设备 ticket 的失败路径恢复。旧失败遇到 StopAll→新 setup 后，恢复权限应失效，不能覆盖新状态。新离线创建失败接替旧已 mute 曲目时，由当前新 token 负责恢复，不依赖旧失效 finalizer。

长等待改为取消返回也不能跳过手工 free。PCM/decoder 采用 RAII；任务 wrapper 在 `vTaskDelete` 前先销毁 scope、closure 和资源，因为 FreeRTOS 自删不会替 C++ 展开栈对象。

### 2.8 正常 ducking 的局部回归

第二次独立 QA 用非零 PCM 发现：`duck_state`、`duck_transition_us` 虽已去掉共享 static，却仍声明在每帧处理块中，每帧归零，持续 Speaking640ms 后输出仍为10000。

最终只将两条普通局部声明移到本次 `DecodeSingleFile` 调用的 decode loop 外，与 `decode_gain` 同寿命；**不恢复共享 static，不改 owner/其他机制**。非零对照现为10000→2000，Idle 后恢复10000，下一次 decoder 调用初始状态独立。

## 3. 修改位置与新旧差别

以下位置全部固定到上述程序提交，不随分支后续文档提交漂移。

- [cuckoo_action_gate.h：Write/Cancel](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_action_gate.h#L53-L64)、[Scope](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_action_gate.h#L125-L136)、[TaskEntry](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_action_gate.h#L211-L227)：从“布尔检查后任意写”改为 task-handle 绑定不可变票；末端校验与 GPIO/LEDC/state 写共享短递归 fence。创建前 reservation，失败/取消释放；service/close 有独立寿命计数。
- [cuckoo_controller.h：ClaimPerformance](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_controller.h#L572-L582)、[PerformanceMedia/Track](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_controller.h#L596-L615)、[OfflineJob](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_controller.h#L180-L191)：明确机械接纳、设备音频、曲目及播放器对象四类归属，不用同一个 generation 代替所有权。
- [part1：Motor/Servo 末端](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part1.cc#L173-L189)、[Servo::Sweep](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part1.cc#L274-L284)、[DecodeSingleFile](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part1.cc#L416-L652)、[每次调用的 duck 状态](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part1.cc#L420-L421)：包括 Stop/Release 也受 fence；Sweep/解码等待可取消；解码资源私有，duck 状态跨帧保留但不跨 decoder 共享。
- [part2：QueueOffline](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part2.cc#L1245-L1300)、[PlayTaskEntry](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part2.cc#L1302-L1338)、[DecodeToBuffer](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part2.cc#L822-L953)、[DecodeStreaming](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part2.cc#L962-L1061)、[Bell Async](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part2.cc#L1183-L1191)：用不可变 job 替代共享 pending 轮询，本地/chime queue、实际 Sync、PCM chunk 和 finalizer 都按归属执行；PCM/decoder/lease 有释放路径。
- [part3：RetireMotion](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part3.cc#L1085-L1106)、[FailPerformanceStart](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part3.cc#L1107-L1118)、[StartPerformance](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part3.cc#L424-L482)、[PerformanceTask](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part3.cc#L491-L675)、[StopAlarm](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part3.cc#L982-L1003)：原子退役端点、创建失败恢复、半点旧 cleanup、取消 PCM 与真实无父 scope 入口一起处理。
- [part4：StopAll](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part4.cc#L912-L941)、[PlayShowMusicBg](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part4.cc#L231-L270)、[PlayMusic](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part4.cc#L888-L902)、[StopMusic](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part4.cc#L947-L958)：raw 机械停先在线性化 fence 中完成；慢媒体/网络/等待/创建 task 不进入该 fence。音频恢复由预持票且计入 lifetime 的 worker 执行，旧恢复不能覆盖新 admission；恢复 worker 创建失败会告警，不能承诺低内存下音频必恢复。
- [part5：MusicDanceTick](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part5.cc#L227-L372)、[KidsRest](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part5.cc#L576-L604)、[PlayOnlineMusicNow](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part5.cc#L1073-L1186)、[MCP 注册](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_part5.cc#L1421-L1973)：补偿、Intro/Outro、LED、延迟 URL、返回类型边界均使用明确票据；自动 tick 停止后不擅自重启机械动作，显式新动作/新表演可以恢复。
- [cuckoo_board.cc：时钟 service](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_board.cc#L147-L162)、[BOOT/TOUCH 入口](https://github.com/guosh-star/xiaozhi-esp32/blob/ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd/main/boards/cuckoo-clock/cuckoo_board.cc#L163-L226)：把实际按键与250ms tick 纳入边界，而非只保护同步 MCP 调用。

20ms 是可取消等待的轮询设计粒度，**不是目标板硬实时或物理停止时限**。停止后不再做补偿回正/回家演出；释放 PWM 后实际机械位置未知。

## 4. 审查迭代与正常功能保持

- 首轮独立 QA 的 S3-01～06 均已修正，第二次独立 QA 复核关闭；旧失败退出23/24/25及对应对照没有被删除来伪造通过。
- 第二次发现的正常 ducking 回归旧退出27也保留；最终定向复核确认仅移动两条声明，并亲自重跑最终七套公开测试。
- 正向 Intro、KidsRest 补偿、半点结束恢复、闹钟唤醒退出、背景音乐保留、新曲替换、创建失败恢复均有软件对照。
- 没有迁移到 S31；没有更改引脚映射、分区、音频素材、MCP工具名称或原分支历史。`StopAll` 不负责停止背景音乐；停止音乐使用 `StopMusic`。
- 取消后的旧任务不会靠重新读取新 running 续命，也不能以 Stop/Release/清 ring 为名破坏新拥有者。

## 5. 实际测试、环境与命令

公开入口：

~~~sh
node tests/cuckoo-stop/run.cjs
~~~

需要 Node.js、git（包含完整基线对象）和支持 C++20/pthread 的 g++。实际运行于已有 Linux/WSL host 环境；使用 `-std=c++20 -pthread -Wall -Wextra`。可用 `CXX` 选编译器、`CUCKOO_STOP_OUT` 选独立输出目录；Windows 可用 `CUCKOO_WSL_DISTRO` 指定已安装发行版。产物不要求写入固件 build 目录，没有 SDK安装或设备访问。

| 套件 | 实际编译/运行退出 | 核验内容 |
| --- | --- | --- |
| baseline | 0 / 42（预期负例） | 固定 git show 完整 base SHA，11个缺陷交错仍可见；不把新 HEAD 当旧版 |
| fixed | 0 / 0 | 12动作场景+26命名断言；204个正反 Sweep 取消边界、1000次 stale 写等 |
| extra-mechanical | 0 / 0 | 9检查：全端点、真实 Intro/Outro 新接纳成功/失败、重启旧 cleanup |
| extra-media | 0 / 0 | 9检查：无父 scope StopAlarm、实际 AlarmTask、同 epoch 媒体替换、离线撤销旧在线 pending |
| extra-admission-failure | 0 / 0 | 2检查：失败恢复，以及旧创建失败不能覆盖新 setup |
| real-offline | 0 / 0 | 20检查：真实 Mp3 类、29个真实方法、queue/chunk/EOF/finalizer/lease/析构 |
| normal-ducking | 0 / 0 | 4检查：非零淡出、恢复、独立状态、禁用对照 |

非零 ducking 实际结果：64帧、虚拟640ms，first=10000、last=2000；Speaking后切 Idle 的128帧/虚拟1280ms，min=2000、last=10000；新 decoder 首帧10000，禁用对照64帧均10000。

Bell 使用零值小 fixture；离线及 ducking 的 Assets/codec/RTOS/Audio 是明确模拟 backend，PCM 由零值或10000的合成样本构成，不使用私人媒体或真实音频输入。被测 QueueOffline/PlayTaskEntry/DecodeSingleFile 等是原样提取真实方法，不等于实际 ESP codec 库或外设已验收。

真实体 provenance：73 host（33 baseline+40 fixed）、81 native、29 offline，共183条逐字符/hash匹配，**有重叠，不是183个不同函数**；实际 Mp3 class 与 header 一致，非零用例编译当前 decoder 原体。另以真实 controller header、声明级 SDK/backend，`CONFIG_IDF_TARGET_ESP32S3=1 -fsyntax-only` 核验81个实际方法，退出0。编译告警保持可见，不宣称无 warning 构建。

开发中的 lvalue 编译失败、漏 scope/释放路径、测试提取/路径及局部声明寿命问题均经过修正与重跑；没有把中间 running、旧结果或声明检查当最终固件通过。

## 6. 完整构建与硬件限制

完整原 S3 工程需要 ESP-IDF **5.5.4**、兼容 Python 和 Xtensa toolchain，现有环境缺失；已有 IDF6.1/RISC-V 不能代替。原顶层 CMake 还强制原 SDK 路径。本次没有为了绕过限制更改 SDK/目标平台/全局配置、进行大安装，或运行带 flash 的批处理。

原 CI push 只覆盖 main/ci/*，本 fix/* 不自动取得完整构建结果；CI 的 IDF5.5.2 也不等于本工程5.5.4的构建证明。CI未改。

**仍未获得**目标链接/BIN/ELF、尺寸与分区/资源余量、真实 FreeRTOS 双核/优先级/锁 WCET、机械电源/PWM、真实音频网络或睡眠电流的通过证据。已进入的同步媒体调用可能完成，不能宣称声音在 StopAll 返回瞬间物理消失。原有网络 transport 全生命周期与整个应用销毁过程不属于本次全面重写/验收范围。

因此限定放行不代表 firmware/hardware PASS。合并或烧录前仍须补齐完整目标构建、资源检查和安全台架验收；当前 `safeToFlash=false`，`hardwareAccepted=false`。

## 7. 回退办法

程序提交为 `ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd`。需要撤销时，使用普通逆向提交，不改写历史：

~~~sh
git revert ac4145474e172d59f44f1fb8bd6c0fffdca7c8bd
~~~

若同时撤销本文，先 revert 文档提交，再 revert 上述程序提交。没有合并到其他分支时，也可以停止使用该修复分支而保持原基线不动。不要 force push、覆盖原分支，或把未验证回退镜像直接烧录。
