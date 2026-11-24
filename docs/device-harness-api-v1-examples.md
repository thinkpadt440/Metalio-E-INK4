# Metalio E-Ink4 ↔ Harness 完整报文示例 v1（Demo）

日期：2026-11-24 · 配套[接口约定](device-harness-api-v1.md)。以下逐项展开现有协议，不增加新接口。

## 1. 阅读说明

HTTP各例独立；H07配置变更不代表WS主线提前改了模型。W01–W25为成功链，B/E为互斥分支，不在同任务上连续全执行。字段值为示例，后端使用真实数据；JSON块均为完整报文/Body，HTTP公共头已列出。

## 2. HTTP完整请求与响应

### H01 · Dashboard

~~~http
GET /api/v1/dashboard/overview?range=30d HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "range": "30d",
  "timezone": "Asia/Shanghai",
  "sessions": 53,
  "messages": 5980,
  "total_tokens": 1900000000,
  "active_days": 22,
  "peak_hour": "14:00",
  "favorite_model": {
    "id": "deepseek-chat",
    "name": "DeepSeek Chat"
  },
  "unavailable_fields": []
}
~~~

### H02 · 模型目录

~~~http
GET /api/v1/models HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "default_model_id": "deepseek-chat",
  "items": [
    {
      "id": "deepseek-chat",
      "name": "DeepSeek Chat",
      "available": true
    },
    {
      "id": "deepseek-reasoner",
      "name": "DeepSeek Reasoner",
      "available": true
    }
  ],
  "next_cursor": null
}
~~~

### H03 · 授权目录

~~~http
GET /api/v1/authorization-levels HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "default_authorization_level_id": "request_approval",
  "items": [
    {
      "id": "request_approval",
      "name": "请求批准",
      "available": true
    },
    {
      "id": "full_access",
      "name": "完全访问",
      "available": true
    }
  ],
  "next_cursor": null
}
~~~

### H04 · 最近工作会话

~~~http
GET /api/v1/sessions?limit=10 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "items": [
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
  ],
  "next_cursor": null
}
~~~

### H05 · 固定闲聊

~~~http
PUT /api/v1/devices/metalio_01/default-casual-session HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 201，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "session": {
    "id": "sess_casual_01",
    "title": "固定闲聊",
    "kind": "casual",
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
}
~~~

已有则HTTP200，id不变；并发PUT也只能创建一个。

### H06 · 会话详情

~~~http
GET /api/v1/sessions/sess_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

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

### H07 · 配置变更

~~~http
PATCH /api/v1/sessions/sess_01/settings HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
Content-Type: application/json

{
  "expected_settings_version": 1,
  "model_id": "deepseek-reasoner",
  "authorization_level_id": "request_approval"
}
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "id": "sess_01",
  "title": "固件开发",
  "kind": "work",
  "model_id": "deepseek-reasoner",
  "model_name": "DeepSeek Reasoner",
  "authorization_level_id": "request_approval",
  "authorization_level_name": "请求批准",
  "settings_version": 2,
  "session_version": 2,
  "active_run_id": null,
  "pending_approval_count": 0,
  "updated_at": "2026-11-24T03:20:00Z"
}
~~~

独立分支；WS语音成功主线仍使用配置版本1。

### H08 · 消息和正文一次读取

~~~http
GET /api/v1/sessions/sess_01/messages?limit=10 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

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

### H08a · 超长消息先返回末尾

~~~http
GET /api/v1/sessions/sess_01/messages?limit=10 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "items": [
    {
      "id": "msg_02",
      "role": "assistant",
      "content": "构建完成。",
      "offset_bytes": 28,
      "ends_message": true,
      "created_at": "2026-11-24T03:20:01Z"
    }
  ],
  "before_cursor": "hist_01"
}
~~~

用短文本演示分片；正常短消息不刻意拆分。

### H08b · 同Session接口续取较早内容

~~~http
GET /api/v1/sessions/sess_01/messages?limit=10&before=hist_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

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
      "content": "正在检查项目配置。\n",
      "offset_bytes": 0,
      "ends_message": false,
      "created_at": "2026-11-24T03:20:01Z"
    }
  ],
  "before_cursor": null
}
~~~

msg_02按offset拼到H08a之前，得到完整正文；没有正文版本或单消息接口。

