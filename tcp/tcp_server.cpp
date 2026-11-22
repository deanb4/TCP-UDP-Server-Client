/**
 * TCP Server
 */
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <cstdint>

#define MYPORT "3490"
#define BACKLOG 10 // how many requests can pile up in queue

bool recvAll(int, void*, size_t);

int main(int argc, char** argv) {
    const char* port = nullptr;
    int backlog = 0;
    // get args
    argc > 1 ? port = argv[1] : port = MYPORT;
    argc > 2 ? backlog = std::stoi(argv[2]) : backlog = BACKLOG;

    // get address info
    int sockfd = -1; // file descriptor for listening for connections
    int newfd = -1; // new file descriptor for the tcp connection
    struct addrinfo hints, *server_info; // server info will point to results
    struct sockaddr_storage their_addr;
    char response[1000]; 
    socklen_t addr_size;
    int opt = 1;

    memset(&hints, 0, sizeof(hints)); // make sure struct is empty
    hints.ai_family = AF_UNSPEC; // ipv4 or ipv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream
    hints.ai_flags = AI_PASSIVE; // fill in my IP

    int status = getaddrinfo(nullptr, port, &hints, &server_info);

    if (status != 0) {
        // throw std::runtime_error("Error getting address info: " + gai_sterror(status));
        std::cerr << "Error getting address info: " << gai_strerror(status);
        return 1;
    }

    // create socket (get file descriptor back)
    sockfd = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);

    if (sockfd == -1) {
        std::cerr << "Error creating socket: " << strerror(errno);
        freeaddrinfo(server_info);
        return 1;
    }

    // TODO: loop through all the results and bind to the first we can


    // make sure to force connection in case of a lingering conncection that was just closed
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt,  sizeof(int)) == -1) {
        std::cerr << "Error setting up socket (could be a lingering connection on desired port)" << strerror(errno);
        freeaddrinfo(server_info);
        close(sockfd);
        return 1;

    }
    
    // bind socket to port
    if (bind(sockfd, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        std::cerr << "Error creating socket: " << strerror(errno);
        freeaddrinfo(server_info);
        close(sockfd);
        return 1;
    }
    // free struct once done
    freeaddrinfo(server_info);

    // start listening for connections
    if (listen(sockfd, backlog) == -1) {
        std::cerr << "Error listening on port: " << port << "| "<< strerror(errno);
        close(sockfd);
        return 1;
    }

    // accept loop until conncection terminates
    bool accept_connection = true;
    while (accept_connection) {
        addr_size = sizeof(their_addr);
        newfd = accept(sockfd, reinterpret_cast<sockaddr*>(&their_addr), &addr_size);

        if (newfd == -1) {
            std::cerr << "error accepting connection: " << strerror(errno);
            continue;
        } 

        std::cout <<  "Connected! Listening on port: "<< port << "\n";

        while (true) {
            uint32_t netLen;
            if (!recvAll(newfd, &netLen, sizeof(netLen))) {
                std::cerr << "Failed to read image length\n";
                // close(newfd);
                break;
            }
            uint32_t len = ntohl(netLen); // Network to host Long 
            std::cout << "Expecting: " << len << " bytes\n";
            // Read that many bytes
            std::vector<char> imageData(len);
            if (!recvAll(newfd, imageData.data(), len)) {
                std::cerr << "Connection closed before full image arrived\n";
                // close(newfd);
                break;
            }
            std::cout << "Received all " << len << " bytes\n";

            // save to file
            std::ofstream out("received.png", std::ios::binary);
            out.write(imageData.data(), len);
            if (!out) { 
                std::cerr << "Failed to write\n";
            } else {
                std::cout << "Saved to received.png\n"; 
                const char* confirmation = "Image Received and saved to server successfully!";
                send(newfd, confirmation, strlen(confirmation), 0);
            }
            
            // // temporary for now so only one image can be sent.
            // break;


            // FOR INITIAL TESTING OF RECEIVING A MESSAGE
            // ssize_t bytes = recv(newfd, response, sizeof(response), 0);
            // if (bytes == -1) {
            //     std::cerr << "recv: " << strerror(errno) << "\n";
            //     break;
            // }
            // if (bytes == 0) {
            //     std::cout << "client disconnected\n";
            //     break;
            // }
            // std::cout.write(response, bytes);
            // std::cout.flush();
            
        }
        std::cout << "Closing Connection\n";
        close(newfd);
        accept_connection = false;
    }

    // send message and receive it then an image before making client


    std::cout << "TCP Socket listening on Port: " << port << " has been closed.\n";
    close(sockfd);
    
    return 0;
}


// receive all bytes until length left is 0
bool recvAll(int fd, void* data, size_t len) {
    char* bytes_data = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = recv(fd, bytes_data, len, 0);
        if (n <= 0) return false;
        bytes_data += n;
        len -= n;
    }
    return true;
}