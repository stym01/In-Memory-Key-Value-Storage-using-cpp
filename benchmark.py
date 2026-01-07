import socket
import time

HOST = '127.0.0.1'
PORT = 8080
REQUESTS = 10000  # no of req send by us

def benchmark():
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect((HOST, PORT))
    
    print(f"Starting benchmark for {REQUESTS} requests...")
    start_time = time.time()
    
    for i in range(0,REQUESTS):
        msg = f"SET key{i} value{i} {i}" 
        client.sendall(msg.encode())
        response = client.recv(1024) 
        
    end_time = time.time()
    duration = end_time - start_time
    rps = REQUESTS / duration
    
    print(f"Finished in {duration:.4f} seconds.")
    print(f"Speed: {rps:.2f} Requests Per Second (RPS)")
    
    client.close()

if __name__ == "__main__":
    benchmark()