/*
 * AlphaPi_Xiaozhi —— STEM 板（ESP32-C3）极简小智 AI 客户端
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
 *   A 键长按 2s = AP 配网门户开/关（物理救援入口）。
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_heap_caps.h>   // ★V2.1.4：DMA 堆诊断（heap_caps_get_free_size / largest_free_block）
#include <opus.h>

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
static const size_t PLAY_CHUNK = 200;

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

static const uint16_t DL_SIZE = 20480;  // 20KB≈853ms@24000，容纳 880ms 起播预滚（按容量钳制）
// 本地服务器 TTS 逐帧发送不均，FIFO 平滑供给；满时丢最老（延迟保护）
static uint8_t dlFifo[DL_SIZE];
static uint16_t dlHead = 0, dlTail = 0;
static uint32_t dlOver = 0;           // FIFO 溢出丢样计数
static uint32_t dlStarvePrintAt = 0;  // 断粮周期打印节拍（每 2s 一条）

// ---- 生产策略（v8.2 低水位 + 饿透即重启 + v9.5.3 死亡指纹复活）----
//   ① 垫 880ms（吸收服务器 150-350ms 间隙）；
//   ② 饿透标记（FIFO 空 + 积压 ≤50ms）→ 数据恢复瞬间重推头开新会话；
//   ③ 会话硬顶 120s 只作保险丝（EXP21 单会话 30s 零异常，正常故事永不触发）；
//   ④ 播放冻结指纹（积压增速 >8000 B/s 连续 2 次）→ 0x10+推头直启复活。
static const uint32_t DL_SEG_HARD_MS = 120000;  // 会话硬顶兜底（保险丝）
static const uint32_t DL_JITGATE_MS = 880;      // 起播门槛 ms（起播延迟 +0.2s~0.9s）
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
static uint32_t dlRetryAt = 0;      // 启动失败退避：2 秒内不重试
static bool ttsDone = true;         // 服务器 tts state=stop 已到（流结束的协议判据）
                                    // ★初值 true = 开机视为"无 TTS 进行中"（startTalk 据此决定是否发 abort）

// v9.17 协控不应答看门狗：连续 30 次 ACK 超时（≈3s 零应答）= 协控僵死
// （健康流实测恒 0 超时）→ 主动 endDlPlay 收场，防"泵空转 + 服务器踢连接"不可恢复态
static uint8_t dlToStreak = 0;
static const uint8_t DL_TO_STREAK_MAX = 30;  // ≈3s（RESP_TIMEOUT=100ms × 30）

// ---- 头前置队列：44B WAV 头必须先于一切 PCM 上线（中途头=会话杀手）----
static uint8_t dlPre[44];
static uint8_t dlPreIdx = 44;  // 44=空；0..43=待发头字节游标

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
static uint8_t dlPendLen[DL_PIPE_MAX];  // 在途帧请求长度
static uint8_t dlPendGot[DL_PIPE_MAX];  // 在途帧已确认接收长度
static uint32_t dlPendAt[DL_PIPE_MAX];  // 在途帧发送时刻（超时判据）
static uint8_t dlPendCnt = 0;           // 当前在途帧数

// 解码统计（2s 汇总打印后清零）
uint32_t dlFrames = 0, dlBytes = 0, dlDropped = 0, dlDecErr = 0;

// 解码输出缓冲（120ms@24k 余量）
static const int DEC_BUF_SAMPLES = 2880;
static int16_t decBuf[DEC_BUF_SAMPLES];

static void endDlPlay(const char* why);
static void stopDlPlayHard();

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
enum { MTX_I_CLEAR = 0, MTX_I_MIC, MTX_I_THINK, MTX_I_PLAY, MTX_I_MUSIC, MTX_I_FACE };
static uint8_t mtxWant = MTX_I_CLEAR;
static uint8_t mtxCur = 0xFF;               // 0xFF=未知（开机首帧必发）
static const uint8_t* mtxFace = MTX_P_NEUTRAL;  // 最近一次情绪脸
static bool mtxMusic = false;               // 本次 TTS=点歌（sentence_start 含 % play_music）
static uint32_t mtxFailUntil = 0;           // 无响应熔断截止时刻
static uint8_t mtxFailCnt = 0;              // 连续失败轮数（阶梯重试：前 10 轮 2s 一试=覆盖协控晚开机，之后 30s）

static void mtxSet(uint8_t id) { mtxWant = id; }
static void mtxSetFace(const uint8_t* face) { mtxFace = face; mtxWant = MTX_I_FACE; }

// 服务器情绪名 → 5x5 脸（未收录情绪一律中性脸）
static void mtxApplyEmotion(const String& emo) {
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

static bool mtxSend(const uint8_t p[25]) {
  uint8_t cols[5] = {0, 0, 0, 0, 0};
  for (uint8_t y = 0; y < 5; y++)
    for (uint8_t x = 0; x < 5; x++)
      if (p[y * 5 + x]) cols[x] |= (1 << (4 - y));  // 行→列位：(1<<(4-y))<<3 = 1<<(7-y)，与手册 set_pixel 一致
  uint8_t f[9] = { 0x90, MTX_ADDR, 0x05, 0, 0, 0, 0, 0, 0 };  // ★9 字节：头3 + 5列 + ck（曾错成 8 字节，第5列被 ck 覆盖）
  for (uint8_t i = 0; i < 5; i++) f[3 + i] = cols[i] << 3;
  uint8_t ck = 0;
  for (uint8_t i = 0; i < 8; i++) ck += f[i];
  f[8] = ck;
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
  if ((int32_t)(millis() - mtxFailUntil) < 0) return;     // 熔断期内
  const uint8_t* p;
  switch (mtxWant) {
    case MTX_I_MIC:   p = MTX_P_MIC;   break;
    case MTX_I_THINK: p = MTX_P_THINK; break;
    case MTX_I_PLAY:  p = MTX_P_SMILE; break;  // V2.0.2：播放中显示笑脸（用户偏好，替换三角）
    case MTX_I_MUSIC: p = MTX_P_MUSIC; break;
    case MTX_I_FACE:  p = mtxFace;     break;
    default:          p = MTX_P_CLEAR; break;
  }
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
  mtxSet(mtxMusic ? MTX_I_MUSIC : MTX_I_PLAY);
  mtxFlush();
  dlActive = true;
  dlPendCnt = 0;  // 清流水线在途队列（残留会让下一场错位结算）
  dlStartAt = millis();
  dlSentTotal = 0;
  dlSegResets = 0;
  dlQuietRun = 0;
  dlSegPeak = 0;
  dlInQuiet = false;
  dlCutPhase = 0;
  dlStarved = false;
  dlFeedDone = false;
  dlToStreak = 0;
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
  dlPendCnt = 0;
  dlCutPhase = 0;
  dlStarved = false;
  dlTail = dlHead;
  dlPreIdx = 44;
  lastConvAt = millis();
  uint32_t ms = millis() - dlStartAt;
  uint32_t bps = ms ? (uint32_t)((uint64_t)dlSentTotal * 1000 / ms) : 0;
  Serial.printf("[DL] 播放结束(%s): %lu 字节 / %lu ms（%lu B/s，%lu=实时）| 分段 %lu 次 | 回填 %lums#%lu | 溢出丢样 %lu\n",
                why,                 (unsigned long)dlSentTotal, (unsigned long)ms, (unsigned long)bps,
                (unsigned long)dlRate, (unsigned long)dlSegResets,
                (unsigned long)dlFillTotal, (unsigned long)dlFillCount,
                (unsigned long)dlOver);
  // 播放结束灭屏（用户指定）：覆盖任何挂起意图（包括播放中缓存的表情）
  // ★V2.1.0 例外：服务器"响应未完成就断连"（故事被打断）⇒ 打一张失望脸。
  //   播放期点阵被总线仲裁禁发，只有这里（dlActive 刚置 false、总线空出来）能上屏；
  //   对用户而言这是"为什么没声了"的唯一可见答案，比静悄悄灭屏有用得多。
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
  dlBarged = true;
  dlPendCnt = 0;
  dlCutPhase = 0;
  dlStarved = false;
  dlTail = dlHead;
  dlPreIdx = 44;
  while (coUart.available()) coUart.read();
  Serial.println("[DL] TTS 已打断");
}

// 非阻塞流控泵：窗口内"发帧→收ACK→未收满重发"循环（在途语义，锁定 1 帧在途）
// ACK 在途时窗口内短轮询等待：若直接 return，下一圈 loop（wss TLS 开销 ~16ms）才收
// ACK → 吞吐掉到 12KB/s，TTS 断续。窗口内等待后每圈可完成 2-3 帧往返。
static const uint32_t PLAY_WINDOW_MS = 20;

static void pumpPlay() {
  if (!dlActive) return;
  uint32_t t0 = millis();
  while (dlActive && millis() - t0 < PLAY_WINDOW_MS) {
    // ---- 流水线 ACK 结算（非阻塞：UART 里够 3 字节就结算队首帧）----
    while (dlPendCnt > 0 && coUart.available() >= 3) {
      uint8_t h0 = coUart.read(), h1 = coUart.read(), h2 = coUart.read();
      if (h0 != 0x91 || h1 != CMD_PLAY) continue;  // 杂散字节：丢弃重同步
      dlToStreak = 0;  // 协控还在应答 ⇒ 看门狗清零
      uint8_t st = h2;
      uint8_t remAsk = (uint8_t)(dlPendLen[0] - dlPendGot[0]);
      if (st > remAsk) st = remAsk;
      dlPendGot[0] += st;
      dlSentTotal += st;
      dlSegSent += st;  // 分段流式：距上次 WAV 头的 ACK 字节数
      if (dlPendGot[0] < dlPendLen[0]) {
        // 短收：未确认的剩余字节归还 FIFO 头部，下轮重取重发（官方流控重发语义）
        dlTail -= (uint16_t)(dlPendLen[0] - dlPendGot[0]);
      }
      for (uint8_t i = 1; i < dlPendCnt; i++) {  // 出队（数组前移）
        dlPendLen[i - 1] = dlPendLen[i];
        dlPendGot[i - 1] = dlPendGot[i];
        dlPendAt[i - 1] = dlPendAt[i];
      }
      dlPendCnt--;
    }
    // 队首超时（协控整帧未回 ACK）→ 视为整帧丢失，剩余归还 FIFO 后出队
    if (dlPendCnt > 0 && millis() - dlPendAt[0] > RESP_TIMEOUT) {
      if (dlPendGot[0] < dlPendLen[0]) {
        dlTail -= (uint16_t)(dlPendLen[0] - dlPendGot[0]);
      }
      for (uint8_t i = 1; i < dlPendCnt; i++) {
        dlPendLen[i - 1] = dlPendLen[i];
        dlPendGot[i - 1] = dlPendGot[i];
        dlPendAt[i - 1] = dlPendAt[i];
      }
      dlPendCnt--;
      // ---- v9.17 协控不应答看门狗 ----
      // 健康场次实测超时恒 0，"连续 30 次零应答"≈3s 只可能是协控僵死。主动收场：
      // 本场 TTS 截断（协控本来也没在播），设备立即回到可响应态，下一句 TTS
      // 会重新走一次 startDlPlay。
      if (dlToStreak < 255) dlToStreak++;
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
    // 在途已满：窗口内短等 ACK（UART 中断自动收）；窗口耗尽则让出 CPU
    if (dlPendCnt >= 1) {
      if (millis() - t0 < PLAY_WINDOW_MS) {
        delayMicroseconds(200);
        continue;
      }
      return;
    }

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
              dlSentTotal = 0;  // 重锚：死亡=缓冲已排空，此刻=零点
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
    if (dlCutPhase == 0 && !dlFeedDone && dlStreamAlive && hardHit) {
      dlCutPhase = 1;
      dlCutPhaseAt = millis();
      Serial.printf("[DL] 段界 %lu：硬顶兜底（played %lums）——饿透复位\n",
                    (unsigned long)(dlSegResets + 1),
                    (unsigned long)((uint64_t)(dlSegSent - aheadNow) * 1000 / dlRate));
      return;
    }

    // ---- 积压钳制 4600ms：回填目标 1500ms 远低于此，只作极端兜底 ----
    // 泄压阀：FIFO 将满时解除钳制（防 FIFO 溢出丢样 + burst 路径死循环→WDT）
    const int32_t aheadLimit = MS2B(4600);
    if (aheadNow > aheadLimit && dlAvail() < DL_SIZE - PLAY_CHUNK - 1) {
      return;
    }

    // ---- 饥饿复位状态机：停发 → 协控播空（饿透）→ 推头 → 复供 ----
    if (dlCutPhase == 1) {
      if (aheadNow <= -MS2B(100) || millis() - dlCutPhaseAt > 1500) {
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
        dlSentTotal = 0;  // 重锚 aheadNow 基线：协控此刻播空=零点
        Serial.printf("[DL] 段界 %lu：饥饿复位完成（停发 %ums，头已入前置通道）\n",
                      (unsigned long)dlSegResets, (unsigned long)(millis() - dlCutPhaseAt));
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

    // ---- 空闲：前置头队列 + FIFO 取 ≤200 字节组帧发送 ----
    uint16_t availAll = (uint16_t)(dlAvail() + (44 - dlPreIdx));
    uint8_t n = (uint8_t)(availAll > PLAY_CHUNK ? PLAY_CHUNK : availAll);
    if (n == 0) {
      // 流结束判定（协议优先）：tts stop 已到 + FIFO 空 + 协控基本播完（积压<100ms）才收尾。
      // ⚠️ 曾按空闲 1500ms 硬猜：句间合成停顿 1.5-2s 常见 → 误触发双重断口。
      uint32_t now = millis();
      int32_t ahead = (int32_t)dlSentTotal
                      - (int32_t)((uint64_t)(now - dlStartAt) * dlRate / 1000);
      // 饿透标记：FIFO 空 + 协控积压 ≤50ms ⇒ 饿透在即/已发生。不立即推头（长停顿
      // 会把新会话也饿死），置标记等数据恢复瞬间再推——重启成本全部落在停顿里=免费。
      // 开播 500ms 内不标记：预滚灌入期 FIFO 瞬时见底属正常（防开机双重推头）。
      if (!ttsDone && !dlFeedDone && !dlStarved && ahead <= MS2B(50)
          && now - dlStartAt > 500) {
        dlStarved = true;
        dlStarveMarkAt = now;
        Serial.printf("[DL] 协控饿透标记（积压 %dms / FIFO 空）——数据恢复时重推头\n",
                      (int)((int64_t)ahead * 1000 / dlRate));
      }
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
        Serial.printf("[DL] %s %ums（协控积压 %dms / FIFO %uB | WiFi %d RSSI %d | WS %s | "
                      "堆 %u 连续 %u | 栈余 %u | 本会话最大帧 %u B%s）\n",
                      gap > DL_STALL_WARN_MS ? "⚠️ 服务器停供" : "断粮", gap,
                      (int)((int64_t)ahead * 1000 / dlRate), dlAvail(),
                      (int)WiFi.status(), (int)WiFi.RSSI(), wsReady ? "连接" : "断开",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                      (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)wsRxMaxAll,
                      (wsRxMaxAll + 1 > heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
                        ? " ⚠️顶到连续块" : "");
        // ★V2.1.4 定案三件套（判读见 README）：
        //   ①环正常 + 距pong <15s ⇒ 链路通、服务器进程活着 ⇒ 它只是不产音频（服务端侧）
        //   ②环正常 + 距pong 持续增大 ⇒ 我们根本收不到包 ⇒ 看 ③
        //   ③DMA 堆见底 ⇒ 网卡申请不到收发缓冲 ⇒ 内存水位就是真凶（V2.1.4 的 UP_SIZE 减半针对它）
        //   ④环暴跌 ⇒ 是我们自己卡住，与服务器无关
        Serial.printf("[DL]   链路: 主循环 %lu 圈/s | DMA 堆 %u 连续 %u | 距上次 pong %ldms"
                      "（ping 发 %lu / 收 ping %lu / 收 pong %lu）\n",
                      (unsigned long)loopRate,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                      (long)(wsLastPongAt ? (int32_t)(millis() - wsLastPongAt) : -1),
                      (unsigned long)wsPingTx, (unsigned long)wsPingRx, (unsigned long)wsPongRx);
      }
      return;
    }
    // ---- 饿透恢复推头：标记后数据恢复（FIFO 有货）且协控确认播空 → 新会话 ----
    // aheadNow ≤ -50ms = 协控已空 ≥50ms（防把头推进仍有余量的活跃会话）；
    // 2000ms 超时兜底（基线估算漂移时强制推）。满水重启：推头前先攒 400ms——
    // 零水位起跑的新会话无法爬回高水位，第一个大发隙即排空死亡。
    if (dlStarved && dlCutPhase == 0 && !ttsDone) {
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
        dlSentTotal = 0;  // 重锚基线：协控此刻播空=零点
        Serial.printf("[DL] 段界 %lu：饿透恢复推头（标记后 %ums，FIFO %uB/%s）\n",
                      (unsigned long)dlSegResets, (unsigned long)(millis() - dlStarveMarkAt),
                      (unsigned)dlAvail(), enough ? "满水" : "超时强推");
      } else {
        return;  // 攒水中（或还差最后 ~100ms 播空），先不发
      }
    }
    for (uint8_t i = 0; i < n; i++) {
      // 取数优先级：头（新会话第一字节）→ FIFO 真数据
      uint8_t b;
      if (dlPreIdx < 44) b = dlPre[dlPreIdx++];
      else b = dlFifo[dlTail++ % DL_SIZE];
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
    dlPendCnt++;
  }
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
  if (len < 2) return;
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
    // 强制起播+泵腾空间，绝不丢样（曾"满丢最老"→ burst 溢出丢样=断续）。
    // 不调 ws.loop()（handleAudio 是其回调，重入禁止）；pumpPlay 不碰 ws 安全
    while (dlAvail() >= DL_SIZE - PLAY_CHUNK - 1) {
      if (!dlActive) {
        startDlPlay();
      }  // 将满即播（不等 jitter 门槛）
      if (!dlActive) {
        dlTail++;
        dlOver++;
        break;
      }  // 起播失败保底丢最老防卡死
      pumpPlay();
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
    startDlPlay();
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

static void pollButton() {
  bool b = digitalRead(BTN_PIN);
  if (b != btnStable) {
    if (btnEdgeAt == 0) btnEdgeAt = millis();
    else if (millis() - btnEdgeAt > 30) {
      btnStable = b;
      btnEdgeAt = 0;
      if (btnStable == HIGH) startTalk();  // 按下=高电平=开始说话
      else stopTalk();                     // 松开=低电平=停止，等 TTS
    }
  } else {
    btnEdgeAt = 0;
  }

  // B 键短按 = 打断当前播放（TTS/音乐），不录音——"只想让它闭嘴"时比按 C 省事。
  // C 键按住时不触发（防误触）；500ms 限频防触点抖动重复 abort。
  static uint32_t btnBActAt = 0;
  bool bb = digitalRead(BTN_B_PIN);
  if (bb != btnBStable) {
    btnBStable = bb;
    if (bb == LOW && btnStable == LOW && (dlActive || !ttsDone)
        && millis() - btnBActAt > 500) {  // 松开瞬间触发，防按下即打断抖动
      btnBActAt = millis();
      if (dlActive) stopDlPlayHard();
      if (!ttsDone) sendAbort();
      mtxSet(MTX_I_CLEAR);
      mtxFlush();
      Serial.println("[TALK] B 键打断播放");
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
  String hello = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                 "\"channels\":1,\"frame_duration\":60}}";
  ws.sendTXT(hello);
  Serial.println("[WS] 已发送 hello");
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
    Serial.println("[SRV] " + msg);
    // tts start/stop 精确匹配（"state":"start" 不会误中 "sentence_start"）：
    // stop=流结束协议判据（pumpPlay 据此收尾，替代旧的空闲硬猜）
    if (msg.indexOf("\"type\":\"tts\"") >= 0) {
      if (msg.indexOf("\"state\":\"stop\"") >= 0) {
        ttsDone = true;
        dlFeedDone = true;
        dlRespOpen = false;  // 响应正常收尾
      } else if (msg.indexOf("\"state\":\"start\"") >= 0) {
        ttsDone = false;
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
          dlBroken = true;  // ★V2.1.0：交给 endDlPlay 打"失望脸"（收场那刻总线才空闲）
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
  h += "<h3>AlphaPi Xiaozhi V2.1.0 配置</h3>";
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
  } else if (!pressed && btnAHeld) {
    btnAHeld = false;
  }
  if (btnAHeld && millis() - btnAAt >= 2000) {
    btnAHeld = false;
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

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== AlphaPi_Xiaozhi V2.1.0 (ESP32-C3) ===");
  Serial.printf("Device-Id: %s\n", deviceId().c_str());
  Serial.println("按住 C 键(GPIO3)说话，松开等 TTS 回话 | B 短按=打断 | A 长按2s=配网门户开关");
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
  Serial.println("[WS] 待机（按需连接：按住说话时才连，官方 S2 同款架构）");
  Serial.println("命令: o=切官方(快照优先) l=切本地 a=联网核对激活 i=信息 h=hello 1/0=listen w=配置网页 s=停止 r=重连 F=恢复出厂 v=详细诊断  A长按2s=配网门户开关");
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
  ws.loop();
  hbTick();    // ★V2.1.3 自建心跳（关掉库心跳后唯一的 ping 来源，失败只记账）
  pollButton();
  pollBtnA();  // A 键长按 2s 切换 AP 配网门户开/关（STA 不受影响，图标提醒）
  mtxFlush();  // 点阵意图补发（内部判总线空闲+熔断，忙时零开销返回）

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

  if (recPhase == 1) {
    coPumpRecord();  // 协控录音泵（零打印）
    drainUplink();
    // 60 秒上限保护：防误触超长上行（曾 3 分钟 4495 帧，疑似触发服务器限流）
    if (millis() - recStartAt > 60000) {
      stopTalk();
      Serial.println("[TALK] 录音超 60s，自动停止");
    }
  }

  if (dlActive) pumpPlay();  // 非阻塞流控播放泵

  // 2s 统计（录音期间跳过——零打印铁律）
  static uint32_t lastStat = 0;
  if (millis() - lastStat >= 2000 && recPhase == 0) {
    lastStat = millis();
    if (dlFrames || dlDecErr) {
      Serial.printf("[DL] 2s: %lu 帧 opus (%lu B)%s%s%s | WS入 %lu 帧，最大 %u B%s\n",
                    (unsigned long)dlFrames, (unsigned long)dlBytes,
                    dlDropped ? " [半双工丢帧]" : "",
                    dlOver ? " [FIFO溢出丢样]" : "",
                    dlDecErr ? " [解码错!]" : "",
                    (unsigned long)wsRxN, (unsigned)wsRxMax,
                    wsRxBig ? " ⚠️含>1KB大帧" : "");
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
