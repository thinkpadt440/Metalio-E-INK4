# Metalio E-Ink4 ↔ Harness 接口约定 v1（Demo）

日期：2026-11-24。规则在本文件；[完整报文示例](device-harness-api-v1-examples.md)逐项展开全部HTTP/WS请求响应，两份合起来作为双方开发依据。尚待真实Harness联调。

## 1. 范围与依据

固定Demo服务地址和工作区，无设备Token、登录或鉴权；保留工具的“请求批准/完全访问”配置。HTTP读快照/历史，设备级一条WS传音频与实时交互。切页面不关连接，任务不依赖连接存活。

已检查192.168.8.72上deepseek-harness-src-0.2.1-rc.5源码：消息有ID、角色、内容，历史来自Session事件日志；审批含toolName、callId、reason及asked/decided事件。下述接口是设备适配层新接口，不声称Harness原生提供。不要新增消息摘要、消息状态或正文多版本存储。

本期覆盖Dashboard、最近Session、固定闲聊、BOOT录音、ASR先回显、长任务输出、模型/工具授权、审批与断线恢复。无实时麦克风流/TTS/附件/同Session并行任务/24小时事件重放。审批必须挂到真正的工具执行前暂停与恢复入口，不是执行后通知。

## 2. 地址和公共规则

- 固件写死 `http://<host>:<port>`；HTTP根路径`/api/v1`，WS`ws://<host>:<port>/api/v1/ws`。由后端交付实际host/port。
- HTTP和WS Upgrade携带`X-Device-ID: metalio_01`，仅标识Demo设备；不要求Authorization。固定工作区由后端配置，不实现多租户。资源ID仍须属于Demo范围，不能串Session。
- JSON为UTF-8，时间UTC ISO8601。model_id/authorization_level_id等只支持后端目录中的可用值，不能由设备构造未知配置。完整序列化JSON≤8192字节，Binary音频块≤4096字节，不以中文字数估算报文长度。
- GET/PUT无Body，PATCH为application/json，响应application/json。GET使用Cache-Control:no-store。
- 示例字段默认必返，允许null时保留键；空列表[]。未知可选字段忽略，缺必需字段报invalid_request。
- 普通列表items+next_cursor，limit默认10最大20；历史items+before_cursor；同步items+next_cursor。游标URL编码后原样回传，null为末页。
- ID不透明，建议≤128字节ASCII标识。device_id稳定，session_id持久；command_id是一条命令，run_id是一次任务，utterance_id是一段录音，upload_id是一条连接中的临时上传，approval_id是一项操作。
- Session.session_version/settings_version、run.state_version、approval.approval_version为正整数，比较各自版本防旧状态覆盖。没有message.content_revision或正文版本号。
- 本文成功对象返回裸JSON，只有默认闲聊PUT使用session包装。HTTP与WS共用数据源，不返回样例假装真实数据。

## 3. HTTP端点与对象

全部请求和成功/失败响应见示例文档H01–H11、HE01–HE04；路径相对/api/v1。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | /dashboard/overview?range=all | 六项统计，all/30d/7d |
| GET | /models | 模型目录 |
| GET | /authorization-levels | 工具授权档位 |
| GET | /sessions?limit=10&cursor=... | 最近工作Session |
| PUT | /devices/{device_id}/default-casual-session | 创建/复用固定闲聊 |
| GET | /sessions/{session_id} | 会话配置和活动任务 |
| PATCH | /sessions/{session_id}/settings | 当前会话模型/授权 |
| GET | /sessions/{session_id}/messages?limit=10&before=... | 消息及正文、向前续取 |
| GET | /runs/{run_id} | 单项任务当前状态 |
| GET | /approvals/{approval_id} | 审批状态和完整操作，一次返回 |
| GET | /sync?sync_token=...&cursor=... | 活动任务/审批恢复快照 |

不再提供单条/messages/{id}、正文/content或审批/details接口；同一Session消息接口解决历史和长消息。录音提交未知由audio.begin去重确认，不另加录音查询接口。

### 3.1 统计/目录

Dashboard字段：range、timezone、sessions、messages、total_tokens、active_days、peak_hour、favorite_model、unavailable_fields。统计Demo工作区，含固定闲聊；7d/30d是本地今天及前6/29自然日，all为已有全部。

会话数按有消息/任务的Session去重；消息数计用户与已完成公开助手消息；Token输入+输出；活跃日计有活动日期。高峰小时与常用模型按模型请求数最多，并列取最早小时/model_id字典序首项。无数据计数0，高峰/模型null；来源缺项值null、unavailable_fields列字段名。设备约15秒刷新。

