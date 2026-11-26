#!/usr/bin/env python3
"""Dependency-free HTTP and WebSocket mock for the Metalio E-Ink4."""
import base64, hashlib, json, os, socket, struct, threading, urllib.parse
from datetime import datetime, timedelta, timezone
from collections import OrderedDict
from uuid import uuid4
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT=int(os.environ.get("HARNESS_MOCK_PORT","8080"))
API="/api/v1"
LOCK=threading.RLock()
CONNECTIONS=0
PEERS=set()
NOW="2026-11-24T03:20:00Z"
MODELS=[
 {"id":"deepseek-v4.1-flash","name":"DeepSeek V4.1-Flash","available":True},
 {"id":"deepseek-v4.1-pro","name":"DeepSeek V4.1-Pro","available":True}]
LEVELS=[
 {"id":"request_approval","name":"请求批准","available":True},
 {"id":"full_access","name":"完全访问","available":True}]
SESSIONS={
 "sess_firmware":{"id":"sess_firmware","title":"固件开发","kind":"work","model_id":MODELS[1]["id"],"authorization_level_id":LEVELS[0]["id"],"settings_version":1,"session_version":1,"active_run_id":None,"pending_approval_count":0,"updated_at":"2026-11-24T03:20:00Z","messages":[("user","帮我检查一下设备端的连接状态。"),("assistant","我会先确认 WebSocket 是否已连接，再检查同步状态。"),("user","如果断线，设备会自动重连吗？"),("assistant","会按退避间隔自动重连。当前是模拟会话，用于验证页面和接口。")]},
 "sess_planning":{"id":"sess_planning","title":"项目规划","kind":"work","model_id":MODELS[0]["id"],"authorization_level_id":LEVELS[1]["id"],"settings_version":1,"session_version":1,"active_run_id":None,"pending_approval_count":0,"updated_at":"2026-11-23T09:30:00Z","messages":[("user","帮我整理下一阶段的实现顺序。"),("assistant","先接通 Dashboard、Session 目录和会话设置，再实现消息历史与录音上传。")]},
 "sess_casual_01":{"id":"sess_casual_01","title":"固定闲聊","kind":"casual","model_id":MODELS[0]["id"],"authorization_level_id":LEVELS[0]["id"],"settings_version":1,"session_version":1,"active_run_id":None,"pending_approval_count":0,"updated_at":"2026-11-24T03:20:00Z","messages":[("assistant","你好，我是设备上的演示助手。你可以在这里验证会话页面和模型设置。")]}
}
# Stable byte bookmarks over append-only fixtures.
SESSIONS["sess_planning"]["messages"] = [
    ("user" if i % 2 else "assistant", f"第 {i} 条项目检查记录。")
    for i in range(1, 24)
] + [("assistant", "\n".join(
    f"{i:02d}. 核对设备连接、聊天消息和翻页。保持当前阅读位置🙂。"
    for i in range(1, 100)))]
HISTORY_CURSORS = OrderedDict()
STREAMING = set()


def history_page(session, limit, cursor):
    messages = session["messages"]
    if cursor:
        saved = HISTORY_CURSORS.get(cursor)
        if not saved or saved[0] != session["id"] or saved[1] <= time.monotonic():
            return None
        _, _, index, end = saved
    else:
        index = len(messages) - 1
        end = None

    items = []
    used = 0
    while index >= 0 and len(items) < limit:
        role, content = messages[index]
        encoded = content.encode()
        stop = len(encoded) if end is None else end
        if items and used + stop > 5400:
            break
        start = max(0, stop - 5400)
        while start < stop and encoded[start] & 0xc0 == 0x80:
            start += 1
        items.append({
            "id": f'{session["id"]}_msg_{index + 1:02d}', "role": role,
            "content": encoded[start:stop].decode(),
            "offset_bytes": start, "ends_message": end is None, "created_at": NOW,
        })
        used += stop - start
        if start:
            end = start
            break
        index -= 1
        end = None

    before = None
    if index >= 0:
        before = "hist_" + uuid4().hex
        if len(HISTORY_CURSORS) >= 256:
            HISTORY_CURSORS.popitem(last=False)
        HISTORY_CURSORS[before] = (session["id"], time.monotonic() + 600, index, end)
    return {"items": list(reversed(items)), "before_cursor": before}


def publish(event_type, session_id, data, run_id=None):
    for peer in PEERS:
        event = (event_type, session_id, data, run_id)
        if peer.ws_ready:
            try:
                peer.event(*event)
            except (ConnectionError, OSError):
                pass
        elif len(peer.ws_backlog) < 32:
            peer.ws_backlog.append(event)


