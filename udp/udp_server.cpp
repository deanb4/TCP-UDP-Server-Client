/**
 * UDP Server
 */

#include <iostream>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <fstream>

#define PORT "3940"
// sockfd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&addr), &fromlen)
// Receive all bytes
bool recvAll(int, void*, size_t, sockaddr_storage*, socklen_t, int&);

int main(int argc, char* argv[]) {
    const char* port = nullptr;
    int status = 0;
    int sockfd = 0;
    char buf[5012];
    socklen_t fromlen;
    sockaddr_storage  addr;
    int byte_count;
    char ipstr[INET6_ADDRSTRLEN];

    argc > 1 ? port = argv[1] : port = PORT;

    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints)); // make sure hints is all 0 and not garbage data!
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; // my ip

    // Get address info
    if ((status = getaddrinfo(nullptr, port, &hints, &server_info)) != 0) {
        std::cerr << "error getting pointer to address info" << gai_strerror(status);
        return 1;
    }

    struct addrinfo* i = nullptr;
    // loop through results and bind to first one that succeeds
    for (i = server_info; i != nullptr; i = i->ai_next) {
        if ((sockfd = socket(i->ai_family, i->ai_socktype, i->ai_protocol)) == -1) {
            std::cerr << "error\n";
            continue;
        }

        if (bind(sockfd, i->ai_addr, i->ai_addrlen) == -1) {
            close(sockfd);
            std::cerr << "error\n";
            continue;
        }
        break;
    }

    if (i == nullptr) {
        std::cerr << "listener failed to bind socket\n";
        return 1;
    } 
    freeaddrinfo(server_info);
    while (true) {
        fromlen = sizeof(addr);
        uint32_t netLen;
        if (!recvAll(sockfd, &netLen, sizeof(netLen), &addr, fromlen, byte_count)) {
            std::cerr << "failed to read image\n";
            break;
        }
        uint32_t len = ntohl(netLen);
        std::vector<char> imageData(len);
        std::cout << "Expecting: " << len << " bytes\n";

        // Read that many bytes
        if (!recvAll(sockfd, imageData.data(), len, &addr, fromlen, byte_count)) {
            std::cerr << "Error loading image\n";
            break;
        }    
        // byte_count = recvfrom(sockfd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&addr), &fromlen);
        if (byte_count > 0) {
            // std::cout.write(imageData.data(), imageData.size());
            // std::cout.flush();
            // TODO: SEND CONFIRMATION TO CLIENT

            // save image
            std::ofstream out("received.png", std::ios::binary);
            out.write(imageData.data(), len);
            if (!out) {
                std::cerr << "Failed to write\n";
            } else {
                std::cout << "Saved to received.png\n";
                const char* confimation = "Imaage Received and saved to server successfully via UDP";
                // sendto()
            }

        } else {
            std::cout << "error receiving data\n";
            break;
        }

        // print IP:
        std::cout << "Total byte count: " << byte_count << "\n";
        std::cout << "From IP Address: ";
        const void* address = nullptr;
        
        if (addr.ss_family == AF_INET) {
            address = &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr;
        } else {
            address = &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr;
        }
        inet_ntop(addr.ss_family, address, ipstr, sizeof(ipstr));
        std::cout << ipstr << "\n\n";

    }


    close(sockfd);
    return 0;


}


bool recvAll(int fd, void* data, size_t len, sockaddr_storage* addr, socklen_t fromlen, int &byte_count) {
    char* bytes_data = static_cast<char*>(data);
    ssize_t n = 0;
    while (len > 0) {
        n = recvfrom(fd, bytes_data, len, 0, reinterpret_cast<sockaddr*>(addr), &fromlen);
        if (n <= 0) return false;
        bytes_data += n;
        len -=n;
    }
    byte_count = n;
    return true;
    
}