模型响应default_model_id、items[id,name,available]、next_cursor；授权对应default_authorization_level_id。无可用默认值可null，实际配置映射Harness真实能力。新Session用默认值，旧Session保留配置；不可用选项禁用。

### 3.2 Session配置

~~~json
{
  "id": "sess_01",
  "title": "固件开发",
  "kind": "work",
  "model_id": "deepseek-chat",
  "model_name": "DeepSeek Chat",
  "authorization_level_id": "request_approval",
  "authorization_level_name": "请求批准",
  "settings_version": 1,
  "session_version": 1,
  "active_run_id": null,
  "pending_approval_count": 0,
  "updated_at": "2026-11-24T03:20:00Z"
}
~~~

title非空，kind=work/casual，无任务active_run_id=null。列表updated_at倒序且不列固定闲聊；默认闲聊PUT无Body，session包装返回，新建201/复用200。按工作区+device_id唯一，即使并发也只创建一条。

PATCH请求：
~~~json
{
  "expected_settings_version": 1,
  "model_id": "deepseek-reasoner",
  "authorization_level_id": "request_approval"
}
~~~

两项可只传一个。expected_settings_version匹配后原子更新并返回完整Session；相同配置不加版本。配置变更加settings_version/session_version，其它展示字段变更只加session_version，并推送session.updated。冲突409 details.current_session。

run在录音接受时冻结配置，改模型/授权只影响后续任务，不能以切完全访问自动批准当前pending操作。

### 3.3 一个Session接口直接取正文

~~~json
{
  "items": [
    {
      "id": "msg_01",
      "role": "user",
      "content": "帮我构建固件。",
      "offset_bytes": 0,
      "ends_message": true,
      "created_at": "2026-11-24T03:20:00Z"
    },
    {
      "id": "msg_02",
      "role": "assistant",
      "content": "正在检查项目配置。\n构建完成。",
      "offset_bytes": 0,
      "ends_message": true,
      "created_at": "2026-11-24T03:20:01Z"
    }
  ],
  "before_cursor": null
}
~~~

每条只有id、role、content、offset_bytes、ends_message、created_at。role=user/assistant，取公开用户/助手文本，created_at来自原始消息/事件。offset/ends是传输标记，不是消息状态。

1. 首次不带before，从最新内容向前取，最多覆盖limit条不同消息，同时限制整个JSON≤8192字节，items按旧→新排列。正常消息全文返回offset_bytes=0、ends_message=true。
2. 一条正文超预算：先返回靠近最新位置的末尾文本，offset_bytes为UTF-8原文起点，ends_message表示到达本次读取快照里的消息末尾；before_cursor指向更早的同消息文本/消息。后续响应仍旧→新、同消息按offset递增。完整示例H08a/b。
3. 按id+offset拼接，不把片段当新消息，不重复/漏字节。content不拆UTF-8字符。单批不刻意凑10条，也不能因为只有1条就允许无限大。
4. 游标内部固定首次读取边界及在途助手已生成长度，新消息不挤动旧before边界。这是短期游标，不要求历史正文版本存储。至少10分钟有效；失效409 history_cursor_expired，重新取最近内容。
5. 历史给已保存公开消息；当前流式稳定输出由适配层结合Harness当前输出快照给出，同message_id只末尾追加。若上游暂只支持已落盘历史，后端须补当前输出恢复后再宣称支持任务期间重连。
6. ends_message=true仅表示到达读取快照末尾，不等于任务完成。空会话items=[]/before_cursor=null；没有更旧数据before_cursor=null。无next_cursor、摘要目录或正文revision。

### 3.4 墨水屏阅读与大量历史

进入Session只取最近一批，本地按480×800正文区排版，默认停最新一屏。音量键先翻已缓存屏幕页；一条长回复可占多屏，几条短消息可共一屏，HTTP一批不对应一屏。

翻到缓存最早处还要继续时，用before_cursor取更早正文，接在前方；按message_id+文本字节偏移维持阅读位置，不能跳页。只缓存阅读附近有限字节，远处可释放再读，不能创建全会话LVGL对象/分页索引。

看历史时新输出不抢页，显示“有新消息”；看最新时合并刷新。进入/恢复期间先短暂缓存WS增量，与HTTP消息的id/字节范围合并：已包含范围跳过，连续后续追加，缺口重新取Session最新消息。即使正文前部已释放，也保留已知绝对字节末位置。

### 3.5 Run对象