def stream_example(session_id):
    parts = [
        "正在汇总设备状态。\n",
        "Dashboard 和会话目录已验证。\n",
        "请检查历史翻页和新消息提示。🙂",
    ]
    body = ""
    try:
        for part in parts:
            time.sleep(1.2)
            with LOCK:
                messages = SESSIONS[session_id]["messages"]
                if not body:
                    messages.append(("assistant", ""))
                message_id = f"{session_id}_msg_{len(messages):02d}"
                offset = len(body.encode())
                body += part
                messages[-1] = ("assistant", body)
                publish("message.delta", session_id, {
                    "message_id": message_id, "offset_bytes": offset,
                    "text": part, "created_at": NOW,
                }, run_id="run_mock_history")
        with LOCK:
            publish("message.completed", session_id, {
                "message_id": message_id, "content_bytes": len(body.encode()),
            }, run_id="run_mock_history")
    finally:
        with LOCK:
            STREAMING.discard(session_id)


ACCEPTED_AUDIO = {}
RUNS = {}
CANCEL_RESULTS = {}
APPROVALS = {}
APPROVAL_RESULTS = {}
APPROVAL_EXECUTIONS = set()
APPROVAL_MODES = ("approval", "approval_long", "approval_web_only", "approval_expire",
                  "approval_web_wins", "approval_lost_ack", "approval_version")
APPROVAL_EXPIRY_SECONDS = 2.5
WEB_RUN_OPTIONS = {}
ACTIVE_RUN_STATES = ("queued", "transcribing", "running", "waiting_approval", "cancelling")
WEB_STEP_SECONDS = 3
DROP_AUDIO_ONCE = os.environ.get("HARNESS_MOCK_DROP_AUDIO", "")


def drop_audio(stage):
    global DROP_AUDIO_ONCE
    if DROP_AUDIO_ONCE != stage:
        return False
    DROP_AUDIO_ONCE = ""
    print(f"Mock fault: dropped audio {stage}", flush=True)
    return True


def append_message(session_id, role, text):
    messages = SESSIONS[session_id]["messages"]
    messages.append((role, text))
    return f"{session_id}_msg_{len(messages):02d}"


def set_run_state(run, status, progress):
    run.update(status=status, state_version=run["state_version"] + 1, last_progress=progress)
    publish("run.state", run["session_id"], dict(run), run["run_id"])


def resolve_approval(approval, state, actor):
    if approval["status"] != "pending":
        return False
    approval.update(status=state, actor_source=actor, approval_version=approval["approval_version"] + 1,
                    decision={"approved": "approve_once", "rejected": "reject"}.get(state))
    run = RUNS[approval["run_id"]]
    run["pending_approval_id"] = None
    sid = approval["session_id"]
    session = SESSIONS[sid]
    if WEB_RUN_OPTIONS.get(run["run_id"]) != "approval_lost_ack":
        publish("approval.resolved", sid, dict(approval), run["run_id"])
    session["pending_approval_count"] = 0
    session["session_version"] += 1
    publish("session.updated", sid, view(session))
    return True


def open_approval(run, mode):
    sid = run["session_id"]
    approval_id = "approve_" + uuid4().hex
    operation = {"target": "设备联网状态", "checks": ["连接", "会话", "消息"],
                 "sample_id": 9007199254740993}
    if mode == "approval_long":
        operation["items"] = [f"{i} 项：只读检查网络、同步状态和消息顺序。" for i in range(1, 65)]
    if mode == "approval_web_only":
        operation = None
    expires = APPROVAL_EXPIRY_SECONDS if mode == "approval_expire" else 900
    approval = {
        "approval_id": approval_id, "session_id": sid, "run_id": run["run_id"], "status": "pending",
        "approval_version": 1, "title": "允许检查设备状态吗？", "reason": "读取示例状态，验证设备联调。",
        "tool_name": "inspect_status", "operation": operation, "device_actionable": operation is not None,
        "expires_at": (datetime.now(timezone.utc) + timedelta(seconds=expires)).isoformat(),
        "decision": None, "actor_source": None,
    }
    APPROVALS[approval_id] = approval
    run["pending_approval_id"] = approval_id
    set_run_state(run, "waiting_approval", "等待读取状态的批准")
    publish("approval.requested", sid, dict(approval), run["run_id"])
    session = SESSIONS[sid]
    session["pending_approval_count"] = 1
    session["session_version"] += 1
    publish("session.updated", sid, view(session))
    timer = threading.Timer(expires, expire_approval, args=(approval_id,))
    timer.daemon = True
    timer.start()


def expire_approval(approval_id):
    with LOCK:
        approval = APPROVALS[approval_id]
        if resolve_approval(approval, "expired", "server"):
            run = RUNS[approval["run_id"]]
            run["error"] = {"code": "approval_expired", "message": "审批已过期"}
            finish_run(run, "failed")


def apply_decision(approval, decision, actor):
    run = RUNS[approval["run_id"]]
    if run["status"] != "waiting_approval" or not resolve_approval(
            approval, "approved" if decision == "approve_once" else "rejected", actor):
        return False
    if decision == "approve_once":
        set_run_state(run, "running", "已允许本次检查")
        threading.Thread(target=execute_approved, args=(run["run_id"],), daemon=True).start()
    else:
        run["error"] = {"code": "approval_rejected", "message": "操作未获批准"}
        finish_run(run, "failed")
    return True


