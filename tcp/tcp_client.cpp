/**
 * TCP Client
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <iostream>
#include <netdb.h>
#include <cstring>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <cstdint>

#define PORT "3490"
#define MAXDATASIZE 1000

bool sendAll(int, const void*, size_t);

int main(int argc, char* argv[]) {
    const char* port = nullptr;
    const char* ip = nullptr;
    int status = -1;
    char s[INET6_ADDRSTRLEN];
    argc > 1 ? ip = argv[1]: ip = nullptr;
    argc > 2 ? port = argv[2]: port = PORT;

    // addressInfo
    int sockfd, numbytes;
    char buff[MAXDATASIZE];
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(ip, port, &hints, &servinfo);
    if (status != 0) {
        std::cerr << "Error obtaining address of server." << gai_strerror(status);
        return 1;
    }
    // socket
    sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (sockfd == -1) {
        std::cerr << "Error returning socket file descriptor" << strerror(errno);
        return 1;
    }

    // change ip address data type
    // make it compatible with v4 and v6
    auto* addr4 = reinterpret_cast<sockaddr_in*>(servinfo->ai_addr);
    inet_ntop(AF_INET, &addr4->sin_addr, s, sizeof(s));
    std::cout << "client attempting to connect to: " << s << "\n";

    if (connect(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        std::cerr << "Error connecting to client\n" << strerror(errno);
        freeaddrinfo(servinfo);
        return 1;
    }
    std::cout << "Client Connected to port: " << ntohs(addr4->sin_port) << "\n";
    
    // Send message
    // const char* message = "SUCCESS! Testing TCP client connection!\n";
    // if (send(sockfd, message, strlen(message), 0) == -1) {
    //     std::cerr << "Error sending message\n" << strerror(errno);
    //     return 1;     
    // }

    // Send image

    // Read image in binary
    std::ifstream image("input.png", std::ios::binary);
    if (!image) {
        std::cerr << "Could not open image\n";
        return 1;
    }
    image.seekg(0, std::ios::end);
    std::streamsize size = image.tellg();
    image.seekg(0, std::ios::beg);

    std::vector<char> bytes(size);
    
    image.read(bytes.data(), size);

    std::cout << "Image read into buffer successfully\n";
    uint32_t len = static_cast<uint32_t>(bytes.size());
    uint32_t netLen = htonl(len);

    if (!sendAll(sockfd, &netLen, sizeof(netLen))) {
        std::cerr << "Error with reading 4 byte header\n";
        return 1;
    }
    if (!sendAll(sockfd, bytes.data(), len)) {
        std::cerr << "Error with reading image\n";
        return 1;
    }

    // if (send(sockfd, bytes.data(), bytes.size(), 0) == -1) {
    //     std::cerr << "Error sending image\n" << strerror(errno);
    //     return 1; 
    // }
    std::cout << "Image Sent Successfully over TCP\n";
    char server_confirmation[100];
    ssize_t n = recv(sockfd, server_confirmation, sizeof(server_confirmation) - 1, 0);
    if (n == 0) {
        std::cerr << "Error receiving confrimation from server\n";
    }

    if (n > 0) {
        server_confirmation[n] = '\0';
    }

    std::cout << server_confirmation << "\n";
    freeaddrinfo(servinfo);
    close(sockfd);



    // send
    return 0;
}



bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n == -1) return false;
        p += n;
        len -= n;
    }
    return true;
}