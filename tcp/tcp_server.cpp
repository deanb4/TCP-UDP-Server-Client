/**
 * TCP Server
 *
 * Waits for a TCP client to connect, receives an image from it, saves the
 * image to disk as received.jpeg, and sends a confirmation message back.
 *
 * Usage: ./tcp_server [port] [backlog]      (defaults: 3490 10)
 *
 * How the transfer works:
 *   - The client first sends the image size as a 4-byte number in network
 *     byte order (big-endian), then the image bytes themselves.
 *   - TCP is a byte stream, so one recv() can return only part of the data.
 *     recvAll() keeps calling recv() until exactly the expected number of
 *     bytes has arrived. TCP itself takes care of lost or out-of-order packets.
 *   - Once the file is saved, the server sends a text confirmation (or an
 *     error message if saving failed), closes the connection and exits.
 */
#include <sys/types.h>   // basic system types (ssize_t, socklen_t)
#include <netinet/in.h>  // internet address structures (sockaddr_in, sockaddr_in6)
#include <sys/socket.h>  // socket(), bind(), listen(), accept(), send(), recv()
#include <netdb.h>       // getaddrinfo(), freeaddrinfo(), gai_strerror()
#include <arpa/inet.h>   // ntohl(): network to host byte order
#include <cerrno>        // errno: the reason the last system call failed
#include <stdexcept>     // exceptions std::stoi throws on a bad backlog argument
#include <iostream>      // std::cout, std::cerr
#include <cstring>       // memset(), strlen(), strerror()
#include <unistd.h>      // close()
#include <vector>        // std::vector, holds the received image
#include <fstream>       // std::ofstream, writes the image to disk
#include <cstdint>       // uint32_t, the fixed-size 4-byte length header

#define MYPORT "3490" // default port to listen on (the client must use the same)
#define BACKLOG 10 // how many requests can pile up in queue

// Reads exactly len bytes from the socket into data.
// Returns false if the connection closes or fails before they all arrive.
bool recvAll(int, void*, size_t);

int main(int argc, char** argv) {
    // ---------- Command-line arguments: [port] [backlog] ----------
    const char* port = nullptr;
    int backlog = 0;
    argc > 1 ? port = argv[1] : port = MYPORT;
    argc > 2 ? backlog = std::stoi(argv[2]) : backlog = BACKLOG;

    int sockfd = -1; // file descriptor for listening for connections
    int newfd = -1; // new file descriptor for the tcp connection
    struct addrinfo hints, *server_info; // server info will point to results
    struct sockaddr_storage their_addr; // address of the client that connects
    socklen_t addr_size;
    int opt = 1;

    // ---------- Find an address to listen on ----------
    // AF_UNSPEC + AI_PASSIVE gives the "any" address (IPv4 and/or IPv6),
    // so the server listens on every network interface of this machine.
    memset(&hints, 0, sizeof(hints)); // make sure struct is empty
    hints.ai_family = AF_UNSPEC; // ipv4 or ipv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream
    hints.ai_flags = AI_PASSIVE; // fill in my IP

    int status = getaddrinfo(nullptr, port, &hints, &server_info);

    if (status != 0) {
        std::cerr << "Error getting address info: " << gai_strerror(status) << "\n";
        return 1;
    }

    // ---------- Create the socket and bind it to the port ----------
    // getaddrinfo can return several addresses: try each one until bind succeeds
    struct addrinfo* p = nullptr;
    for (p = server_info; p != nullptr; p=p->ai_next) {
        // create socket (get file descriptor back)
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

        if (sockfd == -1) {
            std::cerr << "socket: " << strerror(errno) << "\n";
            continue;
        }

        // SO_REUSEADDR lets the server restart right away on the same port,
        // even if a connection that was just closed is still lingering
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt,  sizeof(int)) == -1) {
            std::cerr << "setsockopt: " << strerror(errno) << "\n";
            close(sockfd);
            continue;
        }

        // bind socket to port
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            std::cerr << "bind: " << strerror(errno) << "\n";
            close(sockfd);
            continue;
        }
        break; // success
    }
    // free struct once done
    freeaddrinfo(server_info);

    // p is only nullptr if every address failed
    if (p == nullptr) {
        std::cerr << "Failed to bind to port " << port << "\n";
        return 1;
    }

    // ---------- Start listening for connections ----------
    // backlog = how many clients can wait in the queue before being accepted
    if (listen(sockfd, backlog) == -1) {
        std::cerr << "Error listening on port: " << port << " | " << strerror(errno) << "\n";
        close(sockfd);
        return 1;
    }

    // ---------- Accept a client ----------
    // accept() blocks until a client connects, then returns a new socket
    // (newfd) for talking to that client. sockfd stays the listening socket.
    bool accept_connection = true;
    while (accept_connection) {
        addr_size = sizeof(their_addr);
        newfd = accept(sockfd, reinterpret_cast<sockaddr*>(&their_addr), &addr_size);

        if (newfd == -1) {
            std::cerr << "error accepting connection: " << strerror(errno) << "\n";
            continue;
        }

        std::cout <<  "Connected! Listening on port: "<< port << "\n";

        // One image per connection. The loop lets "break" jump straight to the
        // cleanup code below if anything goes wrong along the way.
        while (true) {
            // ---------- Read the 4-byte size header ----------
            uint32_t netLen;
            if (!recvAll(newfd, &netLen, sizeof(netLen))) {
                std::cerr << "Failed to read image length\n";
                break;
            }
            uint32_t len = ntohl(netLen); // Network to host Long
            std::cout << "Expecting: " << len << " bytes\n";

            // ---------- Read exactly that many image bytes ----------
            std::vector<char> imageData(len); // buffer big enough for the whole image
            if (!recvAll(newfd, imageData.data(), len)) {
                std::cerr << "Connection closed before full image arrived\n";
                break;
            }
            std::cout << "Received all " << len << " bytes\n";

            // ---------- Save the image to disk ----------
            // close() is what actually writes the data to disk, so check for errors after it
            std::ofstream out("received.jpeg", std::ios::binary);
            out.write(imageData.data(), len);
            out.close();
            const char* reply;
            if (!out) {
                std::cerr << "Failed to write received.jpeg\n";
                reply = "ERROR: server could not save the image";
            } else {
                std::cout << "Saved to received.jpeg\n";
                reply = "Image Received and saved to server successfully!";
            }

            // ---------- Send the confirmation (or the error) back to the client ----------
            // The client is waiting for a reply either way, so always send one.
            if (send(newfd, reply, strlen(reply), 0) == -1) {
                std::cerr << "Error sending confirmation: " << strerror(errno) << "\n";
            }

            // only one image is sent per connection, so stop here
            break;
        }
        std::cout << "Closing Connection\n";
        close(newfd);
        accept_connection = false; // one transfer only: stop after this client
    }

    std::cout << "TCP Socket listening on Port: " << port << " has been closed.\n";
    close(sockfd);

    return 0;
}


// Receive all bytes until length left is 0. TCP can hand the data over in
// pieces, so keep calling recv() until everything has arrived.
// recv() returns 0 if the other side closed the connection, -1 on an error.
bool recvAll(int fd, void* data, size_t len) {
    char* bytes_data = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = recv(fd, bytes_data, len, 0);
        if (n <= 0) return false; // closed or failed before everything arrived
        bytes_data += n; // move past the bytes just received
        len -= n;        // and count them off
    }
    return true;
}