### H08c · 空会话

~~~http
GET /api/v1/sessions/sess_01/messages?limit=10 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "items": [],
  "before_cursor": null
}
~~~

### H09 · 任务详情

~~~http
GET /api/v1/runs/run_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

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

### H10 · 审批完整状态和操作

~~~http
GET /api/v1/approvals/approval_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

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

### H10a · 操作过长时转网页

~~~http
GET /api/v1/approvals/approval_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "approval_id": "approval_01",
  "session_id": "sess_01",
  "run_id": "run_01",
  "status": "pending",
  "approval_version": 1,
  "title": "详情过长，请到Harness网页处理",
  "reason": "清理旧构建输出后重新构建。",
  "tool_name": "shell",
  "operation": null,
  "device_actionable": false,
  "expires_at": "2026-11-24T03:35:00Z",
  "decision": null,
  "actor_source": null
}
~~~

设备禁用允许，仍可拒绝；后端也不能接受该审批的设备approve_once。

### H11 · 重连活动快照

~~~http
GET /api/v1/sync?sync_token=sync_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "items": [
    {
      "kind": "run",
      "data": {
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
    },
    {
      "kind": "approval",
      "data": {
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
    }
  ],
  "next_cursor": null
}
~~~

### H11a · 初次空闲快照

~~~http
GET /api/v1/sync?sync_token=sync_01 HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 200，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "items": [],
  "next_cursor": null
}
~~~

## 3. HTTP公共错误

### HE01 · 错误参数

~~~http
GET /api/v1/dashboard/overview?range=90d HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 400，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "error": {
    "code": "invalid_request",
    "message": "range仅支持all/30d/7d",
    "retryable": false,
    "details": {}
  }
}
~~~

### HE02 · 错误资源

~~~http
GET /api/v1/runs/missing HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 404，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "error": {
    "code": "resource_not_found",
    "message": "任务不存在",
    "retryable": false,
    "details": {}
  }
}
~~~

### HE03 · 配置冲突

~~~http
PATCH /api/v1/sessions/sess_01/settings HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
Content-Type: application/json

{
  "expected_settings_version": 1,
  "model_id": "deepseek-reasoner",
  "authorization_level_id": "request_approval"
}
~~~

响应HTTP 409，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "error": {
    "code": "settings_conflict",
    "message": "配置已改变",
    "retryable": false,
    "details": {
      "current_session": {
        "id": "sess_01",
        "title": "固件开发",
        "kind": "work",
        "model_id": "deepseek-reasoner",
        "model_name": "DeepSeek Reasoner",
        "authorization_level_id": "request_approval",
        "authorization_level_name": "请求批准",
        "settings_version": 2,
        "session_version": 2,
        "active_run_id": null,
        "pending_approval_count": 0,
        "updated_at": "2026-11-24T03:20:00Z"
      }
    }
  }
}
~~~

### HE04 · 历史游标失效

~~~http
GET /api/v1/sessions/sess_01/messages?before=expired HTTP/1.1
Host: <host>:<port>
X-Device-ID: metalio_01
Accept: application/json
~~~

响应HTTP 409，Content-Type:application/json；GET响应Cache-Control:no-store。

~~~json
{
  "error": {
    "code": "history_cursor_expired",
    "message": "重新加载最近消息",
    "retryable": true,
    "details": {}
  }
}
~~~

## 4. WebSocket完整报文

以下沿用完整JSON消息≤8192字节、每个Binary音频消息≤4096字节的约定。当前2.0.63分支的WebSocket底层默认接收单帧payload为2048字节，固件须在Connect前调用SetReceiveBufferSize(8192)，并实现重组后限长、分片状态随连接清理及按实际长度解析；详见[接口约定4.1.1](device-harness-api-v1.md#411-基于2063的ws底层适配)。后端可发送上限内单文本帧，不必按2KB拆业务消息。WS帧头不计入JSON/Binary payload上限；一次TCP回调不等于一条WS消息。

### W01 · hello（设备→后端）

~~~json
{
  "v": 1,
  "type": "hello",
  "command_id": "cmd_hello",
  "data": {
    "device_id": "metalio_01",
    "firmware_version": "<firmware-version>"
  }
}
~~~

### W02 · hello响应（后端→设备）

~~~json
{
  "v": 1,
  "type": "hello",
  "command_id": "cmd_hello",
  "data": {
    "connection_id": "conn_01",
    "sync_token": "sync_01",
    "heartbeat_interval_ms": 20000,
    "heartbeat_timeout_ms": 60000
  }
}
~~~

### W03 · 快照读取完成（设备→后端）

~~~json
{
  "v": 1,
  "type": "sync.complete",
  "command_id": "cmd_sync",
  "data": {
    "sync_token": "sync_01"
  }
}
~~~

### W04 · 同步确认（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_sync",
  "ok": true,
  "data": {
    "status": "syncing"
  }
}
~~~

