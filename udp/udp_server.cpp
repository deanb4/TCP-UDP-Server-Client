/**
 * UDP Server
 *
 * Receives an image from the UDP client in numbered packets, reassembles it,
 * saves it to disk as received.jpeg, and sends a confirmation message back
 * to the client.
 *
 * Usage: ./udp_server [port]      (default: 3940)
 *
 * How the transfer works:
 *   - Every packet from the client starts with an 8-byte header:
 *       | packet number (4 bytes) | total image size (4 bytes) | image data |
 *     Because every packet carries the total size, whichever packet arrives
 *     first is enough to set up the transfer.
 *   - Packet n's data belongs at byte n * CHUNK_SIZE of the image, so packets
 *     can arrive in any order and resent copies simply fill the gaps.
 *   - Packet loss: if 500 ms pass with no packet while some are still missing,
 *     the server sends the client the number of each missing packet (4 bytes
 *     each) and the client resends them. If the client stays silent for too
 *     long (see MAX_TIMEOUTS), the transfer is abandoned.
 *   - Once every packet is in, the image is saved and a text confirmation is
 *     sent back. The server then waits for the next transfer (Ctrl+C to stop).
 */

#include <iostream>      // std::cout, std::cerr
#include <cerrno>        // errno: the reason the last system call failed (e.g. EAGAIN)
#include <sys/types.h>   // basic system types (ssize_t, socklen_t)
#include <sys/socket.h>  // socket(), bind(), sendto(), recvfrom(), setsockopt()
#include <netinet/in.h>  // internet address structures (sockaddr_in, sockaddr_in6)
#include <arpa/inet.h>   // htonl()/ntohl(): host <-> network byte order
#include <netdb.h>       // getaddrinfo(), freeaddrinfo(), gai_strerror()
#include <unistd.h>      // close()
#include <cstring>       // memset(), memcpy(), strlen(), strerror()
#include <vector>        // std::vector: the image buffer and received-packet flags
#include <fstream>       // std::ofstream, writes the image to disk
#include <sys/time.h>    // struct timeval, for the receive timeout
#include <algorithm>     // std::min
#include <cstdint>       // uint32_t, the fixed-size 4-byte header fields

#define PORT "3940"   // default port to listen on (the client must use the same)

// Transfer settings. HEADER_SIZE and CHUNK_SIZE must match udp_client.cpp.
constexpr uint32_t MAX_IMAGE_SIZE = 50 * 1024 * 1024; // reject images over 50 MB (protects memory)
constexpr size_t HEADER_SIZE = 8; // 4-byte packet number + 4-byte total image size
constexpr size_t CHUNK_SIZE = 1024; // image bytes per packet (the client must use the same)
constexpr int MAX_TIMEOUTS = 10; // silent 500 ms timeouts allowed before abandoning a transfer

