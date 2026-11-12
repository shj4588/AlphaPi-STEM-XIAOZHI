/*
 * AlphaPi_Xiaozhi —— STEM 板（ESP32-C3）极简小智 AI 客户端
 *
 * V2.4.13（2026-11-12 凌晨）：**修「跳到集尾不接下一集，反而倒回 28 秒重播本集」**。
 *   ★实机：第 8 集 dur=771s，用户「跳到第十二分钟」(720s) 播到集尾后，服务端日志反复
 *     「第 8 集起 1 集，跳 741s」+「设备上报位置 pos=769s」+「墙钟兜底 wall=30s」，
 *     每条记录都 devsync 但**没有 fin**，永远不跳第 9 集。用户听感「回退了 28 秒」。
 *   ★★真因：`MS2B(uint32_t ms)` 参数是**毫秒**，而 V2.4.10 的断流判据写成
 *     `MS2B(15)` / `MS2B(30)`，注释却说「差 15s / 30s」——实际只有 360B / 720B
 *     （15ms / 30ms 音频）。任何一次正常 EOF 只要尾部少收 15ms 就被误判成
 *     「流被提前掐断」⇒ 不报 fin、2s 后 `ss≈769` 原地续播；服务端 v1.6.3 的防越界
 *     夹值 `off = dur-30 = 741` 又把它倒回 30 秒 ⇒ 30s 后 EOF、再判断流、再倒回，
 *     循环 3 次都不跳集（769−741=28，正是用户说的「回退 28 秒」）。
 *   修法：① `MS2B(15000)` / `MS2B(30000)`（单位改对）；
 *     ② `atTail`（已听到集尾 5s 内）一律按「本集播完」处理，绝不原地续播；
 *     ③ 原地续播的 `ss` 也夹到 `dur-3`；服务端 v1.6.8 同步把 `dur-30` 改成 `dur-3`。
 * V2.4.12（2026-11-11 深夜）：**语音调进度 + 读时长/位置 + 读信号强度**。
 *   ① 新 MCP 工具 `seek_play`：sec=绝对「跳到第 N 秒/第 N 分钟」，delta=相对
 *      「快进/后退 N 秒」（正数快进、负数后退）。实现 = 同集 `&ss=<目标秒>` 原地重开流，
 *      服务端 v1.6.0 起就支持 `-ss` 输入侧快速定位，无需改服务端；目标夹在
 *      [0, X-Book-Dur-3]，避免落在文件外导致 ffmpeg 零字节「嘀一声」。
 *   ② `current_play` 增强：书名/集数之外再答 **总时长 / 已播到 / 还剩**，
 *      小智能直接回答「这集多长」「现在播到哪了」「还剩多久」。
 *   ③ 新 MCP 工具 `signal_status`：WiFi.RSSI() + 分档结论 + 卡顿建议，
 *      小智能判断「信号好不好 / 怎么老卡」。
 *   ④ STT 兜底（仅 MCP 未握手时）支持中文数字口令：快进五分钟 / 后退三十秒 /
 *      跳到第十分钟；两路互斥，MCP 就绪时仍只交给云端。
 * V2.4.11（2026-11-11 深夜）：**修「真播完却被判没播完 → 续播不跳集」+ 卡顿优化 2/3/4**。
 *   实机：第 6 集 dur=828s(13:48) 真播完，设备却报 pos=602s(10:02)、fin=1，服务端
 *   apFinSane 按"离集尾还远"把正确的 fin 拦下 ⇒ 下次续播倒回同一集。根因不是掐流，
 *   是播放泵的「饿透恢复推头/段界」每次把 dlSentTotal 清零，而 mLibPlayedSec() 只读它
 *   ⇒ 三次重锚砍掉 226s。修法：① mLibAckBase 单调累计（重锚只累加不丢）；
 *   ② 本地流不进饿透状态机（连续 PCM 不重推 WAV 头）；③ 真播完位置直接取 X-Book-Dur
 *   集尾；④ DL_SIZE 20KB→32KB 吸收网络抖动；⑤ 本地流停供日志改打 socket/RSSI 口径。
 * V2.4.10（2026-11-11 夜）：**修「一集被误判成播完」**（13.8 分钟的集子断在 6.8 分钟，
 *   设备照报 fin=1 ⇒ 记录标成已播完、续播跳走一整集）。
 *   ★真因：fin=1 的唯一来源是"设备看到流结束"（mLibEnded），但**流被服务端/网络提前
 *     掐断**是同一个表现 —— 服务端和固件都没法只凭"流结束了"区分这两种。
 *   ★修法（两层）：① 服务端 v1.6.5 拿 `dur` 对账，pos 离集尾 >5s 的 fin 一律拦下；
 *     ② 固件拿响应头新增的 `X-Book-Dur`（本集时长）对账：收到的字节离"播完整集"
 *     还差 15s 以上 = 断流 ⇒ **不报 fin、2s 后原地续播**（最多 3 次，失败才失望脸收场，
 *     记录停在断点）。真播完（字节对上了）才走老路：fin=1 + 自动接下一集。
 *   ★`PLAY_ACK_LOCK=1`，420s 会话硬顶本就不生效 —— 断在 410s 与它无关，是随机掐断。
 *   ★顺带：新增**当前播放信息**（服务端 v1.6.5 响应头 `X-Media-Title`：歌名-歌手 /
 *     书名·演播·第N集 / 台名）—— MCP 新工具 `current_play` 答「现在放的是什么」，
 *     配网门户状态区也显示；响应头缓冲 640→1024B（中文 URL 编码后很占字节）。
 *
 * V2.4.9（2026-11-11 夜）：**播放位置 30s 心跳**（设备断电不再丢整段进度）。
 *   以前只在「按 C 暂停 / 说停 / 本集播完」才报 /ap/pos —— 设备意外断电、死机、被拔电
 *   时一次都发不出去，服务端只能把记录停在起播点：听了 40 分钟等于白听。
 *   ★现在有声书播放中每 30s 报一次 `beat=1`（只更新 pos，不标结束/暂停），最坏丢 30s。
 *   ★心跳是播放中发的同步 HTTP，会抢主循环 ⇒ 只在 FIFO 水位 >500ms 时发、
 *     connect 200ms / 收尾 100ms 短超时（FIFO 总共才 853ms，阈值不能写 1.2s）。
 *   ★队列里已有真事件上报（暂停/播完）时心跳不许插队。
 *
 * V2.4.8（2026-11-11 夜）：**修「说继续播放后十几秒没动静」**（V2.4.7 的死锁）。
 *   ★V2.4.7 写成「等确认语期间一个字节都不读 socket」，而起播要 FIFO 攒够 700ms 预滚、
 *     预滚只能从 socket 读 —— 确认语一说完就进了「等数据、但永不读」的闭环。
 *     而且挂起期间每拍都刷新 mLibBufAt，连 15s 取流超时都不触发，实际是**永不开始**。
 *   ★现在「挂住不读」与「放行起播」共用同一组判据（libWaited / libSpeaking），只在
 *     两种情况下挂住：① 本轮 tts 已开口（等 stop，或尾音还在协控里播）；② 本轮还没
 *     开口且仍在「它可能马上开口」的窗口内（无动静 0.5s / 服务器在生成 6s）。
 *     窗口一过立刻读 socket 灌预滚，20s 绝对兜底。
 *   ★无动静窗口从 1.5s 收到 0.5s（用户口径）；mLibSawReply 也收紧成「真的收到 text」，
 *     情绪/附带字段不再让人白等 6s。
 *
 * V2.4.7（2026-11-11 夜）：**确认语说完才起播 + 播放中图标不被笑脸顶掉**。
 *   ★① 起播判据从"ttsDone"改成"**这一轮** tts 轮次"：工具被调用的那一刻，上一轮的
 *     `tts stop` 还没被新一轮覆盖，ttsDone 仍是 true ⇒ 局域网取流 0.3s 就攒够预滚，
 *     本地 PCM 抢在"继续播放啦"第一个字上桌把它顶断。现在记 ttsSeq（每次 tts start +1），
 *     请求时快照 mLibTtsSeq0：**本轮 tts 已开口 ⇒ 必须等它 stop 且 FIFO 播空**才起播；
 *     本轮一直没开口 ⇒ 1.5s 后认为云端不说话，直接播（12s 绝对兜底）。
 *   ★② 等确认语期间**一个字节都不许进 FIFO**：FIFO 是 TTS 与本地流共用的，提前把歌塞进去
 *     会和"继续播放啦"搅在一起播。socket 不读，TCP 背压让 ffmpeg 自己等着，不丢数据。
 *   ★③ 确认语 stop 且 FIFO 播空 ⇒ 立刻 endDlPlay（静默版：不打屏、不发日志），
 *     不等泵那套"1.5s 无新字节"判据，用户听不到空白。
 *   ★④ 图标：播放中（dlIsLocal）一律显示 kind 图案 —— 情绪脸、音量柱状图恢复都不许
 *     把音符/书/收音机顶掉（以前恢复时画的是 mtxWant，而 mtxWant 早被情绪改成笑脸了）。
 *
 * V2.4.6（2026-11-11 夜）：**播放位置改由设备上报**（修"播放记录位置误判"）。
 *   ★旧做法是服务端拿 `已写进 socket 的字节 / 24000` 当播放位置 —— 本地文件 ffmpeg 转得
 *     比实时快十几倍，一次就能把剩下几分钟全塞进 TCP 缓冲，于是"听了 3 分钟"被记成"集尾
 *     11:17"，续播一下跳走一整集；再叠加续播起点 off，位置越滚越偏。
 *   ★现在设备在**停播/暂停/本集播完**时上报 `/ap/pos?q=&ep=&sec=<集内绝对位置>&fin=`：
 *     位置 = 服务端给的起点（X-Resume-Offset）+ 本次实播（已发送 − 超前量），只有设备自己
 *     知道播到哪。服务端收到后就锁死这条记录（devsync），不再用字节数改写。
 *   ★上报走**队列 + 延迟发送**：不在 mLibStop 里同步发（说话那一刻不能堵 800ms），
 *     而是排队，等播放泵把 FIFO 收尾（或暂停静音）后再发，超时/连不上就丢弃。
 *
 * V2.4.5（2026-11-11 夜）：**有声书自动续播 + 播完重新计空闲**。
 *   ★自动续播：一集真播完（服务端 EOF，不是被打断）后，等 FIFO 里剩下的音频播空
 *     （余量 = 剩余字节/24000 + 400ms），自动请求下一集。判据必须用「服务端正常关流」，
 *     不能拿 pos≈时长去猜 —— 设备中途断连时两个数永远相等。
 *   ★全书的最后一集：服务端回 404（"已经没有第 N 集了"）⇒ 安静收场 + 笑脸，
 *     不报警（自动续播发的请求失败不能给用户一个失望脸）。
 *   ★休眠：播放期间（mLibState / dlActive）**持续刷新空闲基准**，既不许深睡也不让
 *     空闲钟走表 —— 以前一集 11 分钟播完的瞬间就深睡（lastConvAt 还停在上次说话时）。
 *
 * V2.4.3（2026-11-11 夜）：**修「本地播放几分钟就自己停」**。
 *   ★真凶：播放泵里有三条"云端收场判据"，本地媒体库的音频**不经过 ws、也不依赖 TTS**
 *     却照样被它们判死 ——
 *     ① `ttsDone && 1.5s 无新字节 && 协控积压<100ms` → endDlPlay("tts结束")。
 *        ttsDone 只是"确认语说完了"的残留状态，本地流播放中恒为 true；
 *        水位控制下 FIFO 满时本来就 0.5~1.5s 不读 socket ⇒ 随机撞上就收场。
 *     ② `!wsReady && 积压<50ms` → endDlPlay("连接已断")。
 *        播放期间本机不发上行音频，官方服务器约 80s 就把 ws 踢掉 ⇒ wsReady=false；
 *        之后任何一次积压掉到 50ms 以下，正在听的书当场掐断。
 *     ③ 断连时 dlBroken 置位，收场还打"失望脸"，看起来像服务端崩了。
 *   ★修法：这三条判据一律加 `!mLibState`（本地取流中不适用）。本地流的结束只有两个
 *     合法来源：mLibEnded（服务端 EOF → 播空收场）+ mLibTick 的 15s 无字节兜底。
 *   ★顺带：协控不应答的重开次数对本地流放宽（3 → 30），长书里偶发僵死不再判死整场。
 *
 * V2.4.2（2026-11-11 夜）：**设备身份 + 有声书续播**（服务端 v1.6.0 配套）。
 *   ★媒体库请求带 `X-Dev-Mac`（本机 WiFi MAC）+ `X-Dev-Name` —— 服务端按 MAC 记账，
 *     两台机器各记各的播放进度；不带这个头就只能按 IP 猜，换 IP 就认成新设备。
 *   ★解析响应头 `X-Book-Ep`：有声书续播时服务端会自己把集号跳到上次那集，
 *     **本机必须把 mLibPos 同步成服务端实际起播的集号**，否则接着说「下一集」
 *     会拿 mLibPos=0 算出第 1 集（跳过整本书）。这是 V2.4.1 隐藏的坑。
 *   ★响应头缓冲 512 → 640B（v1.6.0 多回两个头，别让长 UA 撑爆）。
 *
 * V2.4.1（2026-11-11）：**本地播放三件套齐全** —— 歌单连播 / 网络电台 / 有声书。
 *   ★背景：V2.4.0 只有"按关键词挑一首歌"，播完就断；说「放个电台」「听某某书」云端
 *     手里根本没有工具，只能自己去搜，用户听到的是云端 search_music 的版本。
 *   ★关键设计：**队列不在设备上**。服务端 v1.5.1 的 `/ap/play?kind=song|radio|book`
 *     用 ffmpeg 把「一张歌单」「一本书的连续几集」concat 成**一条** PCM 流，
 *     本机看到的永远只是"一条音频"，C3 上没有 N 个 rid 的队列、没有第二次取流。
 *     唯一要记的是 (kind, q, pos)，说「下一首/下一集」时带新的 pos 重连即可。
 *   ★三个新工具：play_radio(name) / play_book(keyword, episode?) / next_play()。
 *   ★电台是**无限流**：EOF 不等于"播完了"，会话保险丝临时放宽到 1 小时
 *     （否则播到 7 分钟会被本地场次硬顶自己掐断）。
 *   ★X-Track-Count：服务端在响应头报"本条流实际几首"，"下一首"按实际首数跳，
 *     不死按 6（最后一批可能只有 2 首，死按 6 会直接跳过整批）。
 *
 * V2.1.5（2026-10-01）：**诊断精简** ——「讲着讲着没声」结案后，把周期诊断从"每 2s 两行"收到"每 2s 一行"。
 *   ★结案依据（整合版 V2.1.4 烧录实测，官方服务器，同句"讲一个2分钟的故事"）：
 *     全程 `2073644 字节 / 86956 ms（23847 B/s）` **完整讲完** —— 分段 0 次、回填 0ms、溢出丢样 0、
 *     停供告警 **0 次**；播放期堆从 7.7KB/2.2KB 抬到通用 45.5KB / DMA 23.5KB（连续 9.2KB）。
 *     旁证：全程 `ping 发 9 / 收 pong 9` —— 服务器每个 ping 都回，进程始终活着。
 *   ★本版改动（**只动打印，不动音频/协议一行**，两版同步）：
 *     ①`[DL]   链路:` 每 2s 那条 → **默认关闭**（新增 `dlVerbose=false` 开关，串口按 **v** 切换）。
 *       播放期输出由 **2 行/2s 降为 1 行/2s**（87s 故事：约 88 行 → 约 44 行）。
 *     ②**异常路径的链路行永久保留**（停供/断粮块里那条）—— 它只在出问题时打，永远是关键证据。
 *     ③断开行补 `距pong Xms`，与已有的 `ping 发 / 收 ping / 收 pong` 账本凑成完整"谁先动手"证据链。
 *     ④保留 `[DL] 2s` 吞吐行（同时是"播放还活着"的心跳）、`[WS]`/`[SRV]` 事件行、`[MEM]` 打点。
 *   ★判读口径（V2.1.4 判决沿用）：**pong 还在回 ⇒ 服务端不产音频；pong 停了 ⇒ 我们收不到包**。
 * V2.1.4（2026-10-01）：**UP_SIZE 减半（省 8KB 静态 RAM）+ 一次定案的"链路三件套"**。
 *   ★起因（用户点破）："独立版一直是正常的，不是整合出的问题吗" —— 对。两版对照即控制变量，
 *     独立版不复现 ⇒ 服务端/协议/编解码全线出局；V2.1.2 里"服务端工具调用"的归因**撤回**。
 *   ★实证（`nm` 符号表，不是推测）：两版最大静态项完全相同（upBuf 16384 / dlFifo 20480 /
 *     decBuf 5760），但**整合版多出 2342 个符号**，全是 BLE 协议栈（ble_att_svr_prep_entry_mem、
 *     r_ip_funcs_ro、r_modules_funcs_ro …）—— **BLE 即使永不 init，协议栈照样占 .bss**：
 *     整合版静态 94,380B vs 本独立版 86,764B = **整合版多背 7,616B**（这才是"仅整合版复现"
 *     的硬缺口：V2.1.1 解决的是"运行期碎片"，这一块是"运行期也在"）。
 *   ★后果：播放期（TLS+opus 吃完后）堆余量整合版仅 **7.7KB**、独立版约 **15KB**，正好腰斩；
 *     7.7KB 已贴到 WiFi 驱动/lwIP 动态取内存的地板（网卡收发缓冲要从堆里拿）。
 *   ★修法（两版同步）：`UP_SIZE` 16384 → **8192**，省 8KB **静态** RAM，直接抬升堆起点。
 *     依据：实测上行积压 = 泵窗口 30ms + 编码 40ms ≈ 70ms，16384 给的是 680ms（10 倍余量），
 *     日志 `upBuf 丢最老 0 B` 全场恒 0 ⇒ 从未用满十分之一；8192(≈340ms) 仍留 5 倍余量。
 *   ★新增"链路三件套"（停供行 + 2s 行各一条），把"服务器不发"与"我们收不到"当场分开：
 *     ①主循环 N 圈/s；②DMA 可用堆/最大连续块（= 网卡缓冲池）；③距上次 pong ms + ping/pong 计数。
 *     判读口诀见 README「V2.1.4」节。
 * V2.1.3（2026-10-01）：**归还 16KB 连续堆 + 把"谁先动手"问到底**（V2.1.2 诊断的判决与收口）。
 *   ★V2.1.2 诊断的实测读数（整合版 V2.1.2，官方服务器，同一句"讲一个2分钟长的小故事"）：
 *     · 极简启动**生效**：开机最大连续块 127KB（BLE 碎片假设**被证实修复**，但故障照旧
 *       ⇒ 它**不是**本次"没声"的原因，此前的推断就此作废）；
 *     · 入站最大帧 **252B**、>1KB 的 **0 个** ⇒ 库里 `malloc(payloadLen+1)` 失败 →
 *       `clientDisconnect(1011)` 的地雷**本次未触发**（我上一轮担心的那条排除）；
 *     · 播放期 **堆 7768 / 最大连续块仅 2292 B**；停供 16.9s 期间**堆还在缓慢下降**
 *       （7768→6500→5232→3964，约每 4s 掉 1268B），最大连续块末段掉到 1012；
 *     · 期间 **WS 连接全程"连接"**、无 `[WS] 连接断开`，18.4s 后才断开 ⇒
 *       **服务器真的没发帧**（这一条 V2.0.9 定责机制给得干净利落）。
 *   ★由此确定两件正事（本版全部落地）：
 *     ①**连续堆太瘦是长期隐患**：播放期只剩 2292B 连续块——库对任何大于它的帧都会
 *       当场断连、TLS/lwIP 也没了周转余地。而栈高水位实测给了我们一块现成的肥肉：
 *       `getArduinoLoopTaskStackSize()` 要了 **48000B**，整局跑完（TLS 握手 + SILK 编码 +
 *       流式播放）高水位 27352B ⇒ **峰值只用了 20648B**。降到 32768B（留 12KB / +58%
 *       余量）即**归还 16000B 连续堆**，且它在 setup 之前归还、必然连续。
 *     ②**"为什么断"必须问到字面上**：库的 `clientDisconnect(client, const char* reason)`
 *       会把 reason **原样塞进 `WStype_DISCONNECTED` 事件的 payload** —— 我们此前只打了
 *       "连接断开"，把唯一能区分"谁先动手"的证据扔了。现在打印它：
 *         有字符串 = 库自己判的（Connection lost / TCP connection cleanup / HTTP xxx）；
 *         空 = 服务端 close 帧（或库内部 code 路径）。
 *       再看**服务器 pong 回没回**：`WStype_PING/PONG` 是会回调到应用的，我们全程没统计。
 *       服务器每 10s 回一次 pong ⇒ "进程活着、只是不产音频（工具/LLM 卡）"；
 *       连 pong 都没有 ⇒ 服务器/链路真的死了。这两句能把 18.4s 静默**当场归类**。
 *     ③顺带掐掉库最后一条"自杀"路径：`handleHBPing()` 里 `sendPing()` **写失败即断连**
 *       （`WebSockets::clientDisconnect(1000)`，reason=NULL，和"服务端 close 帧"**无法区分**）。
 *       改为 `disableHeartbeat()` + 自建 10s ping：**失败只记账不断连**，真死链路交给
 *       `wsLastRxAt` 死链检测与 `clientIsConnected()` 兜底。至此 NULL-reason 只剩"服务端
 *       close 帧"一个来源，判别彻底干净。
 *    ④修 `[WS] 断开时` 的误导口径：`wsRxN` 是**每 2s 复位的窗口计数**，断开时打出来必然是 0
 *       （本例"本会话入站 0 帧，最大 252 B"自相矛盾即此），新增 `wsRxNTotal` 会话累计。
 *   期望读数（烧录后对照）：`[MEM]` 行 loop 栈 32768B；播放期最大连续块应从 2292B 显著抬升。
 * V2.1.0（2026-10-01）：心跳「只保活不断连」+ 服务器恢复窗口拉长（V2.0.9 定责后的对症下药）。
 *   ★现象（用户实测·官方服务器 api.tenclass.net）：长故事播到约 23.5s（第 6 句）时服务器零帧，
 *     打点连续 4 行 `⚠️ 服务器停供 2454/4455/6456/8457ms（WS 连接 | WiFi 3 RSSI -60 |
 *     堆 6580 稳定）` —— 链路与内存**全健康**，就是服务器不发帧；9.46s 后 `[WS] 服务器中途
 *     断开：响应未完成`。V2.0.9 的定责机制**成功了**（一眼分开"服务器停发"与"固件收场"）。
 *   ★但它同时暴露两个新问题：
 *     ①**断开很可能是我们自己掐的**：心跳 ping(15s)/pong 超时(5s)、2 次即断 ⇒ 停供后约 10s
 *       自断；实测末帧到断开 9.46s —— 时间高度吻合。服务器也许只是"慢/卡"，我们却把连接
 *       掐了，后面本可恢复的内容全丢（V2.0.8 无心跳时反而没这条自断路径）。
 *     ②服务器若只是慢，15s 静默上限也偏紧。
 *   ★修法：①心跳改 `enableHeartbeat(15000, 5000, 0)` —— ping 照发（NAT 保活），
 *           **disconnectTimeoutCount=0 = 永不因 pong 超时断连**（库语义，见 WebSockets.cpp
 *           handleHBTimeout 的 `if(client->disconnectTimeoutCount && ...)`）；真死链路仍由
 *           `sendPing()` 写失败自动断（handleHBPing 里 sendPing 失败即 clientDisconnect）。
 *         ②`DL_STALL_MAX_MS` 15s → **30s**：连接仍在时给服务器足够恢复窗口，恢复即可续播。
 *         ③新增 `wsLastRxAt` 死链检测：连接变"粘"了，故在 `ensureConnected()` 里加
 *           「自以为连着但 >45s 无任何服务器消息 ⇒ 判定僵死、强制重连」，防按 C 说话没反应。
 *         ④断连提示上屏（"为什么没声了"的唯一可见答案）：★播放期点阵**发不出去**——
 *           mtxFlush() 的仲裁铁律是「recPhase/dlActive 期间总线忙 ⇒ 显示帧绝不发送」
 *           （防显示帧插进音频流吃掉 ACK/录音字节），所以"静默中提示用户"在本硬件上做不到
 *           （曾计划静默 >12s 切等待图标，实测只会被缓存成 mtxWant、等收场才补发，已放弃）。
 *           但 **endDlPlay 那一刻 dlActive 刚置 false、总线刚空出来** ⇒ 在那里判 `dlBroken`
 *           （服务器响应未完成就断连）打一张**失望脸** `MTX_P_FROWN`，替代默认灭屏，
 *           让用户看见"这句话被打断了"，而不是静悄悄没声、怀疑设备坏。
 * V2.0.9（2026-10-01）：下行**停供定责 + 兜底分级**（修「讲着讲着没声了」）。
 *   ★现象（合订版实测日志）：长故事讲到第三句，服务器突然零帧，4.7s 后连接断开，
 *     固件按旧规则（静默 >5000ms）判"流结束兜底"收场 ⇒ 用户听感"讲着讲着没声了"。
 *   ★日志判读：播放 11.16s 只发出 167084B（=6.96s@24000）⇒ 供给率仅 62% 实时，
 *     全程欠供；句间断粮 873/721ms 尚能被缓冲吸收；尾部 4.7s 零帧 + TCP 断开
 *     = **服务端停了**（本机泵一直健康：2s 打点在场、解码错 0、溢出 0、看门狗未触发）。
 *     ★音频链路两版**逐字节相同**（已 diff）⇒ 不是合订同步引入的回归。
 *   ★修法：①兜底分级——链路已断（且协控播空）⇒ 立刻收场，不再白等；
 *            连接仍在 ⇒ 静默上限放宽到 15s（长句 LLM 间隙实测可到 5s+，旧版 5s 判死
 *            会把整段故事丢掉）；
 *          ②停供打点升级：每 2s 一行带 **WiFi 状态 / RSSI / WS 连接态 / 堆水位**，
 *            静默 >2s 标成「⚠️ 服务器停供」，当场分开"服务器不发"与"链路/内存出问题"；
 *          ③新增 `dlRespOpen`：响应未收尾就断连时打 `[WS] ⚠️ 服务器中途断开：响应未完成`
 *            ——"讲着讲着没声"这类报告看这一行即可定责。
 * V2.0.8（2026-10-01）：门户动作加**一次性令牌**，刷新页面不再误重启。
 *   ★现象（用户实测）：点完「重启并联网核对激活状态」后地址栏停在 `/recheck`，一按刷新
 *     （F5 / 回退再提交 / 重开标签页）浏览器就**重发同一个 GET 请求** ⇒ 设备被又写一次
 *     actboot、又重启一次。GET 本该幂等，"重启设备"这种副作用动作裸挂在 GET 上必然出事。
 *   修法：①每次渲染页面生成随机令牌塞进所有表单（`<input type=hidden name=t>`）；
 *         ②每个会重启的 handler 开头校验令牌，**消费即作废**；旧令牌/无令牌 = 直接拒绝并提示
 *           「这个操作已经执行过了，设备不会重复重启」，同时串口打 `[CFG] 拒绝重放的旧请求`；
 *         ③页面底部明确写出这条规则，并在 URL 栏直接敲 /recheck（无令牌）时同样被拒。
 *   覆盖 /activate、/recheck、/savewifi、/uselocal、/reboot 五个入口。
 * V2.0.7（2026-10-01）：去掉官方模式下的「刷新状态」按钮（用户实测"点了没用"）。
 *   ★为什么没用：门户模式（AP+STA + WebServer）跑 TLS 会崩，所以那个按钮压根查不了服务器，
 *     只能重刷页面——而"已激活"是设备本地缓存，刷新当然纹丝不动 ⇒ 用户看到的就是"没反应"。
 *   修法：官方模式激活区只留「重启并联网核对激活状态」（/recheck）——唯一能真正同步服务器
 *   绑定状态的入口；/activate 在官方模式下不再渲染，handler 里保留兜底（防手工 URL /
 *   浏览器缓存旧页面把设备踢去重启）。
 * V2.0.6（2026-10-01）：门户页「点当前模式按钮不再重启」。
 *   ★现象：当前已是本地模式时点「保存并切换到本地服务器」、已是官方模式时点「切换/核对」
 *     按钮，设备都会无脑重启等 15 秒——可实际上模式压根没变，纯属折磨。
 *   修法：①两个切换入口先判「目标模式 == 当前模式」且参数一字未改 ⇒ **只刷新页面状态**，
 *           不写 NVS、不重启（反复调试门户时最常点的就是这个按钮）；
 *         ②官方模式的"实查"独立成按钮「重启并联网核对激活状态」(/recheck)，与切换解耦；
 *         ③按钮文案随当前模式动态生成，点下去会发生什么一眼可见。
 * V2.0.5（2026-10-01）：激活判定改为「服务器说了算」。
 *   ★现象：官网已解绑设备，网页仍显示"已激活"；长按 C 说话时服务器用英语播报激活码。
 *   ★根因：①「已激活」只是 NVS 本地缓存（`cfg/actcode`），解绑只有服务器知道；
 *          ②门户页「获取激活码/完成激活」有官方快照时走**快照快捷路径**（把旧 token 写进
 *            活动槽 + 直接把 gActCode 置成"(已激活)"），**全程没问服务器** ⇒ 状态永远陈旧。
 *   修法：①按钮改为「联网核对激活状态」，**一律**写 actboot + 重启 → 开机在纯 STA 环境跑
 *           officialOta() 实查（已绑定→返回 token；已解绑→返回 activation.code）；
 *         ②新增在线检测：会话中服务器 TTS 播报激活提示（含 activation/激活码 关键词）即
 *           作废本地"(已激活)"缓存、提取 6 位激活码写入 NVS（中英文 + "1 2 3 4 5 6" 形态）；
 *         ③门户页明示"已激活（本地缓存）"并提示解绑后不会自动更新；
 *         ④串口新增 `a` = 联网核对激活状态。
 * V2.0.4（2026-10-01）：全新设备出厂流程。
 *   ★根因：ESP32 烧录**默认不擦 NVS** —— 同一块板子刷过多版固件（或产线刷过前一台设备的
 *   全片镜像）后，NVS 里残留 xiaozhi 活动槽（官方 host + ssl + token）与 cfg/actcode
 *   ="(已激活)"，loadServer 直接继承 ⇒ **新设备刷上就"显示已激活，实际没激活"**。
 *   修法：WiFi 凭据为空（源码常量留空 + NVS 也没存过）= 从未配网 ⇒ 判定全新设备，
 *   开机清空服务器/激活全部残留、跳过 20s STA 等待、直接进 AP 配网门户，走全新激活流程。
 *   另：LOCAL_TOKEN 由占位串"在这里填访问令牌"改为空串（占位串恒非空会污染"未配置"判据）；
 *   新增串口命令 `F` = 恢复出厂（清 WiFi+服务器+激活 NVS 后重启）。
 * V2.0.3（2026-10-01）：修复"网页点切服务器无效、一直是本地模式"。
 *   ★根因：开机 OTA 的触发条件写的是 `curToken.length() == 0`，而 loadServer 找不到活动槽
 *   时会回退烧录常量 LOCAL_TOKEN（占位串"在这里填访问令牌"，恒非空）⇒ 条件永远为假
 *   ⇒ officialOta() 从未执行过（未激活开门户的兜底同款失效）。
 *   现改为显式意图：actboot 置位 + WiFi 通 → 无条件跑 OTA；OTA 未完成 → 自动开门户看激活码。
 * V2.0.2（2026-10-01）：v9.24 点阵版同步翻页器整合固件的全部小智侧修复。
 *   · 按键：C=按住说话 / B=短按打断播放 / A 长按 2s=AP 配网门户开/关。
 *   · 服务器切换 = 写 NVS + 重启（门户模式下运行时 TLS 切换必崩；重启后纯 STA 直读）。
 *   · 官方配置快照（NVS o* 键）：官方↔本地来回切免重走 OTA。
 *   · 点阵：说话麦克风 → 录音结束灭屏 → 播放笑脸 → 播完灭屏（无思考菱形）。
 *   · 静音回填默认关闭（DL_FILL_ENABLE=0，短回复尾音不再被拖长）。
 *   播放路径不变：`90 10 01 00` 启动 + `90 15 <len> <data≤200> <cksum>` 数据帧 + ACK credit 流控。
 *
 * 【已判死结论（勿再试，实验代码已全部移除）】
 *   · 0x16 通道（EXP29/31/32 三轮）：ACK 间隔恒 ≈5.3ms 与帧长无关 = 协控 ~5ms 定时器轮询
 *     ⇒ 通道物理容量 ≈12KB/s 封顶（只够 8kHz 提示音），24kHz TTS 需 24KB/s，判死。
 *   · 0x14 04 不是"进播放模式"钥匙（EXP30：只换来前段静默）；数据帧必须带 len 字节
 *     （EXP30/31：删 len = 协控整帧拒收）；0x10 是会话级指令，绝不能进流中重复（EXP28）。
 *   · lap 掩蔽 / 环保护复位 / 寿命定律 / 记账单位（3:1）等全部证伪。
 *   · 吞字/重播根因 = 协控 64K 录音环（65536 样本 = 2.73067s）写指针绕圈后读指针错位
 *     150~170ms。ESP32 侧无解，根治 = 厂商升级 STEM 协控固件。
 *
 * 链路：上行 按住 C 键(GPIO3) → 协控录音(单发在途泵, 8bit/24000) → 定点重采样→16000
 *       → opus 编码(24kbps AUDIO) → WS 裸 opus 帧（16k/60ms，960 样本/包）
 *       下行 WS opus → 按 hello 采样率解码(默认24000) → 定点重采样→8bit/24000
 *       → FIFO → 非阻塞 0x15 流控泵喂协控。半双工：录音中丢下行帧；按 C/B 打断 TTS。
 *       按需连接（官方 S2 同款）：按住说话现连，空闲 60s 主动断开。
 *
 * 协控音频协议（勿改节奏）：
 *   - UART1 460929 TX=8 RX=9，帧 [0x90,addr,len,data...,ck]，ck=前面字节求和
 *   - 播放：90 10 01 00 启动 → 90 15 n data ck 数据帧（status=实收，未收满重发）
 *   - 录音：90 10 01 01 启动 → 裸 3 字节轮询 [0x80,0x11,n]（无校验和！）
 *           响应 [0x81,0x11,len]+len 字节；单发在途（头20ms超时作废/体100ms超时丢弃）
 *   - 停止：90 10 01 00（阻塞等 ACK）。录音期间绝对零打印（打印会回灌 UART1 RX）
 *
 * 烧录：ESP32C3 Dev Module，Upload Speed 115200，Partition "Huge APP (3MB No OTA)"。
 * 库：WebSockets（Links2004）、arduino-libopus（pschatzmann）。
 *
 * 串口命令（115200）：o=切官方(快照优先)  l=切回本地  i=服务器信息
 *   h=重发 hello  1/0=手动 listen start/stop  w=配置网页(开/关)
 *   a=联网核对激活状态（写 actboot + 重启后 OTA 实查）
 *   s=紧急停止  r=重连 WebSocket  F=恢复出厂（清 NVS + 重启）。
 *   v=详细诊断开关（每 2s 的 [DL] 链路行，默认关）。
 *   A 键长按 2s = AP 配网门户开/关（物理救援入口）。
 *   （诊断工具 p=0x80 指针扫描默认不编译，需将 PROBE_ENABLE 置 1 —— 见下方探针块注释）
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_heap_caps.h>   // ★V2.1.4：DMA 堆诊断（heap_caps_get_free_size / largest_free_block）
#include <esp_sleep.h>      // ★V2.3.2：深度休眠（GPIO 唤醒，参照 PageTurner V1.0.5）
#include <opus.h>

// ================== 固件版本（唯一出处）==================
// ★升版本只改这一行：AP 配置页标题 + 串口启动横幅都由它渲染，避免漏改。
#define XZ_FW_VERSION "V2.4.13"
// 实验版识别标记（正式版留空字符串 ""）：同一处渲染 —— 烧进去后一眼能认版本。
#define XZ_FW_TAG ""

// loopTask 栈：opus SILK 编码路径峰值 15-25KB，默认 8KB 必爆。
// ★V2.1.3：48000 → 32768。依据是**实测**而非估算 —— V2.1.2 加的栈高水位打点显示，
//   整局跑完（TLS 握手 + 录音 SILK 编码 + 流式播放）高水位 27352B，
//   即峰值只用了 48000-27352 = 20648B。留 12KB（+58%）余量足够；
//   而省下的 16KB 会在 setup 之前归还堆里、且必然连续 —— 播放期最大连续块实测只有
//   2292B（TLS/_data/编解码器把堆切碎），这是眼下唯一能同时"省得多"又"省得连续"的一块。
//   ⚠️ 若出现异常重启（FreeRTOS 栈溢出会 panic 重启，不会静默损坏），退回 40960 再试。
size_t getArduinoLoopTaskStackSize() {
  return 32768;
}

// ================== 本地服务器配置（烧录前改这里） ==================
// ★全部留空 = 出厂固件形态：WiFi 凭据为空即判定"全新设备"，开机清空
//   NVS 里的服务器/激活残留并直接进配网门户（详见 setup 的 FACTORY 段）。
//   WiFi 走门户配置（存 NVS cfg/ssid+pass），无需在源码里写死。
static const char* WIFI_SSID = "";
static const char* WIFI_PASS = "";
static const char* LOCAL_HOST = "";
static const uint16_t LOCAL_PORT = 8000;
static const char* LOCAL_PATH = "/xiaozhi/v1/";
// ★空串而非占位串：占位串恒非空，会让 `curToken.length()==0`（未配置判据）永远为假
static const char* LOCAL_TOKEN = "";
// ==================================================================

static const char* OTA_URL = "https://api.tenclass.net/xiaozhi/ota/";

// ---- 小智协议音频参数（上行 16kHz 单声道 60ms 帧）----
// 16k/60ms 与 S2 官方固件及服务器预期(960 样本/包)对齐；hello 声明 16k/60ms，
// 下行走服务器原生 24k 路径（服务器下行恒发 60ms 帧实测实锤）
static const int SAMPLE_RATE = 16000;
static const int FRAME_MS = 60;
static const int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;  // 960
static const int MAX_PACKET = 1276;

// ---- 按键 ----
static const int BTN_PIN = 3;  // C 键，低有效（板上外部下拉，按下=高电平）
// A 键 GPIO10：长按 5s 开配置热点（无串口/网页进不去时的物理救援入口）
static const int BTN_A_PIN = 10;
static const int BTN_B_PIN = 1;  // B 键：短按打断播放
static bool btnAHeld = false;
static bool btnALong = false;  // ★V2.2.2：本次按下已判为长按（松开时不再当单击）
static uint32_t btnAAt = 0;

WebSocketsClient ws;
Preferences prefs;
bool wsReady = false;
String gSession;  // 服务器 hello 下发的 session_id，listen 时带回

OpusEncoder* enc = nullptr;
OpusDecoder* dec = nullptr;
static int decRate = 24000;  // 当前解码器采样率（按服务器 hello 动态重建）

// 当前生效的服务器
String curHost, curPath, curToken;
uint16_t curPort = 0;
bool useSSL = false;

static String g_wsHeaders;  // setExtraHeaders 只存指针不拷贝，必须全局保活

// ---- Web 配置门户 ----
// WiFi 凭据/激活码存独立 namespace "cfg"——clearServer() 只清 "xiaozhi"，互不干扰
static String wifiSsid, wifiPass;   // NVS 优先，回退烧录常量
static String gActCode = "";        // 最近一次官方 OTA 下发的激活码（NVS 缓存）
static bool cfgMode = false;        // 配置门户开关（串口 w / WiFi 连不上自动进）
static WebServer* cfgWeb = nullptr;
static uint32_t cfgRebootAt = 0;    // 保存 WiFi 后延时重启
// 空闲主动断开阈值：60s（服务器实测 tts stop 后 ~10s 即回收、"未完成会话"80s 超时；
// 60s 主动断是服务器不踢时的兜底，防连接泄漏）
static const uint32_t IDLE_DISCONNECT_MS = 60000;
static uint32_t lastConvAt = 0;  // 最近会话活动时刻：空闲 60s 主动断开（按需连接架构）

// ---- ★V2.3.2 深度休眠（参照 PageTurner V1.0.5 的 1.0.5 实现）----
// C3 深睡只有 RTC GPIO(0~5) 能当唤醒源：C=GPIO3 ✓（A=GPIO10 不行，B=GPIO1 可但用户只要 C）。
// 深睡唤醒 = 整机复位重启（setup 重跑、WiFi 重连），所以醒来后靠 wakeByCKey 标志，
// 在 setup 末尾若 C 仍按住就直接 startTalk() —— 与待机"按住 C 说话"完全一致。
#define SLEEP_IDLE_MS 60000UL    // ★用户指定：60 秒无动作即入深睡（改这里；0 = 不休眠）
static bool wakeByCKey = false;      // 本次启动由 C 键从深睡唤醒
static uint32_t lastVolActAt = 0;    // 音量键也算"交互"，避免调着音量突然睡

// 上行状态机：0=空闲 1=录音中（停止为阻塞式，无需中间态）
static uint8_t recPhase = 0;

static String deviceId() {
  String s = WiFi.macAddress();
  s.toLowerCase();  // 官方固件用小写 MAC，服务器按此字符串注册
  return s;
}

static String clientId() {
  // 官方 Client-Id 是 UUID，这里用 MAC 拼确定性伪 UUID
  String m = deviceId();
  m.replace(":", "");
  return m.substring(0, 8) + "-" + m.substring(8) + "-a1b2-c3d4e5f60718";
}

static void saveServer(const String& host, uint16_t port, const String& path,
                       const String& token, bool ssl) {
  prefs.begin("xiaozhi", false);
  prefs.putString("host", host);
  prefs.putUShort("port", port);
  prefs.putString("path", path);
  prefs.putString("token", token);
  prefs.putBool("ssl", ssl);
  prefs.end();
}

static void clearServer() {
  prefs.begin("xiaozhi", false);
  prefs.clear();
  prefs.end();
}

// ---- 官方服务器快照（cfg namespace，clearServer 不清）----
// 切本地时快照官方配置，切回官方直接恢复，不必重走 OTA。
// 为什么必须快照：OTA 是 HTTPS（TLS 握手 ~45KB 堆），配置门户开启时堆仅 ~65KB，
// 实测握手失败 HTTP -1 —— 官方↔本地来回切换不能依赖 OTA 通道。
static void saveOfficialSnapshot() {
  prefs.begin("cfg", false);
  prefs.putString("ohost", curHost);
  prefs.putUShort("oport", curPort);
  prefs.putString("opath", curPath);
  prefs.putString("otok", curToken);
  prefs.putBool("ossl", useSSL);
  prefs.end();
}

static bool loadOfficialSnapshot() {
  prefs.begin("cfg", true);
  String h = prefs.getString("ohost", "");
  if (!h.length()) { prefs.end(); return false; }
  curHost = h;
  curPort = prefs.getUShort("oport", 443);
  curPath = prefs.getString("opath", "/xiaozhi/v1/");
  curToken = prefs.getString("otok", "");
  useSSL = prefs.getBool("ossl", true);
  prefs.end();
  return curToken.length() > 0;
}

static void loadServer() {
  prefs.begin("xiaozhi", true);
  String host = prefs.getString("host", "");
  if (host.length()) {
    curHost = host;
    curPort = prefs.getUShort("port", 8000);
    curPath = prefs.getString("path", "/xiaozhi/v1/");
    curToken = prefs.getString("token", "");
    useSSL = prefs.getBool("ssl", false);
    prefs.end();
    if (useSSL) saveOfficialSnapshot();  // 迁移：老设备的官方配置也补一份快照
    return;
  }
  prefs.end();
  // 无活动配置 → 本地服务器：网页存的 lhost 优先，回退烧录常量
  prefs.begin("cfg", true);
  String lh = prefs.getString("lhost", "");
  if (lh.length()) {
    curHost = lh;
    curPort = prefs.getUShort("lport", LOCAL_PORT);
    curPath = prefs.getString("lpath", LOCAL_PATH);
    curToken = prefs.getString("ltoken", LOCAL_TOKEN);
  } else {
    curHost = LOCAL_HOST;
    curPort = LOCAL_PORT;
    curPath = LOCAL_PATH;
    curToken = LOCAL_TOKEN;
  }
  useSSL = false;
  prefs.end();
}

static void printServer() {
  Serial.printf("[SRV] %s://%s:%u%s (%s)\n", useSSL ? "wss" : "ws",
                curHost.c_str(), curPort, curPath.c_str(),
                useSSL ? "虾哥官方" : "本地服务器");
  Serial.printf("[SRV] token: %s\n", curToken.length() ? "已设置" : "(空)");
}

// ---------- 官方 OTA 激活流程（轻量 JSON 提取） ----------
// 报文类型判定：兼容紧凑与带空格两种序列化（python json.dumps 默认 "type": "x"）
static bool hasMsgType(const String& s, const char* t) {
  String pat = String("\"type\":\"") + t + "\"";
  if (s.indexOf(pat) >= 0) return true;
  pat = String("\"type\": \"") + t + "\"";
  return s.indexOf(pat) >= 0;
}

static String jsonStr(const String& s, const char* key) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  while (i < (int)s.length() && s[i] == ' ') i++;  // 兼容 python json.dumps 带空格风格
  if (i >= (int)s.length() || s[i] != '\"') return "";
  i++;
  int j = s.indexOf("\"", i);
  if (j < 0) return "";
  return s.substring(i, j);
}

static int jsonInt(const String& s, const char* key) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return -1;
  i += pat.length();
  while (i < (int)s.length() && s[i] == ' ') i++;
  int j = i;
  while (j < (int)s.length() && isDigit(s[j])) j++;
  if (j == i) return -1;
  return s.substring(i, j).toInt();
}

// 提取扁平 JSON 段（如 "websocket":{...}），返回段内容（含花括号）
static String jsonSection(const String& s, const char* sec) {
  String pre = String("\"") + sec + "\":";
  int i = s.indexOf(pre + "{");
  if (i < 0) i = s.indexOf(pre + " {");  // python json.dumps 带空格风格
  if (i < 0) return "";
  int j = s.indexOf("}", i);
  if (j < 0) return "";
  return s.substring(i, j + 1);
}

// ========== 协控 UART 底层 ==========

static HardwareSerial coUart(1);

static const uint8_t CMD_CTL = 0x10;   // 播放启动/录音停止=0x00，录音启动=0x01
static const uint8_t CMD_PLAY = 0x15;  // 播放数据帧
static const uint8_t REC_POLL_H0 = 0x80;
static const uint8_t REC_POLL_H1 = 0x11;
static const uint32_t RESP_TIMEOUT = 100;
static const uint8_t MAX_RETRIES = 3;
// ★V2.2.0：200 → 160。协控 UART RX 一次能吃下 128~192B（One 板实测 short ack
// 120/124/190 for 200 bytes），200B 帧必短收 → 多一次往返 → 吞吐腰斩。
// 160B 落在它的整帧吸收区间内，一帧一次确认，RTT 只由 UART 传输决定。
static const size_t PLAY_CHUNK = 160;

// 阻塞发一帧并等 3 字节响应 [0x91, addr, status]（含官方重发）
// ★硬校验 resp[1]==frame[1]（addr 回显）：总线上有多类帧（音频 0x10/0x15、点阵 0x08）
//   后，慢到的旧响应不能被当成当前帧的 ACK——否则录音启动可能"幻影成功"实则没进录音态
// 返回 status 字节，-1=无响应
static int sendWait(const uint8_t* frame, uint8_t len) {
  static uint8_t resp[3];
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    coUart.write(frame, len);
    uint8_t got = 0;
    uint32_t t0 = millis();
    while (got < 3 && millis() - t0 < RESP_TIMEOUT) {
      while (coUart.available() && got < 3) {
        uint8_t b = coUart.read();
        if (got == 0 && b != 0x91) continue;  // 跳过回灌/杂散
        if (got == 1 && b != frame[1]) {      // addr 不符=旧响应，滑窗重同步
          got = (b == 0x91) ? 1 : 0;
          continue;
        }
        resp[got++] = b;
      }
    }
    if (got >= 3) return resp[2];
    uint8_t z = 0;
    coUart.write(&z, 1);  // 官方同步字节
    delay(150);
  }
  return -1;
}

// ★协控播放率 = 24000Hz（2026-09-30 EXP21 音阶回归标定实锤，上下行同源）。
//   我们按 24000 生成/计时、协控按 24000 消耗 → 缓冲恒定。dlRate 不得改回 22050
//   （会复现每 2.73s 环回绕重放 = "后面的字重播吞掉前面的字"）
static uint32_t dlRate = 24000;
static inline int32_t MS2B(uint32_t ms) { return (int32_t)((uint64_t)dlRate * ms / 1000); }
static const uint32_t CO_RATE = 24000;  // 协控录音/播放统一采样率

// 44 字节 WAV 头——逐字节复刻官方 S2 固件 alphapi_one_audio_codec.cc
// ConfigureOutputLocked()（金标准）。关键点：fmt 16（无 cbSize）、RIFF size 与
// data 长度均填 0xFFFFFFFF = 协控的"无限流"哨兵值；实测填有限长度会退化失败。
// 实测协控只认以 WAV 头开头的音频流（无头流静默丢弃），下行播放必须带头。
static uint8_t wavHdr[44];
static void buildWavHeader44() {
  const uint32_t rate = dlRate, ch = 1, bits = 8;
  uint32_t byteRate = rate * ch * bits / 8;
  uint16_t blockAlign = 1;
  size_t p = 0;
  auto put = [&](const void* src, size_t n) {
    memcpy(wavHdr + p, src, n);
    p += n;
  };
  auto put32 = [&](uint32_t v) {
    put(&v, 4);
  };
  auto put16 = [&](uint16_t v) {
    put(&v, 2);
  };
  put("RIFF", 4);
  put32(0xFFFFFFFF);
  put("WAVE", 4);
  put("fmt ", 4);
  put32(16);
  put16(1);
  put16((uint16_t)ch);
  put32(rate);
  put32(byteRate);
  put16(blockAlign);
  put16((uint16_t)bits);
  put("data", 4);
  put32(0xFFFFFFFF);
}

// ========== 下行链路：WS opus → FIFO → 非阻塞流控泵 → 协控 ==========

// ★V2.4.11：20KB(853ms) → 32KB(1365ms)。串口实测 RSSI -72→-79 时 socket 连续数秒
//   不来字节，853ms FIFO 一抽干就"饿透→重推头"；加倍后多吸收约 0.5s 网络抖动。
//   （实测连续堆 38.9KB，静态多 12KB 仍安全；32768 < 65535，uint16_t 够用）
static const uint16_t DL_SIZE = 32768;  // 32KB≈1365ms@24000
// 本地服务器 TTS 逐帧发送不均，FIFO 平滑供给；满时丢最老（延迟保护）
static uint8_t dlFifo[DL_SIZE];
// ★V2.2.1 关键修复：指针必须 32 位。
// 旧代码是 uint16_t，`dlFifo[dlHead++ % DL_SIZE]` 在 dlHead 从 65535 回到 0 时，
// 索引从 65535%20480=4095 直接跳到 0 —— 读/写位置瞬间错开 4095 字节，
// 于是每 65536 字节（65536/24000 = 2.7307 秒）必卡一下，和用户听到的
// "每 2.73 秒卡一下"完全对上。DL_SIZE=20480 不是 65536 的约数，所以回绕不连续。
static uint32_t dlHead = 0, dlTail = 0;
static uint32_t dlOver = 0;           // FIFO 溢出丢样计数
static uint32_t dlMbGate = 0;         // ★实验①：因"协控水位未降下来"而本圈跳过投喂的次数
                                      //   （批式节拍证据：>0 说明泵真的在等协控播掉）
static uint32_t dlStarvePrintAt = 0;  // 断粮周期打印节拍（每 2s 一条）

// ---- 生产策略（v8.2 低水位 + 饿透即重启 + v9.5.3 死亡指纹复活）----
//   ① 垫 880ms（吸收服务器 150-350ms 间隙）；
//   ② 饿透标记（FIFO 空 + 积压 ≤50ms）→ 数据恢复瞬间重推头开新会话；
//   ③ 会话硬顶 120s 只作保险丝（EXP21 单会话 30s 零异常，正常故事永不触发）；
//   ④ 播放冻结指纹（积压增速 >8000 B/s 连续 2 次）→ 0x10+推头直启复活。
static const uint32_t DL_SEG_HARD_MS = 120000;  // 会话硬顶兜底（保险丝）

// ================== ★实验①：官方微批"浅水位"复刻（2026-10-10）==================
// 命题（依据官方 controlBoard.mpy 反汇编 `官方音频机制_反汇编还原.md`）：
//   官方播放 = 200B/批 + 在途 ≤1 批 + **协控缓冲几乎不积压**（浅水位）。
// 我方现状核查：帧长 PLAY_CHUNK 已是 200B、在途已锁 1 帧（dlPendCnt>=1 即等 ACK）
//   ⇒ 这两项**本来就与官方同构**，不需要改。真正"重"的只剩**水位**：
//   起播预滚 880ms（≈21KB 一次性塞进协控）+ 积压钳制 4600ms + 回填目标 1500ms。
// 本开关把水位从"千毫秒级"压到官方批大小量级（200ms），复刻度 = **水位维度**。
// 变量：只动 DL_JITGATE_MS 与 DL_AHEAD_MAX_MS 两项（帧长/在途/流控/饿透复位策略不动）。
// 回退：DL_EXP1_WATER 改 0 即回 V2.1.5 原行为（无需恢复备份）。
// 观察判据：①段界复位次数 dlSegResets ②溢出丢样 dlOver ③断粮频率 ④听感吞字有无变化
//   —— 若四者与 V2.1.5 完全一致 ⇒ "水位/批大小"假说被否，转手段④（0x80 扫描协控指针）。
// ★EXP1 实测判决（2026-10-10，用户放歌）：**仍重播 ⇒ 假说否**。且这是物理必然——
//   写指针在环内的"位置"只由累计写入量决定，与在途深浅无关（详见 EXP2 说明）。
//   保留 200ms 水面作为 EXP2 的基线配置（分段才是 EXP2 的唯一自变量）。
// ★EXP4 起关闭（2026-10-10 晚，用户"直接试 4"）：EXP1 浅水位已证伪、EXP2/EXP3 未验证但
//   会**每 2.4s 主动复位一次** ⇒ 协控播放指针被反复推头重置 = 手段④的时序观测被污染。
//   试手段④必须回到**连续播放**基线（V2.1.5：只有 120s 保险丝），才能看到寄存器单调性。
//   两个开关同时置 0 = 纯 V2.1.5 播放行为 + 探针，变量最干净。
#define DL_EXP1_WATER 0
// ================== ★V2.1.6：应答锁速（ACK-LOCK）==================
// 【为什么加这一版】2026-11-04 在 AlphaPi One（**同款协控**，ESP-IDF 移植版）上把
//   "吞字/卡顿"彻底修好了，做法是把播放内核换成三条铁律（2680~2710 实测通过，
//   用户 60 秒长故事验收"没听出吞字"）：
//     ① **只按协控的 ACK 走**：发 200B → 等 `91 15 <实收字节>` → 收到才发下一块，
//        status 说收了多少就前进多少、剩下补发（=本固件已有的 1 帧在途语义）；
//     ② **喂进协控的在途积压压到极浅**（几十毫秒量级），绝不按墙钟建模、绝不重锚；
//     ③ **中途绝不重推 44 字节 WAV 头**（= 绝不在语音里插会话复位）。
// 【与本固件 V2.1.5 的差异（就是要改的三处）】
//     ① 投喂水位上限 4600ms ⇒ 压到 120ms。协控环 65536B=2.73s，积压在途越多，
//        写指针绕圈时"跨过的未播数据"越多；AlphaPi One 上同样协控、积压≈0，
//        连续 60s 零异常 ⇒ 先把这个变量压到同一量级再谈别的。
//     ② 120s 硬顶兜底会在故事中途做一次"饿透复位 + 重推头"⇒ 本开关下关闭
//        （连续流不该有段界；README 自己的 A/B "开复位 6 次 ⇒ 命中 6 处重播"
//        已经说明复位缝本身就是重播源）。
//     ③ 协控连续 6 次 ACK 超时（≈600ms 完全不吃数据）不再直接判死收场，改为
//        **重开会话**（0x10 + 推头），最多 3 次 —— 长故事不会因为一次僵死被截断。
// 【起播门槛为什么是 300ms】本机服务器句间停顿实测 150~350ms，门槛必须 ≥ 停顿
//   才不至于每句都饿透（饿透就要重推头 = 一次缝）。300ms 同时把起播延迟控制在
//   0.3s 量级；稳态上限 120ms 由 ACK 决定，二者不冲突（门槛只在起播/复供时用）。
// 【回退】PLAY_ACK_LOCK 改 0 ⇒ 完全回到 V2.1.5 行为（880/4600 + 120s 硬顶 + 30 次判死）。
#define PLAY_ACK_LOCK 1
#if PLAY_ACK_LOCK
static const uint32_t DL_JITGATE_MS = 300;    // 起播/复供门槛（吸收句间 150~350ms 停顿）
// ★V2.1.7：积压上限 120ms 实测过浅（见 V2.1.6 日志：全程欠供、断粮 4 次、
// 152790B 播了 8918ms = 17132B/s，比实时 24000 慢 29% ⇒ 听感拖长+卡顿）。
// 但 4600ms 又超过协控环 2730ms ⇒ 绕圈覆盖未播数据（README 的重播假说）。
// 取 700ms：环长的 1/4 以内，又足够吸收帧级抖动。真正的吞吐问题由
// DL_INFLIGHT_MAX 修（单帧在途 = 吞吐被 ACK 往返钳制在 frame/RTT）。
static const uint32_t DL_AHEAD_MAX_MS = 700;
#elif DL_EXP1_WATER
static const uint32_t DL_JITGATE_MS = 200;    // 原 880：起播预滚 21KB → 4.8KB
static const uint32_t DL_AHEAD_MAX_MS = 200;  // 原 4600：投喂水位上限（协控在途积压封顶）
#else
static const uint32_t DL_JITGATE_MS = 880;    // 起播门槛 ms（起播延迟 +0.2s~0.9s）
static const uint32_t DL_AHEAD_MAX_MS = 4600;
#endif
// ================== ★实验②：主动定量分段（复刻官方 2.5s 任务边界，2026-10-10）==================
// 【EXP1 实测结案：放歌仍重播 ⇒ "水位/批大小"不是根因】
//   物理原因：写指针在环内的**位置**只由"距上次推头的累计写入量"决定，与在途深浅（水位）无关
//   ⇒ 浅水位改不了绕圈这件事。环长 65536 样本 = 2.73067s @24k，写指针每绕一圈必重播一次。
// 【真正的缺口：现有复位触发器对"连续流"零覆盖】
//   ①饿透标记/恢复推头：需 FIFO 空 + 积压 ≤50ms  ⇒ 连续流从不断供 ⇒ 永不触发；
//   ②死亡指纹复活：需积压增速 >8000 B/s（播放冻结）⇒ 正常播放不触发；
//   ③会话硬顶：DL_SEG_HARD_MS = **120s** 才兜底 ⇒ 这 120s 里写指针已绕 44 圈！
//   ⇒ 连续音乐 = 从头到尾没有一次分段 ⇒ 每 2.73s 重播一次。这就是"放歌还是重播"的完整解释。
// 【官方为什么没事】官方 2.5s/段（< 2.73067s）**每段必然结束任务**，开局重发启动
//   ⇒ 写指针永远走不到 65536 样本 ⇒ 从不绕圈。分段=规避绕圈的手段，绕圈=坏在固件里的 bug。
// 【本实验】把"段界"从 120s 硬顶改成 **定量主动分段**：距上次推头的字节数达到
//   DL_EXP2_SEG_MS（默认 2400ms，环长 2730.67ms 的 88%，留 330ms 余量）即主动进入既有的
//   "停发 → 饿透 → 推头 → 复供"状态机。停发期协控还在播自己缓冲里的货（不出声=不损失），
//   真正的静音只有"播空到新头到货"那一小段 = 由 DL_EXP2_DRAIN_MS 余量决定（原硬编码 100ms）。
// 变量：只动"段界触发条件"+"饿透判据余量"两项；帧长/在途/流控/水位（EXP1）一律不动。
// 回退：DL_EXP2_SEG 改 0 ⇒ 回到"只有 120s 硬顶"的 V2.1.5 原行为。
// 【EXP2b/EXP3 修正（2026-10-10 晚，用户实测 EXP2 仍重播）】
//   ① 门：去掉 !dlFeedDone/!ttsDone —— 点歌时服务器会在播放中发 tts stop，旧门把分段锁死。
//   ② ★同源铁证：2026-09-30 A/B 实测「开复位 6 次 ⇒ 命中 6 处（每次复位必 1 处重播）」，
//      而关复位绕 7.4 圈只命中 3 处 ⇒ **复位缝本身就是重播源**。EXP2 每 2.4s 切一次
//      = 每 2.4s 制造一次重播 ⇒ 比绕圈（2.73s/40%）更密。
//   ③ EXP3 对策 = 段末静音垫：既然重放的总是"缝前区域"，就把缝前 200ms 换成数字静音
//      ⇒ 重放的是静音 ⇒ 重播变"顿一下"。DL_EXP2_PAD_MS 改 0 即回无垫对照。
#define DL_EXP2_SEG 0
#if DL_EXP2_SEG
static const uint32_t DL_EXP2_SEG_MS = 2400;    // 段长（必须 < 2730.67ms = 环长）
static const int32_t DL_EXP2_DRAIN_MS = 20;     // 饿透判据余量（原 100ms：硬等积压 -100ms）
// ★EXP3（2026-10-10 晚）：段末静音垫——实测每个"复位缝"= 1 处重播（2026-09-30 A/B：6 复位 6 命中），
//   机制是协控重生时"重放缝前区域"。若把缝前那 150~170ms 换成数字静音，重放的就是静音 ⇒ 重播消失
//   （代价：换成一次等长静音，听感"顿一下"而非"重复一句"）。
//   实现：先按 DL_EXP2_AUDIO_MS 走音频，再推 DL_EXP2_PAD_MS 静音并把 FIFO 抽干，最后才切段。
//   DL_EXP2_PAD_MS 改 0 = 回到 EXP2（无垫直切，用于对照）。
static const uint32_t DL_EXP2_AUDIO_MS = 2200;  // 音频段长（垫 200ms 后合计 ≈2400ms）
static const int32_t DL_EXP2_PAD_MS = 200;      // 段末静音垫长度（0=关）
// ★EXP3a 天然静音优先：段内已连续 ≥180ms 静音（帧内 max|v−128| 低）就直接切——这段静音
//   本身就是"缝前区域"，重放的是它 ⇒ **零代价**（不用多切 200ms 垫）。垫静音只是没天然静音时的替身。
static const uint32_t DL_EXP2_QUIET_MIN_MS = 1500;   // 过早不切（防切碎）
static const uint32_t DL_EXP2_QUIET_RUN_MS = 180;    // "缝前静音"长度（≥150~170ms 的重放窗口）
static uint32_t dlExp2Segs = 0;                 // 实验②：主动分段触发次数
static uint32_t dlExp2StopSum = 0;              // 实验②：累计停发时长（算平均）
static uint32_t dlExp2StopMax = 0;              // 实验②：单次最长停发
static bool dlPadDone = false;                  // ★EXP3：本段静音垫是否已入队
static uint32_t dlPadAtMs = 0;                  // ★EXP3：静音垫入队时刻（抽干超时兜底）
static uint32_t dlExp2Quiet = 0;                // ★EXP3a：缝落天然静音的段数（零代价的那部分）
// ★EXP2b 绕圈探针（判"分段到底有没有生效"的硬指标）
static uint32_t dlSegMaxMs = 0;                 // 本场最长段长（>2730 ⇒ 绕圈已发生）
static uint32_t dlWrapCross = 0;                // 本场越过环长仍未分段的次数
static bool dlWrapAlarm = false;                // 告警自武装（段内推量回落 <1s 即重新武装）
#else
static const int32_t DL_EXP2_DRAIN_MS = 100;    // 原硬编码值
#endif
// ★V2.0.9 兜底分级：旧版一把尺子（静默 >5000ms 就 endDlPlay"流结束兜底"）在两种
//   场景下都错——① 服务器真挂了：WS 断开事件几秒内必到，多等纯属浪费；
//   ② 服务器只是长句 LLM 间隙（本地服务器实测可到 5s+）：判死 = 整段故事被丢，
//   用户听感正是"讲着讲着没声了"。现改为：连接已断 ⇒ 立即收场；连接仍在 ⇒ 等
//   DL_STALL_MAX_MS；超过 DL_STALL_WARN_MS 的静默在日志里标成"⚠️ 服务器停供"。
// ★V2.1.0：15s → 30s。实测官方服务器停供可达 9.5s+ 才断，而断开大概率是**我们心跳
//   先掐的**（见 enableHeartbeat 注释）⇒ 服务器"慢"与"死"必须分开对待：连接还在时
//   多给它 30s，恢复即可续播（旧 15s 会把还能救的会话判死）。真死的链路不会等满 30s——
//   要么 sendPing 写失败立刻断，要么 wsLastRxAt 死链检测在 <45s 内出手。
static const uint32_t DL_STALL_MAX_MS = 30000;   // 连接仍在时的静默上限（收场阈值）
static const uint32_t DL_STALL_WARN_MS = 2000;   // 超过它判"服务器停供"（区别于句间断粮）
static uint32_t dlSegHard;                      // = MS2B(DL_SEG_HARD_MS)（setup 里初始化）

// ---- v9.5 断供静音回填：服务器停供 ≥150ms 即补静音，把积压维持到 1500ms ----
// 根因：句间停顿服务器不发帧 → 供给间隙抽干协控积压 → 环回绕重播。正常音隙
// 实测 33-72ms，150ms 只命中真断供。
// ★V2.0.2 默认关闭（DL_FILL_ENABLE=0）：短回复时 tts stop 尚未到达就被判"断供"
// 回填 500ms 静音 → 尾音拖长。断供兜底改由"饿透标记+重推头"机制独立承担。
#define DL_FILL_ENABLE 0
static int32_t dlFillTarget;                 // 积压目标水位（setup 里 = MS2B(1500)）
static const uint32_t DL_FILL_CHUNK = 500;   // 单次回填静音量 ms
static const uint32_t DL_FILL_EVERY = 400;   // 两次回填最小间隔 ms
static const uint32_t DL_FILL_QUIET = 250;   // 流内深静音门槛 ms（部分服务器会发静音帧）
static const uint32_t DL_FILL_GAP = 150;     // 断供门槛 ms：服务器停供 ≥此值即补静音
static uint32_t dlFillLastAt = 0;            // 上次回填时刻（限频）
static uint32_t dlFillTotal = 0;             // 累计回填 ms（统计）
static uint32_t dlFillCount = 0;             // 回填次数

// ---- v9.5.3 积压趋势（B/s）：死亡指纹 = 播放冻结时 B' ≈ +22050（活时上限 ~2150）----
static int32_t dlBTrend = 0;                 // 最近一次趋势采样 B/s
static int32_t dlBTrendPrev = 0;             // 上次趋势采样点的 aheadNow
static uint32_t dlBTrendAt = 0;              // 上次趋势采样时刻（0=本会话未采样/已重锚）
static uint8_t dlDeathStreak = 0;            // 连续超死亡阈值的趋势采样次数

static uint32_t dlSegSent = 0;      // 距上次 WAV 头已 ACK 的字节数
static uint32_t dlQuietRun = 0;     // 已发连续停顿字节数（帧内最大偏移 ≤ 阈值）
static uint8_t dlSegPeak = 0;       // 距上次 WAV 头的帧最大偏移峰值（自适应阈值用）
static bool dlInQuiet = false;      // 上一发送帧是否静音
static uint32_t dlSegResets = 0;    // 段界重发头次数
static uint8_t dlCutPhase = 0;      // 饥饿复位状态：0=正常 1=停发等协控播空
static uint32_t dlCutPhaseAt = 0;   // 进入停发态的时刻
static bool dlStarved = false;      // 饿透标记：FIFO 空+协控≤50ms ⇒ 数据恢复时重推头
static uint32_t dlStarveMarkAt = 0; // 标记时刻（恢复等待超时用）
static bool dlFeedDone = false;     // 本场音频已喂完（tts stop）——喂完后不再硬顶/标记
static uint32_t dlLastAudioAt = 0;  // 最近一次收到下行音频的时刻
static bool dlRespOpen = false;     // ★V2.0.9：本轮响应没收尾（tts start→stop 之间）
                                    //   ⇒ WS 断开时用来判"服务器中途掉了"（定责用）
// ★V2.1.0：WS 层"最后收到任何服务器消息"的时刻（CONNECTED/TEXT/BIN 都刷新）。
//   心跳改成"只保活不断连"后连接变粘，必须有独立手段识别僵死链路 ⇒
//   ensureConnected() 见 >45s 无消息即判僵死、强制重连（否则按 C 说话没反应）
static uint32_t wsLastRxAt = 0;
// ★V2.1.2 诊断：入站 WS 帧大小分布。
//   理由：WebSockets 库的 handleWebsocket() 先 `malloc(payloadLen + 1)`，**失败就直接
//   `clientDisconnect(client, 1011)`** —— 不是丢帧，是整条连接当场死亡，外部只看到
//   "服务器停供 + 没声"。正常 opus 帧 ≈180B；若某帧顶到"最大连续块"（整合版实测仅 2292B），
//   连接必死。所以必须知道服务器到底发多大的帧、我们手上有多少连续块。
static uint32_t wsRxN = 0, wsRxMax = 0, wsRxBig = 0;   // 每 2s 复位的窗口统计
static uint32_t wsRxMaxAll = 0, wsRxBigAll = 0;        // 开机至今的最大帧 / >1KB 帧数
static uint32_t wsRxNTotal = 0;                        // ★V2.1.3 会话累计入站帧（不复位，断开时打它）
// ★V2.1.4：主循环频率（圈/s）+ DMA 可用堆 —— 把"服务器不发"与"我们收不到"当场分开。
//   停供时若"环 N/s"暴跌 ⇒ 我们自己卡住（泵/串口/分配阻塞），不是服务器的事；
//   若环正常而帧仍不来 ⇒ 收包路径（WiFi 驱动 DMA 缓冲/lwIP/内存）有问题。
static uint32_t loopTick = 0, loopRate = 0, loopRatePrev = 0, loopRateAt = 0;
// ★V2.1.5：诊断精简开关。默认 **false** ⇒ 播放期不再每 2s 打那条 `[DL] 链路:`（"讲着讲着没声"
//   已结案，周期诊断的使命完成）。串口按 **v** 切换；**异常路径（停供/断粮块）的链路行不受此开关影响**，
//   永远照打 —— 那才是真正需要证据的时刻。排障时按 v 即可回到 V2.1.4 的全量视图。
static bool dlVerbose = false;
// ★V2.1.3：库给的**断开原因**。本机装的 WebSockets 是改过的版本，签名是
//   `clientDisconnect(client, const char *reason)`，而 reason 会被**原样**当作
//   `WStype_DISCONNECTED` 事件的 payload 回调出来 ⇒ 这是唯一能区分"谁先动手"的证据。
//   已知来源：["Connection lost"=tcp 已断 | "TCP connection cleanup" | "Header response timeout"
//   | "HTTP xxx" | "WebSocket handshake failed"]；**空** = 服务端 close 帧 或 库内部 code 路径
//   （1002 未知 opcode / 数据缺失）。
static char wsDiscReason[64] = "";
// ★V2.1.3：自建心跳账本。库的 handleHBPing() 在 sendPing() 写失败时会直接
//   `WebSockets::clientDisconnect(1000)`（reason=NULL）——又一条"自己把自己掐死"，
//   且与"服务端 close 帧"完全无法区分。故停用库心跳、自己 ping：失败只记账不断连。
//   pong 计数则是"服务器进程是否还活着"的直接证据（WStype_PING/PONG 会回调到应用）。
static uint32_t wsPingTx = 0, wsPingFail = 0, wsPingRx = 0, wsPongRx = 0;
static uint32_t wsLastPingAt = 0, wsLastPongAt = 0;
// ★V2.1.0：服务器"响应未完成就断连"标记。播放期点阵发不出去（总线忙），所以只能把
//   它交给 endDlPlay —— 收场那一刻总线才空出来，趁机把"失望脸"打上屏，让用户**看见**
//   "这句话被打断了"（否则只是静悄悄没声，用户会怀疑设备坏了）。
static bool dlBroken = false;
static uint32_t dlStartAt = 0;      // 会话起点（重锚基线）
static uint32_t dlSentTotal = 0;    // 会话累计已 ACK 字节数
// ★★V2.4.11：播放位置的**单调累计基准**。泵的每次"重锚"（饿透推头/段界/死亡指纹/
//   ACK 超时重开会话）都会把 dlSentTotal 清零，而 mLibPlayedSec() 只读它 ⇒ 一集里
//   重锚三次就把累计实播砍掉三段（实测第 6 集 828s 报成 602s，差 226s），服务端
//   apFinSane 因此把正确的 fin=1 拦下、续播倒回同一集。清零前先把旧值存进这里。
//   只在**新的一场本地流**开始时归零，场次内任何重锚都只累加、绝不倒退。
static uint32_t mLibAckBase = 0;
static uint32_t dlRetryAt = 0;      // 启动失败退避：2 秒内不重试
static bool ttsDone = true;         // 服务器 tts state=stop 已到（流结束的协议判据）
                                    // ★初值 true = 开机视为"无 TTS 进行中"（startTalk 据此决定是否发 abort）
// ★V2.4.7：TTS 轮次计数（每次 tts start 就 +1）。ttsDone 只是"最后一条 tts 消息是不是 stop"，
//   工具被调用的那一刻它还是**上一轮**的 true —— 拿它当"确认语说完"的判据，本地流就会抢在
//   第一个字上桌。有了轮次，才能区分"这一轮的确认语还在说"。
static uint32_t ttsSeq = 0;
static bool dlEndQuiet = false;     // ★V2.4.7：这一次 endDlPlay 不收屏、不写媒体库日志
                                    //   （确认语说完让位给本地流，紧接着就要上播放图案）
static bool dlIsLocal = false;      // ★V2.4.7：本场播放喂的是**本地媒体库**流（不是云端 TTS）
                                    //   一集播完 FIFO 还在排空的那段 mLibState 已是 0，也得靠它认

// v9.17 协控不应答看门狗：连续 30 次 ACK 超时（≈3s 零应答）= 协控僵死
// （健康流实测恒 0 超时）→ 主动 endDlPlay 收场，防"泵空转 + 服务器踢连接"不可恢复态
static uint8_t dlToStreak = 0;
static const uint8_t DL_TO_STREAK_MAX = 30;  // ≈3s（RESP_TIMEOUT=100ms × 30）
static uint8_t dlRestarts = 0;               // ★V2.1.6：本场"协控无应答 → 重开会话"次数

// ---- 头前置队列：44B WAV 头必须先于一切 PCM 上线（中途头=会话杀手）----
static uint8_t dlPre[44];
static uint8_t dlPreIdx = 44;  // 44=空；0..43=待发头字节游标

// ========== 我的媒体库（自建曲库点播，V2.2.0）==========
// 语音「启用/关闭我的媒体库」= 本地开关（NVS 持久化）；开启后「播放XXX」由本机直接向
// MediaServer 取 **8bit/24000 单声道裸 PCM**（服务端已转码），塞进同一条 dlFifo → 协控播放泵。
// ★这里只放**状态变量**：handleAudio 要用 mLibState 丢服务器残帧，故必须早于它声明；
//   函数体统一放在 WS 协议段之后（那里 wsReady/sendAbort/mtx* 都已可见）。
static bool mLibOn = false;          // 媒体库开关（0=关，点歌全部交给云端）
static String mLibHost;              // 媒体库主机（空=沿用本地小智服务器 host，端口默认 8100）
static uint16_t mLibPort = 8100;
static uint8_t mLibState = 0;        // 0=空闲 1=等响应头 2=收流播放
static WiFiClient mLibCli;
// ★V2.4.11：socket 可读字节的**主循环快照**。播放泵跑在独立任务里，不能直接调
//   WiFiClient::available()（lwIP 非线程安全）；泵只打印这个 volatile 值。
static volatile int mLibSockAvail = 0;
// ★V2.4.11：1024 → 2048。服务端一张响应头里有 X-Book-Ep / X-Resume-Offset /
//   X-Book-Dur / X-Media-Title（中文 60 字 URL 编码后最多 ~540B）等十来个头，逼近 1KB；
//   顶满就认不出 \r\n\r\n ⇒ 15s 等头超时 ⇒ 整条流放弃，而且 mLibDur/mLibBaseSec 全为 0，
//   正是"时长和位置口径判断有误"的隐藏来源。宁可多花 1KB 静态 RAM。
static char mLibHdr[2048];
static uint16_t mLibHdrLen = 0;
// ★V2.2.1：中转 1024B（不是 512 —— 一圈要能多读几块补满 FIFO；也不贪大，
//   静态数组同样吃 DRAM，官方 wss 下每一 KB 都算数）。mLibTick 里一圈连读到空为止。
static uint8_t mLibBuf[1024];        // 一次读出的 PCM 中转（≤ FIFO 剩余空间）
static uint32_t mLibBytes = 0;       // 本首已收字节
static uint32_t mLibStartAt = 0;     // 请求起点（等头超时）
static uint32_t mLibBufAt = 0;       // 末次收到数据的时刻（15s 无数据判死）
static bool mLibChunked = false;     // 服务器若仍用 chunked（Node 默认），这里解码
static uint32_t mLibCkRem = 0;       // 当前块剩余字节
static uint8_t mLibCkCRLF = 0;       // 块数据后的 CRLF 待吃字节数
static char mLibCkLine[12];          // 块长行（十六进制文本）
static uint8_t mLibCkLineLen = 0;
static bool mLibEnded = false;       // 流已结束（末块 / 对端关闭）
static String mLibCurTitle;          // 当前点播关键词（串口/排障）
// ★V2.4.10 当前播放的完整信息（服务端 X-Media-Title：歌名-歌手 / 书名·演播·第N集 / 台名）
static String mLibNowTitle;
// ★设备侧 MCP（小智协议 {"type":"mcp","payload":{...}}）：服务器握手过 initialize/tools/list
//   ⇒ 语音指令由云端 LLM 判断意图并调工具（与 R1 客户端同款机制，最稳）。
//   这个标志只管一件事：MCP 可用时**关掉 STT 关键字兜底**，避免同一次请求触发两遍。
static bool mLibMcpReady = false;
// 工具点到歌后不要立刻抢麦：云端还要把"好的，为你播放…"说完。挂起一小会儿，
// 等 tts stop（或 4s 兜底）再真正起播 —— 否则用户只听到半句被切断的确认语。
static String mLibPendingQuery;
static uint32_t mLibPendingAt = 0;
static uint8_t mLibPendingKind = 1;   // ★V2.4.1 挂起请求的类型：1=歌单 2=电台 3=有声书
static uint16_t mLibPendingPos = 0;   // 歌单=idx / 有声书=集号
// ★V2.2.1：**抢跑取流**。MCP 点歌时立刻发 HTTP 请求开始缓冲（这一步要 1~3 秒：
//   服务端搜歌 + ffmpeg 起转），但**不起播**；等云端确认语说完（ttsDone）立刻接上。
//   这样才能做到"说完就唱"，而不是"说完 → 干等 2 秒 → 再唱"（实机听感就是接不上）。
//   STT 兜底路径不走这个：那条已经 abort 了云端，直接起播。
static bool mLibWaitTts = false;
// ★V2.4.7：发起这次请求时的 TTS 轮次快照。起播判据改成"等**本轮** tts 说完"：
//   ttsSeq != mLibTtsSeq0 ⇒ 云端已经为这句话开口了 ⇒ 必须等它 stop；
//   ttsSeq == mLibTtsSeq0 ⇒ 一直没开口（LLM 只回工具不吭声）⇒ 1.5s 后照播。
static uint32_t mLibTtsSeq0 = 0;
static bool mLibTtsDone0 = true;    // 请求那一刻是否"上一轮已经说完"（false = 正说着，得等它停）
// ★V2.4.7：这一次请求**会不会有云端确认语**。按键「继续播放」是本机自己发起的，
//   云端根本不知道，等它开口只会白等 1.5s ⇒ 这种请求只等 250ms 攒预滚就走。
static bool mLibExpectVoice = true;
// ★V2.4.7：请求发出后，服务器有没有"在说话的路上"（收到任何文本消息：llm 情绪 / 句子 / tts）。
//   有 ⇒ 确认语迟早会来，耐心等（6s）；一条都没有 ⇒ 这次就是纯工具调用不吭声，1.5s 后照播。
//   没有这一条，服务器慢一点（LLM 首句 >1.5s）就会重演"把确认语顶断"。
static bool mLibSawReply = false;
// ★本地播放的会话保险丝：TTS 的 120s 硬顶对一首 3~5 分钟的歌太短（每 120s 会
//   "停发→等播空→重推头"，听感就是一次 1.5s 的断口）。本地流是**按消耗速率喂**的
//   （永不 burst），不需要那么密的复位 ⇒ 本地场次放宽到 7 分钟；收场/打断时恢复。
static const uint32_t DL_SEG_HARD_LOCAL_MS = 420000;

// ★V2.4.1 播放队列（歌单 / 电台 / 有声书 —— 服务端 v1.5.1 的 /ap/play）：
//   服务端把「一张歌单」「一本书的连续几集」都用 ffmpeg concat **拼成一条流**，
//   本机看到的永远只是"一条 PCM"，不需要在 C3 上维护 N 个 rid 的队列。
//   唯一要记的是 (kind, q, pos)：说「下一首/下一集」时重连并带上新的 pos 即可。
static uint8_t mLibKind = 0;      // 0=空闲 1=歌单 2=电台 3=有声书
static String mLibQ;              // 歌单关键词 / 台名 / 书名
static uint16_t mLibPos = 0;      // 歌单=已跳过几首；有声书=当前集号
static uint16_t mLibTracks = 0;   // 本条流实际包含几首（服务端 X-Track-Count）
static bool mLibLive = false;     // 电台 = 无限流：EOF 不等于"播完了"
// ★V2.4.2：媒体库用它认设备（服务端按 MAC 记播放进度，两台机器互不干扰）。
//   直接取 WiFi.macAddress() 去冒号转大写，开机算一次，别在请求里现算。
static String mLibDevMac;         // "F09E9ECB6FBC"
static String mLibDevName;        // "AlphaPi-STEM-6FBC"
// ★V2.4.4：有声书「按 C 暂停」—— 停推流 + 上报位置；再按 C 从断点继续。
//   暂停态下上下文（kind/q/pos）全部保留，所以「下一集」「停」仍然有效。
static bool  mLibPaused = false;  // 有声书处于暂停态
static float mLibBaseSec = 0;     // 本条流起点秒（服务端 X-Resume-Offset），算绝对位置用
// ★V2.4.10 断流自愈：fin=1 的唯一来源是"设备看到流结束"，但流被服务端/网络**提前掐断**
//   也是同一个表现 —— 实测 13.8 分钟的集子断在 6.8 分钟，设备照报 fin=1 ⇒ 服务端把
//   记录标成"已播完"、下次续播跳走一整集。现在拿服务端给的集时长（X-Book-Dur）对账：
//   收到的字节数离"播完整集"还差 15s 以上 = 断流，不报 fin、原地续播（最多 3 次）。
static uint16_t mLibDur = 0;      // 本集时长秒（响应头 X-Book-Dur；0=未知，退回旧行为）
static uint8_t  mLibRetryN = 0;   // 本场断流重试次数（上限 3）
static bool     mLibRetryGo = false;   // mLibAutoAt 到点时走"原地续播"而不是"下一集"
static float    mLibRetrySs = 0;  // 重试起点（断流那一刻的集内绝对位置）
static bool  mLibUserStop = false;// 本次收场是用户主动（按 C / 说停）：不自动续播
// ★V2.4.5：有声书**自动续播**。一集真播完（服务端 EOF，不是被打断）后，等 FIFO
//   里剩下的音频播空，再自动请求下一集 —— 用户报「播完一集没有续播」就是缺这一步。
//   mLibAutoAt = 0 表示没有待办；到点后由 mLibTick 顶部消费（那时 mLibState 已经是 0，
//   所以这个判断必须放在 `if (!mLibState) return;` **之前**）。
static uint32_t mLibAutoAt = 0;   // 计划续播的时刻（millis）
static bool mLibAutoReq = false;  // 当前这条请求是自动续播发的（失败就安静收场，别报警）
static bool mLibAutoGo = false;   // 载体：mLibPlayStream 开头会 mLibStop（清上面那个），用它带进去
// ★V2.4.6：播放位置上报队列（服务端 v1.6.3 只认设备报的 /ap/pos）。
//   为什么排队而不在 mLibStop 里直接发：`startTalk()` 也会 mLibStop，用户按下 C 就要说话，
//   不能让一次同步 HTTP（最坏 800ms）堵在那里。所以只记下"报什么"，到点在主循环里发：
//   暂停/说停 → 立刻可发；本集播完 → 等 FIFO 播空（否则尾巴还没出声就报，位置偏小）。
static String   mLibRptQ;         // 要上报的书名（快照，之后 mLibQ 可能被换掉）
static uint16_t mLibRptEp = 0;    // 要上报的集号
static float    mLibRptPos = -1;  // 集内绝对位置（秒）；<0 = 队列空
static float    mLibRptPlay = 0;  // 本次会话实播秒数（老服务端按 sec+起点 结算，兼容用）
static uint8_t  mLibRptFin = 0;   // 1 = 本集真播完（服务端据此下次续播跳下一集）
static uint32_t mLibRptAt = 0;    // 到点才发（millis）
// ★V2.4.9：播放中**周期心跳**（每 30s 把"听到哪了"报一次）。
//   为什么需要：以前只在「暂停 / 说停 / 本集播完」才上报 —— 设备意外断电、死机、被拔电
//   时一次都发不出去，服务端只能把记录停在起播点，听了 40 分钟等于白听。
//   有了心跳，最坏只丢这 30s（服务端还会用墙钟再兜一道，见 v1.6.4 的 histEnd）。
//   ★心跳不能抢真事件的位置：队列里已有待发上报时（mLibRptPos>=0）就不排。
//   ★心跳在**播放中**发，HTTP 阻塞会断音 —— 只在 FIFO 水位充足时发、且用短超时。
#define LIB_POS_BEAT_MS 30000
static bool     mLibRptBeat = false;  // 这一条是心跳（服务端只更新 pos，不标结束/暂停）
static uint32_t mLibPosBeatAt = 0;    // 下一次心跳的时刻；0 = 未启动

// 前置声明（IDE 会自动补原型，但显式写出来更稳 —— 这几个在 startTalk 里先用到）
static void mLibStop(const char* why);
static void mLibPlayStream(uint8_t kind, const String& q, uint16_t pos, bool waitTts = false,
                           float seekSec = -1, bool expectVoice = true);
static void mLibPlayQuery(const String& q, bool waitTts = false);
static bool mLibNext(bool waitTts = false);
static void mLibTick();
static void mLibLog(const char* tag, const char* msg, long a = -1, long b = -1);
static void mLibPauseAndReport();
static void mLibResumePlay();
static float mLibPlayedSec();
static void mLibQueuePos(float absPos, uint8_t fin, uint32_t delayMs, bool beat = false);
static void mLibPosTick();

static inline uint16_t dlAvail() {
  return (uint16_t)(dlHead - dlTail);
}
static inline void fifoPush(uint8_t b) {
  if (dlAvail() >= DL_SIZE - 1) {
    dlTail++;
    dlOver++;
  }  // 满丢最老（延迟保护）
  dlFifo[dlHead++ % DL_SIZE] = b;
}
// v7 自适应停顿阈值：max(10, 段内帧最大偏移峰值/4)。特征=帧内 max|v-128|：
// 语音帧必有 >10 的样本峰值，静音帧 ≤8——对极轻的 TTS 声音判别力远好于帧均值
static inline uint8_t quietThr() {
  uint8_t t = dlSegPeak >> 2;
  return t < 10 ? 10 : t;
}

// 播放会话状态（非阻塞 0x15 流控：流水线在途，ACK status=实收，未收满重发）
static bool dlActive = false;
static bool dlBarged = false;  // ★打断封锁：按 C 打断后置位，丢弃旧歌/旧话的残帧；
                                //   直到服务器推新一轮 tts start 才解封。缺它时打断后
                                //   在途音乐帧重新攒满 FIFO → startDlPlay 自动复活 →
                                //   "边录边播"把协控打进无响应死态（只能断电恢复）
static uint8_t dlPay[PLAY_CHUNK];
// ---- 流水线发送（多帧在途）----
// 【实测】单帧在途吞吐上限 = 200B/9ms ≈ 22.1KB/s < 协控消耗 24000B/s；多帧在途可超。
//   但四档实测吞吐全部 ≈24000（=协控接收速率=物理瓶颈），Q=1 与官方同构、发隙最小
//   ⇒ 生产锁定 1 帧在途。
#define DL_PIPE_MAX 4
// ★V2.1.7：在途帧数 1 → 3。历史注释说"四档实测吞吐全部≈24000"，但 V2.1.6 日志
// 实测全场只有 17132 B/s（152790B / 8918ms，实时 24000），正是吞吐被 ACK 往返
// 钳制在 200B/RTT ≈ 17KB/s 的直接证据：单帧在途时，泵每圈最多发 1 帧，主循环
// 40~150 圈/s ⇒ 上限 8~30KB/s，卡在实时线附近，抖一抖就欠供 ⇒ 断粮/拖长。
// 3 帧在途把上限抬到 ~50KB/s，节拍仍由积压水位（DL_AHEAD_MAX_MS）控制。
// ★V2.2.0：3 → 1，回到 One 板的"一帧一确认"语义。
// 证据（V2.1.11 日志）：短收 526~583 次 / 3.4s（≈150 次/s），stmin=4，
// 即 200B 的一帧平均要 11 次往返才送完，每次 RTT 8ms ⇒ 每帧 ~88ms ⇒ 13 帧/s。
// 三帧在途时协控 UART RX 被连续灌满，它每次只能腾出几十字节 ⇒ 短收自我放大；
// 而短收后的尾巴进 rtx 抢先发，插在在途帧之后 ⇒ 音频字节乱序 ⇒ 卡顿。
// 一帧在途 + 帧长 160（< 协控 RX 缓冲 128~192B 的整帧吸收区间）⇒ RTT 只由
// UART 传输决定（160B@460800 ≈ 3.5ms），吞吐上限 160B/6ms ≈ 26KB/s，够实时。
#define DL_INFLIGHT_MAX 1
static uint8_t dlPendLen[DL_PIPE_MAX];  // 在途帧请求长度
static uint8_t dlPendGot[DL_PIPE_MAX];  // 在途帧已确认接收长度
static uint32_t dlPendAt[DL_PIPE_MAX];  // 在途帧发送时刻（超时判据）
static uint8_t dlPendCnt = 0;           // 当前在途帧数
static volatile bool dlWantStart = false;  // ★V2.2.0：回调只挂"该起播了"，起播动作交给泵任务
// ★V2.2.3 音量：A 单击 -，B 单击 +（原来 B 的"打断"取消，打断统一走 C）。
// 协控音量寄存器 0x14 直接收 0..100（One 板同款 N32，逆向 n32_audio.py 的 set_volume）。
// ★档位：5x5 点阵 = 25 像素 ⇒ 1 像素 = 1 档，25 档。
// ★★关键（2026-11-04 实测纠正）：STEM 这颗 N32 的 0x14 只认 **0..6 硬件档**，
//   不是 One 板的 0..100！发 96/88/80 全被它钳成 6 ⇒ 用户听感"调音量没影响"。
//   所以：25 个 UI 档 = 硬件 1..6 粗档 + 软件增益细调（只衰减，不放大，不会削波）。
//   PageTurner 项目记忆佐证：0x14 payload 0-6，volume%→min(6, v*6/100)。
static const uint8_t CMD_VOLUME = 0x14;
// ★★V2.2.8 定案（依据 PageTurner 9 月逆向笔记，血证）：
//   「0x14 = 音量指令，payload 0~6 硬件档，volume% → min(6, v*6/100)；发 00 = 静音；
//     0x14 只起播发一次，绝不重发 —— 会话级指令反复进流会打烂协控解析。」
//   所以档位就是**硬件真实 6 档**（1..6，0=静音不开放），不做软件增益（会抬底噪），
//   也绝不 0~100、0~6 乱映射（V2.2.6 发 4、V2.2.7 发 40 都被当垃圾丢掉 ⇒ "按了没反应"）。
//   下发时机也固定：只在 startDlPlay() 的起播序列里、0x10/00 停录之前发**一次**。
static const uint8_t VOL_LEVELS = 6;   // 硬件真实档位（= 点阵点亮像素数）
static const uint8_t VOL_MIN_LV = 1;   // 最低 1 档（1/6 ≈ 17%），0 = 静音不开放
static const uint8_t VOL_MAX_LV = 6;   // 满档
static const uint8_t VOL_DEF_LV = 6;   // 默认满档（与 2.2.1 的听感一致）
static uint8_t volLevel = VOL_DEF_LV;  // 已应用的档位
// ★V2.2.9：播放中也刷柱状图，但**合并 + 限流**：
//   V2.2.4 的教训不是"不能画"，而是"连按 5 次就连发 5 帧点阵"——协控渲染任务排队，
//   3 秒不应答 PCM ⇒ 看门狗收场（"播着播着没声了"）。
//   所以这里改成：按键只记账，停止按 70ms 后才发一帧；两帧之间至少间隔 140ms；
//   播放中走 mtxSendFast（9 字节即走，不等渲染 400ms）。
#define VOL_ICON_IN_PLAY 1
#define VOL_ICON_MS 1200        // 柱状图显示时长，之后恢复状态图标
#define VOL_ICON_DEBOUNCE 70    // 连点合并：最后一次按键后 70ms 才刷屏
#define VOL_ICON_MIN_GAP 140    // 两帧柱状图最小间隔（防协控渲染排队）
static uint8_t volTarget = VOL_DEF_LV;      // 按键目标档位
static volatile bool volPending = false;    // 有新的目标档位待落地
static volatile uint8_t volAckLast = 0xFF;  // 音量命令应答（0xFF=尚未收到）
static volatile bool coCmdBusy = false;     // 泵任务正在写命令帧（仅诊断用）
static uint8_t volHwStep = 0xFF;            // 已下发给协控的硬件档（0xFF=未知），仅诊断用
static bool volSaveDirty = false;           // 音量待落盘（NVS 写会卡 10~40ms，不能放在播放中）
static uint32_t volSaveAt = 0;
static uint32_t volIconAt = 0;              // 音量柱状图显示起点（VOL_ICON_MS 后恢复状态图标）
static bool volShowPend = false;            // 待刷柱状图（连点只保留最后一次）
static uint32_t volShowAt = 0;              // 最后一次按键时刻（去抖基准）
static uint32_t volShowSentAt = 0;          // 上一帧柱状图发出时刻（限流基准）
static bool volPushPend = false;            // 播放中待下发 0x14（在帧间隙发，绝不插进在途帧）

// ★★V2.2.8：档位**就是硬件值**，直接 1..6，不做任何映射。
// 教训链：V2.2.5 发 72~100（越界）→ 全被钳成 6 或丢弃 ⇒ "没影响"；
//         V2.2.6 按 0..6 发 4 ⇒ 用户"没声音"；V2.2.7 发 40（越界）⇒ "按了没反应"。
//         根因统一：0x14 只认 0..6，且**播放中反复发会被丢弃/打乱解析**。
static inline uint8_t volHwOf(uint8_t lv) {
  if (lv < VOL_MIN_LV) lv = VOL_MIN_LV;
  if (lv > VOL_LEVELS) lv = VOL_LEVELS;
  return lv;  // 1..6 原样下发
}
// 泵让路标志已删除：V2.2.3 起音量/图标帧都由泵任务自己发（单一写者），不需要暂停泵，
// 也不需要清 RX（清 RX 会把在途 PCM 应答一起吃掉 ⇒ 重发风暴 ⇒ 调音量时断流）。
static volatile bool dlPumpBusy = false;   // 泵正在执行（仅任务外层置位，诊断用）
static uint32_t dlShortAcks = 0;        // ★V2.1.7：短收次数（协控只吃下部分字节）
static uint32_t dlShort2s = 0;          // 最近 2s 的短收次数
static uint32_t dlSentDbgPrev = 0;      // 2s 喂送速率统计基准
// ★V2.1.9 诊断：入流节奏。rxgap max = 相邻两个下行音频包的最大间隔。
//   小（~45ms）且均匀 ⇒ 服务器按帧稳定发，我们也是及时收的；
//   大（>300ms）+ 成串到达 ⇒ 服务器按句 burst 且合成慢 ⇒ 欠供是上游。
static uint32_t dlRxGapMax = 0, dlRxGapAt = 0;
static uint16_t dlGapHist[5] = {0};   // 帧间隔分布：<20/<40/<80/<160/≥160ms
static uint32_t accWsUs = 0, accPumpUs = 0;  // ★V2.1.11 一圈耗时归因
static uint8_t dlAckDbg = 0;            // 前 12 条原始应答样本
static uint32_t dlAckNo = 0;            // 应答总数（★V2.1.8：稳态每 200 条抽样一次）
static uint8_t dlStMin2s = 255;         // 最近 2s 内最小的 st（判协控限流程度）
// ★V2.1.8 ARQ：每帧 payload 副本 + 独立重发缓冲。
// 旧写法短收时 `dlTail -= 未确认字节`（退回 FIFO 读指针）。在途=1 时这是对的；
// 但在途 3 时读指针早已越过后面 2 帧，退回位置落在**最新帧**身上 ⇒
// 前帧尾巴丢失 + 最新帧尾巴被重复播（每帧一次，听感=毛刺/吞字）。
// 正确做法：帧内容留在副本里，未确认尾巴进重发缓冲，优先于新数据发出。
static uint8_t dlPendBuf[DL_PIPE_MAX][PLAY_CHUNK];  // 在途帧 payload 副本
static uint8_t dlRtx[PLAY_CHUNK * DL_PIPE_MAX];     // 重发缓冲（未确认尾巴）
static uint16_t dlRtxLen = 0, dlRtxOff = 0;
static uint32_t dlRtxOverflow = 0;

// 未确认尾巴入重发缓冲（顺序追加；满了说明链路严重欠收，丢新到的计溢出）
static void rtxPush(const uint8_t* p, uint16_t n) {
  if (n == 0) return;
  if ((uint16_t)(dlRtxLen + n) > sizeof(dlRtx)) {
    // 缓冲区压缩：丢掉已发走的头部，腾空间
    if (dlRtxOff > 0) {
      uint16_t keep = (uint16_t)(dlRtxLen - dlRtxOff);
      memmove(dlRtx, dlRtx + dlRtxOff, keep);
      dlRtxLen = keep;
      dlRtxOff = 0;
    }
    if ((uint16_t)(dlRtxLen + n) > sizeof(dlRtx)) {
      dlRtxOverflow++;
      return;
    }
  }
  memcpy(dlRtx + dlRtxLen, p, n);
  dlRtxLen += n;
}

// 解码统计（2s 汇总打印后清零）
uint32_t dlFrames = 0, dlBytes = 0, dlDropped = 0, dlDecErr = 0;

// 解码输出缓冲（120ms@24k 余量）
static const int DEC_BUF_SAMPLES = 2880;
static int16_t decBuf[DEC_BUF_SAMPLES];

static void endDlPlay(const char* why);
static void stopDlPlayHard();

// ================== ★手段④：0x80 寄存器扫描探针（2026-10-10）==================
// 【为什么做】绕圈根因 = 写指针越过环尾 65536 样本后，协控读指针错位 150~170ms
//   ⇒ 吞字/重播（错位点固定在环位置 45952）。EXP1/EXP2/EXP3 三轮全部落在"盲写"
//   框架里 —— 我们不知道协控此刻播到哪、缓冲还剩多少、写指针在环内什么位置。
//   若协控内部存在"播放进度 / 写指针 / 剩余量"这类**可读寄存器**，绕圈就能从
//   "事后听出来"变成"事前刹车"（闭环控制），这是**唯一不依赖厂商升级固件**的根治路径。
// 【依据】官方 controlBoard.mpy 反汇编（官方音频机制_反汇编还原.md，高置信）：
//   uart_read(addr,n) 帧 = [0x80, addr, n]（3 字节，**无 ck**）；
//   回包 = 4+n 字节（"3 头 + n 数据 + 1 ck"），数据在 r[3:3+n]。
//   ★头 3 字节的**具体内容反汇编并未确证**；官方只对 addr=0x11 用它拉录音数据。
// 【★第一版翻车记录（2026-10-10 夜，务必记住）】v1 按 `[0x91, addr, n]` 严格匹配滑窗收包，
//   实测：敲 p 后 A 阶段**一行都不打印**（只有启动两行 + 播放照常继续）—— 这**不是协控
//   没反应**，而是"我不认识它的回包，全当无响应丢了"：回包头大概率不是 0x91。
//   ⇒ 教训（与既有铁律一致：先数线上字节、再立协议假设）：**本版改为原始抓包**——
//   不解析、不假设、收到什么就打印什么。先拿到原始字节，再回头定协议。
//   ⇒ 教训（与既有铁律一致——**先数线上字节，再立协议假设**）：改为**原始抓包**，
//   不解析、不假设、收到什么就打什么。先拿到原始字节，再回头定协议。
// 【★第二版实测（2026-10-10 23:10，EXP4b抓包版，用户日志）】**格式完全破译**：
//   ★读回包 = [0x81, addr, len, data[len], ck]，ck = (0x81+addr+len+Σdata)&0xFF ★
//     证据：A 01 → `81 01 04 00 00 00 00 86`（ck=81+01+04=0x86 ✓）；A FF → 逐字节吻合 ✓
//   ★256/256 全部应答（4588ms）⇒ **0x80 读通道完全开放**；v1 翻车只因我猜了 0x91。
//   ★addr 是**内存偏移**（不是寄存器号）：回包给的是"内存 [addr, addr+4) 窗口"。
//     铁证 = 0xED..0xFF 逐地址 1 字节步进地滑出同一段连续字节流：
//       mem[ED..FF] = 00 00 00 B5 D0 60 01 | 5A 45 52 4F 20 44 65 63 20 32 31 20 20 38 31
//                                          "Z  E  R  O  ' ' D  e  c  ' ' 2  1  ' ' ' ' 8  1"
//     ⇒ 一片 ROM 里的构建字串 "ZERO Dec 21  81"（前 7 字节 00 00 00 B5 D0 60 01 为二进制头）。
//     另见非零：mem[12]=06、mem[14]=05（相隔 2，疑两个 uint16 计数）；其余 0x00..0xEC 全零。
//   ★代价（决定性）：**0x80 读会抢协控 UART、干扰 0x15 写**。B 阶段每 250ms 一读
//     ⇒ 播放 ACK 大面积丢失 ⇒ 30 次超时看门狗反复收场 ⇒ "起-死-起-死"螺旋
//     （日志连发 `播放结束(协控不应答): 1400 字节 / 3137 ms` 即此）。A 阶段高频读反而
//     没打死（写帧的 ACK 重发机制能吸收），但**不可依赖**。
//   ⇒ **判定：手段④关闭。** 两条理由都成立：①256 地址里没有随播放变化的进度/指针
//     寄存器（B 轮所有表面 Δ 全是 ACK 串扰假象，真候选地址重扫值恒定）；
//     ②"边播边读"的闭环物理上不可行（读会打断播放）。根治仍只剩厂商升级协控固件。
// 【第三版（EXP4c）—— 只做"低成本复验"的准备，默认不主动跑】：
//   ①解析化：用已破译格式 probeExtract() 取数据段，不再刷 256 行原始字节；
//   ②A 阶段**先收集后汇总**，只打印数据非零的地址（256 行 → 几行，含进度行防"以为卡住"）；
//   ③B/C 阶段**只扫非零地址**（个数由 256 降到个位数 ⇒ 干扰时长 64s → 2s 级）；
//   ④B 间隔 250→400ms、读前清 RX 残留（进一步降串扰）；
//   ⑤保留 probeSniff 原始抓包（将来格式再变时可直接复用）。
// 【安全】①只在串口命令触发后运行，默认零开销（loop 里一次判空返回）；
//   ②每个地址 = 一个"发-收原子事务"（发送后立即在本函数内轮询收完，绝不与
//     pumpPlay() 的 ACK 结算交错）；③抓包前清 RX 残留（pumpPlay 刚结算完 ACK）；
//   ④★放歌中跑探针会打断播放（见上）——要中止就再敲一次 p / 敲 s。
// 【用法】串口敲 `p` ⇒ 三阶段全自动：
//   A 全表读 addr 0..255（reqLen=4，窗口 8ms）→ 汇总打印非零地址
//   B 隔 400ms 对非零地址重扫 → 打印 前4B -> 后4B，值变了标 Δ
//   C 对 Δ 的地址每 180ms 采一次 × 20（3.6s > 环长 2.73s）→ 打曲线
// 【★状态：整块默认关闭（2026-10-10 收口）】手段④判定关闭（格式已破译，但①无进度
//   寄存器 ②读会打断播放）。探针占 **2.6KB 静态 RAM**（probeDat/probeV2 各 1KB + 两张 256 表），
//   播放期堆余量只有十几 KB ⇒ 生产版必须不编译它。要复跑：`PROBE_ENABLE` 改 1 重编译即可，
//   代码与全部历史记录（含原始抓包 probeSniff）完整保留在本块内。
#define PROBE_ENABLE 0
#if PROBE_ENABLE
static uint8_t  probePhase = 0;      // 0=空闲 1=A全表读 2=B重扫 3=C时序
static uint16_t probeAddr = 0;       // 当前地址游标
static uint32_t probeNextAt = 0;     // 下一次动作最早时刻（ms）
static uint32_t probeT0 = 0;         // 本轮启动时刻（ms）
static uint8_t  probeLen[256];       // 该地址回包数据段长度（解析出的 len）
static uint8_t  probeDat[256][4];    // 该地址数据段（A 轮基准）——复用原 probeV1
static uint8_t  probeHas[256];       // 数据非零 ⇒ 进入 B/C
static uint8_t  probeV2[256][4];     // B 轮数据
static uint8_t  probeChg[6];         // Δ 地址表（≤6）
static uint8_t  probeChgN = 0;
static uint16_t probeAckN = 0;       // A 轮有回包地址数
static uint16_t probeValN = 0;       // A 轮数据非零地址数
static uint8_t  probeSeqAddr = 0;    // C 阶段当前跟踪的 Δ 序号
static uint8_t  probeSeqCol = 0;     // C 阶段采样列号
static const uint8_t PROBE_SEQ_N = 20;   // 20 × 180ms = 3.6s（> 环长 2.73s）
static uint8_t  probeSeq[20][4];     // C 阶段每点数据 4 字节

// ★原始抓包（保留工具）：发 [0x80,addr,n]，清 RX 残留后收 winMs 内到达的所有字节（≤cap）。
//   返回收到字节数（0 = 无任何回包）。不做任何格式假设。
static uint8_t probeSniff(uint8_t addr, uint8_t reqLen, uint8_t* out, uint8_t cap, uint16_t winMs) {
  while (coUart.available()) coUart.read();   // 清残留（pumpPlay 本轮已结算过 ACK）
  uint8_t req[3] = { 0x80, addr, reqLen };
  coUart.write(req, 3);
  coUart.flush();
  uint32_t t0 = micros();
  uint32_t lim = (uint32_t)winMs * 1000u;
  uint8_t n = 0;
  while ((uint32_t)(micros() - t0) < lim && n < cap) {
    if (coUart.available()) out[n++] = (uint8_t)coUart.read();
  }
  return n;
}

// ★解析（已破译格式 [0x81, addr, len, data[len], ck]）：滑窗定位并取数据段。
//   返回数据字节数（≤4；0 = 没找到或没收全）。滑动是为容忍前导串扰字节。
static uint8_t probeExtract(const uint8_t* b, uint8_t n, uint8_t addr, uint8_t* d4) {
  memset(d4, 0, 4);
  for (uint8_t i = 0; (uint8_t)(i + 3) <= n; i++) {
    if (b[i] != 0x81 || b[i + 1] != addr) continue;
    uint8_t len = b[i + 2];
    if (len > 4) len = 4;
    if ((uint8_t)(n - (i + 3)) < len) return 0;   // 数据没收全 ⇒ 当失败
    memcpy(d4, b + i + 3, len);
    return len;
  }
  return 0;
}

static void probeStart() {
  probePhase = 1;
  probeAddr = 0;
  probeNextAt = 0;
  probeT0 = millis();
  probeChgN = 0;
  probeAckN = 0;
  probeValN = 0;
  probeSeqAddr = probeSeqCol = 0;
  memset(probeHas, 0, sizeof(probeHas));
  memset(probeLen, 0, sizeof(probeLen));
  memset(probeDat, 0, sizeof(probeDat));
  Serial.println("[P] === 手段④ 探针 v3（解析版）：扫 addr 0..255，发 [80 xx 04]，窗口 8ms ===");
  Serial.println("[P] 回包格式(已破译): 81 <addr> <len> <data...> <ck>，ck=(81+addr+len+Σdata)&0xFF");
  Serial.printf("[P] 上下文: %s | 环长 2730ms\n",
                dlActive ? "★播放中（⚠️ 读会干扰播放）" : (recPhase == 1 ? "录音中" : "空闲（无播放）"));
  Serial.println("[P] 中止：再敲一次 p（或 P）/ 敲 s");
}

static void probeTick() {
  if (probePhase == 0) return;
  uint32_t now = millis();
  if ((int32_t)(now - probeNextAt) < 0) return;
  uint8_t buf[12], d4[4];

  // ---------- A：全表读（先收集，最后只汇总打印非零）----------
  if (probePhase == 1) {
    memset(buf, 0, sizeof(buf));
    uint8_t n = probeSniff((uint8_t)probeAddr, 4, buf, sizeof(buf), 8);
    uint8_t dl = probeExtract(buf, n, (uint8_t)probeAddr, d4);
    if (dl > 0) {
      probeAckN++;
      probeLen[probeAddr] = dl;
      memcpy(probeDat[probeAddr], d4, 4);
      if ((d4[0] | d4[1] | d4[2] | d4[3]) != 0) { probeHas[probeAddr] = 1; probeValN++; }
    }
    probeAddr++;
    if (probeAddr >= 256) {
      Serial.printf("[P] --- A 完成：应答 %u/256，数据非零 %u 个 | 耗时 %lums ---\n",
                    (unsigned)probeAckN, (unsigned)probeValN, (unsigned long)(millis() - probeT0));
      if (probeAckN == 0) {
        Serial.println("[P] ⚠️ 全表零应答 ⇒ 0x80 读通道不开放（或窗口太短）⇒ 手段④关闭");
        probePhase = 0;
        return;
      }
      Serial.println("[P] 非零地址（addr: d0 d1 d2 d3）:");
      for (uint16_t a = 0; a < 256; a++) {
        if (!probeHas[a]) continue;
        Serial.printf("[P]   %02X: %02X %02X %02X %02X\n", (unsigned)a,
                      probeDat[a][0], probeDat[a][1], probeDat[a][2], probeDat[a][3]);
      }
      if (probeValN == 0) {
        Serial.println("[P] ⚠️ 全部可读地址为 0 ⇒ 无进度/指针类寄存器 ⇒ 手段④关闭");
        probePhase = 0;
        return;
      }
      probePhase = 2;
      probeAddr = 0;
      probeNextAt = now + 400;
      return;
    }
    if ((probeAddr & 63) == 0) Serial.printf("[P] .. 进度 %u/256\n", probeAddr);
    probeNextAt = now;
    return;
  }

  // ---------- B：对非零地址重扫（看值是否在变）----------
  if (probePhase == 2) {
    while (probeAddr < 256 && !probeHas[probeAddr]) probeAddr++;
    if (probeAddr >= 256) {
      Serial.printf("[P] --- B 完成：%u 个非零地址里 %u 个发生变化（Δ）---\n",
                    (unsigned)probeValN, (unsigned)probeChgN);
      if (probeChgN == 0) {
        Serial.println("[P] ⚠️ 无 Δ ⇒ 可读数据全是静态的（非进度/指针类）⇒ 手段④关闭");
        probePhase = 0;
        return;
      }
      Serial.print("[P] Δ 地址: ");
      for (uint8_t i = 0; i < probeChgN; i++) Serial.printf("%02X ", probeChg[i]);
      Serial.println();
      probePhase = 3;
      probeSeqAddr = 0;
      probeSeqCol = 0;
      probeNextAt = now;
      return;
    }
    memset(buf, 0, sizeof(buf));
    uint8_t n = probeSniff((uint8_t)probeAddr, 4, buf, sizeof(buf), 8);
    uint8_t dl = probeExtract(buf, n, (uint8_t)probeAddr, d4);
    if (dl > 0) {
      memcpy(probeV2[probeAddr], d4, 4);
      bool chg = memcmp(probeDat[probeAddr], probeV2[probeAddr], 4) != 0;
      Serial.printf("[P] B %02X: %02X %02X %02X %02X -> %02X %02X %02X %02X%s\n",
                    (unsigned)probeAddr,
                    probeDat[probeAddr][0], probeDat[probeAddr][1],
                    probeDat[probeAddr][2], probeDat[probeAddr][3],
                    probeV2[probeAddr][0], probeV2[probeAddr][1],
                    probeV2[probeAddr][2], probeV2[probeAddr][3],
                    chg ? "  ← Δ" : "");
      if (chg && probeChgN < 6) probeChg[probeChgN++] = (uint8_t)probeAddr;
    } else {
      Serial.printf("[P] B %02X: （本轮无回包/被串扰吃掉）\n", (unsigned)probeAddr);
    }
    probeAddr++;
    probeNextAt = now + 400;
    return;
  }

  // ---------- C：Δ 地址时序（每 180ms × 20 = 3.6s > 环长）----------
  if (probePhase == 3) {
    if (probeSeqCol >= PROBE_SEQ_N) {
      Serial.printf("[P] C %02X 曲线(20 点 × 180ms，每点 4 字节):\n  ", probeChg[probeSeqAddr]);
      for (uint8_t i = 0; i < PROBE_SEQ_N; i++) {
        Serial.printf("%02X%02X%02X%02X|", probeSeq[i][0], probeSeq[i][1],
                      probeSeq[i][2], probeSeq[i][3]);
      }
      Serial.println();
      probeSeqAddr++;
      probeSeqCol = 0;
      if (probeSeqAddr >= probeChgN) {
        Serial.printf("[P] === 手段④ 探针全部完成，总耗时 %lums ===\n",
                      (unsigned long)(millis() - probeT0));
        Serial.println("[P] 判读：某地址数据随采样**单调递增** ⇒ 进度/写指针类寄存器（可闭环）；"
                       "恒定/乱跳 ⇒ 非进度寄存器 ⇒ 手段④关闭");
        probePhase = 0;
        return;
      }
      probeNextAt = now;
      return;
    }
    uint8_t a = probeChg[probeSeqAddr];
    memset(buf, 0, sizeof(buf));
    uint8_t n = probeSniff(a, 4, buf, sizeof(buf), 8);
    probeExtract(buf, n, a, d4);
    memcpy(probeSeq[probeSeqCol], d4, 4);
    probeSeqCol++;
    probeNextAt = now + 180;
    return;
  }
}
#endif  // PROBE_ENABLE

// ========== 点阵屏联动（5x5 LED，协控 addr=8，复用 coUart/sendWait）==========
// 帧格式：[0x90, 0x08, 0x05, 5列数据, ck]，ck=(0x90+0x08+0x05+Σdata)&0xFF
// 列数据列优先、bit7..bit3=第1..5行再左移3位（与 PageTurner MatrixDisplay 逐位一致）。
// ★铁律：录音(recPhase!=0)/播放(dlActive)期间总线忙，显示帧绝不发送——否则显示帧
//   插进音频流会吃掉 ACK/录音字节。播放/录音期间只缓存意图(mtxWant)，回到空闲
//   由 mtxFlush() 补发（endDlPlay/loop 均有调用点）。
// 协控无响应熔断 30s（PageTurner 同款）：防 sendWait 重试链阻塞主循环。

static const uint8_t MTX_ADDR = 8;

// ---- 图案（25 点，行优先，1=亮）----
static const uint8_t MTX_P_CLEAR[25] = {
    0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0};
static const uint8_t MTX_P_MIC[25] = {      // 麦克风（录音中）
    0,0,1,0,0,
    0,1,1,1,0,
    0,1,1,1,0,
    0,0,1,0,0,
    0,1,1,1,0};
static const uint8_t MTX_P_THINK[25] = {    // 菱形（思考中/处理中）
    0,0,1,0,0,
    0,1,1,1,0,
    1,1,1,1,1,
    0,1,1,1,0,
    0,0,1,0,0};
static const uint8_t MTX_P_PLAY[25] = {     // 播放三角（TTS 说话中）
    0,1,0,0,0,
    0,1,1,0,0,
    0,1,1,1,0,
    0,1,1,0,0,
    0,1,0,0,0};
static const uint8_t MTX_P_MUSIC[25] = {    // 音符（点歌播放中）
    0,0,1,1,0,
    0,0,1,1,0,
    0,0,1,0,0,
    0,1,1,0,0,
    0,1,1,0,0};
static const uint8_t MTX_P_BOOK[25] = {     // 摊开的书（有声书播放中，用户 2026-11-11 选定）
    1,0,0,0,1,
    1,1,0,1,1,
    1,1,0,1,1,
    1,1,0,1,1,
    0,1,1,1,0};
static const uint8_t MTX_P_RADIO[25] = {    // 收音机（网络电台播放中，同日选定）
    1,0,0,0,0,
    0,1,1,1,1,
    1,0,1,0,1,
    1,0,1,0,1,
    1,1,1,1,1};
static const uint8_t MTX_P_PAUSE[25] = {    // 暂停（有声书按 C 暂停后）
    0,0,0,0,0,
    1,1,0,1,1,
    1,1,0,1,1,
    1,1,0,1,1,
    0,0,0,0,0};
// ---- 情绪脸（服务器 {"type":"llm","emotion":"..."} 驱动）----
static const uint8_t MTX_P_SMILE[25] = {
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    1,0,0,0,1,
    0,1,1,1,0};
static const uint8_t MTX_P_GRIN[25] = {     // 眯眼大笑
    0,0,0,0,0,
    1,1,0,1,1,
    0,0,0,0,0,
    1,0,0,0,1,
    0,1,1,1,0};
static const uint8_t MTX_P_HEART[25] = {    // 爱心（loving）
    0,1,0,1,0,
    1,1,1,1,1,
    1,1,1,1,1,
    0,1,1,1,0,
    0,0,1,0,0};
static const uint8_t MTX_P_FROWN[25] = {
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    0,1,1,1,0,
    1,0,0,0,1};
static const uint8_t MTX_P_ANGRY[25] = {    // 斜眉+平嘴
    1,1,0,1,1,
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    1,1,1,1,1};
static const uint8_t MTX_P_WOW[25] = {      // 惊讶（圆嘴）
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    0,0,1,0,0,
    0,1,1,1,0};
static const uint8_t MTX_P_CRY[25] = {      // 泪痕+撇嘴
    0,0,0,0,0,
    0,1,0,1,0,
    1,0,0,0,1,
    0,1,1,1,0,
    0,0,0,0,0};
static const uint8_t MTX_P_SLEEPY[25] = {   // 闭眼线+小嘴
    0,0,0,0,0,
    1,1,0,1,1,
    0,0,0,0,0,
    0,0,0,0,0,
    0,0,1,0,0};
static const uint8_t MTX_P_PONDER[25] = {   // 思考（嘴偏一侧）
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    0,1,1,0,0,
    0,0,0,0,0};
static const uint8_t MTX_P_NEUTRAL[25] = {
    0,0,0,0,0,
    0,1,0,1,0,
    0,0,0,0,0,
    0,0,0,0,0,
    0,1,1,1,0};

static const uint8_t MTX_P_WIFI_ON[25] = {   // 配网门户已开（信号弧 + 点）
  0,1,1,1,0,
  1,0,0,0,1,
  0,1,0,1,0,
  0,0,1,0,0,
  0,0,0,0,0
};
static const uint8_t MTX_P_WIFI_OFF[25] = {  // 配网门户已关（大 X）
  1,0,0,0,1,
  0,1,0,1,0,
  0,0,1,0,0,
  0,1,0,1,0,
  1,0,0,0,1
};

// ---- 意图状态机：想显示什么(mtxWant) vs 已上屏什么(mtxCur) ----
enum { MTX_I_CLEAR = 0, MTX_I_MIC, MTX_I_THINK, MTX_I_PLAY, MTX_I_MUSIC,
       MTX_I_BOOK, MTX_I_RADIO, MTX_I_PAUSE, MTX_I_FACE };
static uint8_t mtxWant = MTX_I_CLEAR;
static uint8_t mtxCur = 0xFF;               // 0xFF=未知（开机首帧必发）
static const uint8_t* mtxFace = MTX_P_NEUTRAL;  // 最近一次情绪脸
static bool mtxMusic = false;               // 本次 TTS=点歌（sentence_start 含 % play_music）
static uint32_t mtxFailUntil = 0;           // 无响应熔断截止时刻
static uint8_t mtxFailCnt = 0;              // 连续失败轮数（阶梯重试：前 10 轮 2s 一试=覆盖协控晚开机，之后 30s）

static void mtxSet(uint8_t id) { mtxWant = id; }
static void mtxSetFace(const uint8_t* face) { mtxFace = face; mtxWant = MTX_I_FACE; }

// ★V2.4.7：本地媒体库"图标持有期" —— 本地流播放中（含"一集播完、FIFO 还在排空"那段），
//   以及有声书暂停态。这段期间**云端的一律让位**：以前服务器的 {"emotion":"happy"} 一来就把
//   mtxWant 改成笑脸，用户调一下音量（柱状图 1.2s 后恢复图标画的是 mtxWant）屏幕上就永久
//   变成笑脸，有声书/音乐/电台的图案再也回不来。用户口径：这段只显示 kind 图案 / 暂停图案。
static inline bool mLibIconHold() {
  return (dlActive && dlIsLocal) || (mLibPaused && mLibKind == 3);
}
static inline uint8_t mLibIconId() {
  if (mLibPaused && mLibKind == 3) return MTX_I_PAUSE;
  return mLibKind == 3 ? MTX_I_BOOK : (mLibKind == 2 ? MTX_I_RADIO : MTX_I_MUSIC);
}

// 服务器情绪名 → 5x5 脸（未收录情绪一律中性脸）
static void mtxApplyEmotion(const String& emo) {
  if (mLibIconHold()) return;   // ★V2.4.7：本地流在播/暂停中 —— 图标不换脸
  if (emo == "happy" || emo == "silly" || emo == "delicious" || emo == "kissy") mtxSetFace(MTX_P_SMILE);
  else if (emo == "laughing" || emo == "cool" || emo == "relaxed" || emo == "confident" || emo == "winking") mtxSetFace(MTX_P_GRIN);
  else if (emo == "loving") mtxSetFace(MTX_P_HEART);
  else if (emo == "sad" || emo == "confused" || emo == "embarrassed") mtxSetFace(MTX_P_FROWN);
  else if (emo == "angry" || emo == "shocked") mtxSetFace(MTX_P_ANGRY);
  else if (emo == "surprised") mtxSetFace(MTX_P_WOW);
  else if (emo == "crying") mtxSetFace(MTX_P_CRY);
  else if (emo == "sleepy") mtxSetFace(MTX_P_SLEEPY);
  else if (emo == "thinking") mtxSetFace(MTX_P_PONDER);
  else mtxSetFace(MTX_P_NEUTRAL);
}

// 点阵帧发送+收确认（EXP34 判决语义）：帧 = [0x90, addr, len=5, 5列数据, ck] 共 9 字节；
// 成功判据 = 应答 3 字节且 status==0x05（干净态 @0ms 即回 [91,08,05]）；FF=校验错。
// ★★历史大坑（EXP35 破案）：旧版 f[8] 只有 8 字节——5 列循环写 f[3..7] 后又用 f[7]=ck
//   把第 5 列覆盖掉，线上=头3+4列+ck。协控按 len=5 收数据时把 ck 吃成第 5 列、还在等
//   第 9 字节 ⇒ 静默丢弃 ⇒ 0 字节应答。v9.24 开机以来屏不亮的唯一真因。
// 调用前提：总线空闲（mtxFlush 已保证），可安全清 RX 残渣。
static uint8_t mtxLastGot = 0;  // 末次尝试收到的字节数（诊断用）
static uint8_t mtxLastSt = 0;   // 末次响应第 3 字节（0xFF=不足 3 字节）

// 5x5 位图 → 9 字节线上帧（头3 + 5列 + ck）。抽出来给阻塞版和快速版共用，
// 避免两份逻辑各自演进出"少一字节/第5列被 ck 覆盖"那类老坑。
static void mtxBuild(const uint8_t p[25], uint8_t f[9]) {
  uint8_t cols[5] = {0, 0, 0, 0, 0};
  for (uint8_t y = 0; y < 5; y++)
    for (uint8_t x = 0; x < 5; x++)
      if (p[y * 5 + x]) cols[x] |= (1 << (4 - y));  // 行→列位：(1<<(4-y))<<3 = 1<<(7-y)，与手册 set_pixel 一致
  f[0] = 0x90; f[1] = MTX_ADDR; f[2] = 0x05;
  for (uint8_t i = 0; i < 5; i++) f[3 + i] = cols[i] << 3;
  uint8_t ck = 0;
  for (uint8_t i = 0; i < 8; i++) ck += f[i];
  f[8] = ck;
}

// ★V2.2.3 非阻塞点阵发送：只写 9 字节就走（460800 下 ≈0.2ms），不清 RX、不等应答、
// 不做 400ms 渲染等待。播放中调音量必须走这条——旧路径 mtxSend 最多阻塞 5×150+400ms，
// 还会清掉在途 PCM 应答 ⇒ 每秒十几帧的重发风暴 ⇒ 用户听到的"调音量断流/卡一下"。
// 应答 [91 08 xx] 是 3 字节定长，泵任务的音频应答解析会跳过它，不会错位。
static void mtxSendFast(const uint8_t p[25]) {
  uint8_t f[9];
  mtxBuild(p, f);
  coCmdBusy = true;
  coUart.write(f, 9);
  coCmdBusy = false;
}

// 意图 id → 图案（mtxFlush 与"播放中恢复图标"共用）
static const uint8_t* mtxPatternOf(uint8_t id) {
  switch (id) {
    case MTX_I_MIC:   return MTX_P_MIC;
    case MTX_I_THINK: return MTX_P_THINK;
    case MTX_I_PLAY:  return MTX_P_SMILE;  // V2.0.2：播放中显示笑脸（用户偏好，替换三角）
    case MTX_I_MUSIC: return MTX_P_MUSIC;
    case MTX_I_BOOK:  return MTX_P_BOOK;
    case MTX_I_RADIO: return MTX_P_RADIO;
    case MTX_I_PAUSE: return MTX_P_PAUSE;
    case MTX_I_FACE:  return mtxFace;
    default:          return MTX_P_CLEAR;
  }
}

static bool mtxSend(const uint8_t p[25]) {
  uint8_t f[9];
  mtxBuild(p, f);
  for (uint8_t att = 0; att < 5; att++) {   // 5 次重试+30s 熔断防阻塞主循环
    while (coUart.available()) coUart.read();  // 清残渣（调用点均总线空闲，无音频 ACK 可吃）
    coUart.write(f, 9);
    if (att == 0) {  // 首次尝试打印线上帧（诊断：可与 EXP34 成功帧逐字节比对）
      Serial.printf("[MTX] 帧: ");
      for (uint8_t i = 0; i < 9; i++) Serial.printf("%02X ", f[i]);
      Serial.println();
    }
    uint8_t got = 0, resp[3] = {0, 0, 0};
    uint32_t t0 = millis();
    while (got < 3 && millis() - t0 < 150) {   // 官方 uart timeout=100ms，留余量；3 字节齐即提前退出
      while (coUart.available() && got < 3) resp[got++] = coUart.read();
    }
    mtxLastGot = got;
    mtxLastSt = (got >= 3) ? resp[2] : 0xFF;
    if (got >= 3 && resp[2] == 0x05) {       // EXP34 实测定案：成功=status 0x05（@0ms 即回）
      delay(400);                            // 官方 led_show_bytes 同款：渲染落定 400ms，
      return true;                           //   之后紧接的音频启动帧不会撞上渲染期
    }
    // ★失败路径不补 0x00 同步字节（EXP34：0x00 疑似毒化本协控解析器——V2 帧经 0x00 后即 0 字节）
    delay(150);                              // 等待后重试
  }
  return false;
}

// 真正发送：只在总线空闲瞬间调用（loop/endDlPlay/各状态切换点）
static void mtxFlush() {
  if (mtxWant == mtxCur) return;
  if (recPhase != 0 || dlActive) return;                  // 录音/播放期间总线忙
  // ★V2.2.9：音量柱状图显示期内不许覆盖（否则空闲按音量键只闪一下就没了）
  if (volIconAt && (int32_t)(millis() - volIconAt) < VOL_ICON_MS) return;
  if ((int32_t)(millis() - mtxFailUntil) < 0) return;     // 熔断期内
  const uint8_t* p = mtxPatternOf(mtxWant);
  if (!mtxSend(p)) {
    mtxFailCnt++;
    // 阶梯重试：前 10 轮每 2s 一试（协控可能比 ESP32 晚启动 ~20s，开机 1s 窗口内失败≠死），
    // 之后 30s 熔断防日志刷屏；mtxCur 不更新，到期自动重试
    mtxFailUntil = millis() + (mtxFailCnt <= 10 ? 2000 : 30000);
    Serial.printf("[MTX] 第%d轮未确认，%ds后重试（响应 %d 字节 / status=0x%02X）\n",
                  mtxFailCnt, (mtxFailCnt <= 10 ? 2 : 30), mtxLastGot, mtxLastSt);
    return;
  }
  if (mtxFailCnt) Serial.printf("[MTX] 点阵恢复应答（第%d轮成功）\n", mtxFailCnt);
  mtxFailCnt = 0;
  mtxCur = mtxWant;
}

// 一次性短显图案（A 键配网门户开关提醒用）：直发 → 停留 1s → 清屏。
// 仅限空闲时调用（调用方保证 recPhase==0 && !dlActive，否则总线忙发帧会被协控吃掉）
static void mtxBrief(const uint8_t* pat) {
  mtxSend(pat);
  delay(1000);
  mtxSend(MTX_P_CLEAR);
  mtxWant = MTX_I_CLEAR;
  mtxCur = MTX_I_CLEAR;
}

// [EXP34 判决] 帧格式定案：带len [0x90,08,05,<5列>,ck] 正确——干净状态下协控 @0ms 回
// [91,08,05]（status=05=成功），V0 全亮肉眼可见执行。无len 帧被拒（len 字节位被当数据
// → 校验错回 [91,08,FF]，FF=错误码）。
// ★头号嫌疑=0x00 同步字节：V2 例行帧（与探针 V2 逐字节相同）在探针后经 0x00 再发即 0 字节。
//   0x00 是热修1 引入的（旧版官方库遗产），恰与"屏从未亮过"的时间线吻合。本版全部移除。

// 开播：推 44B WAV 头（官方同款无限流哨兵）+ 发启动指令
static void startDlPlay() {
  dlBroken = false;  // ★V2.1.0：新一轮播放清"中断"标记（防上一轮残留误显示失望脸）
  buildWavHeader44();
  memcpy(dlPre, wavHdr, 44);
  dlPreIdx = 0;    // 头进前置队列：保证先于预滚 PCM 上线（中途头=会话杀手）
  dlSegSent = 44;  // 开播头 44B 计入首段
  // 点阵：说话/点歌图标。★必须在 dlActive=true 之前发——mtxFlush 判总线忙会拒发
  // ★V2.4.4：本地流按 kind 区分图案（音符 / 书 / 收音机）；云端 TTS 的"点歌"仍是音符
  // ★V2.4.7：本地场次用 dlIsLocal 认（一集播完 FIFO 还在排空时 mLibState 已是 0，
  //   但那一场的图标仍然必须是有声书图案，不能退回笑脸/音符）。
  //   ★必须带 !mLibWaitTts：等确认语那段 mLibState 也是 2，可那时播的是**云端 TTS**，
  //   不能被当成"本地流"（否则一集没收场就被判成本地图案 / 云端残帧全丢）。
  dlIsLocal = (mLibState != 0 && !mLibWaitTts);
  mtxSet(mtxMusic ? (dlIsLocal ? (mLibKind == 3 ? MTX_I_BOOK
                                 : (mLibKind == 2 ? MTX_I_RADIO : MTX_I_MUSIC))
                              : MTX_I_MUSIC)
                  : MTX_I_PLAY);
  mtxFlush();
  dlActive = true;
  dlPendCnt = 0;  // 清流水线在途队列（残留会让下一场错位结算）
  dlRtxLen = 0;   // ★V2.1.8：重发缓冲同步清空（上一场的尾巴不能混进新会话）
  dlRtxOff = 0;
  dlMbGate = 0;   // ★实验①：水位跳过计数（每场清零）
#if DL_EXP2_SEG
  dlExp2Segs = 0;     // ★实验②：分段触发计数（每场清零）
  dlExp2StopSum = 0;
  dlExp2StopMax = 0;
  dlSegMaxMs = 0;     // ★EXP2b 探针（每场清零）
  dlWrapCross = 0;
  dlWrapAlarm = false;
  dlPadDone = false;  // ★EXP3 静音垫（每场清零）
  dlPadAtMs = 0;
  dlExp2Quiet = 0;
#endif
  dlStartAt = millis();
  mLibAckBase = 0;      // ★V2.4.11：新场次，单调累计基准归零
  dlSentTotal = 0;
  dlSegResets = 0;
  dlQuietRun = 0;
  dlSegPeak = 0;
  dlInQuiet = false;
  dlCutPhase = 0;
  dlStarved = false;
  dlFeedDone = false;
  dlToStreak = 0;
#if PLAY_ACK_LOCK
  dlRestarts = 0;  // ★V2.1.6：每场播放的重开会话次数（上限 3）
#endif
  dlShortAcks = 0;  // ★V2.1.7：本场短收次数（多帧在途安全性的判据）
  dlShort2s = 0;
  dlSentDbgPrev = 0;
  dlAckDbg = 0;
  dlAckNo = 0;
  dlStMin2s = 255;
  dlRtxOverflow = 0;
  // ★V2.2.8 音量：严格对齐 One 的起播序列 —— 0x14 音量 → 0x10/00 停录 → WAV 头 → PCM。
  // ★V2.2.9：播放中按音量键会在帧间隙补发 0x14（volPushOne），起播这条仍是第一发。
  //   不等应答：它的 0x91 0x14 会被 pumpPlay 的 ACK 解析按"h1 != 0x15"丢弃重同步，安全。
  while (coUart.available()) coUart.read();  // 清上一场残留，避免应答错位
  {
    uint8_t v[5] = { 0x90, CMD_VOLUME, 0x01, volHwOf(volTarget), 0 };
    v[4] = (uint8_t)(v[0] + v[1] + v[2] + v[3]);
    coUart.write(v, 5);
    volHwStep = volHwOf(volTarget);
    volPushPend = false;  // 已按最新档位发过，不再重复
  }
  // 播放启动指令（阻塞一次性 ~几ms）
  uint8_t s[5] = { 0x90, CMD_CTL, 0x01, 0x00, 0 };
  s[4] = (uint8_t)(0x90 + CMD_CTL + 0x01);
  if (sendWait(s, 5) < 0) {
    dlActive = false;
    dlTail = dlHead;
    dlRetryAt = millis() + 2000;
    Serial.println("[DL] 协控播放启动无响应，2 秒后再试");
    return;
  }
  Serial.println("[DL] 播放开始（流控泵运转，TTS 流式喂入）");
}

static void endDlPlay(const char* why) {
  dlActive = false;
  dlIsLocal = false;   // ★V2.4.7：这一场结束，之后的场次重新判
  dlWantStart = false;
  dlPendCnt = 0;
  dlRtxLen = 0;
  dlRtxOff = 0;
  dlCutPhase = 0;
  dlStarved = false;
  dlTail = dlHead;
  dlPreIdx = 44;
  lastConvAt = millis();
  uint32_t ms = millis() - dlStartAt;
  uint32_t bps = ms ? (uint32_t)((uint64_t)dlSentTotal * 1000 / ms) : 0;
  Serial.printf("[DL] 播放结束(%s): %lu 字节 / %lu ms（%lu B/s，%lu=实时）| 分段 %lu 次 | 回填 %lums#%lu | 溢出丢样 %lu | 短收 %lu 在途 %u\n",
                why,                 (unsigned long)dlSentTotal, (unsigned long)ms, (unsigned long)bps,
                (unsigned long)dlRate, (unsigned long)dlSegResets,
                (unsigned long)dlFillTotal, (unsigned long)dlFillCount,
                (unsigned long)dlOver, (unsigned long)dlShortAcks,
                (unsigned)DL_INFLIGHT_MAX);
  // ★V2.4.4：本地流每收一次场就把原因发给媒体库 —— 串口拿不到日志，这是唯一线索。
  //   mLibState 还非 0 = 收场时取流仍挂着（真正的"意外收场"）；用户按 C 暂停那条
  //   已经先 mLibStop 把 mLibState 清 0 了，不会重复上报。
  //   ★V2.4.7 dlEndQuiet：确认语说完让位给本地流，这不是"意外收场"，别污染 devlog
  //   （mLibLog 是阻塞 HTTP，最坏 600ms，紧跟着就要起播）。
  if (mLibState && !dlEndQuiet) mLibLog("DEND", why, (long)dlSentTotal, (long)ms);
#if DL_EXP1_WATER
  // ★实验① 证据行：水位上限是否真的在起约束（dlMbGate>0 = 泵在等协控播掉，批式生效）
  Serial.printf("[DL] 实验① 浅水位: 起播预滚 %lums / 投喂水位上限 %lums | 水位跳过 %lu 次"
                " | 批大小 %u B（官方 200B）\n",
                (unsigned long)DL_JITGATE_MS, (unsigned long)DL_AHEAD_MAX_MS,
                (unsigned long)dlMbGate, (unsigned)PLAY_CHUNK);
#endif
#if DL_EXP2_SEG
  // ★实验② 证据行：段界是否真按 2.4s 定时切 + 每次分段的代价（停在协控播自己缓冲=非静音）
  Serial.printf("[DL] 实验② 主动分段: 段长上限 %lums / 饿透余量 %ldms | 触发 %lu 次"
                " | 停发 平均 %lums / 最长 %lums | ★实测最长段长 %lums / 越过环长 %lu 次"
                " | 静音垫 %ldms | 缝落静音 %lu 次\n",
                (unsigned long)DL_EXP2_SEG_MS, (long)DL_EXP2_DRAIN_MS,
                (unsigned long)dlExp2Segs,
                (unsigned long)(dlExp2Segs ? dlExp2StopSum / dlExp2Segs : 0),
                (unsigned long)dlExp2StopMax,
                (unsigned long)dlSegMaxMs, (unsigned long)dlWrapCross,
                (long)DL_EXP2_PAD_MS, (unsigned long)dlExp2Quiet);
#endif
  // ★V2.3.0：播放结束必须把音量图案收掉 —— 否则 1.2s 显示保护会把清屏挡回去，
  //   末尾按过键的挂起柱状图还会在结束后再点亮一次（用户看到"播完了图案不灭"）。
  volShowPend = false;
  volIconAt = 0;
  // 播放结束灭屏（用户指定）：覆盖任何挂起意图（包括播放中缓存的表情）
  // ★V2.1.0 例外：服务器"响应未完成就断连"（故事被打断）⇒ 打一张失望脸。
  //   播放期点阵被总线仲裁禁发，只有这里（dlActive 刚置 false、总线空出来）能上屏；
  //   对用户而言这是"为什么没声了"的唯一可见答案，比静悄悄灭屏有用得多。
  // ★V2.4.7 dlEndQuiet：确认语说完只是"让位"，屏幕要留给下一拍的播放图案 ——
  //   这里既不清屏也不发帧（一次 mtxSend 要阻塞 ~400ms 渲染，紧跟着起播会被拖慢）。
  if (dlEndQuiet) return;
  if (dlBroken) {
    dlBroken = false;
    mtxFace = MTX_P_FROWN;
    mtxWant = MTX_I_FACE;
  } else {
    mtxWant = MTX_I_CLEAR;
  }
  mtxFlush();  // 总线回到空闲：灭屏（清残显）
}

// 硬打断：清 FIFO+清 RX（协控把已收数据播完自然停，无播放停止指令——官方 stopPlay
// 同款语义：interrupt() 只停本地喂帧，不发 UART 停止命令）。★置打断封锁：服务器
// abort 生效前在途的音乐帧会继续涌入，没有封锁就会重新攒满 FIFO 复活播放。
static void stopDlPlayHard() {
  if (!dlActive) return;
  dlActive = false;
  dlIsLocal = false;   // ★V2.4.7
  dlWantStart = false;
  dlBarged = true;
  dlPendCnt = 0;
  dlRtxLen = 0;
  dlRtxOff = 0;
  dlCutPhase = 0;
  dlStarved = false;
  dlTail = dlHead;
  dlPreIdx = 44;
  volShowPend = false;  // ★V2.3.0：打断也要收掉挂起的音量图案
  volIconAt = 0;
  while (coUart.available()) coUart.read();
  Serial.println("[DL] TTS 已打断");
}

// 非阻塞流控泵：窗口内"发帧→收ACK→未收满重发"循环（在途语义，锁定 1 帧在途）
// ACK 在途时窗口内短轮询等待：若直接 return，下一圈 loop（wss TLS 开销 ~16ms）才收
// ACK → 吞吐掉到 12KB/s，TTS 断续。窗口内等待后每圈可完成 2-3 帧往返。
// ★V2.2.0：泵改由独立任务驱动（见 pumpTaskFunc），窗口只用来"发完一帧就走"，
// 不再靠长窗口在主循环里阻塞等待——主循环被 ws.loop() 的 TLS 读占满（实测
// ws 986ms/s、loop 4 圈/s），泵根本没有执行机会，ACK 结算被拖到 250ms/次。
static const uint32_t PLAY_WINDOW_MS = 6;
// ★V2.1.10：每圈 WS 轮询次数。库一次 loop 只消费一帧，本机主循环 12~15 圈/s，
// 单次轮询 ⇒ 13 帧/s（18.7KB/s）追不上 24KB/s 实时播放。8 次留足余量（≈100 帧/s）。
#define WS_POLL_PER_LOOP 2

#if PLAY_ACK_LOCK
// ★V2.1.6：协控"完全不吃数据"时的会话重启（0x10 启动 + 重推 WAV 头 + 基线重锚）。
// 与"饿透复位"的区别：这个由 **ACK 断流** 触发（6 次 × RESP_TIMEOUT ≈600ms 零应答），
// 不靠水位估算，因此不会因为估算漂移在语音中间误切一刀。
// 上限 3 次：真僵死（只能断电那种）再重开也没用，别无限刷。
static void restartCoSession(const char* why) {
  Serial.printf("[DL] ⚠️ 协控 %s → 重开会话 #%u（已确认 %lu B / FIFO %u B）\n",
                why, (unsigned)(dlRestarts + 1), (unsigned long)dlSentTotal,
                (unsigned)dlAvail());
  uint8_t s[5] = { 0x90, CMD_CTL, 0x01, 0x00, 0 };
  s[4] = (uint8_t)(0x90 + CMD_CTL + 0x01);
  sendWait(s, 5);
  dlRestarts++;
  memcpy(dlPre, wavHdr, 44);
  dlPreIdx = 0;      // 头走前置通道，保证先于 PCM 上线
  dlSegSent = 44;
  dlPendCnt = 0;
  dlRtxLen = 0;
  dlRtxOff = 0;
  dlToStreak = 0;
  dlCutPhase = 0;
  dlStarved = false;
  dlQuietRun = 0;
  dlSegPeak = 0;
  dlInQuiet = false;
  dlStartAt = millis();
  mLibAckBase += dlSentTotal;   // ★V2.4.11：重锚不丢已播累计
  dlSentTotal = 0;   // 基线重锚：此刻协控缓冲按空处理
  dlSentDbgPrev = 0; // ★V2.1.9：同步统计基准，否则 2s 行速率下溢成天文数字
}
#endif

static void pumpPlay() {
  if (!dlActive) return;
  uint32_t t0 = millis();
  while (dlActive && millis() - t0 < PLAY_WINDOW_MS) {
    // ---- 流水线 ACK 结算（非阻塞：UART 里够 3 字节就结算队首帧）----
    while (dlPendCnt > 0 && coUart.available() >= 3) {
      uint8_t h0 = coUart.read(), h1 = coUart.read(), h2 = coUart.read();
      if (h0 != 0x91) continue;                    // 杂散字节：丢弃重同步
      if (h1 == CMD_VOLUME) { volAckLast = h2; continue; }  // ★V2.2.3 音量应答，记账后跳过
      if (h1 != CMD_PLAY) continue;                // 点阵应答 [91 08 xx] 等：跳过（定长 3B 不错位）
      dlToStreak = 0;  // 协控还在应答 ⇒ 看门狗清零
      uint8_t st = h2;
      uint8_t remAsk = (uint8_t)(dlPendLen[0] - dlPendGot[0]);
      if (st > remAsk) st = remAsk;
      // ★V2.1.7 诊断：前 12 条应答原样打出来（sent/st/延迟），用来判定协控的
      // 实际吸收速率——若 st 长期 < sent，说明它在限流，吞吐被它钳住，加帧没用。
      // ★V2.1.8：稳态也要有样本（每 200 条抽 1 条），否则只看得到开头满收的假象。
      dlAckNo++;
      if (st < dlStMin2s) dlStMin2s = st;
      if (dlAckDbg < 12 || (dlAckNo % 200) == 0) {
        dlAckDbg++;
        Serial.printf("[ACK] #%lu sent=%u st=%u lat=%ums 在途=%u rtx=%u/%u\n",
                      (unsigned long)dlAckNo, (unsigned)dlPendLen[0], (unsigned)st,
                      (unsigned)(millis() - dlPendAt[0]), (unsigned)dlPendCnt,
                      (unsigned)(dlRtxLen - dlRtxOff), (unsigned)sizeof(dlRtx));
      }
      dlPendGot[0] += st;
      dlSentTotal += st;
      dlSegSent += st;  // 分段流式：距上次 WAV 头的 ACK 字节数
      if (dlPendGot[0] < dlPendLen[0]) {
        // 短收：协控只吃下 st 字节 ⇒ 未确认的尾巴进重发缓冲，优先补发。
        // ★V2.1.8：不再 `dlTail -=` 回退 FIFO 读指针——在途多帧时那会退到最新帧
        // 身上（丢前帧尾巴 + 重复最新帧尾巴）。ARQ 副本保证零丢失零重复，
        // 代价只是尾巴比在途帧晚到 ≤ 在途字节（≤400B≈16ms）。
        dlShortAcks++;
        dlShort2s++;
        rtxPush(dlPendBuf[0] + dlPendGot[0],
                (uint16_t)(dlPendLen[0] - dlPendGot[0]));
      }
      for (uint8_t i = 1; i < dlPendCnt; i++) {  // 出队（数组前移）
        dlPendLen[i - 1] = dlPendLen[i];
        dlPendGot[i - 1] = dlPendGot[i];
        dlPendAt[i - 1] = dlPendAt[i];
        memcpy(dlPendBuf[i - 1], dlPendBuf[i], PLAY_CHUNK);
      }
      dlPendCnt--;
    }
    // 队首超时（协控整帧未回 ACK）→ 视为整帧丢失，未确认部分进重发缓冲后出队
    if (dlPendCnt > 0 && millis() - dlPendAt[0] > RESP_TIMEOUT) {
      if (dlPendGot[0] < dlPendLen[0]) {
        rtxPush(dlPendBuf[0] + dlPendGot[0],
                (uint16_t)(dlPendLen[0] - dlPendGot[0]));
      }
      for (uint8_t i = 1; i < dlPendCnt; i++) {
        dlPendLen[i - 1] = dlPendLen[i];
        dlPendGot[i - 1] = dlPendGot[i];
        dlPendAt[i - 1] = dlPendAt[i];
        memcpy(dlPendBuf[i - 1], dlPendBuf[i], PLAY_CHUNK);
      }
      dlPendCnt--;
      // ---- v9.17 协控不应答看门狗 ----
      // 健康场次实测超时恒 0，"连续 30 次零应答"≈3s 只可能是协控僵死。主动收场：
      // 本场 TTS 截断（协控本来也没在播），设备立即回到可响应态，下一句 TTS
      // 会重新走一次 startDlPlay。
      if (dlToStreak < 255) dlToStreak++;
#if PLAY_ACK_LOCK
      // ★V2.1.6：600ms 一个字节都不吃 ⇒ 播放任务大概率已终结（欠载后协控自己停了）。
      // 重开会话续播，而不是把整段故事判死收场（旧行为要等 30 次 ≈3s 才收场，
      // 长故事里表现为"讲到一半突然没声、而且再也不回来"）。
      // ★V2.4.3：本地媒体库场次放宽到 30 次重开 —— 一本 20 分钟的书里，协控偶发僵死
      //   2~3 次很常见，用旧的 3 次上限会在第 4 次直接把整场判死（= "听一半没了"）。
      //   云端 TTS 仍用 3 次（它每句都重开会话，没必要一直硬撑）。
      if (dlToStreak >= 6 && dlRestarts < (mLibState ? 30 : 3)) {
        restartCoSession("连续 6 次 ACK 超时");
        return;
      }
#endif
      if (dlToStreak >= DL_TO_STREAK_MAX) {
        Serial.printf("[GUARD] ⚠️ 协控不应答看门狗触发：连续 %u 次 ACK 超时（≈%ums 零应答）"
                      "——判协控僵死，主动收场本场（已确认 %lu B / FIFO 剩 %u B）\n",
                      (unsigned)dlToStreak,
                      (unsigned)(DL_TO_STREAK_MAX * RESP_TIMEOUT),
                      (unsigned long)dlSentTotal, (unsigned)dlAvail());
        endDlPlay("协控不应答");
        return;
      }
    }
    // 在途已满：直接让出（★V2.2.0）。泵跑在独立任务里，1ms 后自动再来 tick，
    // 忙等只会把 WS 读取和 WiFi 任务饿死（C3 单核）。
    if (dlPendCnt >= DL_INFLIGHT_MAX) {
      return;
    }
    // ★V2.2.9：走到这里说明在途帧已全部结算（dlPendCnt==0）⇒ 帧间隙。
    // 音量命令可以在这个缝里插一条 5 字节，不会切在 PCM 帧中间。
    volPushOne();

    int32_t aheadNow = (int32_t)dlSentTotal
                       - (int32_t)((uint64_t)(millis() - dlStartAt) * dlRate / 1000);
    bool gapAlign = dlLastAudioAt != 0 && millis() - dlLastAudioAt >= 400;
    // 对齐条件"入流间隙 ≥400ms"：实锤句间停顿服务器不发帧，真句界没有静音帧可等；
    // 400ms 阈值：节流间隙 150-350ms 不误触，句间停顿 ≥450ms 必命中
    // ⚠️ 对齐检查必须在积压钳制之前——钳制 return 会跳过本检查

    // ---- v9.5.3 积压趋势采样（每 500ms）+ 死亡指纹复活 ----
    // 会话重锚后 800ms 内不采样。死亡指纹：连续 2 次采样 B' > 8000 B/s（活时上限
    // ~2150，死亡=播放冻结、泵照发 → B'≈+22050）→ 0x10+推头直启。
    // 死亡=缓冲排空+任务终结，协控缓冲本就空 → 无需停发饿透，直接重锚。
    {
      uint32_t tnow = millis();
      if (tnow - dlStartAt >= 800) {
        if (dlBTrendAt == 0) {
          dlBTrendPrev = aheadNow;
          dlBTrendAt = tnow;
          dlDeathStreak = 0;
        } else if (tnow - dlBTrendAt >= 500) {
          dlBTrend = (int32_t)((int64_t)(aheadNow - dlBTrendPrev) * 1000
                               / (int32_t)(tnow - dlBTrendAt));
          dlBTrendPrev = aheadNow;
          dlBTrendAt = tnow;
          if (dlBTrend > 8000) {
            dlDeathStreak++;
            if (dlDeathStreak >= 2 && dlCutPhase == 0 && !dlFeedDone && !ttsDone) {
              dlDeathStreak = 0;
              dlBTrendAt = 0;
              dlBTrend = 0;
              uint8_t s5[5] = { 0x90, CMD_CTL, 0x01, 0x00, 0 };
              s5[4] = (uint8_t)(0x90 + CMD_CTL + 0x01);
              sendWait(s5, 5);  // 0x10 重启播放任务（死亡=任务终结，等同冷启动）
              memcpy(dlPre, wavHdr, 44);
              dlPreIdx = 0;
              dlSegSent = 44;
              dlSegResets++;
              dlQuietRun = 0;
              dlSegPeak = 0;
              dlInQuiet = false;
              dlStarved = false;
              dlStartAt = millis();
              mLibAckBase += dlSentTotal;   // ★V2.4.11
              dlSentTotal = 0;  // 重锚：死亡=缓冲已排空，此刻=零点
        dlSentDbgPrev = dlSentTotal;
              Serial.printf("[DL] 死亡指纹复活 #%lu（积压增速 %+ld B/s > 8000 = 播放已冻结，0x10+推头直启）\n",
                            (unsigned long)dlSegResets, (long)dlBTrend);
              return;
            }
          } else {
            dlDeathStreak = 0;
          }
        }
      } else {
        dlBTrendAt = 0;
        dlBTrend = 0;
        dlDeathStreak = 0;
      }
    }

    // ---- 会话硬顶（保险丝，120s）：等静音/间隙对齐再切，超时 2s 强切 ----
    // 缝落在静音/停顿里≈零损失；缝落语音中=协控重生时重放缝前区域=吞字/重播。
    bool dlStreamAlive = dlLastAudioAt != 0 && millis() - dlLastAudioAt < 1500;
    // 1500ms 流存活门槛：断连/流结束后 dlLastAudioAt 停更 → gapAlign 恒真 → 段界
    // 无限复位循环。句间正常停顿 400-552ms < 1500ms 不受影响。
    bool hardHit = dlSegSent >= dlSegHard
                   && ((dlInQuiet && dlQuietRun >= MS2B(40)) || gapAlign
                       || dlSegSent >= dlSegHard + MS2B(2000));
    // ★V2.1.6：ACK-LOCK 模式下关闭 120s 硬顶分段——连续流不该被人为切缝。
    if (!PLAY_ACK_LOCK && dlCutPhase == 0 && !dlFeedDone && dlStreamAlive && hardHit) {
      dlCutPhase = 1;
      dlCutPhaseAt = millis();
      Serial.printf("[DL] 段界 %lu：硬顶兜底（played %lums）——饿透复位\n",
                    (unsigned long)(dlSegResets + 1),
                    (unsigned long)((uint64_t)(dlSegSent - aheadNow) * 1000 / dlRate));
      return;
    }

#if DL_EXP2_SEG
    // ---- ★实验②：主动定量分段（写指针绕到 2.73067s 之前先复位）----
    // 连续流下原有触发器全部失效（见 EXP2 说明），这里按"距上次推头的写入量"定量切段。
    // 阈值用 MS2B（= dlRate 归一），所以在 16k/24k 下行下都等于"2.4s 的内容"、恒 < 65536 样本。
    // ★EXP2b：门里去掉 !dlFeedDone/!ttsDone。点歌（MCP 流式音乐）时服务器常按块发
    //   `tts stop`（或整段只发一次 start），这两个标志会在**播放中**就变 true ⇒ 旧门
    //   把主动分段彻底锁死 ⇒ 绕圈照旧。真正的存活判据是 dlStreamAlive（1.5s 内有音频）。
    // ---- ★EXP3 段末静音垫：把"缝前区域"换成数字静音（复位缝重放的就是静音）----
    if (DL_EXP2_PAD_MS > 0 && dlCutPhase == 0 && !dlPadDone
        && dlSegSent >= MS2B(DL_EXP2_AUDIO_MS)) {
      dlPadDone = true;
      dlPadAtMs = millis();
      int32_t room = (int32_t)DL_SIZE - (int32_t)dlAvail() - 8;
      int32_t want = MS2B(DL_EXP2_PAD_MS);
      int32_t n2 = (room < want) ? room : want;
      for (int32_t k = 0; k < n2; k++) fifoPush(0x80);
      Serial.printf("[DL] 段末静音垫 #%lu：入队 %ldms 静音（段已推 %lums / FIFO 余量 %ld B）\n",
                    (unsigned long)(dlExp2Segs + 1),
                    (long)((int64_t)n2 * 1000 / (int32_t)dlRate),
                    (unsigned long)((uint64_t)dlSegSent * 1000 / dlRate), (long)room);
    }
    // ---- 定量切段：①天然静音缝（零代价）优先 ②垫静音已上线 ③段长硬线（无垫时）----
    bool exp2QuietSeam = (DL_EXP2_QUIET_RUN_MS > 0 && dlSegSent >= MS2B(DL_EXP2_QUIET_MIN_MS)
                          && dlInQuiet && dlQuietRun >= MS2B(DL_EXP2_QUIET_RUN_MS));
    bool exp2Ready = (DL_EXP2_PAD_MS == 0)
                       ? (dlSegSent >= MS2B(DL_EXP2_SEG_MS))
                       : (dlPadDone && (dlAvail() == 0 || millis() - dlPadAtMs >= 800));
    if (dlCutPhase == 0 && dlStreamAlive && (exp2QuietSeam || exp2Ready)) {
      dlCutPhase = 1;
      dlCutPhaseAt = millis();
      dlExp2Segs++;
      if (exp2QuietSeam) dlExp2Quiet++;
      Serial.printf("[DL] 分段触发 #%lu：本段已推 %lums（%lu B / 环长 2730ms）——%s\n",
                    (unsigned long)dlExp2Segs,
                    (unsigned long)((uint64_t)dlSegSent * 1000 / dlRate),
                    (unsigned long)dlSegSent,
                    exp2QuietSeam ? "缝落天然静音（零代价）" : "缝前已垫静音");
      return;
    }
    // ---- ★EXP2b 绕圈探针：本段推量越过环长仍未切段 ⇒ 分段没生效（绕圈必已发生）----
    // 这是"分段到底有没有干活"的唯一硬指标：正常时 dlSegSent 被 2400ms 截断，永不到 2730。
    {
      uint32_t segMsNow = (uint32_t)((uint64_t)dlSegSent * 1000 / dlRate);
      if (segMsNow > dlSegMaxMs) dlSegMaxMs = segMsNow;
      if (segMsNow < 1000) {
        dlWrapAlarm = false;   // 新段开始，重新武装
        dlPadDone = false;     // ★EXP3：静音垫按段自武装（不依赖 startDlPlay）
      } else if (segMsNow >= 2730 && !dlWrapAlarm) {
        dlWrapAlarm = true;
        dlWrapCross++;
        Serial.printf("[DL] ⚠️ 越过环长仍未分段：本段已推 %lums > 2730ms"
                      "（门: 流存活=%d 已喂完=%d tts结束=%d | 停发态=%d）——绕圈必已发生\n",
                      (unsigned long)segMsNow, (int)dlStreamAlive,
                      (int)dlFeedDone, (int)ttsDone, (int)dlCutPhase);
      }
    }
#endif

    // ---- 积压钳制 = 投喂水位上限（实验① 的核心变量）----
    // 原 4600ms 只是"极端兜底"，从不生效；实验① 压到 200ms（官方批大小量级）
    // ⇒ 泵只在协控剩余 <1 批时才补货 = 官方"在途 ≤1 批 + 缓冲浅"的同构形态。
    // 泄压阀：FIFO 将满时解除钳制（防 FIFO 溢出丢样 + burst 路径死循环→WDT）
    // ★实验②：dlCutPhase!=0 时不得拦截——停发/复位状态机在下面，被拦=分段卡死。
    const int32_t aheadLimit = MS2B(DL_AHEAD_MAX_MS);
    if (dlCutPhase == 0 && aheadNow > aheadLimit && dlAvail() < DL_SIZE - PLAY_CHUNK - 1) {
      dlMbGate++;  // 实验①证据：协控还有货 ⇒ 本圈不投喂（批式节拍）
      return;
    }

    // ---- 饥饿复位状态机：停发 → 协控播空（饿透）→ 推头 → 复供 ----
    // 停发期协控播的是它自己缓冲里的货（不是丢弃），故"停发时长"≈进复位时的在途水位；
    // 真正的静音 = 播空到新头到货那段窗口 ≈ DL_EXP2_DRAIN_MS 余量（实验② 从 100ms 收到 20ms）。
    if (dlCutPhase == 1) {
      if (aheadNow <= -MS2B(DL_EXP2_DRAIN_MS) || millis() - dlCutPhaseAt > 1500) {
        uint32_t segMs = (uint32_t)((uint64_t)dlSegSent * 1000 / dlRate);
        uint32_t stopMs = millis() - dlCutPhaseAt;
        int32_t pushAhead = aheadNow;
#if DL_EXP2_SEG
        dlExp2StopSum += stopMs;
        if (stopMs > dlExp2StopMax) dlExp2StopMax = stopMs;
#endif
        memcpy(dlPre, wavHdr, 44);
        dlPreIdx = 0;  // 头走前置通道（FIFO 里可能有已回填数据，压队尾=中途头）
        dlSegSent = 44;
        dlSegResets++;
        dlQuietRun = 0;
        dlSegPeak = 0;
        dlInQuiet = false;
        dlCutPhase = 0;
        dlStarved = false;
        dlStartAt = millis();
        mLibAckBase += dlSentTotal;   // ★V2.4.11
        dlSentTotal = 0;  // 重锚 aheadNow 基线：协控此刻播空=零点
        dlSentDbgPrev = dlSentTotal;
        Serial.printf("[DL] 段界 %lu：饥饿复位完成（段长 %lums / 停发 %ums / 推头时积压 %dms）\n",
                      (unsigned long)dlSegResets, (unsigned long)segMs,
                      (unsigned long)stopMs, (int)((int64_t)pushAhead * 1000 / dlRate));
      } else {
        return;  // 停发期：积压被协控"播掉"而非清掉，零丢失
      }
    }

    // ---- 流内静音回填：吸收服务器断供（V2.0.2 默认关闭，见 DL_FILL_ENABLE）----
    // 旧"流内深静音"条件在这台服务器永假（句间停顿服务器不发帧）；真信号 =
    // "FIFO 空 + 服务器停供 ≥150ms"（或流内深静音 ≥250ms，防其他服务器形态）。
    if (DL_FILL_ENABLE) {
      uint32_t fnow = millis();
      int32_t fahead = (int32_t)dlSentTotal
                       - (int32_t)((uint64_t)(fnow - dlStartAt) * dlRate / 1000);
      bool fillNeed = fahead < dlFillTarget;  // 含负水位（负=断供已抽干，正是要救的点）
      bool quietNow = dlInQuiet && dlQuietRun >= MS2B(DL_FILL_QUIET);
      bool gapNow = dlLastAudioAt != 0 && (fnow - dlLastAudioAt) >= DL_FILL_GAP;
      if (fillNeed && dlCutPhase == 0 && !ttsDone
          && !dlFeedDone && !dlStarved
          && (quietNow || gapNow)
          && fnow - dlStartAt > 500 && dlAvail() == 0
          && fnow - dlFillLastAt >= DL_FILL_EVERY) {
        dlFillLastAt = fnow;
        int32_t room = dlFillTarget - fahead;
        uint32_t pushN = MS2B(DL_FILL_CHUNK);
        if (room < (int32_t)pushN) pushN = (room > 0) ? (uint32_t)room : 0;
        while (pushN > 0 && dlAvail() < DL_SIZE - PLAY_CHUNK - 1) {
          fifoPush(0x80);
          pushN--;
        }
        dlFillTotal += DL_FILL_CHUNK;
        dlFillCount++;
        Serial.printf("[DL][FILL] 静音回填 #%lu：推 %lums（积压 %ldms / 目标 %ldms，%s %lums，累计 %lums）\n",
                      (unsigned long)dlFillCount, (unsigned long)DL_FILL_CHUNK,
                      (long)((int64_t)fahead * 1000 / dlRate),
                      (long)((int64_t)dlFillTarget * 1000 / dlRate),
                      gapNow ? "断供" : "静音",
                      (unsigned long)((uint64_t)(gapNow ? (fnow - dlLastAudioAt) : dlQuietRun) * 1000 / dlRate),
                      (unsigned long)dlFillTotal);
      }
    }

    // ---- 发送优先级：ARQ 重发缓冲 > 前置头 > FIFO 新数据 ----
    // 未确认的语音尾巴必须紧跟着原帧补发（官方 0x91 流控语义的直接推论）；
    // 若排到新数据后面，协控就会把音节末尾切掉 8~16ms（听感=吞字）。
    uint16_t availAll = (uint16_t)(dlAvail() + (44 - dlPreIdx));
    bool fromRtx = (dlRtxOff < dlRtxLen);
    uint16_t pendBytes = fromRtx ? (uint16_t)(dlRtxLen - dlRtxOff) : availAll;
    uint8_t n = (uint8_t)(pendBytes > PLAY_CHUNK ? PLAY_CHUNK : pendBytes);
    if (!fromRtx && n == 0) {
      // 流结束判定（协议优先）：tts stop 已到 + FIFO 空 + 协控基本播完（积压<100ms）才收尾。
      // ⚠️ 曾按空闲 1500ms 硬猜：句间合成停顿 1.5-2s 常见 → 误触发双重断口。
      uint32_t now = millis();
      int32_t ahead = (int32_t)dlSentTotal
                      - (int32_t)((uint64_t)(now - dlStartAt) * dlRate / 1000);
      // 饿透标记：FIFO 空 + 协控积压 ≤50ms ⇒ 饿透在即/已发生。不立即推头（长停顿
      // 会把新会话也饿死），置标记等数据恢复瞬间再推——重启成本全部落在停顿里=免费。
      // 开播 500ms 内不标记：预滚灌入期 FIFO 瞬时见底属正常（防开机双重推头）。
      // ★★V2.4.11：本地媒体库流**不进饿透状态机**。它是连续 PCM 字节流、没有
      //   "会话边界"可重开；一次网络抖动就重推 44B WAV 头，既把断口切进音频里，
      //   又清零 dlSentTotal 让实播少算（见 mLibAckBase 注释）。只补水、不重开。
      if (!mLibState && !ttsDone && !dlFeedDone && !dlStarved && ahead <= MS2B(50)
          && now - dlStartAt > 500) {
        dlStarved = true;
        dlStarveMarkAt = now;
        Serial.printf("[DL] 协控饿透标记（积压 %dms / FIFO 空）——数据恢复时重推头\n",
                      (int)((int64_t)ahead * 1000 / dlRate));
      }
      // ★★V2.4.3 本地媒体库场次（mLibState != 0）：下面两条"云端收场判据"必须跳过。
      //   本地流的 PCM 直接从 HTTP socket 进 FIFO，**不经过 ws、也不依赖 TTS 状态**：
      //   ① ttsDone 是"确认语说完"的残留，播放中恒 true；水位控制下 FIFO 满时本来就
      //     0.5~1.5s 不读 socket ⇒ 随机撞上就 endDlPlay("tts结束")。
      //   ② 播放期本机不发上行音频，官方服务器约 80s 踢掉 ws ⇒ wsReady=false，之后
      //     任何一次积压掉到 50ms 以下就 endDlPlay("连接已断")。
      //   两者都表现为"听几分钟自己停了"。本地流合法结束只有 mLibEnded（EOF）和
      //   mLibTick 的 15s 无字节兜底，这里一律不管。
      if (!mLibState) {
        // tts stop 后仍有尾包迟到（实测 10 帧/1.8s），等 1500ms 让尾包进 FIFO 播完
        if (ttsDone && now - dlLastAudioAt > 1500 && ahead < MS2B(100)) {
          endDlPlay("tts结束");
          return;
        }
        // ---- ★V2.0.9 兜底分级：先看"链路死没死"，再谈"服务器卡了多久" ----
        // 链路已断 + 协控也播空 ⇒ 会话真的没了，立刻收场（旧版在这里白等 5s）。
        // 协控还有积压（ahead ≥50ms）则继续等它播完，别把已收的尾巴掐掉。
        if (!wsReady && ahead < MS2B(50)) {
          endDlPlay("连接已断");
          return;
        }
      }
      if (now - dlLastAudioAt > DL_STALL_MAX_MS) {  // 连接仍在：给足恢复时间
        endDlPlay("流结束兜底");
        return;
      }
      // 停供打点（每 2s）：长句"大段没声音"一行定责——协控有积压=发送链断；
      // 协控空+FIFO 空=服务器不发了；再带上 WiFi/WS/堆，把"服务器不发"与
      // "链路掉了 / 固件内存枯了"当场分开（本次实测的"讲着讲着没声"就是这一类）。
      if (now - dlStarvePrintAt > 2000) {
        dlStarvePrintAt = now;
        uint32_t gap = now - dlLastAudioAt;
        // ★★V2.4.11 分场合打点：云端 TTS 场次才看 WS/pong/最大帧（V2.1.4 三件套）；
        //   本地媒体库流的 PCM 走 HTTP socket、根本不经 ws —— "⚠️ 服务器停供 /
        //   距上次 pong 796s / WS 断开"全是噪声，会把排障带到"云端链路断了"的错误方向
        //   （2026-11-11 那次就是这么误判的）。改打 socket 可读字节、距上次收字节、
        //   FIFO、RSSI，一眼分清"上游没产/没送到"与"已到货待喂"。
        if (mLibState) {
          Serial.printf("[DL][本地流] 断粮 %ums（FIFO %uB / socket 可读 %dB / 距上次收字节 %lums"
                        " | WiFi %d RSSI %d | 协控积压 %dms | 主循环 %lu 圈/s | 本流已收 %lu B）\n",
                        gap, dlAvail(), (int)mLibSockAvail,
                        (unsigned long)(now - mLibBufAt),
                        (int)WiFi.status(), (int)WiFi.RSSI(),
                        (int)((int64_t)ahead * 1000 / dlRate),
                        (unsigned long)loopRate, (unsigned long)mLibBytes);
        } else {
          Serial.printf("[DL] %s %ums（协控积压 %dms / FIFO %uB | WiFi %d RSSI %d | WS %s | "
                        "堆 %u 连续 %u | 栈余 %u | 本会话最大帧 %u B%s）\n",
                        gap > DL_STALL_WARN_MS ? "⚠️ 服务器停供" : "断粮", gap,
                        (int)((int64_t)ahead * 1000 / dlRate), dlAvail(),
                        (int)WiFi.status(), (int)WiFi.RSSI(), wsReady ? "连接" : "断开",
                        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                        (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)wsRxMaxAll,
                        (wsRxMaxAll + 1 > heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
                          ? " ⚠️顶到连续块" : "");
          Serial.printf("[DL]   链路: 主循环 %lu 圈/s | DMA 堆 %u 连续 %u | 距上次 pong %ldms"
                        "（ping 发 %lu / 收 ping %lu / 收 pong %lu）\n",
                        (unsigned long)loopRate,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                        (long)(wsLastPongAt ? (int32_t)(millis() - wsLastPongAt) : -1),
                        (unsigned long)wsPingTx, (unsigned long)wsPingRx, (unsigned long)wsPongRx);
        }
      }
      return;
    }
    // ---- 饿透恢复推头：标记后数据恢复（FIFO 有货）且协控确认播空 → 新会话 ----
    // aheadNow ≤ -50ms = 协控已空 ≥50ms（防把头推进仍有余量的活跃会话）；
    // 2000ms 超时兜底（基线估算漂移时强制推）。满水重启：推头前先攒 400ms——
    // 零水位起跑的新会话无法爬回高水位，第一个大发隙即排空死亡。
    if (!mLibState && dlStarved && dlCutPhase == 0 && !ttsDone) {
      bool drained = aheadNow <= -MS2B(50) || millis() - dlStarveMarkAt > 2000;
      bool enough = dlAvail() >= MS2B(400);
      if ((drained && enough) || (drained && millis() - dlStarveMarkAt > 2000)) {
        memcpy(dlPre, wavHdr, 44);
        dlPreIdx = 0;
        dlSegSent = 44;
        dlSegResets++;
        dlQuietRun = 0;
        dlSegPeak = 0;
        dlInQuiet = false;
        dlStarved = false;
        dlStartAt = millis();
        mLibAckBase += dlSentTotal;   // ★V2.4.11
        dlSentTotal = 0;  // 重锚基线：协控此刻播空=零点
        dlSentDbgPrev = dlSentTotal;
        Serial.printf("[DL] 段界 %lu：饿透恢复推头（标记后 %ums，FIFO %uB/%s）\n",
                      (unsigned long)dlSegResets, (unsigned long)(millis() - dlStarveMarkAt),
                      (unsigned)dlAvail(), enough ? "满水" : "超时强推");
      } else {
        return;  // 攒水中（或还差最后 ~100ms 播空），先不发
      }
    }
    if (fromRtx) {  // 重发：直接从缓冲取，不再消耗 FIFO、也不重复做静音判定
      memcpy(dlPay, dlRtx + dlRtxOff, n);
      dlRtxOff += n;
      if (dlRtxOff >= dlRtxLen) {
        dlRtxLen = 0;
        dlRtxOff = 0;
      }
    } else {
      for (uint8_t i = 0; i < n; i++) {
        // 取数优先级：头（新会话第一字节）→ FIFO 真数据
        uint8_t b;
        if (dlPreIdx < 44) b = dlPre[dlPreIdx++];
        else b = dlFifo[dlTail++ % DL_SIZE];
        // ★V2.2.7：数字通路 1:1 直通，不做软件增益（会抬底噪）。音量全交给协控 0x14。
        dlPay[i] = b;
      }
      {  // 帧停顿判定：特征=帧内 max|v-128|，≤ max(10, 段内峰值/4) 视为停顿帧。
        // 语音帧必有 >10 的样本（音节峰值），静音帧 ≤8；均值特征对极轻 TTS 无判别力。
        uint8_t fmax = 0;
        for (uint8_t i = 0; i < n; i++) {
          uint8_t v = dlPay[i];
          uint8_t d = (v > 128) ? (uint8_t)(v - 128) : (uint8_t)(128 - v);
          if (d > fmax) fmax = d;
        }
        if (fmax > dlSegPeak) dlSegPeak = fmax;
        if (fmax <= quietThr()) {
          dlQuietRun += n;
          dlInQuiet = true;
        } else {
          dlQuietRun = 0;
          dlInQuiet = false;
        }
      }
    }
    uint8_t frame[4 + PLAY_CHUNK];
    // 帧格式（实测锁定）：`90 15 <len> <data...> <checksum>`——len 字节必须保留
    //（删掉它 ⇒ 协控整帧拒收，全程无声，EXP30/31 两次验证）
    frame[0] = 0x90;
    frame[1] = CMD_PLAY;
    frame[2] = n;
    memcpy(frame + 3, dlPay, n);
    uint8_t sum = 0;
    for (uint8_t i = 0; i < n + 3; i++) sum += frame[i];
    frame[3 + n] = sum;  // 除末字节外全部累加 & 0xFF
    coUart.write(frame, n + 4);
    // 入在途队列（不等 ACK，下一圈立刻可再发一帧）
    dlPendLen[dlPendCnt] = n;
    dlPendGot[dlPendCnt] = 0;
    dlPendAt[dlPendCnt] = millis();
    memcpy(dlPendBuf[dlPendCnt], dlPay, n);  // ★V2.1.8：留副本，短收时按 st 补发尾巴
    dlPendCnt++;
  }
}

// ★V2.2.0 播放泵独立任务（解耦关键）
// 【为什么】实测 ws.loop() 占了主循环 986ms/s（TLS 读阻塞），主循环只有 4 圈/s。
//   泵寄生在 loop 里 ⇒ ACK 结算每 250ms 才做一次，一帧要等 250ms 才发下一帧，
//   吞吐被压在 1KB/s 级；而回调里泵又和 WS 读取串在一起，形成恶性循环。
// 【怎么做】把泵放到独立任务，1ms 一 tick：
//   · ACK 结算/发帧都在这里做，与 WS 读取互不阻塞（SSL 阻塞时自动让出 CPU）；
//   · 回调只写 FIFO + 挂 dlWantStart，绝不碰 UART（避免两个上下文抢同一串口）。
// 【注意】C3 单核，泵不能长窗口忙等，否则把 WS 任务饿死；所以 pumpPlay 在途满
//   直接 return，1ms 后再 tick（RTT 4ms ⇒ 4~5 tick 发一帧 ≈ 32KB/s > 实时 24KB/s）。
// ★ 教训（V2.2.3 翻车）：**空闲≠安全**。协控 RX 里的字节不止是"应答"——
//   录音时它就是麦克风 PCM（coPumpRecord 自己按 [81 11 n] 头解析）、播放时是 PCM 应答、
//   调点时是点阵应答。任何第三方任务去"顺手读两字节看看"，都会把别人的流撕开：
//   实测 coScanSideAcks 把录音 PCM 吃掉 ⇒ 上行 opus 0 帧 ⇒ 服务器当你在 silence ⇒
//   一声不回（"没声音了"），同时把 mtxSend 的应答偷走 ⇒ 帧永远不确认 ⇒ 主循环被
//   5×150ms 重试卡死 ⇒ "按音量也没反应"。
//   结论：**协控串口只有一个读主，谁用谁读，绝不旁路偷看。**
//   音量应答因此不做读取（0x14 是幂等设置，不依赖 ack），只在播放期间由 pumpPlay
//   的 3 字节应答解析顺带记账（那里天然是音频应答的唯一读主）。

static TaskHandle_t pumpTaskH = nullptr;
static void pumpTaskFunc(void*) {
  for (;;) {
    if (recPhase == 0) {
      volApplyPending();  // ★录音中绝不插命令（RX 是麦克风 PCM）
      volService();       // 音量下发（帧间隙）+ 柱状图（去抖/限流）
    }
    if (!dlActive && dlWantStart) {
      dlWantStart = false;
      startDlPlay();
    }
    if (dlActive) {
      dlPumpBusy = true;
      pumpPlay();
      dlPumpBusy = false;
    }
    // 音量柱状图到点恢复状态图标：播放中 mtxFlush 会拒发，这里用快速路径补一帧。
    // ★V2.4.7：恢复的一定是**当前该显示的东西** —— 本地流播放中就是 kind 图案，
    //   不能画 mtxWant（那个早被服务器的情绪脸改掉了，于是"音量一调，书变笑脸"）。
    if (volIconAt && millis() - volIconAt > VOL_ICON_MS) {
      volIconAt = 0;
      if (dlActive) {
        mtxSendFast(mtxPatternOf(mLibIconHold() ? mLibIconId() : mtxWant));
      } else {
        // ★V2.3.0：空闲时立刻补回（含清屏），不再只丢给 loop —— 播放刚好在这 1.2s 内
        //   结束时，旧写法会让柱状图一直挂到下一次状态变化，看着就是"播完了不灭"。
        mtxCur = 0xFF;
        mtxFlush();
      }
    }
    vTaskDelay(1);
  }
}
static void pumpTaskBegin() {
  if (pumpTaskH) return;
  xTaskCreate(pumpTaskFunc, "dlpump", 4096, nullptr, 5, &pumpTaskH);
}

// WS 下行 opus 帧：半双工丢包 → 剥帧头(宽松兼容) → 解码 → 帧内定点重采样 → 8bit 进 FIFO
static void handleAudio(uint8_t* payload, size_t len) {
  if (recPhase != 0) {
    dlDropped++;
    return;
  }  // 录音中丢下行（半双工）
  if (dlBarged) {
    dlDropped++;  // 打断封锁：旧歌/旧话残帧全丢（服务器 abort 生效前在途帧），防播放复活
    return;
  }
  // ★V2.4.7：本地媒体库场次 —— 云端残帧一个字节都不许掺进 FIFO（那条队列是共用的，
  //   掺进去就是在歌里插几个字，或者干脆把本地流挤得断续）。等确认语那段 dlIsLocal
  //   还是 false，云端那句"继续播放啦"照收，不受影响。
  if (dlIsLocal) {
    dlDropped++;
    return;
  }
  if (len < 2) return;
  {  // ★V2.1.9：入流节奏采样（判"上游慢"还是"我们读得慢"的唯一硬指标）
    uint32_t rn = millis();
    if (dlRxGapAt) {
      uint32_t g = rn - dlRxGapAt;
      if (g > dlRxGapMax) dlRxGapMax = g;
      uint8_t b = (g < 20) ? 0 : (g < 40) ? 1 : (g < 80) ? 2 : (g < 160) ? 3 : 4;
      dlGapHist[b]++;
    }
    dlRxGapAt = rn;
  }
  if (payload[0] != 0x01 || payload[1] != 0x01) {
    // 小智协议帧头 [0x01 版本][0x01 opus 类型]。部分 xiaozhi-esp32-server 旧版
    // 直接发裸 opus（无帧头）——严格校验会把下行全丢且零日志（TTS 无声！）。
    static bool hdrWarned = false;
    if (!hdrWarned) {
      hdrWarned = true;
      Serial.printf("[DL] 下行帧无协议帧头(0x%02X 0x%02X)，按裸 opus 兼容解码（旧版服务器?）\n",
                    payload[0], payload[1]);
    }
  } else {
    payload += 2;
    len -= 2;
  }
  if (!dec || len == 0) return;
  int n = opus_decode(dec, payload, (opus_int32)len, decBuf, DEC_BUF_SAMPLES, 0);
  if (n <= 0) {
    dlDecErr++;
    static uint32_t lastDecErrAt = 0;
    if (millis() - lastDecErrAt > 2000) {  // 节流：2s 一条
      lastDecErrAt = millis();
      Serial.printf("[DL] opus 解码失败（累计 %lu，最近 err=%d，解码器 %dHz）"
                    "——检查 hello 的 audio_params.sample_rate 与实际是否一致\n",
                    (unsigned long)dlDecErr, n, decRate);
    }
    return;
  }
  dlFrames++;
  dlBytes += len;
  dlLastAudioAt = millis();

  // 帧内重采样（跨帧无状态）：输出 k 对应输入位置 k*decRate/dlRate
  // 线性插值（原最近邻跳取会产生镜像高频毛刺）；降采样加三点盒式均值抗混叠
  const uint64_t step = ((uint64_t)decRate << 32) / (uint64_t)dlRate;  // 必须 uint64
  const bool decimating = decRate > dlRate;
  uint64_t p = 0;
  uint32_t outN = (uint32_t)(((uint64_t)n << 32) / step) + 1;
  for (uint32_t k = 0; k < outN; k++) {
    uint32_t idx = (uint32_t)(p >> 32);
    if (idx >= (uint32_t)n) break;
    // burst 背压：官方服务器按句 burst 发送（实测 1.86x 实时），FIFO 将满时
    // 立刻起播并让泵腾空间，绝不丢样（曾"满丢最老"→ burst 溢出丢样=断续）。
    // ★V2.2.0：这里不能直接调 pumpPlay()／startDlPlay()——本函数跑在 WS 回调里，
    //   而泵跑在独立任务中，两边同时动协控串口会错帧。只挂标志，泵任务立刻接手。
    if (dlAvail() >= DL_SIZE - PLAY_CHUNK - 1) {
      if (!dlActive) dlWantStart = true;
      if (!dlActive && dlAvail() >= DL_SIZE - 1) {
        dlTail++;
        dlOver++;
      }  // 起播还没跟上就彻底满了：丢最老一字节防卡死（极小概率）
    }
    int32_t v;
    if (decimating) {
      // 三点盒式均值（低通）：抑制抽取混叠；语音主能量 <4kHz 基本无损
      int32_t s0 = decBuf[idx > 0 ? idx - 1 : 0];
      int32_t s1 = decBuf[idx];
      int32_t s2 = (idx + 1 < n) ? decBuf[idx + 1] : s1;
      v = (s0 + s1 + s2) / 3;
    } else {
      int32_t a = decBuf[idx];
      int32_t b = (idx + 1 < n) ? decBuf[idx + 1] : a;
      // v = a + (b-a)*frac/2^32；int64 防溢出（差值×65535 超 int32）
      v = a + (int32_t)(((int64_t)(b - a) * (int64_t)(p & 0xFFFFFFFFULL)) >> 32);
    }
    fifoPush((uint8_t)((v >> 8) + 128));
    p += step;
  }

  // jitter buffer：攒 ≥880ms 再开播。⚠️ 判断必须在 push 之后（曾放 push 前 →
  // dlAvail 永远只有单帧 < 门槛 → startDlPlay 永不触发 → TTS 全程无声）。
  // 按容量自适应钳制：门槛 > FIFO 容量时数学上永不满足，会被"FIFO 快满强制
  // 起播"分支兜底（起播点≈845ms）。
  int32_t jitterGate = MS2B(DL_JITGATE_MS);
  if (jitterGate > (int32_t)(DL_SIZE - PLAY_CHUNK - 1))
    jitterGate = (int32_t)(DL_SIZE - PLAY_CHUNK - 1);
  if (!dlActive && dlAvail() >= jitterGate && millis() > dlRetryAt) {
    dlWantStart = true;  // ★V2.2.0：起播交给泵任务（回调里不碰协控串口）
  }
}

// ========== 上行链路：协控录音泵 → upBuf → 重采样 → opus → WS ==========

static const uint16_t UP_SIZE = 8192;   // 2 的幂，≈0.34s @24000B/s（★V2.1.4 由 16384 减半）
static const uint16_t UP_MASK = UP_SIZE - 1;
static uint8_t upBuf[UP_SIZE];
static uint16_t upHead = 0, upTail = 0;
static inline uint16_t upAvail() {
  return (uint16_t)(upHead - upTail);
}

// 录音状态：recPhase 已在文件前部声明（0=空闲 1=录音中）
static bool _pollInFlight = false;  // 单发在途：任意时刻至多一发轮询
static uint8_t _recHdr[3], _recHdrCnt = 0;
static uint16_t _recDataNeed = 0, _recDataGot = 0;
static uint32_t _lastPoll = 0, recStartAt = 0, recFirstDataAt = 0;
static uint32_t recBytes = 0;
static uint32_t upSkipped = 0;  // upBuf 满丢弃的最老字节数（编码吞吐不足时 >0）
// 泵/编码实测仪表（stopTalk 打印后复位）
static uint32_t pollCount = 0, pollSum = 0, pollMax = 0;      // 轮询发数/实收和/单发最大
static uint32_t encFrames = 0, encTotalUs = 0, encMaxUs = 0;  // 编码帧数/总耗时/单帧最大

// 统计
uint32_t upFrames = 0, upSentBytes = 0;

// opus 上行工作缓冲（upPkt 前留 2 字节位，实际发送裸 opus）
static int16_t upEnc16[FRAME_SAMPLES];
static uint8_t upPkt[2 + MAX_PACKET];

// 官方 __tickRecord 同款单发在途录音泵。窗口 30ms：VOIP 编码预算加大后泵让位——
// 收包靠"泵窗口 + RX 缓冲 2048B 跨圈缓冲"联合消化 → 净收包率 100%。泵内绝对零打印！
static void coPumpRecord() {
  uint32_t t0 = millis();
  while (millis() - t0 < 30) {
    // 数据体在途：读完为止，期间绝不发新轮询
    if (_recDataNeed > 0) {
      while (coUart.available() && _recDataGot < _recDataNeed) {
        uint8_t b = coUart.read();
        if (upAvail() >= UP_SIZE - 1) {
          upTail++;
          upSkipped++;
        }  // 满丢最老（滑窗保最新）
        upBuf[upHead++ & UP_MASK] = b;
        recBytes++;
        _recDataGot++;
      }
      if (_recDataGot >= _recDataNeed) {
        _recDataNeed = 0;  // 完整收包，解锁下一发
      } else if (millis() - _lastPoll > 100) {
        _recDataNeed = 0;  // 残包丢弃（下一发前重同步）
      } else {
        return;
      }
      continue;
    }

    // 响应头在途：收 3 字节 [0x81,0x11,len]，杂散丢弃重同步
    if (_pollInFlight) {
      while (coUart.available() && _recHdrCnt < 3) {
        uint8_t b = coUart.read();
        if (_recHdrCnt == 0 && b != 0x81) continue;
        if (_recHdrCnt == 1 && b != 0x11) {
          _recHdrCnt = 0;
          continue;
        }
        _recHdr[_recHdrCnt++] = b;
      }
      if (_recHdrCnt >= 3) {
        _recHdrCnt = 0;
        _pollInFlight = false;
        uint16_t len = _recHdr[2];
        pollCount++;
        pollSum += len;
        if (len > pollMax) pollMax = len;
        if (recFirstDataAt == 0) recFirstDataAt = millis();
        if (len > 0 && len <= 202) {
          _recDataNeed = len;
          _recDataGot = 0;
        }
        continue;
      }
      if (millis() - _lastPoll > 20) {  // 头超时：本轮询作废，允许重发
        _pollInFlight = false;
        _recHdrCnt = 0;
        continue;
      }
      return;
    }

    // 空闲：距上次轮询 ≥2ms 才发下一发（仍只有一发在途）
    if (millis() - _lastPoll >= 2) {
      uint8_t n = 200;
      uint8_t poll[3] = { REC_POLL_H0, REC_POLL_H1, n };
      coUart.write(poll, 3);
      _lastPoll = millis();
      _pollInFlight = true;
    } else {
      delayMicroseconds(200);
    }
  }
}

// upBuf → 跨帧连续定点重采样 CO_RATE(24000)→16000 → opus 编码 → sendBIN
// upPosAcc 跨帧保留小数位置：帧内独立会导致每帧边界重复 1 样本（每秒 16 次颗粒声）
static uint64_t upPosAcc = 0;  // 相对 upTail 的定点读位置（跨帧连续）

static void drainUplink() {
  if (!wsReady) {
    upTail = upHead;
    upPosAcc = 0;
    return;
  }                                                               // 未握手直接丢
  const uint64_t step = ((uint64_t)CO_RATE << 32) / SAMPLE_RATE;  // 必须 uint64
  // 限时预算 40ms：每圈 1.3+ 帧 = 80ms+ 音频/75ms 圈 → 编码吞吐 ≥107%
  uint32_t tEnc = millis();
  while (millis() - tEnc < 40) {
    uint64_t endPos = upPosAcc + (uint64_t)FRAME_SAMPLES * step;
    if (upAvail() < (uint16_t)((endPos >> 32) + 1)) return;  // 输入不足一帧
    uint64_t p = upPosAcc;
    for (int k = 0; k < FRAME_SAMPLES; k++) {
      uint32_t idx = (uint32_t)(p >> 32);
      // 一阶低通（相邻输入样本平均）：24000→16000 下采样必须滤 8kHz 以上防混叠。
      // 注意必须取 idx-1 的相邻样本！曾误用上一次输出的跳步样本（频响畸变），
      // 恰是 ASR 识别劣化元凶
      int cur = upBuf[(upTail + idx) & UP_MASK];
      int prev = upBuf[(upTail + idx - 1) & UP_MASK];  // idx=0 时环形取上一帧末字节
      int v = (cur + prev) >> 1;
      upEnc16[k] = (int16_t)((v - 128) << 8);  // 8bit 无符号 → 16bit 有符号
      p += step;
    }
    upTail += (uint16_t)(p >> 32);  // 消费整数部分字节
    upPosAcc = p & 0xFFFFFFFF;      // 保留小数给下一帧
    uint32_t t0e = micros();
    int npkt = opus_encode(enc, upEnc16, FRAME_SAMPLES, upPkt + 2, MAX_PACKET);
    uint32_t dtUs = micros() - t0e;
    encFrames++;
    encTotalUs += dtUs;
    if (dtUs > encMaxUs) encMaxUs = dtUs;
    if (npkt > 0) {
      // 裸 opus 上行（无 [0x01,0x01] 帧头）——对齐 S2 官方固件与本地服务器预期。
      // 曾带帧头：server 把 0x01 0x01 当 opus 数据 → 每包前 2 字节污染 → ASR 只吐单字
      ws.sendBIN(upPkt + 2, npkt);
      upFrames++;
      upSentBytes += npkt;
    }
  }
}

static void wsConnect();  // 前置声明（定义在 WS 协议段）
static void sendListen(bool start);  // 前置声明（定义在 WS 协议段）
static void sendAbort();  // 前置声明：打断 TTS 时通知服务器停止生成（定义在 WS 协议段）

// 按需连接（官方 S2 同款架构）：说话才连、说完即关、空闲 60s 主动断——
// 常连设备空闲期反复被服务器踢 = "大段没声音"的根源
static bool ensureConnected() {
  // ★V2.1.0 死链检测：心跳不再因 pong 超时断连（连接变"粘"），半开连接必须在这里兜住。
  //   判据：自以为连着，但 >45s 未收到服务器任何消息（CONNECTED/TEXT/BIN 都刷新
  //   wsLastRxAt）⇒ 链路僵死（服务器卡死 / NAT 掉映射 / TCP 半开）。
  //   ★不处理的话，用户按 C 会「说话没反应」——本函数见 wsReady 就直接 return true，
  //   于是往一条死连接上发 listen start，永远等不到 TTS。
  if (wsReady && wsLastRxAt && millis() - wsLastRxAt > 45000) {
    Serial.printf("[WS] 连接疑似僵死（%lums 无服务器消息）——强制重连重来\n",
                  (unsigned long)(millis() - wsLastRxAt));
    ws.disconnect();
    wsReady = false;
    delay(50);
  }
  if (wsReady) return true;
  Serial.println("[WS] 按需连接...");
  wsConnect();
  uint32_t t0 = millis();
  while (!wsReady && millis() - t0 < 6000) ws.loop();
  ws.setReconnectInterval(86400000UL);  // 无论成败压回：库永不自动重连（唯一重连入口=本函数）
  if (wsReady) {
    Serial.println("[WS] 已就绪");
    return true;
  }
  Serial.println("[WS] 连接失败（6s 超时）");
  return false;
}

// 按下 C 键：打断 TTS → listen start → 协控启动录音
static void startTalk() {
  if (recPhase != 0) return;
  if (mLibState) mLibStop("用户说话");      // ★V2.4.0 先停取流再打断播放
  bool wasPlaying = dlActive;               // 本轮打断前是否在播 TTS/音乐
  if (dlActive) stopDlPlayHard();  // 软打断 TTS
  if (!ensureConnected()) {
    Serial.println("[TALK] 未连上服务器，松开重试");
    return;
  }
  // 打断通知：TTS/音乐进行中按 C = 用户要求中断生成。必须显式发 abort，
  // 否则服务器不知道要停，继续推歌、LLM 上下文还挂在播放会话上（实测：问星期几回歌词）。
  // 判据：刚打断播放(wasPlaying) 或 上一轮 TTS 未收到 stop(!ttsDone)。
  if (wasPlaying || !ttsDone) sendAbort();
  lastConvAt = millis();
  mtxSet(MTX_I_MIC);   // 点阵：麦克风图标（此刻总线空闲，先于录音启动帧发出）
  mtxFlush();
  sendListen(true);
  // 协控启动录音（阻塞等 ACK，官方同款）
  while (coUart.available()) coUart.read();
  uint8_t start[5] = { 0x90, CMD_CTL, 0x01, 0x01, 0 };
  start[4] = (uint8_t)(0x90 + CMD_CTL + 0x01 + 0x01);
  int r = sendWait(start, 5);
  if (r < 0) {
    Serial.println("[TALK] 协控录音启动无响应");
    return;
  }
  // 复位上行状态
  upTail = upHead = 0;
  upPosAcc = 0;
  recBytes = 0;
  recStartAt = millis();
  recFirstDataAt = 0;
  _pollInFlight = false;
  _recHdrCnt = 0;
  _recDataNeed = 0;
  _recDataGot = 0;
  _lastPoll = 0;
  recPhase = 1;
  Serial.println("[TALK] 开始说话（松开结束，录音中无日志属正常）");
}

// 松开 C 键：listen stop → 协控停止（阻塞等 ACK）→ 统计
static void stopTalk() {
  if (recPhase != 1) return;
  recPhase = 0;
  lastConvAt = millis();
  sendListen(false);
  uint8_t stopF[5] = { 0x90, CMD_CTL, 0x01, 0x00, 0 };
  stopF[4] = (uint8_t)(0x90 + CMD_CTL + 0x01);
  sendWait(stopF, 5);
  while (coUart.available()) coUart.read();
  uint32_t ms = (recFirstDataAt && recFirstDataAt < millis())
                  ? millis() - recFirstDataAt
                  : millis() - recStartAt;
  uint32_t bps = ms ? (uint32_t)((uint64_t)recBytes * 1000 / ms) : 0;
  Serial.printf("[TALK] 录音结束: %lu 字节 / %lu ms（%lu B/s）| 上行 opus %lu 帧 / %lu B"
                " | 下行丢弃 %lu 帧 | upBuf 丢最老 %lu B | 泵 %lu 发 avg %lu B | 编码 %lu 帧 avg %lu us\n",
                (unsigned long)recBytes, (unsigned long)ms, (unsigned long)bps,
                (unsigned long)upFrames, (unsigned long)upSentBytes,
                (unsigned long)dlDropped, (unsigned long)upSkipped,
                (unsigned long)pollCount,
                (unsigned long)(pollCount ? pollSum / pollCount : 0),
                (unsigned long)encFrames,
                (unsigned long)(encFrames ? encTotalUs / encFrames : 0));
  upFrames = 0;
  upSentBytes = 0;
  dlDropped = 0;
  upSkipped = 0;
  pollCount = 0;
  pollSum = 0;
  pollMax = 0;
  encFrames = 0;
  encTotalUs = 0;
  encMaxUs = 0;
  upTail = upHead;
  mtxSet(MTX_I_CLEAR);  // 点阵：录音结束灭屏（V2.0.2 用户偏好：不要菱形，等 LLM/TTS）
  mtxFlush();
}

// ========== 按键（非阻塞防抖沿检测）==========
// 硬件：按键外部下拉输入，按下=高电平！（曾误写 INPUT_PULLUP 低有效 →
// 上电自动 startTalk 且按/松极性全反）

static bool btnStable = LOW;
static uint32_t btnEdgeAt = 0;
static bool btnBStable = LOW;   // B 键（GPIO1）：短按打断 TTS/音乐

// ========== ★V2.2.3 音量控制（A 单击 -，B 单击 +；25 档，1 像素=1 档）==========
// 协控音量寄存器 0x14 直接收 0..100（One 板同款 N32）。命令帧与播放帧同格式：
//   [0x90][0x14][len=1][vol][checksum]
// ★ 单一写者原则：播放期间协控串口只允许「泵任务」写（音频帧/音量帧/图标帧全在它手里）。
//   按键上下文只改 volTarget + 挂 volPending，1ms 内由泵任务落地 ⇒ 连点不排队、不阻塞。
//   旧版从按键上下文发命令，被迫 dlPumpPause + 清 RX + 等应答（最多 750ms），
//   清 RX 会把在途 PCM 应答吃掉 ⇒ 重发风暴 ⇒ 调音量时断流+卡顿。

// ★V2.3.1 音量图案（用户指定：实心三角，不是描边）：
//   1~5 档 = 从左下角顶点起的**实心**直角三角形，逐档扩一层：
//     1→1 点 / 2→3 点 / 3→6 点 / 4→10 点 / 5→15 点（左下三角填满）
//   6 档 = 满档：除右上角 6 格外全亮（19 点），一眼看出"到顶了"
//   实现：d = x + (4-y)（到左下角的曼哈顿距离），d <= lv-1 的全部点亮。
static void volBarsPattern(uint8_t lv, uint8_t out[25]) {
  memset(out, 0, sizeof(uint8_t) * 25);
  if (lv < 1) lv = 1;
  if (lv > VOL_LEVELS) lv = VOL_LEVELS;
  if (lv < VOL_LEVELS) {
    for (uint8_t y = 0; y < 5; y++) {
      for (uint8_t x = 0; x < 5; x++) {
        if ((uint8_t)(x + (4 - y)) <= (uint8_t)(lv - 1)) out[y * 5 + x] = 1;
      }
    }
  } else {
    memset(out, 1, sizeof(uint8_t) * 25);
    out[2] = 0; out[3] = 0; out[4] = 0;   // 顶行右侧 3 格
    out[8] = 0; out[9] = 0;               // 第 2 行右侧 2 格
    out[14] = 0;                          // 第 3 行最右 1 格（右上角共 6 格不亮）
  }
}

// 阻塞版：仅开机同步音量用（此刻泵任务还没起、总线干净）。
static int volWriteHardwareBlocking() {
  uint8_t f[5] = {0x90, CMD_VOLUME, 0x01, volHwStep, 0};
  f[4] = (uint8_t)(f[0] + f[1] + f[2] + f[3]);
  coUart.write(f, 5);
  uint8_t got = 0, resp[3] = {0, 0, 0};
  uint32_t t1 = millis();
  while (got < 3 && millis() - t1 < 80) {
    while (coUart.available() && got < 3) {
      uint8_t b = coUart.read();
      if (got == 0 && b != 0x91) continue;
      if (got == 1 && b != CMD_VOLUME) {
        got = (b == 0x91) ? 1 : 0;
        continue;
      }
      resp[got++] = b;
    }
  }
  return (got >= 3) ? resp[2] : -1;
}

// 按键只做这一件事：改目标档位。真正的串口动作在泵任务里（volApplyPending）。
static void volStep(int8_t d) {
  lastVolActAt = millis();  // ★V2.3.2：调音量也算交互，别刚调完就休眠
  if (recPhase != 0) {  // 录音中协控串口是上行 PCM，绝不能插命令
    Serial.println("[VOL] 录音中忽略音量键");
    return;
  }
  int nv = (int)volTarget + d;
  if (nv < (int)VOL_MIN_LV) nv = VOL_MIN_LV;
  if (nv > (int)VOL_MAX_LV) nv = VOL_MAX_LV;
  if (nv == volTarget) {
    // ★V2.3.0：到顶/到底再按也要有反馈 —— 重新刷一遍当前档位图案（满档=19 点全亮图案），
    //   旧版直接 return 导致"按加键屏幕没反应"，用户以为按键失效。
    volShowPend = true;
    volShowAt = millis();
    Serial.printf("[VOL] 已到%s限 %u/%u（重刷图案）\n", d > 0 ? "上" : "下",
                  (unsigned)volTarget, (unsigned)VOL_LEVELS);
    return;
  }
  volTarget = (uint8_t)nv;
  volPending = true;  // 泵任务下一 tick 落地（≈1ms），连点只会覆盖成最新值，不会排队卡顿
  Serial.printf("[VOL] 目标 %u/%u 档（硬件 %u）%s\n", (unsigned)volTarget,
                (unsigned)VOL_LEVELS, (unsigned)volHwOf(volTarget),
                dlActive ? "播放中→立即下发" : "");
}

// 播放中把新档位推给协控：**只在流水线空窗（无在途 PCM 帧）发 5 字节**。
// 调用点有两个：pumpPlay 的 ACK 结算之后、泵任务 tick。
// 有在途帧时直接返回、保留标志，等下一个空窗——绝不切进 PCM 字节流中间。
static void volPushOne() {
  if (!volPushPend || !dlActive || recPhase != 0) return;
  if (dlPendCnt != 0) return;
  uint8_t hw = volHwOf(volLevel);
  if (hw == volHwStep) {
    volPushPend = false;
    return;
  }
  uint8_t v[5] = { 0x90, CMD_VOLUME, 0x01, hw, 0 };
  v[4] = (uint8_t)(v[0] + v[1] + v[2] + v[3]);
  coCmdBusy = true;
  coUart.write(v, 5);
  coCmdBusy = false;
  volHwStep = hw;
  volPushPend = false;
  Serial.printf("[VOL] 播放中下发 0x14=%u（帧间隙，在途 0）\n", (unsigned)hw);
}

// 泵任务调用：按键落地（只改软件状态，串口动作交给 volService，保证单一写者）。
// ★V2.2.9：播放中不再"等下一句"，改成在 PCM 帧间隙插一条 0x14（见 volService）。
static void volApplyPending() {
  if (!volPending) return;
  volPending = false;
  volLevel = volTarget;
  volShowPend = true;      // 刷柱状图（去抖 70ms，连点只画最后一帧）
  volShowAt = millis();
  volPushPend = true;      // 播放中把 0x14 推到协控；空闲时不用（起播序列会发）
  volSaveDirty = true;     // NVS 落盘推迟到空闲（loop 里做），播放中写 flash 会卡 10~40ms
  volSaveAt = millis();
  Serial.printf("[VOL] 第 %u/%u 档（硬件 %u）%s\n", (unsigned)volLevel,
                (unsigned)VOL_LEVELS, (unsigned)volHwOf(volLevel),
                dlActive ? "播放中→帧间隙下发" : "空闲→已记下");
}

// 泵任务调用：音量相关的全部串口/点阵动作都从这里出去（单一写者）。
// ★V2.2.9 两件事：
//   ① 播放中下发 0x14 —— **只在流水线空窗（dlPendCnt==0）插 5 字节**，绝不插进在途
//      PCM 帧的字节流中间（那会把帧尾当命令、把命令当 PCM）。它的应答 [91 14 xx]
//      由 pumpPlay 的定长解析按 h1!=CMD_PLAY 丢弃，不会抢 PCM 结算。
//   ② 刷 5x5 柱状图 —— 去抖 70ms + 最小间隔 140ms，播放中走快速路径。
//      旧版"连按 5 次连发 5 帧"会把协控渲染任务排满 ⇒ 3s 不应答 PCM ⇒ 看门狗收场。
static void volService() {
  uint32_t now = millis();
  volPushOne();
  if (!volShowPend) return;
  if (now - volShowAt < VOL_ICON_DEBOUNCE) return;              // 还在连点：等松手
  if (now - volShowSentAt < VOL_ICON_MIN_GAP) return;           // 限流：协控渲染队列
  volShowPend = false;
  uint8_t bars[25];
  volBarsPattern(volLevel, bars);
  if (dlActive) mtxSendFast(bars);   // 播放中：9 字节即走，不等渲染
  else mtxSend(bars);                // 空闲：阻塞确认（总线干净，失败会重试）
  volShowSentAt = millis();
  volIconAt = volShowSentAt;
  mtxCur = 0xFF;    // 与 mtxWant 不同 ⇒ 显示期结束后 mtxFlush 补回状态图标
  Serial.printf("[MTX] 音量柱状图 %u/%u 像素\n", (unsigned)volLevel, (unsigned)VOL_LEVELS);
}

static void pollButton() {
  // ★V2.4.4 C 键：有声书在播/暂停时改成"暂停·继续"，短按长按都先停推流+报位置。
  //   0=普通（按下即说话）1=本次按下刚暂停（长按到点说话）2=暂停态（短按继续，长按说话）
  static uint32_t btnCHoldAt = 0;
  static uint8_t  btnCMode = 0;
  static bool     btnCLongDone = false;
  const uint32_t BTN_C_LONG_MS = 700;

  bool b = digitalRead(BTN_PIN);
  if (b != btnStable) {
    if (btnEdgeAt == 0) btnEdgeAt = millis();
    else if (millis() - btnEdgeAt > 30) {
      btnStable = b;
      btnEdgeAt = 0;
      if (btnStable == HIGH) {                      // ---- 按下沿 ----
        btnCHoldAt = millis();
        btnCLongDone = false;
        if (mLibKind == 3 && !mLibPaused && (mLibState || dlActive)) {
          // 正在听书：按下即"停推流 + 记录位置"（短按长按都一样，先做这件事）
          mLibPauseAndReport();
          btnCMode = 1;
        } else if (mLibKind == 3 && mLibPaused) {
          btnCMode = 2;                             // 暂停态：短按=继续，长按=说话
        } else {
          btnCMode = 0;
          startTalk();                              // 按下=高电平=开始说话
        }
      } else {                                      // ---- 松开沿 ----
        if (btnCMode == 2 && !btnCLongDone && mLibPaused) {
          mLibResumePlay();
        } else if (btnCMode == 0 || btnCLongDone) {
          stopTalk();                               // 松开=低电平=停止，等 TTS
        }
        btnCMode = 0;
        btnCHoldAt = 0;
      }
    }
  } else {
    btnEdgeAt = 0;
    // 长按到点（按住不动也要判）：暂停态/刚暂停 → 直接开口说话。
    // 此时推流已经停了，说话不会和播放打架。
    if (btnStable == HIGH && btnCHoldAt && !btnCLongDone
        && (uint32_t)(millis() - btnCHoldAt) > BTN_C_LONG_MS) {
      btnCLongDone = true;
      if (btnCMode == 1 || btnCMode == 2) {
        btnCMode = 0;
        startTalk();
      }
    }
  }

  // ★V2.2.3：B 键单击 = 音量 +（取消原来的"B 短按打断"，打断统一走 C）。
  // 松开沿触发，30~600ms 算单击；取消 300ms 限频——连点每一下都计数（只改目标档位，
  // 不碰串口，所以再快也不会排队或卡顿）。
  static uint32_t btnBDownAt = 0;
  bool bb = digitalRead(BTN_B_PIN);
  if (bb != btnBStable) {
    btnBStable = bb;
    if (bb == HIGH) {
      btnBDownAt = millis();  // 按下沿
    } else {
      uint32_t held = btnBDownAt ? (millis() - btnBDownAt) : 0;
      btnBDownAt = 0;
      if (held > 30 && held < 600 && btnStable == LOW) {
        volStep(+1);
      }
    }
  }
}

// ========== WebSocket 协议 ==========

static String makeHeaders() {
  // 注意：末尾绝不能带 \r\n！库会自己补 NEW_LINE，多一个就会产生空行提前终结
  // HTTP 头，User-Agent 变成非法 WS 帧 → 服务器静默丢弃
  String h = "Authorization: Bearer " + curToken + "\r\n";
  h += "Protocol-Version: 1\r\n";
  h += "Device-Id: " + deviceId() + "\r\n";
  h += "Client-Id: " + clientId();
  return h;
}

static void wsConnect() {
  // ⚠️ begin() 会把 _lastConnectionFail 清 0：若 interval 还是 24h 压制值，loop 里
  // millis()-0=开机毫秒 < 24h 恒真 → 一次 TCP 都不发起 → 必超时。连接期间必须
  // 恢复小间隔；连接结束由 ensureConnected 压回
  ws.setReconnectInterval(500);
  if (useSSL) ws.beginSSL(curHost.c_str(), curPort, curPath.c_str());
  else ws.begin(curHost.c_str(), curPort, curPath.c_str());
  g_wsHeaders = makeHeaders();
  ws.setExtraHeaders(g_wsHeaders.c_str());  // 指针存进库里，字符串必须常驻
  Serial.printf("[WS] 连接 %s://%s:%u%s\n", useSSL ? "wss" : "ws",
                curHost.c_str(), curPort, curPath.c_str());
}

static void sendHello() {
  // 上行 16k 编码；hello 声明 16k/60ms。sample_rate 若声明 24000，部分服务器需
  // 逐帧实时转码跟不上 → TTS 断续（本地 NAS 实测）
  // ★V2.4.0：features.mcp 只在媒体库开启时声明 —— 关库时协议面与 V2.3.2 完全一致
  String hello = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",";
  if (mLibOn) hello += "\"features\":{\"mcp\":true},";
  hello += "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
           "\"channels\":1,\"frame_duration\":60}}";
  ws.sendTXT(hello);
  Serial.printf("[WS] 已发送 hello（features.mcp=%s）\n", mLibOn ? "true" : "关闭");
}

static void sendListen(bool start) {
  if (!wsReady) {
    Serial.println("[WS] 未握手，忽略 listen");
    return;
  }
  String msg = String("{\"type\":\"listen\",\"session_id\":\"") + gSession + "\",\"state\":\"" + (start ? "start" : "stop") + "\",\"mode\":\"manual\"}";
  ws.sendTXT(msg);
}

// 打断通知（官方协议必需）：客户端打断 TTS/音乐必须显式发 abort，
// 服务器才会停止音频生成并清掉会话的播放上下文。只发 listen start 时
// 服务器把新录音当普通听写，音乐继续推、LLM 继续围着歌转（实测教训）。
static void sendAbort() {
  if (!wsReady) return;
  String msg = String("{\"type\":\"abort\",\"session_id\":\"") + gSession + "\"}";
  ws.sendTXT(msg);
  Serial.println("[WS] 已发送 abort（打断通知）");
}

// 服务器 hello 里的采样率必须落在 opus 合法集合，否则保持当前值
// 先建新、成功才毁旧：曾"先毁后建"遇堆碎片 ALLOC_FAIL(-7) → dec=nullptr →
// 下行帧全丢（TTS 无声）。失败沿用旧解码器——opus_decode 自动转换采样率
static void rebuildDecoder(int sr) {
  if (sr != 8000 && sr != 12000 && sr != 16000 && sr != 24000 && sr != 48000) return;
  if (sr == decRate && dec) return;
  int err;
  OpusDecoder* nd = opus_decoder_create(sr, 1, &err);
  if (err != OPUS_OK || !nd) {
    Serial.printf("[DL] 解码器重建失败 %d，沿用 %dHz（opus 自动转换采样率）\n", err, decRate);
    return;
  }
  opus_decoder_destroy(dec);
  dec = nd;
  decRate = sr;
  Serial.printf("[DL] 解码器切换到 %dHz\n", sr);
}

// ---- 服务器"未激活"在线检测（V2.0.5）----
// 官方服务器对 token 失效（官网解绑/删除设备）的设备不拒绝连接，而是照常建会话，
// 然后用 TTS **播报激活码**（中英文均出现过）。这是"网页显示已激活但实际已解绑"的
// 唯一在线信号，必须抓下来：命中即把本地"(已激活)"缓存作废，换成服务器给的真状态。
static bool isActivationHint(const String& t) {
  String s = t; s.toLowerCase();
  return s.indexOf("activation") >= 0 || s.indexOf("activate") >= 0 ||
         s.indexOf("deactivat") >= 0 || s.indexOf("unbind") >= 0 ||
         s.indexOf("verification code") >= 0 ||
         t.indexOf("激活") >= 0 || t.indexOf("绑定") >= 0;
}
// 激活码 = 6 位数字。支持两种播报形态：「123456」与「1 2 3 4 5 6」
static String extractActCode(const String& t) {
  int n = (int)t.length();
  // ① 连续 6 位数字（前后不能再贴数字，防截取更长数字串的中段）
  for (int i = 0; i + 6 <= n; i++) {
    bool ok = true;
    for (int j = 0; j < 6; j++) { char c = t[i + j]; if (c < '0' || c > '9') { ok = false; break; } }
    if (!ok) continue;
    auto dig = [&](int p) { return p >= 0 && p < n && t[p] >= '0' && t[p] <= '9'; };
    if (dig(i - 1) || dig(i + 6)) continue;
    return t.substring(i, i + 6);
  }
  // ② 等间距单数字序列「1 2 3 4 5 6」（英文逐个念号的常见形态）
  for (int i = 0; i + 11 <= n; i++) {
    bool ok = true; String d;
    for (int k = 0; k < 6; k++) {
      int p = i + k * 2;
      char c = t[p];
      if (c < '0' || c > '9') { ok = false; break; }
      if (k < 5) {
        char sep = t[p + 1];
        if (sep != ' ' && sep != '-' && sep != ',' && sep != '.') { ok = false; break; }
      }
      d += c;
    }
    if (ok) return d;
  }
  return "";
}

// 服务器报告"未激活"：作废本地缓存，写入服务器给的真实状态
static void markNotActivated(const String& code, const String& src) {
  // 去重：激活提示常被 TTS 分成多句播报（"设备未激活" / "激活码是" / "123456"），
  // 只在状态真的发生变化时动作，避免日志刷屏
  if (code.length() ? (code == gActCode) : (gActCode != "(已激活)")) return;
  Serial.println("==========================================");
  Serial.println("[ACT] ⚠ 服务器报告设备未激活 / 已被解绑");
  Serial.println("[ACT] 依据文本: " + src);
  if (code.length()) {
    gActCode = code;
    prefs.begin("cfg", false);
    prefs.putString("actcode", code);
    prefs.end();
    Serial.printf("[ACT] 新激活码: %s（已写入 NVS，门户页可见）\n", code.c_str());
  } else if (gActCode == "(已激活)") {
    // 有未激活提示但没报码：至少不能再显示"已激活"
    gActCode = "";
    prefs.begin("cfg", false);
    prefs.remove("actcode");
    prefs.end();
    Serial.println("[ACT] 未提取到激活码，已清空本地\"已激活\"缓存");
  }
  Serial.println("[ACT] 打开 xiaozhi.me 重新绑定/输入激活码；或点门户页「联网核对激活状态」");
  Serial.println("==========================================");
}

// ========== 我的媒体库：语音开关 + 本地点播（V2.2.0）==========
// 数据格式：MediaServer 的 `/ap/song` 给的是 **8bit / 24000 / 单声道裸 PCM**
// （服务端 ffmpeg 现转码），所以这里**不做任何解码和重采样** —— 收到什么就往 dlFifo
// 里塞什么，和 TTS 解码后的 8bit 输出完全同源，播放泵（pumpPlay）不需要知道这数据
// 是"云端的"还是"曲库的"。这是整件事能做得这么小的根本原因。

// 32 点正弦（-100..100）。8bit 提示音用它合成，避免引入 sinf（C3 无 FPU）。
static const int8_t SINE32[32] = {
    0, 20, 38, 56, 71, 83, 92, 98, 100, 98, 92, 83, 71, 56, 38, 20,
    0, -20, -38, -56, -71, -83, -92, -98, -100, -98, -92, -83, -71, -56, -38, -20};

// 媒体库主机：没显式配就用「当前本地小智服务器」那台（自建曲库通常与它同机/同 NAS）
static String mLibHostNow() {
  if (mLibHost.length()) return mLibHost;
  if (!useSSL && curHost.length()) return curHost;
  return String();
}

static void mLibSave() {
  prefs.begin("cfg", false);
  prefs.putBool("mlon", mLibOn);
  prefs.putString("mlhost", mLibHost);
  prefs.putUShort("mlport", mLibPort);
  prefs.end();
}

// 开关提示音：两段 150ms 正弦。开=上行(660→880Hz)，关=下行(880→660Hz)。
// 走同一条播放泵（先灌 FIFO 再 startDlPlay），所以它和 TTS 永远不会混音。
static void mLibBeep(bool up) {
  if (recPhase) return;
  if (dlActive) stopDlPlayHard();   // 已在播：彻底停（清在途/清 RX）再播提示音，别硬切 0x10
  const uint16_t n = (uint16_t)(dlRate * 150 / 1000);
  const uint32_t f1 = up ? 660 : 880, f2 = up ? 880 : 660;
  dlTail = dlHead = 0;
  for (uint8_t seg = 0; seg < 2; seg++) {
    uint32_t f = seg ? f2 : f1;
    uint32_t step = (uint32_t)(((uint64_t)f * 32 << 16) / dlRate);  // Q16：每样本前进多少个表步
    uint32_t ph = 0;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t d = (i < (uint16_t)(n - i)) ? i : (uint16_t)(n - i);        // 距两端样本数
      uint16_t amp = d >= 240 ? 42 : (uint16_t)((uint32_t)d * 42 / 240);   // 10ms 淡入淡出防爆音
      ph += step;
      int16_t s = SINE32[(ph >> 16) & 31];
      fifoPush((uint8_t)(128 + (int16_t)((int16_t)s * (int16_t)amp) / 100));
    }
  }
  mtxMusic = false;
  dlBarged = false;
  startDlPlay();
  // 提示音是"已经喂完"的流：让泵播空后走正常收场（ttsDone 判据）
  ttsDone = true;
  dlFeedDone = true;
}

static void mLibStop(const char* why) {
  bool had = (mLibState != 0);
  // ★V2.4.6：有声书停播前，把"这一刻播到哪"排进上报队列 —— 服务端 v1.6.3 起只认设备
  //   报的位置，它的字节数在本地文件上完全失真（ffmpeg 一次能把整集塞进 TCP 缓冲）。
  //   此处 dlActive 还在 ⇒ mLibPlayedSec() 有效；歌单/电台不记位置。
  if (had && dlActive && mLibKind == 3) mLibQueuePos(mLibBaseSec + mLibPlayedSec(), 0, 0);
  mLibState = 0;
  mLibWaitTts = false;
  mLibEnded = false;
  // ★V2.4.7：任何一次"停"都退出暂停态（说话/说停/换一条）。以前这里不清，暂停后
  //   说句话 mLibPaused 会永远挂 true —— 短按 C 被当成"继续"，图标也会被暂停图案锁住。
  //   mLibPauseAndReport 是"先 mLibStop 再置 true"，顺序上不受影响。
  mLibPaused = false;
  mLibAutoAt = 0;        // ★V2.4.5：任何"停"都取消自动续播（用户说话/说停/按 C 暂停都算）
  mLibAutoReq = false;
  mLibRetryGo = false;   // ★V2.4.10：断流重试同样取消（用户主动停了就不许再自己续）
  mLibNowTitle = "";     // ★V2.4.10：没在播就没有"正在播放"信息
  mLibPosBeatAt = 0;     // ★V2.4.9：停播即停心跳（下一条流起播时重新计时）
  mLibLive = false;      // ★V2.4.1 电台标记只对本条流有效
  mLibCkRem = 0;
  mLibCkCRLF = 0;
  mLibCkLineLen = 0;
  // ★注意：不清 mLibKind/mLibQ/mLibPos —— 「停」完再说「下一首」还得有上下文
  if (mLibCli.connected() || mLibCli.available()) mLibCli.stop();
  dlSegHard = MS2B(DL_SEG_HARD_MS);   // 恢复 TTS 场次的会话保险丝
  if (had) Serial.printf("[LIB] 停止取流(%s)：本首已收 %lu B\n", why, (unsigned long)mLibBytes);
}

// UTF-8 百分号编码（歌名是中文，必须编；只放行 unreserved 字符）
static String mLibEnc(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 0x0F];
    }
  }
  return o;
}

// ★V2.4.10：URL 解码（服务端 X-Media-Title 里的中文是 %XX%XX… 三字节 UTF-8）
static String mLibDec(const char* s) {
  String o;
  size_t n = strlen(s);
  o.reserve(n);
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '%' && i + 2 < n) {
      char hx[3] = { s[i + 1], s[i + 2], 0 };
      long v = strtol(hx, nullptr, 16);
      if (v > 0) { o += (char)v; i += 2; continue; }
    }
    o += s[i];
  }
  return o;
}
// ★为什么不直接用 client.read(buf,len)：Node 默认对无 Content-Length 的响应上
//   chunked 编码，裸读会把 "1a2b\r\n" 这类块头当 PCM 播成噪声。服务端虽然刻意
//   关掉了 chunked（Connection: close + useChunkedEncodingByDefault=false），
//   这里仍保留解码分支 —— 服务端换实现时不会把设备变成"沙沙声"。
static size_t mLibPull(size_t want) {
  mLibSockAvail = mLibCli.available();   // ★V2.4.11：只在这里（主循环）碰 socket
  if (want > sizeof(mLibBuf)) want = sizeof(mLibBuf);
  size_t out = 0;
  if (!mLibChunked) {
    int a = mLibCli.available();
    if (a <= 0) {
      if (!mLibCli.connected()) mLibEnded = true;
      return 0;
    }
    if ((size_t)a > want) a = (int)want;
    int r = mLibCli.read(mLibBuf, (size_t)a);
    return r > 0 ? (size_t)r : 0;
  }
  while (out < want) {
    if (mLibCkRem == 0) {
      if (mLibCkCRLF) {   // 块数据尾的 CRLF：原样吃掉，别当空行（空行会被当成结束块）
        if (!mLibCli.available()) break;
        mLibCli.read();
        mLibCkCRLF--;
        continue;
      }
      if (!mLibCli.available()) break;
      int c = mLibCli.read();
      if (c < 0) break;
      if (c == '\n') {
        long v = strtol(mLibCkLine, nullptr, 16);
        mLibCkLineLen = 0;
        mLibCkLine[0] = 0;
        if (v <= 0) {   // 0 长块 = 末块
          mLibEnded = true;
          break;
        }
        mLibCkRem = (uint32_t)v;
      } else if (c != '\r') {
        if (mLibCkLineLen < sizeof(mLibCkLine) - 1) {
          mLibCkLine[mLibCkLineLen++] = (char)c;
          mLibCkLine[mLibCkLineLen] = 0;
        }
      }
      continue;
    }
    size_t w = mLibCkRem;
    if (w > want - out) w = want - out;
    int a = mLibCli.available();
    if (a <= 0) break;
    if ((size_t)a < w) w = (size_t)a;
    int r = mLibCli.read(mLibBuf + out, w);
    if (r <= 0) break;
    out += (size_t)r;
    mLibCkRem -= (uint32_t)r;
    if (mLibCkRem == 0) mLibCkCRLF = 2;
  }
  return out;
}

// 起一条本地流：kind 1=歌单 2=电台 3=有声书（服务端 /ap/play 统一出口）
// ★V2.4.4 设备事件上报：这块板子的 USB-Serial-JTAG 在应用运行时一个字节日志都取不到
//   （三种 dtr/rts 组合都试过，见 README），所以关键事件直接丢给媒体库 ——
//   服务端 console 会打，同时追加到 files/devlog.txt。「播几分钟就停」只看这一条就能定责。
//   ★只在收场/暂停这类低频点调用（最坏阻塞 600ms），绝不能放进循环。
static void mLibLog(const char* tag, const char* msg, long a, long b) {
  if (!mLibOn) return;
  String host = mLibHostNow();
  if (!host.length()) return;
  WiFiClient c;
  if (!c.connect(host.c_str(), mLibPort, 800)) return;
  String path = String("/ap/log?tag=") + tag + "&m=" + mLibEnc(String(msg ? msg : ""));
  path += "&mac=" + mLibDevMac + "&ep=" + String(mLibPos) + "&kind=" + String(mLibKind);
  if (a >= 0) path += "&a=" + String(a);
  if (b >= 0) path += "&b=" + String(b);
  c.print("GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + String(mLibPort)
          + "\r\nConnection: close\r\n\r\n");
  uint32_t t0 = millis();
  while (c.connected() && millis() - t0 < 600) { while (c.available()) c.read(); }
  c.stop();
}

// ★V2.4.6：把一个「集内绝对位置」排进上报队列。
//   绝对位置 = 服务端给的起点（mLibBaseSec = X-Resume-Offset）+ 本次实播（mLibPlayedSec）。
//   ★服务端收到后**直接落库，不再加起播点** —— 以前两边都加，位置越滚越偏（这就是
//     "起播点被算两遍"）。delayMs：暂停/说停传 0；本集播完传"FIFO 播空所需时间"。
static void mLibQueuePos(float absPos, uint8_t fin, uint32_t delayMs, bool beat) {
  if (!mLibOn || mLibKind != 3) return;
  if (absPos < 0) absPos = 0;
  mLibRptQ = mLibQ;          // 快照：之后 mLibQ 可能被下一条流覆盖
  mLibRptEp = mLibPos;
  mLibRptPos = absPos;
  mLibRptPlay = (absPos > mLibBaseSec) ? (absPos - mLibBaseSec) : 0;
  mLibRptFin = fin;
  mLibRptBeat = beat;
  mLibRptAt = millis() + delayMs;
  Serial.printf("[LIB] 位置入队%s：第 %u 集 %.0fs%s（%lums 后上报）\n",
                beat ? "（心跳）" : "", (unsigned)mLibRptEp, absPos,
                fin ? "（本集播完）" : "", (unsigned long)delayMs);
}

// 到点把队列里的位置发给媒体库。连不上就丢弃 —— 位置不是关键路径，绝不堵播放。
static void mLibPosTick() {
  if (mLibRptPos < 0) return;
  if ((int32_t)(millis() - mLibRptAt) < 0) return;
  String host = mLibHostNow();
  if (!host.length() || WiFi.status() != WL_CONNECTED) { mLibRptPos = -1; return; }
  // ★V2.4.9：心跳是**播放中**发的，而这条 HTTP 是同步阻塞的 —— 最坏 300ms 会把音频泵
  //   饿死（听感就是每 30s 卡一下）。所以心跳只在 FIFO 水位充足时才发，不够就等下一拍；
  //   真事件（暂停/播完）不设这道闸。★水位用 500ms：FIFO 总共才 853ms（DL_SIZE=20480），
  //   写 1.2s 这种阈值永远达不到 = 心跳永不触发。
  bool beat = mLibRptBeat;
  if (beat && dlActive && dlAvail() < MS2B(500)) return;
  // sec = 本次会话实播（老服务端 v1.6.2 会自己加上起播点，正好对）
  // abs = 集内绝对位置（v1.6.3 起直接用这个，不再叠加）—— 两版都能算对
  String path = String("/ap/pos?q=") + mLibEnc(mLibRptQ) + "&ep=" + String(mLibRptEp)
              + "&sec=" + String((unsigned long)(mLibRptPlay + 0.5f))
              + "&abs=" + String((unsigned long)(mLibRptPos + 0.5f))
              + "&fin=" + String((int)mLibRptFin)
              + (beat ? "&beat=1" : "");   // ★V2.4.9：心跳 → 服务端只更新位置
  WiFiClient c;
  if (!c.connect(host.c_str(), mLibPort, beat ? 200 : 800)) { mLibRptPos = -1; return; }
  c.print("GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + String(mLibPort)
          + "\r\nX-Dev-Mac: " + mLibDevMac + "\r\nConnection: close\r\n\r\n");
  uint32_t t0 = millis();
  uint32_t rto = beat ? 100 : 700;
  while (c.connected() && millis() - t0 < rto) { while (c.available()) c.read(); }
  c.stop();
  Serial.printf("[LIB] 位置已上报%s：第 %u 集 %.0fs%s\n", beat ? "（心跳）" : "",
                (unsigned)mLibRptEp, mLibRptPos, mLibRptFin ? "（本集播完）" : "");
  mLibRptPos = -1;
  mLibRptBeat = false;
}

// 本次会话**实际播出**的秒数（不含起点偏移）= 已发送量 - 超前量。// ★别用 (now-dlStartAt)/1000 —— 起播前预滚那段并没有放出来，会多算一个 FIFO。
// ★★V2.4.11：前面所有"重锚"会把 dlSentTotal 清零，所以累计量 = mLibAckBase + 本段。
//   mLibAckBase 只在新的一场本地流归零 ⇒ 位置单调不倒退，心跳/暂停/播完上报全受益。
static float mLibPlayedSec() {
  if (!dlActive) return 0;
  uint32_t now = millis();
  if (now < dlStartAt) return 0;
  int32_t ahead = (int32_t)dlSentTotal
                  - (int32_t)((uint64_t)(now - dlStartAt) * dlRate / 1000);
  int32_t sent = (int32_t)dlSentTotal - (ahead > 0 ? ahead : 0);
  if (sent < 0) sent = 0;
  uint32_t total = mLibAckBase + (uint32_t)sent;
  if (total == 0) return 0;
  return (float)total / (float)dlRate;
}

// 有声书暂停：先量位置 → 停取流 → 立刻停播 → 上报 → 显示暂停图案。
// 短按 C 和长按 C 都先走这一条（用户口径 2026-11-11：两种都要停推流 + 记位置）。
static void mLibPauseAndReport() {
  float played = mLibPlayedSec();
  float absPos = mLibBaseSec + played;
  mLibUserStop = true;
  mLibStop("暂停");          // 断开 socket：服务端立刻停止转码/推流
  stopDlPlayHard();          // 不能等 FIFO 播空 —— 用户按下就要静音
  mLibPaused = true;
  mLibBaseSec = absPos;      // 继续时以这里为起点
  mtxSet(MTX_I_PAUSE);
  mtxFlush();
  Serial.printf("[LIB] 暂停：本集播 %.0fs + 起点 %.0fs = %.0fs\n", played, absPos - played, absPos);
  mLibLog("PAUSE", "user-pause", (long)played, (long)absPos);
}

// 继续播放：从暂停点重连，服务端用 -ss 直接跳过去（不重新解整集）
static void mLibResumePlay() {
  if (!mLibPaused || mLibKind != 3) return;
  float from = mLibBaseSec;
  mLibPaused = false;
  Serial.printf("[LIB] 继续：第 %u 集 +%.0fs\n", (unsigned)mLibPos, from);
  mLibLog("RESUME", "user-resume", (long)from, (long)mLibPos);
  // ★V2.4.7：继续播放**没有**确认语（这一下是本机按键触发的，云端不知道），
  //   但也不能传 waitTts=false —— 那条路会置 ttsDone=false，把泵里"饿透重推头"那条
  //   云端分支重新打开。所以走 waitTts=true + expectVoice=false：只等 250ms 攒预滚。
  mLibPlayStream(3, mLibQ, mLibPos, true, from, false);
}

// ★V2.2.1 waitTts：true = 先抢跑缓冲，等云端确认语说完（ttsDone）再起播。
//   点歌那条 1~3 秒的网络/转码延迟就藏进云端说话的时间里，接缝几乎听不出来。
// ★V2.4.1 pos：歌单=跳过前 N 首（"下一首"就是 pos+=本批首数），有声书=从第 N 集起。
static void mLibPlayStream(uint8_t kind, const String& q, uint16_t pos, bool waitTts, float seekSec,
                           bool expectVoice) {
  mLibStop("换一条");
  // ★V2.4.7：正在放本地流时又来了新命令（"下一首 / 换个电台 / 换本书"）⇒ 立刻把旧场次收掉。
  //   不收的话 FIFO 里还压着旧歌，云端那句确认语会被排在旧歌后面（听起来就是"没应答"），
  //   而且 canStart 的 !dlActive 条件要等 1.5s 泵超时才满足。dlEndQuiet：屏幕别闪一下。
  if (dlActive && dlIsLocal) {
    dlEndQuiet = true;
    endDlPlay("换一条");
    dlEndQuiet = false;
  }
  mLibAutoReq = mLibAutoGo;   // ★V2.4.5：本次是不是自动续播（mLibStop 刚清掉，这里补回来）
  mLibWaitTts = waitTts;
  mLibExpectVoice = expectVoice;
  if (waitTts) {
    mLibPendingAt = millis();   // 兜底基准（云端不吭声也不能一直等）
    mLibTtsSeq0 = ttsSeq;       // ★V2.4.7：记下"请求发出时的 TTS 轮次"，只等之后新开的那一轮
    mLibTtsDone0 = ttsDone;     // 万一请求时云端正在说（llm 先开口再调工具），那条也得等
    mLibSawReply = false;       // 之后每收到一条服务器文本就置 true（判"它到底说不说"）
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LIB] WiFi 未连接，无法点播");
    return;
  }
  String host = mLibHostNow();
  if (!host.length()) {
    Serial.println("[LIB] 未配置媒体库地址：配网门户「我的媒体库」填 host + port(默认 8100)");
    return;
  }
  mLibKind = kind;
  mLibQ = q;
  mLibPos = pos;
  mLibTracks = 0;
  mLibPaused = false;
  mLibUserStop = false;
  // 本条流的起点秒：显式 seek（暂停后继续）优先，其它从 0 起（服务端会用自己的历史）
  mLibBaseSec = (seekSec > 0) ? seekSec : 0;
  mLibLive = (kind == 2);          // 电台是无限流：永远没有"本段结束"
  mLibCurTitle = q;
  mLibBytes = 0;
  mLibAckBase = 0;                 // ★V2.4.11：新的一条流，实播累计从零开始
  mLibDur = 0;                     // ★V2.4.10：本集时长（X-Book-Dur），断流判据用
  mLibRetryN = 0;                  // ★V2.4.10：新一场，重试计数清零
  mLibHdrLen = 0;
  mLibHdr[0] = 0;
  mLibChunked = false;
  mLibEnded = false;
  mLibStartAt = millis();
  mLibBufAt = millis();

  // ★三条腿共用一个出口：服务端已把「歌单连播 / 直播流 / 一本书的连续集」都整理成
  //   一条 8bit/24k 裸 PCM 流，本机只要把 path 拼对，别的什么都不用管。
  String path;
  const char* kn;
  if (kind == 2) {
    kn = "电台";
    path = "/ap/play?kind=radio&q=" + mLibEnc(q);
  } else if (kind == 3) {
    kn = "有声书";
    path = "/ap/play?kind=book&q=" + mLibEnc(q) + "&ep=" + String(pos) + "&n=1";
    // ★V2.4.4：暂停后继续 —— 集号由本机定，集内偏移也由本机定（服务端只管 -ss 跳过去）。
    //   这样即使服务端还是老版本（没有 /ap/pos 历史），暂停/继续也不会跳回集首。
    if (seekSec > 0) path += "&ss=" + String((long)seekSec);
  } else {
    kn = "歌单";
    path = "/ap/play?kind=song&q=" + mLibEnc(q) + "&idx=" + String(pos) + "&n=6";
  }
  Serial.printf("[LIB] %s「%s」pos=%u → %s\n", kn, q.c_str(), pos, path.c_str());
  // ★V2.2.1：超时 3000 → 1200ms。这是**阻塞式**连接，连不上时会把主循环连同
  //   正在播的 TTS 一起卡住 3 秒（实测日志里就有一条 889ms 断粮紧跟着它）。
  if (!mLibCli.connect(host.c_str(), mLibPort, 1200)) {
    Serial.println("[LIB] 连不上媒体库（查地址/端口/容器是否在跑）");
    mLibBeep(false);
    mtxSetFace(MTX_P_FROWN);
    mtxFlush();
    return;
  }
  // ★V2.4.2：媒体库请求要报设备身份（服务端按 MAC 分设备记播放进度）。
  //   deviceId() 是小写带冒号（"f0:9e:9e:cb:6f:bc"），这里转成 12 位大写十六进制，
  //   服务端 devIdOf() 正好这么归一化，两边对得上。
  {
    String m = WiFi.macAddress();
    m.replace(":", "");
    m.toUpperCase();
    mLibDevMac = m;
    mLibDevName = "AlphaPi-STEM-" + m.substring(8);   // 末 4 位足够人眼区分
    Serial.printf("[LIB] 设备身份：%s（%s）\n", mLibDevMac.c_str(), mLibDevName.c_str());
  }
  String req = "GET " + path + " HTTP/1.1\r\n"
               "Host: " + host + ":" + String(mLibPort) + "\r\n"
               "User-Agent: AlphaPi-STEM/2.4.13\r\n"
               "X-Dev-Mac: " + mLibDevMac + "\r\n"
               "X-Dev-Name: " + mLibDevName + "\r\n"
               "Connection: close\r\n\r\n";
  mLibCli.print(req);
  mLibState = 1;
  // waitTts 时**不能**清 ttsDone —— 那一位正是"云端确认语说完了没"的判据，
  // 起播就等它变 true（见 mLibTick 的 canStart）。非 waitTts（STT 兜底）沿用原语义。
  if (!waitTts) ttsDone = false;
  // ★电台是无限流：会话保险丝不能按"一首歌"设，否则播到 7 分钟会被自己的硬顶掐断
  dlSegHard = MS2B(mLibLive ? 3600000UL : DL_SEG_HARD_LOCAL_MS);
}

// 点一首歌（关键词）—— 兼容原来的调用点，内部就是"歌单从第 0 首起"
static void mLibPlayQuery(const String& q, bool waitTts) {
  mLibPlayStream(1, q, 0, waitTts);
}

// 下一首 / 下一集：带新的 pos 重连同一条队列（服务端按 pos 重新拼流）
// 返回 false = 没有可切换的上下文（没点过歌 / 当前是电台）
static bool mLibNext(bool waitTts) {
  if (!mLibKind || mLibKind == 2) return false;
  // 歌单：按**本条流实际包含的首数**跳（服务端在 X-Track-Count 里报），拿不到就按 6
  uint16_t step = (mLibKind == 1) ? (mLibTracks ? mLibTracks : 6) : 1;
  Serial.printf("[LIB] 下一个：%s pos %u → %u\n",
                mLibKind == 1 ? "下一首" : "下一集", mLibPos, (unsigned)(mLibPos + step));
  mLibPlayStream(mLibKind, mLibQ, (uint16_t)(mLibPos + step), waitTts);
  return true;
}


// 非阻塞取流泵：等头 → 边收边喂（FIFO 有空间才读 = 天然背压）→ 攒够预滚起播 → 收尾
static void mLibTick() {
  uint32_t now = millis();

  // ---- ★V2.4.5 有声书自动续播：上一集真播完（服务端 EOF）→ 接下一集 ----
  //   必须放在 `if (!mLibState) return;` **之前**：排这个待办的时候本条流已经收场、
  //   状态早就清零了，放后面永远轮不到。
  //   注意 mLibUserStop：按 C 暂停 / 说"停"是用户意图，不许自动接下一集。
  if (mLibAutoAt && (int32_t)(now - mLibAutoAt) >= 0) {
    mLibAutoAt = 0;
    if (mLibKind == 3 && mLibQ.length() && !mLibUserStop && !mLibPaused
        && WiFi.status() == WL_CONNECTED) {
      if (mLibRetryGo) {
        // ★V2.4.10 断流自愈：同一集、从断流点原地续播（不跳集、不报 fin）
        mLibRetryGo = false;
        Serial.printf("[LIB] 断流续播：第 %u 集 %.0fs 处接着听\n",
                      (unsigned)mLibPos, (double)mLibRetrySs);
        mLibPlayStream(3, mLibQ, mLibPos, false, mLibRetrySs);
      } else {
        Serial.printf("[LIB] 自动续播：第 %u 集播完 → 第 %u 集\n",
                      (unsigned)mLibPos, (unsigned)(mLibPos + 1));
        mLibAutoGo = true;
        mLibPlayStream(3, mLibQ, (uint16_t)(mLibPos + 1), false);
        mLibAutoGo = false;
      }
    }
  }

  if (!mLibState) return;

  // ---- 状态 1：等响应头（只关心状态码 / 是否 chunked，body 一个字节都不碰）----
  if (mLibState == 1) {
    while (mLibCli.available()) {
      int c = mLibCli.read();
      if (c < 0) break;
      if (mLibHdrLen < sizeof(mLibHdr) - 1) {
        mLibHdr[mLibHdrLen++] = (char)c;
        mLibHdr[mLibHdrLen] = 0;
      }
      uint16_t L = mLibHdrLen;
      if (L >= 4 && mLibHdr[L - 4] == '\r' && mLibHdr[L - 3] == '\n'
          && mLibHdr[L - 2] == '\r' && mLibHdr[L - 1] == '\n') break;
      if (L >= sizeof(mLibHdr) - 1) break;
    }
    uint16_t L = mLibHdrLen;
    bool hdrDone = (L >= 4 && mLibHdr[L - 4] == '\r' && mLibHdr[L - 3] == '\n'
                    && mLibHdr[L - 2] == '\r' && mLibHdr[L - 1] == '\n');
    // ★V2.4.11：万一还是顶满，也按"已收部分"解析（关键头都排在前面），别等 15s 超时
    //   把整场播放判死。剩下的头尾字节会混进 PCM 开头，属于可接受的降级。
    if (!hdrDone && L >= (uint16_t)sizeof(mLibHdr) - 1) {
      Serial.printf("[LIB] ⚠️ 响应头超长（%u B），按已收部分解析\n", (unsigned)L);
      hdrDone = true;
    }
    if (!hdrDone) {
      if (now - mLibStartAt > 15000) {
        Serial.printf("[LIB] 等响应头超时（已收 %u B）\n", (unsigned)mLibHdrLen);
        mLibStop("等头超时");
        mLibBeep(false);
      }
      return;
    }
    int code = 0;
    { const char* sp = strchr(mLibHdr, ' ');
      if (sp) code = atoi(sp + 1); }
    // chunked 判定用全小写副本（新 libc 不一定提供 strcasestr）
    {
      char low[sizeof(mLibHdr)];
      uint16_t i;
      for (i = 0; i < L && i < sizeof(low) - 1; i++) {
        char c = mLibHdr[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
      }
      low[i] = 0;
      mLibChunked = (strstr(low, "transfer-encoding: chunked") != nullptr);
      // ★V2.4.1：本条流实际包含几首（服务端 X-Track-Count）——
      //   "下一首"要按**实际**首数跳，不能死按 6（最后一批可能只有 2 首）
      mLibTracks = 0;
      const char* tcn = strstr(low, "x-track-count:");
      if (tcn) mLibTracks = (uint16_t)atoi(tcn + 14);
      /* ★V2.4.2：有声书续播时服务端会自己把集号往前跳（上次那集），
         本机必须把 mLibPos 同步成**实际起播的集号** —— 否则接着说「下一集」
         会拿 mLibPos=0 算出第 1 集，整本书被跳过。 */
      const char* bep = strstr(low, "x-book-ep:");
      if (bep) {
        uint16_t ep = (uint16_t)atoi(bep + 10);
        if (ep && ep != mLibPos) {
          Serial.printf("[LIB] 续播定位：第 %u 集（原请求 pos=%u）\n", (unsigned)ep, (unsigned)mLibPos);
          mLibPos = ep;
        }
      }
      /* ★V2.4.4：服务端自己跳了位置（续播）就把它记成起点 —— 「按 C 暂停」上报
         和「继续播放」的 ss 都用这个基准，否则会把整段位置算成 0。 */
      const char* rof = strstr(low, "x-resume-offset:");
      if (rof) {
        float off = (float)atol(rof + 16);
        if (off > 0) {
          mLibBaseSec = off;
          Serial.printf("[LIB] 起点偏移 %.0fs（服务端续播）\n", off);
        }
      }
      /* ★V2.4.10：本集时长 —— 流结束时拿"收到的字节数"跟它对账，
         区分「真播完」和「流被提前掐断」（后者不许报 fin、不许跳下一集）。 */
      const char* bdur = strstr(low, "x-book-dur:");
      if (bdur) mLibDur = (uint16_t)atoi(bdur + 11);
      /* ★V2.4.10：当前播放信息（服务端 v1.6.5 起）—— 歌名-歌手 / 书名·演播·第N集 / 台名。
         MCP 的 current_play 工具答「现在放的是什么」就用它；低副本里 %XX 的十六进制
         字母被小写化没关系（解码按 16 进制认）。标题行以 \r\n 结尾。 */
      const char* mt = strstr(low, "x-media-title:");
      if (mt) {
        char v[700];
        size_t vi = 0;
        for (const char* p = mt + 14; *p && *p != '\r' && *p != '\n' && vi < sizeof(v) - 1; p++) {
          v[vi++] = *p;
        }
        v[vi] = 0;
        mLibNowTitle = mLibDec(v);
      }
    }
    if (code != 200) {
      // ★V2.4.5：自动续播撞上 404 = 这本书已经到最后一集了（服务端明确回"没有第 N 集"）。
      //   那是正常收尾，不该给用户来一个失望脸 + 错误蜂鸣。
      bool tail = mLibAutoReq;
      Serial.printf("[LIB] 媒体库返回 HTTP %d（%s）\n", code,
                    tail ? "自动续播到头，全书播完" : "没搜到 / 取链失败，看服务端日志");
      mLibStop("HTTP 非 200");
      if (tail) {
        mtxSetFace(MTX_P_SMILE);
      } else {
        mLibBeep(false);
        mtxSetFace(MTX_P_FROWN);
      }
      mtxFlush();
      return;
    }
    Serial.printf("[LIB] 开始取流（%s）\n", mLibChunked ? "chunked" : "裸流");
    mLibState = 2;
    mLibBufAt = now;
    return;
  }

  // ---- 状态 2：边收边喂（**水位控制** = 按播放速率喂，绝不灌爆）----
  // ★★V2.2.1 这是"正确给协控喂音频"的核心，和小智 TTS 的节奏保持一致：
  //   起播前把 FIFO 填满（攒够预滚再开播）；起播后只在**水位低于 3/4** 时补回去。
  //   播放端只按 24000 B/s 消费 ⇒ 补多少就等于消费多少，读 socket 的速率被
  //   自然钳在 24KB/s，服务端也被 TCP 窗口背压成实时。
  //   V2.2.0 的写法是"有多少空位就读多少"，而 /ap/song 是整首 6.4MB 全速推的，
  //   缓冲被灌爆 ⇒ WiFi/TCP 拥塞 + FIFO 反复打满丢样 ⇒ 实机"破音严重且卡顿"。
  const uint16_t LIB_LOOP_BUDGET = 4096;   // 单圈最多 4KB≈170ms，别让一轮读太久
  const uint16_t libTarget = dlActive ? (uint16_t)(DL_SIZE * 3 / 4)
                                      : (uint16_t)(DL_SIZE - 1);
  // ★★V2.4.7：等确认语期间**一个字节都不许进 FIFO**。
  //   以前这里是"早就抢跑灌满"：FIFO 是 TTS 和本地流共用的一条队列，把歌提前塞进去，
  //   云端那句"继续播放啦"就跟歌搅在同一口锅里 —— 要么话被顶断，要么歌里混着人声。
  //   现在干脆不读 socket：TCP 背压会让服务端/ffmpeg 自己停下来等，数据一个不丢。
  //   ★为什么要连"tts stop 之后、尾音还没播完"那段也挂住：tts start 到达时
  //     `if (!dlActive) dlTail = dlHead;` 会清 FIFO 里的残留帧 —— 那时里面装的正是我们
  //     提前读进来的歌，一清就等于把开头几百毫秒**真丢了**（socket 里没有第二份）。
  //     所以宁可晚 100ms 读，也不提前读：确认语说完后重新灌 700ms 预滚只要几毫秒。
  // ★★V2.4.8（2026-11-11 当晚修）：这里的条件**不能**是裸的 mLibWaitTts。
  //   起播要 FIFO 攒够 700ms 预滚，而预滚只能从 socket 读 —— 无条件"等话期间不读"
  //   就构成死锁：确认语一说完 ⇒ 等 FIFO 有数据 ⇒ 可它永远不去读。
  //   实机表现：说"继续播放"后十几秒一点动静都没有（其实永不开始，因为挂起期间
  //   每拍都刷新 mLibBufAt，连 15s 取流超时都不会触发）。
  //   判据必须和下面 canStart **严格对称**，只在"确实还在等这句话"时挂住：
  //     ① 这一轮已经开口：stop 未到，或 stop 到了但尾音还在协控里播（dlActive）；
  //     ② 这一轮还没开口：只在"它可能马上就开口"的窗口内挂住（无动静 0.5s /
  //        服务器在生成 6s），窗口一过就当它不说话，立刻读 socket 灌预滚；
  //     ③ 20s 绝对兜底（服务器漏发 stop 也不能一直哑着）。
  uint32_t libWaited = (uint32_t)(now - mLibPendingAt);
  bool libSpeaking = (ttsSeq != mLibTtsSeq0) || !mLibTtsDone0;
  bool holdTts = false;
  if (mLibWaitTts && libWaited <= 20000) {
    if (libSpeaking) holdTts = (!ttsDone || dlActive);   // 话没说完 / 尾音还在播
    else if (mLibExpectVoice) holdTts = libWaited <= (mLibSawReply ? 6000 : 500);
  }
  if (holdTts) {
    mLibBufAt = now;   // 挂起不算"15s 没收到数据"
  } else if (dlAvail() < libTarget) {
    uint16_t budget = LIB_LOOP_BUDGET;
    while (budget && dlAvail() < libTarget) {
      uint16_t room = (uint16_t)(libTarget - dlAvail());
      size_t want = room < sizeof(mLibBuf) ? (size_t)room : sizeof(mLibBuf);
      if (want > budget) want = budget;
      size_t got = mLibPull(want);
      if (!got) break;
      for (size_t i = 0; i < got; i++) fifoPush(mLibBuf[i]);
      mLibBytes += (uint32_t)got;
      budget -= (uint16_t)got;
      mLibBufAt = now;
      dlLastAudioAt = now;   // 播放泵的"流还活着"判据（gapAlign / 硬顶都读它）
    }
  }

  // ---- 起播门槛：攒够 700ms 再开（沿用 TTS 的 jitter 思路）；流尾不足 700ms 也照播 ----
  // ★V2.2.1：门槛要按 FIFO 容量钳制 —— DL_SIZE 缩到 14KB 后 16800B 永远攒不到，
  //   本地点歌就会退化成"整首下完才开播"（等于点歌没反应）。
  int32_t libGate = MS2B(700);
  if (libGate > (int32_t)(DL_SIZE - PLAY_CHUNK - 1)) libGate = (int32_t)(DL_SIZE - PLAY_CHUNK - 1);
  // ★★V2.4.7：确认语说完（tts stop）且 FIFO 已播空 ⇒ **立刻**收掉 TTS 场次，把总线让给本地流。
  //   不这么做就要等泵那条"ttsDone && 1.5s 无新字节"的收场判据，用户会听到 1.5s 空白。
  //   dlEndQuiet：别清屏、别写媒体库日志 —— 下一拍 startDlPlay 就要上音符/书/收音机图案了。
  //   ★积压 ≤120ms 才收：dlAvail()==0 只说明"字都发出去了"，协控自己那几帧还在播；
  //     这时候推新 WAV 头 = 会话中途插头（总纲里明令禁止的"会话杀手"）。
  {
    int32_t aheadTts = (int32_t)dlSentTotal
                       - (int32_t)((uint64_t)(now - dlStartAt) * dlRate / 1000);
    bool ttsOver = ttsDone && dlAvail() == 0 && aheadTts <= MS2B(120);
    // 20s 硬出口：服务器极端情况下不发 stop（漏了 stop / 说个没完），不能永远哑着 ——
    // 那时先把 TTS 场次收掉，让下面的 canStart 兜底把本地流放出来。
    bool forceOver = (uint32_t)(now - mLibPendingAt) > 20000;
    if (mLibWaitTts && dlActive && (ttsOver || forceOver)) {
      dlEndQuiet = true;
      endDlPlay("确认语说完");
      dlEndQuiet = false;
    }
  }
  // ★★V2.4.7：起播判据改看**轮次**，不能再信 ttsDone。
  //   工具被调用的那一刻，上一轮的 tts stop 还挂在 ttsDone 上（true）⇒ 旧判据当场放行，
  //   局域网 0.3s 就攒够 700ms 预滚 ⇒ 本地音乐抢在"继续播放啦"第一个字上桌把它顶断。
  //   现在：这一轮已经开口（ttsSeq 变了）就必须等它 stop 且 FIFO 播空；
  //         一轮都没开口（LLM 只调工具不说话）则 0.5s 后照播；服务器明明在生成
  //         （mLibSawReply）就耐心等 6s；20s 绝对兜底（漏发 stop 也不能一直哑着）。
  //   ★V2.4.8：判据与上面 holdTts 共用 libWaited / libSpeaking（两边必须一致，
  //     否则一边放行、另一边还挂着，就又成了"判据放行了但没数据"的死相）。
  bool canStart = true;
  if (mLibWaitTts) {
    if (libSpeaking) canStart = ttsDone && !dlActive;            // 等它说完、且 FIFO 播空
    else if (!mLibExpectVoice) canStart = libWaited > 250;        // 本机自发的（继续播放）：等预滚就够
    else canStart = libWaited > (mLibSawReply ? 6000 : 500);      // 它在说话的路上就多等，完全没动静就别干等
    if (libWaited > 20000) canStart = true;
  }
  if (!dlActive && canStart && (dlAvail() >= libGate || (mLibEnded && dlAvail() > 0))) {
    mLibWaitTts = false;
    mtxMusic = true;       // 点阵：播放中（startDlPlay 据此选图案）
    // ★V2.4.4：三种本地流各用各的图案 —— 音符 / 摊开的书 / 收音机（用户 2026-11-11 选定）
    mtxSet(mLibKind == 3 ? MTX_I_BOOK : (mLibKind == 2 ? MTX_I_RADIO : MTX_I_MUSIC));
    dlBarged = false;
    startDlPlay();
    if (dlActive) {
      if (mLibKind == 3) mLibPosBeatAt = now + LIB_POS_BEAT_MS;   // ★V2.4.9 起播即开始计时
      Serial.printf("[LIB] 起播（预滚 %lums，抢跑 %lums）\n",
                    (unsigned long)((uint64_t)dlAvail() * 1000 / dlRate),
                    (unsigned long)(now - mLibStartAt));
    }
  }

  // ★★V2.4.9：有声书播放中每 30s 报一次"听到哪了"（心跳）。
  //   没有它，设备一断电这段进度就全丢（服务端只知道起播点）。最坏只丢 30s。
  //   ★队列里已有待发上报（暂停/播完，mLibRptPos>=0）时不许插队 —— 真事件优先。
  if (mLibKind == 3 && dlActive && mLibPosBeatAt
      && (int32_t)(now - mLibPosBeatAt) >= 0) {
    mLibPosBeatAt = now + LIB_POS_BEAT_MS;
    if (mLibRptPos < 0) mLibQueuePos(mLibBaseSec + mLibPlayedSec(), 0, 0, true);
  }

  // ---- 收尾：末块/对端关闭 ⇒ 交给播放泵把 FIFO 播空后 endDlPlay ----
  if (mLibEnded) {
    while (mLibCli.available()) mLibCli.read();   // 丢掉 chunked 尾巴/trailer
    if (mLibCli.connected()) mLibCli.stop();
    uint32_t ms = millis() - mLibStartAt;
    bool wasLive = mLibLive;
    // ★V2.4.5：有声书的服务端 EOF = **这一集真播完了**（不是被打断）⇒ 排下一集。
    //   要等 FIFO 里剩下的音频播空再重连：立刻重连会把结尾切掉，也会让播放泵
    //   在两段流之间插一个断口。400ms 是不把话头咬掉的余量。
    if (mLibKind == 3 && !wasLive) {
      // ★★V2.4.10 断流对账：收到字节数离"播完整集"还差 15s 以上 = 流被提前掐断
      //   （服务端/网络抖动），绝不是"本集播完" —— 实测 13.8 分钟的集子断在 6.8 分钟，
      //   旧逻辑照报 fin=1，服务端把记录标成已播完、续播直接跳走一整集。
      // ★★V2.4.13：`MS2B(uint32_t ms)` 的单位是**毫秒**！V2.4.10 这里写的
      //   MS2B(15)/MS2B(30) 本意是「差 15 秒 / 至少 30 秒」，实际只有 360B/720B
      //   = 15ms/30ms 音频 ⇒ 一次正常 EOF 只要尾部少收 15ms 就被判成断流。
      //   后果链：不报 fin → ss≈集尾原地续播 → 服务端夹到 dur-30 → 倒回 30 秒
      //   重播本集 → 30s 后又是 EOF → 循环 3 次，永远不跳下一集。
      float spanSec = (float)mLibDur - mLibBaseSec;
      if (spanSec < 0) spanSec = 0;
      float expect = spanSec * (float)dlRate;
      float nowSec = mLibBaseSec + mLibPlayedSec();
      // ★已经听到集尾 5s 内 ⇒ 无论字节差多少都按「本集播完」收场，绝不原地续播。
      //   断流重试的 ss 本来就落在集尾附近，再夹一次只会无限重播同一段。
      bool atTail = (mLibDur > 0 && nowSec >= (float)mLibDur - 5);
      bool cutShort = mLibDur > 0 && !atTail
                      && (float)mLibBytes < expect - (float)MS2B(15000)
                      && expect > (float)MS2B(30000);
      Serial.printf("[LIB] 断流对账：dur=%us base=%.0fs bytes=%.0fs expect=%.0fs now=%.0fs"
                    " atTail=%d cutShort=%d\n",
                    (unsigned)mLibDur, mLibBaseSec, (float)mLibBytes / (float)dlRate,
                    expect / (float)dlRate, nowSec, (int)atTail, (int)cutShort);
      if (cutShort && mLibRetryN < 3) {
        mLibRetryN++;
        mLibRetryGo = true;
        mLibRetrySs = mLibBaseSec + mLibPlayedSec();
        // ★V2.4.13：断点不许贴到集尾（服务端会夹 off，两边一起夹才不会越界）
        if (mLibDur > 0) {
          float cap = (float)mLibDur - 3;
          if (cap < 0) cap = 0;
          if (mLibRetrySs > cap) mLibRetrySs = cap;
        }
        uint32_t rest = 2000;    // 给 WiFi/服务端 2s 喘息再原地续播
        mLibAutoAt = millis() + rest;
        // ★不报 fin：位置照报（fin=0），服务端把记录停在真实听到的位置
        mLibQueuePos(mLibBaseSec + mLibPlayedSec(), 0, 0);
        Serial.printf("[LIB] ⚠️ 断流（收 %.0fs / 全集应 %.0fs，第 %u 集）→ 2s 后原地续播\n",
                      (float)mLibBytes / (float)dlRate, expect / (float)dlRate,
                      (unsigned)mLibPos);
        (void)rest;
      } else if (cutShort) {
        // 重试 3 次都断：放弃，但不许污染播放记录 —— 报"听到这里"（fin=0）+ 失望脸
        mLibQueuePos(mLibBaseSec + mLibPlayedSec(), 0, 0);
        mLibAutoAt = 0;
        mtxSetFace(MTX_P_FROWN);
        mtxFlush();
        Serial.println("[LIB] 断流重试 3 次仍失败，收场（记录停在断点）");
      } else {
        uint32_t rest = (uint32_t)((uint64_t)dlAvail() * 1000 / dlRate) + 400;
        if (rest > 60000) rest = 60000;
        mLibAutoAt = millis() + rest;
        mLibRetryN = 0;      // 真播完：重试计数清零
        // ★V2.4.6：本集真播完 ⇒ 报 fin=1（服务端下次续播跳下一集）。
        //   位置也要等 FIFO 播空再报：现在报会把尾巴（还在缓冲里）漏掉，续播会重听几秒。
        // ★★V2.4.11：真播完时位置**直接取集尾**，不再用"起点 + 实播累加"。
        //   cutShort 已经拿 socket 字节数与 X-Book-Dur 对过账、确认不是断流；此时再用
        //   易受重锚/心跳影响的累加值，只会把 828s 报成 602s ⇒ 服务端把 fin 拦下、
        //   下次续播倒回这一集。dur 未知（老服务端）才退回累加以保兼容。
        float donePos = (mLibDur > 0) ? (float)mLibDur
                                      : (mLibBaseSec + mLibPlayedSec());
        mLibQueuePos(donePos, 1, rest);
        Serial.printf("[LIB] 本集播完，%lums 后自动接第 %u 集\n",
                      (unsigned long)rest, (unsigned)(mLibPos + 1));
      }
    }
    mLibState = 0;
    mLibLive = false;
    dlSegHard = MS2B(DL_SEG_HARD_MS);
    ttsDone = true;
    dlFeedDone = true;
    Serial.printf("[LIB] %s结束：%lu B / %lums（%lu B/s）\n",
                  wasLive ? "电台（上游断流）" : "点播",
                  (unsigned long)mLibBytes, (unsigned long)ms,
                  (unsigned long)(ms ? (uint64_t)mLibBytes * 1000 / ms : 0));
    return;
  }
  if (now - mLibBufAt > 15000) {
    Serial.println("[LIB] 15s 没收到数据，放弃本首");
    mLibStop("取流超时");
    mtxSetFace(MTX_P_FROWN);
    mtxFlush();
  }
}

// 开关指令识别：0=不是 1=开 2=关。必须先认出「媒体库/曲库」这个对象词，
// 否则"关闭蓝牙"之类的普通指令会被误判成关媒体库。
static int mLibCmdKind(const String& t) {
  if (t.indexOf("媒体库") < 0 && t.indexOf("曲库") < 0) return 0;
  static const char* off[] = {"关闭", "关掉", "关上", "停用", "禁用", "不要用", "别用", "不用"};
  static const char* on[] = {"启用", "打开", "开一下", "开启", "使用", "用上", "切到", "换成"};
  for (uint8_t i = 0; i < sizeof(off) / sizeof(off[0]); i++)
    if (t.indexOf(off[i]) >= 0) return 2;
  for (uint8_t i = 0; i < sizeof(on) / sizeof(on[0]); i++)
    if (t.indexOf(on[i]) >= 0) return 1;
  return 0;
}

// 点歌意图 → 关键词。返回 false = 不接（交还云端，保持原有对话能力）。
// ★宁可漏接不可误接：一旦接了就 abort 云端，误判的代价是"这句话没回答"。
static bool mLibSongQuery(const String& t, String& q) {
  static const char* neg[] = {"怎么", "为什么", "什么", "如何", "能不能", "可以吗",
                              "有没有", "是不是", "关掉", "关闭", "停用"};
  for (uint8_t i = 0; i < sizeof(neg) / sizeof(neg[0]); i++)
    if (t.indexOf(neg[i]) >= 0) return false;
  static const char* kw[] = {"播放一首", "播放首", "播放一下", "播放", "放一首", "放首",
                             "来一首", "来首", "点一首", "点首", "点歌", "唱一首", "唱首",
                             "听一首", "听首", "放个", "来点"};
  int best = -1;
  uint8_t bestLen = 0;
  for (uint8_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
    int p = t.indexOf(kw[i]);
    if (p >= 0 && (best < 0 || p < best)) {
      best = p;
      bestLen = (uint8_t)strlen(kw[i]);
    }
  }
  if (best < 0) return false;
  String r = t.substring(best + bestLen);
  // 量词/助词（长串在前，且**不含单独的"歌"**——"播放歌唱祖国"会被啃掉一个字）
  static const char* lead[] = {"一首", "一曲", "一个", "首歌", "歌曲", "首", "曲",
                               "下", "点", "些", "个", "的"};
  static const char* tail[] = {"这首歌", "的歌曲", "吧", "呗", "呀", "啊", "哦", "嘛",
                               "。", "，", "！", "？", "?", "!", " "};
  bool ch = true;
  while (ch) {
    ch = false;
    r.trim();
    for (uint8_t i = 0; i < sizeof(lead) / sizeof(lead[0]); i++) {
      if (r.startsWith(lead[i])) {
        r = r.substring(strlen(lead[i]));
        ch = true;
        break;
      }
    }
  }
  ch = true;
  while (ch) {
    ch = false;
    r.trim();
    for (uint8_t i = 0; i < sizeof(tail) / sizeof(tail[0]); i++) {
      if (r.endsWith(tail[i])) {
        r = r.substring(0, r.length() - strlen(tail[i]));
        ch = true;
        break;
      }
    }
  }
  r.trim();
  if (!r.length() || r.length() > 30) return false;
  q = r;
  return true;
}

// ---------------- V2.4.12：进度 / 时长 / 信号强度 的公共口径 ----------------

// 秒 → 「1小时5分30秒」，给小智念出来比裸秒数自然
static String mLibFmtSec(float s) {
  if (s < 0) s = 0;
  long v = (long)(s + 0.5f);
  long h = v / 3600, m = (v % 3600) / 60, sec = v % 60;
  String r;
  if (h) r += String(h) + "小时";
  if (h || m) r += String(m) + "分";
  r += String(sec) + "秒";
  return r;
}

// 集内绝对位置 = 本条流起点 + 本次实播。
// ★暂停时 mLibPauseAndReport 已把 mLibBaseSec 写成暂停位置、mLibPlayedSec() 为 0 ⇒ 仍正确。
// ★V2.4.11 起 mLibPlayedSec() 用 mLibAckBase 单调累计，重锚不再让位置倒退。
static float mLibNowSec() {
  return mLibBaseSec + mLibPlayedSec();
}

// 从一段 JSON 里取**带符号 / 带小数**的数字（jsonInt 只认正整数，后退的 -30 会被吃掉）
static bool mLibJsonNum(const String& s, const char* key, float& out) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i += pat.length();
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\"')) i++;
  int j = i;
  if (j < (int)s.length() && (s[j] == '-' || s[j] == '+')) j++;
  bool any = false;
  while (j < (int)s.length() && (isDigit(s[j]) || s[j] == '.')) {
    if (isDigit(s[j])) any = true;
    j++;
  }
  if (!any) return false;
  out = s.substring(i, j).toFloat();
  return true;
}

// 口令里的数量：先认阿拉伯数字，再认中文数字（含「十/百」组合）。找不到返回 -1。
// 用 indexOf/逐段比较而不是按 3 字节步进，混了 ASCII 也不会错位。
static long mLibParseNum(const String& src) {
  for (unsigned i = 0; i < src.length(); i++) {
    if (isDigit(src[i])) {
      long v = 0;
      while (i < src.length() && isDigit(src[i])) { v = v * 10 + (src[i] - '0'); i++; }
      return v;
    }
  }
  String s = src;
  s.replace("两", "二");
  static const char* dg[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  long total = 0, cur = 0;
  bool hit = false;
  for (unsigned i = 0; i < s.length();) {
    int d = -1;
    for (int k = 0; k < 10; k++) {
      unsigned L = strlen(dg[k]);
      if (i + L <= s.length() && s.substring(i, i + L) == dg[k]) { d = k; break; }
    }
    if (d >= 0) { cur = d; hit = true; i += 3; continue; }
    if (i + 3 <= s.length() && s.substring(i, i + 3) == "十") {
      total += (cur ? cur : 1) * 10; cur = 0; hit = true; i += 3; continue;
    }
    if (i + 3 <= s.length() && s.substring(i, i + 3) == "百") {
      total += (cur ? cur : 1) * 100; cur = 0; hit = true; i += 3; continue;
    }
    if (hit) break;
    i++;
  }
  return hit ? (total + cur) : -1;
}

// 进度口令解析：返回 1=绝对秒(sec)，2=相对偏移(delta)，0=不是进度口令
static int mLibSeekQuery(const String& t, float& outSec, float& outDelta) {
  long n = mLibParseNum(t);
  if (n < 0) return 0;
  float mult = 1;
  if (t.indexOf("小时") >= 0) mult = 3600;
  else if (t.indexOf("分钟") >= 0 || t.indexOf("分") >= 0) mult = 60;
  float val = (float)n * mult;
  bool absMode = (t.indexOf("跳到") >= 0 || t.indexOf("定位到") >= 0
                  || t.indexOf("从第") >= 0 || t.indexOf("到第") >= 0
                  || t.indexOf("开始播") >= 0 || t.indexOf("开始放") >= 0
                  || t.indexOf("播到") >= 0);
  bool back = (t.indexOf("后退") >= 0 || t.indexOf("倒退") >= 0 || t.indexOf("退回") >= 0
               || t.indexOf("往前") >= 0 || t.indexOf("往回") >= 0
               || t.indexOf("快退") >= 0 || t.indexOf("倒回") >= 0);
  bool fwd = (t.indexOf("快进") >= 0 || t.indexOf("前进") >= 0
              || t.indexOf("往后") >= 0 || t.indexOf("跳过") >= 0);
  if (!absMode && !back && !fwd) return 0;
  if (absMode) { outSec = val; return 1; }
  outDelta = back ? -val : val;
  return 2;
}

// WiFi 信号分档 + 能直接念给用户的结论与建议（小智据此判断「信号好不好」）
static String mLibSignalText() {
  if (WiFi.status() != WL_CONNECTED) return "WiFi 未连接，设备当前离线，媒体库暂时取不到流";
  int r = WiFi.RSSI();
  String lv;
  if (r >= -55) lv = "很好（满格）";
  else if (r >= -65) lv = "良好（3 格）";
  else if (r >= -75) lv = "一般（2 格）";
  else lv = "较差（1 格）";
  String s = String("WiFi 信号 ") + r + " dBm，" + lv + "，已连接「" + WiFi.SSID() + "」";
  if (r < -70)
    s += "。信号偏弱，听书可能偶有卡顿，建议把设备靠近路由器、避开金属遮挡或换 2.4G 信道";
  else if (r < -60) s += "。基本稳定，偶发卡顿多来自服务端或网络抖动";
  else s += "。信号充足，卡顿基本与 WiFi 无关";
  return s;
}

// 执行跳转：同一集用 &ss=<目标秒> 原地重开一条流（服务端输入侧 -ss 快速定位）
static String mLibDoSeek(bool hasSec, float sec, bool hasDelta, float delta) {
  if (!mLibKind) return "当前没有在播放，无法调进度";
  if (mLibKind == 2) return "电台是直播流，不能调进度";
  if (mLibKind != 3) return "歌单暂不支持调进度，可以说「下一首」";
  float target = hasSec ? sec : (mLibNowSec() + delta);
  if (mLibDur > 0) {
    // ★别落在文件外：-ss 超出时长 ⇒ ffmpeg 零字节 ⇒ 设备「嘀一声没声」（v1.6.3 踩过）
    float cap = (float)mLibDur - 3;
    if (cap < 0) cap = 0;
    if (target > cap) target = cap;
  }
  if (target < 0) target = 0;
  // ★MCP 路径等云端那句确认语说完再起播（V2.4.8 的 ttsSeq 轮次判据）；
  //   STT 兜底路径已经 abort 云端，不等，否则白挂 500ms。
  bool wait = mLibMcpReady;
  // mLibStop() 内部会把「这一刻听到哪」排进 /ap/pos 上报队列，这里不必重复排
  mLibPlayStream(3, mLibQ, mLibPos, wait, target, wait);
  String r = "已跳到 " + mLibFmtSec(target);
  if (mLibDur > 0) r += "（本集共 " + mLibFmtSec((float)mLibDur) + "）";
  return r;
}

// STT 文本的本地兜底入口（**仅当服务器没做 MCP 握手时**才走到这里）：
// 返回 true = 这句话已被本机处理，不要再交给云端
static bool mLibHandleText(const String& t) {
  if (mLibMcpReady) return false;   // 云端能调工具：交给 LLM 判断，避免一次请求触发两遍
  int kind = mLibCmdKind(t);
  if (kind) {
    mLibOn = (kind == 1);
    mLibSave();
    if (mLibState) mLibStop("开关切换");
    if (wsReady) sendAbort();   // 云端不知道这个开关，让它答只会答错
    Serial.printf("[LIB] 指令「%s」→ 我的媒体库 %s\n", t.c_str(),
                  mLibOn ? "已启用（点歌默认走自建库）" : "已关闭（点歌交还云端）");
    mLibBeep(mLibOn);
    return true;
  }
  if (!mLibOn) return false;
  // ★V2.4.12：进度口令（「快进五分钟」「后退三十秒」「跳到第十分钟」）。
  //   与 MCP 两路互斥：mLibMcpReady 时本函数直接 return false，口令交给云端工具。
  {
    float s = 0, d = 0;
    int sk = mLibSeekQuery(t, s, d);
    if (sk) {
      if (wsReady) sendAbort();
      String res = mLibDoSeek(sk == 1, s, sk == 2, d);
      Serial.printf("[LIB] 进度口令「%s」→ %s\n", t.c_str(), res.c_str());
      mLibBeep(true);
      return true;
    }
  }
  String q;
  if (!mLibSongQuery(t, q)) return false;
  Serial.printf("[LIB] 点歌意图：「%s」→ 关键词「%s」\n", t.c_str(), q.c_str());
  if (wsReady) sendAbort();   // 立刻掐云端（它会先答一句"我没有音乐"）
  mLibPlayQuery(q);
  return true;
}

// ---------- 设备侧 MCP（小智 {"type":"mcp","payload":{...}}，与 R1 客户端同款）----------
// 场景：服务器连上就问 initialize / tools/list，把设备能力登记进 LLM 的工具表；
// 之后用户说「启用我的媒体库」「播放晴天」，云端直接调 tools/call 到这里。
// 相比 STT 关键字，这条路的意图识别在**云端 LLM**，不用我们猜句子结构。
// 报文（照 R1 客户端 XiaoZhiWs.smali）：
//   收 {"type":"mcp","payload":{"id":1,"method":"tools/call","params":{"name":"...","arguments":{...}}}}
//   回 {"type":"mcp","payload":{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"..."}],"isError":false}}}

// 取嵌套对象段（带花括号配对）。jsonSection 只找第一个 '}'，遇到 arguments 这种
// 嵌套对象会被截断，所以 MCP 解析统一用这个。
static String jsonObj(const String& s, const char* key) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return "";
  i += (int)pat.length();
  while (i < (int)s.length() && s[i] == ' ') i++;
  if (i >= (int)s.length() || s[i] != '{') return "";
  int depth = 0;
  bool inStr = false, esc = false;
  for (int j = i; j < (int)s.length(); j++) {
    char c = s[j];
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) return s.substring(i, j + 1);
    }
  }
  return "";
}

// 取布尔字段（找不到返回 false，用 out 带值）
static bool jsonBool(const String& s, const char* key, bool& out) {
  String pat = String("\"") + key + "\":";
  int i = s.indexOf(pat);
  if (i < 0) return false;
  i += (int)pat.length();
  while (i < (int)s.length() && s[i] == ' ') i++;
  if (i < (int)s.length() && s[i] == 't') { out = true;  return true; }
  if (i < (int)s.length() && s[i] == 'f') { out = false; return true; }
  return false;
}

// JSON 字符串值转义（中文按 UTF-8 原样过，只处理引号/反斜杠/控制字符）
static String mcpEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o;
}

static void mcpSend(const String& payloadJson) {
  if (!wsReady) return;
  String m = "{\"type\":\"mcp\",\"payload\":" + payloadJson + "}";
  ws.sendTXT(m);
  if (dlVerbose) Serial.println("[MCP] -> " + m);
}

static void mcpResult(int id, const String& text, bool isErr) {
  mcpSend("{\"jsonrpc\":\"2.0\",\"id\":" + String(id)
          + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + mcpEsc(text)
          + "\"}],\"isError\":" + (isErr ? "true" : "false") + "}}");
  Serial.printf("[MCP] <- result id=%d isError=%d：%s\n", id, isErr ? 1 : 0, text.c_str());
}

// 工具表。★V2.2.1：描述从 1.5KB 压到 ~0.6KB。
//   为什么必须压：每次按住说话都会**新建会话**，服务器每次都重新 initialize +
//   tools/list，这 1.5KB schema 会进 LLM 的每一次推理 —— 官方服务器实测首字延迟
//   明显变大，用户听感就是"说话卡卡的"。触发词留全，解释性文字全删。
static const char* MCP_TOOLS =
    "{\"tools\":["
    "{\"name\":\"self.stem.media_library\","
    "\"description\":\"开关设备自建曲库。on=true 启用（之后点歌走自建库），false 关闭，不传=查询。"
    "用户说「启用/关闭我的媒体库（曲库）」时调用。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}}}},"
    "{\"name\":\"self.stem.play_song\","
    "\"description\":\"从自建曲库放歌，可连续播多首。用户要听某首歌、某歌手、某个类型的歌时用它，"
    "不要用云端音乐搜索。keyword 传歌名或歌手原句。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"keyword\":{\"type\":\"string\"}},"
    "\"required\":[\"keyword\"]}},"
    "{\"name\":\"self.stem.play_radio\","
    "\"description\":\"播放网络电台/广播。用户说「放XX电台」「听广播」「听新闻台」时用它；"
    "name 传台名（如「中国之声」），不必报网址。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
    "\"required\":[\"name\"]}},"
    "{\"name\":\"self.stem.play_book\","
    "\"description\":\"播放有声书/评书/相声。用户说「听XX」「放某本书」时用它；keyword 传书名，"
    "episode 传集号（用户没点名就省略，从上次听到的那集续播）。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"keyword\":{\"type\":\"string\"},"
    "\"episode\":{\"type\":\"integer\"}},\"required\":[\"keyword\"]}},"
    "{\"name\":\"self.stem.next_play\","
    "\"description\":\"下一首/下一集。用户说「下一首」「换一个」「下一集」「跳过这首」时调用。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.stem.current_play\","
    "\"description\":\"查询当前播放内容与进度（歌名歌手 / 书名集数 / 电台台名）。"
    "问「现在放的是什么」「这集多长」「播到哪了」「还剩多久」时调用。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.stem.seek_play\","
    "\"description\":\"调整有声书进度。说「跳到第N秒/第N分钟」「从第N分钟开始播」传 sec（绝对秒）；"
    "说「快进/后退N秒(分钟)」传 delta（秒，正数快进负数后退，中文数量请换算成秒）。"
    "都不传则只查询当前进度。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sec\":{\"type\":\"number\"},"
    "\"delta\":{\"type\":\"number\"}}}},"
    "{\"name\":\"self.stem.signal_status\","
    "\"description\":\"查询设备 WiFi 信号强度与好坏结论。问「信号好不好」「网络怎么样」「怎么老卡」时调用。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.stem.stop_play\","
    "\"description\":\"停止当前播放。用户说「停」「别放了」「安静」「关掉音乐」时调用。\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
    "]}";

// 三个播放工具共用的前置校验（★V2.2.1 教训：宁可回"没配地址"，也绝不能谎报成功 ——
// 谎报会让云端照着说"已经帮你放啦"，而实际响的是它自己 search_music 搜来的版本）
static bool mLibReadyOrSay(int id) {
  if (!mLibOn) {
    mcpResult(id, "我的媒体库未启用：先调用 self.stem.media_library 并传 on=true", true);
    return false;
  }
  if (!mLibHostNow().length()) {
    mcpResult(id, "媒体库地址还没配置，现在放不了。请让用户在设备配网门户"
                  "「我的媒体库」里填 NAS 的 IP（端口默认 8100）后再试。", true);
    Serial.println("[LIB] 播放被拒：媒体库地址未配置（连官方服务器时无法自动推断）");
    return false;
  }
  return true;
}

static void mcpHandle(const String& msg) {
  String payload = jsonObj(msg, "payload");
  if (!payload.length()) return;
  int id = jsonInt(payload, "id");
  if (id < 0) return;                       // 通知类报文（无 id）不需要回
  String method = jsonStr(payload, "method");

  if (method == "initialize") {
    mLibMcpReady = true;
    mcpSend("{\"jsonrpc\":\"2.0\",\"id\":" + String(id)
            + ",\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
              "\"serverInfo\":{\"name\":\"alphapi-stem\",\"version\":\"2.4.13\"}}}");
    Serial.println("[MCP] initialize ✅（设备工具：媒体库开关 / 点歌单 / 电台 / 有声书 / 下一个 / 正在播放 / 停止）");
    return;
  }
  if (method == "tools/list") {
    mLibMcpReady = true;
    mcpSend("{\"jsonrpc\":\"2.0\",\"id\":" + String(id) + ",\"result\":" + MCP_TOOLS + "}");
    Serial.println("[MCP] tools/list → media_library / play_song / play_radio / play_book / next_play / current_play / seek_play / signal_status / stop_play");
    return;
  }
  if (method != "tools/call") {
    Serial.printf("[MCP] 忽略未支持的方法：%s\n", method.c_str());
    return;
  }

  String params = jsonObj(payload, "params");
  String name = jsonStr(params, "name");
  String args = jsonObj(params, "arguments");

  if (name == "self.stem.media_library") {
    bool want = false;
    if (!jsonBool(args, "on", want)) {   // 只查询
      mcpResult(id, String("我的媒体库当前") + (mLibOn ? "已启用，点歌走自建曲库" : "已关闭，点歌交还云端"), false);
      return;
    }
    if (want != mLibOn) {
      mLibOn = want;
      mLibSave();
      if (!mLibOn) {          // 关库即停：否则正在播的歌会继续放，和云端那句"已关闭"自相矛盾
        mLibPendingQuery = "";
        if (mLibState) mLibStop("工具关闭媒体库");
        if (dlActive) stopDlPlayHard();
      }
      // ★这里**不播提示音**：MCP 路径下云端本来就会说"已启用/已关闭"，
      //   播提示音等于把服务端那句话拦腰截断（提示音只在 STT 兜底路径用，那条已 abort 云端）。
    }
    mcpResult(id, mLibOn ? "我的媒体库已启用，之后点歌默认走自建曲库" : "我的媒体库已关闭，点歌交还云端", false);
    return;
  }

  if (name == "self.stem.play_song") {
    String q = jsonStr(args, "keyword");
    if (!q.length()) {
      mcpResult(id, "缺少 keyword（歌名或歌手）", true);
      return;
    }
    if (!mLibReadyOrSay(id)) return;
    // ★V2.4.1 抢跑：工具一回就把请求挂出去（loop 下一圈立刻取流），
    //   起播等云端确认语说完（见 loop 里的 mLibPendingQuery + mLibTick 的 canStart）
    mLibPendingKind = 1;
    mLibPendingPos = 0;
    mLibPendingQuery = q;
    mLibPendingAt = millis();
    mcpResult(id, "已从自建曲库取「" + q + "」，马上播放", false);
    return;
  }

  if (name == "self.stem.play_radio") {
    String n = jsonStr(args, "name");
    if (!n.length()) n = jsonStr(args, "keyword");
    if (!n.length()) {
      mcpResult(id, "缺少 name（台名，例如「中国之声」）", true);
      return;
    }
    if (!mLibReadyOrSay(id)) return;
    mLibPendingKind = 2;
    mLibPendingPos = 0;
    mLibPendingQuery = n;
    mLibPendingAt = millis();
    mcpResult(id, "正在打开电台「" + n + "」", false);
    return;
  }

  if (name == "self.stem.play_book") {
    String q = jsonStr(args, "keyword");
    if (!q.length()) q = jsonStr(args, "name");
    if (!q.length()) {
      mcpResult(id, "缺少 keyword（书名）", true);
      return;
    }
    if (!mLibReadyOrSay(id)) return;
    int ep = jsonInt(args, "episode");
    mLibPendingKind = 3;
    mLibPendingPos = (uint16_t)(ep > 0 ? ep : 0);   // 0 = 续播（服务端按上次那集）
    mLibPendingQuery = q;
    mLibPendingAt = millis();
    mcpResult(id, ep > 0 ? ("正在打开《" + q + "》第 " + String(ep) + " 集")
                         : ("正在从上次听到的位置打开《" + q + "》"), false);
    return;
  }

  if (name == "self.stem.next_play") {
    if (!mLibKind) {
      mcpResult(id, "当前没有在播的歌或有声书", true);
      return;
    }
    if (mLibKind == 2) {
      mcpResult(id, "电台是直播流，没有下一首；可以换一个台，或者说停", true);
      return;
    }
    bool isSong = (mLibKind == 1);
    mLibNext(true);
    mcpResult(id, isSong ? "已跳到下一首" : "已跳到下一集", false);
    return;
  }

  if (name == "self.stem.current_play") {
    // ★V2.4.10：信息来自服务端 X-Media-Title（歌名-歌手 / 书名·演播·第N集 / 台名）。
    // ★V2.4.12：有声书再答总时长 / 已播到 / 还剩，小智能直接回答「这集多长」「还剩多久」。
    if (!mLibKind) {
      mcpResult(id, "现在没有在播放", false);
      return;
    }
    const char* kn = (mLibKind == 3) ? "有声书" : (mLibKind == 2 ? "电台" : "歌曲");
    String t = mLibNowTitle.length() ? mLibNowTitle : mLibCurTitle;
    String extra;
    if (mLibKind == 3) {
      float now = mLibNowSec();
      extra = String("（第 ") + mLibPos + " 集";
      if (mLibDur > 0) extra += "，总时长 " + mLibFmtSec((float)mLibDur);
      extra += "，已播到 " + mLibFmtSec(now);
      if (mLibDur > 0) extra += "，还剩 " + mLibFmtSec((float)mLibDur - now);
      if (mLibPaused) extra += "，已暂停";
      extra += "）";
    } else if (mLibPaused) extra = "（已暂停）";
    mcpResult(id, String("正在播放") + kn + "：" + t + extra, false);
    return;
  }

  if (name == "self.stem.seek_play") {
    float sec = 0, delta = 0;
    bool hasSec = mLibJsonNum(args, "sec", sec);
    bool hasDelta = mLibJsonNum(args, "delta", delta);
    if (!hasSec && !hasDelta) {          // 只问进度，不跳转
      if (!mLibKind) { mcpResult(id, "现在没有在播放", false); return; }
      String r = "已播到 " + mLibFmtSec(mLibNowSec());
      if (mLibKind == 3 && mLibDur > 0)
        r += String("，本集共 ") + mLibFmtSec((float)mLibDur)
           + "，还剩 " + mLibFmtSec((float)mLibDur - mLibNowSec());
      mcpResult(id, r, false);
      return;
    }
    mcpResult(id, mLibDoSeek(hasSec, sec, hasDelta, delta), false);
    return;
  }

  if (name == "self.stem.signal_status") {
    mcpResult(id, mLibSignalText(), false);
    return;
  }

  if (name == "self.stem.stop_play") {
    mLibPendingQuery = "";
    if (mLibState) mLibStop("工具停止");
    if (dlActive) stopDlPlayHard();
    if (wsReady) sendAbort();
    mcpResult(id, "已停止播放", false);
    return;
  }

  mcpResult(id, "未知工具：" + name, true);
}


static void handleText(uint8_t* payload, size_t len) {
  String msg;
  msg.reserve(len);
  for (size_t i = 0; i < len; i++) msg += (char)payload[i];
  lastConvAt = millis();  // 任何服务器消息=会话活动（空闲断开计时基准）
  if (msg.indexOf("\"hello\"") >= 0) {
    wsReady = true;
    Serial.println("[WS] 服务器 hello 握手成功 ✅");
    Serial.println("[WS] " + msg);
    String sid = jsonStr(msg, "session_id");
    if (sid.length()) gSession = sid;
    // 采样率必须在 audio_params 段内取，防误匹配消息其他字段
    String ap = jsonSection(msg, "audio_params");
    int sr = ap.length() ? jsonInt(ap, "sample_rate") : jsonInt(msg, "sample_rate");
    if (sr > 0) rebuildDecoder(sr);
    else Serial.printf("[DL] hello 未含 sample_rate，解码器沿用 %dHz\n", decRate);
  } else {
    // ★V2.4.0 设备侧 MCP：initialize / tools/list / tools/call
    if (hasMsgType(msg, "mcp")) { mcpHandle(msg); return; }
    // ★V2.4.0 我的媒体库 STT 兜底：只在服务器没做 MCP 握手时生效（两路互斥）
    if (hasMsgType(msg, "stt") && !mLibMcpReady) {
      String stt = jsonStr(msg, "text");
      if (stt.length() && mLibHandleText(stt)) return;
    }
    Serial.println("[SRV] " + msg);
    if (mLibWaitTts) {
      // ★V2.4.8：只有"服务器真在生成回答"才算在说话的路上。以前是"任何文本都算"，
      //   情绪/附带字段一来就白等 6s —— 用户口径是没动静 0.5s 就该出声。
      String t = jsonStr(msg, "text");
      if (t.length()) mLibSawReply = true;
    }
    // tts start/stop 精确匹配（"state":"start" 不会误中 "sentence_start"）：
    // stop=流结束协议判据（pumpPlay 据此收尾，替代旧的空闲硬猜）
    if (msg.indexOf("\"type\":\"tts\"") >= 0) {
      if (msg.indexOf("\"state\":\"stop\"") >= 0) {
        ttsDone = true;
        dlFeedDone = true;
        dlRespOpen = false;  // 响应正常收尾
      } else if (msg.indexOf("\"state\":\"start\"") >= 0) {
        ttsDone = false;
        ttsSeq++;            // ★V2.4.7：本轮确认语开口（本地点播靠它判断"该等的是哪一轮"）
        dlRespOpen = true;   // ★V2.0.9：响应开始，等 stop 才算收尾（断开定责用）
        dlBarged = false;  // 新一轮 TTS = 解除打断封锁（旧响应残帧到此为止）
        mtxMusic = false;  // 新一轮响应：点歌标记复位（点阵回三角/音符由 startDlPlay 决定）
        if (!dlActive) dlTail = dlHead;  // 丢上一轮 tts stop 后到达的残留尾帧（防混入新响应开头）
      } else if (msg.indexOf("\"state\":\"sentence_start\"") >= 0) {
        // 点歌侦测：服务器先发 "% play_music..." 占位句（实测日志），命中即切音符图标
        if (msg.indexOf("% play_music") >= 0 || msg.indexOf("% search_music") >= 0) mtxMusic = true;
      }
    } else {
      // 情绪消息 {"type":"llm",...,"emotion":"happy"} → 点阵脸谱（播放/录音中只缓存，
      // endDlPlay/loop 的 mtxFlush 会在总线空闲时补上屏）
      String emo = jsonStr(msg, "emotion");
      if (emo.length()) mtxApplyEmotion(emo);
    }
    // ★未激活在线检测（V2.0.5）：服务器对已解绑设备会用 TTS 播报激活码——扫 text/message
    //   字段，命中即作废本地"(已激活)"缓存（长按 C 说话时会话正常但播报激活码那串日志即来源）。
    //   带 6 位码的才算数；无码时只认短句（防把 LLM 长回复里的"激活"字样误判）。
    {
      String txt = jsonStr(msg, "text");
      if (!txt.length()) txt = jsonStr(msg, "message");
      if (txt.length() && isActivationHint(txt)) {
        String code = extractActCode(txt);
        if (code.length() || txt.length() < 120) markNotActivated(code, txt);
      }
    }
  }
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsLastRxAt = millis();  // ★V2.1.0：死链检测基线
      wsRxMaxAll = 0; wsRxBigAll = 0; wsRxNTotal = 0;   // ★V2.1.3 新会话重新计数
      wsPingTx = 0; wsPingFail = 0; wsPingRx = 0; wsPongRx = 0;
      wsLastPingAt = millis(); wsLastPongAt = 0;
      Serial.printf("[WS] 已连接 MAC=%s\n", deviceId().c_str());
      sendHello();
      break;
    case WStype_DISCONNECTED:
      // ★V2.1.3：先把库给的原因抄下来（payload = reason 字符串，可能 NULL）
      wsDiscReason[0] = 0;
      if (payload && len) {
        size_t n = len < sizeof(wsDiscReason) - 1 ? len : sizeof(wsDiscReason) - 1;
        memcpy(wsDiscReason, payload, (size_t)n);
        wsDiscReason[n] = 0;
      }
      if (wsReady) {
        // ★V2.1.3：一行把"谁先动手"打在脸上 —— 库原因 + 心跳账本（服务器到底活没活）
        // ★V2.1.5：补「末次 pong 距今」——与 ping/pong 账本合起来判"是谁先不动"：
        //   末次 pong 很近（<15s）⇒ 断前服务器进程还活着；末次 pong 很久/从未 ⇒ 服务器或链路先死。
        Serial.printf("[WS] 连接断开 —— 库原因「%s」| ping 发 %lu 失败 %lu | "
                      "收到 ping %lu / pong %lu（末次 pong %ldms 前）—— %s\n",
                      wsDiscReason[0] ? wsDiscReason
                                      : "（空：服务端 close 帧 / 库内部 code 路径）",
                      (unsigned long)wsPingTx, (unsigned long)wsPingFail,
                      (unsigned long)wsPingRx, (unsigned long)wsPongRx,
                      (long)(wsLastPongAt ? (int32_t)(millis() - wsLastPongAt) : -1),
                      wsLastPongAt ? "服务器回应过 ping，进程是活的"
                                   : "全程无 pong，未见服务器心跳回应");
        // ★V2.0.9 定责：响应尚未收尾（tts start 已到、stop 未到）就断 = 服务器中途掉了，
        //   不是固件主动收场。下次报"讲着讲着没声"时，看这一行 + 上面 ⚠️ 服务器停供 行即定案。
        // ★V2.1.0：补"末次服务器消息"一栏 —— 判读时可一眼区分
        //   ①服务器先停供、再断开（末帧/末消息时长 ≈ 停供时长，本例 9.46s 属此类）；
        //   ②链路突然掉（两值都接近 0）。
        if (dlRespOpen && dlLastAudioAt && millis() - dlLastAudioAt < 60000) {
          Serial.printf("[WS] ⚠️ 服务器中途断开：响应未完成（已发 %lu B | 末帧 %lums 前 | "
                        "末次服务器消息 %lums 前）——服务端/链路问题，非固件收场\n",
                        (unsigned long)dlSentTotal,
                        (unsigned long)(millis() - dlLastAudioAt),
                        wsLastRxAt ? (unsigned long)(millis() - wsLastRxAt) : 0UL);
          // ★V2.4.3：本地媒体库播放中 ws 被踢是**正常现象**（播放期无上行，服务器约 80s
          //   断开会话），不是"响应未完成"，别打失望脸 —— 否则用户会以为音频流挂了。
          if (!mLibState) dlBroken = true;  // ★V2.1.0：交给 endDlPlay 打"失望脸"（收场那刻总线才空闲）
        }
      }
      // ★V2.1.2 诊断：断开瞬间的内存水位。判据 —— 若"本会话最大帧 + 1" > "最大连续块"，
      //   则凶手就是库里的 malloc(payloadLen+1) 失败（clientDisconnect 1011），
      //   与服务器无关；反之才是服务端/链路真的停了。
      {
        size_t contig = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        Serial.printf("[WS] 断开时：堆 %u / 连续 %u | 本会话入站 %lu 帧，最大 %u B"
                      "（>1KB 的 %lu 个）%s\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)contig,
                      (unsigned long)wsRxNTotal, (unsigned)wsRxMaxAll, (unsigned long)wsRxBigAll,
                      (wsRxMaxAll + 1 > contig)
                        ? "  ⚠️ 最大帧 > 最大连续块 ⇒ 库 malloc 失败直接断连（真凶在此）" : "");
      }
      wsRxN = 0; wsRxMax = 0; wsRxBig = 0;
      wsReady = false;
      // 任何断开都不自动重连：库 loop() 按间隔自动重连（默认 500ms，无禁用开关），
      // 压成 24h ≈ 禁用。重连统一走 ensureConnected（按住说话现连）
      ws.setReconnectInterval(86400000UL);
      Serial.println("[WS] 不自动重连（按住说话时现连）");
      break;
    // ★V2.1.0：任何服务器消息都刷新"死链检测"基线（含心跳 pong 之外的文本/音频）
    case WStype_TEXT: wsLastRxAt = millis(); handleText(payload, len); break;
    case WStype_BIN:
      wsLastRxAt = millis();
      // ★V2.1.2 诊断：入库帧大小统计（>1KB 视为大帧——库对超过最大连续块的帧会直接断连）
      wsRxN++;
      wsRxNTotal++;   // ★V2.1.3：会话累计（不复位）
      if ((uint32_t)len > wsRxMax) wsRxMax = (uint32_t)len;
      if (len > 1024) wsRxBig++;
      if ((uint32_t)len > wsRxMaxAll) wsRxMaxAll = (uint32_t)len;
      if (len > 1024) wsRxBigAll++;
      handleAudio(payload, len);
      break;
    // ★V2.1.3：心跳两向都记账。服务器回的 pong 是"它的进程还活着"的直接证据 ——
    //   停供期间若 pong 仍在，可当场判定"服务器卡在生成/工具调用，不是链路死"。
    //   注意：**不动 wsLastRxAt**（那是 payload 级死链基线），免得 pong 把僵死连接洗白。
    case WStype_PING: wsPingRx++; break;
    case WStype_PONG: wsPongRx++; wsLastPongAt = millis(); break;
    case WStype_ERROR: Serial.println("[WS] 错误"); break;
    default: break;
  }
}

// ★V2.1.3：自建心跳。库的 handleHBPing() 在 sendPing() 写失败时会直接断连（见变量区注释），
//   所以关掉库心跳、只保留我们这一条：10s 一次 ping，**写失败只记账不断连**——
//   真死链路由 `wsLastRxAt` 死链检测（ensureConnected）与 `clientIsConnected()` 兜底。
static void hbTick() {
  if (!wsReady) return;
  uint32_t now = millis();
  if (!wsLastPingAt) { wsLastPingAt = now; return; }
  if (now - wsLastPingAt < 10000) return;
  wsLastPingAt = now;
  if (ws.sendPing()) wsPingTx++;
  else              wsPingFail++;
}

// ---------- Opus ----------
static void opusInit() {
  int err;
  // AUDIO 模式对齐 S2 官方（AUDIO + 24kbps + c3 + VBR）；c3 留音质冗余（无 AEC/NS 前端）
  enc = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &err);
  if (err != OPUS_OK) {
    Serial.printf("[OPUS] 编码器创建失败 %d\n", err);
    return;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
  opus_encoder_ctl(enc, OPUS_SET_VBR(1));
  dec = opus_decoder_create(decRate, 1, &err);  // 默认 24k，hello 到达后按实际重建
  if (err != OPUS_OK) {
    Serial.printf("[OPUS] 解码器创建失败 %d\n", err);
    return;
  }
  Serial.printf("[OPUS] 就绪 %s（上行16k/60ms AUDIO 裸流/下行默认%dk）\n",
                opus_get_version_string(), decRate / 1000);
}

// ---------- 官方 OTA ----------
// 激活协议（官方 ota.cc 对照）：
//   ① GET OTA_URL（带 Activation-Version 头）→ 未激活设备响应含 activation.code
//   ② 设备侧必须 POST OTA_URL+"activate" 轮询：202=码未录入、200=激活成功
//   ③ 激活后重新 GET → websocket 段才带可用 token
//   ⚠️ 判据必须先看 activation.code：带码时 token 即使存在也不可用
static int otaActivatePost() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  String url = String(OTA_URL);
  if (!url.endsWith("/")) url += "/";
  url += "activate";
  if (!http.begin(cli, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Device-Id", deviceId());
  http.addHeader("Activation-Version", "1");  // 无序列号设备= v1
  int code = http.POST("{}");
  http.end();
  return code;
}

static void officialOta() {
  static bool otaReentry = false;  // 激活成功后递归拉 token 一次；防服务器异常时无限递归
  Serial.println("[OTA] 请求虾哥官方服务器...");
  WiFiClientSecure cli;
  cli.setInsecure();  // 极简版跳过证书校验
  HTTPClient http;
  if (!http.begin(cli, OTA_URL)) {
    Serial.println("[OTA] 连接失败");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Device-Id", deviceId());
  http.addHeader("Activation-Version", "1");
  String body = "{\"application\":{\"version\":\"1.0.0\",\"elf_sha256\":\"\"},"
                "\"board\":{\"type\":\"AlphaPi-STEM\",\"mac\":\""
                + deviceId() + "\"},"
                               "\"language\":\"zh-CN\"}";
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[OTA] HTTP %d\n", code);
    http.end();
    return;
  }
  String resp = http.getString();
  http.end();

  // 按段提取：firmware.url 是空串会干扰全局查找，必须只在 websocket/activation 段内找
  String wsSection = jsonSection(resp, "websocket");
  String actSection = jsonSection(resp, "activation");
  String wsUrl = jsonStr(wsSection, "url");
  String token = jsonStr(wsSection, "token");
  String actCode = jsonStr(actSection, "code");

  // ---- 未激活分支必须先于 token 判断（官方语义：带 activation.code = 未激活）----
  if (actCode.length()) {
    gActCode = actCode;  // Web 配置页显示用（NVS 缓存，断电不丢）
    prefs.begin("cfg", false);
    prefs.putString("actcode", actCode);
    prefs.end();
    Serial.println("==========================================");
    Serial.printf("[OTA] 需要激活！激活码: %s\n", actCode.c_str());
    Serial.println("[OTA] 打开 xiaozhi.me 登录 -> 控制台 -> 设备管理 -> 添加设备");
    Serial.println("[OTA] 输入激活码后本设备会自动完成激活（/activate 轮询）");
    Serial.println("==========================================");
    // 设备侧激活收尾：官方 Activate() 同款轮询（202=等输码，200=成功）
    if (!otaReentry) {
      otaReentry = true;
      for (int i = 0; i < 5; i++) {
        int rc = otaActivatePost();
        Serial.printf("[OTA] /activate 轮询 %d: HTTP %d（202=等待输码）\n", i + 1, rc);
        if (rc == 200) {
          Serial.println("[OTA] 激活成功，重新拉取 token...");
          officialOta();  // 递归一次：激活后 GET 才返回可用 token
          otaReentry = false;
          return;
        }
        if (rc != 202) break;  // 其他错误码不再轮询
        delay(2000);
      }
      otaReentry = false;
    }
    return;
  }

  if (token.length() && wsUrl.length()) {
    useSSL = wsUrl.startsWith("wss");
    String rest = wsUrl.substring(wsUrl.indexOf("://") + 3);
    int slash = rest.indexOf('/');
    String hostport = (slash >= 0) ? rest.substring(0, slash) : rest;
    curPath = (slash >= 0) ? rest.substring(slash) : "/xiaozhi/v1/";
    int colon = hostport.indexOf(':');
    if (colon >= 0) {
      curHost = hostport.substring(0, colon);
      curPort = hostport.substring(colon + 1).toInt();
    } else {
      curHost = hostport;
      curPort = useSSL ? 443 : 80;
    }
    curToken = token;
    saveServer(curHost, curPort, curPath, curToken, useSSL);
    saveOfficialSnapshot();  // 快照：切本地后再切回官方免 OTA
    gActCode = "(已激活)";  // Web 页状态显示
    prefs.begin("cfg", false);
    prefs.putString("actcode", gActCode);
    prefs.end();
    Serial.printf("[OTA] 激活完成 ✅ %s://%s:%u%s\n", useSSL ? "wss" : "ws",
                  curHost.c_str(), curPort, curPath.c_str());
    wsReady = false;
    ensureConnected();  // 统一走按需连接（内部处理重连间隔恢复/压回）
    return;
  }

  Serial.printf("[OTA] 响应无法解析（共 %u 字节）：\n", (unsigned)resp.length());
  Serial.println(resp.substring(0, 1200));
}

// 切服务器前的公共收尾：结束会话 + 强制断开旧 socket。
// ★必须先 ws.disconnect()：useOfficial 曾只置 wsReady=false 就 ensureConnected()，
//   beginSSL 作用在还连着本地服务器的旧明文 socket 上 → TLS 握手永远出不来 → 6s 超时。
//   （串口 'r' 命令一直是对的：先 disconnect 再 connect——本函数对齐该时序）
static void serverSwitchTeardown() {
  if (recPhase) stopTalk();              // 说话中：先干净收尾
  if (dlActive) endDlPlay("切换服务器");  // 播放中：停播灭屏（残帧随断连作废）
  wsReady = false;
  ws.disconnect();
  delay(50);                             // 等 DISCONNECTED 事件出队
}

static void useLocal() {
  if (useSSL) saveOfficialSnapshot();  // 切走前快照官方配置（切回免 OTA）
  serverSwitchTeardown();
  clearServer();
  Serial.println("[SRV] 已清除官方配置，改用本地服务器");
  loadServer();
  printServer();
}

// 切回官方：快照优先（纯内存恢复，0.1s）；无快照才走 OTA（HTTPS，堆紧张时可能失败）
static void useOfficial() {
  if (loadOfficialSnapshot()) {
    serverSwitchTeardown();
    saveServer(curHost, curPort, curPath, curToken, useSSL);  // 活动槽同步恢复
    // ★不再无条件宣称"(已激活)"（V2.0.5）：快照只是本地存下的旧 token，**不校验服务器
    //   绑定状态**。官网解绑后该 token 已失效，若此处强行把状态写成"已激活"，就会成为
    //   网页状态骗人的第二个来源。保留 gActCode 原值，门户页会标注"本地缓存"并提示实查。
    printServer();
    ensureConnected();
    Serial.println("[SRV] 已切回官方服务器（本地快照恢复，未走 OTA）");
    Serial.println("[SRV] ⚠ 快照不校验绑定状态：若官网已解绑该设备，快照 token 必失效 —— "
                   "发 'a' 或网页点「联网核对激活状态」实查");
  } else {
    Serial.println("[SRV] 无官方快照，走 OTA 激活...");
    officialOta();
  }
}

// ---------- Web 配置门户（串口 w 进入 / WiFi 连不上自动进） ----------
// 设计：AP+STA 双模。配置模式下手机连 AP "AlphaPi-XZ-xxxx" 开 http://192.168.4.1/
// 功能：①配 WiFi（存 NVS 重启生效）②配本地/官方服务器（立即生效）③显示/刷新激活码
static String escHtml(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

// ---------- 一次性动作令牌（V2.0.8）----------
// ★为什么非有不可：门户页的动作全是 **GET 表单**，而浏览器在**刷新（F5）/ 前进后退 /
//   预取 / 重开标签页**时都会**重发同一个 GET 请求**——于是 `/recheck` 点一次、再按一次 F5，
//   设备就被又写一次 actboot、又重启一次（用户实测：`192.168.4.1/recheck` 刷新导致误重启）。
//   GET 本该是幂等的，"重启设备"这种副作用动作不能裸挂在 GET 上（也没法给手机浏览器讲道理）。
//   做法：每次渲染页面生成一个新随机令牌塞进所有表单；handler 先校验，且**消费即作废**。
//   刷新 = 拿旧令牌重发 ⇒ 直接拒绝并提示，绝不重复执行。
static uint32_t cfgToken = 0;
static String cfgNewToken() {
  cfgToken = esp_random() | 1u;   // 0 保留 = 无有效令牌
  return String(cfgToken);
}
static bool cfgCheckToken() {
  if (!cfgToken) return false;                          // 未发过 / 已被消费
  if (cfgWeb->arg("t") != String(cfgToken)) return false;  // 旧页面重放
  cfgToken = 0;                                         // 一次性：消费即作废
  return true;
}

static void cfgSendPage(const String& msg = "") {
  String tok = cfgNewToken();   // 每次渲染都换新令牌：本页每个表单只能用一次
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>AlphaPi Xiaozhi 配置</title><style>"
             "body{font-family:sans-serif;margin:14px;max-width:480px;color:#222}"
             "fieldset{margin:12px 0;padding:10px}input{width:96%;margin:3px 0;padding:4px}"
             "button{padding:7px 16px;margin-top:6px}.v{color:#666;font-size:13px}"
             "code{background:#eee;padding:2px 6px;border-radius:3px}"
             ".msg{background:#e8f5e9;padding:8px;border-radius:4px}</style></head><body>";
  if (msg.length()) h += "<p class='msg'>" + msg + "</p>";
  // ★全新设备（WiFi 未配置）：必须先配网，激活才有意义——给最醒目的引导横幅
  if (wifiSsid.length() == 0) {
    h += "<p style='background:#fff3cd;border:1px solid #ffc107;padding:8px;border-radius:4px'>"
         "<b>新设备 · 未配网</b><br>请先在下方「WiFi」区块填入路由器 SSID 与密码，"
         "点「保存并重启」。设备联网后才能连官方服务器获取激活码。</p>";
  }
  String mac = WiFi.macAddress();
  h += "<h3>AlphaPi Xiaozhi " XZ_FW_VERSION XZ_FW_TAG " 配置</h3>";
  // 当前模式徽章：本地=绿 / 官方=红 / 未配置=红，一眼可辨（重启切换架构下刷新即见真相）
  // ★V2.0.6：切换按钮必须知道"现在是什么模式"，才能判断点了算不算"真切换"
  bool isOfficialMode = useSSL;
  bool isLocalMode = (!useSSL && curHost.length() > 0);
  String modeBadge;
  if (!useSSL && curHost.length() == 0) {
    modeBadge = "<b style='color:#c33'>未配置（新设备）</b>";   // 空 host 的"本地服务器"会误导
  } else if (useSSL) {
    modeBadge = "<b style='color:#c33'>官方服务器（wss）</b>";
  } else {
    modeBadge = "<b style='color:#2a7'>本地服务器</b> " + escHtml(curHost) + ":" + String(curPort);
  }
  String actBadge = (gActCode == "(已激活)") ? " · 已激活" : "";
  h += "<p style='font-size:16px'>当前模式: " + modeBadge + actBadge + "</p>";
  // ★待生效配置（读 NVS 活动槽 / actboot 标志）：切换 = "写 NVS + 重启"，页面必须让用户
  //   看见"点击已记下、重启后会变成什么"，否则极易误判成"点了没反应"。
  {
    prefs.begin("xiaozhi", true);
    String ph = prefs.getString("host", "");
    bool pssl = prefs.getBool("ssl", false);
    uint16_t pport = prefs.getUShort("port", 8000);
    prefs.end();
    prefs.begin("cfg", true);
    bool pact = prefs.getBool("actboot", false);
    prefs.end();
    String pend;
    // ★V2.0.6：NVS 活动槽 == 运行时当前值 ⇒ 没有待生效的变更，别让用户以为"还得重启"
    bool sameAsRun = (pssl == useSSL) && (pssl || (ph == curHost && pport == curPort));
    if (pact) pend = "官方服务器（wss）· 开机自动获取激活码";
    else if (sameAsRun) pend = "无 —— 当前配置已生效，无需重启";
    else if (ph.length()) pend = pssl ? String("官方服务器（wss）")
                                      : ("本地服务器 " + escHtml(ph) + ":" + String(pport));
    else pend = "本地服务器（烧录常量 / NVS l* 键）";
    h += "<p class='v'>重启后生效: <b>" + pend + "</b> · 切换需等设备重启完成（约 15 秒）</p>";
  }
  h += "<p class='v'>Device-Id: <code>" + mac + "</code><br>"
       "信号: " + String(WiFi.RSSI()) + " dBm · 堆: " + String(ESP.getFreeHeap() / 1024) + " KB</p>";
  // ★V2.4.10：正在播放（X-Media-Title）—— 歌名歌手 / 书名演播集题 / 台名
  if (mLibKind) {
    const char* kn = (mLibKind == 3) ? "有声书" : (mLibKind == 2 ? "电台" : "歌曲");
    h += String("<p class='v'>正在播放") + kn + ": <b>"
         + escHtml(mLibNowTitle.length() ? mLibNowTitle : mLibCurTitle) + "</b>"
         + (mLibKind == 3 ? (String("（第 ") + mLibPos + " 集）") : "")
         + (mLibPaused ? " · 已暂停" : "") + "</p>";
  }
  String actStatus;
  if (gActCode == "(已激活)") {
    actStatus = "<p>状态: <b style='color:#2a7'>已激活</b>（本地缓存）</p>"
                "<p class='v'>此状态来自设备本地记录——若已在 xiaozhi.me <b>解绑/删除</b>该设备，"
                "这里<b>不会</b>自动变化。点下方按钮联网向服务器实查即可同步。</p>";
  } else if (gActCode.length()) {
    actStatus = "<p>状态: <b style='color:#c33'>未激活</b> · 激活码: <code>" + escHtml(gActCode) + "</code></p>"
                "<p class='v'>在 xiaozhi.me 输入激活码后，点下方按钮完成激活（设备自动拉取 token）。</p>";
  } else if (wifiSsid.length() == 0) {
    actStatus = "<p>状态: <b style='color:#c33'>未激活</b> · 需先配置 WiFi（见上方黄色提示）</p>";
  } else {
    actStatus = "<p>状态: <b>未激活</b>（点下方按钮连官方服务器获取激活码）</p>";
  }
  h += "<fieldset><legend>激活</legend>" + actStatus
       + "<p class='v'>用本地服务器（192.168.x.x）则无需激活。</p>";
  if (isOfficialMode) {
    // ★V2.0.7：官方模式下**去掉"刷新状态"按钮**——门户模式（AP+STA）跑不了 TLS，
    //   它只能重刷页面、激活状态纹丝不动，点了像"没反应"（用户实测反馈）。
    //   官方模式下真正有用的只有一件事：让服务器说话 ⇒ /recheck。
    h += "<form action='/recheck'><input type='hidden' name='t' value='" + tok + "'>"
         "<button>重启并联网核对激活状态（约 15 秒）</button></form>"
         "<p class='v'>当前已是官方服务器，模式无需切换。本按钮 = 写标志 + 重启，重启后在纯 STA "
         "环境跑 OTA 实查（门户模式 AP+STA 并存跑 TLS 会崩，故不做即时查询）——"
         "<b>只有它才能同步服务器端的真实绑定状态</b>（例如官网已解绑）。</p>";
  } else {
    h += "<form action='/activate'><input type='hidden' name='t' value='" + tok + "'>"
         "<button>切换到官方服务器 / 获取激活码（重启生效）</button></form>"
         "<p class='v'>按钮 = 写标志 + 重启，重启后在纯 STA 环境实查（门户模式跑 TLS 会崩，故不做即时查询）。</p>";
  }
  h += "</fieldset>";
  h += "<fieldset><legend>WiFi</legend><p class='v'>当前: " + escHtml(wifiSsid) + "</p>"
       "<form action='/savewifi'><input type='hidden' name='t' value='" + tok + "'>"
       "SSID:<input name='ssid' value='" + escHtml(wifiSsid) + "'>"
       "密码:<input name='pass' type='password' value='" + escHtml(wifiPass) + "'>"
       "<button>保存并重启</button></form></fieldset>";
  // 服务器区：官方固定不可改（由激活流程管理），表单只编辑本地服务器
  String lh, lp, lpa, lt;
  prefs.begin("cfg", true);
  lh = prefs.getString("lhost", LOCAL_HOST);
  lp = String(prefs.getUShort("lport", LOCAL_PORT));
  lpa = prefs.getString("lpath", LOCAL_PATH);
  lt = prefs.getString("ltoken", LOCAL_TOKEN);
  prefs.end();
  String srvInfo;
  if (useSSL) srvInfo = "官方服务器（固定，由激活流程管理，切回官方点上方激活按钮）";
  else if (curHost.length()) srvInfo = "ws://" + escHtml(curHost) + ":" + String(curPort) + escHtml(curPath);
  else srvInfo = "未配置（新设备：填下方本地服务器，或点上方激活按钮连官方）";
  // ★V2.0.6：已是本地模式 ⇒ 按钮不再宣称"切换"，同模式且参数没改就只刷新不重启
  String localBtn = isLocalMode ? "保存本地服务器配置（当前已是本地模式 · 未改动则不重启）"
                                : "保存并切换到本地服务器（重启生效）";
  h += "<fieldset><legend>服务器</legend>"
       "<p class='v'>当前: " + srvInfo + "</p>"
       "<form action='/uselocal'><input type='hidden' name='t' value='" + tok + "'>"
       "本地服务器主机:<input name='host' value='" + escHtml(lh) + "'>"
       "端口:<input name='port' value='" + lp + "'>"
       "路径:<input name='path' value='" + escHtml(lpa) + "'>"
       "令牌:<input name='token' value='" + escHtml(lt) + "'>"
       "<button>" + localBtn + "</button></form>"
       "<p class='v'>当前已是本地模式且这四项都没改时，点按钮<b>只刷新状态、设备不重启</b>；"
       "改过任一项才写入并重启生效。</p></fieldset>";
  // ★V2.4.13 补回：这个区块在 V2.4.0 重构门户时被误删，但代码提示 / README / 刷机页
  //   一直写着「配网门户『我的媒体库』填 IP」—— 这就是设置页找不到的原因。
  //   V2.2.0 原文见 AlphaPi_Xiaozhi/AlphaPi_Xiaozhi.ino；此处按 V2.4.13 的
  //   mLibOn / mLibHostNow() / mLibSave() / /ap/play 现状重写。
  {
    String eff = mLibHostNow();
    String ml = "<fieldset><legend>我的媒体库（自建曲库）</legend>";
    ml += "<p class='v'>当前: <b>" + String(mLibOn ? "已启用" : "已关闭") + "</b> · 目标 <b>"
          + (eff.length() ? escHtml(eff) : String("（未配置）")) + ":" + String(mLibPort) + "</b></p>";
    if (useSSL && !mLibHost.length()) {
      ml += "<p style='color:#c33'>当前连的是<b>官方小智服务器</b>，媒体库地址无法自动推断，"
            "<b>必须在下面填媒体库主机的 IP</b>，否则点歌会被拒。</p>";
    } else if (!mLibHost.length()) {
      ml += "<p class='v'>主机留空 = 沿用上方「服务器」里那台本地小智服务器的 IP（端口默认 8100）。</p>";
    }
    ml += "<form action='/savemlib'><input type='hidden' name='t' value='" + tok + "'>";
    ml += "主机:<input name='host' value='" + escHtml(mLibHost)
          + "' placeholder='NAS / Podroid 的 IP；留空=沿用本地服务器'>";
    ml += "端口:<input name='port' value='" + String(mLibPort) + "'>";
    ml += "开关:<select name='on'><option value='1" + String(mLibOn ? "' selected>" : "'>") + "启用</option>";
    ml += "<option value='0" + String(mLibOn ? "'>" : "' selected>") + "关闭</option></select>";
    ml += "<button>保存（立即生效，不重启）</button></form>";
    ml += "<p class='v'>设备从这里取流：<code>/ap/play?kind=song|radio|book&amp;q=</code>，"
          "服务端现转成 8bit/24000 单声道裸 PCM。启用后说「播放XXX」「听凡人修仙传」"
          "由本机直接播放；也可说「关闭我的媒体库」交还云端。</p></fieldset>";
    h += ml;
  }
  h += "<fieldset><legend>操作</legend>"
       "<form action='/reboot'><input type='hidden' name='t' value='" + tok + "'>"
       "<button>重启设备</button></form>"
       "<p class='v'>所有按钮都是一次性的：<b>刷新页面 / 回退再提交不会重复执行</b>"
       "（旧令牌会被拒绝，设备不会误重启）。</p></fieldset></body></html>";
  cfgWeb->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  cfgWeb->send(200, "text/html", h);
}

// 旧令牌 / 无令牌请求的统一回应：只提示，绝不执行动作（V2.0.8）
static void cfgStalePage() {
  Serial.println("[CFG] 拒绝重放的旧请求（令牌已消费）——设备不重启");
  cfgSendPage("⚠️ 这个操作<b>已经执行过了</b>（你刷新了页面，或点的是浏览器缓存的旧页面）——"
              "<b>设备不会重复重启</b>。<br>如需再执行一次，请在下方重新点一次按钮。");
}

static void cfgSaveWifi() {
  if (!cfgCheckToken()) { cfgStalePage(); return; }
  String ssid = cfgWeb->arg("ssid"), pass = cfgWeb->arg("pass");
  if (!ssid.length()) { cfgSendPage("SSID 不能为空"); return; }
  wifiSsid = ssid; wifiPass = pass;
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.printf("[CFG] WiFi 已保存: %s —— 3 秒后重启\n", ssid.c_str());
  cfgWeb->send(200, "text/html",
               "<meta charset='utf-8'><p>已保存，设备重启中…请稍后用新 WiFi 连接。</p>");
  cfgRebootAt = millis() + 2500;
}

static void cfgUseLocal() {
  if (!cfgCheckToken()) { cfgStalePage(); return; }
  String host = cfgWeb->arg("host");
  uint16_t port = (uint16_t)cfgWeb->arg("port").toInt();
  if (!host.length()) { cfgSendPage("主机不能为空"); return; }
  String path = cfgWeb->arg("path"); if (!path.length()) path = "/xiaozhi/v1/";
  String token = cfgWeb->arg("token");
  if (!port) port = 8000;
  // ★V2.0.6：目标模式 == 当前模式，且 host/端口/路径/令牌一字未改 ⇒ 这不是"切换"，
  //   只是点了一下当前模式那个按钮：**只刷新页面状态**，不写 NVS、不重启。
  //   （网络不稳时用户常反复点按钮确认，每次白等 15 秒重启纯属折磨）
  bool sameAsCur = (!useSSL && host == curHost && port == curPort &&
                    path == curPath && token == curToken);
  if (sameAsCur) {
    Serial.println("[CFG] 点的是当前模式按钮（本地）：仅刷新状态，不重启");
    cfgSendPage("ℹ️ 当前已是<b>本地服务器</b>模式，配置未变化——<b>仅刷新状态，设备不重启</b>。"
                "<br>改了主机/端口/路径/令牌任一项后再点，才会写入并重启生效。");
    return;
  }
  // 存本地服务器配置（cfg namespace，l* 键）+ 写活动槽（固定 ws 非 ssl）
  if (useSSL) saveOfficialSnapshot();  // 切走前快照官方配置（切回免 OTA）
  prefs.begin("cfg", false);
  // ★清残留的"开机切官方"请求：若用户先点了激活（写了 actboot）又改点本地，
  //   残留标志会让重启后的开机 OTA 反过来盖掉刚保存的本地配置（表现为"点了本地却没生效"）
  prefs.putBool("actboot", false);
  prefs.putString("lhost", host);
  prefs.putUShort("lport", port);
  prefs.putString("lpath", path);
  prefs.putString("ltoken", token);
  prefs.end();
  useSSL = false;
  curHost = host; curPort = port; curPath = path; curToken = token;
  saveServer(curHost, curPort, curPath, curToken, false);
  printServer();
  // ★切换即重启（V2.0.2 定案）：门户模式下（AP+STA + WebServer）跑 TLS 会崩
  //   （handler 栈 / 主循环都实测崩过），唯一稳定解 = 写活动槽 + 重启。
  //   重启后纯 STA + 堆全新，loadServer 直读。
  cfgSendPage("✅ 已切换到本地服务器，设备 2 秒后重启生效…");
  cfgRebootAt = millis() + 2000;
}

// 切换到官方服务器（页面只在非官方模式渲染本入口；官方模式下只剩 /recheck）
static void cfgActivate() {
  if (!cfgCheckToken()) { cfgStalePage(); return; }
  // ★V2.0.7：官方模式下页面已不再渲染这个入口，这里只兜住"手工敲 URL / 浏览器缓存的旧页面"，
  //   一律只提示、绝不重启（别让一个失效入口又把设备踢去重启）。
  if (useSSL) {
    Serial.println("[CFG] 已是官方模式，忽略 /activate：仅提示，不重启");
    cfgSendPage("ℹ️ 当前已是<b>官方服务器</b>模式，无需切换——<b>设备未重启</b>。"
                "<br>要同步服务器端的真实绑定状态（例如官网已解绑），请用"
                "「重启并联网核对激活状态」。");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    cfgSendPage("设备尚未连上 WiFi（STA），无法联网核对激活状态。"
                "请先在下方「WiFi」区块配置并重启。");
    return;
  }
  // ★★切换必走 OTA 实查（V2.0.5 修复）：旧版有官方快照时直接把快照 token 写进活动槽、
  //   并顺手把 gActCode 置成"(已激活)"——**全程没问过服务器**。于是官网**解绑设备**后
  //   网页依旧显示"已激活"（本地缓存骗人），一直要等长按 C 说话、服务器用 TTS 播报
  //   激活码才暴露。激活状态只有服务器说了算 ⇒ 必须实查。
  //   路径仍是"写 NVS + 重启"（门户模式跑 TLS 会崩，V2.0.2 定案不变）：
  //     · 服务器认为已绑定 → OTA 返回 websocket.token（顺手刷新 token）
  //     · 已解绑 / 未激活 → OTA 返回 activation.code（新激活码，网页与串口都能看到）
  //   ★不清活动槽：OTA 万一失败（断网/服务器异常），原服务器配置仍可用，不至于变砖。
  prefs.begin("cfg", false);
  prefs.putBool("actboot", true);
  prefs.end();
  Serial.println("[CFG] 网页请求切换到官方服务器：写 actboot 标志，重启后 OTA 实查");
  cfgSendPage("✅ 设备 2 秒后重启，切换到官方服务器并联网<b>实查</b>激活状态…"
              "<br>重启完成后（约 15 秒）重新连接设备热点刷新本页：「激活」区块会显示服务器"
              "返回的真实状态——已绑定显示「已激活」，已解绑则显示新的激活码。串口同步打印进度。");
  cfgRebootAt = millis() + 2000;
}

// 「重启并联网核对激活状态」：已经是官方模式、只是想同步服务器端绑定状态时用（V2.0.6 新增）
// ★V2.0.8：一次性令牌拦住"刷新这个 URL 就再重启一次"（用户实测踩到过）。
static void cfgRecheck() {
  if (!cfgCheckToken()) { cfgStalePage(); return; }
  if (WiFi.status() != WL_CONNECTED) {
    cfgSendPage("设备尚未连上 WiFi（STA），无法联网核对激活状态。"
                "请先在下方「WiFi」区块配置并重启。");
    return;
  }
  prefs.begin("cfg", false);
  prefs.putBool("actboot", true);   // 只写标志：活动槽保持官方配置不动，重启后直连官方跑 OTA
  prefs.end();
  Serial.println("[CFG] 网页请求「重启并联网核对激活状态」：写 actboot 标志，重启后 OTA 实查");
  cfgSendPage("✅ 设备 2 秒后重启，联网向官方服务器<b>实查</b>激活状态…"
              "<br>重启完成后（约 15 秒）刷新本页：「激活」区块显示服务器返回的真实状态——"
              "已绑定显示「已激活」，已解绑则显示新的激活码。串口同步打印进度。");
  cfgRebootAt = millis() + 2000;
}

// ★V2.4.13 补回（V2.2.0 原有、V2.4.0 误删）：保存「我的媒体库」地址 / 端口 / 开关。
// 不重启 —— 只是 HTTP 客户端参数，改完立即生效。host 留空 = 沿用本地小智服务器那台机器。
static void cfgSaveMLib() {
  if (!cfgCheckToken()) { cfgStalePage(); return; }
  String host = cfgWeb->arg("host");
  host.trim();
  uint16_t port = (uint16_t)cfgWeb->arg("port").toInt();
  if (!port) port = 8100;
  bool on = (cfgWeb->arg("on") == "1");
  mLibHost = host;
  mLibPort = port;
  mLibOn = on;
  mLibSave();                       // cfg 命名空间：mlhost / mlport / mlon
  if (!mLibOn && mLibState) mLibStop("网页关闭媒体库");
  Serial.printf("[CFG] 我的媒体库: %s:%u %s\n",
                mLibHost.length() ? mLibHost.c_str() : "(沿用本地服务器)",
                mLibPort, mLibOn ? "启用" : "关闭");
  String shown = mLibHost.length() ? mLibHost
                 : (mLibHostNow().length() ? mLibHostNow() : String("(未配置)"));
  cfgSendPage(String("✅ 我的媒体库已保存（立即生效，设备不重启）：") + shown
              + ":" + String(mLibPort) + " · " + (mLibOn ? "已启用" : "已关闭"));
}

static void cfgStart() {
  if (cfgMode) return;
  ws.disconnect();
  WiFi.mode(WIFI_AP_STA);           // 保 STA：网页可刷新激活码
  String mac = WiFi.macAddress(); mac.replace(":", "");
  String ap = "AlphaPi-XZ-" + mac.substring(8);
  WiFi.softAP(ap.c_str());          // 开放网络，配置页无敏感操作风险
  cfgWeb = new WebServer(80);
  cfgWeb->on("/", []() { cfgSendPage(); });
  cfgWeb->on("/savewifi", cfgSaveWifi);
  cfgWeb->on("/uselocal", cfgUseLocal);
  cfgWeb->on("/activate", cfgActivate);
  cfgWeb->on("/recheck", cfgRecheck);   // V2.0.6：重启并实查激活状态（与"切换"解耦）
  cfgWeb->on("/savemlib", cfgSaveMLib); // ★V2.4.13 补回：我的媒体库（地址/开关，即时生效不重启）
  cfgWeb->on("/reboot", []() {
    if (!cfgCheckToken()) { cfgStalePage(); return; }
    cfgWeb->send(200, "text/html", "<meta charset='utf-8'><p>重启中…</p>");
    cfgRebootAt = millis() + 1200;
  });
  cfgWeb->begin();
  cfgMode = true;
  Serial.printf("[CFG] 配置模式开启：手机连 AP \"%s\"，网页 http://%s/\n",
                ap.c_str(), WiFi.softAPIP().toString().c_str());
}

static void cfgStop() {
  if (!cfgMode) return;
  if (cfgWeb) { cfgWeb->stop(); delete cfgWeb; cfgWeb = nullptr; }
  cfgMode = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.println("[CFG] 配置模式关闭，回 STA");
}

// A 键长按 2s → 切换 AP 配网门户开/关（V2.0.2：切的是 AP 热点，不是 STA！
// STA 全程保持连路由器，小智不受影响）。开=手机连 "AlphaPi-XZ-xxxx" 进配置网页
// （切服务器/配 WiFi/激活）；关=门户收摊回纯 STA。切换后点阵显示 WiFi 图标 1s 提醒。
// 说话/播放中忽略（防切断会话）；门户开着时长按 A = 关门户。
static void pollBtnA() {
  bool pressed = digitalRead(BTN_A_PIN);
  if (pressed && !btnAHeld) {
    btnAHeld = true;
    btnAAt = millis();
    btnALong = false;
  } else if (!pressed && btnAHeld) {
    btnAHeld = false;
    // ★V2.2.3：A 单击（<800ms）= 音量 -。长按已处理过的这次松开不再算单击。
    uint32_t held = millis() - btnAAt;
    if (!btnALong && held > 30 && held < 800) volStep(-1);
  }
  if (btnAHeld && millis() - btnAAt >= 2000) {
    btnAHeld = false;
    btnALong = true;
    if (recPhase || dlActive) {
      Serial.println("[CFG] A 长按忽略（会话进行中）");
      return;
    }
    if (cfgMode) {
      // ---- 关 AP 配置门户（STA 保持）----
      cfgStop();
      Serial.println("[CFG] 配置门户已关闭（A 长按 2s）——STA 保持连接");
      mtxBrief(MTX_P_WIFI_OFF);
    } else {
      // ---- 开 AP 配置门户（cfgStart 内部 WIFI_AP_STA，STA 不断）----
      cfgStart();
      mtxBrief(MTX_P_WIFI_ON);
    }
  }
}

// ================== ★V2.3.2 深度休眠（参照 PageTurner V1.0.5）==================
// 进睡条件：无播放、无录音、非配网门户，且"最近一次交互"已超过 SLEEP_IDLE_MS。
// 睡前必须收干净：断 WS → 关 WiFi/射频 → 灭屏 → 配 GPIO 电平唤醒 → esp_deep_sleep_start()。
// 深睡唤醒在 C3 上是**整机复位重启**，所以"唤醒后直接说话"只能靠 setup 里重新检测按键。
static void enterDeepSleep() {
  Serial.printf("[SLEEP] 空闲 %lus，进入深度休眠（长按 C 唤醒并直接说话）\n",
                (unsigned long)(SLEEP_IDLE_MS / 1000));
  if (dlActive) stopDlPlayHard();            // 理论上不会（条件已排除），兜底
  // 休眠动画（与翻页器一致）：闪 2 次 X（MTX_P_WIFI_OFF = 大叉），每次 220ms。
  // 用快速路径（9 字节即走）保证节奏，不等协控渲染 400ms。
  for (uint8_t i = 0; i < 2; i++) {
    mtxSendFast(MTX_P_WIFI_OFF);
    delay(220);
    mtxSendFast(MTX_P_CLEAR);
    delay(220);
  }
  mtxSend(MTX_P_CLEAR);                      // 收尾灭屏（阻塞确认，顺带清 RX 残渣）
  if (wsReady) { ws.disconnect(); wsReady = false; delay(50); }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);                       // 关射频：深睡电流从 mA 级降到 µA 级
  coUart.flush();
  Serial.flush();
  // C3 深睡只有 RTC GPIO(0~5) 能唤醒；C=GPIO3，板上外部下拉，按下=高电平
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  delay(50);
  esp_deep_sleep_start();                    // 不返回；醒来 = setup() 重跑
  Serial.println("[SLEEP] ⚠️ 深睡启动失败（未返回），继续运行");
}

static void sleepTick() {
  if (SLEEP_IDLE_MS == 0) return;
  // ★V2.4.5：本地播放期间（媒体库取流 mLibState / FIFO 播放泵 dlActive）既不许睡，
  //   **也不许"空闲钟"继续走表**。
  //   用户实测：一集 11 分钟播完的**那一瞬间**就自动深睡了 —— 因为 lastConvAt 还停在
  //   上次说话的时刻（早就超过 60s），dlActive 一落（FIFO 播空）就立刻满足进睡条件。
  //   这里播放期间持续把基准推到"当下" ⇒ 播完那刻重新开始数 60s。
  if (mLibState || dlActive) {
    lastConvAt = millis();
    return;
  }
  if (recPhase != 0 || cfgMode) return;      // 录音/配网：绝不睡（也不动空闲钟）
  uint32_t ref = (lastConvAt > lastVolActAt) ? lastConvAt : lastVolActAt;
  if (millis() - ref < SLEEP_IDLE_MS) return;
  enterDeepSleep();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== AlphaPi_Xiaozhi " XZ_FW_VERSION XZ_FW_TAG " (ESP32-C3) ===");
  // ★V2.3.2：本次是不是 C 键从深睡唤醒（深睡唤醒=复位重启，只能靠这个标志接力）
  wakeByCKey = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO);
  if (wakeByCKey) Serial.println("[SLEEP] C 键唤醒（深睡重启）：初始化完若仍按住就直接说话");
  Serial.printf("Device-Id: %s\n", deviceId().c_str());
  Serial.println("按住 C=说话/播放中按下即打断 | A 单击=音量- | B 单击=音量+（25 档，1 像素1档）| A 长按2s=配网门户");
  // ★V2.1.3：顺带把 loopTask 栈大小打出来 —— 48000→32768 是否生效一眼可见
  Serial.printf("[MEM] 启动堆 %u KB（WiFi/协控 UART 前）| loopTask 栈 %u B\n",
                ESP.getFreeHeap() / 1024, (unsigned)getArduinoLoopTaskStackSize());

  dlSegHard = MS2B(DL_SEG_HARD_MS);
  dlFillTarget = MS2B(1500);

  // 协控 UART：460929，RX=9 TX=8
  // RX 缓冲 2048B：录音泵窗口 30ms + 编码预算 40ms 交替，编码期间 UART 进 ~880B
  // 由缓冲承接、下圈泵窗口读走（默认 256B 会溢出丢录音数据）
  coUart.setRxBufferSize(2048);
  coUart.begin(460929, SERIAL_8N1, 9, 8);
  while (coUart.available()) coUart.read();
  delay(100);  // ★等 UART 引脚/协控稳定再发首帧（PageTurner begin() 同款时序）——
               //   过早发帧会被吃成半帧导致协控解析错乱，显示/录音全部不应答
  // ★V2.2.2：恢复上次音量并同步给协控（协控自己上电是默认值，不设会与显示不一致）
  // ★V2.2.3：存档改为档位；旧版存的是 0..100 或 25 档，统一折回 1..6 硬件档。
  {
    prefs.begin("cfg", true);
    uint16_t v = prefs.getUChar("vol", 0);
    prefs.end();
    // 旧存档折算：0..100（更早版本）或 1..25（V2.2.3~2.2.7）都按比例落到 1..6
    uint8_t lv;
    if (v == 0) lv = VOL_DEF_LV;
    else if (v > VOL_LEVELS) lv = (uint8_t)((v <= 25) ? ((v * 6 + 12) / 25) : ((v * 6 + 50) / 100));
    else lv = (uint8_t)v;
    if (lv < VOL_MIN_LV) lv = VOL_MIN_LV;
    if (lv > VOL_MAX_LV) lv = VOL_MAX_LV;
    volLevel = volTarget = lv;
    volHwStep = volHwOf(lv);
    int ack = volWriteHardwareBlocking();  // 开机同步一次（此后只在每次起播发）
    Serial.printf("[VOL] 开机第 %u/%u 档（硬件 %u）ack=%d\n",
                  (unsigned)lv, (unsigned)VOL_LEVELS, (unsigned)volHwStep, ack);
  }
  // ★不发明屏前置命令、不发 0x00 同步字节（EXP34：0x00 后例行帧 0 字节，疑似毒化解析器；
  //   干净状态 + 带len帧 = @0ms 回 [91,08,05]）
  while (coUart.available()) coUart.read();
  mtxFlush();  // 点阵清屏：开机残显（上次会话的图标/脸）灭掉。协控不在也不阻塞（30s 熔断）
  if (mtxCur == 0xFF) {
    Serial.printf("[MTX] 点阵首帧无响应（前10轮每2s重试，之后30s；末次响应 %d 字节 / status=0x%02X）\n",
                  mtxLastGot, mtxLastSt);
  } else {
    Serial.println("[MTX] 点阵就绪");
  }

  pinMode(BTN_PIN, INPUT);    // 板上已有外部下拉，按下=高电平
  pinMode(BTN_A_PIN, INPUT);  // A 键 GPIO10，长按 2s 开关配网门户
  pinMode(BTN_B_PIN, INPUT);  // B 键 GPIO1，短按打断播放

  opusInit();

  // WiFi 凭据：NVS 优先（Web 配置页写入），回退烧录常量
  prefs.begin("cfg", true);
  wifiSsid = prefs.getString("ssid", WIFI_SSID);
  wifiPass = prefs.getString("pass", WIFI_PASS);
  gActCode = prefs.getString("actcode", "");
  // ★V2.4.0 我的媒体库（cfg 命名空间，烧录不擦 NVS ⇒ 升级后配置保留）
  mLibOn = prefs.getBool("mlon", false);
  mLibHost = prefs.getString("mlhost", "");
  mLibPort = prefs.getUShort("mlport", 8100);
  prefs.end();

  // ★★全新设备判定（V2.0.4）：WiFi 凭据为空 = 设备从未配过网 = 出厂状态。
  //   为什么必须清：**ESP32 烧录默认不擦 NVS**！同一块板子刷过几版固件、或产线上
  //   刷过前一台设备的全片镜像，NVS 里会残留 xiaozhi 活动槽（官方 host + ssl=true +
  //   有效 token）与 cfg/actcode="(已激活)" ⇒ loadServer 直接继承 ⇒ 新设备刷上就
  //   "显示已激活，实际没激活"（点说话才暴露）。
  //   凭据为空即判定全新：清空服务器/激活残留 → 强制走全新激活流程（配网门户）。
  bool freshDevice = (wifiSsid.length() == 0);
  if (freshDevice) {
    prefs.begin("xiaozhi", false); prefs.clear(); prefs.end();  // 活动服务器槽
    prefs.begin("cfg", false);
    prefs.remove("actcode"); prefs.remove("actboot");           // 激活缓存 / 开机切官方请求
    prefs.remove("ohost"); prefs.remove("oport"); prefs.remove("opath");
    prefs.remove("otok");  prefs.remove("ossl");                // 官方配置快照
    prefs.remove("lhost"); prefs.remove("lport"); prefs.remove("lpath");
    prefs.remove("ltoken");                                     // 本地服务器配置
    prefs.end();
    gActCode = "";
    Serial.println("[FACTORY] WiFi 凭据为空 → 判定为全新设备：已清空服务器/激活残留");
  }

  bool wifiOk = false;
  if (freshDevice) {
    // 无凭据时 WiFi.begin("","") 只会白等 20s 超时——直接跳过，秒进配网门户
    Serial.println("[WiFi] 无凭据 → 跳过 STA 连接，直接进入配网门户");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // 关调制解调器省电：排除 DTIM 唤醒造成的接收延迟抖动变量
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("[WiFi] 连接 %s ", wifiSsid.c_str());
    uint32_t wifiT0 = millis();
    wifiOk = true;
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
      if (millis() - wifiT0 > 20000) { wifiOk = false; break; }  // 20s 超时：可能密码错/AP 不在
    }
    if (wifiOk) {
      Serial.printf(" OK  IP=%s  RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      Serial.println(" 超时！——20 秒后进配置模式（手机连 AlphaPi-XZ-xxxx 网页改 WiFi）");
    }
  }
  Serial.printf("[MEM] 堆剩余 %u KB（WiFi 后，20KB 下行 FIFO 已计入；最大连续块 %u KB）\n",
                ESP.getFreeHeap() / 1024,
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);

  loadServer();
  // 自愈：旧版 bug 可能把"(已激活)"写进 NVS 而实际无 token（NVS 烧录不擦除，跨固件残留）
  if (gActCode == "(已激活)" && curToken.length() == 0) {
    gActCode = "";
    prefs.begin("cfg", false);
    prefs.remove("actcode");
    prefs.end();
    Serial.println("[OTA] 检测到假激活缓存（无 token），已清除——请重新获取激活码");
  }
  // 网页"切官方"请求的开机收尾：无快照时重启前清了活动槽，在这里纯 STA + 堆全新的
  // 最安全环境跑 OTA 激活（门户模式跑 TLS 必崩的根治路径）。
  // ★★不能用 curToken.length()==0 当触发条件（V2.0.3 修复）：loadServer 找不到活动槽时
  //   会回退烧录常量 LOCAL_TOKEN（非空！，且 NVS ltoken 也可能非空）⇒ 条件恒假
  //   ⇒ OTA 永不执行 ⇒ 用户看到的就是"网页点切官方没反应，一直是本地模式"。
  //   显式请求就该显式执行：只要 actboot 置位且 WiFi 通，无条件跑 OTA。
  bool otaBoot = false;
  {
    prefs.begin("cfg", true);
    otaBoot = prefs.getBool("actboot", false);
    prefs.end();
  }
  bool portalNeeded = !wifiOk;  // 连不上 WiFi：必然要进门户救回
  if (otaBoot) {
    prefs.begin("cfg", false);
    prefs.putBool("actboot", false);
    prefs.end();
    if (wifiOk) {
      Serial.println("[OTA] 网页切官方请求：开机自动获取官方配置...");
      officialOta();
      // OTA 未拿到可用配置（首次激活待输码/网络异常）→ 开门户，让用户能看见激活码
      if (gActCode != "(已激活)") portalNeeded = true;
      String otaRes = (gActCode == "(已激活)") ? String("已激活 ✅") : ("未完成（" + gActCode + "）");
      Serial.println("[OTA] 开机激活结果: " + otaRes);
    }
  }
  printServer();
  // 量产友好：①连不上 WiFi / 切官方未完成 → 进门户救回；②完全没配置 → 也开门户
  if (portalNeeded || curToken.length() == 0) {
    cfgStart();
    if (wifiOk) {
      Serial.println("[CFG] 配置门户已自动开启（手机连 AlphaPi-XZ-xxxx → http://192.168.4.1/）");
    }
  }

  ws.onEvent(wsEvent);
  // 按需连接架构：开机不连、不自动重连。按住说话时 ensureConnected() 现连，
  // 会话结束空闲 60s 主动断开（服务器 80s 会话超时内安全）
  // ★V2.1.3：**停用库心跳，改自建**（hbTick()，10s 一次，失败只记账不断连）。
  //   复盘：V2.1.0 把 disconnectTimeoutCount 设 0，只堵住了"pong 超时断连"这一条；
  //   同一函数里还有另一条 —— handleHBPing() 中 `sendPing()` **写失败即刻**
  //   `WebSockets::clientDisconnect(1000)`，且 reason=NULL ⇒ 在日志上与"服务端 close 帧"
  //   **完全同形**，等于把"是服务器关的、还是我们自己关的"这个关键问题永久蒙住。
  //   现在 ping 由我们发（服务器 pong 照样收得到、照样计数），**断连决定权只留给真证据**
  //   （tcp 掉线 / 死链检测）。半开僵死连接由 wsLastRxAt 在 ensureConnected() 里兜底。
  ws.disableHeartbeat();
  pumpTaskBegin();  // ★V2.2.0：播放泵独立任务（与 WS 读取解耦，1ms 一 tick）
  Serial.println("[WS] 待机（按需连接：按住说话时才连，官方 S2 同款架构）");
  Serial.println("命令: o=切官方(快照优先) l=切本地 a=联网核对激活 i=信息 h=hello 1/0=listen w=配置网页 s=停止 r=重连 F=恢复出厂 v=详细诊断  A长按2s=配网门户开关");

  // ★V2.3.2：C 键深睡唤醒 → 初始化完若仍按住，直接进入对话（与待机按住 C 完全一致）
  if (wakeByCKey) {
    wakeByCKey = false;
    if (digitalRead(BTN_PIN) == HIGH) {
      Serial.println("[SLEEP] 唤醒时 C 仍按住 → 直接开始对话");
      btnStable = HIGH;   // 视作"已按下"：松手时才走 stopTalk，避免再触发一次 startTalk
      startTalk();
    } else {
      Serial.println("[SLEEP] 唤醒时 C 已松开 → 正常待机");
    }
  }
}

void loop() {
  // ★V2.1.4 主循环频率采样（每 ≥1s 更新 loopRate，供停供/2s 打点引用）
  loopTick++;
  {
    uint32_t nowMs = millis();
    if (nowMs - loopRateAt >= 1000) {
      loopRate = (uint32_t)((uint64_t)(loopTick - loopRatePrev) * 1000 / (nowMs - loopRateAt));
      loopRatePrev = loopTick;
      loopRateAt = nowMs;
    }
  }
  // ★V2.1.10：入流瓶颈修复。WebSocketsClient::loop() 每次**只处理一帧**
  //（WebSocketsClient.cpp: handleClientData → handleWebsocket 单条），而本主循环
  //  实测只有 12~15 圈/s ⇒ 下行音频被硬钳在 13 帧/s ≈ 18.7KB/s < 实时 24KB/s
  //  ⇒ 协控永远欠供 ⇒ 语速被拖慢 + 卡顿（实测 26 帧/2s，正常应 34）。
  //  一圈多轮询几次即可把上限抬到实时以上；空轮询（available()==0）开销可忽略。
  uint32_t lp0 = micros();
  for (uint8_t i = 0; i < WS_POLL_PER_LOOP; i++) {
    ws.loop();
    if (!wsReady) break;
  }
  uint32_t lp1 = micros();
  accWsUs += (uint32_t)(lp1 - lp0);
  hbTick();    // ★V2.1.3 自建心跳（关掉库心跳后唯一的 ping 来源，失败只记账）
  pollButton();
  pollBtnA();  // A 键长按 2s 切换 AP 配网门户开/关（STA 不受影响，图标提醒）
  mtxFlush();  // 点阵意图补发（内部判总线空闲+熔断，忙时零开销返回）
  mLibTick();  // ★V2.4.0 媒体库取流泵（空闲时立即返回）
  mLibPosTick();  // ★V2.4.6 到点把「播到第几秒」报给媒体库（队列空时立即返回）
  if (mLibPendingQuery.length()) {   // 抢跑：工具一调用就发请求，起播等云端说完
    String q = mLibPendingQuery;
    uint8_t kd = mLibPendingKind;
    uint16_t ps = mLibPendingPos;
    mLibPendingQuery = "";
    if (mLibOn) mLibPlayStream(kd, q, ps, true);
  }
  // ★V2.2.3：音量落盘推迟到空闲（NVS 提交会卡 10~40ms，播放中写 flash 就是一次断流）。
  // 图标 1.5s 后的恢复动作已交给泵任务（播放中也走非阻塞快速路径）。
  if (volSaveDirty && !dlActive && recPhase == 0 && millis() - volSaveAt > 800) {
    volSaveDirty = false;
    prefs.begin("cfg", false);
    prefs.putUChar("vol", volLevel);
    prefs.end();
    Serial.printf("[VOL] 已保存 %u/%u 档（ack=%u）\n",
                  (unsigned)volLevel, (unsigned)VOL_LEVELS, (unsigned)volAckLast);
  }

  // Web 配置门户：运行期服务 + 延时重启收尾
  if (cfgMode && cfgWeb) cfgWeb->handleClient();
  if (cfgRebootAt && millis() > cfgRebootAt) {
    cfgRebootAt = 0;
    Serial.println("[CFG] 保存完成，重启...");
    delay(100);
    ESP.restart();
  }

  // 按需连接架构：会话结束空闲 60s 主动断开（60s 内对话间隔免重连握手，
  // 60s 主动断早于服务器 80s 超时）
  if (wsReady && recPhase == 0 && !dlActive && millis() - lastConvAt > IDLE_DISCONNECT_MS) {
    Serial.println("[WS] 空闲 60s，主动断开（下次按住说话现连）");
    ws.disconnect();
  }

  // ★V2.3.2：60s 无动作 → 深睡（播放/录音/配网期间不进；长按 C 唤醒后可直接说话）
  sleepTick();

  if (recPhase == 1) {
    coPumpRecord();  // 协控录音泵（零打印）
    drainUplink();
    // 60 秒上限保护：防误触超长上行（曾 3 分钟 4495 帧，疑似触发服务器限流）
    if (millis() - recStartAt > 60000) {
      stopTalk();
      Serial.println("[TALK] 录音超 60s，自动停止");
    }
  }

  // ★V2.2.0：播放泵已移到 dlpump 任务，主循环不再调用（否则两边抢协控串口）
  accPumpUs += 0;
#if PROBE_ENABLE
  probeTick();  // ★手段④：0x80 寄存器指针扫描（probePhase==0 时一次判空返回，零开销）
#endif

  // 2s 统计（录音期间跳过——零打印铁律）
  static uint32_t lastStat = 0;
  uint32_t statNow = millis();
  if (statNow - lastStat >= 2000 && recPhase == 0) {
    // ★V2.1.11：真实窗口。阻塞读/慢泵会让一圈跨过 2s，写死除以 2 会把速率虚报，
    // 之前"喂 23000 B/s"其实是 2.4s 窗口摊出来的，实际只有 18.9k。
    uint32_t dtms = statNow - lastStat;
    if (dtms == 0) dtms = 1;
    lastStat = statNow;
    if (dlFrames || dlDecErr) {
      // ★V2.1.6：2s 行补两项关键判据（ACK-LOCK 是否真的生效）：
      //   积压 = 按 ACK 记账推出的"协控在途积压"（目标 ≤ DL_AHEAD_MAX_MS）；
      //   重开 = 本场因 ACK 断流重开会话的次数（正常必须为 0）。
      {
        int32_t aheadDbg = dlActive
                             ? (int32_t)dlSentTotal
                                 - (int32_t)((uint64_t)(millis() - dlStartAt) * dlRate / 1000)
                             : 0;
        Serial.printf("[DL] %lums: %lu 帧 opus (%lu B)%s%s%s | WS入 %lu 帧，最大 %u B%s"
                      " | 积压 %ldms 喂 %lu B/s 短收 %lu stmin %u 待重发 %u 溢出 %lu 重开 %u"
                      " | rxgap %ums loop %lu/s fps %lu.%lu gap[%u/%u/%u/%u/%u]"
                      " ws %lums/s pump %lums/s\n",
                      (unsigned long)dtms,
                      (unsigned long)dlFrames, (unsigned long)dlBytes,
                      dlDropped ? " [半双工丢帧]" : "",
                      dlOver ? " [FIFO溢出丢样]" : "",
                      dlDecErr ? " [解码错!]" : "",
                      (unsigned long)wsRxN, (unsigned)wsRxMax,
                      wsRxBig ? " ⚠️含>1KB大帧" : "",
                      (long)((int64_t)aheadDbg * 1000 / (int32_t)dlRate),
                      (unsigned long)((uint64_t)(dlSentTotal - dlSentDbgPrev) * 1000 / dtms),
                      (unsigned long)dlShort2s,
                      (unsigned)dlStMin2s,
                      (unsigned)(dlRtxLen - dlRtxOff),
                      (unsigned long)dlRtxOverflow,
                      (unsigned)dlRestarts,
                      (unsigned)dlRxGapMax, (unsigned long)loopRate,
                      (unsigned long)((uint64_t)dlFrames * 100 / dtms),
                      (unsigned long)((uint64_t)dlFrames * 1000 / dtms % 10),
                      (unsigned)dlGapHist[0], (unsigned)dlGapHist[1], (unsigned)dlGapHist[2],
                      (unsigned)dlGapHist[3], (unsigned)dlGapHist[4],
                      (unsigned long)((uint64_t)accWsUs * 1000 / dtms / 1000),
                      (unsigned long)((uint64_t)accPumpUs * 1000 / dtms / 1000));
                      accWsUs = 0;
                      accPumpUs = 0;
        dlSentDbgPrev = dlSentTotal;
        dlShort2s = 0;
        dlStMin2s = 255;
        dlRxGapMax = 0;
        memset(dlGapHist, 0, sizeof(dlGapHist));
      }
      // ★V2.1.4 链路健康三件套：主循环是否卡住 / 网卡可用 DMA 堆 / 服务器是否还在回心跳
      // ★V2.1.5 精简：这条周期行默认**关闭**（结案后没必要每 2s 刷），串口按 v 打开。
      //   注意：异常路径（停供/断粮块）里那条同内容的链路行**不受此开关影响**，永远照打。
      if (dlVerbose) {
        Serial.printf("[DL]   链路: 主循环 %lu 圈/s | DMA 堆 %u 连续 %u | 距上次 pong %ldms"
                      "（ping 发 %lu / 收 ping %lu / 收 pong %lu）\n",
                      (unsigned long)loopRate,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                      (long)(wsLastPongAt ? (int32_t)(millis() - wsLastPongAt) : -1),
                      (unsigned long)wsPingTx, (unsigned long)wsPingRx, (unsigned long)wsPongRx);
      }
      wsRxN = 0; wsRxMax = 0; wsRxBig = 0;
      dlFrames = 0;
      dlBytes = 0;
      dlDropped = 0;
      dlOver = 0;
      dlDecErr = 0;
#if DL_EXP2_SEG
      // ★EXP2b 分段探针（每 2s）：本段推量是否被 2400ms 截断 + 是否越过环长
      if (dlActive) {
        Serial.printf("[DL]   分段: 本段 %lums / 本场最长 %lums（环长 2730）| 已切 %lu 次"
                      " | 越过环长 %lu 次 | 停发态 %s\n",
                      (unsigned long)((uint64_t)dlSegSent * 1000 / dlRate),
                      (unsigned long)dlSegMaxMs, (unsigned long)dlExp2Segs,
                      (unsigned long)dlWrapCross, dlCutPhase ? "是" : "否");
      }
#else
      // ★EXP4 基线（EXP1/EXP2 全关=连续播放）：本段累计推量越过 2730ms ⇒ 写指针已过环尾，
      //   绕圈必已发生。这一行是"手段④ 探针测的就是这件事"的现场对照。
      if (dlActive) {
        Serial.printf("[DL]   本段 %lums（环长 2730ms）| 本段已绕圈 %s\n",
                      (unsigned long)((uint64_t)dlSegSent * 1000 / dlRate),
                      ((uint64_t)dlSegSent * 1000 / dlRate >= 2730) ? "是" : "否");
      }
#endif
    }
  }

  // 串口命令
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'o') useOfficial();
    else if (c == 'l') useLocal();
    else if (c == 'i') {
      printServer();
      Serial.printf("[SRV] ready=%d rec=%u play=%d | 解码器 %dHz | 下行丢帧=%lu 溢出=%lu 解码错=%lu\n",
                    wsReady, recPhase, dlActive, decRate,
                    (unsigned long)dlDropped, (unsigned long)dlOver, (unsigned long)dlDecErr);
    } else if (c == 'm') {   // 我的媒体库：打印状态 + 切开关（NVS 持久化）
      static const char* kn[] = {"空闲", "歌单", "电台", "有声书"};
      Serial.printf("[LIB] 媒体库 %s | 目标 %s:%u | 状态 %u | 当前 %s「%s」pos=%u 本批 %u 首\n",
                    mLibOn ? "开" : "关",
                    mLibHostNow().length() ? mLibHostNow().c_str() : "(未配置)", mLibPort,
                    mLibState, kn[mLibKind > 3 ? 0 : mLibKind], mLibQ.c_str(),
                    (unsigned)mLibPos, (unsigned)mLibTracks);
      mLibOn = !mLibOn;
      mLibSave();
      if (!mLibOn && mLibState) mLibStop("串口关闭");
      Serial.printf("[LIB] 已%s（串口切换，NVS 已存）\n", mLibOn ? "启用" : "关闭");
    } else if (c == 'h') {
      if (wsReady) sendHello();
      else Serial.println("[WS] 未握手");
    } else if (c == '1') {
      sendListen(true);
    } else if (c == '0') {
      sendListen(false);
    } else if (c == 'w') {  // Web 配置门户开/关（配 WiFi/服务器/看激活码）
      if (cfgMode) cfgStop();
      else cfgStart();
    } else if (c == 's') {  // 紧急停止
      if (recPhase == 1) stopTalk();
      if (dlActive) endDlPlay("手动停止");
      Serial.println("[SRV] 已全部停止");
    } else if (c == 'r') {  // 重连 WebSocket
      wsReady = false;
      ws.disconnect();
      wsConnect();
    } else if (c == 'v') {  // ★V2.1.5：详细诊断开关（只管每 2s 那条 [DL] 链路行）
      dlVerbose = !dlVerbose;
      Serial.printf("[DBG] 详细诊断 %s —— 每 2s 的「[DL] 链路」行%s；"
                    "异常路径（停供/断粮/断开）的链路信息不受本开关影响，始终打印\n",
                    dlVerbose ? "开" : "关", dlVerbose ? "已开启" : "已关闭");
#if PROBE_ENABLE
    } else if (c == 'p' || c == 'P') {  // ★手段④：0x80 寄存器指针扫描探针（放歌中按 p）
      if (probePhase != 0) {
        Serial.printf("[P] 探针中止（phase=%u，游标 addr=%u）\n", probePhase, probeAddr);
        probePhase = 0;
      } else {
        probeStart();
      }
#else
    } else if (c == 'p' || c == 'P') {
      Serial.println("[P] 探针未编译（PROBE_ENABLE=0）");
#endif
    } else if (c == 'a') {  // 联网核对激活状态（写 actboot + 重启，与网页按钮同义）
      Serial.println("[ACT] 联网核对激活状态：写 actboot 标志，重启后 OTA 实查...");
      if (cfgMode) cfgStop();
      prefs.begin("cfg", false);
      prefs.putBool("actboot", true);
      prefs.end();
      delay(300);
      ESP.restart();
    } else if (c == 'F') {  // 恢复出厂：清空 WiFi + 服务器 + 激活全部 NVS 后重启
      Serial.println("[FACTORY] 恢复出厂：清空 WiFi/服务器/激活配置，重启后自动进配网门户...");
      if (cfgMode) cfgStop();
      prefs.begin("cfg", false); prefs.clear(); prefs.end();
      prefs.begin("xiaozhi", false); prefs.clear(); prefs.end();
      delay(300);
      ESP.restart();
    }
  }
}