### W05 · 允许业务交互（后端→设备）

~~~json
{
  "v": 1,
  "type": "sync.ready",
  "data": {}
}
~~~

### W06 · 申请录音上传（设备→后端）

松开BOOT后锁住本次PCM缓冲并自动发送；页面显示“正在发送”，在确认接受前禁止开始第二段录音。失败/结果未知仍保留缓冲供重试，不默默覆盖。

~~~json
{
  "v": 1,
  "type": "audio.begin",
  "command_id": "cmd_begin",
  "session_id": "sess_01",
  "data": {
    "utterance_id": "utt_01",
    "expected_settings_version": 1,
    "format": "pcm_s16le",
    "sample_rate": 16000,
    "channels": 1,
    "duration_ms": 4250,
    "total_bytes": 136000,
    "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  }
}
~~~

### W07 · 允许二进制上传（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_begin",
  "ok": true,
  "data": {
    "status": "upload_required",
    "upload_id": "upload_01",
    "max_chunk_bytes": 4096
  }
}
~~~

### W08 · 提交录音（设备→后端）

全部PCM二进制块发完后发送。

~~~json
{
  "v": 1,
  "type": "audio.end",
  "command_id": "cmd_end",
  "session_id": "sess_01",
  "data": {
    "utterance_id": "utt_01",
    "upload_id": "upload_01"
  }
}
~~~

### W09 · 录音接受确认（后端→设备）

设备校验utterance_id匹配并保存run_id后，即可释放/复用PCM缓冲，不必等W12转写或W24任务结束。W07的upload_required以及PCM/audio.end发送成功都不能替代此确认；同Session仍只能有一个非终态任务。

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_end",
  "ok": true,
  "data": {
    "status": "accepted",
    "utterance_id": "utt_01",
    "run_id": "run_01",
    "settings_version": 1,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval"
  }
}
~~~

### W10 · 任务已接受（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.accepted",
  "connection_id": "conn_01",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "queued",
    "state_version": 1,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": null,
    "error": null
  }
}
~~~

### W11 · 正在转写（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.state",
  "connection_id": "conn_01",
  "seq": "2",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "transcribing",
    "state_version": 2,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": "正在转写",
    "error": null
  }
}
~~~

### W12 · 先回显转写（后端→设备）

~~~json
{
  "v": 1,
  "type": "asr.final",
  "connection_id": "conn_01",
  "seq": "3",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "utterance_id": "utt_01",
    "message_id": "msg_01",
    "text": "帮我构建固件。",
    "created_at": "2026-11-24T03:20:00Z"
  }
}
~~~

### W13 · 智能体运行（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.state",
  "connection_id": "conn_01",
  "seq": "4",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "running",
    "state_version": 3,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": "正在检查项目",
    "error": null
  }
}
~~~

### W14 · 进度（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.progress",
  "connection_id": "conn_01",
  "seq": "5",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "state_version": 3,
    "text": "正在检查项目"
  }
}
~~~

### W15 · 助手增量（后端→设备）

~~~json
{
  "v": 1,
  "type": "message.delta",
  "connection_id": "conn_01",
  "seq": "6",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "message_id": "msg_02",
    "offset_bytes": 0,
    "text": "正在检查项目配置。\n",
    "created_at": "2026-11-24T03:20:01Z"
  }
}
~~~

### W16 · 等待批准（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.state",
  "connection_id": "conn_01",
  "seq": "7",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
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
}
~~~

### W17 · 完整审批推送（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.requested",
  "connection_id": "conn_01",
  "seq": "8",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
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
}
~~~

### W18 · 仅本次允许（设备→后端）