def execute_approved(run_id):
    time.sleep(0.25)
    with LOCK:
        run = RUNS[run_id]
        if run["status"] != "running":
            return
        # Record the simulated read-only operation before generating its result.
        APPROVAL_EXECUTIONS.add(run_id)
        complete_web_run(run)


def finish_run(run, status):
    sid = run["session_id"]
    approval = APPROVALS.get(run["pending_approval_id"])
    if approval:
        resolve_approval(approval, "cancelled", "server")
    run.update(status=status, state_version=run["state_version"] + 1,
               last_progress={"completed": "", "cancelled": "已取消", "failed": ""}[status] or None)
    if WEB_RUN_OPTIONS.get(run["run_id"]) != "lost_terminal":
        publish("run." + status, sid, dict(run), run["run_id"])
    session = SESSIONS[sid]
    if session["active_run_id"] == run["run_id"]:
        session["active_run_id"] = None
        session["session_version"] += 1
        publish("session.updated", sid, view(session))


def start_web_run(session, mode):
    sid = session["id"]
    run_id = "run_web_" + uuid4().hex
    run = {
        "run_id": run_id, "session_id": sid, "utterance_id": None,
        "source": "web", "status": "queued", "state_version": 1,
        "settings_version": session["settings_version"], "model_id": session["model_id"],
        "authorization_level_id": session["authorization_level_id"],
        "pending_approval_id": None, "final_message_id": None, "last_progress": None, "error": None,
    }
    RUNS[run_id] = run
    WEB_RUN_OPTIONS[run_id] = mode
    session["active_run_id"] = run_id
    session["session_version"] += 1
    append_message(sid, "user", "从网页启动一次设备状态检查。")
    publish("run.accepted", sid, dict(run), run_id)
    publish("session.updated", sid, view(session))
    threading.Thread(target=execute_web_run, args=(run_id,), daemon=True).start()
    return run_id


def complete_web_run(run):
    sid = run["session_id"]
    run_id = run["run_id"]
    text = "检查已完成，设备连接与消息记录正常。"
    message_id = append_message(sid, "assistant", text)
    run["final_message_id"] = message_id
    publish("message.delta", sid, {
        "message_id": message_id, "offset_bytes": 0, "text": text, "created_at": NOW,
    }, run_id)
    publish("message.completed", sid, {
        "message_id": message_id, "content_bytes": len(text.encode()),
    }, run_id)
    finish_run(run, "completed")


def execute_web_run(run_id):
    time.sleep(0.25)
    with LOCK:
        run = RUNS[run_id]
        if run["status"] != "queued":
            return
        mode = WEB_RUN_OPTIONS.get(run_id)
        if mode in APPROVAL_MODES:
            open_approval(run, mode)
            return
        set_run_state(run, "running", "正在检查连接")
    for progress in ("正在读取会话", "正在核对消息", "正在整理结果"):
        time.sleep(WEB_STEP_SECONDS)
        with LOCK:
            if run["status"] != "running":
                return
            run["last_progress"] = progress
            publish("run.progress", run["session_id"], {
                "state_version": run["state_version"], "text": progress,
            }, run_id)
    with LOCK:
        if run["status"] == "running":
            complete_web_run(run)


def finish_cancellation(run_id):
    time.sleep(0.6)
    with LOCK:
        run = RUNS[run_id]
        if run["status"] == "cancelling":
            finish_run(run, "cancelled")


def finish_mock_audio(run_id, has_voice):
    time.sleep(0.4)
    with LOCK:
        run = RUNS[run_id]
        sid = run["session_id"]
        if run["status"] != "queued":
            return
        run.update(status="transcribing", state_version=2, last_progress="正在转写")
        publish("run.state", sid, dict(run), run_id)
    time.sleep(0.8)
    with LOCK:
        if run["status"] != "transcribing":
            return
        if has_voice:
            text = "请帮我检查设备的联网状态。"
            message_id = append_message(sid, "user", text)
            publish("asr.final", sid, {
                "utterance_id": run["utterance_id"], "message_id": message_id,
                "text": text, "created_at": NOW,
            }, run_id)
            run.update(status="running", state_version=3, last_progress=None)
            publish("run.state", sid, dict(run), run_id)
    if has_voice:
        time.sleep(1.2)
    with LOCK:
        expected = "running" if has_voice else "transcribing"
        if run["status"] != expected:
            return
        if has_voice:
            text = "已收到语音并生成示例回复。"
            message_id = append_message(sid, "assistant", text)
            publish("message.delta", sid, {
                "message_id": message_id, "offset_bytes": 0, "text": text, "created_at": NOW,
            }, run_id)
            publish("message.completed", sid, {
                "message_id": message_id, "content_bytes": len(text.encode()),
            }, run_id)
            run["final_message_id"] = message_id
            finish_run(run, "completed")
        else:
            run["error"] = {"code": "asr_no_speech", "message": "没有检测到语音"}
            finish_run(run, "failed")


def view(s):
 d={k:v for k,v in s.items() if k!="messages"}
 d["model_name"]=next(x["name"] for x in MODELS if x["id"]==s["model_id"])
 d["authorization_level_name"]=next(x["name"] for x in LEVELS if x["id"]==s["authorization_level_id"])
 return d