~~~json
{
  "run_id": "run_01",
  "session_id": "sess_01",
  "utterance_id": "utt_01",
  "source": "device",
  "status": "waiting_approval",
  "state_version": 4,
  "model_id": "deepseek-chat",
  "authorization_level_id": "request_approval",
  "settings_version": 1,
  "pending_approval_id": "approval_01",
  "final_message_id": null,
  "last_progress": "等待批准",
  "error": null
}
~~~

source=device/web，网页文字utterance_id=null；无审批pending_approval_id=null，无最终回复final_message_id=null，无进度last_progress=null，非失败error=null。status为queued/transcribing/running/waiting_approval/cancelling/completed/failed/cancelled，终态不重新打开。等待时pending_approval_id须有效。

## 4. WebSocket连接与业务

### 4.1 hello、同步、心跳

命令v=1/type/command_id/data；会话相关带session_id，已有任务相关带run_id。每种完整报文见示例W01起。

1. WS连接后10秒内hello(data=device_id/firmware_version)，服务端hello回connection_id/sync_token及20秒心跳、60秒超时。
2. 建立同一时刻的非终态run和pending审批快照，并从边界缓存后续事件。GET /sync分页items[kind=run/approval,data=完整对象]，token绑定当前connection_id、120秒有效，不是认证Token。
3. 设备读齐各页后替换本地活动集合，sync.complete(data={sync_token})；服务端ack.data.status=syncing，先发缓存事件，再sync.ready，之后实时推送。切换不得漏事件。
4. sync.ready前不录音上传/审批/取消，也不弹旧审批；请求过早报not_ready。快照过期/缓存满sync_expired并关闭重试。跟踪run已不在快照则GET run确认终态；正文从Session接口读。
5. 同device_id新连接接管时关闭旧连接并阻止旧写。切页不关WS。重连退避1/2/4/8/16/30秒；WS Ping/Pong每20秒、60秒无Pong断开。UI线程不阻塞。

事件信封：
~~~json
{
  "v": 1,
  "type": "run.progress",
  "connection_id": "conn_01",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "state_version": 3,
    "text": "正在构建"
  }
}
~~~

seq在当前connection_id内从1连续递增，十进制字符串，去重/跳号重连。新连接重置，不保存/回放24小时日志。Session级run_id=null，控制hello/ack/sync.ready/error无seq。关键状态先保存再发，发送队列有界，满断开重同步，不静默丢事件。设备应用完某对象快照后，忽略同对象更旧版本事件；新连接先清旧增量队列再同步，避免把旧连接数据混入新会话。

ack目标2秒内，10秒未到=结果未知，不擅自新建任务。按device_id+command_id保存原结果，原命令重试回原结果，同ID改payload报command_conflict。临时upload_id不跨连接复用，录音另依utterance_id去重。

### 4.1.1 基于2.0.63的WS底层适配

当前仓库的components/esp-ml307/include/web_socket.h中，receive_buffer_size_默认2048字节；src/web_socket.cc据此限制接收单帧payload。这是现有底层默认值，协议仍约定完整JSON消息≤8192字节、每个Binary音频消息≤4096字节。单帧、完整WS消息和TCP读取回调是不同边界，不能把一次回调当一条完整业务消息。

- 固件创建Harness连接后、Connect前调用SetReceiveBufferSize(8192)，使接收单帧payload至少支持8192字节；接收缓存还须容纳WS帧头和TCP拆包/合包。后端可将上限内JSON发为一个文本帧，不要求为旧2KB默认值额外切业务报文。
- WS continuation帧应由底层重组；完整重组后的JSON仍限8192字节，不能只限制各帧。分片累积状态改为连接实例持有，关闭/重连时清空；解析JSON使用实际长度，不能假定回调数据以NUL结尾。
- 4096字节音频是设备向服务端发送的Binary消息，不是TCP分包大小，也不因旧接收默认值自动改成2048字节。单帧接收配置不会替代完整消息、上传总量和发送队列的边界检查。
- 联调覆盖超过2048字节至8192字节的UTF-8 JSON、超过8192字节拒绝、continuation/TCP拆包合包、分片中断后重连；这些是固件适配要求，尚未表示当前底层已改好。报文格式不变。

### 4.2 BOOT录音和ASR

