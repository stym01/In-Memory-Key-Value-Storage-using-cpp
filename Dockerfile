# Use a lightweight Linux image with C++ installed
FROM ubuntu:latest

# Install necessary tools (g++ and make)
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory inside the container
WORKDIR /app

# Copy all your project files into the container
COPY . .

# Build the project using your Makefile
RUN make

# Expose port 8080 (so we can connect from outside)
EXPOSE 8080

# Command to run the server when container starts
CMD ["./nitredis_server"]