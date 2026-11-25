#!/usr/bin/env python3
"""Dependency-free HTTP and WebSocket mock for the Metalio E-Ink4."""
import base64, hashlib, json, os, socket, struct, threading, urllib.parse
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT=int(os.environ.get("HARNESS_MOCK_PORT","8080"))
API="/api/v1"
LOCK=threading.RLock()
CONNECTIONS=0
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
def view(s):
 d={k:v for k,v in s.items() if k!="messages"}
 d["model_name"]=next(x["name"] for x in MODELS if x["id"]==s["model_id"])
 d["authorization_level_name"]=next(x["name"] for x in LEVELS if x["id"]==s["authorization_level_id"])
 return d

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
 def do_GET(self):
  global CONNECTIONS
  u=urllib.parse.urlsplit(self.path);p=u.path;q=urllib.parse.parse_qs(u.query)
  if self.headers.get("Upgrade","").lower()=="websocket" and p==API+"/ws":self.websocket();return
  if p=="/health":self.reply(200,{"status":"ok","service":"Metalio E-Ink4 mock backend"});return
  if p==API+"/dashboard/overview":
   r=q.get("range",["all"])[0]
   values={"all":(53,5980,1900000000,22),"30d":(18,742,284100,16),"7d":(5,96,38100,5)}
   if r not in values:self.reply(400,{"error":{"code":"invalid_request","message":"range must be all, 30d or 7d"}});return
   a,b,c,d=values[r];self.reply(200,{"range":r,"timezone":"Asia/Shanghai","sessions":a,"messages":b,"total_tokens":c,"active_days":d,"peak_hour":"14:00","favorite_model":{"id":MODELS[1]["id"],"name":MODELS[1]["name"]},"unavailable_fields":[]});return
  if p==API+"/models":self.reply(200,{"default_model_id":MODELS[1]["id"],"items":MODELS,"next_cursor":None});return
  if p==API+"/authorization-levels":self.reply(200,{"default_authorization_level_id":LEVELS[0]["id"],"items":LEVELS,"next_cursor":None});return
  if p==API+"/sessions":
   rows=[view(x) for x in sorted(SESSIONS.values(),key=lambda x:x["updated_at"],reverse=True) if x["kind"]=="work"]
   try:n=max(1,min(20,int(q.get("limit",["10"])[0])))
   except ValueError:n=10
   self.reply(200,{"items":rows[:n],"next_cursor":None});return
  if p==API+"/sync":
   if not q.get("sync_token"):self.reply(400,{"error":{"code":"invalid_request","message":"sync_token required"}})
   else:self.reply(200,{"items":[],"next_cursor":None})
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
     rows=s["messages"][-n:];base=len(s["messages"])-len(rows)
     items=[{"id":"%s_msg_%02d"%(sid,base+i+1),"role":role,"content":text,"offset_bytes":0,"ends_message":True,"created_at":NOW} for i,(role,text) in enumerate(rows)]
     self.reply(200,{"items":items,"before_cursor":None});return
  self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}})
 def do_PUT(self):
  prefix=API+"/devices/";suffix="/default-casual-session"
  if self.path.startswith(prefix) and self.path.endswith(suffix):
   self.reply(200,{"session":view(SESSIONS["sess_casual_01"])})
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
   self.reply(200,view(s))
 def do_POST(self):self.reply(404,{"error":{"code":"resource_not_found","message":"unknown endpoint"}})
 def websocket(self):
  global CONNECTIONS
  self.close_connection=True
  key=self.headers.get("Sec-WebSocket-Key")
  if not key:self.send_error(400);return
  accept=base64.b64encode(hashlib.sha1((key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
  self.send_response(101);self.send_header("Upgrade","websocket");self.send_header("Connection","Upgrade");self.send_header("Sec-WebSocket-Accept",accept);self.end_headers()
  CONNECTIONS+=1;cid="mock_conn_%d"%CONNECTIONS
  print("WS connected: device=%s connection=%s"%(self.headers.get("X-Device-ID","?"),cid),flush=True)
  try:
   while True:
    fin,op,payload=self.read_frame()
    if op==8:self.frame(8,payload[:125]);break
    if op==9:self.frame(10,payload[:125]);continue
    if op==2:self.frame(8,struct.pack("!H",1003));break
    if op!=1 or not fin:continue
    try:m=json.loads(payload.decode())
    except (ValueError,UnicodeDecodeError):continue
    typ,cmd=m.get("type"),m.get("command_id","")
    if typ=="hello":
     self.text({"v":1,"type":"hello","command_id":cmd,"data":{"connection_id":cid,"sync_token":"mock_sync_%d"%CONNECTIONS,"heartbeat_interval_ms":20000,"heartbeat_timeout_ms":60000}})
     print("WS hello accepted; syncing",flush=True)
    elif typ=="sync.complete":
     self.text({"v":1,"type":"ack","command_id":cmd,"ok":True,"data":{"status":"syncing"}})
     self.text({"v":1,"type":"sync.ready","data":{}})
     print("WS sync.ready",flush=True)
  except (ConnectionError,OSError,ValueError):pass
  print("WS disconnected: "+cid,flush=True)
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
