import socket, time, threading, random

def attack_hotspot():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', 6379))
        s.sendall(b'*2\r\n\r\nAUTH\r\n\r\nMySuperSecret\r\n')
        s.recv(1024)
        
        while True:
            # 99% of traffic hits a single key to trigger the ML anomaly detection
            key = 'THUNDERING_HERD_KEY' if random.random() < 0.99 else f'random_{random.randint(1,100)}'
            cmd = f'*3\r\n\r\nSET\r\n\r\n{key}\r\n\r\nVALUE\r\n'
            s.sendall(cmd.encode())
            s.recv(1024)
    except:
        pass

for _ in range(50):
    threading.Thread(target=attack_hotspot, daemon=True).start()

print('Hotspot Attack Running... (Check Grafana!)')
print('Press Ctrl+C to stop the attack.')
while True:
    time.sleep(1)
