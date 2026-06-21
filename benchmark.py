import socket
import threading
import time
import random
import argparse
import string
def generate_random_string(length=10):
    letters = string.ascii_lowercase
    return ''.join(random.choice(letters) for i in range(length))
def worker(host, port, num_requests, results, worker_id):
    success_count = 0
    failure_count = 0
    
    # Pre-generate commands to avoid python generation overhead during the benchmark
    commands = []
    for _ in range(num_requests):
        key = f"key_{worker_id}_{random.randint(1, 1000)}"
        if random.random() > 0.5:
            # 50% chance of SET
            val = generate_random_string(16)
            commands.append(f"SET {key} {val}\n".encode())
        else:
            # 50% chance of GET
            commands.append(f"GET {key}\n".encode())
    # Create a single persistent connection for this thread
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        
        for cmd in commands:
            sock.sendall(cmd)
            # Wait for the server's reply
            data = sock.recv(1024)
            if data:
                success_count += 1
            else:
                failure_count += 1
                
        sock.close()
    except Exception as e:
        print(f"Worker {worker_id} failed: {e}")
        failure_count = num_requests
        
    results[worker_id] = (success_count, failure_count)
def main():
    parser = argparse.ArgumentParser(description="Benchmark the NitKVStore Cluster")
    parser.add_argument("--host", default="127.0.0.1", help="Target host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8081, help="Target port (default: 8081)")
    parser.add_argument("--threads", type=int, default=10, help="Number of concurrent clients (default: 10)")
    parser.add_argument("--requests", type=int, default=1000, help="Number of requests per client (default: 1000)")
    args = parser.parse_args()
    total_requests = args.threads * args.requests
    print(f"==========================================")
    print(f" NitKVStore Benchmark Tool")
    print(f"==========================================")
    print(f" Target Node : {args.host}:{args.port}")
    print(f" Concurrency : {args.threads} threads")
    print(f" Requests    : {args.requests} per thread")
    print(f" Total Load  : {total_requests} operations (50% GET / 50% SET)")
    print(f"------------------------------------------")
    print("Running... Please wait.\n")
    results = [None] * args.threads
    threads = []
    
    start_time = time.time()
    
    # Spawn threads
    for i in range(args.threads):
        t = threading.Thread(target=worker, args=(args.host, args.port, args.requests, results, i))
        threads.append(t)
        t.start()
        
    # Wait for all threads to finish
    for t in threads:
        t.join()
        
    end_time = time.time()
    duration = end_time - start_time
    
    total_success = sum(r[0] for r in results if r)
    total_failures = sum(r[1] for r in results if r)
    
    ops = total_success / duration if duration > 0 else 0
    
    print(f"==========================================")
    print(f" Benchmark Results")
    print(f"==========================================")
    print(f" Time taken    : {duration:.2f} seconds")
    print(f" Total Success : {total_success}")
    print(f" Total Failed  : {total_failures}")
    print(f" Throughput    : {ops:.2f} Requests/Second (OPS)")
    print(f"==========================================\n")
if __name__ == "__main__":
    main()
