import socket
import time

REQUESTS = 10000  # Number of requests
PAYLOAD = "SET benchmark_key 12345\n" # The command to send

def run_test(port, name):
    print(f"--- Benchmarking {name} on port {port} ---")
    try:
        # Create a socket connection
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.connect(('127.0.0.1', port))
        
        start_time = time.time()
        
        for i in range(REQUESTS):
            client.sendall(PAYLOAD.encode())
            # We read up to 1024 bytes (waiting for "OK" or "+OK")
            response = client.recv(1024)
            
        end_time = time.time()
        duration = end_time - start_time
        rps = REQUESTS / duration
        
        print(f"Finished {REQUESTS} requests in {duration:.4f} seconds.")
        print(f"Result: {rps:.2f} Requests Per Second (RPS)")
        client.close()
        return rps
        
    except ConnectionRefusedError:
        print(f"ERROR: Could not connect to {name}. Is it running?")
        return 0

if __name__ == "__main__":
    # 1. Test Your Server (NitRedis)
    nit_rps = run_test(8080, "NitRedis (Your C++ Project)")
    print("\n")
    
    # 2. Test Real Redis (Industry Standard)
    redis_rps = run_test(6379, "Official Redis")
    
    print("\n------------------------------------------------")
    if redis_rps > 0:
        ratio = (nit_rps / redis_rps) * 100
        print(f"SUMMARY: Your engine is performing at {ratio:.2f}% of Redis speed.")
    print("------------------------------------------------")