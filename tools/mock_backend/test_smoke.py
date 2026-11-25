import base64, json, os, socket, struct, unittest, urllib.request
from mock_backend import PORT

class MockBackendSmokeTest(unittest.TestCase):
    def get(self,path):
        with urllib.request.urlopen("http://127.0.0.1:%d%s"%(PORT,path)) as r:return json.loads(r.read())
    def test_routes(self):
        self.assertEqual(self.get("/health")["status"],"ok")
        self.assertEqual(len(self.get("/api/v1/models")["items"]),2)
        self.assertEqual(len(self.get("/api/v1/authorization-levels")["items"]),2)
        self.assertEqual(len(self.get("/api/v1/sessions")["items"]),2)
        self.assertEqual(self.get("/api/v1/dashboard/overview?range=7d")["sessions"],5)
        self.assertEqual(self.get("/api/v1/sync?sync_token=test")["items"],[])
    def test_settings_and_session_routes(self):
        self.assertEqual(len(self.get("/api/v1/sessions/sess_firmware/messages?limit=10")["items"]),4)
        body=json.dumps({"expected_settings_version":1,"model_id":"deepseek-v4.1-flash"}).encode()
        req=urllib.request.Request("http://127.0.0.1:%d/api/v1/sessions/sess_firmware/settings"%PORT,data=body,headers={"Content-Type":"application/json"},method="PATCH")
        with urllib.request.urlopen(req) as r: updated=json.loads(r.read())
        self.assertEqual(updated["settings_version"],2)
        self.assertEqual(updated["model_id"],"deepseek-v4.1-flash")
        req=urllib.request.Request("http://127.0.0.1:%d/api/v1/devices/demo/default-casual-session"%PORT,method="PUT")
        with urllib.request.urlopen(req) as r:self.assertEqual(json.loads(r.read())["session"]["kind"],"casual")
    def test_websocket_ready(self):
        s=socket.create_connection(("127.0.0.1",PORT),timeout=2)
        key=base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /api/v1/ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nX-Device-ID: test_device\r\n\r\n"%key).encode())
        response=b""
        while b"\r\n\r\n" not in response:response+=s.recv(1)
        self.assertIn(b"101",response)
        def send(obj):
            data=json.dumps(obj,separators=(",",":")).encode();mask=os.urandom(4)
            s.sendall(bytes([0x81,0x80|len(data)])+mask+bytes(v^mask[i%4] for i,v in enumerate(data)))
        def receive():
            h=s.recv(2);n=h[1]&127
            if n==126:n=struct.unpack("!H",s.recv(2))[0]
            out=b""
            while len(out)<n:out+=s.recv(n-len(out))
            return json.loads(out.decode())
        send({"v":1,"type":"hello","command_id":"hello_test","data":{"device_id":"test_device","firmware_version":"test"}})
        self.assertEqual(receive()["type"],"hello")
        send({"v":1,"type":"sync.complete","command_id":"sync_test","data":{"sync_token":"mock_sync"}})
        self.assertEqual(receive()["type"],"ack");self.assertEqual(receive()["type"],"sync.ready")
        s.close()

if __name__=="__main__":unittest.main()
