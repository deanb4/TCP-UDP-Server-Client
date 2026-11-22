/**
 * UDP Client
 * // get address info
 * // create socket
 * // bind 
 * // send data
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <iostream>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>

#define PORT "3940"
#define IP "127.0.0.1"

bool sendAll(int, const void*, size_t, const sockaddr*, socklen_t);

int main(int argc, char* argv[]) {
    const char* port = nullptr;
    const char* ip = nullptr;
    argc > 1 ? ip = argv[1] : ip = IP;
    argc > 2 ? port = argv[2] : port = PORT;

    int status;
    sockaddr_storage their_addr;
    socklen_t addr_size;
    addrinfo hints, *res;
    sockaddr_in dest;
    int sockfd;

    // load up address structs 
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    // hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(nullptr, port, &hints, &res);
    if (status != 0) {
        std::cerr << "Error loading address info" << gai_strerror(status);
        return 1;
    }

    // create socket
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (sockfd == -1) {
        std::cerr << "Error creating socket\n";
        freeaddrinfo(res);
        return 1;
    }

    // read image
    std::ifstream image("input.png", std::ios::binary);
    if (!image) {
        std::cerr << "Could not opne image\n";
        return 1;
    }
    image.seekg(0, std::ios::end);
    std::streamsize size = image.tellg();
    image.seekg(0, std::ios::beg);

    std::vector<char> bytes(size);

    image.read(bytes.data(), size);

    std::cout << "Image read into buffer successfully\n";
    uint32_t len = static_cast<uint32_t>(bytes.size());
    uint32_t netlen = htonl(len); // host to network long (4 bytes)

    // send image
    if (!sendAll(sockfd, &netlen, sizeof(netlen), res->ai_addr, res->ai_addrlen)) {
        std::cerr << "Error with 4 byte header\n";
        return 1;
    }
    if (!sendAll(sockfd, bytes.data(), len, res->ai_addr, res->ai_addrlen)) {
        std::cerr << "Error sending image\n";
        return 1;
    }

    std::cout << "Image sent successfully\n";

    // TODO: GET CONFIRMATION MESSAGE FROM SERVER





    // if (sendto(sockfd, bytes.data(), bytes.size(), 0, res->ai_addr, res->ai_addrlen) == -1) {
    //     std::cerr << "error sending image\n";
    //     return 1;
    // }

    // const char* message = "UDP CLIENT SUCCESS!!!!!!!!!!!\n";
    // if (sendto(sockfd, message, strlen(message), 0,
    //     res->ai_addr, res->ai_addrlen) == 0) {
    //         std::cerr << "Sending failed\n";
    //         return 1;
    //     }


    

    freeaddrinfo(res);
    close(sockfd);

    return 0;
}

bool sendAll(int fd, const void* data, size_t len, const sockaddr* addr, socklen_t addrlen) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = sendto(fd, p, len, 0, addr, addrlen);
        if (n == -1) return false;
        p += n;
        len -= n;
    }
    return true;
}