~~~json
{
  "v": 1,
  "type": "approval.respond",
  "command_id": "cmd_allow",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "expected_approval_version": 1,
    "decision": "approve_once"
  }
}
~~~

### W19 · 允许确认（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_allow",
  "ok": true,
  "data": {
    "approval_id": "approval_01",
    "status": "approved",
    "approval_version": 2
  }
}
~~~

### W20 · 已允许（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.resolved",
  "connection_id": "conn_01",
  "seq": "9",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "session_id": "sess_01",
    "run_id": "run_01",
    "status": "approved",
    "approval_version": 2,
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
    "decision": "approve_once",
    "actor_source": "device"
  }
}
~~~

### W21 · 任务继续（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.state",
  "connection_id": "conn_01",
  "seq": "10",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "running",
    "state_version": 5,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": "正在重新构建",
    "error": null
  }
}
~~~

### W22 · 正文继续（后端→设备）

~~~json
{
  "v": 1,
  "type": "message.delta",
  "connection_id": "conn_01",
  "seq": "11",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "message_id": "msg_02",
    "offset_bytes": 28,
    "text": "构建完成。",
    "created_at": "2026-11-24T03:20:01Z"
  }
}
~~~

### W23 · 消息完成（后端→设备）

~~~json
{
  "v": 1,
  "type": "message.completed",
  "connection_id": "conn_01",
  "seq": "12",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "message_id": "msg_02",
    "content_bytes": 43
  }
}
~~~

### W24 · 任务完成（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.completed",
  "connection_id": "conn_01",
  "seq": "13",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "completed",
    "state_version": 6,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": "msg_02",
    "last_progress": "构建完成",
    "error": null
  }
}
~~~

### W25 · 会话状态更新（后端→设备）

~~~json
{
  "v": 1,
  "type": "session.updated",
  "connection_id": "conn_01",
  "seq": "14",
  "session_id": "sess_01",
  "run_id": null,
  "data": {
    "id": "sess_01",
    "title": "固件开发",
    "kind": "work",
    "model_id": "deepseek-chat",
    "model_name": "DeepSeek Chat",
    "authorization_level_id": "request_approval",
    "authorization_level_name": "请求批准",
    "settings_version": 1,
    "session_version": 4,
    "active_run_id": null,
    "pending_approval_count": 0,
    "updated_at": "2026-11-24T03:25:00Z"
  }
}
~~~

### B01 · 同录音重试begin（设备→后端）

重连分支，同utterance、新command_id。

~~~json
{
  "v": 1,
  "type": "audio.begin",
  "command_id": "cmd_begin_retry",
  "session_id": "sess_01",
  "data": {
    "utterance_id": "utt_01",
    "expected_settings_version": 1,
    "format": "pcm_s16le",
    "sample_rate": 16000,
    "channels": 1,
    "duration_ms": 4250,
    "total_bytes": 136000,
    "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  }
}
~~~

### B02 · 原录音已接受（后端→设备）

不再发送PCM。与W09一样，确认utterance_id并保存run_id后即可释放录音缓冲；若重试返回upload_required，则仍保留原PCM并从头上传。若用户明确放弃本地录音，不代表取消了服务端可能已接受的任务。

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_begin_retry",
  "ok": true,
  "data": {
    "status": "accepted",
    "utterance_id": "utt_01",
    "run_id": "run_01",
    "settings_version": 1,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval"
  }
}
~~~

### B03 · 拒绝请求（设备→后端）

~~~json
{
  "v": 1,
  "type": "approval.respond",
  "command_id": "cmd_reject",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "expected_approval_version": 1,
    "decision": "reject"
  }
}
~~~

### B04 · 拒绝确认（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_reject",
  "ok": true,
  "data": {
    "approval_id": "approval_01",
    "status": "rejected",
    "approval_version": 2
  }
}
~~~

### B05 · 已拒绝（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.resolved",
  "connection_id": "conn_B05",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "session_id": "sess_01",
    "run_id": "run_01",
    "status": "rejected",
    "approval_version": 2,
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
    "decision": "reject",
    "actor_source": "device"
  }
}
~~~

### B06 · 过期（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.resolved",
  "connection_id": "conn_B06",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "session_id": "sess_01",
    "run_id": "run_01",
    "status": "expired",
    "approval_version": 2,
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
    "actor_source": "server"
  }
}
~~~

