"""Disposable loopback-only SSH fixture. Runtime-generated keys; no user files."""
import json, os, select, socket, sys, threading, time
import paramiko
root, port, password = sys.argv[1], int(sys.argv[2]), os.environ['NAVIDOG_TEST_SSH_PASSWORD']
host_key = paramiko.RSAKey.generate(2048)
client_key = paramiko.RSAKey.generate(2048)
key_path = os.path.join(root, 'owned-client-key.pem')
client_key.write_private_key_file(key_path)
os.chmod(key_path, 0o600)
listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(8)
print(json.dumps({'port': listener.getsockname()[1], 'hostKey': host_key.get_name()+' '+host_key.get_base64(), 'keyFile': key_path}), flush=True)
class Server(paramiko.ServerInterface):
    def __init__(self): self.destinations = {}
    def get_allowed_auths(self, user): return 'password,publickey'
    def check_auth_password(self, user, candidate):
        return paramiko.AUTH_SUCCESSFUL if user == 'owned' and candidate == password else paramiko.AUTH_FAILED
    def check_auth_publickey(self, user, key):
        return paramiko.AUTH_SUCCESSFUL if user == 'owned' and key == client_key else paramiko.AUTH_FAILED
    def check_channel_direct_tcpip_request(self, channel_id, origin, destination):
        if destination[0] != '127.0.0.1' or destination[1] != port: return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
        self.destinations[channel_id] = destination
        return paramiko.OPEN_SUCCEEDED

def forward(channel, destination):
    target = socket.create_connection(destination)
    try:
        while True:
            readable, _, _ = select.select([channel, target], [], [], 1)
            for source in readable:
                data = source.recv(65536)
                if not data: return
                if source is channel: target.sendall(data)
                else: channel.sendall(data)
    except (OSError, EOFError): pass
    finally: channel.close(); target.close()

def serve(client):
    transport=paramiko.Transport(client); transport.add_server_key(host_key); server=Server()
    try:
        transport.start_server(server=server)
        while transport.is_active():
            channel=transport.accept(1)
            if channel: threading.Thread(target=forward,args=(channel,server.destinations[channel.get_id()]),daemon=True).start()
    except (EOFError, OSError, paramiko.SSHException): pass
    finally: transport.close()
while True:
    client, _ = listener.accept()
    threading.Thread(target=serve,args=(client,),daemon=True).start()
