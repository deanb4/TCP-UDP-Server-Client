/**
 * TCP Client
 *
 * Reads input.jpeg from the current directory, sends it to the TCP server,
 * then waits for the server's confirmation message and prints it.
 *
 * Usage: ./tcp_client [server_ip] [port]      (defaults: 127.0.0.1 3490)
 *
 * How the transfer works:
 *   - The client first sends the image size as a 4-byte number in network
 *     byte order (big-endian), so the server knows how many bytes to expect,
 *     then sends the image bytes themselves.
 *   - TCP delivers the bytes reliably and in order (it resends anything lost),
 *     so unlike the UDP version no packet numbers or resend requests are needed.
 *   - The server replies with a text confirmation once the file is saved
 *     (or an error message if it could not save it), which is printed here.
 */
#include <sys/socket.h>  // socket(), connect(), send(), recv()
#include <netinet/in.h>  // sockaddr_in: an IPv4 address and port
#include <errno.h>       // errno: the reason the last system call failed
#include <iostream>      // std::cout, std::cerr
#include <netdb.h>       // getaddrinfo(), freeaddrinfo(), gai_strerror()
#include <cstring>       // memset(), strerror()
#include <sys/types.h>   // basic system types (ssize_t)
#include <arpa/inet.h>   // htonl()/ntohs() byte order conversion, inet_ntop()
#include <unistd.h>      // close()
#include <fstream>       // std::ifstream, reads the image from disk
#include <vector>        // std::vector, holds the image bytes
#include <cstdint>       // uint32_t, the fixed-size 4-byte length header

#define PORT "3490" // default server port (must match the server)

// Sends all len bytes of data, calling send() as many times as needed.
// Returns false if sending fails.
bool sendAll(int, const void*, size_t);

int main(int argc, char* argv[]) {
    // ---------- Command-line arguments: [server_ip] [port] ----------
    // No IP given -> nullptr, which makes getaddrinfo use this machine (127.0.0.1)
    const char* port = nullptr;
    const char* ip = nullptr;
    int status = -1;
    char s[INET6_ADDRSTRLEN]; // the server's IP address as text, for printing
    argc > 1 ? ip = argv[1]: ip = nullptr;
    argc > 2 ? port = argv[2]: port = PORT;

    // ---------- Look up the server's address ----------
    // AF_INET: IPv4 address. SOCK_STREAM: TCP.
    int sockfd;
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints)); // start from all zeros, not leftover garbage
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(ip, port, &hints, &servinfo);
    if (status != 0) {
        std::cerr << "Error obtaining address of server: " << gai_strerror(status) << "\n";
        return 1;
    }

    // ---------- Create the socket ----------
    sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (sockfd == -1) {
        std::cerr << "Error returning socket file descriptor: " << strerror(errno) << "\n";
        return 1;
    }

    // ---------- Connect to the server ----------
    // Convert the server's IPv4 address from binary to text so it can be printed
    auto* addr4 = reinterpret_cast<sockaddr_in*>(servinfo->ai_addr);
    inet_ntop(AF_INET, &addr4->sin_addr, s, sizeof(s));
    std::cout << "client attempting to connect to: " << s << "\n";

    // connect() does the TCP handshake; it fails if no server is listening
    if (connect(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        std::cerr << "Error connecting to server: " << strerror(errno) << "\n";
        freeaddrinfo(servinfo);
        return 1;
    }
    std::cout << "Client Connected to port: " << ntohs(addr4->sin_port) << "\n";

    // ---------- Read the whole image file into memory ----------
    // Binary mode, so every byte is read exactly as it is on disk
    std::ifstream image("input.jpeg", std::ios::binary);
    if (!image) {
        std::cerr << "Could not open image\n";
        return 1;
    }
    // Find the file size: jump to the end, read the position, jump back
    image.seekg(0, std::ios::end);
    std::streamsize size = image.tellg();
    image.seekg(0, std::ios::beg);

    // tellg() returns -1 on error, and an empty file has nothing to send
    if (size <= 0) {
        std::cerr << "input.jpeg is empty or unreadable.\n";
        return 1;
    }

    std::vector<char> bytes(size); // buffer big enough for the whole file

    if (!image.read(bytes.data(), size)) {
        std::cerr << "Failed to read input.jpeg\n";
        return 1;
    }

    std::cout << "Image read into buffer successfully\n";

    // ---------- Send the size header, then the image ----------
    // htonl = host to network long: puts the 4-byte size in big-endian order,
    // so the server reads the same number whatever machine it runs on
    uint32_t len = static_cast<uint32_t>(bytes.size());
    uint32_t netLen = htonl(len);

    if (!sendAll(sockfd, &netLen, sizeof(netLen))) {
        std::cerr << "Error with sending 4 byte header\n";
        return 1;
    }
    if (!sendAll(sockfd, bytes.data(), len)) {
        std::cerr << "Error with sending image\n";
        return 1;
    }
    std::cout << "Image Sent Successfully over TCP\n";

    // ---------- Wait for the server's confirmation ----------
    // recv() returns the number of bytes received, 0 if the server closed the
    // connection without replying, or -1 on a network error.
    // sizeof - 1 leaves room for the '\0' added below.
    char server_confirmation[100];
    bool confirmed = false;
    ssize_t n = recv(sockfd, server_confirmation, sizeof(server_confirmation) - 1, 0);
    if (n > 0) {
        server_confirmation[n] = '\0'; // end the string so it can be printed
        std::cout << server_confirmation << "\n";
        confirmed = true;
    } else if (n == 0) {
        std::cerr << "Error receiving confirmation from server\n";
    } else {
        std::cerr << "Error receiving confirmation: " << strerror(errno) << "\n";
    }

    // ---------- Clean up ----------
    // Exit code 0 = the server replied, 1 = it never did
    freeaddrinfo(servinfo);
    close(sockfd);
    return confirmed ? 0 : 1;
}


// Sends all len bytes of data. send() may only send part of the buffer, so
// keep calling it until nothing is left. MSG_NOSIGNAL makes send() return -1
// if the server has disconnected, instead of the OS killing the program.
bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n == -1) return false;
        p += n;   // move past the bytes just sent
        len -= n; // and count them off
    }
    return true;
}