### B07 · 取消关闭审批（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.resolved",
  "connection_id": "conn_B07",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "session_id": "sess_01",
    "run_id": "run_01",
    "status": "cancelled",
    "approval_version": 2,
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
    "actor_source": "server"
  }
}
~~~

### B08 · 超长操作转网页（后端→设备）

~~~json
{
  "v": 1,
  "type": "approval.requested",
  "connection_id": "conn_B08",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "approval_id": "approval_01",
    "session_id": "sess_01",
    "run_id": "run_01",
    "status": "pending",
    "approval_version": 1,
    "title": "详情过长，请到Harness网页处理",
    "reason": "清理旧构建输出后重新构建。",
    "tool_name": "shell",
    "operation": null,
    "device_actionable": false,
    "expires_at": "2026-11-24T03:35:00Z",
    "decision": null,
    "actor_source": null
  }
}
~~~

### B09 · ASR失败（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.failed",
  "connection_id": "conn_B09",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "failed",
    "state_version": 3,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": null,
    "error": {
      "code": "asr_no_speech",
      "message": "没有有效语音，请重新录音",
      "retryable": false,
      "details": {}
    }
  }
}
~~~

### B10 · 取消请求（设备→后端）

~~~json
{
  "v": 1,
  "type": "run.cancel",
  "command_id": "cmd_cancel",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {}
}
~~~

### B11 · 取消确认（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_cancel",
  "ok": true,
  "data": {
    "run_id": "run_01",
    "status": "cancelling"
  }
}
~~~

### B12 · 正在取消（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.state",
  "connection_id": "conn_cancel",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "cancelling",
    "state_version": 4,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": "正在取消",
    "error": null
  }
}
~~~

### B13 · 确认已停止（后端→设备）

~~~json
{
  "v": 1,
  "type": "run.cancelled",
  "connection_id": "conn_cancel",
  "seq": "2",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "run_id": "run_01",
    "session_id": "sess_01",
    "utterance_id": "utt_01",
    "source": "device",
    "status": "cancelled",
    "state_version": 5,
    "model_id": "deepseek-chat",
    "authorization_level_id": "request_approval",
    "settings_version": 1,
    "pending_approval_id": null,
    "final_message_id": null,
    "last_progress": "已取消",
    "error": null
  }
}
~~~

### B14 · 超长ASR回显（后端→设备）

独立分支：完整转写已保存，事件只提示读取。设备仍从GET /sessions/sess_01/messages读取，不新增正文端点。

~~~json
{
  "v": 1,
  "type": "asr.final",
  "connection_id": "conn_long_asr",
  "seq": "1",
  "session_id": "sess_01",
  "run_id": "run_01",
  "data": {
    "utterance_id": "utt_01",
    "message_id": "msg_01",
    "text": null,
    "text_preview": "这是一段需要从历史读取的长转写……",
    "created_at": "2026-11-24T03:20:00Z"
  }
}
~~~

### E01 · 会话忙（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_begin_new",
  "ok": false,
  "error": {
    "code": "session_busy",
    "message": "会话已有任务",
    "retryable": false,
    "details": {
      "active_run_id": "run_01"
    }
  }
}
~~~

### E02 · 配置冲突（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_end",
  "ok": false,
  "error": {
    "code": "settings_conflict",
    "message": "配置已改变，本录音未执行",
    "retryable": false,
    "details": {
      "current_session": {
        "id": "sess_01",
        "title": "固件开发",
        "kind": "work",
        "model_id": "deepseek-reasoner",
        "model_name": "DeepSeek Reasoner",
        "authorization_level_id": "request_approval",
        "authorization_level_name": "请求批准",
        "settings_version": 2,
        "session_version": 2,
        "active_run_id": null,
        "pending_approval_count": 0,
        "updated_at": "2026-11-24T03:20:00Z"
      }
    }
  }
}
~~~

### E03 · 网页已批准（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_allow_new",
  "ok": false,
  "error": {
    "code": "approval_already_resolved",
    "message": "已由网页处理",
    "retryable": false,
    "details": {
      "current_approval": {
        "approval_id": "approval_01",
        "session_id": "sess_01",
        "run_id": "run_01",
        "status": "approved",
        "approval_version": 2,
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
        "decision": "approve_once",
        "actor_source": "web"
      }
    }
  }
}
~~~