def page(items, query):
 try:
  limit=int(query.get("limit", ["10"])[0])
  cursor=int(query.get("cursor", ["0"])[0])
  if not 1 <= limit <= 20 or not 0 <= cursor <= len(items):
   raise ValueError
 except ValueError:
  return None
 end=cursor+limit
 return {"items":items[cursor:end],"next_cursor":str(end) if end<len(items) else None}

class Handler(BaseHTTPRequestHandler):
 protocol_version="HTTP/1.1"
 server_version="Eink4Mock/1.0"
 def log_message(self,fmt,*args): print("[%s] %s %s"%(datetime.now().strftime("%H:%M:%S"),self.client_address[0],fmt%args),flush=True)
 def reply(self,status,obj):
  b=json.dumps(obj,ensure_ascii=False,separators=(",",":")).encode()
  self.send_response(status);self.send_header("Content-Type","application/json; charset=utf-8")
  self.send_header("Cache-Control","no-store");self.send_header("Content-Length",str(len(b)))
  self.end_headers();self.wfile.write(b)
 def body(self):
  try:
   n=int(self.headers.get("Content-Length","0"))
   if n<0 or n>8192:return None
   return json.loads((self.rfile.read(n) if n else b"{}").decode())
  except (ValueError,UnicodeDecodeError):return None
 def list_reply(self, items, query, **fields):
  result=page(items, query)
  if result is None:
   self.reply(400,{"error":{"code":"invalid_request","message":"invalid page","retryable":False,"details":{}}})
  else:
   self.reply(200,dict(result,**fields))

 def do_GET(self):
  u=urllib.parse.urlsplit(self.path);p=u.path;q=urllib.parse.parse_qs(u.query)
  if self.headers.get("Upgrade","").lower()=="websocket" and p==API+"/ws":self.websocket();return
  if p=="/health":self.reply(200,{"status":"ok","service":"Metalio E-Ink4 mock backend"});return
  if p==API+"/dashboard/overview":
   r=q.get("range",["all"])[0]
   values={"all":(53,5980,1900000000,22),"30d":(18,742,284100,16),"7d":(5,96,38100,5)}
   if r not in values:self.reply(400,{"error":{"code":"invalid_request","message":"range must be all, 30d or 7d"}});return
   a,b,c,d=values[r];self.reply(200,{"range":r,"timezone":"Asia/Shanghai","sessions":a,"messages":b,"total_tokens":c,"active_days":d,"peak_hour":"14:00","favorite_model":{"id":MODELS[1]["id"],"name":MODELS[1]["name"]},"unavailable_fields":[]});return
  if p==API+"/models":
   self.list_reply(MODELS,q,default_model_id=MODELS[1]["id"]);return
  if p==API+"/authorization-levels":
   self.list_reply(LEVELS,q,default_authorization_level_id=LEVELS[0]["id"]);return
  if p==API+"/sessions":
   with LOCK:
    rows=[view(x) for x in sorted(SESSIONS.values(),key=lambda x:x["updated_at"],reverse=True) if x["kind"]=="work"]
   self.list_reply(rows,q);return
  if p==API+"/sync":
   token=q.get("sync_token",[""])[0]
   with LOCK:
    peer=next((peer for peer in PEERS if peer.ws_token==token),None)
    snapshot=peer.ws_snapshot if peer else []
   try:
    start=int(q.get("cursor",["0"])[0])
    if not token or not 0<=start<=len(snapshot):
     raise ValueError
   except ValueError:
    self.reply(400,{"error":{"code":"invalid_request","message":"invalid sync cursor"}});return
   items=[]
   remaining=7800
   end=start
   while end<len(snapshot):
    item=snapshot[end]
    size=len(json.dumps(item,ensure_ascii=False,separators=(",",":")).encode())+1
    if items and size>remaining:
     break
    remaining-=size
    items.append(item)
    end+=1
   self.reply(200,{"items":items,"next_cursor":str(end) if end<len(snapshot) else None})
   return
  if p.startswith(API+"/approvals/"):
   approval_id=urllib.parse.unquote(p[len(API+"/approvals/"):])
   with LOCK:
    approval=APPROVALS.get(approval_id)
    if approval:self.reply(200,dict(approval))
    else:self.reply(404,{"error":{"code":"resource_not_found","message":"approval not found"}})
   return
  if p.startswith(API+"/runs/"):
   run_id=urllib.parse.unquote(p[len(API+"/runs/"):])
   with LOCK:
    run=RUNS.get(run_id)
    if run:self.reply(200,dict(run))
    else:self.reply(404,{"error":{"code":"resource_not_found","message":"run not found"}})
   return
  if p.startswith(API+"/sessions/"):
   parts=p[len(API+"/sessions/"):].split("/")
   sid=urllib.parse.unquote(parts[0])
   with LOCK:
    s=SESSIONS.get(sid)
    if not s:self.reply(404,{"error":{"code":"resource_not_found","message":"session not found"}});return
    if len(parts)==1:self.reply(200,view(s));return
    if len(parts)==2 and parts[1]=="messages":
     try:n=max(1,min(20,int(q.get("limit",["10"])[0])))
     except ValueError:n=10
     history=history_page(s,n,q.get("before",[""])[0])
     if history is None:
      self.reply(409,{"error":{"code":"history_cursor_expired","message":"reload latest messages",
                              "retryable":True,"details":{}}});return
     self.reply(200,history);return
  self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}})
 def do_PUT(self):
  prefix=API+"/devices/";suffix="/default-casual-session"
  if self.path.startswith(prefix) and self.path.endswith(suffix):
   device_id=urllib.parse.unquote(self.path[len(prefix):-len(suffix)])
   if not device_id or len(device_id)>128:
    self.reply(400,{"error":{"code":"invalid_request","message":"invalid device id","retryable":False,"details":{}}});return
   session_id="sess_casual_"+hashlib.sha256(device_id.encode()).hexdigest()[:24]
   with LOCK:
    existing=session_id in SESSIONS
    if not existing:
     SESSIONS[session_id]=dict(SESSIONS["sess_casual_01"],id=session_id,model_id=MODELS[1]["id"])
    session=view(SESSIONS[session_id])
   self.reply(200 if existing else 201,{"session":session})
  else:self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}})
 def do_PATCH(self):
  prefix=API+"/sessions/";suffix="/settings"
  if not self.path.startswith(prefix) or not self.path.endswith(suffix):
   self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}});return
  sid=urllib.parse.unquote(self.path[len(prefix):-len(suffix)]);data=self.body()
  if not isinstance(data,dict) or not isinstance(data.get("expected_settings_version"),int):
   self.reply(400,{"error":{"code":"invalid_request","message":"invalid settings body"}});return
  with LOCK:
   s=SESSIONS.get(sid)
   if not s:self.reply(404,{"error":{"code":"resource_not_found","message":"session not found"}});return
   if data["expected_settings_version"]!=s["settings_version"]:
    self.reply(409,{"error":{"code":"settings_conflict","message":"settings changed","details":{"current_session":view(s)}}});return
   if "model_id" in data and data["model_id"] not in [x["id"] for x in MODELS]:
    self.reply(400,{"error":{"code":"invalid_request","message":"unknown model"}});return
   if "authorization_level_id" in data and data["authorization_level_id"] not in [x["id"] for x in LEVELS]:
    self.reply(400,{"error":{"code":"invalid_request","message":"unknown authorization level"}});return
   changed=False
   if "model_id" in data and s["model_id"]!=data["model_id"]:
    s["model_id"]=data["model_id"];changed=True
   if "authorization_level_id" in data and s["authorization_level_id"]!=data["authorization_level_id"]:
    s["authorization_level_id"]=data["authorization_level_id"];changed=True
   if changed:
    s["settings_version"]+=1;s["session_version"]+=1
    s["updated_at"]=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
   result=view(s)
   if changed:
    publish("session.updated",sid,result)
   self.reply(200,result)
 def do_POST(self):
  url=urllib.parse.urlsplit(self.path)
  if url.path.startswith("/mock/approval/"):
   decision=urllib.parse.parse_qs(url.query).get("decision",["approve_once"])[0]
   with LOCK:
    approval=APPROVALS.get(url.path.rsplit("/",1)[-1])
    if not approval:
     self.reply(404,{"error":{"code":"resource_not_found"}});return
    if decision not in ("approve_once","reject") or not apply_decision(approval,decision,"web"):
     self.reply(409,{"error":{"code":"approval_already_resolved"}});return
    self.reply(200,dict(approval));return
  if url.path not in ("/mock/stream","/mock/run"):
   self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}});return
  query=urllib.parse.parse_qs(url.query)
  session_id=query.get("session_id",["sess_planning"])[0]
  mode=query.get("mode",["normal"])[0]
  with LOCK:
   if session_id not in SESSIONS:
    self.reply(404,{"error":{"code":"resource_not_found","message":"session not found"}});return
   if url.path=="/mock/run":
    session=SESSIONS[session_id]
    if session["active_run_id"]:
     self.reply(409,{"error":{"code":"session_busy","details":{"active_run_id":session["active_run_id"]}}});return
    if mode not in ("normal","unsupported","race","lost_ack","unknown","lost_terminal")+APPROVAL_MODES:
     self.reply(400,{"error":{"code":"invalid_request","message":"unsupported mock mode"}});return
    self.reply(202,{"run_id":start_web_run(session,mode),"session_id":session_id});return
   if session_id in STREAMING:
    self.reply(409,{"error":{"code":"session_busy","message":"stream is active"}});return
   STREAMING.add(session_id)
  threading.Thread(target=stream_example,args=(session_id,),daemon=True).start()
  self.reply(202,{"session_id":session_id,"status":"streaming"})
 def websocket(self):
  global CONNECTIONS
  self.close_connection=True
  key=self.headers.get("Sec-WebSocket-Key")
  if not key:self.send_error(400);return
  accept=base64.b64encode(hashlib.sha1((key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
  self.send_response(101);self.send_header("Upgrade","websocket");self.send_header("Connection","Upgrade");self.send_header("Sec-WebSocket-Accept",accept);self.end_headers()
  with LOCK:
   CONNECTIONS+=1
   cid="mock_conn_%d"%CONNECTIONS
  self.ws_id=cid
  self.ws_token="sync_"+cid
  self.ws_ready=False
  self.ws_seq=0
  self.ws_write=threading.Lock()
  self.ws_backlog=[]
  self.ws_snapshot=[]
  self.audio=None
  print("WS connected: device=%s connection=%s"%(self.headers.get("X-Device-ID","?"),cid),flush=True)
  try:
   while True:
    fin,op,payload=self.read_frame()
    if op==8:self.frame(8,payload[:125]);break
    if op==9:self.frame(10,payload[:125]);continue
    if op==2:
     if not self.audio_block(payload):break
     continue
    if op!=1 or not fin:continue
    try:m=json.loads(payload.decode())
    except (ValueError,UnicodeDecodeError):continue
    if not isinstance(m,dict):continue
    typ,cmd=m.get("type"),m.get("command_id","")
    if typ=="hello":
     with LOCK:
      self.ws_snapshot=[{"kind":"run","data":dict(run)} for run in RUNS.values()
                        if run["status"] in ACTIVE_RUN_STATES]
      self.ws_snapshot.extend({"kind":"approval","data":dict(approval)} for approval in APPROVALS.values()
                              if approval["status"]=="pending")
      PEERS.add(self)
      self.text({"v":1,"type":"hello","command_id":cmd,"data":{"connection_id":cid,"sync_token":self.ws_token,"heartbeat_interval_ms":20000,"heartbeat_timeout_ms":60000}})
     print("WS hello accepted; syncing",flush=True)
    elif typ=="sync.complete":
     with LOCK:
      self.text({"v":1,"type":"ack","command_id":cmd,"ok":True,"data":{"status":"syncing"}})
      for event in self.ws_backlog:self.event(*event)
      self.ws_backlog.clear()
      self.text({"v":1,"type":"sync.ready","data":{}})
      self.ws_ready=True
     print("WS sync.ready",flush=True)
    elif typ in ("audio.begin","audio.end"):
     if not self.audio_command(m):break
    elif typ=="run.cancel":
     self.cancel_command(m)
    elif typ=="approval.respond":
     self.approval_command(m)
  except (ConnectionError,OSError,ValueError):pass
  finally:
   with LOCK:PEERS.discard(self)
  print("WS disconnected: "+cid,flush=True)
 def audio_ack(self, command, data=None, error=None):
  reply={"v":1,"type":"ack","command_id":command,"ok":error is None}
  if error is None:
   reply["data"]=data
  else:
   reply["error"]={"code":error,"message":error,"retryable":error=="session_busy","details":{}}
  self.text(reply)

 def audio_command(self, msg):
  command=msg.get("command_id")
  sid=msg.get("session_id")
  data=msg.get("data")
  if not self.ws_ready:
   self.audio_ack(command,error="not_ready")
   return True
  if not isinstance(data,dict) or not isinstance(sid,str):
   self.audio_ack(command,error="invalid_request")
   return True
  utterance=data.get("utterance_id")
  if not isinstance(utterance,str) or not 1<=len(utterance)<=128:
   self.audio_ack(command,error="invalid_request")
   return True
  key=(self.headers.get("X-Device-ID",""),utterance)
  with LOCK:
   session=SESSIONS.get(sid)
   if not session:
    self.audio_ack(command,error="resource_not_found")
    return True
   existing=ACCEPTED_AUDIO.get(key)
   if msg["type"]=="audio.begin":
    return self.audio_begin(command,session,key,data,existing)
   return self.audio_end(command,session,key,data,existing)

 def audio_begin(self, command, session, key, data, existing):
  length=data.get("total_bytes")
  version=data.get("expected_settings_version")
  checksum=data.get("sha256")
  duration=data.get("duration_ms")
  if (type(length) is not int or not 6400<=length<=960000 or length%2 or
      type(version) is not int or version<=0 or type(duration) is not int or
      duration!=length//32 or data.get("format")!="pcm_s16le" or
      data.get("sample_rate")!=16000 or data.get("channels")!=1 or
      not isinstance(checksum,str) or len(checksum)!=64 or
      any(ch not in "0123456789abcdef" for ch in checksum)):
   self.audio_ack(command,error="invalid_audio")
   return True
  signature=(session["id"],version,length,checksum)
  if existing:
   if existing["signature"]!=signature:
    self.audio_ack(command,error="utterance_conflict")
   else:
    self.audio_ack(command,existing["ack"])
   return True
  if self.audio:
   self.audio_ack(command,error="upload_busy")
  elif session["active_run_id"]:
   self.audio_ack(command,error="session_busy")
  elif session["settings_version"]!=version:
   self.audio_ack(command,error="settings_conflict")
  else:
   self.audio={"key":key,"signature":signature,"received":0,"hash":hashlib.sha256(),
               "has_voice":False,"upload_id":"upload_"+uuid4().hex}
   self.connection.settimeout(15)
   if not drop_audio("begin_ack"):
    self.audio_ack(command,{"status":"upload_required","upload_id":self.audio["upload_id"],
                            "max_chunk_bytes":4096})
  return True

 def audio_block(self, payload):
  audio=self.audio
  if (not audio or not 0<len(payload)<=4096 or len(payload)%2 or
      audio["received"]+len(payload)>audio["signature"][2]):
   self.frame(8,struct.pack("!H",1002))
   return False
  audio["received"]+=len(payload)
  audio["hash"].update(payload)
  audio["has_voice"]|=any(payload)
  with LOCK:
   return not drop_audio("body")

 def audio_end(self, command, session, key, data, existing):
  audio=self.audio
  self.audio=None
  self.connection.settimeout(None)
  if existing:
   if existing["signature"][0]!=session["id"]:
    self.audio_ack(command,error="utterance_conflict")
   else:
    self.audio_ack(command,existing["ack"])
   return True
  if (not audio or audio["key"]!=key or audio["signature"][0]!=session["id"] or
      data.get("upload_id")!=audio["upload_id"] or audio["received"]!=audio["signature"][2] or
      audio["hash"].hexdigest()!=audio["signature"][3]):
   self.audio_ack(command,error="invalid_audio")
   return True
  if session["active_run_id"] or drop_audio("busy"):
   self.audio_ack(command,error="session_busy")
   return True
  if session["settings_version"]!=audio["signature"][1]:
   self.audio_ack(command,error="settings_conflict")
   return True

  sid=session["id"]
  run_id="run_"+uuid4().hex
  ack={"status":"accepted","utterance_id":key[1],"run_id":run_id,
       "settings_version":session["settings_version"],"model_id":session["model_id"],
       "authorization_level_id":session["authorization_level_id"]}
  run={**ack,"session_id":sid,"source":"device","status":"queued","state_version":1,
       "pending_approval_id":None,"final_message_id":None,"last_progress":None,"error":None}
  ACCEPTED_AUDIO[key]={"signature":audio["signature"],"ack":ack}
  RUNS[run_id]=run
  session["active_run_id"]=run_id
  session["session_version"]+=1
  delivered=True
  try:
   if drop_audio("wrong_ack"):
    self.audio_ack(command,{**ack,"utterance_id":"unexpected_utterance"})
   elif not drop_audio("end_ack"):
    self.audio_ack(command,ack)
  except (ConnectionError,OSError):
   delivered=False
  publish("run.accepted",sid,dict(run),run_id)
  publish("session.updated",sid,view(session))
  threading.Thread(target=finish_mock_audio,args=(run_id,audio["has_voice"]),daemon=True).start()
  return delivered

 def cancel_command(self, message):
  command=message.get("command_id")
  sid=message.get("session_id")
  run_id=message.get("run_id")
  if not all(isinstance(value,str) and value for value in (command,sid,run_id)):
   self.audio_ack(command,error="invalid_request")
   return
  key=(self.headers.get("X-Device-ID",""),command)
  with LOCK:
   if not self.ws_ready:
    self.audio_ack(command,error="not_ready")
    return
   previous=CANCEL_RESULTS.get(key)
   if previous:
    old_sid,old_run,error=previous
    if (old_sid,old_run)!=(sid,run_id):error="command_conflict"
    self.audio_ack(command,{"run_id":run_id,"status":"cancelling"},error)
    return
   run=RUNS.get(run_id)
   mode=WEB_RUN_OPTIONS.get(run_id,"normal")
   if mode=="unknown":
    return
   error=None
   if not run or run["session_id"]!=sid or run["status"] not in ACTIVE_RUN_STATES:
    error="resource_not_found"
   elif mode=="unsupported":
    error="cancel_not_supported"
   elif mode=="race":
    complete_web_run(run)
    error="resource_not_found"
   if len(CANCEL_RESULTS)>=256:CANCEL_RESULTS.pop(next(iter(CANCEL_RESULTS)))
   CANCEL_RESULTS[key]=(sid,run_id,error)
   if error:
    self.audio_ack(command,error=error)
    return
   if run["status"]!="cancelling":
    pending=APPROVALS.get(run["pending_approval_id"])
    if pending:
     resolve_approval(pending,"cancelled","server")
    set_run_state(run,"cancelling","正在停止后续执行")
    threading.Thread(target=finish_cancellation,args=(run_id,),daemon=True).start()
   if mode!="lost_ack":
    self.audio_ack(command,{"run_id":run_id,"status":"cancelling"})

 def approval_command(self, message):
  command=message.get("command_id")
  sid=message.get("session_id")
  run_id=message.get("run_id")
  data=message.get("data")
  if (not isinstance(data,dict) or
      not all(isinstance(value,str) and value for value in (command,sid,run_id))):
   self.audio_ack(command,error="invalid_request")
   return
  approval_id=data.get("approval_id")
  version=data.get("expected_approval_version")
  decision=data.get("decision")
  if not isinstance(approval_id,str) or type(version) is not int or decision not in ("approve_once","reject"):
   self.audio_ack(command,error="invalid_request")
   return
  key=(self.headers.get("X-Device-ID",""),command)
  signature=(sid,run_id,approval_id,version,decision)
  with LOCK:
   if not self.ws_ready:
    self.audio_ack(command,error="not_ready")
    return
   if key in APPROVAL_RESULTS:
    old,reply=APPROVAL_RESULTS[key]
    if old==signature:self.text(reply)
    else:self.audio_ack(command,error="command_conflict")
    return
   approval=APPROVALS.get(approval_id)
   mode=WEB_RUN_OPTIONS.get(run_id)
   error=None
   actual=None
   if not approval or (approval["session_id"],approval["run_id"])!=(sid,run_id):
    error="resource_not_found"
   else:
    if approval["status"]=="pending" and mode=="approval_web_wins":
     apply_decision(approval,"reject","web")
    if approval["status"]=="pending" and mode=="approval_version" and approval["approval_version"]==1:
     approval["approval_version"]=2
    if approval["status"]=="expired":
     error="approval_expired"
    elif approval["status"]!="pending":
     error="approval_already_resolved"
    elif approval["approval_version"]!=version:
     error="approval_version_conflict"
    elif decision=="approve_once" and not approval["device_actionable"]:
     error="approval_requires_web"
    elif not apply_decision(approval,decision,"device"):
     error="approval_already_resolved"
    actual=dict(approval)
   reply={"v":1,"type":"ack","command_id":command,"ok":error is None}
   if error:
    reply["error"]={"code":error,"message":error,"retryable":False,
                    "details":{"current_approval":actual} if actual else {}}
   else:
    reply["data"]={key:actual[key] for key in ("approval_id","status","approval_version")}
   if len(APPROVAL_RESULTS)>=256:APPROVAL_RESULTS.pop(next(iter(APPROVAL_RESULTS)))
   APPROVAL_RESULTS[key]=(signature,reply)
   if mode!="approval_lost_ack":self.text(reply)

 def event(self, event_type, session_id, data, run_id):
  self.ws_seq+=1
  self.text({"v":1,"type":event_type,"connection_id":self.ws_id,"seq":str(self.ws_seq),
             "session_id":session_id,"run_id":run_id,"data":data})

 def read_frame(self):
  h=self.rfile.read(2)
  if len(h)!=2:raise ConnectionError()
  b1,b2=h;fin=bool(b1&128);op=b1&15;n=b2&127
  if b1&112:raise ValueError()
  if n==126:
   x=self.rfile.read(2)
   if len(x)!=2:raise ConnectionError()
   n=struct.unpack("!H",x)[0]
  elif n==127:
   x=self.rfile.read(8)
   if len(x)!=8:raise ConnectionError()
   n=struct.unpack("!Q",x)[0]
  if n>8192 or (op>=8 and (not fin or n>125)):raise ValueError()
  mask=self.rfile.read(4) if b2&128 else b""
  if len(mask)!= (4 if b2&128 else 0):raise ConnectionError()
  data=self.rfile.read(n)
  if len(data)!=n:raise ConnectionError()
  if mask:data=bytes(x^mask[i%4] for i,x in enumerate(data))
  return fin,op,data
 def frame(self,op,data):
  n=len(data);h=bytes([128|op])+(bytes([n]) if n<126 else b"\x7e"+struct.pack("!H",n) if n<=65535 else b"\x7f"+struct.pack("!Q",n))
  with self.ws_write:
   self.wfile.write(h+data);self.wfile.flush()
 def text(self,obj):self.frame(1,json.dumps(obj,ensure_ascii=False,separators=(",",":")).encode())

class Server(ThreadingHTTPServer):
 daemon_threads=True
 allow_reuse_address=True

def addresses():
 out=set()
 try:
  for row in socket.getaddrinfo(socket.gethostname(),None,socket.AF_INET,socket.SOCK_STREAM):
   ip=row[4][0]
   if not ip.startswith("127."):out.add(ip)
 except OSError:pass
 try:
  s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.connect(("192.0.2.1",80));ip=s.getsockname()[0];s.close()
  if not ip.startswith("127."):out.add(ip)
 except OSError:pass
 return sorted(out)

if __name__=="__main__":
 srv=Server(("0.0.0.0",PORT),Handler)
 print("Metalio E-Ink4 模拟后端已启动，端口 %d"%PORT)
 print("本机检查: http://127.0.0.1:%d/health"%PORT)
 for ip in addresses():print("设备配置: http://%s:%d"%(ip,PORT))
 print("需电脑和设备处于同一局域网；首次启动允许 Windows 专用网络防火墙。按 Ctrl+C 停止。",flush=True)
 try:srv.serve_forever()
 except KeyboardInterrupt:print("\n模拟后端已停止")
 finally:srv.server_close()