- 聊天页长按BOOT锁当前Session；Dashboard/列表长按用固定闲聊。按住录音，松开上传；目标不因切页变化。初始化前松开不启动录音。
- PCM16kHz/单声道/signed16-bit小端，无WAV头。200ms–30s，即6400–960000字节。设备只保留一段未确认接受的录音，不建立录音队列；RAM掉电丢失不报已发送。
- 松开BOOT后锁住PCM缓冲并自动上传，显示“正在发送”；未确认期间不开始第二段录音，也不因切页释放或覆盖缓冲。发送失败/确认超时保留录音，提示重试，或由用户明确放弃后重新录制；放弃本地录音不等于取消服务端可能已接受的任务。
- audio.begin含utterance_id、expected_settings_version、format/sample_rate/channels、duration_ms/total_bytes/实际PCM sha256。先查已接受录音，再查忙/配置。允许时ack upload_required/upload_id/max_chunk_bytes。
- 顺序发送≤4096字节且偶数长度Binary，无Base64/额外头；每连接一个上传，新上传同时发报upload_busy。之后audio.end(data=utterance_id/upload_id)。
- end先查录音去重，再核对长度/摘要、原子检查Session空闲和配置版本，可靠保存音频/任务/映射后ack accepted/run_id/所用配置。begin/end之间网页任务抢占报session_busy，改配置报settings_conflict，不静默换模型执行。
- 设备收到与当前utterance_id匹配的accepted确认并取得run_id后，才能释放/复用PCM缓冲；audio.begin重试返回的accepted同样有效。无需等待ASR、智能体执行或任务完成。upload_required、Binary发送成功、audio.end发送成功均不代表服务端已接受。释放缓冲后，新输入仍受同Session单任务限制。
- 已接受同utterance优先回原run，不被忙/配置变化拦截。同ID不同会话/内容utterance_conflict。断线或上传15秒空闲清临时数据并断开；重连用新command_id、原utterance从头begin，若accepted则不再传PCM。提交失败清临时上传但保留设备录音供用户明确重试。
- ASR成功先保存用户消息、发asr.final(utterance_id/message_id/text/created_at)回显，再调智能体。极长转写text=null、附简短text_preview，从同Session接口取正文（示例B14）；无语音/识别失败run.failed，不启动空任务。
- Demo期间保存去重关系；网络重试不换utterance，主动重新做任务才新建。重置Demo数据时双方一起清旧任务/录音状态。

### 4.3 任务输出与取消

| 事件 | data |
|---|---|
| run.accepted / run.state | 完整Run，分别已提交/转写/执行/等待/取消中 |
| asr.final | utterance_id、message_id、text、created_at |
| run.progress | state_version、text；公开阶段信息 |
| message.delta | message_id、offset_bytes、text、created_at |
| message.completed | message_id、content_bytes（最终正文总字节数） |
| approval.requested / approval.resolved | 第5节完整Approval |
| run.completed / run.failed / run.cancelled | 完整终态Run |
| session.updated | 完整Session |

run状态/待审批变化加state_version；旧进度不覆盖新状态或终态。正文按id/字节偏移拼接，无正文版本；message.completed不重复发送大文本，从已收增量或同Session历史得到正文。

单Session最多1个非终态run，网页/设备一同受限；新输入busy，不排队/不打断，不把语音当审批。不同Session可并发。断线/切页不取消任务。run.cancel接受后cancelling，真正停了才cancelled；不支持报cancel_not_supported，自然先结束则真实completed/failed。取消不回滚已执行操作。成功前存最终消息，失败/取消保留已有公开文字。

## 5. 审批：完整操作一次返回

~~~json
{
  "approval_id": "approval_01",
  "session_id": "sess_01",
  "run_id": "run_01",
  "status": "pending",
  "approval_version": 1,
  "title": "允许清理构建目录吗？",
  "reason": "清理旧构建输出后重新构建。",
  "tool_name": "shell",
  "operation": {
    "command": "rm -rf ./build",
    "cwd": "/home/example/eink4",
    "targets": [
      "/home/example/eink4/build"
    ]
  },
  "device_actionable": true,
  "expires_at": "2026-11-24T03:35:00Z",
  "decision": null,
  "actor_source": null
}
~~~

status=pending/approved/rejected/expired/cancelled；pending版本1，最终决定后递增。每个approval_id绑定不可变操作，操作变则新审批。decision仅批准/拒绝时为approve_once/reject，其他null；actor_source=device/web/server，pending为null。

WS请求已带完整操作，GET /approvals/{id}核对状态也一次返完整对象，无/details分块。tool_name与operation取真实工具关键参数，shell命令只是示例，不用省略号替代实际操作范围。

极少数脚本/补丁导致完整审批JSON>8192字节：operation=null、device_actionable=false，明确“请到Harness网页处理”。设备和后端都禁止该项设备approve_once，仍可reject；不截断危险操作让用户盲批。原生网页保留完整工具详情。