int main(int argc, char* argv[]) {
    const char* port = nullptr; // port to listen on (command line, or PORT)
    int status = 0;             // return value of getaddrinfo
    int sockfd = 0;             // the server's UDP socket
    socklen_t fromlen;          // size of addr, passed to recvfrom
    sockaddr_storage  addr;     // address of whoever sent the latest packet

    // ---------- Command-line argument: [port] ----------
    argc > 1 ? port = argv[1] : port = PORT;

    // ---------- Find an address to listen on ----------
    // AF_INET6 + AI_PASSIVE gives the IPv6 "any" address (::), so the server
    // listens on every network interface. On Linux an IPv6 socket also accepts
    // IPv4 clients (they appear as ::ffff:a.b.c.d). SOCK_DGRAM = UDP.
    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints)); // make sure hints is all 0 and not garbage data!
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; // fill in my IP (listen on all interfaces)

    // Get address info
    if ((status = getaddrinfo(nullptr, port, &hints, &server_info)) != 0) {
        std::cerr << "Error getting address info: " << gai_strerror(status) << "\n";
        return 1;
    }

    // ---------- Create the socket and bind it to the port ----------
    // bind() attaches the socket to our port, so packets sent there reach us.
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
        break; // success: this socket is bound
    }

    // i is only nullptr if every address failed
    if (i == nullptr) {
        std::cerr << "listener failed to bind socket\n";
        return 1;
    }
    freeaddrinfo(server_info); // the address list is no longer needed

    // ---------- Receive timeout ----------
    // recvfrom() gives up after 500 ms and returns -1 (errno EAGAIN). That is
    // how the server notices packets have stopped arriving, so it can ask the
    // client to resend the ones still missing.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        std::cerr << "setsockopt: " << strerror(errno) << "\n";
        close(sockfd);
        return 1;
    }


    // ---------- Main loop: one iteration per image transfer ----------
    while (true) {
        // state for one image transfer (starts fresh for every transfer)
        std::vector<char> imageData; // the whole image, sized when the first packet arrives
        std::vector<bool> received; // received[i] is true once packet i has been copied in
        uint32_t totalSize = 0;      // image size in bytes, taken from the packet headers
        uint32_t numPackets = 0;     // 0 = no packet of this transfer has arrived yet
        uint32_t receivedCount = 0;  // how many different packets have been stored
        int timeouts = 0;            // 500 ms timeouts in a row without progress
        sockaddr_storage clientAddr; // where to send resend requests and the confirmation
        socklen_t clientLen = 0;     // size of clientAddr
        std::vector<char> packet(HEADER_SIZE + CHUNK_SIZE); // temp buffer (reused for every packet)

        // ---------- Receive packets until every one has arrived ----------
        while (numPackets == 0 || receivedCount < numPackets) {
            // fromlen tells recvfrom how much room addr has, so reset it every time
            fromlen = sizeof(addr);
            ssize_t n = recvfrom(sockfd, packet.data(), packet.size(), 0,
                                reinterpret_cast<sockaddr*>(&addr), &fromlen);

            // -1 means either the 500 ms timeout expired (EAGAIN) or a real error
            if (n == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // a real error: report it and keep listening
                    std::cerr << "recvfrom: " << strerror(errno) << "\n";
                    continue;
                }

                // 500 ms passed with no packet
                if (numPackets == 0) continue; // between transfers: just keep waiting
                // the client has been silent for too long: give up on this transfer
                if (timeouts++ > MAX_TIMEOUTS) {
                    std::cerr << "Client stopped sending, abandoning transfer\n";
                    break;
                }

                // Ask the client to resend every packet we're still missing.
                // Each request is just the packet number: 4 bytes, network byte order.
                std::cout << "Missing " << (numPackets - receivedCount) << " packets, asking client to resend\n";
                for (uint32_t i = 0; i < numPackets; i++) {
                    if (!received[i]) {
                        uint32_t netNum = htonl(i);
                        sendto(sockfd, &netNum, sizeof(netNum), 0, reinterpret_cast<sockaddr*>(&clientAddr), clientLen);
                    }
                }
                continue;
            }

            // ---------- Read the 8-byte header ----------
            // anything shorter than a header can't be one of our packets: ignore it
            if (n < static_cast<ssize_t>(HEADER_SIZE)) continue;
            uint32_t seq, total;
            memcpy(&seq, packet.data(), 4);       // bytes 0-3: packet number
            memcpy(&total, packet.data() + 4, 4); // bytes 4-7: total image size
            seq = ntohl(seq);                     // network byte order -> this machine's byte order
            total = ntohl(total);
            size_t dataLen = n - HEADER_SIZE;     // image bytes in this packet

            // ---------- First packet of a transfer: set everything up ----------
            // Any packet can be the first to arrive (packet 0 might be lost),
            // which works because every packet carries the total image size.
            if (numPackets == 0) {
                if (total == 0 || total > MAX_IMAGE_SIZE) { // check before allocating
                    std::cerr << "Ignoring packet with invalid image size " << total << "\n";
                    continue;
                }
                totalSize = total;
                numPackets = (total + CHUNK_SIZE -1) / CHUNK_SIZE; // divide (rounding up)
                imageData.resize(totalSize);         // room for the whole image
                received.assign(numPackets, false);  // nothing received yet
                clientAddr = addr;                   // remember the client, for resend requests
                clientLen = fromlen;                 // and the confirmation
                std::cout << "Expecting " << totalSize << " bytes in " << numPackets << " packets\n";
            }

            // ---------- Check the packet before trusting it ----------
            // memcpy below writes wherever it is told, so a bad packet number or
            // length could write past the end of imageData. Skip any packet that:
            //  - belongs to a different transfer (different total size)
            //  - has a packet number outside the image
            //  - has the wrong amount of data (each packet holds CHUNK_SIZE bytes,
            //    except the last, which holds whatever is left over)
            //  - is a duplicate of a packet already stored
            if (total != totalSize || seq >= numPackets) continue;
            size_t offset = static_cast<size_t>(seq) * CHUNK_SIZE;
            size_t expectedLen = std::min<size_t>(CHUNK_SIZE, totalSize - offset);
            if (dataLen != expectedLen) continue;
            if (received[seq]) continue; // duplicate

            // ---------- Copy the packet's data into its spot in the image ----------
            // Packet n always goes at byte n * CHUNK_SIZE, so arrival order doesn't matter.
            memcpy(imageData.data() + offset, packet.data() + HEADER_SIZE, dataLen);
            received[seq] = true;
            receivedCount++;
            timeouts = 0; // progress was made: reset the silence counter
            std::cout << "Packet " << seq + 1 << "/" << numPackets << " received\n";


            // still missing packets: go back and keep receiving
            if (receivedCount < numPackets) continue;

            // ---------- Every packet is here: save the image ----------
            // close() is what actually writes the data to disk, so check for errors after it
            std::ofstream out("received.jpeg", std::ios::binary);
            out.write(imageData.data(), imageData.size());
            out.close();
            const char* reply;
            if (!out) {
                std::cerr << "Failed to write received.jpeg\n";
                reply = "ERROR: server could not save the image";
            } else {
                std::cout << "Saved " << totalSize << " bytes to received.jpeg\n";
                reply = "Image received and saved to server successfully via UDP";
            }

            // ---------- Send the confirmation (or the error) back to the client ----------
            // After this the inner loop ends (receivedCount == numPackets) and the
            // outer loop starts fresh, ready for the next transfer.
            if (sendto(sockfd, reply, strlen(reply), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientLen) == -1) {

                std::cerr << "Error sending confirmation: " << strerror(errno) << "\n";
            }
        }

    }

    // not reached: the server runs until it is stopped with Ctrl+C
    close(sockfd);
    return 0;


}
