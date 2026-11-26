#!/usr/bin/env python3
"""Local-only protocol fixture, not a Harness implementation."""
import base64, hashlib, http.server, json, socket, struct, subprocess, sys, threading
from urllib.parse import urlsplit, parse_qs
class State:
    def __init__(self):
        self.connections=0;self.pages=0;self.pings=0;self.ids=set();self.errors=[];self.lock=threading.Lock()
state=State()
def encoded(value):return json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()
def frame(opcode,payload,fin=True):
    if isinstance(payload,str):payload=payload.encode()
    head=bytes([(128 if fin else 0)|opcode])
    n=len(payload)
    return head+(bytes([n]) if n<126 else b'\x7e'+struct.pack('!H',n))+payload
def exact(sock,n):
    data=b''
    while len(data)<n:
        piece=sock.recv(n-len(data))
        if not piece:raise EOFError()
        data+=piece
    return data
def recv_frame(sock):
    a,b=exact(sock,2);n=b&127
    if n==126:n=struct.unpack('!H',exact(sock,2))[0]
    elif n==127:n=struct.unpack('!Q',exact(sock,8))[0]
    if n>8192:raise ValueError('client frame too large')
    mask=exact(sock,4) if b&128 else None
    if mask is None:raise ValueError('client frame unmasked')
    data=exact(sock,n)
    return a&15,bytes(c^mask[i%4] for i,c in enumerate(data))
def run_data():
    return dict(run_id='run_1',session_id='sess_1',utterance_id=None,source='web',status='waiting_approval',
                state_version=4,model_id='deepseek-chat',authorization_level_id='request_approval',
                settings_version=1,pending_approval_id='approval_1',final_message_id=None,last_progress='等待批准',error=None)