### E04 · 审批过期（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_allow_late",
  "ok": false,
  "error": {
    "code": "approval_expired",
    "message": "审批已过期",
    "retryable": false,
    "details": {
      "current_approval": {
        "approval_id": "approval_01",
        "session_id": "sess_01",
        "run_id": "run_01",
        "status": "expired",
        "approval_version": 2,
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
        "actor_source": "server"
      }
    }
  }
}
~~~

### E05 · 只允许网页批准（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_allow_large",
  "ok": false,
  "error": {
    "code": "approval_requires_web",
    "message": "请到网页核对完整操作",
    "retryable": false,
    "details": {
      "current_approval": {
        "approval_id": "approval_01",
        "session_id": "sess_01",
        "run_id": "run_01",
        "status": "pending",
        "approval_version": 1,
        "title": "详情过长，请到Harness网页处理",
        "reason": "清理旧构建输出后重新构建。",
        "tool_name": "shell",
        "operation": null,
        "device_actionable": false,
        "expires_at": "2026-11-24T03:35:00Z",
        "decision": null,
        "actor_source": null
      }
    }
  }
}
~~~

### E06 · 不支持取消（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_cancel",
  "ok": false,
  "error": {
    "code": "cancel_not_supported",
    "message": "当前任务不支持取消",
    "retryable": false,
    "details": {}
  }
}
~~~

### E07 · 音频校验失败（后端→设备）

~~~json
{
  "v": 1,
  "type": "ack",
  "command_id": "cmd_end",
  "ok": false,
  "error": {
    "code": "invalid_audio",
    "message": "音频长度或摘要不一致",
    "retryable": false,
    "details": {}
  }
}
~~~

### E08 · upload_expired（后端→设备）

随后关闭连接；未接受录音仍用原utterance重传。

~~~json
{
  "v": 1,
  "type": "error",
  "error": {
    "code": "upload_expired",
    "message": "请重新连接",
    "retryable": true,
    "details": {}
  }
}
~~~

### E09 · sync_expired（后端→设备）

随后关闭连接；未接受录音仍用原utterance重传。

~~~json
{
  "v": 1,
  "type": "error",
  "error": {
    "code": "sync_expired",
    "message": "请重新连接",
    "retryable": true,
    "details": {}
  }
}
~~~

## 5. 二进制音频

W07之后发WS Binary消息，payload直接PCM，无WAV头、Base64或upload_id前缀。

~~~text
int16：0, 1000, -1000, 32767, -32768
小端：00 00 E8 03 18 FC FF 7F 00 80
~~~

上面10字节只说明编码，不足200ms。4250ms示例实际136000字节，33个4096字节块+832字节尾块，之后audio.end。摘要对真实PCM计算，不能用重复a示例。WS Ping/Pong是控制帧，连接库处理。

## 6. 时序与覆盖

初次空闲：W01/W02 → H11a → W03/W04/W05 → W06/W07 → Binary → W08/W09 → W10/W11/W12 → W13/W14/W15 → W16/W17 → W18/W19/W20 → W21/W22/W23/W24；W25刷新会话。H11是已有任务待审批时的独立恢复例。ack/事件可能交错，按ID与资源版本关联。

| 类别 | 覆盖 |
|---|---|
| HTTP | 全部11端点请求/响应；历史正文/长消息续取/空会话、超长审批、空/非空同步、4类错误 |
| WS命令 | hello、sync.complete、audio.begin、audio.end、approval.respond、run.cancel |
| WS控制 | hello、各命令ack、sync.ready、失败ack、连接error |
| WS事件 | run.accepted/state/progress/completed/failed/cancelled、asr.final、message.delta/completed、approval.requested/resolved、session.updated |
| 分支 | 同录音重试、拒绝/过期/取消审批、超长操作转网页、ASR失败/任务取消、并发/配置冲突、无效上传 |
| 固件适配 | WS默认2KB接收限制调整为支持8KB JSON；分片/超限/重连清理；录音未确认保留、accepted后释放 |

未单列的错误码复用同error结构，见规则文档；样例不等于服务已实现。协议字段/状态变更须双方同步两份文档，不各自猜测。