approval.respond带approval_id/expected_approval_version/decision。后端校验Session/run/版本/期限/可操作性，原子持久决定后ack(approval_id/status/approval_version)，再resolved完整对象，并按真实结果恢复/结束run。

网页/设备并发只一个决定；同command重试回原结果，新command处理旧审批返回approval_already_resolved+current_approval。批准与取消竞争，恢复工具前核对run未取消。每run一次只pending一个，真正执行前暂停。拒绝传给Harness解释/换方案，断网/掉电不批准，默认15分钟到期按服务端时钟expired。时间到/网页抢先决定时后端返回当前完整审批对象，设备不能本地假定仍可批准。取消任务使未决审批cancelled，清pending_approval_id并保持run状态一致。

### 墨水屏展示

当前Session审批到达：聊天中按approval_id插“需要批准”系统提示并打开审批子页，保留标题和原阅读位置。正文展示原因、工具、完整操作/目标，音量键翻详情；底部暂换“仅本次允许 / 拒绝”。回聊天恢复模型/授权按钮，pending仍有查看入口。

其他Session待审批只显示可点击提醒，不强制跳页；多个请求排队。处理后回原页面，不改普通录音会话。审批子页BOOT不录音、不批准；同Session忙时不创建第二任务。

点击后禁用按钮显示提交中，ack/resolved后显示真实决定，任务完成另看run。离线禁用提交，重连sync.ready后只显示仍pending；超长操作只允许网页批准。

## 6. 必要异常、界限与验收

已接受utterance/run映射、审批决定、任务和公开消息在Demo保留期可靠保存，可复用Harness/SQLite，不要求分布式架构。服务重启后从真实Harness恢复；操作结果未知时待核查、不自动重做，人工解除前阻止该Session重复提交。

HTTP失败{"error":{code,message,retryable,details}}；WS失败v/type=ack/command_id/ok=false/error；连接错误type=error。details为空{}，配置冲突带current_session，busy带active_run_id，审批冲突带current_approval。

| code | HTTP适用状态 | 处理 |
|---|---|---|
| invalid_request / invalid_audio | 400 | 参数/音频问题 |
| resource_not_found | 404 | 刷新，不静默换会话 |
| session_busy / settings_conflict | 409 | 显示任务/配置，由用户决定 |
| command_conflict / utterance_conflict | 409 | ID内容冲突，停自动重试 |
| history_cursor_expired | 409 | 重新取Session最近正文 |
| approval_already_resolved / approval_expired | 409 | 显示最新审批 |
| approval_requires_web | 409 | 转网页批准 |
| upload_busy / not_ready | 409 | 等上传/同步完成 |
| upload_expired / sync_expired | 409 | 重连；未接受录音同utterance重传 |
| model_unavailable / authorization_level_unavailable | 409 | 刷新目录 |
| audio_too_large | 413 | 最多30秒 |
| cancel_not_supported | 409 | 保持真实状态 |
| upstream_unavailable | 503 | 明确暂不可用 |
| asr_no_speech / asr_failed | 任务错误 | run.failed，不启动智能体 |

报文上限含全部JSON转义字节；大列表少返给游标，消息正文按3.3节续取，超长审批转网页。参数不能塞无限日志。接收须正确重组跨回调分片。后端250–1000ms合并输出，屏幕1–2秒/整句合并，转写/审批/终态优先，不逐token刷屏。

Demo保持Dashboard/待机画面网络在线，需改旧浅睡关网策略；明确关机后只能唤醒恢复，不能宣称离线通知。

| 验收场景 | 要求 |
|---|---|
| 完整语音→转写→任务→审批→完成 | 转写先回显，阶段真实 |
| 10分钟任务/切页/断网 | 不丢任务不串会话，恢复正文和状态 |
| 上传确认丢失/重传 | 一个utterance仅一个run；未确认不覆盖录音，accepted后可释放缓冲 |
| 同Session并发/上传时改配置 | 原子busy/版本检查 |
| 重复审批/网页竞争/拒绝/过期/取消 | 唯一决定，操作批准前不执行 |
| 大会话/长回复 | 最近先显，按需旧记录，不混字节或抢页 |
| WS长度与分片 | 大于2KB且不超过8KB的JSON正常接收；超限拒绝，分片/重连不串消息 |
| 超长审批 | 显示网页处理，设备不盲批 |
| 重启 | 保存去重/待审批，RAM录音丢失不假报成功 |

后端交付固定地址端口、真实模型/授权映射、适配服务和验收结果。固件交付协议适配、历史缓存排版、审批UI、在线电源策略。两份文档静态校验不等于真实联调通过。