def approval():
    return dict(approval_id='approval_1',session_id='sess_1',run_id='run_1',status='pending',approval_version=1,
                title='允许构建项目吗？',reason='验证项目变更',tool_name='shell',
                operation=dict(command='idf.py build',cwd='/workspace/eink4',targets=[]),device_actionable=True,
                expires_at='2026-11-24T12:00:00Z',decision=None,actor_source=None)
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_GET(self):
        try:
            if self.headers.get('X-Device-ID')!='metalio_test' or self.headers.get('Authorization'):
                raise ValueError('incorrect device/auth headers')
            if self.path=='/api/v1/ws':return self.websocket()
            u=urlsplit(self.path);q=parse_qs(u.query)
            if u.path!='/api/v1/sync':return self.reply(404,{})
            token=q.get('sync_token',[''])[0]
            if not token.startswith('token_conn_') or not token.endswith('+/='):raise ValueError('token not URL encoded')
            cursor=q.get('cursor',[None])[0]
            if cursor not in (None,'page+/='):raise ValueError('bad cursor')
            with state.lock:state.pages+=1
            return self.reply(200,dict(items=[dict(kind='run',data=run_data())] if cursor is None else [dict(kind='approval',data=approval())],
                                       next_cursor='page+/=' if cursor is None else None))
        except (BrokenPipeError,ConnectionResetError):pass
        except Exception as e:
            with state.lock:state.errors.append(str(e))
            try:self.reply(500,dict(error=str(e)))
            except OSError:pass
    def reply(self,status,body):
        payload=encoded(body);self.send_response(status);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(payload)));self.send_header('Connection','close');self.end_headers()
        self.wfile.write(payload);self.wfile.flush();self.close_connection=True
    def websocket(self):
        with state.lock:
            state.connections+=1;number=state.connections
        connection='conn_'+str(number);token='token_'+connection+'+/='
        key=self.headers.get('Sec-WebSocket-Key','')
        accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade')
        self.send_header('Sec-WebSocket-Accept',accept);self.end_headers();self.wfile.flush()
        sock=self.connection;sock.settimeout(5)
        seq=0
        def send_json(value,fragment=False):
            payload=encoded(value)
            if fragment:
                # Split inside a UTF-8 character where possible.
                cut=next((i+1 for i,b in enumerate(payload) if b>=0xe0),len(payload)//2)
                sock.sendall(frame(1,payload[:cut],False)+frame(0,payload[cut:]))
            else:sock.sendall(frame(1,payload))
        try:
            while True:
                op,payload=recv_frame(sock)
                if op==8:break
                if op==9:
                    with state.lock:state.pings+=1
                    sock.sendall(frame(10,payload))
                    if number==1:
                        sock.shutdown(socket.SHUT_RDWR);break
                    continue
                if op!=1:raise ValueError('unexpected binary application command')
                msg=json.loads(payload);kind=msg['type'];cid=msg['command_id']
                with state.lock:
                    if cid in state.ids:raise ValueError('reused command id across connections')
                    state.ids.add(cid)
                if kind=='hello':
                    send_json(dict(v=1,type='hello',command_id=cid,data=dict(connection_id=connection,sync_token=token,
                              heartbeat_interval_ms=20000,heartbeat_timeout_ms=60000)),True)
                elif kind=='sync.complete':
                    if msg['data']['sync_token']!=token:raise ValueError('wrong sync token')
                    send_json(dict(v=1,type='ack',command_id=cid,ok=True,data=dict(status='syncing')))
                    event=dict(v=1,type='run.progress',connection_id=connection,seq='1',session_id='sess_1',run_id='run_1',
                               data=dict(state_version=4,text='等待批准'))
                    send_json(event,True);send_json(event)  # Duplicate must not consume another seq.
                    send_json(dict(v=1,type='sync.ready',data={}))
                    send_json(dict(v=1,type='message.delta',connection_id=connection,seq='2',session_id='sess_1',run_id='run_1',
                                   data=dict(message_id='msg_1',offset_bytes=0,text='正在检查项目🙂',created_at='2026-11-24T03:20:00Z')),True)
                else:raise ValueError('unexpected command '+kind)
        except (EOFError,ConnectionResetError,BrokenPipeError,socket.timeout):pass
        finally:self.close_connection=True
def run_mock_backend(executable, mode, scenario=None):
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mock_backend"))
    import mock_backend
    mock_backend.DROP_AUDIO_ONCE = "busy" if scenario == "busy_retry" else scenario or ""
    if mode == "--approval":
        mock_backend.APPROVAL_EXPIRY_SECONDS = 0.9
    if mode == "--run":
        mock_backend.WEB_STEP_SECONDS = 0.45
    with mock_backend.Server(("127.0.0.1", 0), mock_backend.Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            origin = "http://127.0.0.1:" + str(server.server_port)
            command = [executable, origin, mode]
            if scenario:
                command.append(scenario)
            result = subprocess.run(command, timeout=18)
            if mode == "--audio" and result.returncode == 0:
                expected = 0 if scenario in ("busy", "conflict", "bad_digest", "short", "odd") else 1
                assert len(mock_backend.ACCEPTED_AUDIO) == len(mock_backend.RUNS) == expected
            if mode == "--approval" and result.returncode == 0:
                allowed = scenario in ("approve", "long", "lost_ack", "version", "multi")
                with mock_backend.LOCK:
                    assert len(mock_backend.APPROVAL_EXECUTIONS) == int(allowed)
                    assert len(mock_backend.APPROVALS) == (2 if scenario == "multi" else 1)
            return result.returncode
        finally:
            server.shutdown()
            thread.join()

def main():
    if sys.argv[2:] in (["--catalog"], ["--standby"]):
        return run_mock_backend(sys.argv[1], sys.argv[2])
    if len(sys.argv) == 4 and sys.argv[2] in ("--audio", "--run", "--approval"):
        return run_mock_backend(sys.argv[1], sys.argv[2], sys.argv[3])
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        result=subprocess.run([sys.argv[1],'http://127.0.0.1:'+str(server.server_port)],timeout=18)
        if result.returncode:return result.returncode
        with state.lock:
            assert not state.errors,state.errors
            assert state.connections>=2 and state.pages>=4 and state.pings>=1,(state.connections,state.pages,state.pings)
            print('PASS mock service verified no-auth device headers, cursor encoding, unique command ids and reconnect')
        return 0
    finally:server.shutdown();server.server_close()
if __name__=='__main__':sys.exit(main())
